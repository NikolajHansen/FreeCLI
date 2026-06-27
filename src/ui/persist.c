#include "persist.h"
#include "chat.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
# include <windows.h>
# include <direct.h>
# define fc_mkdir(p) _mkdir(p)
#else
# include <unistd.h>
# include <pwd.h>
# define fc_mkdir(p) mkdir((p), 0755)
#endif

/* --- Path helpers --- */

static void get_data_dir(char *out, size_t sz) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    snprintf(out, sz, "%s\\FreeCLI", appdata ? appdata : ".");
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, sz, "%s/freecli", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) {
            struct passwd *pw = getpwuid(getuid());
            home = (pw && pw->pw_dir) ? pw->pw_dir : ".";
        }
        snprintf(out, sz, "%s/.local/share/freecli", home);
    }
#endif
}

static void ensure_dirs(void) {
    char dir[512];
    get_data_dir(dir, sizeof(dir));
    fc_mkdir(dir);

    char sub[600];
#ifdef _WIN32
    snprintf(sub, sizeof(sub), "%s\\sessions", dir);
#else
    snprintf(sub, sizeof(sub), "%s/sessions", dir);
#endif
    fc_mkdir(sub);
}

static void index_path(char *out, size_t sz) {
    char dir[512];
    get_data_dir(dir, sizeof(dir));
#ifdef _WIN32
    snprintf(out, sz, "%s\\sessions.json", dir);
#else
    snprintf(out, sz, "%s/sessions.json", dir);
#endif
}

static void session_path(char *out, size_t sz, int id) {
    char dir[512];
    get_data_dir(dir, sizeof(dir));
#ifdef _WIN32
    snprintf(out, sz, "%s\\sessions\\%d.json", dir, id);
#else
    snprintf(out, sz, "%s/sessions/%d.json", dir, id);
#endif
}

/* --- File I/O --- */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(data, f);
    fclose(f);
}

/* --- Index (sessions.json) --- */

static void save_index(const SessionManager *sm) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "next_id", sm->next_id);
    cJSON *arr = cJSON_AddArrayToObject(root, "sessions");
    for (int i = 0; i < sm->count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id",   sm->sessions[i].id);
        cJSON_AddStringToObject(obj, "name", sm->sessions[i].name);
        cJSON_AddItemToArray(arr, obj);
    }
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return;
    char path[600];
    index_path(path, sizeof(path));
    write_file(path, str);
    free(str);
}

/* --- Public API --- */

void persist_save_session(const SessionManager *sm, int idx) {
    if (idx < 0 || idx >= sm->count) return;
    ensure_dirs();
    const Session *s = &sm->sessions[idx];

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id",   s->id);
    cJSON_AddStringToObject(root, "name", s->name);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    for (int i = 0; i < s->chat.count; i++) {
        const Message *m = &s->chat.msgs[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "role",    m->is_user ? "user" : "assistant");
        cJSON_AddStringToObject(obj, "content", m->text);
        if (!m->is_user && m->reasoning)
            cJSON_AddStringToObject(obj, "reasoning", m->reasoning);
        cJSON_AddItemToArray(msgs, obj);
    }
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return;
    char path[600];
    session_path(path, sizeof(path), s->id);
    write_file(path, str);
    free(str);
}

void persist_save_all(const SessionManager *sm) {
    ensure_dirs();
    save_index(sm);
    for (int i = 0; i < sm->count; i++)
        persist_save_session(sm, i);
}

void persist_delete_session(int session_id) {
    char path[600];
    session_path(path, sizeof(path), session_id);
    remove(path);
}

int persist_load(SessionManager *sm) {
    char path[600];
    index_path(path, sizeof(path));
    char *buf = read_file(path);
    if (!buf) return 0;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return 0;

    /* Restore next_id from index before creating any sessions */
    cJSON *nid = cJSON_GetObjectItemCaseSensitive(root, "next_id");
    int saved_next_id = cJSON_IsNumber(nid) ? (int)nid->valuedouble : 1;

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "sessions");
    int loaded = 0;
    cJSON *entry;
    cJSON_ArrayForEach(entry, arr) {
        cJSON *id_j   = cJSON_GetObjectItemCaseSensitive(entry, "id");
        cJSON *name_j = cJSON_GetObjectItemCaseSensitive(entry, "name");
        if (!cJSON_IsNumber(id_j) || !cJSON_IsString(name_j)) continue;

        int         id   = (int)id_j->valuedouble;
        const char *name = name_j->valuestring;

        int idx = sm_new_session(sm, name);
        if (idx < 0) break;
        sm->sessions[idx].id = id;   /* override auto-assigned ID with stored one */

        char spath[600];
        session_path(spath, sizeof(spath), id);
        char *sbuf = read_file(spath);
        if (sbuf) {
            cJSON *sroot = cJSON_Parse(sbuf);
            free(sbuf);
            if (sroot) {
                cJSON *msgs = cJSON_GetObjectItemCaseSensitive(sroot, "messages");
                cJSON *msg;
                cJSON_ArrayForEach(msg, msgs) {
                    cJSON *role_j    = cJSON_GetObjectItemCaseSensitive(msg, "role");
                    cJSON *content_j = cJSON_GetObjectItemCaseSensitive(msg, "content");
                    cJSON *rsn_j     = cJSON_GetObjectItemCaseSensitive(msg, "reasoning");
                    if (!cJSON_IsString(role_j) || !cJSON_IsString(content_j)) continue;
                    bool is_user = strcmp(role_j->valuestring, "user") == 0;
                    const char *rsn = (cJSON_IsString(rsn_j) && rsn_j->valuestring)
                                      ? rsn_j->valuestring : NULL;
                    chat_add_r(&sm->sessions[idx].chat,
                               content_j->valuestring, is_user, rsn);
                }
                cJSON_Delete(sroot);
            }
        }
        loaded++;
    }

    /* Always restore next_id to the saved value (loading sessions increments it) */
    sm->next_id = saved_next_id;
    cJSON_Delete(root);
    return loaded;
}
