/*
 * libc.c -- the C library of gork.efi: what main.c, relay.c and cJSON
 * call, on top of UEFI and the UEFI shell, and nothing else.  Strings,
 * numbers, printf and stdio are in ../fwlibc.c, shared with barebox.  Files go
 * through the shell's file calls, so paths are the shell's (fs0:\dir\x,
 * or relative to its current directory), and the program must be
 * started from the shell.  The network is in net.c.  See
 * docs/build/uefi.md.
 *
 * Text is UTF-8 here and CHAR16 at the firmware: efi_wide() and
 * efi_narrow() convert.  The console takes \r\n; files get what was
 * written.
 */

#include "efi.h"
#include "../fwlibc.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

EFI_HANDLE          efi_image;
EFI_SYSTEM_TABLE   *efi_st;
EFI_BOOT_SERVICES  *efi_bs;
EFI_SHELL_PROTOCOL *efi_shell;
static EFI_SHELL_PARAMETERS_PROTOCOL *params;

int errno;
int _fltused;           /* the MSVC target wants it once floats are used */

int main(int argc, char **argv);

/* ------------------------------------------------------------------ */
/* memory: pool allocations, kept on a list so that exit() frees them  */
/* ------------------------------------------------------------------ */

struct block {
    struct block *prev, *next;
    size_t        size;
    size_t        pad;          /* 32 bytes: what follows stays aligned */
};

static struct block *blocks;

void *malloc(size_t n)
{
    struct block *b;

    if (n > (size_t) -1 - sizeof *b ||
        efi_bs->AllocatePool(EfiLoaderData, sizeof *b + n, (void **) &b)
            != EFI_SUCCESS) {
        errno = ENOMEM;
        return NULL;
    }
    b->size = n;
    b->prev = NULL;
    b->next = blocks;
    if (blocks != NULL) blocks->prev = b;
    blocks = b;
    return b + 1;
}

void free(void *p)
{
    struct block *b;

    if (p == NULL) return;
    b = (struct block *) p - 1;
    if (b->prev != NULL) b->prev->next = b->next;
    else blocks = b->next;
    if (b->next != NULL) b->next->prev = b->prev;
    efi_bs->FreePool(b);
}

void *calloc(size_t n, size_t size)
{
    void *p;

    if (size != 0 && n > (size_t) -1 / size) {
        errno = ENOMEM;
        return NULL;
    }
    p = malloc(n * size);
    if (p != NULL) memset(p, 0, n * size);
    return p;
}

void *realloc(void *p, size_t n)
{
    void  *q;
    size_t old;

    if (p == NULL) return malloc(n);
    old = ((struct block *) p - 1)->size;
    if (n <= old) return p;
    q = malloc(n);
    if (q == NULL) return NULL;
    memcpy(q, p, old);
    free(p);
    return q;
}

int efi_errno(EFI_STATUS s)
{
    switch (s) {
    case EFI_NOT_FOUND:          return ENOENT;
    case EFI_ACCESS_DENIED:      return EACCES;
    case EFI_WRITE_PROTECTED:    return EROFS;
    case EFI_VOLUME_FULL:        return ENOSPC;
    case EFI_OUT_OF_RESOURCES:   return ENOMEM;
    case EFI_INVALID_PARAMETER:  return EINVAL;
    case EFI_UNSUPPORTED:        return ENOSYS;
    case EFI_NO_MAPPING:         return ENETDOWN;
    case EFI_TIMEOUT:            return ETIMEDOUT;
    case EFI_CONNECTION_RESET:   return ECONNRESET;
    case EFI_CONNECTION_REFUSED: return ECONNREFUSED;
    case EFI_NETWORK_UNREACHABLE: return ENETUNREACH;
    case EFI_HOST_UNREACHABLE:   return EHOSTUNREACH;
    case EFI_NO_MEDIA:           return ENOENT;
    }
    return EIO;
}

/* ------------------------------------------------------------------ */
/* UTF-8 and CHAR16                                                   */
/* ------------------------------------------------------------------ */

/* The next code point of UTF-8 at *s; a byte that is not UTF-8 stands
   for itself, as Latin-1. */
static unsigned long utf8_next(const unsigned char **s, const unsigned char *e)
{
    const unsigned char *p = *s;
    unsigned long        c = *p++;
    int                  more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    unsigned long        v = c & (0x3f >> more);
    int                  i;

    for (i = 0; i < more; i++) {
        if (p + i >= e || (p[i] & 0xc0) != 0x80) {
            *s = *s + 1;
            return c;
        }
        v = v << 6 | (p[i] & 0x3f);
    }
    *s = p + more;
    return more ? v : c;
}

/* n bytes of UTF-8 as CHAR16 into w (room for 2n + 1), \n as \r\n if
   `crlf`: the number of CHAR16 written */
static size_t to_wide(const char *s, size_t n, CHAR16 *w, int crlf)
{
    const unsigned char *p = (const unsigned char *) s, *e = p + n;
    size_t               k = 0;

    while (p < e) {
        unsigned long c = utf8_next(&p, e);

        if (c == '\n' && crlf) w[k++] = '\r';
        if (c >= 0x10000) {             /* outside the BMP: a surrogate pair */
            c -= 0x10000;
            w[k++] = (CHAR16) (0xd800 + (c >> 10));
            w[k++] = (CHAR16) (0xdc00 + (c & 0x3ff));
        } else {
            w[k++] = (CHAR16) c;
        }
    }
    w[k] = 0;
    return k;
}

