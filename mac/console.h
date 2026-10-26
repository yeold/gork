/*
 * console.h -- gork's console window on the classic Mac OS (console.c).
 */
#ifndef GORK_MAC_CONSOLE_H
#define GORK_MAC_CONSOLE_H

int  con_init(void);                    /* the Toolbox, and the window */
void con_write(const char *p, unsigned long n);
int  con_readline(char *buf, unsigned long size);  /* its length, or -1
                                                       when the user quits */
void con_idle(void);                    /* the window's own events */

#endif
