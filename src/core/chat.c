#include "chat.h"
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
 * UTF-8 aware rendering helpers
 * --------------------------------------------------------------------- */

/* Byte length of one UTF-8 character starting at *s. */
static int utf8_clen(const char *s) {
    unsigned char c = (unsigned char)*s;
    if (!c)       return 1;
    if (c < 0x80) return 1;
    if (c < 0xC0) return 1;  /* stray continuation — treat as 1 */
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    return 4;
}

/* Approximate display column width of the UTF-8 character at *s. */
static int utf8_cwidth(const char *s) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return (c == '\t') ? 1 : 1;  /* control chars rare here */
    if (c < 0xE0) return 1;   /* 2-byte: Latin ext, Cyrillic, Greek… */
    if (c < 0xF0) {
        unsigned int cp = ((c & 0x0F) << 12)
                        | (((unsigned char)s[1] & 0x3F) << 6)
                        | ((unsigned char)s[2] & 0x3F);
        /* Double-width CJK blocks (simplified) */
        if ((cp >= 0x1100 && cp <= 0x115F) ||
            (cp >= 0x2E80 && cp <= 0x303E) ||
            (cp >= 0x3040 && cp <= 0xA4CF) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) ||
            (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFF01 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6))
            return 2;
        return 1;
    }
    return 2;  /* 4-byte: emoji etc. */
}

/*
 * Find the end byte of a word-wrapped line starting at *text,
 * given that the first line starts at column x_start and the window
 * is width columns wide. indent is used for all continuation lines too.
 *
 * Returns pointer to first byte NOT included in this line.
 * *next_p is set to the first byte of the NEXT line (skips the break space/newline).
 */
static const char *wrap_line_end(const char *text, int x_start, int width,
                                  const char **next_p) {
    int avail = width - x_start;
    if (avail < 1) avail = 1;

    const char *p          = text;
    const char *last_space = NULL;   /* byte right after last space run */
    const char *last_end   = NULL;   /* byte of the space character */
    int col = 0;

    while (*p && *p != '\n') {
        int clen = utf8_clen(p);
        int cw   = utf8_cwidth(p);

        if (col + cw > avail) {
            /* Overflow — break here */
            if (last_space) {
                *next_p = last_space;
                return last_end;
            }
            /* No space found — hard break (never splits a multibyte seq) */
            *next_p = p;
            return p;
        }

        if (*p == ' ' || *p == '\t') {
            /* Record the space position; the visual line ends before it */
            if (col > 0) {   /* ignore leading spaces on first line */
                last_end   = p;
                last_space = p + clen;  /* next word starts here */
                while (*last_space == ' ' || *last_space == '\t') last_space++;
            }
        }

        col += cw;
        p += clen;
    }

    /* Reached end-of-string or newline naturally */
    *next_p = (*p == '\n') ? p + 1 : p;
    return p;
}

/*
 * Render text into win starting at row y, first-line indent x_start.
 * Continuation lines are indented by `indent` columns.
 * Handles embedded \n. Never splits UTF-8.
 * Returns number of rows used.
 */
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

/*
 * Count display rows for text without rendering.
 * Must use identical logic to render_wrapped.
 */
static int count_rows(const char *text, int x_start, int indent, int width) {
    int rows     = 0;
    const char *p = text;
    int x        = x_start;

    while (*p) {
        const char *next;
        wrap_line_end(p, x, width, &next);
        p = next;
        rows++;
        x = indent;
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