CHAR16 *efi_wide(const char *s, int path)
{
    size_t  n = strlen(s), i;
    CHAR16 *w = malloc((2 * n + 1) * sizeof *w);

    if (w == NULL) return NULL;
    to_wide(s, n, w, 0);
    if (path)
        for (i = 0; w[i] != 0; i++)
            if (w[i] == '/') w[i] = '\\';
    return w;
}

/* CHAR16 as UTF-8 into out (room for 3 bytes each, and the NUL) */
size_t to_narrow(const CHAR16 *w, size_t n, char *out)
{
    unsigned char *o = (unsigned char *) out;
    size_t         i;

    for (i = 0; i < n; i++) {
        unsigned long c = w[i];

        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n &&
            w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (w[i + 1] - 0xdc00);
            i++;
        }
        if (c < 0x80) {
            *o++ = (unsigned char) c;
        } else if (c < 0x800) {
            *o++ = (unsigned char) (0xc0 | c >> 6);
            *o++ = (unsigned char) (0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            *o++ = (unsigned char) (0xe0 | c >> 12);
            *o++ = (unsigned char) (0x80 | ((c >> 6) & 0x3f));
            *o++ = (unsigned char) (0x80 | (c & 0x3f));
        } else {
            *o++ = (unsigned char) (0xf0 | c >> 18);
            *o++ = (unsigned char) (0x80 | ((c >> 12) & 0x3f));
            *o++ = (unsigned char) (0x80 | ((c >> 6) & 0x3f));
            *o++ = (unsigned char) (0x80 | (c & 0x3f));
        }
    }
    *o = '\0';
    return (size_t) ((char *) o - out);
}

static size_t wlen(const CHAR16 *w)
{
    size_t n = 0;

    while (w[n] != 0) n++;
    return n;
}

char *efi_narrow(const CHAR16 *w)
{
    size_t n = wlen(w);
    char  *s = malloc(3 * n + 1);

    if (s != NULL) to_narrow(w, n, s);
    return s;
}

/* ------------------------------------------------------------------ */
/* descriptors                                                        */
/* ------------------------------------------------------------------ */

enum { K_FREE, K_CONIN, K_CONOUT, K_FILE, K_SOCK };

struct ofile {                  /* what a descriptor stands for */
    int               kind;
    int               refs;     /* descriptors that share it (dup) */
    SHELL_FILE_HANDLE h;
    int               append;   /* K_FILE: O_APPEND */
    int               sock;     /* K_SOCK: its number in net.c */
    char             *path;     /* K_FILE: for system()'s redirection */
};

#define NFD 32
static struct ofile *fds[NFD];
static struct ofile  con_in  = { K_CONIN, 1, NULL, 0, 0, NULL };
static struct ofile  con_out = { K_CONOUT, 1, NULL, 0, 0, NULL };
static struct ofile  con_err = { K_CONOUT, 1, NULL, 0, 0, NULL };

static int new_fd(struct ofile *o)
{
    int fd;

    for (fd = 0; fd < NFD; fd++)
        if (fds[fd] == NULL) {
            fds[fd] = o;
            return fd;
        }
    errno = EMFILE;
    return -1;
}

static struct ofile *get_fd(int fd)
{
    if (fd < 0 || fd >= NFD || fds[fd] == NULL) {
        errno = EBADF;
        return NULL;
    }
    return fds[fd];
}

/* For fwlibc.c's streams. */
int fw_fd_valid(int fd) { return get_fd(fd) != NULL; }
int fw_text_fd(int fd) { return fds[fd]->kind == K_FILE; }

/* For net.c: a descriptor for socket number `sock`, or -1. */
int efi_fd_new_socket(int sock)
{
    struct ofile *o = calloc(1, sizeof *o);
    int           fd;

    if (o == NULL) return -1;
    o->kind = K_SOCK;
    o->refs = 1;
    o->sock = sock;
    if ((fd = new_fd(o)) < 0) free(o);
    return fd;
}

/* The socket number behind fd, or -1. */
int efi_fd_socket(int fd)
{
    struct ofile *o = fd >= 0 && fd < NFD ? fds[fd] : NULL;

    return o != NULL && o->kind == K_SOCK ? o->sock : -1;
}

/* Write CHAR16 text to the console (or wherever the shell sent it). */
static void con_write_wide(struct ofile *o, CHAR16 *w, size_t k)
{
    SHELL_FILE_HANDLE h = params == NULL ? NULL :
                          o == &con_err ? params->StdErr : params->StdOut;
    UINTN             bytes = k * sizeof *w;

    if (h != NULL && efi_shell != NULL)
        efi_shell->WriteFile(h, &bytes, w);
    else
        efi_st->ConOut->OutputString(efi_st->ConOut, w);
}

