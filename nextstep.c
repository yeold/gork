/*
 * nextstep.c -- what OPENSTEP 4.2 lacks.  With cc -posix its headers
 * declare the POSIX calls, but its library has only the 4.3BSD ones:
 * waitpid, sigaction, sigemptyset, setpgid, tcgetpgrp, tcsetpgrp and
 * putenv are made from wait4, signal and siginterrupt, setpgrp and ioctl
 * here.  Two library calls disagree with those headers: readdir returns
 * the 4.3BSD record (NeXTSTEP 3.3's the POSIX one), and realpath leaves a
 * relative path as it is.
 * NeXTSTEP 3.3's getwd, and its realpath with it, fail outright there
 * (getwd reads directories the 4.3BSD way, which -posix breaks), and
 * OPENSTEP 4.2 has no getcwd: the current directory is found here by
 * walking up "..", and a path that realpath fails on is resolved here.
 * Built only there, by hand: see docs/BUILD.md.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "port.h"
#undef readdir
#undef realpath

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

extern char *realpath(), *getwd();
extern int   readlink();

#ifndef SA_RESTART
#define SA_RESTART 0
#endif

extern int wait4(), setpgrp(), siginterrupt();
extern char **environ;

pid_t waitpid(pid_t pid, int *status, int options)
{
    return wait4(pid, status, options, (void *) 0);
}

int sigemptyset(sigset_t *set)
{
    *set = 0;
    return 0;
}

/* Only the handler and SA_RESTART, which is all gork asks for: without
   it, the signal interrupts a read instead of restarting it. */
int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    void (*prev)(int);

    if (act == NULL) {
        prev = signal(sig, SIG_DFL);
        signal(sig, prev);
    } else {
        prev = signal(sig, act->sa_handler);
        siginterrupt(sig, !(act->sa_flags & SA_RESTART));
    }
    if (prev == SIG_ERR)
        return -1;
    if (old != NULL) {
        memset(old, 0, sizeof *old);
        old->sa_handler = prev;
    }
    return 0;
}

/* POSIX's 0s mean "the caller" and "the same as pid"; setpgrp takes them
   literally. */
int setpgid(pid_t pid, pid_t pgid)
{
    if (pid == 0)
        pid = getpid();
    if (pgid == 0)
        pgid = pid;
    return setpgrp(pid, pgid);
}

pid_t tcgetpgrp(int fd)
{
    int pg;

    return ioctl(fd, TIOCGPGRP, &pg) < 0 ? -1 : pg;
}

int tcsetpgrp(int fd, pid_t pgrp)
{
    int pg = (int) pgrp;

    return ioctl(fd, TIOCSPGRP, &pg);
}

/* The string itself becomes part of the environment, as POSIX has it. */
int putenv(char *s)
{
    size_t n = strcspn(s, "="), i;
    char **ne;

    for (i = 0; environ[i] != NULL; i++)
        if (strncmp(environ[i], s, n) == 0 && environ[i][n] == '=') {
            environ[i] = s;
            return 0;
        }
    ne = (char **) malloc((i + 2) * sizeof *ne);
    if (ne == NULL)
        return -1;
    memcpy(ne, environ, i * sizeof *ne);
    ne[i] = s;
    ne[i + 1] = NULL;
    environ = ne;
    return 0;
}

/* OPENSTEP 4.2's readdir fills in a 4.3BSD struct direct, whose name sits
   four bytes earlier than in the struct dirent of the POSIX headers.
   NeXTSTEP 3.3's fills in its POSIX record, four bytes longer at the
   front.  Which one is told once, from where the first entry of "/",
   which is ".", has its name; a guess per record mistakes some POSIX
   records, whose inode numbers happen to read as a 4.3BSD header. */
struct bsd_direct {
    unsigned long  d_fileno;
    unsigned short d_reclen;
    unsigned short d_namlen;
    char           d_name[256];
};

struct posix33_dirent {
    unsigned long  d_off;
    unsigned long  d_fileno;
    unsigned short d_reclen;
    unsigned short d_namlen;
    char           d_name[256];
};

static int bsd_layout(void)
{
    static int     bsd = -1;
    DIR           *d;
    struct dirent *e;

    if (bsd < 0) {
        bsd = 1;
        if ((d = opendir("/")) != NULL) {
            e = readdir(d);
            if (e != NULL && memcmp(((char *) e) + 8, ".", 2) != 0 &&
                memcmp(((char *) e) + 12, ".", 2) == 0)
                bsd = 0;
            closedir(d);
        }
    }
    return bsd;
}

struct dirent *next_readdir(DIR *dir)
{
    static struct dirent   out;
    int                    bsd = bsd_layout();
    struct bsd_direct     *b = (struct bsd_direct *) readdir(dir);
    struct posix33_dirent *q = (struct posix33_dirent *) b;
    const char            *name;
    size_t                 n;

