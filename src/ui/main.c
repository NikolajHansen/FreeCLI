/*
 * FreeCLI - LLM-agnostic terminal UI
 * main.c
 */

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <locale.h>
#include "chat.h"
#include "input.h"
#include "session.h"
#include "persist.h"
#include "commands.h"
#include "ipc.h"
#include "provider_registry.h"

#ifndef _WIN32
# include <unistd.h>
# include <spawn.h>
# include <sys/wait.h>
# include <libgen.h>
# include <time.h>
# if defined(__APPLE__)
#   include <mach-o/dyld.h>
# elif defined(__FreeBSD__) || defined(__DragonFly__)
#   include <sys/types.h>
#   include <sys/sysctl.h>
# endif
  extern char **environ;
#else
# include <windows.h>
# include <time.h>
#endif

/* --- InFlight request tracker --- */

#define MAX_IN_FLIGHT 32
#define WORKERS_H     11   /* rows for the workers pane (incl. header) */

typedef struct {
    uint64_t corr_id;
    uint64_t parent_corr_id; /* 0 = top-level user request */
    int      depth;          /* 0 = top-level, 1+ = sub-worker */
    int      session_idx;
    char     name[64];       /* model name or description */
    time_t   started;
    int      active;
} InFlight;

static InFlight in_flight[MAX_IN_FLIGHT];
static int      spin_tick = 0;           /* incremented every 100ms poll */

/* --- Pending multi-choice picker --- */

#define MAX_CHOICES 8

typedef enum {
    CHOICES_DRAFT  = 0,  /* N parallel drafts — selected becomes assistant message */
    CHOICES_OPTION = 1,  /* LLM-offered options — selected becomes next user message */
} ChoicesMode;

typedef struct {
    int         session_idx;
    int         n;
    char       *choices[MAX_CHOICES];
    int         selected;   /* 0-based highlighted choice */
    ChoicesMode mode;
} PendingChoices;

static PendingChoices *pending_choices = NULL;

/* Forward declaration — defined after draw_layout in source order */
static void draw_choice_picker(WINDOW *win, PendingChoices *pc);

static void inflight_add(uint64_t corr_id, int sidx, const char *name) {
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        if (!in_flight[i].active) {
            in_flight[i].corr_id        = corr_id;
            in_flight[i].parent_corr_id = 0;
            in_flight[i].depth          = 0;
            in_flight[i].session_idx    = sidx;
            in_flight[i].started        = time(NULL);
            in_flight[i].active         = 1;
            strncpy(in_flight[i].name, name ? name : "?", 63);
            in_flight[i].name[63]       = '\0';
            return;
        }
    }
}

static void inflight_add_sub(uint64_t corr_id, uint64_t parent_corr_id,
                              int parent_sidx, const char *model,
                              const char *desc) {
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        if (!in_flight[i].active) {
            char label[64];
            snprintf(label, sizeof(label), "→%s: %.30s",
                     model ? model : "?", desc ? desc : "");
            in_flight[i].corr_id        = corr_id;
            in_flight[i].parent_corr_id = parent_corr_id;
            in_flight[i].depth          = 1;
            in_flight[i].session_idx    = parent_sidx;
            in_flight[i].started        = time(NULL);
            in_flight[i].active         = 1;
            strncpy(in_flight[i].name, label, 63);
            in_flight[i].name[63]       = '\0';
            return;
        }
    }
}

/* Returns session_idx for the completed entry, or -1 if not found. */
static int inflight_remove(uint64_t corr_id) {
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        if (in_flight[i].active && in_flight[i].corr_id == corr_id) {
            in_flight[i].active = 0;
            return in_flight[i].session_idx;
        }
    }
    return -1;
}

/* Find session_idx for a corr_id without removing */
static int inflight_find_sidx(uint64_t corr_id) {
    for (int i = 0; i < MAX_IN_FLIGHT; i++)
        if (in_flight[i].active && in_flight[i].corr_id == corr_id)
            return in_flight[i].session_idx;
    return -1;
}

static int inflight_session_active(int sidx) {
    for (int i = 0; i < MAX_IN_FLIGHT; i++)
        if (in_flight[i].active && in_flight[i].session_idx == sidx
            && in_flight[i].depth == 0) return 1;
    return 0;
}

static int inflight_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_IN_FLIGHT; i++)
        if (in_flight[i].active) n++;
    return n;
}

static WINDOW *win_title, *win_status, *win_sidebar, *win_main,
              *win_workers, *win_decisions, *win_cmd, *win_footer;
#define DECISIONS_W 30

enum { COLOR_PAIR_TITLE=1, COLOR_PAIR_STATUS, COLOR_PAIR_SIDEBAR, COLOR_PAIR_ACCENT, COLOR_PAIR_MUTED };

static SessionManager sessions;
static InputLine       input_line;
static int             focus         = 0;
static int             sidebar_sel   = 0;
static net_fd_t        ipc_fd        = NET_INVALID;
static uint64_t        next_corr     = 1;
static char            selected_model[64]    = "grok-3-fast";
static char            selected_provider[32] = "xai";
static int             selected_n_choices    = 1;

/* --- Backend process management --- */

#ifndef _WIN32
static pid_t backend_pid = -1;

/* Cross-platform: resolve the full path of the running executable. */
static int self_exe_path(char *out, size_t sz) {
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, sz - 1);
    if (n <= 0) return -1;
    out[n] = '\0';
    return 0;