static ssize_t con_write(struct ofile *o, const char *p, size_t n)
{
    CHAR16 buf[1025];
    size_t done = 0;

    /* in pieces that end on a character, at most 512 bytes each */
    while (done < n) {
        size_t part = n - done > 512 ? 512 : n - done;

        while (done + part < n && part > 0 &&
               ((unsigned char) p[done + part] & 0xc0) == 0x80)
            part--;
        if (part == 0) part = n - done > 512 ? 512 : n - done;
        con_write_wide(o, buf, to_wide(p + done, part, buf, 1));
        done += part;
    }
    return (ssize_t) n;
}

/* A line from the console, as UTF-8, kept until it is read. */
static ssize_t con_read(char *p, size_t n)
{
    static char   line[1024 * 3 + 2];
    static size_t have, at;
    CHAR16        w[1024];
    UINTN         bytes = sizeof w - sizeof *w;
    size_t        k;

    if (at == have) {
        if (params == NULL || efi_shell == NULL ||
            efi_shell->ReadFile(params->StdIn, &bytes, w) != EFI_SUCCESS)
            return 0;
        k = bytes / sizeof *w;
        if (k == 0) return 0;
        have = to_narrow(w, k, line);
        at = 0;
    }
    if (n > have - at) n = have - at;
    memcpy(p, line + at, n);
    at += n;
    return (ssize_t) n;
}

static int full_path(const char *path, char *out);

/* path as the shell takes it: absolute, "." and ".." out, backslashes */
static CHAR16 *wide_path(const char *path)
{
    char *p = malloc(4096);
    CHAR16 *w;

    if (p == NULL) return NULL;
    w = full_path(path, p) == 0 ? efi_wide(p, 1) : NULL;
    free(p);
    return w;
}

int open(const char *path, int flags, ...)
{
    struct ofile     *o;
    CHAR16           *w;
    SHELL_FILE_HANDLE h = NULL;
    EFI_FILE_INFO    *info;
    EFI_STATUS        s;
    int               fd, exists, wr = (flags & O_ACCMODE) != O_RDONLY;
    uint64_t          mode = EFI_FILE_MODE_READ | (wr ? EFI_FILE_MODE_WRITE : 0);

    if (efi_shell == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if ((w = wide_path(path)) == NULL) return -1;
    s = efi_shell->OpenFileByName(w, &h, EFI_FILE_MODE_READ);
    exists = s == EFI_SUCCESS;
    if (exists) {
        info = efi_shell->GetFileInfo(h);
        if (info != NULL && (info->Attribute & EFI_FILE_DIRECTORY) && wr) {
            efi_bs->FreePool(info);
            efi_shell->CloseFile(h);
            free(w);
            errno = EISDIR;
            return -1;
        }
        if (info != NULL) efi_bs->FreePool(info);
        efi_shell->CloseFile(h);
        h = NULL;
    }
    if (exists && (flags & O_CREAT) && (flags & O_EXCL)) {
        free(w);
        errno = EEXIST;
        return -1;
    }
    if (!exists && !(flags & O_CREAT)) {
        free(w);
        errno = ENOENT;
        return -1;
    }
    if (exists && wr && (flags & O_TRUNC)) {
        s = efi_shell->DeleteFileByName(w);
        if (s != EFI_SUCCESS) {
            free(w);
            errno = efi_errno(s);
            return -1;
        }
        exists = 0;
    }
    s = exists ? efi_shell->OpenFileByName(w, &h, mode)
               : efi_shell->CreateFile(w, 0, &h);
    free(w);
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    o = calloc(1, sizeof *o);
    if (o == NULL || (o->path = strdup(path)) == NULL) {
        free(o);
        efi_shell->CloseFile(h);
        return -1;
    }
    o->kind = K_FILE;
    o->refs = 1;
    o->h = h;
    o->append = (flags & O_APPEND) != 0;
    if ((fd = new_fd(o)) < 0) {
        efi_shell->CloseFile(h);
        free(o->path);
        free(o);
    }
    return fd;
}

ssize_t read(int fd, void *p, size_t n)
{
    struct ofile *o = get_fd(fd);
    UINTN         k = n;
    EFI_STATUS    s;

    if (o == NULL) return -1;
    switch (o->kind) {
    case K_CONIN:
        return con_read(p, n);
    case K_SOCK:
        return efi_sock_read(o->sock, p, n);
    case K_FILE:
        s = efi_shell->ReadFile(o->h, &k, p);
        if (s != EFI_SUCCESS) {
            errno = efi_errno(s);
            return -1;
        }
        return (ssize_t) k;
    }
    errno = EBADF;
    return -1;
}

ssize_t write(int fd, const void *p, size_t n)
{
    struct ofile *o = get_fd(fd);
    UINTN         k = n;
    EFI_STATUS    s;

    if (o == NULL) return -1;
    switch (o->kind) {
    case K_CONOUT:
        return con_write(o, p, n);
    case K_SOCK:
        return efi_sock_write(o->sock, p, n);
    case K_FILE:
        if (o->append) efi_shell->SetFilePosition(o->h, (uint64_t) -1);
        s = efi_shell->WriteFile(o->h, &k, (void *) p);
        if (s != EFI_SUCCESS) {
            errno = efi_errno(s);
            return -1;
        }
        return (ssize_t) k;
    }
    errno = EBADF;
    return -1;
}

int close(int fd)
{
    struct ofile *o = get_fd(fd);

    if (o == NULL) return -1;
    fds[fd] = NULL;
    if (--o->refs > 0) return 0;
    if (o->kind == K_FILE) {
        efi_shell->CloseFile(o->h);
        free(o->path);
        free(o);
    } else if (o->kind == K_SOCK) {
        efi_sock_close(o->sock);
        free(o);
    }
    return 0;
}

int dup2(int fd, int to)
{
    struct ofile *o = get_fd(fd);

    if (o == NULL || to < 0 || to >= NFD) {
        errno = EBADF;
        return -1;
    }
    if (fd == to) return to;
    if (fds[to] != NULL) close(to);
    fds[to] = o;
    o->refs++;
    return to;
}

int dup(int fd)
{
    struct ofile *o = get_fd(fd);
    int           to;

    if (o == NULL) return -1;
    if ((to = new_fd(o)) >= 0) o->refs++;
    return to;
}

off_t lseek(int fd, off_t off, int whence)
{
    struct ofile *o = get_fd(fd);
    uint64_t      pos = 0, size = 0;

    if (o == NULL) return -1;
    if (o->kind != K_FILE) {
        errno = ESPIPE;
        return -1;
    }
    if (whence == SEEK_CUR) efi_shell->GetFilePosition(o->h, &pos);
    if (whence == SEEK_END) efi_shell->GetFileSize(o->h, &size);
    pos = (whence == SEEK_SET ? 0 : whence == SEEK_CUR ? pos : size) + off;
    if (efi_shell->SetFilePosition(o->h, pos) != EFI_SUCCESS) {
        errno = EINVAL;
        return -1;
    }
    return (off_t) pos;
}

int fsync(int fd)
{
    struct ofile *o = get_fd(fd);

    if (o == NULL) return -1;
    if (o->kind == K_FILE) efi_shell->FlushFile(o->h);
    return 0;
}

int isatty(int fd)
{
    struct ofile *o = fd >= 0 && fd < NFD ? fds[fd] : NULL;

    return o != NULL && (o->kind == K_CONIN || o->kind == K_CONOUT);
}

/* ------------------------------------------------------------------ */
/* time                                                               */
/* ------------------------------------------------------------------ */

static volatile clock_t ticks;
static EFI_EVENT        tick_event;

static void EFIAPI on_tick(EFI_EVENT e, void *ctx)
{
    (void) e;
    (void) ctx;
    ticks++;
}

clock_t clock(void) { return ticks; }

/* The firmware's clock, taken to be UTC.  ponytail: its TimeZone is not
   applied; most firmware leaves it unspecified anyway. */
time_t time(time_t *t)
{
    EFI_TIME now;
    time_t   v = 0;

    if (efi_st->RuntimeServices->GetTime(&now, NULL) == EFI_SUCCESS)
        v = (time_t) fw_days_from_civil(now.Year, now.Month, now.Day) * 86400 +
            now.Hour * 3600 + now.Minute * 60 + now.Second;
    if (t != NULL) *t = v;
    return v;
}

unsigned sleep(unsigned secs)
{
    clock_t end = ticks + (clock_t) secs * CLOCKS_PER_SEC;

    while (ticks < end) {
        efi_net_poll();
        efi_bs->Stall(10000);
    }
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void) tz;
    tv->tv_sec = (long) time(NULL);
    tv->tv_usec = 0;
    return 0;
}

