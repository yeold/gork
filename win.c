/*
 * win.c -- Windows only: the POSIX functions gork needs that MinGW's C
 * library (msvcrt) does not have.  See port.h.
 *
 * Only calls that Windows 95 and NT 4 already have, so that one gork.exe
 * runs on both lines.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"

static void slashes(char *p)
{
    for (; *p != '\0'; p++)
        if (*p == '\\') *p = '/';
}

char *win_realpath(const char *path, char *out)
{
    size_t n;

    if (_fullpath(out, path, MAX_PATH) == NULL) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    slashes(out);
    n = strlen(out);
    if (n > 3 && out[n - 1] == '/') out[n - 1] = '\0';     /* not "c:/" */
    if (GetFileAttributesA(out) == (DWORD) -1) {
        errno = ENOENT;
        return NULL;
    }
    return out;
}

char *win_tmpnam(char *out)
{
    char dir[MAX_PATH];
    DWORD n = GetTempPathA(sizeof dir, dir);

    /* GetTempFileName wants room for its 14-character name. */
    if (n == 0 || n > sizeof dir - 14 || GetTempFileNameA(dir, "grk", 0, out) == 0)
        return NULL;
    DeleteFileA(out);           /* it made the file; callers make their own */
    return out;
}

void win_sleep(int secs)
{
    Sleep((DWORD) secs * 1000);
}

int win_home(char *out, size_t size)
{
    const char *p = getenv("USERPROFILE");
    char        exe[MAX_PATH], *s;

    if (p == NULL || *p == '\0') {
        if (GetModuleFileNameA(NULL, exe, sizeof exe) == 0) return -1;
        s = strrchr(exe, '\\');
        if (s == NULL) return -1;
        s[s == exe + 2] = '\0';         /* "c:\gork.exe": keep the slash */
        p = exe;
    }
    if (strlen(p) >= size) return -1;
    strcpy(out, p);
    slashes(out);
    return 0;
}
