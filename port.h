/*
 * port.h -- what differs on the systems without fork: DOS (DJGPP) and
 * Windows (MinGW).  The Windows functions are in win.c.
 */

#ifndef GORK_PORT_H
#define GORK_PORT_H

/* One program at a time: run_command waits for the command with no timeout
   and no interrupt, there are no MCP servers, the request timeout is polled
   instead of a SIGALRM, and an open file cannot be unlinked. */
#if defined(__MSDOS__) || defined(_WIN32)
#define GORK_NO_FORK
#endif

#ifdef _WIN32
/* realpath(): absolute, with forward slashes, ENOENT if nothing is there.
   ponytail: junctions and 8.3 short names are not resolved, so a junction
   inside the working directory leads out of it. */
char *win_realpath(const char *path, char *out);
/* tmpnam() in the user's temporary directory; msvcrt's names a file in the
   root directory, which is often read-only.  `out` holds TMPNAM_MAX. */
char *win_tmpnam(char *out);
void  win_sleep(int secs);
/* The directory for gork.cnf and _gork: %USERPROFILE%, or where gork.exe
   is (Windows 9x sets no USERPROFILE).  0, or -1 if `out` is too small. */
int   win_home(char *out, size_t size);
/* Give curses a console handle of its own, before it starts: it keeps the
   standard output handle, and that one is closed when the TUI points fd 1
   at its capture file. */
void  win_own_console(void);

/* The API speaks UTF-8, a Windows console its code page.
   win_utf8_to_console(): rewrite s[0..len) in place for display in the
   console's code page (best fit, `?` where there is none; bytes that are
   not UTF-8 stay) and return the new length, never more than len.  `cells`
   is nonzero for curses, which writes one byte per cell, so that a console
   set to UTF-8 gets plain ASCII; printf to such a console needs nothing.
   win_input_to_utf8(): text the user typed at the console (`typed`) or
   gave on the command line, as UTF-8.  win_output_to_utf8(): a command's
   output, taken to be in the OEM code page unless it is valid UTF-8.
   Both return malloc'd text, or NULL if `s` can stay as it is. */
size_t win_utf8_to_console(char *s, size_t len, int cells);
char  *win_input_to_utf8(const char *s, int typed);
char  *win_output_to_utf8(const char *s);

#define TMPNAM_MAX     260
#define tmpnam         win_tmpnam
#define realpath       win_realpath
#define sleep(n)       win_sleep(n)
#define fsync          _commit
#define lstat          stat             /* no symbolic links to tell apart */
#define S_ISLNK(m)     0
#define mkdir(p, mode) mkdir(p)
#else
#define TMPNAM_MAX     L_tmpnam
#endif

#endif /* GORK_PORT_H */
