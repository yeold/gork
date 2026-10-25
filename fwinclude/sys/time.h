#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <sys/types.h>
struct timeval {
    long tv_sec;
    long tv_usec;
};
#define FD_SETSIZE 32
typedef struct { unsigned long bits; } fd_set;     /* long: 32 bits */
#define FD_ZERO(s)     ((s)->bits = 0)
#define FD_SET(fd, s)  ((s)->bits |= 1UL << (fd))
#define FD_CLR(fd, s)  ((s)->bits &= ~(1UL << (fd)))
#define FD_ISSET(fd, s) (((s)->bits >> (fd)) & 1)
int select(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv);
int gettimeofday(struct timeval *tv, void *tz);
#endif
