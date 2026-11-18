/*
 * notui.c -- tui.h for a system without curses (Plan 9's APE): batch mode
 * only.  tui_init fails, so every other call stays a no-op, as tui.h
 * promises for batch mode.  Built in place of tui.c, by hand: see
 * docs/build/plan9.md.
 */
#include <stddef.h>
#include "port.h"
#include "tui.h"
#include "acp.h"

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

/* ACP stubs for GORK_NO_FORK builds (GRUB, barebox, UEFI, Mac): these
   targets have no fork, so ACP is not available, but main.c references
   the functions inside #ifndef GORK_NO_FORK guards.  The guards should
   work, but on some cross-compilers they don't; provide no-op stubs
   here to satisfy the linker. */
#ifdef GORK_NO_FORK

int  acp_serve(const struct acp_hooks *h) { (void) h; return -1; }
void acp_text(const char *t) { (void) t; }
void acp_tool_start(const char *a, const char *b, const char *c,
                    const char *d) { (void) a; (void) b; (void) c; (void) d; }
void acp_tool_done(const char *a, int b) { (void) a; (void) b; }
void acp_usage(void) { }
int  acp_ask(const char *a, const char *b) { (void) a; (void) b; return 0; }
int  acp_cancelled(void) { return 0; }
const char *acp_kind(const char *name) { (void) name; return "other"; }

#endif /* GORK_NO_FORK */
