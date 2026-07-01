#include "chat.h"
#include "utf8.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAP 64

static void chat_ensure_capacity(ChatBuffer *cb) {
    if (cb->count >= cb->capacity) {
        cb->capacity = cb->capacity ? cb->capacity * 2 : INITIAL_CAP;
        cb->msgs = realloc(cb->msgs, cb->capacity * sizeof(Message));
    }
}

static void message_clear_extensions(Message *m) {
    toolcall_array_free(m->tool_calls, m->n_tool_calls);
    free(m->tool_calls);
    m->tool_calls   = NULL;
    m->n_tool_calls = 0;
    free(m->tool_call_id);
    m->tool_call_id = NULL;
}

void toolcall_array_free(ToolCall *tc, int n) {
    if (!tc) return;
    for (int i = 0; i < n; i++) {
        free(tc[i].id);
        free(tc[i].name);
        free(tc[i].arguments);
    }
}

void chat_init(ChatBuffer *cb) {
    cb->msgs = NULL;
    cb->count = cb->capacity = cb->scroll = 0;
}

void chat_add(ChatBuffer *cb, const char *text, bool is_user) {
    chat_ensure_capacity(cb);
    Message *m       = &cb->msgs[cb->count];
    m->text          = strdup(text);
    m->is_user       = is_user;
    m->reasoning     = NULL;
    m->tool_calls    = NULL;
    m->n_tool_calls  = 0;
    m->tool_call_id  = NULL;
    cb->count++;
}

void chat_add_r(ChatBuffer *cb, const char *text, bool is_user, const char *reasoning) {
    chat_ensure_capacity(cb);
    Message *m       = &cb->msgs[cb->count];
    m->text          = strdup(text);
    m->is_user       = is_user;
    m->reasoning     = (reasoning && *reasoning) ? strdup(reasoning) : NULL;
    m->tool_calls    = NULL;
    m->n_tool_calls  = 0;
    m->tool_call_id  = NULL;
    cb->count++;
}

void chat_add_tool_calls(ChatBuffer *cb, const char *content,
                         const char *reasoning,
                         ToolCall *tc, int n_tc) {
    chat_ensure_capacity(cb);
    Message *m       = &cb->msgs[cb->count];
    m->text          = (content && *content) ? strdup(content) : NULL;
    m->is_user       = false;
    m->reasoning     = (reasoning && *reasoning) ? strdup(reasoning) : NULL;
    m->tool_calls    = tc;
    m->n_tool_calls  = n_tc;
    m->tool_call_id  = NULL;
    cb->count++;
}

void chat_add_tool_result(ChatBuffer *cb, const char *tool_call_id,
                          const char *result) {
    chat_ensure_capacity(cb);
    Message *m       = &cb->msgs[cb->count];
    m->text          = strdup(result ? result : "");
    m->is_user       = false;
    m->reasoning     = NULL;
    m->tool_calls    = NULL;
    m->n_tool_calls  = 0;
    m->tool_call_id  = strdup(tool_call_id ? tool_call_id : "");
    cb->count++;
}

void chat_scroll(ChatBuffer *cb, int delta) {
    cb->scroll += delta;
    if (cb->scroll < 0) cb->scroll = 0;
    if (cb->scroll > cb->count) cb->scroll = cb->count;
}

/* -----------------------------------------------------------------------
 * UTF-8 aware rendering helpers (implementations in utf8.c)
 * --------------------------------------------------------------------- */
static int render_wrapped(WINDOW *win, int y, int x_start, int indent,
                           int width, const char *text) {
    int max_y    = getmaxy(win);
    int rows     = 0;
    const char *p = text;
    int x        = x_start;
    char seg[4096];

    while (*p && y < max_y) {
        const char *next;
        const char *end = wrap_line_end(p, x, width, &next);

        /* Copy segment to null-terminated buffer */
        int len = (int)(end - p);
        if (len >= (int)sizeof(seg)) len = (int)sizeof(seg) - 1;
        /* Ensure we don't end on a continuation byte */
        while (len > 0 && (((unsigned char)p[len]) & 0xC0) == 0x80) len--;
        memcpy(seg, p, len);
        seg[len] = '\0';

        if (len > 0) {
            wmove(win, y, x);
            waddstr(win, seg);
        }

        p = next;
        y++;
        rows++;
        x = indent;  /* continuation lines use indent */
    }

    return rows > 0 ? rows : 1;
}

/* -----------------------------------------------------------------------
 * Public render functions
 * --------------------------------------------------------------------- */

void chat_render(ChatBuffer *cb, WINDOW *win) {
    werase(win);
    int h = getmaxy(win);
    int w = getmaxx(win);
    int y = h - 1;
    int start = cb->count - cb->scroll;

    for (int i = start - 1; i >= 0 && y >= 0; i--) {
        if (cb->msgs[i].tool_call_id) continue;
        if (cb->msgs[i].n_tool_calls > 0 && !cb->msgs[i].text) continue;

        const char *prefix  = cb->msgs[i].is_user ? "You: " : "Assistant: ";
        const char *text    = cb->msgs[i].text ? cb->msgs[i].text : "";
        int prefix_cols     = (int)strlen(prefix);  /* ASCII prefix, cols == bytes */

        /* How many rows does this message occupy? */
        int rows = count_rows(text, prefix_cols, prefix_cols, w);

        y -= rows;
        if (y < -1) break;
        int render_y = (y < 0) ? 0 : y;

        /* Render prefix on first row */
        if (render_y < h) {
            wattron(win, cb->msgs[i].is_user ? A_BOLD : A_DIM);
            wmove(win, render_y, 0);
            waddstr(win, prefix);
            wattroff(win, A_BOLD | A_DIM);
        }

        /* Render text (first line continues after prefix; continuation indented) */
        render_wrapped(win, render_y, prefix_cols, prefix_cols, w, text);

        if (y < 0) break;
    }
    wrefresh(win);
}

void chat_render_decisions(ChatBuffer *cb, WINDOW *win) {
    werase(win);
    int w = getmaxx(win);
    int y = 0;
    int h = getmaxy(win);

    for (int i = 0; i < cb->count && y < h; i++) {
        if (cb->msgs[i].is_user || !cb->msgs[i].reasoning) continue;

        char hdr[32];
        int msg_num = 0;
        for (int j = 0; j <= i; j++) if (!cb->msgs[j].is_user) msg_num++;
        snprintf(hdr, sizeof(hdr), "── #%d ──", msg_num);
        wattron(win, A_BOLD);
        wmove(win, y++, 0); waddnstr(win, hdr, w);
        wattroff(win, A_BOLD);

        y += render_wrapped(win, y, 0, 0, w, cb->msgs[i].reasoning);
        if (y < h) y++;
    }

    if (y == 0) {
        wattron(win, A_DIM);
        mvwaddstr(win, 0, 0, "No reasoning yet");
        wattroff(win, A_DIM);
    }
    wrefresh(win);
}

void chat_remove_last(ChatBuffer *cb) {
    if (cb->count > 0) {
        Message *m = &cb->msgs[cb->count - 1];
        free(m->text);
        free(m->reasoning);
        message_clear_extensions(m);
        cb->count--;
    }
}

void chat_free(ChatBuffer *cb) {
    for (int i = 0; i < cb->count; i++) {
        free(cb->msgs[i].text);
        free(cb->msgs[i].reasoning);
        message_clear_extensions(&cb->msgs[i]);
    }
    free(cb->msgs);
}
