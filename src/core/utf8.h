#ifndef UTF8_H
#define UTF8_H

/* Byte length of one UTF-8 character starting at *s. */
int utf8_clen(const char *s);

/* Approximate display column width of the UTF-8 character at *s. */
int utf8_cwidth(const char *s);

/*
 * Find the end byte of a word-wrapped line starting at *text,
 * given that the first line starts at column x_start and the window
 * is width columns wide.
 *
 * Returns pointer to first byte NOT included in this line.
 * *next_p is set to the first byte of the NEXT line.
 */
const char *wrap_line_end(const char *text, int x_start, int width,
                           const char **next_p);

/*
 * Count display rows for text without rendering.
 * Mirrors wrap_line_end logic for scroll calculations.
 */
int count_rows(const char *text, int x_start, int indent, int width);

#endif /* UTF8_H */