pid_t getpid(void) { return 1; }

/* ------------------------------------------------------------------ */
/* signals: none; Ctrl-C is the shell's ExecutionBreak event           */
/* ------------------------------------------------------------------ */

sighandler_t signal(int sig, sighandler_t fn)
{
    (void) sig;
    (void) fn;
    return SIG_DFL;
}

int sigaction(int sig, const struct sigaction *sa, struct sigaction *old)
{
    (void) sig;
    (void) sa;
    if (old != NULL) memset(old, 0, sizeof *old);
    return 0;
}

int sigemptyset(sigset_t *s)
{
    *s = 0;
    return 0;
}

int raise(int sig)
{
    exit(128 + sig);
}

int kill(pid_t pid, int sig)
{
    (void) pid;
    (void) sig;
    errno = ENOSYS;
    return -1;
}

/* Ctrl-C at the shell: end the program, as SIGINT would. */
void efi_check_break(void)
{
    if (efi_shell != NULL && efi_shell->ExecutionBreak != NULL &&
        efi_bs->CheckEvent(efi_shell->ExecutionBreak) == EFI_SUCCESS) {
        fputs("^C\n", stderr);
        exit(130);
    }
}

/* select() for reading: sockets, files and the console. */
int select(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    clock_t end = 0;
    fd_set  out;
    int     fd, ready;

    (void) ex;
    if (tv != NULL)
        end = ticks + tv->tv_sec * CLOCKS_PER_SEC +
              tv->tv_usec / (1000000 / CLOCKS_PER_SEC);
    for (;;) {
        FD_ZERO(&out);
        ready = 0;
        efi_net_poll();
        for (fd = 0; fd < n && fd < NFD; fd++) {
            struct ofile *o = fds[fd];

            if (rd == NULL || !FD_ISSET(fd, rd) || o == NULL) continue;
            if (o->kind != K_SOCK || efi_sock_ready(o->sock)) {
                FD_SET(fd, &out);
                ready++;
            }
        }
        if (ready > 0 || (tv != NULL && ticks >= end)) break;
        efi_check_break();
        efi_bs->Stall(1000);
    }
    if (rd != NULL) *rd = out;
    if (wr != NULL) FD_ZERO(wr);
    return ready;
}

