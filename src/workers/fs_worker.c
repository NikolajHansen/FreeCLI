#include "fs_worker.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifndef _WIN32
# include <unistd.h>
# include <limits.h>
# include <sys/stat.h>
# include <dirent.h>
# include <pwd.h>
#else
# include <windows.h>
# include <direct.h>
# define PATH_MAX MAX_PATH
#endif

#define FS_PATH_MAX   4096
#define FS_MAX_READ   (512 * 1024)   /* 512 KB hard cap on file reads */

/* -----------------------------------------------------------------------
 * Private worker state
 * --------------------------------------------------------------------- */

typedef struct {
    char root[FS_PATH_MAX];  /* canonical sandbox root (no trailing slash) */
} FsPriv;

/* -----------------------------------------------------------------------
 * Path safety
 *
 * All user-supplied paths are resolved through fs_resolve().  It returns
 * a heap-allocated canonical absolute path guaranteed to be inside the
 * sandbox root, or NULL if the path is invalid / escaping.
 *
 * for_create=1: the file need not exist; the parent directory must exist
 *               and the basename must be a plain filename (no slashes, no ..).
 * for_create=0: the full path must already exist on disk.
 * --------------------------------------------------------------------- */

static char *fs_resolve(const FsPriv *priv, const char *requested, int for_create) {
    if (!requested) return NULL;

    /* Strip leading slashes — all paths are relative to the sandbox */
    while (*requested == '/') requested++;

    /* Build the candidate absolute path */
    char cand[FS_PATH_MAX];
    if (*requested == '\0') {
        /* Empty path → sandbox root itself */
        return strdup(priv->root);
    }
    if (snprintf(cand, sizeof(cand), "%s/%s", priv->root, requested)
            >= (int)sizeof(cand))
        return NULL;

    char resolved[FS_PATH_MAX];

    if (!for_create) {
        /* File must exist — realpath resolves and checks everything */
#ifndef _WIN32
        if (realpath(cand, resolved) == NULL) return NULL;
#else
        if (_fullpath(resolved, cand, FS_PATH_MAX) == NULL) return NULL;
#endif
    } else {
        /* For create: parent must exist; basename must be a plain name */
        char dir_part[FS_PATH_MAX];
        if ((int)strlen(cand) >= FS_PATH_MAX) return NULL;
        strcpy(dir_part, cand);

        char *slash = strrchr(dir_part, '/');
        if (!slash) return NULL;
        const char *base = slash + 1;
        *slash = '\0';

        /* Reject empty, dot, dotdot, or embedded-slash basenames */
        if (!*base ||
            strcmp(base, ".")  == 0 ||
            strcmp(base, "..") == 0 ||
            strchr(base, '/') != NULL)
            return NULL;

        /* Resolve the parent directory */
        char par[FS_PATH_MAX];
#ifndef _WIN32
        if (realpath(dir_part, par) == NULL) return NULL;
#else
        if (_fullpath(par, dir_part, FS_PATH_MAX) == NULL) return NULL;
#endif
        if (snprintf(resolved, sizeof(resolved), "%s/%s", par, base)
                >= (int)sizeof(resolved))
            return NULL;
    }

    /* Enforce sandbox boundary: resolved must start with root + '/' or == root */
    size_t root_len = strlen(priv->root);
    if (strncmp(resolved, priv->root, root_len) != 0 ||
        (resolved[root_len] != '/' && resolved[root_len] != '\0'))
        return NULL;

    return strdup(resolved);
}

/* Relative portion of an absolute sandbox path (for display / JSON) */
static const char *fs_rel(const FsPriv *priv, const char *abs_path) {
    size_t root_len = strlen(priv->root);
    if (strncmp(abs_path, priv->root, root_len) != 0) return abs_path;
    const char *rel = abs_path + root_len;
    if (*rel == '/') rel++;
    return *rel ? rel : ".";
}

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

/* Read entire file into a malloc'd buffer (NUL-terminated).
 * Returns NULL on error, sets *out_len to byte count (excl. NUL). */
static char *read_file_contents(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsz < 0 || fsz > (long)FS_MAX_READ) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)fsz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t got = fread(buf, 1, (size_t)fsz, f);
    fclose(f);

    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

/* Write content to path, creating or truncating. Returns 0 on success. */
static int write_file_contents(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t len = strlen(content);
    int ok = (fwrite(content, 1, len, f) == len);
    fclose(f);
    return ok ? 0 : -1;
}

/* -----------------------------------------------------------------------
 * Operation implementations
 * --------------------------------------------------------------------- */

