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

typedef void (*sighandler_t)(int);

// Install a signal handler for the calling process. Returns 0 on success.
sighandler_t signal(int sig, sighandler_t handler);

#endif
