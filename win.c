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
#include <stdio.h>
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

void win_own_console(void)
{
    HANDLE h = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);

    if (h != INVALID_HANDLE_VALUE) SetStdHandle(STD_OUTPUT_HANDLE, h);
}

/* Length of the UTF-8 sequence at p (2 to 4 bytes, at most `left`), with
   its code point in *cp; 0 if it is not one. */
static int utf8_seq(const unsigned char *p, size_t left, unsigned long *cp)
{
    int n, i;

    if (p[0] >= 0xC2 && p[0] <= 0xDF) { n = 2; *cp = p[0] & 0x1F; }
    else if (p[0] >= 0xE0 && p[0] <= 0xEF) { n = 3; *cp = p[0] & 0x0F; }
    else if (p[0] >= 0xF0 && p[0] <= 0xF4) { n = 4; *cp = p[0] & 0x07; }
    else return 0;
    if ((size_t) n > left) return 0;
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        *cp = *cp << 6 | (p[i] & 0x3F);
    }
    if ((n == 3 && *cp < 0x800) || (n == 4 && (*cp < 0x10000 || *cp > 0x10FFFF)))
        return 0;
    return n;
}

size_t win_utf8_to_console(char *s, size_t len, int cells)
{
    UINT          page = GetConsoleOutputCP();
    unsigned long cp;
    WCHAR         w[2];
    char          out[8];
    size_t        i = 0, o = 0;
    int           n, wn, m;

    if (page == 0) page = GetOEMCP();
    if (page == CP_UTF8) {
        if (!cells) return len;
        page = 20127;                   /* US-ASCII, with best fit */
    }
    while (i < len) {
        n = (unsigned char) s[i] < 0x80 ? 0
          : utf8_seq((const unsigned char *) s + i, len - i, &cp);
        if (n == 0) {                   /* ASCII, or not UTF-8: as it is */
            s[o++] = s[i++];
            continue;
        }
        if (cp < 0x10000) {
            w[0] = (WCHAR) cp;
            wn = 1;
        } else {
            w[0] = (WCHAR) (0xD800 + ((cp - 0x10000) >> 10));
            w[1] = (WCHAR) (0xDC00 + ((cp - 0x10000) & 0x3FF));
            wn = 2;
        }
        m = WideCharToMultiByte(page, 0, w, wn, out, sizeof out, NULL, NULL);
        if (m < 1 || m > n) {
            s[o++] = '?';
        } else {
            memcpy(s + o, out, (size_t) m);
            o += (size_t) m;
        }
        i += (size_t) n;
    }
    return o;
}

/* `s`, in code page `page`, as malloc'd UTF-8; NULL if that fails.  Windows
   95 cannot convert to UTF-8 itself, so the encoding is done here. */
static char *to_utf8(const char *s, UINT page)
{
    int            wn = MultiByteToWideChar(page, 0, s, -1, NULL, 0), i;
    WCHAR         *w;
    unsigned char *u, *p;
    unsigned long  c;

    if (wn < 1) return NULL;
    w = (WCHAR *) malloc((size_t) wn * sizeof *w);
    u = (unsigned char *) malloc((size_t) wn * 3 + 1);
    if (w == NULL || u == NULL ||
        MultiByteToWideChar(page, 0, s, -1, w, wn) < 1) {
        free(w);
        free(u);
        return NULL;
    }
    for (p = u, i = 0; i < wn && w[i] != 0; i++) {
        c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < wn &&
            w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
            *p++ = (unsigned char) (0xF0 | c >> 18);
            *p++ = (unsigned char) (0x80 | (c >> 12 & 0x3F));
            *p++ = (unsigned char) (0x80 | (c >> 6 & 0x3F));
            *p++ = (unsigned char) (0x80 | (c & 0x3F));
        } else if (c < 0x80) {
            *p++ = (unsigned char) c;
        } else if (c < 0x800) {
            *p++ = (unsigned char) (0xC0 | c >> 6);
            *p++ = (unsigned char) (0x80 | (c & 0x3F));
        } else {
            *p++ = (unsigned char) (0xE0 | c >> 12);
            *p++ = (unsigned char) (0x80 | (c >> 6 & 0x3F));
            *p++ = (unsigned char) (0x80 | (c & 0x3F));
        }
    }
    *p = '\0';
    free(w);
    return (char *) u;
}

/* 0: all ASCII; 1: valid UTF-8 with more than ASCII in it; -1: neither. */
static int text_kind(const char *s)
{
    const unsigned char *p = (const unsigned char *) s;
    size_t               len = strlen(s), i = 0;
    unsigned long        cp;
    int                  n, kind = 0;

    while (i < len) {
        if (p[i] < 0x80) {
            i++;
            continue;
        }
        n = utf8_seq(p + i, len - i, &cp);
        if (n == 0) return -1;
        kind = 1;
        i += (size_t) n;
    }
    return kind;
}

char *win_input_to_utf8(const char *s, int typed)
{
    if (text_kind(s) == 0) return NULL;
    return to_utf8(s, typed ? GetConsoleCP() : CP_ACP);
}

char *win_output_to_utf8(const char *s)
{
    if (text_kind(s) >= 0) return NULL;
    return to_utf8(s, GetOEMCP());
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

/* MinGW replaces msvcrt's strtod() with its own, and Debian's copy of that
   is compiled for the Pentium Pro: cJSON's first number was an illegal
   instruction on a Pentium.  This one, found first by the linker, goes
   through msvcrt's sscanf(). */
double strtod(const char *s, char **end)
{
    double d = 0;
    int    n = 0;

    if (sscanf(s, "%lf%n", &d, &n) < 1) n = 0;
    if (end != NULL) *end = (char *) s + n;
    return d;
}

/* MinGW's start-up code calls this to apply "runtime pseudo-relocations",
   of which gork has none: Makefile.w32 links with
   --disable-runtime-pseudo-reloc, so the linker would say if it had.  The
   library's own version is left out by defining it here, and with it the
   only two instructions in gork.exe that a 486 does not have (cmov:
   Debian ships that code built for the Pentium Pro). */
void _pei386_runtime_relocator(void)
{
}