/* ------------------------------------------------------------------ */
/* paths: fs0:/dir/file                                               */
/* ------------------------------------------------------------------ */

#define PATHMAX 4096

/* The shell's current directory as fs0:/dir, into out[PATHMAX]. */
static int cwd(char *out)
{
    const CHAR16 *w = efi_shell != NULL ? efi_shell->GetCurDir(NULL) : NULL;
    char         *s, *p;

    if (w == NULL) {
        errno = ENOENT;                 /* no file system chosen yet */
        return -1;
    }
    if ((s = efi_narrow(w)) == NULL) return -1;
    if (strlen(s) >= PATHMAX) {
        free(s);
        errno = ENAMETOOLONG;
        return -1;
    }
    for (p = s; *p != '\0' && *p != ':'; p++)
        *p = (char) tolower((unsigned char) *p);
    for (; *p != '\0'; p++)
        if (*p == '\\') *p = '/';
    strcpy(out, s);
    free(s);
    return 0;
}

char *getcwd(char *buf, size_t size)
{
    char p[PATHMAX];

    if (cwd(p) != 0) return NULL;
    if (strlen(p) >= size) {
        errno = ERANGE;
        return NULL;
    }
    return strcpy(buf, p);
}

int chdir(const char *path)
{
    (void) path;
    errno = ENOSYS;
    return -1;
}

/* path, absolute and with "." and ".." taken out, into out[PATHMAX]:
   map:/a/b.  Whether it exists is not checked. */
static int full_path(const char *path, char *out)
{
    char        in[PATHMAX], *p, *w, *start;
    const char *colon = strchr(path, ':');

    if (colon != NULL && strcspn(path, "/\\") > (size_t) (colon - path)) {
        if (strlen(path) >= PATHMAX) goto toolong;
        strcpy(in, path);
    } else {
        if (cwd(in) != 0) return -1;
        if (path[0] == '/' || path[0] == '\\') {    /* this map's root */
            in[strcspn(in, ":") + 1] = '\0';
        } else {
            if (strlen(in) + 1 >= PATHMAX) goto toolong;
            strcat(in, "/");
        }
        if (strlen(in) + strlen(path) >= PATHMAX) goto toolong;
        strcat(in, path);
    }
    for (p = in; *p != '\0'; p++)
        if (*p == '\\') *p = '/';

    /* the map's name in lower case, then one name at a time */
    for (p = in, w = out; *p != ':'; p++)
        *w++ = (char) tolower((unsigned char) *p);
    *w++ = ':';
    p++;
    start = w;
    while (*p != '\0') {
        size_t n;

        while (*p == '/') p++;
        n = strcspn(p, "/");
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (w > start && *--w != '/')
                ;
        } else if (n > 0 && !(n == 1 && p[0] == '.')) {
            *w++ = '/';
            memmove(w, p, n);
            w += n;
        }
        p += n;
    }
    if (w == start) *w++ = '/';
    *w = '\0';
    return 0;

toolong:
    errno = ENAMETOOLONG;
    return -1;
}

char *realpath(const char *path, char *out)
{
    char        p[PATHMAX];
    struct stat st;

    if (full_path(path, p) != 0) return NULL;
    if (stat(p, &st) != 0) return NULL;     /* ENOENT if nothing is there */
    if (out == NULL) return strdup(p);
    return strcpy(out, p);
}

/* ------------------------------------------------------------------ */
/* files by name                                                      */
/* ------------------------------------------------------------------ */

static time_t efi_to_time(const EFI_TIME *t)
{
    if (t->Year < 1970) return 0;
    return (time_t) fw_days_from_civil(t->Year, t->Month, t->Day) * 86400 +
           t->Hour * 3600 + t->Minute * 60 + t->Second;
}

static void fill_stat(const EFI_FILE_INFO *info, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_mode = (info->Attribute & EFI_FILE_DIRECTORY) ? S_IFDIR | 0755
                  : (info->Attribute & EFI_FILE_READ_ONLY) ? S_IFREG | 0444
                  : S_IFREG | 0644;
    st->st_size = (off_t) info->FileSize;
    st->st_mtime = efi_to_time(&info->ModificationTime);
    st->st_atime = efi_to_time(&info->LastAccessTime);
    st->st_ctime = efi_to_time(&info->CreateTime);
    st->st_nlink = 1;
}

