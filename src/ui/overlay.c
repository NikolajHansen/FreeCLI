#include "overlay.h"
#include <ncurses.h>
#include <string.h>

#define OVL_MAX_W 52
#define OVL_MIN_H  6

int overlay_pick(const char *title,
                 const char * const *options,
                 int count,
                 int current) {
    if (count <= 0) return -1;
    if (current < 0 || current >= count) current = 0;

    int screen_h = LINES;
    int screen_w = COLS;

    /* Size the popup */
    int inner_w = 0;
    for (int i = 0; i < count; i++) {
        int l = (int)strlen(options[i]);
        if (l > inner_w) inner_w = l;
    }
    int title_len = title ? (int)strlen(title) : 0;
    if (title_len > inner_w) inner_w = title_len;
    if (inner_w > OVL_MAX_W - 4) inner_w = OVL_MAX_W - 4;

    int box_w  = inner_w + 4;             /* 2 border + 2 padding */
    int vis    = count < (screen_h - 8) ? count : (screen_h - 8);
    if (vis < 1) vis = 1;
    int box_h  = vis + 5;                 /* border + title + sep + hint */
    if (box_h < OVL_MIN_H) box_h = OVL_MIN_H;

    int y0 = (screen_h - box_h) / 2;
    int x0 = (screen_w - box_w) / 2;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;

    WINDOW *win = newwin(box_h, box_w, y0, x0);
    if (!win) return -1;
    keypad(win, TRUE);

    int sel    = current;
    int scroll = 0;   /* first visible item index */

    for (;;) {
        /* Keep sel in view */
        if (sel < scroll)           scroll = sel;
        if (sel >= scroll + vis)    scroll = sel - vis + 1;

        werase(win);
        box(win, 0, 0);

        /* Title */
        if (title) {
            wattron(win, A_BOLD);
            int tx = (box_w - (int)strlen(title)) / 2;
            if (tx < 1) tx = 1;
            mvwaddnstr(win, 1, tx, title, inner_w);
            wattroff(win, A_BOLD);
        }
        wmove(win, 2, 1); whline(win, ACS_HLINE, box_w - 2);

        /* Options */
        for (int i = 0; i < vis; i++) {
            int idx = scroll + i;
            if (idx >= count) break;
            int row = 3 + i;
            if (idx == sel) wattron(win, A_REVERSE);
            /* Marker for currently-active item */
            char prefix = (idx == current) ? '*' : ' ';
            mvwprintw(win, row, 2, "%c %-*.*s",
                      prefix, inner_w, inner_w, options[idx]);
            if (idx == sel) wattroff(win, A_REVERSE);
        }

        /* Scroll hints */
        if (scroll > 0)
            mvwaddch(win, 3, box_w - 2, ACS_UARROW);
        if (scroll + vis < count)
            mvwaddch(win, 3 + vis - 1, box_w - 2, ACS_DARROW);

        /* Footer hint */
        wmove(win, box_h - 2, 1); whline(win, ACS_HLINE, box_w - 2);
        wattron(win, A_DIM);
        mvwaddstr(win, box_h - 1, 2, "[↑↓] Navigate  [Enter] Select  [Esc] Cancel");
        wattroff(win, A_DIM);

        wrefresh(win);

        int ch = wgetch(win);
        switch (ch) {
        case KEY_UP:
            if (sel > 0) sel--;
            break;
        case KEY_DOWN:
            if (sel < count - 1) sel++;
            break;
        case KEY_PPAGE:
            sel -= vis;
            if (sel < 0) sel = 0;
            break;
        case KEY_NPAGE:
            sel += vis;
            if (sel >= count) sel = count - 1;
            break;
        case '\n': case KEY_ENTER:
            delwin(win);
            return sel;
        case 27:   /* Escape */
        case 'q':
            delwin(win);
            return -1;
        }
    }
}
