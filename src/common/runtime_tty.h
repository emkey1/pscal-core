#pragma once

#include <stdbool.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pscalRuntimeFdIsInteractive(int fd);
bool pscalRuntimeStdinIsInteractive(void);
bool pscalRuntimeStdoutIsInteractive(void);
bool pscalRuntimeStderrIsInteractive(void);

bool pscalRuntimeFdHasRealTTY(int fd);
bool pscalRuntimeStdinHasRealTTY(void);
bool pscalRuntimeStdoutHasRealTTY(void);
bool pscalRuntimeStderrHasRealTTY(void);

int pscalRuntimeDetectWindowRows(void);
int pscalRuntimeDetectWindowCols(void);

/* Reclaim `fd`'s controlling terminal for the caller's own process group.
 * Safe to call unconditionally after waitpid() on any foreground child:
 * it is a no-op when `fd` has no real TTY, and it silently ignores
 * failures (e.g. ENOTTY when `fd` isn't a controlling terminal) since
 * exec()-style builtins are used plenty outside interactive contexts.
 * Needed because a child that took job-control ownership of the
 * terminal (setpgid + tcsetpgrp, as any interactive shell does) may
 * exit without giving it back, leaving the caller's next terminal read
 * permanently stuck. */
void pscalRuntimeReclaimForegroundTerminal(int fd);

#ifdef __cplusplus
}
#endif