/* Open by name for reading: 0, or -1 with errno. */
static int open_name(const char *path, SHELL_FILE_HANDLE *h, uint64_t mode)
{
    CHAR16    *w;
    EFI_STATUS s;

    if (efi_shell == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if ((w = wide_path(path)) == NULL) return -1;
    s = efi_shell->OpenFileByName(w, h, mode);
    free(w);
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    return 0;
}

int stat(const char *path, struct stat *st)
{
    SHELL_FILE_HANDLE h;
    EFI_FILE_INFO    *info;

    if (open_name(path, &h, EFI_FILE_MODE_READ) != 0) return -1;
    info = efi_shell->GetFileInfo(h);
    efi_shell->CloseFile(h);
    if (info == NULL) {
        errno = EIO;
        return -1;
    }
    fill_stat(info, st);
    efi_bs->FreePool(info);
    return 0;
}

int lstat(const char *path, struct stat *st) { return stat(path, st); }

int fstat(int fd, struct stat *st)
{
    struct ofile  *o = get_fd(fd);
    EFI_FILE_INFO *info;

    if (o == NULL) return -1;
    memset(st, 0, sizeof *st);
    if (o->kind != K_FILE) {
        st->st_mode = S_IFCHR | 0600;
        return 0;
    }
    if ((info = efi_shell->GetFileInfo(o->h)) == NULL) {
        errno = EIO;
        return -1;
    }
    fill_stat(info, st);
    efi_bs->FreePool(info);
    return 0;
}

int access(const char *path, int mode)
{
    struct stat st;

    (void) mode;
    return stat(path, &st);
}

int chmod(const char *path, mode_t mode)
{
    (void) path;
    (void) mode;
    return 0;                   /* FAT keeps no modes */
}

int fchmod(int fd, mode_t mode)
{
    (void) fd;
    (void) mode;
    return 0;
}

mode_t umask(mode_t m)
{
    (void) m;
    return 022;
}

int mkdir(const char *path, mode_t mode)
{
    SHELL_FILE_HANDLE h;
    CHAR16           *w;
    EFI_STATUS        s;
    struct stat       st;

    (void) mode;
    if (stat(path, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    if ((w = wide_path(path)) == NULL) return -1;
    s = efi_shell->CreateFile(w, EFI_FILE_DIRECTORY, &h);
    free(w);
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    efi_shell->CloseFile(h);
    return 0;
}

int remove(const char *path)
{
    CHAR16    *w;
    EFI_STATUS s;

    if (efi_shell == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if ((w = wide_path(path)) == NULL) return -1;
    s = efi_shell->DeleteFileByName(w);
    free(w);
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    return 0;
}

int unlink(const char *path) { return remove(path); }
int rmdir(const char *path)  { return remove(path); }

/* rename() where the firmware renames nothing (U-Boot): a copy, then
   the old one removed. */
static int copy_over(const char *from, const char *to)
{
    char   buf[4096];
    int    in, out, e = 0;
    ssize_t n;

    if ((in = open(from, O_RDONLY)) < 0) return -1;
    if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
        e = errno;
        close(in);
        errno = e;
        return -1;
    }
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t) n) != n) {
            e = errno ? errno : ENOSPC;
            break;
        }
    if (n < 0) e = errno;
    close(in);
    close(out);
    if (e != 0) {
        remove(to);
        errno = e;
        return -1;
    }
    return remove(from);
}

/* The new name goes in the file's information: as \dir\name, from the
   root of its file system, so both must be on the same one.  An existing
   `to` is removed first, so this is not atomic. */
int rename(const char *from, const char *to)
{
    char              f[PATHMAX], t[PATHMAX];
    SHELL_FILE_HANDLE h;
    EFI_FILE_INFO    *info, *ninfo;
    CHAR16           *w;
    struct stat       st;
    size_t            mf, mt, wn;
    EFI_STATUS        s;

    if (full_path(from, f) != 0 || full_path(to, t) != 0) return -1;
    mf = strcspn(f, ":");
    mt = strcspn(t, ":");
    if (mf != mt || strncmp(f, t, mf) != 0) {
        errno = EINVAL;                 /* another file system */
        return -1;
    }
    if (strcasecmp(f, t) == 0) return 0;
    if (open_name(f, &h, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE) != 0)
        return -1;
    if (stat(t, &st) == 0 && remove(t) != 0) {
        efi_shell->CloseFile(h);
        return -1;
    }
    info = efi_shell->GetFileInfo(h);
    w = efi_wide(t + mt + 1, 1);
    if (info == NULL || w == NULL) {
        if (info != NULL) efi_bs->FreePool(info);
        free(w);
        efi_shell->CloseFile(h);
        errno = ENOMEM;
        return -1;
    }
    wn = wlen(w);
    ninfo = malloc(sizeof *ninfo + wn * sizeof *w);
    if (ninfo == NULL) {
        efi_bs->FreePool(info);
        free(w);
        efi_shell->CloseFile(h);
        return -1;
    }
    memcpy(ninfo, info, sizeof *ninfo);
    memcpy(ninfo->FileName, w, (wn + 1) * sizeof *w);
    ninfo->Size = sizeof *ninfo + wn * sizeof *w;
    s = efi_shell->SetFileInfo(h, ninfo);
    efi_bs->FreePool(info);
    free(ninfo);
    free(w);
    efi_shell->CloseFile(h);
    if (s == EFI_ACCESS_DENIED || s == EFI_UNSUPPORTED || s == EFI_WRITE_PROTECTED)
        return copy_over(f, t);
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    return 0;
}

