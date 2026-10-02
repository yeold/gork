/*
 * tui.h -- interactive front end: chat pane, input line, approvals.
 *
 * Every call is a no-op (tui_confirm answers 'n') until tui_init succeeds,
 * so the agent loop can call them unconditionally in batch mode too.
 */

#ifndef GORK_TUI_H
#define GORK_TUI_H

/* Take over the terminal.  `status` is shown on the top bar and must stay
   valid until tui_end.  Returns 0 on success. */
int   tui_init(const char *status);
void  tui_end(void);

/* Next line the user typed, or NULL when they quit.  Static storage. */
char *tui_read_line(void);

/* Move whatever has been printed to stdout/stderr into the chat pane. */
void  tui_drain(void);

/* Ask before running a tool: returns 'y', 'n', 'a' (always) or 27 (Esc:
   decline and abort the task). */
int   tui_confirm(const char *title, const char *detail);

/* Around a shell command: give the terminal normal line mode so the
   command can prompt on /dev/tty, then take it back and repaint. */
void  tui_suspend(void);
void  tui_resume(void);

/* While a task runs: show new output, handle scrolling, and return 1 if the
   user pressed Esc.  Never blocks. */
int   tui_poll_abort(void);

#endif /* GORK_TUI_H */
