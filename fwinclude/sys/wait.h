#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
/* system() returns the command's status as it is: see the port's libc.c */
#define WIFEXITED(s)   1
#define WEXITSTATUS(s) (s)
#define WIFSIGNALED(s) 0
#define WTERMSIG(s)    0
#endif
