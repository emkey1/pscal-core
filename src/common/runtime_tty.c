#include "common/runtime_tty.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if defined(PSCAL_TARGET_IOS)
#include "runtime/vproc/vproc.h"
#endif

bool pscalRuntimeFdIsInteractive(int fd) {
    if (fd < 0) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    if (vprocIsattyShim(fd)) {
#else
    if (isatty(fd)) {
#endif
        return true;
    }
    return false;
}

bool pscalRuntimeStdinIsInteractive(void) {
    return pscalRuntimeFdIsInteractive(STDIN_FILENO);
}

bool pscalRuntimeStdoutIsInteractive(void) {
    return pscalRuntimeFdIsInteractive(STDOUT_FILENO);
}

bool pscalRuntimeStderrIsInteractive(void) {
    return pscalRuntimeFdIsInteractive(STDERR_FILENO);
}

bool pscalRuntimeFdHasRealTTY(int fd) {
    if (fd < 0) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (session && !vprocSessionStdioIsDefault(session)) {
            if (session->pty_active) {
                /* PTY-backed stdio supports termios even without a host TTY. */
                return true;
            }
            if (session->stdin_pscal_fd ||
                session->stdout_pscal_fd ||
                session->stderr_pscal_fd) {
                return false;
            }
        }
    }
#endif
    return isatty(fd) != 0;
}

bool pscalRuntimeStdinHasRealTTY(void) {
    return pscalRuntimeFdHasRealTTY(STDIN_FILENO);
}

bool pscalRuntimeStdoutHasRealTTY(void) {
    return pscalRuntimeFdHasRealTTY(STDOUT_FILENO);
}

bool pscalRuntimeStderrHasRealTTY(void) {
    return pscalRuntimeFdHasRealTTY(STDERR_FILENO);
}

void pscalRuntimeReclaimForegroundTerminal(int fd) {
#if defined(PSCAL_TARGET_IOS)
    /* iOS has no real fork/exec, so no child can ever steal the (virtual)
     * terminal's foreground process group out from under us here. */
    (void)fd;
#else
    if (!pscalRuntimeFdHasRealTTY(fd)) {
        return;
    }

    /* tcsetpgrp() itself delivers SIGTTOU to the caller if the caller is
     * currently in a background process group with respect to `fd' --
     * exactly the state we're in right after a child has taken the
     * terminal for its own process group and exited without giving it
     * back. Ignore SIGTTOU/SIGTTIN for the duration of the call so we
     * can't be stopped by the very syscall meant to fix that. */
    struct sigaction ignore_action, saved_ttou, saved_ttin;
    memset(&ignore_action, 0, sizeof(ignore_action));
    sigemptyset(&ignore_action.sa_mask);
    ignore_action.sa_handler = SIG_IGN;
    if (sigaction(SIGTTOU, &ignore_action, &saved_ttou) != 0) {
        return;
    }
    if (sigaction(SIGTTIN, &ignore_action, &saved_ttin) != 0) {
        sigaction(SIGTTOU, &saved_ttou, NULL);
        return;
    }

    pid_t pgrp = getpgrp();
    while (tcsetpgrp(fd, pgrp) != 0 && errno == EINTR) {
        /* retry */
    }
    /* Any other failure (e.g. ENOTTY if `fd' isn't our controlling
     * terminal) is expected in non-interactive contexts and is silently
     * ignored -- exec()-style builtins run outside interactive sessions
     * plenty of the time. */

    sigaction(SIGTTOU, &saved_ttou, NULL);
    sigaction(SIGTTIN, &saved_ttin, NULL);
#endif
}

static int pscalRuntimeParseEnvInt(const char *name) {
    const char *value = getenv(name);
    if (!value || !*value) {
        return -1;
    }
    char *endptr = NULL;
    long parsed = strtol(value, &endptr, 10);
    if (!endptr || *endptr != '\0' || parsed <= 0 || parsed > 1000) {
        return -1;
    }
    return (int)parsed;
}

static int pscalRuntimeDetectDimension(bool rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        unsigned short value = rows ? ws.ws_row : ws.ws_col;
        if (value > 0) {
            return (int)value;
        }
    }
    int envValue = pscalRuntimeParseEnvInt(rows ? "LINES" : "COLUMNS");
    if (envValue > 0) {
        return envValue;
    }
    return rows ? 24 : 80;
}

int pscalRuntimeDetectWindowRows(void) {
    return pscalRuntimeDetectDimension(true);
}

int pscalRuntimeDetectWindowCols(void) {
    return pscalRuntimeDetectDimension(false);
}
