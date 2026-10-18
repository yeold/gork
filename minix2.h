/*
 * minix2.h -- what Minix 2's curses lacks, for tui.c, after <curses.h>.
 * The functions are in minix2.c.
 */
#ifndef GORK_MINIX2_H
#define GORK_MINIX2_H

#define A_NORMAL      0
#define KEY_DOWN      0x102
#define KEY_UP        0x103
#define KEY_LEFT      0x104
#define KEY_RIGHT     0x105
#define KEY_HOME      0x106
#define KEY_BACKSPACE 0x107
#define KEY_DC        0x14A
#define KEY_NPAGE     0x152
#define KEY_PPAGE     0x153
#define KEY_ENTER     0x157
#define KEY_END       0x168

WINDOW *m2_newterm(char *type, FILE *out, FILE *in);
int     m2_refresh(void);
int     m2_endwin(void);
int     m2_getch(void);
void    m2_nodelay(int on);
int     waddnstr(WINDOW *w, char *s, int n);
int     hline(int ch, int n);
void    flushinp(void);
void    reset_shell_mode(void);
void    reset_prog_mode(void);

#ifndef GORK_MINIX2_C           /* minix2.c calls the real ones */
#undef  refresh
#undef  getch
#define newterm(t, o, i) m2_newterm(t, o, i)
#define refresh()        m2_refresh()
#define endwin()         m2_endwin()
#define getch()          m2_getch()
#define nodelay(w, on)   m2_nodelay(on)
#endif

#endif /* GORK_MINIX2_H */
