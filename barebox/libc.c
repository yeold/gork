/*
 * libc.c -- gork's half of the C library for barebox: what main.c,
 * relay.c and cJSON call that fwlibc.c does not give, on barebox's own
 * calls (bb.h, done by glue.c) and on gork's own TCP/IP (ip.c).  Built
 * against fwinclude/, all names gork_* (barebox/rename.py), so nothing
 * here meets barebox's C library.
 *
 * Descriptors 0, 1 and 2 are the console; files are barebox's; a socket
 * is ip.c's one TCP connection.  Paths are barebox's, /dir/file, made
 * absolute here by the text alone (barebox resolves only paths that
 * exist).  Memory is kept on a list, so that exit() frees it: gork runs
 * as a command, and barebox goes on after it.  See docs/build/barebox.md.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "../fwlibc.h"
#include "../ip.h"
#include "bb.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int errno;

/* ------------------------------------------------------------------ */
/* memory: barebox's, on a list so that exit() frees it               */
/* ------------------------------------------------------------------ */

struct block {
    struct block *next, *prev;
    size_t        size;
    long long     align;        /* what follows is aligned for anything */
};
static struct block *blocks;

void *malloc(size_t n)
{
    struct block *b = bb_malloc(sizeof *b + n);

    if (b == NULL) {
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
    bb_free(b);
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
    void *q;

    if (p == NULL) return malloc(n);
    if (n == 0) {
        free(p);
        return NULL;
    }
    q = malloc(n);
    if (q == NULL) return NULL;
    memcpy(q, p, ((struct block *) p - 1)->size < n
                 ? ((struct block *) p - 1)->size : n);
    free(p);
    return q;
}

static void free_all(void)
{
    while (blocks != NULL) free(blocks + 1);
}

/* ------------------------------------------------------------------ */
/* descriptors                                                        */
/* ------------------------------------------------------------------ */

#define NFD 16
enum { K_FREE, K_CON, K_FILE, K_SOCK };
static struct ofile {
    int kind;
    int bfd;                    /* K_FILE: barebox's descriptor */
    int refs;                   /* descriptors sharing it (dup) */
} ofiles[NFD];
static struct ofile *fds[NFD];

static int new_fd(struct ofile *o)
{
    int i;

    for (i = 0; i < NFD; i++)
        if (fds[i] == NULL) {
            fds[i] = o;
            o->refs++;
            return i;
        }
    errno = EMFILE;
    return -1;
}

static struct ofile *new_ofile(int kind)
{
    int i;

    for (i = 0; i < NFD; i++)
        if (ofiles[i].kind == K_FREE) {
            ofiles[i].kind = kind;
            ofiles[i].refs = 0;
            return &ofiles[i];
        }
    errno = EMFILE;
    return NULL;
}

static struct ofile *get_fd(int fd)
{
    if (fd < 0 || fd >= NFD || fds[fd] == NULL) {
        errno = EBADF;
        return NULL;
    }
    return fds[fd];
}

/* For fwlibc.c's streams: barebox's files are as written, no text mode. */
int fw_fd_valid(int fd) { return get_fd(fd) != NULL; }
int fw_text_fd(int fd) { (void) fd; return 0; }

static long fail(long r)
{
    errno = (int) -r;
    return -1;
}

/* The console's input, a line at a time, echoed: what the trust prompt
   reads. */
static char   line[256];
static size_t line_len, line_at;

static ssize_t con_read(char *p, size_t n)
{
    size_t k;

    while (line_at == line_len) {
        int c;

        line_len = line_at = 0;
        for (;;) {
            while ((c = bb_console_getc()) < 0) {
                card_break();
                bb_udelay(1000);
            }
            if (c == '\r' || c == '\n') {
                bb_console_write("\n", 1);
                line[line_len++] = '\n';
                break;
            }
            if ((c == 8 || c == 127) && line_len > 0) {
                line_len--;
                bb_console_write("\b \b", 3);
            } else if (c >= ' ' && line_len < sizeof line - 1) {
                line[line_len++] = (char) c;
                bb_console_write((char *) &line[line_len - 1], 1);
            }
        }
    }
    k = line_len - line_at < n ? line_len - line_at : n;
    memcpy(p, line + line_at, k);
    line_at += k;
    return (ssize_t) k;
}

static void console_init(void)
{
    static struct ofile con;
    int i;

    con.kind = K_CON;
    for (i = 0; i < 3; i++) {
        fds[i] = &con;
        con.refs++;
    }
}

int open(const char *path, int flags, ...)
{
    struct ofile *o;
    unsigned mode = 0644;
    long r;
    va_list ap;

    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    if ((o = new_ofile(K_FILE)) == NULL) return -1;
    if ((r = bb_open(path, flags, mode)) < 0) {
        o->kind = K_FREE;
        return (int) fail(r);
    }
    o->bfd = (int) r;
    if ((r = new_fd(o)) < 0) {
        bb_close(o->bfd);
        o->kind = K_FREE;
    }
    return (int) r;
}

ssize_t read(int fd, void *p, size_t n)
{
    struct ofile *o = get_fd(fd);
    long r;

    if (o == NULL) return -1;
    switch (o->kind) {
    case K_CON:  return con_read(p, n);
    case K_SOCK: r = ip_tcp_read(p, n); return r;
    default:     r = bb_read(o->bfd, p, n); return r < 0 ? fail(r) : r;
    }
}

ssize_t write(int fd, const void *p, size_t n)
{
    struct ofile *o = get_fd(fd);
    long r;

    if (o == NULL) return -1;
    switch (o->kind) {
    case K_CON:  bb_console_write(p, n); return (ssize_t) n;
    case K_SOCK: return ip_tcp_write(p, n);
    default:     r = bb_write(o->bfd, p, n); return r < 0 ? fail(r) : r;
    }
}

int close(int fd)
{
    struct ofile *o = get_fd(fd);

    if (o == NULL) return -1;
    fds[fd] = NULL;
    if (--o->refs > 0) return 0;
    if (o->kind == K_FILE) bb_close(o->bfd);
    if (o->kind == K_SOCK) ip_tcp_close();
    if (o->kind != K_CON) o->kind = K_FREE;
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

    return o == NULL ? -1 : new_fd(o);
}

off_t lseek(int fd, off_t off, int whence)
{
    struct ofile *o = get_fd(fd);
    long long r;

    if (o == NULL) return -1;
    if (o->kind != K_FILE) {
        errno = ESPIPE;
        return -1;
    }
    r = bb_lseek(o->bfd, off, whence);
    return r < 0 ? fail((long) r) : r;
}

int fsync(int fd) { return get_fd(fd) == NULL ? -1 : 0; }

int isatty(int fd)
{
    struct ofile *o = get_fd(fd);

    return o != NULL && o->kind == K_CON;
}

/* ------------------------------------------------------------------ */
/* sockets: ip.c's one TCP connection                                 */
/* ------------------------------------------------------------------ */

int socket(int domain, int type, int proto)
{
    struct ofile *o;
    int fd;

    (void) proto;
    if (domain != AF_INET || type != SOCK_STREAM) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (ip_start() != 0) {
        errno = ENETDOWN;
        return -1;
    }
    if ((o = new_ofile(K_SOCK)) == NULL) return -1;
    if ((fd = new_fd(o)) < 0) o->kind = K_FREE;
    return fd;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *) addr;
    struct ofile *o = get_fd(fd);

    (void) len;
    if (o == NULL) return -1;
    if (o->kind != K_SOCK) {
        errno = ENOTSOCK;
        return -1;
    }
    return ip_tcp_connect((const uint8_t *) &in->sin_addr.s_addr,
                          ntohs(in->sin_port)) == 0 ? 0 : -1;
}

