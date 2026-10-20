/*
 * plan9.c -- what Plan 9's APE lacks: realpath.  Plan 9 has no symbolic
 * links, so the directory resolves through chdir and getcwd.  Built only
 * there, by hand: see docs/build/plan9.md.
 */
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "port.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* realpath: the path must exist. */
char *realpath(const char *path, char *out)
{
    char        cur[PATH_MAX], here[PATH_MAX];
    char       *slash, *base;
    const char *dir;
    struct stat st;
    int         ok;

    if (strlen(path) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcpy(cur, path);
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
