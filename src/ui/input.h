#ifndef INPUT_H
#define INPUT_H

#include <ncurses.h>
#include <stdbool.h>

#define INPUT_MAX 1024

typedef struct {
    char buffer[INPUT_MAX];
    int len;
    int pos;
    char history[32][INPUT_MAX];
    int hist_count;
    int hist_pos;
} InputLine;

void input_init(InputLine *in);
void input_handle_key(InputLine *in, int ch, bool *submitted);
void input_render(InputLine *in, WINDOW *win);
void input_reset(InputLine *in);

#endif