ssize_t send(int fd, const void *p, size_t n, int flags)
{
    (void) flags;
    return write(fd, p, n);
}

ssize_t recv(int fd, void *p, size_t n, int flags)
{
    (void) flags;
    return read(fd, p, n);
}

int shutdown(int fd, int how)
{
    (void) how;
    return get_fd(fd) == NULL ? -1 : 0;
}

in_addr_t inet_addr(const char *s)
{
    unsigned long part[4];
    int i;
    char *end;

    for (i = 0; i < 4; i++) {
        if (!isdigit((unsigned char) *s)) return INADDR_NONE;
        part[i] = strtoul(s, &end, 10);
        if (part[i] > 255) return INADDR_NONE;
        s = end;
        if (i < 3 && *s++ != '.') return INADDR_NONE;
    }
    if (*s != '\0') return INADDR_NONE;
    return htonl((in_addr_t) (part[0] << 24 | part[1] << 16 | part[2] << 8 |
                              part[3]));
}

struct hostent *gethostbyname(const char *name)
{
    static struct hostent h;
    static uint8_t        addr[4];
    static char          *list[2];
    static char           hname[256];

    in_addr_t a = inet_addr(name);      /* a dotted address is itself */

    if (a != INADDR_NONE) memcpy(addr, &a, 4);
    else if (ip_start() != 0 || ip_resolve(name, addr) != 0) return NULL;
    strncpy(hname, name, sizeof hname - 1);
    list[0] = (char *) addr;
    list[1] = NULL;
    h.h_name = hname;
    h.h_addrtype = AF_INET;
    h.h_length = 4;
    h.h_addr_list = list;
    return &h;
}

