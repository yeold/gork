/*
 * nextstep.c -- what OPENSTEP 4.2 lacks.  With cc -posix its headers
 * declare the POSIX calls, but its library has only the 4.3BSD ones:
 * waitpid, sigaction, sigemptyset, setpgid, tcgetpgrp, tcsetpgrp and
 * putenv are made from wait4, signal and siginterrupt, setpgrp and ioctl
 * here.  Two library calls disagree with those headers: readdir returns
 * the 4.3BSD record, and realpath leaves a relative path as it is.
 * Built only there, by hand: see docs/BUILD.md.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
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

/* The library's readdir fills in a 4.3BSD struct direct, whose name sits
   four bytes earlier than in the struct dirent of the POSIX headers. */
struct bsd_direct {
    unsigned long  d_fileno;
    unsigned short d_reclen;
    unsigned short d_namlen;
    char           d_name[256];
};

struct dirent *next_readdir(DIR *dir)
{
    static struct dirent out;
    struct bsd_direct   *b = (struct bsd_direct *) readdir(dir);
    size_t               n;

    if (b == NULL)
        return NULL;
    n = b->d_namlen < sizeof out.d_name ? b->d_namlen : sizeof out.d_name - 1;
    memset(&out, 0, sizeof out);
    memcpy(out.d_name, b->d_name, n);
    return &out;
}

/* The library's realpath resolves an absolute path (symbolic links and
   all) but hands back a relative one unchanged: make it absolute first. */
char *next_realpath(const char *path, char *out)
{
    char abs[MAXPATHLEN];

    if (path[0] != '/') {
        if (getwd(abs) == NULL)
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
    return realpath(path, out);
}