#elif defined(__APPLE__)
    uint32_t len = (uint32_t)sz;
    if (_NSGetExecutablePath(out, &len) != 0) return -1;
    return 0;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t len = sz;
    if (sysctl(mib, 4, out, &len, NULL, 0) != 0) return -1;
    return 0;
#else
    /* Generic fallback: search PATH for argv[0] — not ideal but portable */
    (void)out; (void)sz;
    return -1;
#endif
}

static int spawn_backend(int port) {
    char self_path[512] = {0};
    if (self_exe_path(self_path, sizeof(self_path)) != 0) return -1;

    char dir_buf[512];
    strncpy(dir_buf, self_path, sizeof(dir_buf) - 1);
    char *dir = dirname(dir_buf);

    char backend_path[512];
    snprintf(backend_path, sizeof(backend_path), "%s/freecli-backend", dir);

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    char *argv[] = { backend_path, port_str, NULL };
    pid_t pid;
    int r = posix_spawn(&pid, backend_path, NULL, NULL, argv, environ);
    if (r != 0) return -1;
    backend_pid = pid;
    return 0;
}

static void stop_backend(void) {
    if (backend_pid > 0) {
        kill(backend_pid, SIGTERM);
        waitpid(backend_pid, NULL, 0);
        backend_pid = -1;
    }
}

#else /* Windows */

static HANDLE backend_handle = NULL;

static int spawn_backend(int port) {
    char module_path[MAX_PATH];
    GetModuleFileNameA(NULL, module_path, MAX_PATH);

    char dir[MAX_PATH];
    strncpy(dir, module_path, MAX_PATH);
    PathRemoveFileSpecA(dir);

    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof(cmd), "%s\\freecli-backend.exe %d", dir, port);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return -1;

    backend_handle = pi.hProcess;
    CloseHandle(pi.hThread);
    return 0;
}

static void stop_backend(void) {
    if (backend_handle) {
        TerminateProcess(backend_handle, 0);
        CloseHandle(backend_handle);
        backend_handle = NULL;
    }
}
#endif /* Windows */

/* Bind to a random port, spawn backend, accept its connection.
 * Returns 0 on success; ipc_fd is set. */
static int start_backend(void) {
    net_fd_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == NET_INVALID) return -1;

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0; /* OS assigns */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0) {
        net_close(listen_fd);
        return -1;
    }

    /* Find out which port was assigned */
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    getsockname(listen_fd, (struct sockaddr *)&bound, &blen);
    int port = ntohs(bound.sin_port);

    if (spawn_backend(port) != 0) {
        net_close(listen_fd);
        return -1;
    }

    /* Give backend a moment to connect (accept with short timeout) */
#ifndef _WIN32
    {
        struct timeval tv = { 5, 0 };
        setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&tv, sizeof(tv));
    }
#endif

    ipc_fd = accept(listen_fd, NULL, NULL);
    net_close(listen_fd);
    if (ipc_fd == NET_INVALID) return -1;

    net_set_nonblock(ipc_fd);
    return 0;
}

/* --- Window management --- */

static void init_colors(void) {
    start_color(); use_default_colors();
    init_pair(COLOR_PAIR_TITLE,   COLOR_WHITE, COLOR_BLUE);
    init_pair(COLOR_PAIR_STATUS,  COLOR_CYAN,  -1);
    init_pair(COLOR_PAIR_SIDEBAR, COLOR_WHITE, -1);
    init_pair(COLOR_PAIR_ACCENT,  COLOR_BLUE,  -1);
    init_pair(COLOR_PAIR_MUTED,   COLOR_BLACK + 8, -1);
}

static void create_windows(int rows, int cols) {
    int content_h = rows - 4;  /* rows minus title+status+cmd+footer */
    int dec_h     = content_h - WORKERS_H;
    if (dec_h < 2) dec_h = 2;

    win_title     = newwin(1, cols, 0, 0);
    win_status    = newwin(1, cols, 1, 0);
    win_sidebar   = newwin(content_h, 20, 2, 0);
    win_main      = newwin(content_h, cols - 20 - DECISIONS_W, 2, 20);
    win_workers   = newwin(WORKERS_H, DECISIONS_W, 2, cols - DECISIONS_W);
    win_decisions = newwin(dec_h, DECISIONS_W, 2 + WORKERS_H, cols - DECISIONS_W);
    win_cmd       = newwin(1, cols, rows - 2, 0);
    win_footer    = newwin(1, cols, rows - 1, 0);
}

static void destroy_windows(void) {
    delwin(win_title);   delwin(win_status);  delwin(win_sidebar);
    delwin(win_main);    delwin(win_workers); delwin(win_decisions);
    delwin(win_cmd);     delwin(win_footer);
}

