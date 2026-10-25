#ifndef _SIGNAL_H
#define _SIGNAL_H
/* No signals on UEFI or barebox: these keep the code that sets them compiling. */
typedef int sig_atomic_t;
typedef unsigned long sigset_t;
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t) 0)
#define SIG_IGN ((sighandler_t) 1)
#define SIG_ERR ((sighandler_t) -1)
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGWINCH 28
struct sigaction {
    sighandler_t sa_handler;
    sigset_t     sa_mask;
    int          sa_flags;
};
sighandler_t signal(int sig, sighandler_t fn);
int sigaction(int sig, const struct sigaction *sa, struct sigaction *old);
int sigemptyset(sigset_t *s);
int raise(int sig);
int kill(pid_t pid, int sig);
#endif
