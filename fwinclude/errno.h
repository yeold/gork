#ifndef _ERRNO_H
#define _ERRNO_H
extern int errno;
#define EPERM         1
#define ENOENT        2
#define EINTR         4
#define EIO           5
#define EBADF         9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EEXIST       17
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define ENOTTY       25
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define EPIPE        32
#define ERANGE       34
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39
#define ENOTSOCK     88
#define EAFNOSUPPORT 97
#define ENETDOWN    100
#define ENETUNREACH 101
#define ECONNRESET  104
#define ETIMEDOUT   110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#endif
