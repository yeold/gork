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

/* UEFI (gork.efi, started from the UEFI shell): the build passes
   -DGORK_EFI, and its C library is uefi/libc.c.  One program at a time,
   as on DOS, and no curses: notui.c takes tui.c's place, so only batch
   mode works.  Paths are the shell's, fs0:\dir\file, which realpath()
   gives back as fs0:/dir/file; names do not tell upper case from lower. */
#ifdef GORK_EFI
#define GORK_NO_FORK
#define GORK_NO_TUI
#endif

/* barebox (the `gork` command, built into a barebox image): the build
   passes -DGORK_BAREBOX, its C library is barebox/libc.c with fwlibc.c,
   and the network gork's own TCP/IP (ip.c).  As on UEFI, one program at a
   time and batch mode only.  Paths are barebox's, /dir/file. */
#ifdef GORK_BAREBOX
#define GORK_NO_FORK
#define GORK_NO_TUI
#endif

#if defined(__MSDOS__) || defined(_WIN32)
#define GORK_NO_FORK
#endif

/* The bare bones, for the DOS builds: replies come whole (no streaming)
   and Enter always sends (no input of several lines). */
#ifdef __MSDOS__
#define GORK_BARE
#endif

/* AIX PS/2 1.3 (MetaWare High C): no ssize_t, strerror is not declared,
   libc has no realpath, memmove or strtoul and curses no waddnstr or
   hline; aixps2.c supplies them. */
#if defined(_AIX) && defined(_I386)
#define GORK_AIXPS2
typedef int ssize_t;
char *strerror(int errnum);
char *realpath(const char *path, char *out);
#endif

/* Minix 2 (the ACK compiler; Minix 3 is clang and needs none of this): no
   select, no symbolic links, no job control and no BSD sockets.  Without
   select a reply cannot be interrupted with Esc, and a pipe is read with
   read_timed().  A command gets a session of its own with setsid(), which
   makes it a process group that killpg() reaches.  minix2.c has the rest,
   and minix2.h what its curses lacks. */
#if defined(__minix) && defined(__ACK__)
#define GORK_MINIX2
#define GORK_NO_SELECT
#include <signal.h>
char *realpath(const char *path, char *out);
int   mkstemp(char *tmpl);
int   read_timed(int fd, char *p, unsigned n, long secs);
int   killpg(int pgrp, int sig);
#define lstat      stat
#define S_ISLNK(m) 0
#endif

/* OPENSTEP 4.2 (NeXT Mach, cc -posix): POSIX headers over a 4.3BSD
   library; nextstep.c has the calls it lacks, and fixes readdir and
   realpath.  Its curses (ncurses on the
   old sgtty interface) is no place for termios, so, as on OS/2, Esc does
   not interrupt a command. */
#ifdef __NeXT__
#define GORK_NEXT
#define readdir  next_readdir
#define realpath next_realpath
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & 0170000) == 0120000)
#endif
#endif

/* SunOS 4.1 (gcc; Solaris is SunOS 5 and defines __SVR4): libc has no
   memmove, raise, strerror or strtoul; sunos4.c supplies them.  Its
   headers have no ssize_t and leave out what returns a pointer.  Its
   realloc(NULL, n) returns NULL instead of acting as malloc, and its
   getpgrp is the BSD one, which takes a pid (0 for this process). */
#if defined(__sun__) && !defined(__SVR4) && !defined(__svr4__)
#define GORK_SUNOS4
#include <sys/types.h>
typedef int ssize_t;
char         *strerror(int errnum);
unsigned long strtoul(const char *s, char **end, int base);
char         *realpath(const char *path, char *out);
void         *memchr(const void *s, int c, size_t n);
void         *memmove(void *dst, const void *src, size_t n);
void         *sunos4_realloc(void *p, size_t n);
#define realloc sunos4_realloc
pid_t         sunos4_getpgrp(void);
#define getpgrp sunos4_getpgrp
#endif

/* System V Release 4.0 for the 386 (Dell Unix 2.2 and its AT&T cc): the
   compiler predefines only i386 and unix, and the headers have no
   ssize_t or S_ISLNK; libc has no killpg or mkstemp (svr40.c).  Later
   SVR4s (Solaris, UnixWare) define _SSIZE_T, which keeps them out. */
#if defined(i386) && defined(unix) && !defined(__GNUC__) && \
    !defined(__svr4__) && !defined(__SVR4) && !defined(__USLC__)
#include <sys/types.h>
#ifndef _SSIZE_T
#define GORK_SVR40
typedef int ssize_t;
int mkstemp(char *tmpl);
#define killpg(p, sig) kill(-(p), sig)
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & 0170000) == 0120000)
#endif
#endif
#endif

/* Plan 9 (9front), through APE: pcc predefines nothing of its own, so
   the build passes -DGORK_PLAN9 (docs/build/plan9.md).  There is no curses, so
   notui.c takes tui.c's place and only batch mode works; select is in
   <sys/select.h> and realpath in plan9.c.  No fchmod, killpg or
   symbolic links. */
#ifdef GORK_PLAN9
#define GORK_NO_TUI
#include <sys/types.h>
#include <sys/select.h>
char *realpath(const char *path, char *out);
#define killpg(p, sig) kill(-(p), sig)
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
#endif

/* BeOS R5 (gcc 2.9; its net_server stack, not BONE, and not Haiku):
   sockets are not file descriptors, so they take send, recv and
   closesocket, and select() takes nothing else: a pipe is read with
   read_timed(), in beos.c, which polls it.  libroot has no realpath
   (beos.c) and no killpg. */
#if defined(__BEOS__) && !defined(__HAIKU__)
#define GORK_BEOS
#define GORK_NO_PIPE_SELECT
int   read_timed(int fd, char *p, unsigned n, long secs);
char *realpath(const char *path, char *out);
#define killpg(p, sig) kill(-(p), sig)
#endif

#ifdef GORK_NO_SELECT
#define GORK_NO_PIPE_SELECT
#endif

/* OS/2 with EMX: Unix enough to fork, but paths are DOS paths (drive
   letters, either slash, any case), there are no symbolic links and no
   job control. */
#ifdef __EMX__
#define GORK_EMX
#include <sys/select.h>
/* realpath(): absolute, with forward slashes, ENOENT if nothing is there
   (main.c), as on Windows. */
char *os2_realpath(const char *path, char *out);
#define realpath       os2_realpath
#define lstat          stat
#define S_ISLNK(m)     0
#define SIGTTOU        0        /* never raised: run_command is never fg */
/* ponytail: kills the shell only, not commands it started. */
#define killpg(p, sig) kill(p, sig)
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