/* select() for reading: the socket, the console and files. */
int select(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    unsigned long long end = 0;
    fd_set want;
    int    fd, ready;

    (void) ex;
    if (rd != NULL) want = *rd;
    else FD_ZERO(&want);
    if (tv != NULL)
        end = bb_ms() + (unsigned long long) tv->tv_sec * 1000 +
              (unsigned long long) tv->tv_usec / 1000;
    for (;;) {
        ready = 0;
        if (rd != NULL) FD_ZERO(rd);
        ip_poll();
        for (fd = 0; fd < n && fd < NFD; fd++) {
            struct ofile *o = fds[fd];
            int r;

            if (!FD_ISSET(fd, &want) || o == NULL) continue;
            r = o->kind == K_SOCK ? ip_tcp_ready()
              : o->kind == K_CON  ? line_at < line_len
              : 1;
            if (r) {
                FD_SET(fd, rd);
                ready++;
            }
        }
        if (wr != NULL) {
            for (fd = 0; fd < n && fd < NFD; fd++)
                if (FD_ISSET(fd, wr)) ready++;
        }
        if (ready > 0 || (tv != NULL && bb_ms() >= end)) return ready;
        card_break();
        card_pause();
    }
}

/* ------------------------------------------------------------------ */
/* time                                                               */
/* ------------------------------------------------------------------ */

clock_t clock(void)
{
    return (clock_t) (bb_ms() / (1000 / CLOCKS_PER_SEC));
}

/* The time of day if a clock has it, else the seconds since barebox
   started: time moves either way, which is what the timeouts need. */
time_t time(time_t *t)
{
    static long long base;
    static int       have_base;
    time_t v;

#ifdef GORK_GRUB
    /* GRUB's milliseconds come from a TSC it calibrated at boot, which
       in an emulator on a busy host can be several times off; the
       firmware's clock (EFI GetTime) keeps real seconds. */
    {
        long long now = bb_epoch();

        if (now > 0) {
            if (t != NULL) *t = (time_t) now;
            return (time_t) now;
        }
    }
#endif
    if (!have_base) {           /* negative, with no clock: still the base */
        base = bb_epoch() - (long long) (bb_ms() / 1000);
        have_base = 1;
    }
    v = (time_t) (base + (long long) (bb_ms() / 1000));
    if (t != NULL) *t = v;
    return v;
}

unsigned sleep(unsigned secs)
{
    unsigned long long end = bb_ms() + (unsigned long long) secs * 1000;

    while (bb_ms() < end) {
        ip_poll();
        card_break();
        bb_udelay(10000);
    }
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    unsigned long long ms = bb_ms();

    (void) tz;
    tv->tv_sec = (long) time(NULL);
    tv->tv_usec = (long) (ms % 1000) * 1000;
    return 0;
}

pid_t getpid(void) { return 1; }

/* ------------------------------------------------------------------ */
/* signals: none; Ctrl-C is checked where gork waits (card_break)     */
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
    if (sig == SIGINT || sig == SIGTERM) exit(128 + sig);
    return 0;
}

int kill(pid_t pid, int sig)
{
    (void) pid;
    return raise(sig);
}

/* ------------------------------------------------------------------ */
/* paths: /dir/file, absolute by the text alone                       */
/* ------------------------------------------------------------------ */

char *getcwd(char *buf, size_t size)
{
    const char *c = bb_getcwd();

    if (c == NULL) c = "/";
    if (strlen(c) + 1 > size) {
        errno = ERANGE;
        return NULL;
    }
    return strcpy(buf, c);
}