/* tmpnam(): in HOME, the directory gork.efi is in */
char *tmpnam(char *s)
{
    static char   name[L_tmpnam];
    static int    n;
    const char   *home = getenv("HOME");
    struct stat   st;

    if (s == NULL) s = name;
    if (home == NULL || strlen(home) + 16 >= L_tmpnam) return NULL;
    do
        sprintf(s, "%s%sgork%04d.tmp", home,
                home[strlen(home) - 1] == '/' ? "" : "/", n++ % 10000);
    while (stat(s, &st) == 0 && n < 10000);
    return s;
}

/* ------------------------------------------------------------------ */
/* directories                                                        */
/* ------------------------------------------------------------------ */

struct _DIR {
    SHELL_FILE_HANDLE    h;
    EFI_SHELL_FILE_INFO *list;
    EFI_LIST_ENTRY      *at;
    struct dirent        ent;
};

DIR *opendir(const char *path)
{
    DIR        *d;
    struct stat st;
    EFI_STATUS  s;

    if (stat(path, &st) != 0) return NULL;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }
    if ((d = calloc(1, sizeof *d)) == NULL) return NULL;
    if (open_name(path, &d->h, EFI_FILE_MODE_READ) != 0) {
        free(d);
        return NULL;
    }
    s = efi_shell->FindFilesInDir(d->h, &d->list);
    if (s != EFI_SUCCESS) {
        efi_shell->CloseFile(d->h);
        free(d);
        errno = efi_errno(s);
        return NULL;
    }
    d->at = d->list != NULL ? d->list->Link.Flink : NULL;
    return d;
}

struct dirent *readdir(DIR *d)
{
    EFI_SHELL_FILE_INFO *f;
    char                *name;

    if (d->list == NULL || d->at == &d->list->Link) return NULL;
    f = (EFI_SHELL_FILE_INFO *) d->at;
    d->at = d->at->Flink;
    name = efi_narrow(f->FileName);
    if (name == NULL) return NULL;
    strncpy(d->ent.d_name, name, sizeof d->ent.d_name - 1);
    d->ent.d_name[sizeof d->ent.d_name - 1] = '\0';
    free(name);
    return &d->ent;
}

