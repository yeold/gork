/* gork's C library for UEFI (uefi/libc.c): see docs/build/uefi.md. */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#include <stddef.h>
__extension__ typedef long long ssize_t;
__extension__ typedef long long off_t;
__extension__ typedef long long time_t;
typedef unsigned int       mode_t;
typedef int                pid_t;
typedef unsigned int       uid_t;
typedef unsigned int       gid_t;
#endif