int chdir(const char *path)
{
    long r = bb_chdir(path);

    return r < 0 ? (int) fail(r) : 0;
}

/* the device of a path, as GRUB names one: "(hd0,gpt1)" in front, or
   nothing (barebox's paths have none): its length */
static size_t device_len(const char *p)
{
    const char *e;

    if (p[0] != '(' || (e = strchr(p, ')')) == NULL) return 0;
    return (size_t) (e + 1 - p);
}

/* path made absolute against the current directory, with "." and ".."
   taken out, into out[PATH_MAX]: 0, or -1 with errno.  A path that starts
   with a device is absolute; one that starts with "/" is on the current
   directory's device. */
static int full_path(const char *path, char *out)
{
    char   tmp[PATH_MAX];
    char  *seg[PATH_MAX / 2];
    size_t n = 0, k, dev;
    char  *p, *s;

    if (device_len(path) > 0) {
        if (strlen(path) >= sizeof tmp) goto toolong;
        strcpy(tmp, path);
    } else {
        if (getcwd(tmp, sizeof tmp) == NULL) return -1;
        if (path[0] == '/') tmp[device_len(tmp)] = '\0';
        if (strlen(tmp) + 1 + strlen(path) >= sizeof tmp) goto toolong;
        if (path[0] != '/') strcat(tmp, "/");
        strcat(tmp, path);
    }
    dev = device_len(tmp);
    memcpy(out, tmp, dev);
    out[dev] = '\0';
    for (p = tmp + dev; (s = strtok(p, "/")) != NULL; p = NULL) {
        if (strcmp(s, ".") == 0) continue;
        if (strcmp(s, "..") == 0) {
            if (n > 0) n--;
            continue;
        }
        seg[n++] = s;
    }
    for (k = 0; k < n; k++) {
        strcat(out, "/");
        strcat(out, seg[k]);
    }
    if (n == 0) strcat(out, "/");
    return 0;
toolong:
    errno = ENAMETOOLONG;
    return -1;
}

char *realpath(const char *path, char *out)
{
    char full[PATH_MAX];
    struct stat st;

    if (full_path(path, full) != 0) return NULL;
    if (stat(full, &st) != 0) return NULL;      /* as realpath: it exists */
    if (out == NULL && (out = malloc(strlen(full) + 1)) == NULL) return NULL;
    return strcpy(out, full);
}

/* ------------------------------------------------------------------ */
/* files by name                                                      */
/* ------------------------------------------------------------------ */

static int do_stat(const char *path, int follow, struct stat *st)
{
    int       isdir;
    long long size;
    long      r = bb_stat(path, follow, &isdir, &size);

    if (r < 0) return (int) fail(r);
    memset(st, 0, sizeof *st);
    st->st_mode = (isdir ? S_IFDIR | 0755 : S_IFREG | 0644);
    st->st_size = isdir ? 0 : size;
    st->st_nlink = 1;
    return 0;
}

int stat(const char *path, struct stat *st) { return do_stat(path, 1, st); }
int lstat(const char *path, struct stat *st) { return do_stat(path, 0, st); }

int fstat(int fd, struct stat *st)
{
    struct ofile *o = get_fd(fd);
    long long at, end;

    if (o == NULL) return -1;
    memset(st, 0, sizeof *st);
    st->st_nlink = 1;
    if (o->kind != K_FILE) {
        st->st_mode = S_IFCHR | 0600;
        return 0;
    }
    st->st_mode = S_IFREG | 0644;
    at = bb_lseek(o->bfd, 0, SEEK_CUR);
    end = bb_lseek(o->bfd, 0, SEEK_END);
    if (at >= 0) bb_lseek(o->bfd, at, SEEK_SET);
    st->st_size = end > 0 ? end : 0;
    return 0;
}

int access(const char *path, int mode)
{
    struct stat st;

    (void) mode;
    return stat(path, &st);
}

/* barebox keeps no permissions: these succeed and change nothing */
int chmod(const char *path, mode_t mode)
{
    struct stat st;

    (void) mode;
    return stat(path, &st);
}

int fchmod(int fd, mode_t mode)
{
    (void) mode;
    return get_fd(fd) == NULL ? -1 : 0;
}

