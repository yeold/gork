#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <sys/types.h>

struct stat {
    mode_t st_mode;
    off_t  st_size;
    time_t st_mtime;
    time_t st_atime;
    time_t st_ctime;
    int    st_nlink;
    uid_t  st_uid;
    gid_t  st_gid;
};

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISLNK(m) 0
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100

int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
int fchmod(int fd, mode_t mode);
mode_t umask(mode_t m);
#endif