static void draw_sidebar(void) {
    wbkgd(win_sidebar, COLOR_PAIR(COLOR_PAIR_SIDEBAR));
    werase(win_sidebar);
    int n = sm_count(&sessions);
    int h = getmaxy(win_sidebar) - 2;
    mvwprintw(win_sidebar, 0, 1, "Conversations");
    wmove(win_sidebar, 1, 0);
    whline(win_sidebar, ACS_HLINE, 20);

    static const int DISP_W = 17;  /* usable name width inside the box */

    for (int i = 0; i < n && i < h; i++) {
        Session *s = sm_get(&sessions, i);
        if (!s) continue;

        int is_active = (i == sessions.active);
        int is_sel    = (i == sidebar_sel && focus == 0);
        int is_highlighted = (is_active || is_sel);

        /* Compute display slice with left-scroll for highlighted long names */
        int name_len = (int)strlen(s->name);
        int offset = 0;
        if (is_highlighted && name_len > DISP_W) {
            int range  = name_len - DISP_W;
            /* cycle: 4 ticks pause at start, range ticks scroll, 4 ticks pause at end */
            int period = range + 8;
            int t = (spin_tick / 3) % period;
            if (t < 4)            offset = 0;
            else if (t < 4+range) offset = t - 4;
            else                  offset = range;
        }

        char buf[18];
        strncpy(buf, s->name + offset, DISP_W);
        buf[DISP_W] = '\0';

        if (is_active && is_sel)
            wattron(win_sidebar, A_REVERSE | A_BOLD);
        else if (is_active)
            wattron(win_sidebar, A_BOLD);
        else if (is_sel)
            wattron(win_sidebar, A_REVERSE);
        mvwprintw(win_sidebar, i + 2, 1, "%-17s", buf);
        wattroff(win_sidebar, A_REVERSE | A_BOLD);
    }
    mvwprintw(win_sidebar, getmaxy(win_sidebar)-1, 1, "[n]ew [d]el");
    box(win_sidebar, 0, 0);
    wrefresh(win_sidebar);
}

static void draw_workers(void) {
    static const char spinner[] = "|/-\\";
    int w = getmaxx(win_workers) - 2;

    wbkgd(win_workers, COLOR_PAIR(COLOR_PAIR_MUTED));
    werase(win_workers);
    wattron(win_workers, A_BOLD);
    mvwaddstr(win_workers, 0, 1, "Workers");
    wattroff(win_workers, A_BOLD);
    wmove(win_workers, 1, 0);
    whline(win_workers, ACS_HLINE, DECISIONS_W);

    int y = 2;
    int max_y = getmaxy(win_workers) - 1;
    int any = 0;
    time_t now = time(NULL);

    /* Top-level workers first, then sub-workers below their parents */
    for (int pass = 0; pass <= 1 && y < max_y; pass++) {
        for (int i = 0; i < MAX_IN_FLIGHT && y < max_y; i++) {
            if (!in_flight[i].active) continue;
            if (pass == 0 && in_flight[i].depth != 0) continue;
            if (pass == 1 && in_flight[i].depth == 0) continue;

            any = 1;
            long elapsed = (long)(now - in_flight[i].started);
            char spin = spinner[spin_tick % 4];

            int bar_max = (w > 10) ? w - 10 : 1;
            int bar_len = (int)(elapsed / 2);
            if (bar_len > bar_max) bar_len = bar_max;

            char line[128];
            if (in_flight[i].depth > 0) {
                /* Sub-worker: indented, cyan tint */
                char truncname[20];
                strncpy(truncname, in_flight[i].name, 18);
                truncname[18] = '\0';
                snprintf(line, sizeof(line), "  %c %-16s %3lds",
                         spin, truncname, elapsed);
                wattron(win_workers, COLOR_PAIR(COLOR_PAIR_STATUS));
                mvwaddnstr(win_workers, y, 1, line, w);
                wattroff(win_workers, COLOR_PAIR(COLOR_PAIR_STATUS));
            } else {
                char truncname[20];
                strncpy(truncname, in_flight[i].name, 18);
                truncname[18] = '\0';
                snprintf(line, sizeof(line), "%c %-18s %3lds",
                         spin, truncname, elapsed);
                mvwaddnstr(win_workers, y, 1, line, w);

                /* Progress bar */
                wmove(win_workers, y, w - bar_max - 1);
                wattron(win_workers, COLOR_PAIR(COLOR_PAIR_ACCENT));
                for (int b = 0; b < bar_len; b++) waddch(win_workers, ACS_CKBOARD);
                wattroff(win_workers, COLOR_PAIR(COLOR_PAIR_ACCENT));
            }
            y++;
        }
    }
    if (!any) {
        wattron(win_workers, A_DIM);
        mvwaddstr(win_workers, 2, 1, "idle");
        wattroff(win_workers, A_DIM);
    }
    box(win_workers, 0, 0);
    wrefresh(win_workers);
}

