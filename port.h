/*
 * port.h -- what differs on the systems without fork: DOS (DJGPP) and
 * Windows (MinGW).  The Windows functions are in win.c.
 */

#ifndef GORK_PORT_H
#define GORK_PORT_H

/* One program at a time: run_command waits for the command with no timeout
   and no interrupt, there are no MCP servers, the request timeout is polled
   instead of a SIGALRM, and an open file cannot be unlinked. */
/* The 16-bit DOS build (Open Watcom, large model, real mode): DOS as the
   DJGPP build is, with what 640K and a 16-bit int change.  No buffer can
   pass 64K, so everything that grows is capped: see GORK_DOS16 in main.c. */
#if defined(__WATCOMC__) && defined(__I86__)
#define GORK_DOS16
#ifndef __MSDOS__
#define __MSDOS__
#endif
#endif

#if defined(__MSDOS__) || defined(_WIN32)
#define GORK_NO_FORK
#endif

/* The bare bones, for the DOS builds: replies come whole (no streaming)
   and Enter always sends (no input of several lines). */
#ifdef __MSDOS__
#define GORK_BARE
#endif

#ifdef GORK_DOS16
/* realpath(): absolute, lower case, with forward slashes, as DJGPP's. */
char *dos16_realpath(const char *path, char *out);
#define realpath dos16_realpath
#endif

#ifdef __MSDOS__
/* SPX through the IPX driver (dosspx.c): one connection at a time.
   dosspx_open(): 0, -1 with a message, or -2 if `stop` returned nonzero.
   dosspx_read(): bytes read, 0 at the end, -1 on error, -3 if none yet. */
int  dosspx_open(const unsigned char *net, const unsigned char *node,
                 unsigned socket, int (*stop)(void));
int  dosspx_write(const char *p, size_t n);
int  dosspx_read(char *buf, size_t n);
void dosspx_close(void);
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
