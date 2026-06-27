#include "input.h"
#include <string.h>
#include <ctype.h>

void input_init(InputLine *in) {
    memset(in->buffer, 0, INPUT_MAX);
    in->len = in->pos = in->hist_count = in->hist_pos = 0;
}

static void save_history(InputLine *in) {
    if (in->len == 0) return;
    if (in->hist_count < 32) {
        strcpy(in->history[in->hist_count++], in->buffer);
    }
    in->hist_pos = in->hist_count;
}

void input_handle_key(InputLine *in, int ch, bool *submitted) {
    *submitted = false;
    if (ch == '\n' || ch == KEY_ENTER) {
        if (in->len > 0) {
            save_history(in);
            *submitted = true;
        }
        return;
    }
    if (ch == KEY_BACKSPACE || ch == 127) {
        if (in->pos > 0) {
            memmove(&in->buffer[in->pos-1], &in->buffer[in->pos], in->len - in->pos + 1);
            in->pos--; in->len--;
        }
        return;
    }
    if (ch == KEY_LEFT)  { if (in->pos > 0) in->pos--; return; }
    if (ch == KEY_RIGHT) { if (in->pos < in->len) in->pos++; return; }
    if (ch == KEY_HOME)  { in->pos = 0; return; }
    if (ch == KEY_END)   { in->pos = in->len; return; }
    if (ch == KEY_UP) {
        if (in->hist_pos > 0) {
            in->hist_pos--;
            strcpy(in->buffer, in->history[in->hist_pos]);
            in->len = in->pos = strlen(in->buffer);
        }
        return;
    }
    if (ch == KEY_DOWN) {
        if (in->hist_pos < in->hist_count) {
            in->hist_pos++;
            if (in->hist_pos == in->hist_count) {
                in->buffer[0] = 0; in->len = in->pos = 0;
            } else {
                strcpy(in->buffer, in->history[in->hist_pos]);
                in->len = in->pos = strlen(in->buffer);
            }
        }
        return;
    }
    if (isprint(ch) && in->len < INPUT_MAX-1) {
        memmove(&in->buffer[in->pos+1], &in->buffer[in->pos], in->len - in->pos + 1);
        in->buffer[in->pos++] = ch;
        in->len++;
    }
}

void input_render(InputLine *in, WINDOW *win) {
    werase(win);
    mvwaddnstr(win, 0, 0, in->buffer, in->len);
    wmove(win, 0, in->pos);
    wrefresh(win);
}

void input_reset(InputLine *in) {
    in->buffer[0] = 0;
    in->len = in->pos = 0;
}
