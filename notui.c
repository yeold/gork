/*
 * notui.c -- tui.h for a system without curses (Plan 9's APE): batch mode
 * only.  tui_init fails, so every other call stays a no-op, as tui.h
 * promises for batch mode.  Built in place of tui.c, by hand: see
 * docs/BUILD.md.
 */
#include <stddef.h>
#include "tui.h"

int   tui_init(const char *status, const char *title)
{
    (void) status;
    (void) title;
    return -1;
}
void  tui_end(void) {}
char *tui_read_line(void) { return NULL; }
void  tui_drain(void) {}
int   tui_confirm(const char *title, const char *detail)
{
    (void) title;
    (void) detail;
    return 'n';
}
void  tui_suspend(void) {}
void  tui_resume(void) {}
int   tui_poll_abort(void) { return 0; }
