#ifndef _UNISTD_H
#define _UNISTD_H
#include <sys/types.h>

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t  read(int fd, void *p, size_t n);
ssize_t  write(int fd, const void *p, size_t n);
int      close(int fd);
off_t    lseek(int fd, off_t off, int whence);
int      fsync(int fd);
int      unlink(const char *path);
int      rmdir(const char *path);
int      access(const char *path, int mode);
char    *getcwd(char *buf, size_t size);
int      chdir(const char *path);
int      isatty(int fd);
int      dup(int fd);
int      dup2(int fd, int to);
unsigned sleep(unsigned secs);
pid_t    getpid(void);
#endif
