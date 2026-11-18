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

/* ACP stubs for real firmware builds (GRUB, barebox, UEFI, Mac):
   These targets compile notui.c but NOT acp.c.
   The GORK_BARE test configuration compiles both, so exclude it. */

#if defined(GORK_GRUB) || defined(GORK_BAREBOX) || defined(GORK_EFI) || defined(GORK_MAC)

int  acp_serve(const struct acp_hooks *h) { (void) h; return -1; }
void acp_text(const char *t) { (void) t; }
void acp_tool_start(const char *a, const char *b, const char *c,
                    const char *d) { (void) a; (void) b; (void) c; (void) d; }
void acp_tool_done(const char *a, int b) { (void) a; (void) b; }
void acp_usage(void) { }
int  acp_ask(const char *a, const char *b) { (void) a; (void) b; return 0; }
int  acp_cancelled(void) { return 0; }
const char *acp_kind(const char *name) { (void) name; return "other"; }

/* MCP stubs for the same firmware builds: they compile notui.c but not mcp.c. */

void  mcp_start(const char *path) { (void) path; }
void  mcp_stop(void) { }
int   mcp_tool_count(void) { return 0; }
int   mcp_add_tools(void *tools) { (void) tools; return 0; }
int   mcp_find(const char *name) { (void) name; return -1; }
int  *mcp_always(int i) { (void) i; return NULL; }
char *mcp_call(int i, void *input, int *is_err) { (void) i; (void) input; (void) is_err; return NULL; }

#endif
