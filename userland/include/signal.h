#ifndef SIGNAL_H
#define SIGNAL_H

/* Minimal signal numbers for UELF programs */
#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGABRT 6
#define SIGKILL 9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
/* Child status changed (POSIX) */
#define SIGCHLD 17

typedef void (*sighandler_t)(int);

/* Basic sigset_t for mask operations */
typedef unsigned long sigset_t;

/* Minimal sigaction structure */
struct sigaction {
	void (*sa_handler)(int);
	sigset_t sa_mask;
	int sa_flags;
};

/* Install a simple handler (BSD-style) - provided by libc. */
sighandler_t signal(int sig, sighandler_t handler);

/* POSIX-style sigaction: minimal implementation mapping to kernel signal syscall. */
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

/* sigset operations */
int sigemptyset(sigset_t *set);
int sigaddset(sigset_t *set, int signo);

#endif
