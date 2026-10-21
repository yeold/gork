#ifndef _NETINET_IN_H
#define _NETINET_IN_H
#include <stdint.h>
#include <sys/socket.h>
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};
#define INADDR_ANY  ((in_addr_t) 0)
#define INADDR_NONE ((in_addr_t) 0xffffffff)
#define IPPROTO_TCP 6
#define htons(x) ((uint16_t) ((((x) & 0xff) << 8) | (((x) >> 8) & 0xff)))
#define ntohs(x) htons(x)
#define htonl(x) __builtin_bswap32(x)
#define ntohl(x) __builtin_bswap32(x)
#endif
