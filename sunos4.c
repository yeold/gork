/*
 * sunos4.c -- what SunOS 4.1 lacks: memmove, raise, strerror and strtoul,
 * a realloc that takes NULL and the POSIX getpgrp.  Built only there, by
 * hand: see docs/BUILD.md.
 */
#include <sys/types.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "port.h"
#undef realloc
#undef getpgrp

extern int   sys_nerr;
extern char *sys_errlist[];
extern int   getpgrp();

void *sunos4_realloc(void *p, size_t n)
{
    return p == NULL ? malloc(n) : realloc(p, n);
}

pid_t sunos4_getpgrp(void)
{
    return (pid_t) getpgrp(0);
}

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

int raise(int sig)
{
    return kill(getpid(), sig);
}

char *strerror(int errnum)
{
    static char buf[32];

    if (errnum >= 0 && errnum < sys_nerr)
        return sys_errlist[errnum];
    sprintf(buf, "Error %d", errnum);
    return buf;
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