static void op_read_file(BackendWorker *self, BusContext *bus,
                          const BusMsg *msg) {
    FsPriv *priv = (FsPriv *)self->priv;

    cJSON *args = msg->args_json ? cJSON_Parse(msg->args_json) : NULL;
    cJSON *path_j = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
    const char *req_path = (cJSON_IsString(path_j) && path_j->valuestring)
                           ? path_j->valuestring : NULL;

    if (!req_path) {
        if (args) cJSON_Delete(args);
        bw_send_error(bus, msg->corr_id, "read_file: missing 'path' argument");
        return;
    }

    char *abs = fs_resolve(priv, req_path, 0);
    if (!args) { /* parse error */ }
    cJSON_Delete(args);

    if (!abs) {
        bw_send_error(bus, msg->corr_id, "read_file: path outside sandbox or not found");
        return;
    }

    bw_send_progress(bus, msg->corr_id, 0, self->name, "reading file…");

    size_t flen;
    char *content = read_file_contents(abs, &flen);
    free(abs);

    if (!content) {
        bw_send_error(bus, msg->corr_id,
                      flen == 0 ? "read_file: file too large (> 512 KB)"
                                : "read_file: could not read file");
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "content", content);
    free(content);

    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    bw_send_result(bus, msg->corr_id, 1, json);
    free(json);
}

static void op_list_dir(BackendWorker *self, BusContext *bus,
                         const BusMsg *msg) {
    FsPriv *priv = (FsPriv *)self->priv;

    cJSON *args = msg->args_json ? cJSON_Parse(msg->args_json) : NULL;
    cJSON *path_j = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
    const char *req_path = (cJSON_IsString(path_j) && path_j->valuestring)
                           ? path_j->valuestring : "";

    char *abs = fs_resolve(priv, req_path, 0);
    cJSON_Delete(args);

    if (!abs) {
        bw_send_error(bus, msg->corr_id, "list_dir: path outside sandbox or not found");
        return;
    }

    bw_send_progress(bus, msg->corr_id, 0, self->name, "listing directory…");

#ifndef _WIN32
    DIR *d = opendir(abs);
    if (!d) {
        free(abs);
        bw_send_error(bus, msg->corr_id, "list_dir: cannot open directory");
        return;
    }

    cJSON *entries = cJSON_CreateArray();
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char entry_path[FS_PATH_MAX];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", abs, ent->d_name);

        struct stat st;
        if (stat(entry_path, &st) != 0) continue;

        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", ent->d_name);
        cJSON_AddStringToObject(e, "type",
                                S_ISDIR(st.st_mode) ? "dir" : "file");
        if (!S_ISDIR(st.st_mode))
            cJSON_AddNumberToObject(e, "size", (double)st.st_size);
        cJSON_AddItemToArray(entries, e);
    }
    closedir(d);
#else
    /* Windows: FindFirstFile */
    char pattern[FS_PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*", abs);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    cJSON *entries = cJSON_CreateArray();
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                continue;
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "name", fd.cFileName);
            int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            cJSON_AddStringToObject(e, "type", is_dir ? "dir" : "file");
            if (!is_dir) {
                ULARGE_INTEGER sz;
                sz.LowPart  = fd.nFileSizeLow;
                sz.HighPart = fd.nFileSizeHigh;
                cJSON_AddNumberToObject(e, "size", (double)sz.QuadPart);
            }
            cJSON_AddItemToArray(entries, e);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#endif

    free(abs);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "entries", entries);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    bw_send_result(bus, msg->corr_id, 1, json);
    free(json);
}