static void draw_layout(void) {
    wbkgd(win_title, COLOR_PAIR(COLOR_PAIR_TITLE));
    werase(win_title);
    mvwprintw(win_title, 0, 2, " FreeCLI v0.1 ");
    mvwprintw(win_title, 0, COLS - 20, " [user@host] ");
    wrefresh(win_title);

    wbkgd(win_status, COLOR_PAIR(COLOR_PAIR_STATUS));
    werase(win_status);
    Session *active = sm_active_session(&sessions);
    int n_busy = inflight_count();
    if (n_busy > 0)
        mvwprintw(win_status, 0, 2, " %s/%s: %d request%s in flight | %s",
                  selected_provider, selected_model,
                  n_busy, n_busy > 1 ? "s" : "",
                  active ? active->name : "");
    else
        mvwprintw(win_status, 0, 2, " %s/%s | %s",
                  selected_provider, selected_model,
                  active ? active->name : "");
    wrefresh(win_status);

    draw_sidebar();

    ChatBuffer *cb = sm_active_chat(&sessions);
    wbkgd(win_main, COLOR_PAIR(COLOR_PAIR_SIDEBAR));
    if (cb) chat_render(cb, win_main);
    else { werase(win_main); wrefresh(win_main); }

    /* Overlay choice picker on top of chat if pending for this session */
    if (pending_choices && pending_choices->session_idx == sessions.active)
        draw_choice_picker(win_main, pending_choices);

    /* Workers pane + Decisions pane */
    draw_workers();

    /* Decisions pane */
    wbkgd(win_decisions, COLOR_PAIR(COLOR_PAIR_MUTED));
    werase(win_decisions);
    wattron(win_decisions, A_BOLD);
    mvwaddstr(win_decisions, 0, 1, "Reasoning");
    wattroff(win_decisions, A_BOLD);
    wmove(win_decisions, 1, 0); whline(win_decisions, ACS_HLINE, DECISIONS_W);
    if (cb) chat_render_decisions(cb, win_decisions);
    box(win_decisions, 0, 0);
    wrefresh(win_decisions);

    wbkgd(win_cmd, focus == 2 ? A_REVERSE : COLOR_PAIR(COLOR_PAIR_MUTED));
    input_render(&input_line, win_cmd);

    wbkgd(win_footer, COLOR_PAIR(COLOR_PAIR_MUTED));
    werase(win_footer);
    if (focus == 0) {
        mvwprintw(win_footer, 0, 2,
            "[↑↓] Select  [Enter/→] Open  [n] New  [Tab] To input");
    } else if (pending_choices && pending_choices->session_idx == sessions.active) {
        mvwprintw(win_footer, 0, 2,
            "[1-%d] Pick  [↑↓] Highlight  [Enter] Accept  [Tab] Cancel",
            pending_choices->n);
    } else if (input_line.len > 0 && input_line.buffer[0] == '/') {
        const char *hint = cmd_hint(input_line.buffer);
        if (hint) mvwprintw(win_footer, 0, 2, "%s", hint);
    } else {
        mvwprintw(win_footer, 0, 2,
            "[Tab/←] Sidebar  [Enter] Send  [PgUp/Dn] Scroll  [n] New  [q] Quit  [/] Commands");
    }
    wrefresh(win_footer);

    if (focus == 2) {
        curs_set(1);
        wmove(win_cmd, 0, input_line.pos);
        wrefresh(win_cmd);
    } else {
        curs_set(0);
    }
    doupdate();
}

static void handle_resize(void) {
    endwin(); refresh();
    int rows, cols; getmaxyx(stdscr, rows, cols);
    destroy_windows();
    create_windows(rows, cols);
    draw_layout();
}

/* --- Persist callbacks (used by CmdCtx) --- */

static void do_draw(void) { draw_layout(); }

static void do_save(const SessionManager *sm, int idx) {
    if (idx == -1) persist_save_all(sm);
    else           persist_save_session(sm, idx);
}

static void do_delete_session(int idx) {
    if (idx < 0 || idx >= sm_count(&sessions)) return;
    int del_id = sessions.sessions[idx].id;
    int new_active = sm_remove(&sessions, idx);
    persist_delete_session(del_id);
    if (sm_count(&sessions) == 0)
        sm_new_session(&sessions, "Chat 1");
    if (sidebar_sel >= sm_count(&sessions))
        sidebar_sel = sm_count(&sessions) - 1;
    if (new_active >= 0) sm_select(&sessions, new_active);
    else                 sm_select(&sessions, sidebar_sel);
    persist_save_all(&sessions);
}

static void do_set_model(const char *model) {
    if (model && *model) {
        strncpy(selected_model, model, sizeof(selected_model) - 1);
        selected_model[sizeof(selected_model) - 1] = '\0';
    }
}

static void do_set_provider(const char *provider_id) {
    if (provider_id && *provider_id) {
        strncpy(selected_provider, provider_id, sizeof(selected_provider) - 1);
        selected_provider[sizeof(selected_provider) - 1] = '\0';
    }
}

static void do_set_n_choices(int n) {
    if (n >= 1 && n <= MAX_CHOICES) selected_n_choices = n;
}

/*
 * Scan text for a numbered or lettered choice list (e.g. "1. ...\n2. ...\n3. ...").
 * Returns number of choices found (0 if none / fewer than 2).
 * Populates pc->choices[] and pc->n on success.
 * pc must be zero-initialised by caller.
 */