mode_t umask(mode_t m)
{
    (void) m;
    return 022;
}

int mkdir(const char *path, mode_t mode)
{
    long r = bb_mkdir(path);

    (void) mode;
    return r < 0 ? (int) fail(r) : 0;
}

int unlink(const char *path)
{
    long r = bb_unlink(path);

    return r < 0 ? (int) fail(r) : 0;
}

int rmdir(const char *path)
{
    long r = bb_rmdir(path);

    return r < 0 ? (int) fail(r) : 0;
}

int remove(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0) return -1;
    return S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
}

int rename(const char *from, const char *to)
{
    long r;

    remove(to);                 /* as rename() replaces it */
    r = bb_rename(from, to);
    return r < 0 ? (int) fail(r) : 0;
}

/* In a directory in RAM: barebox's /tmp, GRUB's (gork)/. */
char *tmpnam(char *s)
{
    static char  name[L_tmpnam];
    static unsigned n;
    struct stat  st;

    if (s == NULL) s = name;
    do {
        sprintf(s, "%s/gork%u.tmp", bb_tmpdir(), n++);
    } while (stat(s, &st) == 0 && n < 10000);
    return s;
}

/* ------------------------------------------------------------------ */
/* directories                                                        */
/* ------------------------------------------------------------------ */

struct _DIR {
    void         *d;
    struct dirent e;
};

DIR *opendir(const char *path)
{
    DIR *d = malloc(sizeof *d);

    if (d == NULL) return NULL;
    if ((d->d = bb_opendir(path)) == NULL) {
        free(d);
        errno = ENOENT;
        return NULL;
    }
    return d;
}

struct dirent *readdir(DIR *d)
{
    const char *name = bb_readdir(d->d);

    if (name == NULL) return NULL;
    strncpy(d->e.d_name, name, sizeof d->e.d_name - 1);
    d->e.d_name[sizeof d->e.d_name - 1] = '\0';
    return &d->e;
}

int closedir(DIR *d)
{
    bb_closedir(d->d);
    free(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the environment: barebox's variables                               */
/* ------------------------------------------------------------------ */

/* HOME is the glue's unless set: barebox's /env, which `saveenv` keeps,
   or GRUB's $prefix, where grub.cfg is. */
char *getenv(const char *name)
{
    const char *v = bb_getenv(name);

    if ((v == NULL || *v == '\0') && strcmp(name, "HOME") == 0) v = bb_home();
    return v != NULL && *v != '\0' ? (char *) v : NULL;
}

int putenv(char *s)
{
    char  name[128];
    char *eq = strchr(s, '=');
    long  r;

    if (eq == NULL || (size_t) (eq - s) >= sizeof name) {
        errno = EINVAL;
        return -1;
    }
    memcpy(name, s, (size_t) (eq - s));
    name[eq - s] = '\0';
    r = bb_setenv(name, eq + 1);
    return r < 0 ? (int) fail(r) : 0;
}

/* ------------------------------------------------------------------ */
/* system(): barebox's shell runs the line                            */
/* ------------------------------------------------------------------ */

/* With descriptor 1 pointed elsewhere (run_command's file), what the
   command prints goes there: barebox's shell redirects nothing, so
   glue.c takes it from the console. */
int system(const char *cmd)
{
    char  *out = NULL;
    size_t len = 0;
    int    st;

    if (cmd == NULL) return 1;
    fflush(stdout);
    fflush(stderr);
    if (fds[1] != NULL && fds[1]->kind == K_CON)
        return bb_run(cmd, NULL, NULL);
    st = bb_run(cmd, &out, &len);
    if (out != NULL) {
        write(1, out, len);
        bb_free(out);
    }
    return st;
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

/* The command's end: what is open is closed and what was taken given
   back, and barebox's prompt comes again (glue.c). */
void exit(int status)
{
    int i;

    while (nat_exit > 0) at_exit[--nat_exit]();
    fflush(NULL);
    for (i = 3; i < NFD; i++)
        if (fds[i] != NULL) close(i);        /* a socket's connection too */
    free_all();
    bb_exit(status);
}

/* glue.c's way in: the descriptors and streams, then main(). */
int main(int argc, char **argv);

int gork_start(int argc, char **argv)
{
    console_init();
    fw_stdio_init();
    return main(argc, argv);
}
