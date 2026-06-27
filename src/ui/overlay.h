#ifndef OVERLAY_H
#define OVERLAY_H

/*
 * Generic blocking list-picker overlay.
 *
 * Displays a centered popup with a scrollable list of options.
 * Blocks until the user selects an item or cancels.
 *
 * Returns the selected index (0-based), or -1 on Escape/cancel.
 * current: index of the pre-highlighted item (-1 = none).
 */
int overlay_pick(const char *title,
                 const char * const *options,
                 int count,
                 int current);

#endif /* OVERLAY_H */
