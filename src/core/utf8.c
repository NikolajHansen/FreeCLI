#include "utf8.h"
#include <stddef.h>

int utf8_clen(const char *s) {
    unsigned char c = (unsigned char)*s;
    if (!c)       return 1;
    if (c < 0x80) return 1;
    if (c < 0xC0) return 1;  /* stray continuation — treat as 1 */
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    return 4;
}

int utf8_cwidth(const char *s) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;
    if (c < 0xE0) return 1;   /* 2-byte: Latin ext, Cyrillic, Greek… */
    if (c < 0xF0) {
        unsigned int cp = ((c & 0x0F) << 12)
                        | (((unsigned char)s[1] & 0x3F) << 6)
                        | ((unsigned char)s[2] & 0x3F);
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

const char *wrap_line_end(const char *text, int x_start, int width,
                           const char **next_p) {
    int avail = width - x_start;
    if (avail < 1) avail = 1;

    const char *p          = text;
    const char *last_space = NULL;
    const char *last_end   = NULL;
    int col = 0;

    while (*p && *p != '\n') {
        int clen = utf8_clen(p);
        int cw   = utf8_cwidth(p);

        if (col + cw > avail) {
            if (last_space) {
                *next_p = last_space;
                return last_end;
            }
            *next_p = p;
            return p;
        }

        if (*p == ' ' || *p == '\t') {
            if (col > 0) {
                last_end   = p;
                last_space = p + clen;
                while (*last_space == ' ' || *last_space == '\t') last_space++;
            }
        }

        col += cw;
        p += clen;
    }

    *next_p = (*p == '\n') ? p + 1 : p;
    return p;
}

int count_rows(const char *text, int x_start, int indent, int width) {
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
