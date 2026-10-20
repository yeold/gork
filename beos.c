/*
 * beos.c -- what BeOS R5 lacks: realpath in libroot, and a select() that
 * takes pipes (its net_server one takes only sockets), so a pipe is read
 * with read_timed(), which polls it.  Built only there, by hand: see
 * docs/build/beos.md.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <OS.h>
#include "port.h"

/* realpath: the path must exist.  Symbolic links in the last part are
   followed here; the directory then resolves through chdir and getcwd,
   which leave none. */
char *realpath(const char *path, char *out)
{
    char        cur[PATH_MAX], link[PATH_MAX], here[PATH_MAX];
    char       *slash, *base;
    const char *dir;
    struct stat st;
    int         n, hops = 0, ok;

    if (strlen(path) >= PATH_MAX) {
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
            if ((size_t) (slash + 1 - cur) + n >= PATH_MAX) {
                errno = ENAMETOOLONG;
                return NULL;
            }
            strcpy(slash + 1, link);
        }
    }
    if (stat(cur, &st) != 0 || getcwd(here, sizeof here) == NULL)
        return NULL;
    dir = cur;
    base = NULL;
    if (!S_ISDIR(st.st_mode)) {
        slash = strrchr(cur, '/');
        if (slash == NULL) {
            dir = ".";
            base = cur;
        } else {
            *slash = '\0';
            base = slash + 1;
            if (cur[0] == '\0')
                dir = "/";
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

/* read() from a pipe, waiting at most 200 ms (callers pass at least a
   second in `secs`), so that the caller can look for Esc.  With nothing to
   read by then: -1 with EINTR, as a read cut short by an alarm. */
int read_timed(int fd, char *p, unsigned n, long secs)
{
    bigtime_t end = system_time() + 200000;
    int       fl = fcntl(fd, F_GETFL), r, e;

    (void) secs;
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    for (;;) {
        r = read(fd, p, n);
        if (r >= 0 || (errno != EWOULDBLOCK && errno != EAGAIN)) break;
        if (system_time() >= end) {
            errno = EINTR;
            break;
        }
        snooze(20000);
    }
    e = errno;
    fcntl(fd, F_SETFL, fl);
    errno = e;
    return r;
}