static int detect_choices(const char *text, PendingChoices *pc) {
    if (!text) return 0;
    pc->n = 0;

    const char *p = text;
    int expected = 1;          /* next integer bullet expected */
    int use_letters = 0;       /* 0 = numeric, 1 = A/B/C... */

    /* Two-pass: try numeric first, then letter-based */
    for (int pass = 0; pass < 2 && pc->n < 2; pass++) {
        pc->n = 0;
        for (int i = 0; i < pc->n; i++) { free(pc->choices[i]); pc->choices[i] = NULL; }
        expected = pass == 0 ? 1 : 0; /* 0 = 'A' */
        use_letters = pass;

        const char *scan = text;
        while (*scan && pc->n < MAX_CHOICES) {
            /* find start of a line */
            const char *line = scan;
            while (*line == '\r') line++;

            /* skip leading spaces */
            const char *t = line;
            while (*t == ' ' || *t == '\t') t++;

            int matched = 0;
            const char *item_start = NULL;

            if (!use_letters) {
                /* try "N. " or "N) " where N == expected */
                int num = 0;
                const char *q = t;
                while (*q >= '0' && *q <= '9') { num = num * 10 + (*q - '0'); q++; }
                if (num == expected && q > t &&
                    (*q == '.' || *q == ')') &&
                    (q[1] == ' ' || q[1] == '\t')) {
                    item_start = q + 2;
                    while (*item_start == ' ') item_start++;
                    matched = 1;
                }
            } else {
                /* try "A. " or "A) " (case-insensitive, expected offset from 'A') */
                char want = (char)('A' + expected);
                if ((*t == want || *t == want + 32) &&
                    (t[1] == '.' || t[1] == ')') &&
                    (t[2] == ' ' || t[2] == '\t')) {
                    item_start = t + 3;
                    while (*item_start == ' ') item_start++;
                    matched = 1;
                }
            }

            if (matched && item_start) {
                /* read to end of this item (until next numbered bullet or blank line) */
                const char *end = item_start;
                while (*end) {
                    /* peek at next line for a new bullet */
                    if (*end == '\n') {
                        const char *next = end + 1;
                        while (*next == ' ' || *next == '\t') next++;
                        /* stop if empty line or next bullet starts */
                        if (*next == '\n' || *next == '\r' || *next == '\0') break;
                        int is_bullet = 0;
                        if (!use_letters) {
                            int nn = 0; const char *q2 = next;
                            while (*q2 >= '0' && *q2 <= '9') { nn = nn * 10 + (*q2 - '0'); q2++; }
                            if (nn == expected + 1 && q2 > next && (*q2 == '.' || *q2 == ')'))
                                is_bullet = 1;
                        } else {
                            char nw = (char)('A' + expected + 1);
                            if ((*next == nw || *next == nw + 32) &&
                                (next[1] == '.' || next[1] == ')'))
                                is_bullet = 1;
                        }
                        if (is_bullet) break;
                    }
                    end++;
                }
                /* trim trailing whitespace */
                while (end > item_start && (end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\r'))
                    end--;
                int len = (int)(end - item_start);
                if (len > 0) {
                    pc->choices[pc->n] = malloc(len + 1);
                    if (pc->choices[pc->n]) {
                        memcpy(pc->choices[pc->n], item_start, len);
                        pc->choices[pc->n][len] = '\0';
                        pc->n++;
                        expected++;
                    }
                }
                scan = end;
            } else {
                /* not a matching bullet — skip to end of line */
                while (*scan && *scan != '\n') scan++;
                if (*scan == '\n') scan++;
            }
        }
    }

    if (pc->n < 2) {
        for (int i = 0; i < pc->n; i++) { free(pc->choices[i]); pc->choices[i] = NULL; }
        pc->n = 0;
        return 0;
    }
    return pc->n;
}

/* Submit an arbitrary text string as the next user message. */
static void submit_text(const char *text);   /* forward decl */

/* Accept one of the pending choices. */
static void accept_choice(int idx) {
    if (!pending_choices || idx < 0 || idx >= pending_choices->n) return;
    int sidx = pending_choices->session_idx;
    if (sidx < 0 || sidx >= sm_count(&sessions)) return;

    char *chosen = pending_choices->choices[idx]
                   ? strdup(pending_choices->choices[idx]) : NULL;
    ChoicesMode mode = pending_choices->mode;

    for (int i = 0; i < pending_choices->n; i++)
        free(pending_choices->choices[i]);
    free(pending_choices);
    pending_choices = NULL;

    if (!chosen) return;

    if (mode == CHOICES_DRAFT) {
        /* Draft picker: chosen text becomes the accepted assistant reply */
        ChatBuffer *cb = &sessions.sessions[sidx].chat;
        chat_add_r(cb, chosen, false, NULL);
        cb->scroll = 0;
        persist_save_session(&sessions, sidx);
    } else {
        /* Option picker: chosen text becomes the next user message */
        sm_select(&sessions, sidx);
        sessions.active = sidx;
        submit_text(chosen);
    }
    free(chosen);
}

/* Draw choice picker overlay at the bottom of win_main. */
static void draw_choice_picker(WINDOW *win, PendingChoices *pc) {
    int rows, cols;
    getmaxyx(win, rows, cols);
    int n = pc->n;
    int start_row = rows - n - 3;
    if (start_row < 1) start_row = 1;

    /* Separator line */
    wattron(win, A_DIM);
    wmove(win, start_row, 0);
    whline(win, ACS_HLINE, cols);
    const char *action = (pc->mode == CHOICES_OPTION) ? "Select option" : "Pick response";
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "[ %s: 1-%d  ↑↓  Enter ]", action, n);
    mvwaddstr(win, start_row, 2, hdr);
    wattroff(win, A_DIM);

    for (int i = 0; i < n && (start_row + 1 + i) < rows - 1; i++) {
        int row = start_row + 1 + i;
        wmove(win, row, 0);
        wclrtoeol(win);
        if (i == pc->selected)
            wattron(win, A_REVERSE);
        /* Truncate choice preview to one line */
        char preview[256] = {0};
        const char *src = pc->choices[i] ? pc->choices[i] : "";
        /* strip leading whitespace */
        while (*src == '\n' || *src == ' ') src++;
        int avail = cols - 6;
        if (avail < 1) avail = 1;
        int slen = (int)strlen(src);
        /* take first line only */
        const char *nl = strchr(src, '\n');
        int take = nl ? (int)(nl - src) : slen;
        if (take > avail) take = avail;
        snprintf(preview, sizeof(preview), " [%d] %.*s", i + 1, take, src);
        mvwaddnstr(win, row, 0, preview, cols);
        if (i == pc->selected)
            wattroff(win, A_REVERSE);
    }
    wrefresh(win);
}

/* --- IPC --- */

static void check_ipc(void) {
    if (ipc_fd == NET_INVALID) return;

    spin_tick++;   /* advance spinner animation every 100ms */

    IpcMsg msg;
    int r = ipc_recv(ipc_fd, &msg);
    if (r < 0) {
        net_close(ipc_fd);
        ipc_fd = NET_INVALID;
        /* Clear all in-flight trackers */
        for (int i = 0; i < MAX_IN_FLIGHT; i++) in_flight[i].active = 0;
        draw_layout();
        return;
    }
    /* Always redraw workers pane to keep spinner live, even if no new message */
    if (inflight_count() > 0) draw_workers();

    /* Redraw sidebar to animate name scrolling for long selected names */
    {
        Session *sel = sm_get(&sessions, sidebar_sel);
        if (sel && (int)strlen(sel->name) > 17) draw_sidebar();
    }

    if (r == 0) return;

    if (msg.type == MSG_REP_FINAL && msg.payload) {
        uint32_t sidx;
        char *content = NULL, *reasoning = NULL;
        if (ipc_decode_rep_final(msg.payload, msg.payload_len,
                                  &sidx, &content, &reasoning) == 0) {
            /* Find session via corr_id in InFlight (sidx is a hint, corr_id is authoritative) */
            int real_sidx = inflight_remove(msg.corr_id);
            if (real_sidx >= 0) sidx = (uint32_t)real_sidx;

            ChatBuffer *cb = ((int)sidx < sm_count(&sessions))
                ? &sessions.sessions[sidx].chat
                : sm_active_chat(&sessions);
            if (cb) {
                chat_remove_last(cb);   /* remove "..." placeholder */
                chat_add_r(cb, content && *content ? content : "(no response)",
                           false, reasoning);
                cb->scroll = 0;
                persist_save_session(&sessions, (int)sidx);

                /* Auto-detect numbered/lettered option lists in the reply */
                const char *reply_text = cb->msgs[cb->count - 1].text;
                if (reply_text && !pending_choices) {
                    PendingChoices *pc = calloc(1, sizeof(PendingChoices));
                    if (pc && detect_choices(reply_text, pc) >= 2) {
                        pc->session_idx = (int)sidx;
                        pc->selected    = 0;
                        pc->mode        = CHOICES_OPTION;
                        if (pending_choices) {
                            for (int i = 0; i < pending_choices->n; i++)
                                free(pending_choices->choices[i]);
                            free(pending_choices);
                        }
                        pending_choices = pc;
                    } else {
                        free(pc);
                    }
                }
            }
            free(content);
            free(reasoning);
        }
        draw_layout();
    } else if (msg.type == MSG_REP_CHOICES && msg.payload) {
        uint32_t sidx = 0;
        char **choices = NULL;
        int n = 0;
        if (ipc_decode_rep_choices(msg.payload, msg.payload_len,
                                    &sidx, &choices, &n) == 0 && n > 0) {
            int real_sidx = inflight_remove(msg.corr_id);
            if (real_sidx >= 0) sidx = (uint32_t)real_sidx;

            ChatBuffer *cb = ((int)sidx < sm_count(&sessions))
                ? &sessions.sessions[sidx].chat
                : sm_active_chat(&sessions);
            if (cb) chat_remove_last(cb);   /* remove "..." placeholder */

            /* Free previous pending choices if any */
            if (pending_choices) {
                for (int i = 0; i < pending_choices->n; i++)
                    free(pending_choices->choices[i]);
                free(pending_choices);
            }
            pending_choices = calloc(1, sizeof(PendingChoices));
            if (pending_choices) {
                pending_choices->session_idx = (int)sidx;
                pending_choices->n = n > MAX_CHOICES ? MAX_CHOICES : n;
                pending_choices->mode = CHOICES_DRAFT;
                for (int i = 0; i < pending_choices->n; i++)
                    pending_choices->choices[i] = choices[i] ? choices[i] : strdup("");
                pending_choices->selected = 0;
            } else {
                for (int i = 0; i < n; i++) free(choices[i]);
            }
            free(choices);
        }
        draw_layout();
        /* Draw choice picker on top of chat */
        if (pending_choices && pending_choices->session_idx == sessions.active)
            draw_choice_picker(win_main, pending_choices);
    } else if (msg.type == MSG_REP_ERROR) {
        int sidx = inflight_remove(msg.corr_id);
        ChatBuffer *cb = (sidx >= 0 && sidx < sm_count(&sessions))
            ? &sessions.sessions[sidx].chat
            : sm_active_chat(&sessions);
        if (cb) {
            chat_remove_last(cb);   /* remove "..." placeholder */
            const char *err = msg.payload
                ? (const char *)msg.payload : "backend error";
            chat_add(cb, err, false);
        }
        draw_layout();
    } else if (msg.type == MSG_WORKER_START && msg.payload) {
        uint64_t parent_corr = 0;
        char *sub_model = NULL, *desc = NULL;
        if (ipc_decode_worker_start(msg.payload, msg.payload_len,
                                     &parent_corr, &sub_model, &desc) == 0) {
            int parent_sidx = inflight_find_sidx(parent_corr);
            inflight_add_sub(msg.corr_id, parent_corr,
                             parent_sidx >= 0 ? parent_sidx : sessions.active,
                             sub_model, desc);
        }
        free(sub_model);
        free(desc);
        draw_workers();
    } else if (msg.type == MSG_WORKER_END) {
        inflight_remove(msg.corr_id);
        draw_workers();
    } else if (msg.type == MSG_SESSION_RENAME && msg.payload) {
        uint32_t sidx = 0;
        char *topic = NULL;
        if (ipc_decode_session_rename(msg.payload, msg.payload_len,
                                       &sidx, &topic) == 0
            && topic && *topic
            && (int)sidx < sm_count(&sessions)) {
            Session *s = sm_get(&sessions, (int)sidx);
            /* Only rename if still a default "Chat N" name */
            if (s) {
                int n;
                char expected[32];
                if (sscanf(s->name, "Chat %d", &n) == 1) {
                    snprintf(expected, sizeof(expected), "Chat %d", n);
                    if (strcmp(s->name, expected) == 0) {
                        /* Format: "#<id> <topic>" */
                        snprintf(s->name, sizeof(s->name), "#%d %s", s->id, topic);
                        persist_save_all(&sessions);
                        draw_sidebar();
                    }
                }
            }
        }
        free(topic);
    }

    free(msg.payload);
}

/*
 * Core send logic — adds text as user message and dispatches to backend.
 * Used by both submit_message (from input bar) and accept_choice (option mode).
 */
static void submit_text(const char *text) {
    if (!text || !*text) return;
    ChatBuffer *cb = sm_active_chat(&sessions);
    if (!cb) return;
    if (ipc_fd == NET_INVALID) return;
    if (inflight_session_active(sessions.active)) return;

    chat_add(cb, text, true);
    chat_add(cb, "...", false);
    draw_layout();

    uint32_t msg_count = (uint32_t)(cb->count - 1);
    const char **texts   = malloc(msg_count * sizeof(char *));
    uint8_t     *is_user = malloc(msg_count);
    for (uint32_t i = 0; i < msg_count; i++) {
        texts[i]   = cb->msgs[i].text;
        is_user[i] = (uint8_t)cb->msgs[i].is_user;
    }

    uint32_t payload_len;
    uint8_t *payload = ipc_encode_req_send((uint32_t)sessions.active,
                                            msg_count, texts, is_user,
                                            selected_model,
                                            selected_provider,
                                            1,   /* n_choices always 1 for option replies */
                                            &payload_len);
    free(texts);
    free(is_user);

    if (payload) {
        uint64_t corr = next_corr++;
        Session *s = sm_active_session(&sessions);
        inflight_add(corr, sessions.active, s ? s->name : "?");
        ipc_send(ipc_fd, MSG_REQ_SEND, corr, payload, payload_len);
        free(payload);
    }
}

static void submit_message(void) {
    ChatBuffer *cb = sm_active_chat(&sessions);
    if (!cb || input_line.len == 0) return;

    /* Intercept /commands — handle locally, never send to backend */
    if (input_line.buffer[0] == '/') {
        char cmd_buf[256];
        strncpy(cmd_buf, input_line.buffer, sizeof(cmd_buf) - 1);
        cmd_buf[sizeof(cmd_buf) - 1] = '\0';
        input_reset(&input_line);
        CmdCtx ctx = {
            .sessions         = &sessions,
            .sidebar_sel      = &sidebar_sel,
            .focus            = &focus,
            .draw             = do_draw,
            .save_session     = do_save,
            .delete_session   = do_delete_session,
            .set_model        = do_set_model,
            .set_provider     = do_set_provider,
            .set_n_choices    = do_set_n_choices,
            .current_model    = selected_model,
            .current_provider = selected_provider,
            .n_choices_val    = selected_n_choices,
        };
        cmd_dispatch(cmd_buf, &ctx);
        return;
    }

    /* Dismiss any pending option picker — user typed instead of selecting */
    if (pending_choices && pending_choices->mode == CHOICES_OPTION) {
        for (int i = 0; i < pending_choices->n; i++)
            free(pending_choices->choices[i]);
        free(pending_choices);
        pending_choices = NULL;
    }

    char text[sizeof(input_line.buffer)];
    strncpy(text, input_line.buffer, sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    input_reset(&input_line);

    if (ipc_fd == NET_INVALID) return;
    if (inflight_session_active(sessions.active)) return;

    chat_add(cb, text, true);
    chat_add(cb, "...", false);
    draw_layout();

    uint32_t msg_count = (uint32_t)(cb->count - 1);
    const char **texts   = malloc(msg_count * sizeof(char *));
    uint8_t     *is_user = malloc(msg_count);
    for (uint32_t i = 0; i < msg_count; i++) {
        texts[i]   = cb->msgs[i].text;
        is_user[i] = (uint8_t)cb->msgs[i].is_user;
    }

    uint32_t payload_len;
    uint8_t *payload = ipc_encode_req_send((uint32_t)sessions.active,
                                            msg_count, texts, is_user,
                                            selected_model,
                                            selected_provider,
                                            selected_n_choices,
                                            &payload_len);
    free(texts);
    free(is_user);

    if (payload) {
        uint64_t corr = next_corr++;
        Session *s = sm_active_session(&sessions);
        inflight_add(corr, sessions.active, s ? s->name : "?");
        ipc_send(ipc_fd, MSG_REQ_SEND, corr, payload, payload_len);
        free(payload);
    }
}

static void new_session(void) {
    char name[64];
    snprintf(name, sizeof(name), "Chat %d", sm_count(&sessions) + 1);
    int idx = sm_new_session(&sessions, name);
    if (idx >= 0) {
        sm_select(&sessions, idx);
        sidebar_sel = idx;
        focus = 2;
        persist_save_all(&sessions);
    }
    draw_layout();
}

/* --- Main --- */

int main(void) {
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");

    net_init();

    initscr();
    refresh();
    cbreak(); noecho(); keypad(stdscr, TRUE);
    if (has_colors()) init_colors();

    int rows, cols; getmaxyx(stdscr, rows, cols);
    create_windows(rows, cols);

    sm_init(&sessions);
    int loaded = persist_load(&sessions);
    if (loaded == 0) {
        sm_new_session(&sessions, "Chat 1");
    }
    sm_select(&sessions, 0);
    sidebar_sel = 0;
    focus = 2;
    input_init(&input_line);

    /* Status: attempting to start backend */
    wbkgd(win_status, COLOR_PAIR(COLOR_PAIR_STATUS));
    werase(win_status);
    mvwprintw(win_status, 0, 2, " Starting backend...");
    wrefresh(win_status);

    if (start_backend() != 0) {
        endwin();
        fprintf(stderr, "Failed to start freecli-backend. "
                        "Ensure it is in the same directory.\n");
        net_cleanup();
        return 1;
    }

    wtimeout(stdscr, 100);
    draw_layout();

    int ch;
    for (;;) {
        ch = getch();

        if (ch == ERR) { check_ipc(); continue; }
        if (ch == KEY_RESIZE) { handle_resize(); continue; }

        if (focus == 0) {
            if (ch == 'n') { new_session(); continue; }
            if (ch == 'q') break;
            if (ch == 'd' && sm_count(&sessions) > 0) {
                do_delete_session(sidebar_sel);
                draw_layout();
                continue;
            }
            if (ch == KEY_UP && sidebar_sel > 0) {
                sidebar_sel--;
                draw_sidebar();
                Session *s = sm_get(&sessions, sidebar_sel);
                wbkgd(win_main, COLOR_PAIR(COLOR_PAIR_SIDEBAR));
                if (s) {
                    chat_render(&s->chat, win_main);
                    chat_render_decisions(&s->chat, win_decisions);
                }
            } else if (ch == KEY_DOWN && sidebar_sel < sm_count(&sessions)-1) {
                sidebar_sel++;
                draw_sidebar();
                Session *s = sm_get(&sessions, sidebar_sel);
                wbkgd(win_main, COLOR_PAIR(COLOR_PAIR_SIDEBAR));
                if (s) {
                    chat_render(&s->chat, win_main);
                    chat_render_decisions(&s->chat, win_decisions);
                }
            } else if (ch == '\n' || ch == KEY_ENTER || ch == KEY_RIGHT || ch == '\t') {
                sm_select(&sessions, sidebar_sel);
                focus = 2;
                draw_layout();
            }
        } else { /* focus == 2: input */
            if (ch == KEY_LEFT || ch == '\t') {
                /* Cancel pending choices on focus change */
                if (pending_choices) {
                    for (int i = 0; i < pending_choices->n; i++)
                        free(pending_choices->choices[i]);
                    free(pending_choices);
                    pending_choices = NULL;
                }
                focus = 0;
                draw_layout();
                continue;
            }

            /* --- Choice picker intercepts --- */
            if (pending_choices &&
                pending_choices->session_idx == sessions.active) {
                /* Number keys 1–N: immediate selection */
                if (ch >= '1' && ch < '1' + pending_choices->n) {
                    accept_choice(ch - '1');
                    draw_layout();
                    continue;
                }
                /* Arrow keys move highlight */
                if (ch == KEY_UP && pending_choices->selected > 0) {
                    pending_choices->selected--;
                    draw_layout();
                    draw_choice_picker(win_main, pending_choices);
                    continue;
                }
                if (ch == KEY_DOWN &&
                    pending_choices->selected < pending_choices->n - 1) {
                    pending_choices->selected++;
                    draw_layout();
                    draw_choice_picker(win_main, pending_choices);
                    continue;
                }
                /* Enter (with empty input) confirms highlighted choice */
                if ((ch == '\n' || ch == KEY_ENTER) && input_line.len == 0) {
                    accept_choice(pending_choices->selected);
                    draw_layout();
                    continue;
                }
            }
            if (ch == KEY_PPAGE) {
                ChatBuffer *cb = sm_active_chat(&sessions);
                if (cb) { chat_scroll(cb, -3); chat_render(cb, win_main); }
                continue;
            }
            if (ch == KEY_NPAGE) {
                ChatBuffer *cb = sm_active_chat(&sessions);
                if (cb) { chat_scroll(cb, 3); chat_render(cb, win_main); }
                continue;
            }
            bool submitted = false;
            input_handle_key(&input_line, ch, &submitted);
            if (submitted) {
                submit_message();
            } else {
                input_render(&input_line, win_cmd);
                wmove(win_cmd, 0, input_line.pos);
                wrefresh(win_cmd);
            }
        }
    }

    for (int i = 0; i < sm_count(&sessions); i++)
        chat_free(&sessions.sessions[i].chat);
    destroy_windows();
    endwin();

    persist_save_all(&sessions);
    if (ipc_fd != NET_INVALID) net_close(ipc_fd);
    stop_backend();
    net_cleanup();
    return EXIT_SUCCESS;
}
