/*
 * minix2.c -- what Minix 2 lacks: realpath, mkstemp, a read() with a
 * timeout and the job control calls (as no-ops) in libc; in its curses, newterm, waddnstr, hline, key codes, a
 * getch that does not block, and the shell and program modes.
 * Built only there, by hand: see docs/build/minix.md.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <curses.h>
#include "port.h"
#define GORK_MINIX2_C
#include "minix2.h"

/* realpath: the path must exist.  There are no symbolic links, so a
   directory resolves through chdir and getcwd, and anything else through
   its directory. */
char *realpath(const char *path, char *out)
{
    char        here[PATH_MAX], dir[PATH_MAX];
    const char *base;
    char       *slash;
    struct stat st;
    int         ok;

    if (strlen(path) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    if (stat(path, &st) != 0 || getcwd(here, sizeof here) == NULL)
        return NULL;
    strcpy(dir, path);
    base = NULL;
    if (!S_ISDIR(st.st_mode)) {
        slash = strrchr(dir, '/');
        if (slash == NULL) {
            base = path;
            strcpy(dir, ".");
        } else {
            base = path + (slash - dir) + 1;
            slash[slash == dir ? 1 : 0] = '\0';
        }
    }
    if (chdir(dir) != 0)
        return NULL;
    ok = getcwd(out, PATH_MAX) != NULL;
    chdir(here);
    if (!ok)
        return NULL;
    if (base != NULL) {
        if (strlen(out) + 1 + strlen(base) >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (strcmp(out, "/") != 0)
            strcat(out, "/");
        strcat(out, base);
    }
    return out;
}

/* The trailing XXXXXX become the process id and a counter. */
int mkstemp(char *tmpl)
{
    static unsigned n;
    size_t          len = strlen(tmpl);
    char            tail[16];
    int             fd, tries;

    if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    for (tries = 0; tries < 100; tries++) {
        sprintf(tail, "%03u%03u", (unsigned) getpid() % 1000, n++ % 1000);
        memcpy(tmpl + len - 6, tail, 6);
        fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    return -1;
}

static void wake(int sig)
{
    (void) sig;
}

/* read(), given up after `secs` seconds: -1 with errno EINTR. */
int read_timed(int fd, char *p, unsigned n, long secs)
{
    struct sigaction sa, old;
    int              r, e;

    sa.sa_handler = wake;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, &old);
    alarm(secs > 0 ? (unsigned) secs : 1);
    r = read(fd, p, n);
    e = errno;
    alarm(0);
    sigaction(SIGALRM, &old, (struct sigaction *) NULL);
    errno = e;
    return r;
}

/* No job control: setpgid(0, 0), in the child, starts a session instead,
   which makes it a process group; the parent's own call has nothing to
   do.  No terminal has a foreground group to hand over. */
int setpgid(pid_t pid, pid_t pgid)
{
    (void) pgid;
    return pid == 0 && setsid() < 0 ? -1 : 0;
}

int killpg(int pgrp, int sig)
{
    return kill(-pgrp, sig);
}

pid_t tcgetpgrp(int fd)
{
    (void) fd;
    errno = ENOTTY;
    return -1;
}

int tcsetpgrp(int fd, pid_t pgrp)
{
    (void) fd;
    (void) pgrp;
    errno = ENOTTY;
    return -1;
}

/* ---------------------------------------------------------------- */
/* curses                                                           */
/* ---------------------------------------------------------------- */

/* Minix curses writes to stdout, which the TUI points at its capture
   file: for each update stdout's descriptor becomes the terminal's. */
static int term_fd = 1, no_delay;

WINDOW *m2_newterm(char *type, FILE *out, FILE *in)
{
    (void) type;                /* initscr reads $TERM itself */
    (void) in;
    term_fd = fileno(out);
    return initscr();
}

int m2_refresh(void)
{
    int keep;

    fflush(stdout);
    keep = stdout->_fd;
    stdout->_fd = term_fd;
    wrefresh(stdscr);
    fflush(stdout);
    stdout->_fd = keep;
    return OK;
}

int m2_endwin(void)
{
    int keep;

    fflush(stdout);
    keep = stdout->_fd;
    stdout->_fd = term_fd;
    endwin();
    /* endwin sets the old modes on fd 1, which is the capture file */
    tcsetattr(0, TCSANOW, &_orig_tty);
    putchar('\n');              /* its refresh put the cursor back on the
                                   input line: the shell's prompt goes below */
    fflush(stdout);
    stdout->_fd = keep;
    return OK;
}

/* Up to n characters of s (all of it if n < 0). */
int waddnstr(WINDOW *w, char *s, int n)
{
    while ((n < 0 || n-- > 0) && *s != '\0')
        waddch(w, (unsigned char) *s++);
    return OK;
}

/* n copies of ch from the cursor on, the cursor staying where it is. */
int hline(int ch, int n)
{
    int y, x;

    getyx(stdscr, y, x);
    while (n-- > 0 && waddch(stdscr, ch) != ERR)
        ;
    wmove(stdscr, y, x);
    return OK;
}

void m2_nodelay(int on)
{
    no_delay = on;
}

void flushinp(void)
{
    tcflush(0, TCIFLUSH);
}

void reset_shell_mode(void)
{
    tcsetattr(0, TCSANOW, &_orig_tty);
}

void reset_prog_mode(void)
{
    tcsetattr(0, TCSANOW, &_tty);
}

/* One byte from the keyboard, or -1 if none comes: wait for one if `min`
   is 1, else for `tenths` tenths of a second. */
static int key_byte(int min, int tenths)
{
    struct termios t, keep;
    unsigned char  c;
    int            n;

    tcgetattr(0, &keep);
    t = keep;
    t.c_cc[VMIN] = min;
    t.c_cc[VTIME] = tenths;
    tcsetattr(0, TCSANOW, &t);
    n = read(0, (char *) &c, 1);
    tcsetattr(0, TCSANOW, &keep);
    return n == 1 ? c : -1;
}

/* A key, with the escape sequences of the Minix console (ESC [ V is
   PgUp) and of a VT100 or xterm (ESC [ 5 ~) turned into KEY_ codes.  A
   lone Esc is 27. */
int m2_getch(void)
{
    int c, n = 0;

    c = key_byte(no_delay ? 0 : 1, 0);
    if (c != 27)
        return c < 0 ? ERR : c;
    c = key_byte(0, 1);                 /* the rest of a key comes at once */
    if (c != '[' && c != 'O')
        return 27;
    c = key_byte(0, 1);
    while (c >= '0' && c <= '9') {
        n = n * 10 + c - '0';
        c = key_byte(0, 1);
    }
    switch (c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'Y': case 'F': return KEY_END;
    case 'V': return KEY_PPAGE;
    case 'U': return KEY_NPAGE;
    case '~':
        switch (n) {
        case 1: case 7: return KEY_HOME;
        case 4: case 8: return KEY_END;
        case 3: return KEY_DC;
        case 5: return KEY_PPAGE;
        case 6: return KEY_NPAGE;
        }
    }
    return ERR;
}