static void op_create_file(BackendWorker *self, BusContext *bus,
                             const BusMsg *msg) {
    FsPriv *priv = (FsPriv *)self->priv;

    cJSON *args    = msg->args_json ? cJSON_Parse(msg->args_json) : NULL;
    cJSON *path_j  = args ? cJSON_GetObjectItemCaseSensitive(args, "path")    : NULL;
    cJSON *cont_j  = args ? cJSON_GetObjectItemCaseSensitive(args, "content") : NULL;

    const char *req_path = (cJSON_IsString(path_j) && path_j->valuestring)
                           ? path_j->valuestring : NULL;
    const char *content  = (cJSON_IsString(cont_j) && cont_j->valuestring)
                           ? cont_j->valuestring  : "";

    if (!req_path) {
        cJSON_Delete(args);
        bw_send_error(bus, msg->corr_id, "create_file: missing 'path' argument");
        return;
    }

    char *abs = fs_resolve(priv, req_path, 1 /* for_create */);
    cJSON_Delete(args);

    if (!abs) {
        bw_send_error(bus, msg->corr_id, "create_file: invalid or unsafe path");
        return;
    }

    /* Check the file does not already exist */
#ifndef _WIN32
    struct stat st;
    if (stat(abs, &st) == 0) {
#else
    if (GetFileAttributesA(abs) != INVALID_FILE_ATTRIBUTES) {
#endif
        free(abs);
        bw_send_error(bus, msg->corr_id,
                      "create_file: file already exists — use modify_file");
        return;
    }

    /* Request explicit approval before touching the filesystem */
    char approval_desc[FS_PATH_MAX + 64];
    snprintf(approval_desc, sizeof(approval_desc),
             "Create file: %s", fs_rel(priv, abs));

    if (!bw_request_approval(bus, msg->corr_id, approval_desc)) {
        free(abs);
        bw_send_error(bus, msg->corr_id, "create_file: denied by user");
        return;
    }

    if (write_file_contents(abs, content) != 0) {
        free(abs);
        bw_send_error(bus, msg->corr_id, "create_file: write failed");
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "path",   fs_rel(priv, abs));
    free(abs);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    bw_send_result(bus, msg->corr_id, 1, json);
    free(json);
}

static void op_modify_file(BackendWorker *self, BusContext *bus,
                             const BusMsg *msg) {
    FsPriv *priv = (FsPriv *)self->priv;

    cJSON *args   = msg->args_json ? cJSON_Parse(msg->args_json) : NULL;
    cJSON *path_j = args ? cJSON_GetObjectItemCaseSensitive(args, "path")    : NULL;
    cJSON *cont_j = args ? cJSON_GetObjectItemCaseSensitive(args, "content") : NULL;

    const char *req_path = (cJSON_IsString(path_j) && path_j->valuestring)
                           ? path_j->valuestring : NULL;
    const char *content  = (cJSON_IsString(cont_j) && cont_j->valuestring)
                           ? cont_j->valuestring  : "";

    if (!req_path) {
        cJSON_Delete(args);
        bw_send_error(bus, msg->corr_id, "modify_file: missing 'path' argument");
        return;
    }

    char *abs = fs_resolve(priv, req_path, 0 /* must exist */);
    cJSON_Delete(args);

    if (!abs) {
        bw_send_error(bus, msg->corr_id,
                      "modify_file: path outside sandbox or file not found");
        return;
    }

    char approval_desc[FS_PATH_MAX + 64];
    snprintf(approval_desc, sizeof(approval_desc),
             "Modify file: %s", fs_rel(priv, abs));

    if (!bw_request_approval(bus, msg->corr_id, approval_desc)) {
        free(abs);
        bw_send_error(bus, msg->corr_id, "modify_file: denied by user");
        return;
    }

    if (write_file_contents(abs, content) != 0) {
        free(abs);
        bw_send_error(bus, msg->corr_id, "modify_file: write failed");
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "path",   fs_rel(priv, abs));
    free(abs);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    bw_send_result(bus, msg->corr_id, 1, json);
    free(json);
}

static void op_delete_file(BackendWorker *self, BusContext *bus,
                             const BusMsg *msg) {
    FsPriv *priv = (FsPriv *)self->priv;

    cJSON *args   = msg->args_json ? cJSON_Parse(msg->args_json) : NULL;
    cJSON *path_j = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
    const char *req_path = (cJSON_IsString(path_j) && path_j->valuestring)
                           ? path_j->valuestring : NULL;

    if (!req_path) {
        cJSON_Delete(args);
        bw_send_error(bus, msg->corr_id, "delete_file: missing 'path' argument");
        return;
    }

    char *abs = fs_resolve(priv, req_path, 0 /* must exist */);
    cJSON_Delete(args);

    if (!abs) {
        bw_send_error(bus, msg->corr_id,
                      "delete_file: path outside sandbox or file not found");
        return;
    }

    char approval_desc[FS_PATH_MAX + 64];
    snprintf(approval_desc, sizeof(approval_desc),
             "Delete file: %s", fs_rel(priv, abs));

    if (!bw_request_approval(bus, msg->corr_id, approval_desc)) {
        free(abs);
        bw_send_error(bus, msg->corr_id, "delete_file: denied by user");
        return;
    }

#ifndef _WIN32
    if (remove(abs) != 0) {
#else
    if (DeleteFileA(abs) == 0) {
#endif
        free(abs);
        bw_send_error(bus, msg->corr_id, "delete_file: remove failed");
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "path",   fs_rel(priv, abs));
    free(abs);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    bw_send_result(bus, msg->corr_id, 1, json);
    free(json);
}

/* -----------------------------------------------------------------------
 * BackendWorker vtable
 * --------------------------------------------------------------------- */

static int fs_init(BackendWorker *self) {
    FsPriv *priv = (FsPriv *)self->priv;

    /* Ensure sandbox directory exists */
#ifndef _WIN32
    struct stat st;
    if (stat(priv->root, &st) != 0) {
        if (mkdir(priv->root, 0700) != 0) {
            fprintf(stderr, "[fs_worker] cannot create sandbox '%s': %s\n",
                    priv->root, strerror(errno));
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "[fs_worker] sandbox path '%s' is not a directory\n",
                priv->root);
        return -1;
    }

    /* Canonicalize (resolve symlinks etc.) */
    char canonical[FS_PATH_MAX];
    if (realpath(priv->root, canonical) == NULL) {
        fprintf(stderr, "[fs_worker] realpath('%s') failed: %s\n",
                priv->root, strerror(errno));
        return -1;
    }
    strncpy(priv->root, canonical, FS_PATH_MAX - 1);
    priv->root[FS_PATH_MAX - 1] = '\0';
#else
    if (CreateDirectoryA(priv->root, NULL) == 0 &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "[fs_worker] cannot create sandbox '%s'\n", priv->root);
        return -1;
    }
    char canonical[FS_PATH_MAX];
    if (_fullpath(canonical, priv->root, FS_PATH_MAX) == NULL) return -1;
    strncpy(priv->root, canonical, FS_PATH_MAX - 1);
#endif

    fprintf(stderr, "[fs_worker] sandbox: %s\n", priv->root);
    return 0;
}

static int fs_dispatch(BackendWorker *self, BusContext *bus, const BusMsg *msg) {
    const char *op = msg->op ? msg->op : "";

    if      (strcmp(op, "read_file")   == 0) op_read_file(self, bus, msg);
    else if (strcmp(op, "list_dir")    == 0) op_list_dir(self, bus, msg);
    else if (strcmp(op, "create_file") == 0) op_create_file(self, bus, msg);
    else if (strcmp(op, "modify_file") == 0) op_modify_file(self, bus, msg);
    else if (strcmp(op, "delete_file") == 0) op_delete_file(self, bus, msg);
    else {
        char err[128];
        snprintf(err, sizeof(err), "fs_worker: unknown operation '%s'", op);
        bw_send_error(bus, msg->corr_id, err);
    }
    return 0;
}

static void fs_free(BackendWorker *self) {
    free(self->priv);
    free(self);
}

/* -----------------------------------------------------------------------
 * Factory
 * --------------------------------------------------------------------- */

BackendWorker *fs_worker_create(const char *root_dir) {
    BackendWorker *w = calloc(1, sizeof(BackendWorker));
    if (!w) return NULL;

    FsPriv *priv = calloc(1, sizeof(FsPriv));
    if (!priv) { free(w); return NULL; }

    if (root_dir && *root_dir) {
        strncpy(priv->root, root_dir, FS_PATH_MAX - 1);
    } else {
        /* Default: $FREECLI_SANDBOX or ~/freecli-sandbox */
        const char *env = getenv("FREECLI_SANDBOX");
        if (env && *env) {
            strncpy(priv->root, env, FS_PATH_MAX - 1);
        } else {
#ifndef _WIN32
            const char *home = getenv("HOME");
            if (!home) {
                struct passwd *pw = getpwuid(getuid());
                if (pw) home = pw->pw_dir;
            }
            if (home)
                snprintf(priv->root, FS_PATH_MAX, "%s/freecli-sandbox", home);
            else
                strncpy(priv->root, "/tmp/freecli-sandbox", FS_PATH_MAX - 1);
#else
            const char *home = getenv("USERPROFILE");
            if (home)
                snprintf(priv->root, FS_PATH_MAX, "%s\\freecli-sandbox", home);
            else
                strncpy(priv->root, "C:\\freecli-sandbox", FS_PATH_MAX - 1);
#endif
        }
    }
    priv->root[FS_PATH_MAX - 1] = '\0';

    w->type     = BACKEND_TYPE_FILESYSTEM;
    w->name     = "filesystem";
    w->init     = fs_init;
    w->dispatch = fs_dispatch;
    w->free_w   = fs_free;
    w->priv     = priv;
    return w;
}