    if (b == NULL)
        return NULL;
    if (bsd) {
        name = b->d_name;
        n = b->d_namlen;
    } else {
        name = q->d_name;
        n = q->d_namlen < 256 ? q->d_namlen : 255;
    }
    if (n > sizeof out.d_name - 1)
        n = sizeof out.d_name - 1;
    memset(&out, 0, sizeof out);
    memcpy(out.d_name, name, n);
    return &out;
}

/* The current directory into out (MAXPATHLEN bytes): getwd's answer if
   it names this directory (NeXTSTEP 3.3's names some other one), or else
   the name of each directory looked up in its parent. */
static char *cwd(char *out)
{
    char           up[MAXPATHLEN], path[MAXPATHLEN], ent[MAXPATHLEN];
    struct stat    cur, par, root, st;
    struct dirent *e;
    DIR           *d;
    size_t         pos = sizeof path - 1, n;
    int            found;

    if (stat(".", &cur) != 0 || stat("/", &root) != 0)
        return NULL;
    if (getwd(out) != NULL && out[0] == '/' && stat(out, &st) == 0 &&
        st.st_dev == cur.st_dev && st.st_ino == cur.st_ino)
        return out;
    path[pos] = '\0';
    strcpy(up, "..");
    while (cur.st_dev != root.st_dev || cur.st_ino != root.st_ino) {
        if (stat(up, &par) != 0 || (d = opendir(up)) == NULL)
            return NULL;
        found = 0;
        while (!found && (e = next_readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
                strlen(up) + 1 + strlen(e->d_name) >= sizeof ent)
                continue;
            sprintf(ent, "%s/%s", up, e->d_name);
            if (lstat(ent, &st) == 0 && st.st_dev == cur.st_dev &&
                st.st_ino == cur.st_ino) {
                n = strlen(e->d_name);
                if (n + 1 > pos) {
                    closedir(d);
                    errno = ENAMETOOLONG;
                    return NULL;
                }
                pos -= n;
                memcpy(path + pos, e->d_name, n);
                path[--pos] = '/';
                found = 1;
            }
        }
        closedir(d);
        if (!found) {
            errno = ENOENT;
            return NULL;
        }
        if (strlen(up) + 3 >= sizeof up) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        strcat(up, "/..");
        cur = par;
    }
    strcpy(out, path[pos] == '\0' ? "/" : path + pos);
    return out;
}

/* realpath for an absolute path, where the library's fails (NeXTSTEP
   3.3 with -posix): symbolic links in the last part are followed here,
   then the directory resolves through chdir and cwd, which leave
   none. */
static char *own_realpath(const char *path, char *out)
{
    char        cur[MAXPATHLEN], link[MAXPATHLEN], here[MAXPATHLEN];
    char       *slash, *base;
    const char *dir;
    struct stat st;
    int         n, hops = 0, ok;

    if (strlen(path) >= MAXPATHLEN) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcpy(cur, path);
    while (lstat(cur, &st) == 0 && S_ISLNK(st.st_mode)) {
        if (++hops > 16) {
            errno = ELOOP;
            return NULL;
        }
        n = readlink(cur, link, sizeof link - 1);
        if (n < 0)
            return NULL;
        link[n] = '\0';
        slash = strrchr(cur, '/');
        if (link[0] == '/' || slash == NULL) {
            strcpy(cur, link);
        } else {
            if ((size_t) (slash + 1 - cur) + n >= MAXPATHLEN) {
                errno = ENAMETOOLONG;
                return NULL;
            }
            strcpy(slash + 1, link);
        }
    }
    if (stat(cur, &st) != 0 || cwd(here) == NULL)
        return NULL;
    dir = cur;
    base = NULL;
    if (!S_ISDIR(st.st_mode)) {
        slash = strrchr(cur, '/');
        *slash = '\0';
        base = slash + 1;
        if (cur[0] == '\0')
            dir = "/";
    }
    if (chdir(dir) != 0)
        return NULL;
    ok = cwd(out) != NULL;
    chdir(here);
    if (!ok)
        return NULL;
    if (base != NULL) {
        if (strlen(out) + 1 + strlen(base) >= MAXPATHLEN) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (strcmp(out, "/") != 0)
            strcat(out, "/");
        strcat(out, base);
    }
    return out;
}

/* The library's realpath resolves an absolute path (symbolic links and
   all) but hands back a relative one unchanged: make it absolute first. */
char *next_realpath(const char *path, char *out)
{
    char abs[MAXPATHLEN];

    if (path[0] != '/') {
        if (cwd(abs) == NULL)
            return NULL;
        if (strlen(abs) + 1 + strlen(path) >= sizeof abs) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (strcmp(abs, "/") != 0)
            strcat(abs, "/");
        strcat(abs, path);
        path = abs;
    }
    if (realpath(path, out) != NULL)
        return out;
    return own_realpath(path, out);
}
