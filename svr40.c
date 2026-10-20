/*
 * svr40.c -- what System V Release 4.0 for the 386 lacks: mkstemp.
 * Built only there, by hand: see docs/build/svr4.md.
 */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "port.h"

/* The trailing XXXXXX become the process id and a counter. */
int mkstemp(char *tmpl)
{
    static unsigned n;
    size_t          len = strlen(tmpl);
    char            tail[16];
    int             fd, tries;

    if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    for (tries = 0; tries < 100; tries++) {
        sprintf(tail, "%03u%03u", (unsigned) getpid() % 1000, n++ % 1000);
        memcpy(tmpl + len - 6, tail, 6);
        fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    return -1;
}
