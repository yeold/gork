/*
 * aixps2.c -- what AIX PS/2 1.3 lacks: memmove, realpath and strtoul
 * in libc, waddnstr and hline in its (System V release 3) curses.
 * Built only there, by hand: see docs/BUILD.md.
 */
#include <curses.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

void *memmove(void *dst, const void *src, size_t n)
{
    char       *d = (char *) dst;
    const char *s = (const char *) src;

    if (d < s)
        while (n-- > 0) *d++ = *s++;
    else if (d > s) {
        d += n;
        s += n;
        while (n-- > 0) *--d = *--s;
    }
    return dst;
}

/* realpath: the path must exist.  Directories resolve through chdir and
   getcwd; for anything else the directory part does, and the last part is
   followed if it is a symbolic link (up to 8 deep). */
char *realpath(const char *path, char *out)
{
    char        here[PATH_MAX], dir[PATH_MAX], name[PATH_MAX], link[PATH_MAX];
    char       *slash;
    struct stat st;
    int         depth, n;

    if (strlen(path) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    if (getcwd(here, sizeof here) == NULL)
        return NULL;
    strcpy(name, path);
    for (depth = 0; depth < 8; depth++) {
        if (lstat(name, &st) != 0)
            return NULL;
        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            if (chdir(name) != 0)
                return NULL;
            n = getcwd(out, PATH_MAX) != NULL;
            chdir(here);
            return n ? out : NULL;
        }
        slash = strrchr(name, '/');
        if (slash == NULL)
            strcpy(dir, ".");
        else if (slash == name)
            strcpy(dir, "/");
        else {
            memcpy(dir, name, (size_t) (slash - name));
            dir[slash - name] = '\0';
        }
        if ((st.st_mode & S_IFMT) == S_IFLNK) {
            n = readlink(name, link, sizeof link - 1);
            if (n < 0)
                return NULL;
            link[n] = '\0';
            if (link[0] == '/' || slash == NULL)
                strcpy(name, link);
            else if (strlen(dir) + 1 + strlen(link) < sizeof name) {
                strcpy(name, dir);
                strcat(name, "/");
                strcat(name, link);
            } else {
                errno = ENAMETOOLONG;
                return NULL;
            }
            continue;
        }
        if (chdir(dir) != 0)
            return NULL;
        n = getcwd(out, PATH_MAX) != NULL;
        chdir(here);
        if (!n)
            return NULL;
        slash = slash == NULL ? name : slash + 1;
        if (strlen(out) + 1 + strlen(slash) >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (strcmp(out, "/") != 0)
            strcat(out, "/");
        strcat(out, slash);
        return out;
    }
    errno = ELOOP;
    return NULL;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    const char   *p = s;
    unsigned long v = 0;
    int           d, any = 0;

    while (isspace((unsigned char) *p)) p++;
    if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0)
        base = *p == '0' ? 8 : 10;
    for (;; p++) {
        if (isdigit((unsigned char) *p)) d = *p - '0';
        else if (isalpha((unsigned char) *p)) d = tolower((unsigned char) *p) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        any = 1;
    }
    if (end != NULL) *end = (char *) (any ? p : s);
    return v;
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
    while (n-- > 0 && addch(ch) != ERR)
        ;
    move(y, x);
    return OK;
}
