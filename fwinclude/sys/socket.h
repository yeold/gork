#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#include <sys/types.h>
/* TCP over IPv4 only: the firmware's TCP4 (uefi/net.c) or gork's own (ip.c). */
typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};
#define AF_INET     2
#define PF_INET     AF_INET
#define SOCK_STREAM 1
int     socket(int domain, int type, int proto);
int     connect(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t send(int fd, const void *p, size_t n, int flags);
ssize_t recv(int fd, void *p, size_t n, int flags);
int     shutdown(int fd, int how);
#endif