int closedir(DIR *d)
{
    if (d->list != NULL) efi_shell->FreeFileList(&d->list);
    efi_shell->CloseFile(d->h);
    free(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the environment: the shell's variables                             */
/* ------------------------------------------------------------------ */

#define NENV 32
static char *env_set[NENV];             /* putenv()'s, "NAME=value" */
static char *env_got[NENV];             /* what getenv() handed out */
static char  home_dir[PATHMAX];         /* where gork.efi is */

char *getenv(const char *name)
{
    size_t        n = strlen(name);
    const CHAR16 *v;
    CHAR16       *w;
    char         *s;
    int           i;

    for (i = 0; i < NENV; i++)
        if (env_set[i] != NULL && strncmp(env_set[i], name, n) == 0 &&
            env_set[i][n] == '=')
            return env_set[i] + n + 1;
    v = NULL;
    if (efi_shell != NULL && (w = efi_wide(name, 0)) != NULL) {
        v = efi_shell->GetEnv(w);
        free(w);
    }
    if (v == NULL)
        return strcmp(name, "HOME") == 0 && home_dir[0] != '\0' ? home_dir
                                                                : NULL;
    if ((s = efi_narrow(v)) == NULL) return NULL;
    /* the same name gets the same slot, so asking again leaks nothing */
    for (i = 0; i < NENV; i++)
        if (env_got[i] != NULL && strncmp(env_got[i], name, n) == 0 &&
            env_got[i][n] == '=')
            break;
    if (i == NENV)
        for (i = 0; i < NENV && env_got[i] != NULL; i++)
            ;
    if (i == NENV) i = 0;
    free(env_got[i]);
    env_got[i] = malloc(n + 1 + strlen(s) + 1);
    if (env_got[i] == NULL) {
        free(s);
        return NULL;
    }
    sprintf(env_got[i], "%s=%s", name, s);
    free(s);
    return env_got[i] + n + 1;
}

int putenv(char *s)
{
    size_t n = strcspn(s, "=");
    int    i, free_slot = -1;

    for (i = 0; i < NENV; i++) {
        if (env_set[i] != NULL && strncmp(env_set[i], s, n) == 0 &&
            env_set[i][n] == '=') {
            env_set[i] = s;
            return 0;
        }
        if (env_set[i] == NULL && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        errno = ENOMEM;
        return -1;
    }
    env_set[free_slot] = s;
    return 0;
}

/* ------------------------------------------------------------------ */
/* system(): the shell runs the line                                  */
/* ------------------------------------------------------------------ */

/* " >>a \"fs0:\dir\file\"" at out: `op` and path as the shell's parser
   takes them */
static void shell_path(const char *path, char *out, const char *op)
{
    char  p[PATHMAX], *c;

    if (full_path(path, p) != 0) strcpy(p, path);
    for (c = p; *c != '\0'; c++)
        if (*c == '/') *c = '\\';
    sprintf(out, "%s\"%s\"", op, p);
}

/* Where fd 1 points at a file (as run_command points 1 and 2), the
   shell is told to send the command's output there, as ASCII.  It will
   not send stderr to the same file, so that goes to a second one, added
   to the first at the end. */
int system(const char *cmd)
{
    struct ofile *o1 = fds[1], *o2 = fds[2];
    char         *line, err[L_tmpnam];
    CHAR16       *w;
    EFI_STATUS    s, status = EFI_SUCCESS;
    int           to_file = o1 != NULL && o1->kind == K_FILE;
    int           both = to_file && o2 == o1 && tmpnam(err) != NULL;
    size_t        n;

    if (cmd == NULL) return efi_shell != NULL;
    if (efi_shell == NULL) {
        errno = ENOSYS;
        return -1;
    }
    n = strlen(cmd) + 64 + (to_file ? 2 * PATHMAX : 0);
    if ((line = malloc(n)) == NULL) return -1;
    /* EDK2 runs the line as `Shell.efi -exit LINE`: a shell of its own,
       which would show its banner and the mapping table first */
    strcpy(line, "-nomap -noversion -nostartup ");
    strcat(line, cmd);
    if (to_file) {
        /* the command writes the file itself: let go of it meanwhile */
        efi_shell->CloseFile(o1->h);
        o1->h = NULL;
        shell_path(o1->path, line + strlen(line), " >>a ");
        if (both) shell_path(err, line + strlen(line), " 2>a ");
    }
    w = efi_wide(line, 0);
    free(line);
    if (w == NULL) return -1;
    s = efi_shell->Execute(&efi_image, w, NULL, &status);
    free(w);
    if (to_file &&
        open_name(o1->path, &o1->h, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE)
            == 0) {
        /* back to the end of the file, then what went to stderr */
        efi_shell->SetFilePosition(o1->h, (uint64_t) -1);
        if (both) {
            FILE  *f = fopen(err, "r");
            char   buf[1024];
            size_t k;

            while (f != NULL && (k = fread(buf, 1, sizeof buf, f)) > 0)
                write(1, buf, k);
            if (f != NULL) fclose(f);
        }
    }
    if (both) remove(err);
    if (s != EFI_SUCCESS) return 1;
    return EFI_ERROR(status) ? (int) (status & 0xff) : (int) status;
}

/* ------------------------------------------------------------------ */
/* start and end                                                      */
/* ------------------------------------------------------------------ */

#define NATEXIT 8
static void (*at_exit[NATEXIT])(void);
static int    nat_exit;

int atexit(void (*fn)(void))
{
    if (nat_exit == NATEXIT) return -1;
    at_exit[nat_exit++] = fn;
    return 0;
}

void efi_net_end(void);

void exit(int status)
{
    int fd;

    while (nat_exit > 0) at_exit[--nat_exit]();
    fflush(NULL);
    for (fd = 3; fd < NFD; fd++)
        if (fds[fd] != NULL) close(fd);
    efi_net_end();
    if (tick_event != NULL) efi_bs->CloseEvent(tick_event);
    while (blocks != NULL) free(blocks + 1);
    /* the shell shows a nonzero status in %lasterror% */
    efi_bs->Exit(efi_image, (EFI_STATUS) (unsigned) status, 0, NULL);
    for (;;)
        ;
}

void abort(void) { exit(134); }

/* What the shell set the program going with: argv as UTF-8. */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    static EFI_GUID shell_guid  = EFI_SHELL_PROTOCOL_GUID;
    static EFI_GUID params_guid = EFI_SHELL_PARAMETERS_PROTOCOL_GUID;
    char          **argv;
    int             argc, i;

    efi_image = image;
    efi_st = st;
    efi_bs = st->BootServices;
    efi_bs->SetWatchdogTimer(0, 0, 0, NULL);
    if (efi_bs->HandleProtocol(image, &params_guid, (void **) &params)
            != EFI_SUCCESS ||
        efi_bs->LocateProtocol(&shell_guid, NULL, (void **) &efi_shell)
            != EFI_SUCCESS) {
        static CHAR16 msg[] = L"gork: start it from the UEFI shell\r\n";

        st->ConOut->OutputString(st->ConOut, msg);
        return EFI_UNSUPPORTED;
    }
    efi_bs->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, on_tick,
                        NULL, &tick_event);
    efi_bs->SetTimer(tick_event, TimerPeriodic, 10000000 / CLOCKS_PER_SEC);

    fds[0] = &con_in;
    fds[1] = &con_out;
    fds[2] = &con_err;
    fw_stdio_init();

    argc = (int) params->Argc;
    argv = calloc((size_t) argc + 1, sizeof *argv);
    if (argv == NULL) exit(1);
    for (i = 0; i < argc; i++)
        if ((argv[i] = efi_narrow(params->Argv[i])) == NULL) exit(1);

    /* HOME, unless the shell sets one: the directory gork.efi is in */
    if (argc > 0) {
        char  dir[PATHMAX];
        char *slash;

        if (full_path(argv[0], dir) == 0 &&
            (slash = strrchr(dir, '/')) != NULL) {
            slash[slash == strchr(dir, '/')] = '\0';    /* keep "fs0:/" */
            strcpy(home_dir, dir);
        }
    }
    exit(main(argc, argv));
}
