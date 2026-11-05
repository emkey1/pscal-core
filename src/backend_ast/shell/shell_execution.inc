/* Auto-generated include: command execution and pipeline management. Included from shell.c. */

#include "runtime/vproc/vproc.h"
#include "backend_ast/builtin.h"
#if defined(PSCAL_TARGET_IOS)
#include "smallclue/smallclue.h"
#include "common/path_virtualization.h"
#include "runtime/vproc/tty/pscal_fd.h"
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#define PIPE_SESSION(ctx) (&(ctx)->session)
#define PIPE_SPOOL_FD(ctx) ((ctx)->session.spool_fd)
#define PIPE_SPOOL_SIZE(ctx) ((ctx)->session.spool_size)
#define PIPE_SPOOL_OWNED(ctx) ((ctx)->session.spool_owned)
#define PIPE_STDOUT_FD(ctx) ((ctx)->session.stdout_fd)
#define PIPE_STDERR_FD(ctx) ((ctx)->session.stderr_fd)
#define PIPE_STDIN_FD(ctx) ((ctx)->session.stdin_fd)
#define PIPE_STDOUT_OWNED(ctx) ((ctx)->session.stdout_owned)
#define PIPE_STDERR_OWNED(ctx) ((ctx)->session.stderr_owned)
#define PIPE_STDIN_OWNED(ctx) ((ctx)->session.stdin_owned)

static int gShellPagerVirtualTTYFd = -1;

static bool shellPathHasShebang(const char *path) {
    if (!path || !*path) {
        return false;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char header[2];
    ssize_t n = read(fd, header, sizeof(header));
    close(fd);
    return n == 2 && header[0] == '#' && header[1] == '!';
}

static bool shellPathProbeExecutable(const char *path, char *display, size_t display_size) {
    if (!path || !*path || !display || display_size == 0) {
        return false;
    }

    const char *probes[4];
    size_t probe_count = 0;
    probes[probe_count++] = path;

#if defined(PSCAL_TARGET_IOS)
    char expanded[PATH_MAX];
    char stripped[PATH_MAX];
    if (path[0] == '/' && pathTruncateExpand(path, expanded, sizeof(expanded)) &&
        strcmp(expanded, path) != 0) {
        probes[probe_count++] = expanded;
    }
    if (path[0] == '/' && pathTruncateStrip(path, stripped, sizeof(stripped)) &&
        strcmp(stripped, path) != 0) {
        probes[probe_count++] = stripped;
        char stripped_expanded[PATH_MAX];
        if (pathTruncateExpand(stripped, stripped_expanded, sizeof(stripped_expanded)) &&
            strcmp(stripped_expanded, stripped) != 0) {
            probes[probe_count++] = stripped_expanded;
        }
    }
#endif

    for (size_t i = 0; i < probe_count; ++i) {
        const char *candidate = probes[i];
        if (!candidate || !*candidate) {
            continue;
        }
        struct stat st;
        if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        bool runnable = (access(candidate, X_OK) == 0);
#if defined(PSCAL_TARGET_IOS)
        if (!runnable && (st.st_mode & 0111) != 0) {
            runnable = true;
        }
        if (!runnable && access(candidate, R_OK) == 0 && shellPathHasShebang(candidate)) {
            runnable = true;
        }
#endif
        if (!runnable) {
            continue;
        }
#if defined(PSCAL_TARGET_IOS)
        if (candidate[0] == '/' && pathTruncateStrip(candidate, display, display_size)) {
            return true;
        }
#endif
        int copied = snprintf(display, display_size, "%s", candidate);
        return copied >= 0 && (size_t)copied < display_size;
    }
    return false;
}

static bool shellResolveExecutableCandidate(const char *candidate, char *resolved, size_t resolved_size) {
    if (!candidate || !resolved || resolved_size == 0) {
        return false;
    }
    return shellPathProbeExecutable(candidate, resolved, resolved_size);
}

/* Resolve bare command names against PATH for in-process shebang execution. */
static char *shellFindExecutableOnPath(const char *name) {
    if (!name || strchr(name, '/')) {
        return NULL;
    }
    char *path_value = shellLookupParameterValue("PATH", strlen("PATH"));
    const char *path = (path_value && *path_value) ? path_value : getenv("PATH");
    char candidate[PATH_MAX];
    if (path) {
        const char *cursor = path;
        while (*cursor) {
            const char *next = strchr(cursor, ':');
            size_t dir_len = next ? (size_t)(next - cursor) : strlen(cursor);
            const char *dir = cursor;
            char dir_buf[PATH_MAX];
            if (dir_len == 0) {
                dir = ".";
                dir_len = 1;
            } else {
                if (dir_len >= sizeof(dir_buf)) {
                    if (!next) {
                        break;
                    }
                    cursor = next + 1;
                    continue;
                }
                memcpy(dir_buf, cursor, dir_len);
                dir_buf[dir_len] = '\0';
                dir = dir_buf;
            }
            int len = snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
            char resolved[PATH_MAX];
            if (len > 0 && (size_t)len < sizeof(candidate) &&
                shellResolveExecutableCandidate(candidate, resolved, sizeof(resolved))) {
                char *out = strdup(resolved);
                free(path_value);
                return out;
            }
            if (!next) {
                break;
            }
            cursor = next + 1;
        }
    }
#if defined(PSCAL_TARGET_IOS)
    /* Keep fallback probing in app-root namespace instead of device-root paths. */
    const char *fallback_dirs[] = { "/bin", "/Documents/bin" };
    for (size_t i = 0; i < sizeof(fallback_dirs) / sizeof(fallback_dirs[0]); ++i) {
        int len = snprintf(candidate, sizeof(candidate), "%s/%s", fallback_dirs[i], name);
        char resolved[PATH_MAX];
        if (len > 0 && (size_t)len < sizeof(candidate) &&
            shellResolveExecutableCandidate(candidate, resolved, sizeof(resolved))) {
            free(path_value);
            return strdup(resolved);
        }
    }
#endif
    free(path_value);
    return NULL;
}

static bool shellResolveExecutionPath(const char *name, char *resolved, size_t resolved_size) {
    if (!name || !*name || !resolved || resolved_size == 0) {
        return false;
    }
    if (strchr(name, '/')) {
        return shellResolveExecutableCandidate(name, resolved, resolved_size);
    }
    char *found = shellFindExecutableOnPath(name);
    if (!found) {
        return false;
    }
    int copied = snprintf(resolved, resolved_size, "%s", found);
    free(found);
    return copied >= 0 && (size_t)copied < resolved_size;
}

static void __attribute__((unused)) shellPagerPushVirtualTTYOverride(int fd) {
    if (gShellPagerVirtualTTYFd >= 0) {
        close(gShellPagerVirtualTTYFd);
    }
    gShellPagerVirtualTTYFd = fd;
}

static void __attribute__((unused)) shellPagerPopVirtualTTYOverride(void) {
    if (gShellPagerVirtualTTYFd >= 0) {
        close(gShellPagerVirtualTTYFd);
        gShellPagerVirtualTTYFd = -1;
    }
}

static int __attribute__((unused)) shellPagerGetVirtualTTYOverride(void) {
    return gShellPagerVirtualTTYFd;
}

static void shellVprocFormatLabel(const ShellCommand *cmd, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (!cmd || cmd->argc == 0 || !cmd->argv) {
        return;
    }
    size_t used = 0;
    for (size_t i = 0; i < cmd->argc && used + 1 < out_sz; ++i) {
        const char *part = cmd->argv[i] ? cmd->argv[i] : "";
        size_t len = strlen(part);
        if (used + len + 1 >= out_sz) {
            len = out_sz - used - 1;
        }
        memcpy(out + used, part, len);
        used += len;
        if (used + 1 < out_sz && i + 1 < cmd->argc) {
            out[used++] = ' ';
        }
    }
    out[used] = '\0';
}

typedef struct {
    int pid;
    bool have_prev;
    char prev_label[64];
} ShellVprocLabelScope;

static void shellVprocLabelScopeBegin(ShellVprocLabelScope *scope, const ShellCommand *cmd) {
    if (!scope) {
        return;
    }
    scope->pid = -1;
    scope->have_prev = false;
    scope->prev_label[0] = '\0';

    VProc *current_vp = vprocCurrent();
    if (!current_vp) {
        return;
    }
    int label_pid = vprocPid(current_vp);
    if (label_pid <= 0) {
        return;
    }
    if (label_pid == vprocGetShellSelfPid()) {
        return;
    }

    if (!vprocGetCommandLabel(label_pid, scope->prev_label, sizeof(scope->prev_label))) {
        return;
    }

    char label[96];
    shellVprocFormatLabel(cmd, label, sizeof(label));
    if (!label[0]) {
        return;
    }

    scope->pid = label_pid;
    scope->have_prev = true;
    vprocSetCommandLabel(label_pid, label);
}

static void shellVprocLabelScopeEnd(ShellVprocLabelScope *scope) {
    if (!scope || scope->pid <= 0) {
        return;
    }
    if (scope->have_prev) {
        vprocSetCommandLabel(scope->pid, scope->prev_label);
    }
    scope->pid = -1;
    scope->have_prev = false;
    scope->prev_label[0] = '\0';
}

static ShellThreadLink shellThreadLinkCapture(void) {
    ShellThreadLink link;
    memset(&link, 0, sizeof(link));
    link.return_scope = gShellReturnScopeDepth + gShellThreadReturnScope;
    link.interactive = gShellInteractiveMode;
    if (!gShellRuntime.in_subshell) {
        return link;
    }
    link.ctx = shellRuntimeCreateContext();
    if (!link.ctx) {
        return link;
    }
    link.env = vprocEnvRetain(vprocEnvCurrent());
    link.subshell_pid = vprocSubshellPid();
    return link;
}

static void shellThreadLinkEnter(const ShellThreadLink *link) {
    gShellThreadReturnScope = link ? link->return_scope : 0;
    if (link) {
        gShellInteractiveMode = link->interactive;
    }
    if (link && link->ctx) {
        vprocEnvActivate(link->env);
        vprocSetSubshellPid(link->subshell_pid);
    }
}

/* Frees what capture took. The context is the caller's: it may have been activated. */
static void shellThreadLinkRelease(ShellThreadLink *link) {
    if (!link) {
        return;
    }
    if (link->env) {
        vprocEnvActivate(NULL);
        vprocSetSubshellPid(0);
    }
    vprocEnvRelease(link->env);
    link->env = NULL;
}

static VProc *shellMakeStageVProc(int stdin_host,
                                  int stdout_host,
                                  int stderr_host,
                                  int cols,
                                  int rows,
                                  int pid_hint,
                                  bool inherit_pscal_stdio) {
    (void)vprocEnsureKernelPid();
    /* Optionally inherit stdio from the active vproc; never fall back to the
     * interactive session if the caller disables inheritance. Background workers
     * must not mutate the interactive session's fds. */
    bool stdin_force_null = (stdin_host == -2);
    bool stdin_use_pscal = false;
    bool stdout_use_pscal = false;
    bool stderr_use_pscal = false;
    /* What the starter closed stays closed. vprocCreate would take the host's descriptor of that
     * number, and `{ ls; } >&-` would write to the process's stdout. */
    bool stdin_closed = false;
    bool stdout_closed = false;
    bool stderr_closed = false;
    struct pscal_fd *pscal_stdin = NULL;
    struct pscal_fd *pscal_stdout = NULL;
    struct pscal_fd *pscal_stderr = NULL;
    VProc *active = inherit_pscal_stdio ? vprocCurrent() : NULL;
    VProcSessionStdio *session_stdio = inherit_pscal_stdio ? vprocSessionStdioCurrent() : NULL;
    if (active) {
        if (!stdin_force_null && stdin_host < 0) {
            int translated = vprocTranslateFd(active, STDIN_FILENO);
            if (translated >= 0) {
                stdin_host = translated;
            } else {
                pscal_stdin = vprocGetPscalFd(active, STDIN_FILENO);
                stdin_use_pscal = (pscal_stdin != NULL);
                stdin_closed = !stdin_use_pscal;
            }
        }
        if (stdout_host < 0) {
            int translated = vprocTranslateFd(active, STDOUT_FILENO);
            if (translated >= 0) {
                stdout_host = translated;
            } else {
                pscal_stdout = vprocGetPscalFd(active, STDOUT_FILENO);
                stdout_use_pscal = (pscal_stdout != NULL);
                stdout_closed = !stdout_use_pscal;
            }
        }
        if (stderr_host < 0) {
            int translated = vprocTranslateFd(active, STDERR_FILENO);
            if (translated >= 0) {
                stderr_host = translated;
            } else {
                pscal_stderr = vprocGetPscalFd(active, STDERR_FILENO);
                stderr_use_pscal = (pscal_stderr != NULL);
                stderr_closed = !stderr_use_pscal;
            }
        }
    } else if (session_stdio) {
        if (!stdin_force_null && stdin_host < 0 && session_stdio->stdin_pscal_fd) {
            pscal_stdin = pscal_fd_retain(session_stdio->stdin_pscal_fd);
            stdin_use_pscal = (pscal_stdin != NULL);
        }
        if (stdout_host < 0 && session_stdio->stdout_pscal_fd) {
            pscal_stdout = pscal_fd_retain(session_stdio->stdout_pscal_fd);
            stdout_use_pscal = (pscal_stdout != NULL);
        }
        if (stderr_host < 0 && session_stdio->stderr_pscal_fd) {
            pscal_stderr = pscal_fd_retain(session_stdio->stderr_pscal_fd);
            stderr_use_pscal = (pscal_stderr != NULL);
        }
    }
    if (!stdin_force_null && stdin_host < 0 && !stdin_use_pscal) {
        stdin_host = STDIN_FILENO;
    }
    if (stdin_use_pscal && stdin_host < 0) {
        stdin_host = -2;
    }
    if (stdout_use_pscal && stdout_host < 0) {
        stdout_host = -2;
    }
    if (stderr_use_pscal && stderr_host < 0) {
        stderr_host = -2;
    }
    bool tool_dbg = getenv("PSCALI_TOOL_DEBUG") != NULL;
    if (tool_dbg) {
        fprintf(stderr,
                "[vproc-stage] stdin_host=%d stdout_host=%d stderr_host=%d force_null=%d pscal=%d/%d/%d\n",
                stdin_host, stdout_host, stderr_host, (int)stdin_force_null,
                (int)stdin_use_pscal, (int)stdout_use_pscal, (int)stderr_use_pscal);
    }
    VProcOptions opts = vprocDefaultOptions();
    opts.stdin_fd = (stdin_host >= 0) ? stdin_host : -2;   /* default to /dev/null */
    opts.stdout_fd = (stdout_host >= 0) ? stdout_host : stdout_host;
    opts.stderr_fd = (stderr_host >= 0) ? stderr_host : stderr_host;
    opts.winsize_cols = cols;
    opts.winsize_rows = rows;
    opts.pid_hint = pid_hint;
    if (pid_hint > 0) {
        int existing_job_id = vprocGetJobId(pid_hint);
        if (existing_job_id > 0) {
            opts.job_id = existing_job_id;
        }
    }
    VProc *vp = vprocCreate(&opts);
    if (!vp) {
        if (pscal_stdin) pscal_fd_close(pscal_stdin);
        if (pscal_stdout) pscal_fd_close(pscal_stdout);
        if (pscal_stderr) pscal_fd_close(pscal_stderr);
        return NULL;
    }
    if (stdin_use_pscal && pscal_stdin) {
        if (vprocAdoptPscalFd(vp, STDIN_FILENO, pscal_stdin) != 0 && tool_dbg) {
            fprintf(stderr, "[vproc-stage] adopt pscal stdin failed: %s\n", strerror(errno));
        }
    }
    if (stdout_use_pscal && pscal_stdout) {
        if (vprocAdoptPscalFd(vp, STDOUT_FILENO, pscal_stdout) != 0 && tool_dbg) {
            fprintf(stderr, "[vproc-stage] adopt pscal stdout failed: %s\n", strerror(errno));
        }
    }
    if (stderr_use_pscal && pscal_stderr) {
        if (vprocAdoptPscalFd(vp, STDERR_FILENO, pscal_stderr) != 0 && tool_dbg) {
            fprintf(stderr, "[vproc-stage] adopt pscal stderr failed: %s\n", strerror(errno));
        }
    }
    if (pscal_stdin) pscal_fd_close(pscal_stdin);
    if (pscal_stdout) pscal_fd_close(pscal_stdout);
    if (pscal_stderr) pscal_fd_close(pscal_stderr);
    if (stdin_closed) (void)vprocClose(vp, STDIN_FILENO);
    if (stdout_closed) (void)vprocClose(vp, STDOUT_FILENO);
    if (stderr_closed) (void)vprocClose(vp, STDERR_FILENO);
#if defined(PSCAL_TARGET_IOS)
    {
        int pid = vprocPid(vp);
        if (pid > 0) {
            vprocResetCaughtSignals(pid);
            int job_mask = 0;
#ifdef SIGTSTP
            job_mask |= (1 << SIGTSTP);
#endif
#ifdef SIGTTIN
            job_mask |= (1 << SIGTTIN);
#endif
#ifdef SIGTTOU
            job_mask |= (1 << SIGTTOU);
#endif
            if (job_mask != 0) {
                (void)vprocDefaultSignal(pid, job_mask);
                (void)vprocUnblockSignals(pid, job_mask);
            }
        }
    }
#endif
    return vp;
}

/* A stage on a thread of its own has only borrowed the vproc it reads its stdio from, and the
 * owner can be killed at any moment, so it holds that vproc until it has one of its own. A cancel
 * in here would leave the pin held, and the owner waiting on it for ever. */
static VProc *shellCreateVProcForStage(int stdin_host,
                                       int stdout_host,
                                       int stderr_host,
                                       int cols,
                                       int rows,
                                       int pid_hint,
                                       bool inherit_pscal_stdio) {
    int cancel_state;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    VProc *parent = vprocPinCurrent();
    VProc *vp = shellMakeStageVProc(stdin_host, stdout_host, stderr_host, cols, rows, pid_hint, inherit_pscal_stdio);
    vprocUnpin(parent);
    pthread_setcancelstate(cancel_state, NULL);
    return vp;
}
#endif

static int shellTryRunScriptFallback(VM *vm, const ShellCommand *cmd) {
    if (!cmd || cmd->argc <= 0 || !cmd->argv || !cmd->argv[0]) {
        return -1;
    }
    const char *script_path = cmd->argv[0];
#if defined(PSCAL_TARGET_IOS)
    const char *probes[4];
    size_t probe_count = 0;
    probes[probe_count++] = cmd->argv[0];

    char expanded[PATH_MAX];
    char stripped[PATH_MAX];
    char stripped_expanded[PATH_MAX];

    if (cmd->argv[0][0] == '/' &&
        pathTruncateExpand(cmd->argv[0], expanded, sizeof(expanded)) &&
        strcmp(expanded, cmd->argv[0]) != 0) {
        probes[probe_count++] = expanded;
    }
    if (cmd->argv[0][0] == '/' &&
        pathTruncateStrip(cmd->argv[0], stripped, sizeof(stripped)) &&
        strcmp(stripped, cmd->argv[0]) != 0) {
        probes[probe_count++] = stripped;
        if (pathTruncateExpand(stripped, stripped_expanded, sizeof(stripped_expanded)) &&
            strcmp(stripped_expanded, stripped) != 0) {
            probes[probe_count++] = stripped_expanded;
        }
    }
#endif

    bool readable = false;
#if defined(PSCAL_TARGET_IOS)
    for (size_t i = 0; i < probe_count; ++i) {
        struct stat st;
        if (stat(probes[i], &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (access(probes[i], R_OK) != 0) {
            continue;
        }
        script_path = probes[i];
        readable = true;
        break;
    }
#else
    struct stat st;
    if (stat(script_path, &st) == 0 && S_ISREG(st.st_mode) && access(script_path, R_OK) == 0) {
        readable = true;
    }
#endif
    if (!readable) {
        return -1;
    }

    char *source = shellLoadFile(script_path);
    if (!source) {
        return -1;
    }

    char **saved_params = gParamValues;
    int saved_count = gParamCount;
    bool saved_owned = gShellPositionalOwned;

    bool replaced_params = false;
    char **new_params = NULL;
    int new_count = (cmd->argc > 1) ? (cmd->argc - 1) : 0;
    if (new_count > 0) {
        new_params = (char **)calloc((size_t)new_count, sizeof(char *));
        if (!new_params) {
            free(source);
            return -1;
        }
        for (int i = 0; i < new_count; ++i) {
            new_params[i] = strdup(cmd->argv[i + 1] ? cmd->argv[i + 1] : "");
            if (!new_params[i]) {
                shellFreeParameterArray(new_params, i);
                free(source);
                return -1;
            }
        }
        gParamValues = new_params;
        gParamCount = new_count;
        gShellPositionalOwned = true;
        replaced_params = true;
    }

    ShellRunOptions opts = {0};
    opts.no_cache = 1;
    opts.quiet = true;
    opts.exit_on_signal = shellRuntimeExitOnSignal();
    opts.suppress_warnings = true;
    /* Setting $0 below frees what this returns. */
    char *frontend_path = shellRuntimeGetArg0() ? strdup(shellRuntimeGetArg0()) : NULL;
    opts.frontend_path = frontend_path ? frontend_path : "exsh";

    /* A script run as a command starts over: what it inherits is the environment. */
    shellExportFlagsClearCurrent();
    ShellProgramScope program_scope;
    shellProgramScopeEnter(&program_scope);
    shellRuntimeSetArg0(cmd->argv[0]);
    bool exit_requested = false;
    /* A script run as a command has a directory of its own. */
    ShellCwdScope cwd_scope;
    shellCwdScopeEnter(&cwd_scope);
    int status = shellRunSource(source, cmd->argv[0], &opts, &exit_requested);
    shellCwdScopeLeave(&cwd_scope);
    free(source);
    bool had_program_ctx = program_scope.ctx != NULL;
    shellProgramScopeLeave(&program_scope);
    if (!had_program_ctx) {
        shellRuntimeSetArg0(frontend_path);
    }
    free(frontend_path);

    if (replaced_params) {
        if (gShellPositionalOwned) {
            shellFreeOwnedPositionalParameters();
        } else {
            gParamValues = NULL;
            gParamCount = 0;
        }
        gParamValues = saved_params;
        gParamCount = saved_count;
        gShellPositionalOwned = saved_owned;
    }

    /* External-style script execution should not request shell exit when the
     * script itself calls `exit`. Treat it as command termination only. */
    (void)exit_requested;

    shellUpdateStatus(status);
    return status;
}

static bool shellAppendCombinedRedirect(ShellCommand *cmd, const char *path, bool append) {
    if (!cmd || !path || !*path) {
        return false;
    }
    ShellRedirection redir;
    memset(&redir, 0, sizeof(redir));
    redir.fd = STDOUT_FILENO;
    redir.kind = SHELL_RUNTIME_REDIR_OPEN;
    redir.flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    redir.mode = 0666;
    redir.path = strdup(path);
    if (!redir.path) {
        return false;
    }

    ShellRedirection dup_redir;
    memset(&dup_redir, 0, sizeof(dup_redir));
    dup_redir.fd = STDERR_FILENO;
    dup_redir.kind = SHELL_RUNTIME_REDIR_DUP;
    dup_redir.dup_target_fd = STDOUT_FILENO;

    ShellRedirection *new_redirs =
        (ShellRedirection *)realloc(cmd->redirs, sizeof(ShellRedirection) * (cmd->redir_count + 2));
    if (!new_redirs) {
        free(redir.path);
        return false;
    }
    cmd->redirs = new_redirs;
    cmd->redirs[cmd->redir_count++] = redir;
    cmd->redirs[cmd->redir_count++] = dup_redir;
    return true;
}

static void shellPatchCombinedRedirectTokens(ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0 || !cmd->argv) {
        return;
    }
    for (size_t i = 0; i + 1 < cmd->argc; ) {
        const char *tok = cmd->argv[i];
        if (!tok) {
            ++i;
            continue;
        }
        bool match_append = (strcmp(tok, "&>>") == 0);
        bool match_trunc = (strcmp(tok, "&>") == 0);
        if (!match_append && !match_trunc) {
            ++i;
            continue;
        }
        const char *path = cmd->argv[i + 1];
        if (!path || *path == '\0') {
            ++i;
            continue;
        }
        if (!shellAppendCombinedRedirect(cmd, path, match_append)) {
            ++i;
            continue;
        }
        /* Remove the operator and path from argv */
        free(cmd->argv[i]);
        free(cmd->argv[i + 1]);
        for (size_t j = i + 2; j < cmd->argc; ++j) {
            cmd->argv[j - 2] = cmd->argv[j];
        }
        cmd->argc -= 2;
        cmd->argv[cmd->argc] = NULL;
        cmd->argv[cmd->argc + 1] = NULL;
        /* Do not advance i so we can catch multiple operators */
    }
}

/* What only the shell knows goes into a program's environment: exported locals, outermost
 * first so an inner one wins, and what is set for this command only. */
static void shellEnvironmentAddCommandValues(const ShellCommand *cmd) {
    for (size_t depth = 0; depth < gShellLocalScopeDepth; ++depth) {
        const ShellLocalScopeFrame *frame = &gShellLocalScopeStack[depth];
        for (size_t i = 0; i < frame->count; ++i) {
            const ShellLocalVariable *var = &frame->variables[i];
            if (var->name && var->value && var->exported && !var->no_value && !var->is_array_literal) {
                setenv(var->name, var->value, 1);
            }
        }
    }
    for (size_t i = 0; cmd && i < cmd->assignment_count; ++i) {
        const char *text = cmd->assignments[i].text;
        const char *eq = text ? strchr(text, '=') : NULL;
        if (eq && !cmd->assignments[i].is_array_literal) {
            char *name = strndup(text, (size_t)(eq - text));
            if (name) {
                /* A readonly one was refused. Attributes do not reach a prefix. */
                if (!strchr(name, '[') && !shellReadonlyContains(name)) {
                    setenv(name, eq + 1, 1);
                }
                free(name);
            }
        }
    }
}

/* In the child, about to exec: what bash would have left out goes. No lock: another
 * thread's lock does not survive into a child. */
static void shellEnvironmentForChild(const ShellCommand *cmd) {
    const ShellNameSet *unexported = &shellExportFlagsCurrent()->unexported;
    for (size_t i = 0; i < unexported->count; ++i) {
        unsetenv(unexported->names[i]);
    }
    /* Arrays are never exported. */
    for (size_t i = 0; i < gShellArrayVarCount; ++i) {
        if (gShellArrayVars[i].name) {
            unsetenv(gShellArrayVars[i].name);
        }
    }
    shellEnvironmentAddCommandValues(cmd);
}

#if defined(PSCAL_TARGET_IOS)
/* Nothing execs here: a program is a call on a thread, reading the environment through
 * the getenv macros. So it gets one of its own, built as a child's would be, and what it
 * sets there goes no further. */
static bool shellEnvEntryIsExported(const char *entry, void *arg) {
    const ShellNameSet *unexported = (const ShellNameSet *)arg;
    const char *eq = strchr(entry, '=');
    size_t len = eq ? (size_t)(eq - entry) : strlen(entry);
    /* Every builtin on a stage's thread rewrites this one, so it is not copied: see below. */
    if (strncmp(entry, "EXSH_LAST_STATUS=", 17) == 0) {
        return false;
    }
    for (size_t i = 0; i < unexported->count; ++i) {
        const char *name = unexported->names[i];
        if (strncmp(entry, name, len) == 0 && name[len] == '\0') {
            return false;
        }
    }
    for (size_t i = 0; i < gShellArrayVarCount; ++i) {
        const char *name = gShellArrayVars[i].name;
        if (name && strncmp(entry, name, len) == 0 && name[len] == '\0') {
            return false;
        }
    }
    return true;
}

static VProcEnv *shellProgramEnvCapture(const ShellCommand *cmd) {
    SHELL_EXPORT_LOCK();
    VProcEnv *env = vprocEnvCaptureFiltered(shellEnvEntryIsExported, &shellExportFlagsCurrent()->unexported);
    SHELL_EXPORT_UNLOCK();
    if (env) {
        VProcEnv *previous = vprocEnvActivate(env);
        char status[16];
        snprintf(status, sizeof(status), "%d", gShellRuntime.last_status);
        setenv("EXSH_LAST_STATUS", status, 1);
        shellEnvironmentAddCommandValues(cmd);
        vprocEnvActivate(previous);
    }
    return env;
}

typedef struct {
    VProcEnv *env;
    VProcEnv *previous;
} ShellProgramEnvScope;

/* A stage's, taken while the shell's thread launched it. The first program on this thread
 * uses it: by the time the stage gets there the shell may have moved on. */
static _Thread_local VProcEnv *gShellStageProgramEnv = NULL;

static void shellProgramEnvEnter(ShellProgramEnvScope *scope, const ShellCommand *cmd) {
    scope->env = gShellStageProgramEnv;
    gShellStageProgramEnv = NULL;
    if (!scope->env) {
        scope->env = shellProgramEnvCapture(cmd);
    }
    scope->previous = scope->env ? vprocEnvActivate(scope->env) : NULL;
}

static void shellProgramEnvLeave(ShellProgramEnvScope *scope) {
    if (scope->env) {
        vprocEnvActivate(scope->previous);
        vprocEnvRelease(scope->env);
        scope->env = NULL;
    }
}

/* For an applet, called from smallclue. */
void *shellRuntimeProgramEnvEnter(void) {
    ShellProgramEnvScope *scope = (ShellProgramEnvScope *)malloc(sizeof(ShellProgramEnvScope));
    if (scope) {
        shellProgramEnvEnter(scope, NULL);
    }
    return scope;
}

void shellRuntimeProgramEnvLeave(void *scope) {
    if (scope) {
        shellProgramEnvLeave((ShellProgramEnvScope *)scope);
        free(scope);
    }
}

static void shellStageCaptureState(ShellCommand *stage, const ShellCommand *cmd, size_t stage_count) {
    if (stage_count > 1 && shellStageNeedsOwnState(stage)) {
        stage->body_state = shellBodyStateCapture();
    }
    if (!stage->body && !stage->body_state) {
        /* exec -c in a substitution has chosen one. */
        if (!stage->program_env) {
            stage->program_env = shellProgramEnvCapture(cmd);
        }
        char lookup_buf[PATH_MAX];
        const char *name = cmd->argv && cmd->argv[0] ? shellCommandLookupName(cmd->argv[0], lookup_buf, sizeof(lookup_buf)) : NULL;
        if (name && !shellIsRuntimeBuiltin(name) && !smallclueFindApplet(name)) {
            stage->program_path = shellFindExecutableOnPath(cmd->argv[0]);
        }
    }
}

/* What PATH found while the stage was launched: it may not be the shell's any more, or
 * one this thread can read. */
static char *shellStageFindExecutable(const ShellCommand *cmd) {
    return cmd->program_path ? strdup(cmd->program_path) : shellFindExecutableOnPath(cmd->argv[0]);
}
#endif

#if !defined(PSCAL_TARGET_IOS)
/* exec -c. */
static void shellEnvironmentClear(void) {
    while (environ && environ[0]) {
        const char *eq = strchr(environ[0], '=');
        char *name = eq ? strndup(environ[0], (size_t)(eq - environ[0])) : strdup(environ[0]);
        if (!name || unsetenv(name) != 0) {
            free(name);
            break;
        }
        free(name);
    }
}

/* What exec's options call the program, or NULL if they do not: -a names it, and -l puts a
 * dash on what it is called, path and all. */
static char *shellExecShownName(const char *program, const char *arg0, bool login) {
    if (!login) {
        return arg0 ? strdup(arg0) : NULL;
    }
    char *name = NULL;
    return asprintf(&name, "-%s", arg0 ? arg0 : program) < 0 ? NULL : name;
}
#endif

/* exec's own search reads the environment, where a PATH that is local or not exported
 * will not be, nor any PATH for `exec -c`. Call before shellEnvironmentForChild. False
 * means no such program. */
static bool shellLookupForExec(const char *name, char **out_found, bool always_search) {
    *out_found = NULL;
    if (strchr(name, '/') || (!always_search && !shellExportIsHidden("PATH") && !shellLocalScopeFind("PATH"))) {
        return true;
    }
#if defined(PSCAL_TARGET_IOS)
    *out_found = shellFindExecutableOnPath(name);
#else
    char *path = shellLookupParameterValue("PATH", strlen("PATH"));
    for (const char *cursor = path ? path : ""; cursor && !*out_found;) {
        const char *next = strchr(cursor, ':');
        int dir_len = next ? (int)(next - cursor) : (int)strlen(cursor);
        char candidate[PATH_MAX];
        struct stat st;
        int len = snprintf(candidate, sizeof(candidate), "%.*s%s%s", dir_len, cursor, dir_len ? "/" : "", name);
        if (len > 0 && (size_t)len < sizeof(candidate) && stat(candidate, &st) == 0 && S_ISREG(st.st_mode) &&
            access(candidate, X_OK) == 0) {
            *out_found = strdup(candidate);
        }
        cursor = next ? next + 1 : NULL;
    }
    free(path);
#endif
    return *out_found != NULL;
}

/* set -x: a word needs quotes if the shell would read anything into it. */
static void shellTraceWord(const char *word) {
    bool plain = *word != '\0';
    for (const char *cursor = word; *cursor && plain; ++cursor) {
        unsigned char c = (unsigned char)*cursor;
        plain = isalnum(c) || strchr("_@%+=:,./-", c) || c >= 0x80;
    }
    if (plain) {
        fputs(word, stderr);
        return;
    }
    fputc('\'', stderr);
    for (const char *cursor = word; *cursor; ++cursor) {
        if (*cursor == '\'') {
            fputs("'\\''", stderr);
        } else {
            fputc(*cursor, stderr);
        }
    }
    fputc('\'', stderr);
}

static void shellTraceCommand(const ShellCommand *cmd) {
    bool is_set = false;
    char *prompt = shellLookupParameterValueInternal("PS4", 3, &is_set);
    fputs((is_set && prompt) ? prompt : "+ ", stderr);
    bool first = true;
    /* An assignment gets a line of its own. */
    const char *line_prompt = (is_set && prompt) ? prompt : "+ ";
    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        const char *text = cmd->assignments[i].text;
        const char *eq = text ? strchr(text, '=') : NULL;
        if (!first) {
            fputs(line_prompt, stderr);
        }
        first = false;
        if (eq) {
            fwrite(text, 1, (size_t)(eq - text) + 1, stderr);
            if (cmd->assignments[i].is_array_literal) {
                fputs(eq + 1, stderr);
            } else if (*(eq + 1)) {
                shellTraceWord(eq + 1);
            }
        }
        fputc('\n', stderr);
    }
    if (cmd->argc == 0 && !first) {
        free(prompt);
        fflush(stderr);
        return;
    }
    if (!first) {
        fputs(line_prompt, stderr);
        first = true;
    }
    bool double_bracket = cmd->argc > 0 && cmd->argv[0] && strcmp(cmd->argv[0], "__shell_double_bracket") == 0;
    for (size_t i = 0; i < cmd->argc; ++i) {
        if (!first) {
            fputc(' ', stderr);
        }
        first = false;
        if (i == 0 && double_bracket) {
            fputs("[[", stderr);
        } else {
            shellTraceWord(cmd->argv[i] ? cmd->argv[i] : "");
        }
    }
    if (double_bracket) {
        fputs(" ]]", stderr);
    }
    fputc('\n', stderr);
    free(prompt);
    fflush(stderr);
}

/* While a command's prefixes are expanded, those already done are set: `A=1 B=$A cmd`. Then
 * it is all put back, and the command sets them for itself. Silent: it reports a failure. */
typedef struct {
    ShellAssignmentBackup **items;
    size_t count;
    size_t shown;
} ShellPrefixView;

static void shellPrefixViewCatchUp(ShellPrefixView *view, const ShellCommand *cmd) {
    while (view->shown < cmd->assignment_count) {
        ShellCommand one;
        memset(&one, 0, sizeof(one));
        one.assignments = &cmd->assignments[view->shown++];
        one.assignment_count = 1;
        ShellAssignmentBackup *backup = NULL;
        size_t count = 0;
        /* With no command they are plain variables, attributes and all, and what a substitution
         * starts does not get them. */
        if (!shellApplyAssignments(&one, &backup, &count, NULL, NULL, false, cmd->argc > 0) || !backup) {
            continue;
        }
        ShellAssignmentBackup **items =
            (ShellAssignmentBackup **)realloc(view->items, (view->count + 1) * sizeof(*items));
        if (!items) {
            shellRestoreAssignments(backup, count);
            continue;
        }
        view->items = items;
        view->items[view->count++] = backup;
    }
}

static void shellPrefixViewRestore(ShellPrefixView *view) {
    while (view->count > 0) {
        shellRestoreAssignments(view->items[--view->count], 1);
    }
    free(view->items);
    view->items = NULL;
}

static bool shellWordIsAssignment(const char *arg) {
    const char *text = NULL;
    const char *meta = NULL;
    size_t meta_len = 0;
    uint8_t flags = 0;
    return strncmp(arg, "redir:", 6) != 0 && shellDecodeWordSpec(arg, &text, &flags, &meta, &meta_len) &&
           (flags & SHELL_WORD_FLAG_ASSIGNMENT) != 0;
}

static void shellHashRecordExecution(const char *name);
static bool shellBuildCommand(VM *vm, int arg_count, Value *args, ShellCommand *out_cmd) {
    if (!out_cmd) {
        return false;
    }
    bool redir_dbg = getenv("PSCALI_REDIR_DEBUG") != NULL;
    memset(out_cmd, 0, sizeof(*out_cmd));
    if (arg_count <= 0) {
        runtimeError(vm, "shell exec: missing metadata argument");
        return false;
    }
    Value meta = args[0];
    if (VALUE_TYPE(meta) != TYPE_STRING || !AS_STRING(meta)) {
        runtimeError(vm, "shell exec: metadata must be a string");
        return false;
    }
    shellParseMetadata(AS_STRING(meta), out_cmd);
    if (out_cmd->line > 0) {
        gShellCurrentCommandLine = out_cmd->line;
    }
    gShellExpansionFailed = false;
    bool saw_command_word = false;
    unsigned long substitutions_before = gShellRuntime.command_substitutions_run;
    if (redir_dbg) {
        fprintf(stderr, "[redir-build] arg_count=%d meta=%s\n", arg_count, AS_STRING(meta));
    }
    /* Prefix assignments come last: the command's words do not see them. */
    int first_prefix = 0;
    int last_prefix = 0;
    for (int i = 1; i < arg_count; ++i) {
        Value v = args[i];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "shell exec: arguments must be strings");
            shellFreeCommand(out_cmd);
            return false;
        }
        if (redir_dbg) {
            fprintf(stderr, "[redir-build]   arg[%d]=%s\n", i, AS_STRING(v));
        }
        if (!saw_command_word && shellWordIsAssignment(AS_STRING(v))) {
            first_prefix = first_prefix ? first_prefix : i;
            last_prefix = i;
            continue;
        }
        if (strncmp(AS_STRING(v), "redir:", 6) == 0) {
            if (!shellAddRedirection(out_cmd, AS_STRING(v))) {
                runtimeError(vm, "shell exec: invalid redirection '%s'", AS_STRING(v));
                shellFreeCommand(out_cmd);
                return false;
            }
        } else {
            if (!shellAddArg(out_cmd, AS_STRING(v), &saw_command_word)) {
                runtimeError(vm, "shell exec: unable to add argument");
                shellFreeCommand(out_cmd);
                return false;
            }
            /* Even one that expands to nothing: `$empty A=1` runs a command named A=1. */
            saw_command_word = true;
        }
        if (gShellExpansionFailed) {
            /* Reported already, and the command does not run. */
            shellFreeCommand(out_cmd);
            return false;
        }
    }
    if (first_prefix > 0) {
        ShellPrefixView view = {0};
        bool added = true;
        for (int i = first_prefix; i <= last_prefix && added && !gShellExpansionFailed; ++i) {
            if (shellWordIsAssignment(AS_STRING(args[i]))) {
                bool no_command = false;
                shellPrefixViewCatchUp(&view, out_cmd);
                added = shellAddArg(out_cmd, AS_STRING(args[i]), &no_command);
            }
        }
        shellPrefixViewRestore(&view);
        if (!added) {
            runtimeError(vm, "shell exec: unable to add argument");
        }
        if (!added || gShellExpansionFailed) {
            shellFreeCommand(out_cmd);
            return false;
        }
    }
    out_cmd->ran_command_substitution = gShellRuntime.command_substitutions_run != substitutions_before;
    shellRewriteDoubleBracketTest(out_cmd);
    return true;
}

static bool shellAliasBufferAppendChar(char **buffer, size_t *length, size_t *capacity, char ch) {
    if (!buffer || !length || !capacity) {
        return false;
    }
    if (*length + 1 >= *capacity) {
        size_t new_capacity = (*capacity == 0) ? 32 : (*capacity * 2);
        while (new_capacity < (*length + 2)) {
            new_capacity *= 2;
        }
        char *resized = (char *)realloc(*buffer, new_capacity);
        if (!resized) {
            return false;
        }
        *buffer = resized;
        *capacity = new_capacity;
    }
    (*buffer)[(*length)++] = ch;
    return true;
}

static bool shellAliasFlushWord(char ***out_words,
                                size_t *out_count,
                                size_t *out_capacity,
                                char **buffer,
                                size_t *length) {
    if (!out_words || !out_count || !out_capacity || !buffer || !length) {
        return false;
    }
    if (*length == 0) {
        return true;
    }
    char *word = (char *)malloc(*length + 1);
    if (!word) {
        return false;
    }
    memcpy(word, *buffer, *length);
    word[*length] = '\0';
    if (*out_count >= *out_capacity) {
        size_t new_capacity = (*out_capacity == 0) ? 4 : (*out_capacity * 2);
        char **resized = (char **)realloc(*out_words, new_capacity * sizeof(char *));
        if (!resized) {
            free(word);
            return false;
        }
        *out_words = resized;
        *out_capacity = new_capacity;
    }
    (*out_words)[(*out_count)++] = word;
    *length = 0;
    return true;
}

static void shellAliasFreeWordList(char **words, size_t count) {
    if (!words) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(words[i]);
    }
    free(words);
}

static bool shellAliasTokenizeValue(const char *value, char ***out_words, size_t *out_count) {
    if (out_words) {
        *out_words = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!value || *value == '\0') {
        return true;
    }

    char **words = NULL;
    size_t word_count = 0;
    size_t word_capacity = 0;
    char *buffer = NULL;
    size_t buffer_len = 0;
    size_t buffer_capacity = 0;
    bool in_single = false;
    bool in_double = false;
    bool escaping = false;

    for (const char *cursor = value; *cursor; ++cursor) {
        char c = *cursor;
        if (escaping) {
            if (!shellAliasBufferAppendChar(&buffer, &buffer_len, &buffer_capacity, c)) {
                shellAliasFreeWordList(words, word_count);
                free(buffer);
                return false;
            }
            escaping = false;
            continue;
        }
        if (in_single) {
            if (c == '\'') {
                in_single = false;
            } else {
                if (!shellAliasBufferAppendChar(&buffer, &buffer_len, &buffer_capacity, c)) {
                    shellAliasFreeWordList(words, word_count);
                    free(buffer);
                    return false;
                }
            }
            continue;
        }
        if (in_double) {
            if (c == '"') {
                in_double = false;
                continue;
            }
            if (c == '\\') {
                escaping = true;
                continue;
            }
            if (!shellAliasBufferAppendChar(&buffer, &buffer_len, &buffer_capacity, c)) {
                shellAliasFreeWordList(words, word_count);
                free(buffer);
                return false;
            }
            continue;
        }
        if (c == '\\') {
            escaping = true;
            continue;
        }
        if (c == '\'') {
            in_single = true;
            continue;
        }
        if (c == '"') {
            in_double = true;
            continue;
        }
        if (isspace((unsigned char)c)) {
            if (!shellAliasFlushWord(&words, &word_count, &word_capacity, &buffer, &buffer_len)) {
                shellAliasFreeWordList(words, word_count);
                free(buffer);
                return false;
            }
            continue;
        }
        if (!shellAliasBufferAppendChar(&buffer, &buffer_len, &buffer_capacity, c)) {
            shellAliasFreeWordList(words, word_count);
            free(buffer);
            return false;
        }
    }
    if (escaping) {
        if (!shellAliasBufferAppendChar(&buffer, &buffer_len, &buffer_capacity, '\\')) {
            shellAliasFreeWordList(words, word_count);
            free(buffer);
            return false;
        }
    }
    if (!shellAliasFlushWord(&words, &word_count, &word_capacity, &buffer, &buffer_len)) {
        shellAliasFreeWordList(words, word_count);
        free(buffer);
        return false;
    }

    free(buffer);
    if (out_words) {
        *out_words = words;
    } else {
        shellAliasFreeWordList(words, word_count);
    }
    if (out_count) {
        *out_count = word_count;
    }
    return true;
}

static bool shellAliasApplyTokens(ShellCommand *cmd, char **alias_words, size_t alias_count) {
    if (!cmd || !cmd->argv) {
        shellAliasFreeWordList(alias_words, alias_count);
        return false;
    }

    size_t remaining = (cmd->argc > 0) ? (cmd->argc - 1) : 0;
    size_t new_argc = alias_count + remaining;
    char **new_argv = (char **)calloc(new_argc + 1, sizeof(char *));
    if (!new_argv) {
        shellAliasFreeWordList(alias_words, alias_count);
        return false;
    }

    size_t dest = 0;
    for (size_t i = 0; i < alias_count; ++i) {
        new_argv[dest++] = alias_words[i];
        alias_words[i] = NULL;
    }
    for (size_t i = 1; i < cmd->argc; ++i) {
        new_argv[dest++] = cmd->argv[i];
        cmd->argv[i] = NULL;
    }

    if (cmd->argc > 0 && cmd->argv[0]) {
        free(cmd->argv[0]);
    }
    free(cmd->argv);
    if (new_argc == 0) {
        free(new_argv);
        cmd->argv = NULL;
        cmd->argc = 0;
        shellAliasFreeWordList(alias_words, alias_count);
        return true;
    }
    cmd->argv = new_argv;
    cmd->argc = new_argc;
    cmd->argv[cmd->argc] = NULL;

    free(alias_words);
    return true;
}

static bool shellExpandAliasOnce(ShellCommand *cmd, const char *value) {
    if (!cmd || !value) {
        return true;
    }
    char **alias_words = NULL;
    size_t alias_count = 0;
    if (!shellAliasTokenizeValue(value, &alias_words, &alias_count)) {
        return false;
    }
    if (alias_count == 0 && cmd->argc <= 1) {
        if (cmd->argc == 1 && cmd->argv) {
            free(cmd->argv[0]);
            free(cmd->argv);
            cmd->argv = NULL;
            cmd->argc = 0;
        }
        shellAliasFreeWordList(alias_words, alias_count);
        return true;
    }
    bool ok = shellAliasApplyTokens(cmd, alias_words, alias_count);
    return ok;
}

static bool shellAliasTextNeedsParsing(const char *value) {
    bool in_single = false;
    bool in_double = false;
    for (const char *cursor = value; *cursor; ++cursor) {
        char c = *cursor;
        if (c == '\\' && !in_single && cursor[1]) {
            cursor++;
        } else if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (!in_single && (c == '$' || c == '`')) {
            return true;
        } else if (!in_single && !in_double && strchr(";|&<>()\n", c)) {
            return true;
        }
    }
    return false;
}

/* The command becomes an eval of the alias's text and its other words, quoted. It stays
 * one command: a pipeline stage still gets its pipes, and its redirections still apply. */
static bool shellRewriteAliasAsEval(ShellCommand *cmd, const char *value) {
    char *script = NULL;
    size_t length = 0;
    size_t capacity = 0;
    shellBufferAppendString(&script, &length, &capacity, value);
    for (size_t i = 1; i < cmd->argc; ++i) {
        shellBufferAppendString(&script, &length, &capacity, " '");
        for (const char *cursor = cmd->argv[i] ? cmd->argv[i] : ""; *cursor; ++cursor) {
            if (*cursor == '\'') {
                shellBufferAppendString(&script, &length, &capacity, "'\\''");
            } else {
                shellBufferAppendChar(&script, &length, &capacity, *cursor);
            }
        }
        shellBufferAppendChar(&script, &length, &capacity, '\'');
    }
    char *word = strdup("eval");
    if (!script || !word) {
        free(script);
        free(word);
        return false;
    }
    for (size_t i = 0; i < cmd->argc; ++i) {
        free(cmd->argv[i]);
    }
    cmd->argv[0] = word;
    cmd->argv[1] = script;
    cmd->argv[2] = NULL;
    cmd->argc = 2;
    return true;
}

static bool shellExpandAliasesIfNeeded(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0 || cmd->ignore_functions) {
        return true;
    }
    if (!shellShoptOptionEnabled("expand_aliases")) {
        return true;
    }
    int depth = 0;
    enum { kMaxAliasDepth = 16 };
    const char *expanded_names[kMaxAliasDepth];
    size_t expanded_count = 0;
    while (cmd->argc > 0 && depth < kMaxAliasDepth) {
        const char *name = cmd->argv[0];
        if (!name) {
            break;
        }
        bool already_expanded = false;
        for (size_t i = 0; i < expanded_count; ++i) {
            if (strcmp(expanded_names[i], name) == 0) {
                already_expanded = true;
                break;
            }
        }
        if (already_expanded) {
            break;
        }
        ShellAlias *alias = shellFindAlias(name);
        if (!alias || !alias->value) {
            break;
        }
        if (shellAliasTextNeedsParsing(alias->value)) {
            /* Splitting into words cannot carry `;`, `|` or `>`. */
            if (cmd->argc < 2) {
                char **grown = (char **)realloc(cmd->argv, sizeof(char *) * 3);
                if (!grown) {
                    return false;
                }
                cmd->argv = grown;
            }
            return shellRewriteAliasAsEval(cmd, alias->value);
        }
        if (!shellExpandAliasOnce(cmd, alias->value)) {
            if (vm) {
                runtimeError(vm, "shell exec: failed to expand alias '%s'", name);
            }
            return false;
        }
        if (expanded_count < kMaxAliasDepth) {
            expanded_names[expanded_count++] = alias->name ? alias->name : name;
        }
        if (!cmd->argv || cmd->argc == 0 || !cmd->argv[0]) {
            break;
        }
        depth++;
    }
    if (depth >= kMaxAliasDepth) {
        if (vm) {
            const char *label = (cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<alias>";
            runtimeError(vm, "shell exec: alias expansion depth exceeded for '%s'", label);
        }
        return false;
    }
    /* Alias text is split into words, not parsed, so its `&>` arrives as an argument.
     * Anywhere else that word was quoted and means itself: echo "&>" x. */
    if (depth > 0) {
        shellPatchCombinedRedirectTokens(cmd);
    }
    return true;
}

typedef enum {
    SHELL_REDIR_OP_OPEN,
    SHELL_REDIR_OP_DUP,
    SHELL_REDIR_OP_HEREDOC,
    SHELL_REDIR_OP_HERE_STRING
} ShellRuntimeRedirOpType;

typedef struct {
    ShellRuntimeRedirOpType type;
    int target_fd;
    int source_fd;
    int write_fd;
    const char *here_body;
    size_t here_length;
    bool close_target;
    bool append_newline;
    const char *fd_var;
} ShellRuntimeRedirOp;

/* The number in {name}>&-'s name. -1 for anything but a plain number, unset included. */
static int shellFdVarValue(const char *name) {
    const char *bracket = strchr(name, '[');
    size_t name_len = bracket ? (size_t)(bracket - name) : strlen(name);
    ShellParamSubject subject;
    int fd = -1;
    if (shellParamLoadSubject(name, name_len, bracket ? bracket + 1 : NULL, bracket ? strlen(bracket + 1) - 1 : 0,
                              &subject) &&
        subject.value && subject.value[0]) {
        char *end = NULL;
        errno = 0;
        long value = strtol(subject.value, &end, 10);
        if (errno == 0 && *end == '\0' && isdigit((unsigned char)subject.value[0]) && value <= INT_MAX) {
            fd = (int)value;
        }
    }
    shellParamSubjectFree(&subject);
    return fd;
}

/* False, with both of bash's lines said, if name cannot take it. */
static bool shellFdVarAssign(const char *name, int fd) {
    char text[32];
    snprintf(text, sizeof(text), "%d", fd);
    const char *bracket = strchr(name, '[');
    size_t name_len = bracket ? (size_t)(bracket - name) : strlen(name);
    char *base = strndup(name, name_len);
    if (!base) {
        return false;
    }
    bool ok = !shellReadonlyContains(base);
    if (!ok) {
        fprintf(stderr, "exsh: %s: readonly variable\n", base);
    } else if (bracket) {
        char *key = shellParamSubscriptKey(name, name_len, bracket + 1, strlen(bracket + 1) - 1);
        ok = key != NULL;
        if (key) {
            shellArrayRegistrySetElement(base, key, text);
            free(key);
        }
    } else {
        ok = shellSetTrackedVariable(base, text, false);
    }
    if (!ok) {
        fprintf(stderr, "exsh: %s: cannot assign fd to variable\n", base);
    }
    free(base);
    return ok;
}

static int shellSpawnProcess(VM *vm,
                             const ShellCommand *cmd,
                             int stdin_fd,
                             int stdout_fd,
                             int stderr_fd,
                             pid_t *child_pid,
                             bool ignore_job_signals) {
    if (!cmd || cmd->argc == 0 || !cmd->argv || !cmd->argv[0] || !child_pid) {
        return EINVAL;
    }

    ShellRuntimeRedirOp local_ops[16];
    memset(local_ops, 0, sizeof(local_ops));
    ShellRuntimeRedirOp *ops = local_ops;
    size_t op_capacity = sizeof(local_ops) / sizeof(local_ops[0]);
    if (cmd->redir_count > op_capacity) {
        ops = (ShellRuntimeRedirOp *)calloc(cmd->redir_count, sizeof(ShellRuntimeRedirOp));
        if (!ops) {
            return ENOMEM;
        }
        op_capacity = cmd->redir_count;
    }

    size_t op_count = 0;
    int prep_error = 0;
    /* A redirection that cannot open fails this command, not the script. The child
     * reports it and exits 1, so a pipeline stage, `&` and `wait` see an ordinary
     * failed process and the rest of a pipeline still runs. */
    int redir_error = 0;
    const char *redir_error_target = NULL;
    enum { SHELL_REDIR_ERROR_AMBIGUOUS = -1 };
    for (size_t i = 0; i < cmd->redir_count; ++i) {
        if (op_count >= op_capacity) {
            prep_error = ENOMEM;
            goto spawn_cleanup;
        }
        const ShellRedirection *redir = &cmd->redirs[i];
        ShellRuntimeRedirOp op;
        memset(&op, 0, sizeof(op));
        op.target_fd = redir->fd;
        op.fd_var = redir->fd_var;
        switch (redir->kind) {
            case SHELL_RUNTIME_REDIR_OPEN: {
                if (!redir->path) {
                    prep_error = EINVAL;
                    goto spawn_cleanup;
                }
                int fd = open(redir->path, redir->flags, redir->mode);
                if (fd < 0) {
                    redir_error = errno;
                    redir_error_target = redir->path;
                    break;
                }
                op.type = SHELL_REDIR_OP_OPEN;
                op.source_fd = fd;
                break;
            }
            case SHELL_RUNTIME_REDIR_DUP: {
                if (redir->ambiguous) {
                    redir_error = SHELL_REDIR_ERROR_AMBIGUOUS;
                    redir_error_target = redir->path;
                    break;
                }
                op.type = SHELL_REDIR_OP_DUP;
                op.close_target = redir->close_target;
                op.source_fd = redir->dup_target_fd;
                if (!op.close_target && op.source_fd < 0) {
                    prep_error = EBADF;
                    goto spawn_cleanup;
                }
                if (op.close_target && redir->fd_var) {
                    op.target_fd = shellFdVarValue(redir->fd_var);
                    if (op.target_fd < 0) {
                        redir_error = SHELL_REDIR_ERROR_AMBIGUOUS;
                        redir_error_target = redir->fd_var;
                    }
                }
                break;
            }
            case SHELL_RUNTIME_REDIR_HEREDOC: {
                const char *body = redir->here_doc ? redir->here_doc : "";
                size_t body_length = redir->here_doc_length ? redir->here_doc_length : strlen(body);
                op.type = SHELL_REDIR_OP_HEREDOC;
                op.source_fd = shellOpenHereBody(body, body_length, false);
                if (op.source_fd < 0) {
                    prep_error = errno;
                    goto spawn_cleanup;
                }
                op.write_fd = -1;
                op.here_body = body;
                op.here_length = body_length;
                op.append_newline = false;
                break;
            }
            case SHELL_RUNTIME_REDIR_HERE_STRING: {
                op.type = SHELL_REDIR_OP_HERE_STRING;
                const char *body = redir->here_string ? redir->here_string
                                                      : (redir->here_string_literal ? redir->here_string_literal : "");
                size_t body_length = redir->here_string_length ? redir->here_string_length : strlen(body);
                op.source_fd = shellOpenHereBody(body, body_length, true);
                if (op.source_fd < 0) {
                    prep_error = errno;
                    goto spawn_cleanup;
                }
                op.write_fd = -1;
                op.here_body = body;
                op.here_length = redir->here_string_length;
                if (op.here_length == 0 && body) {
                    op.here_length = strlen(body);
                }
                op.append_newline = true;
                break;
            }
            default:
                prep_error = EINVAL;
                goto spawn_cleanup;
        }
        if (redir_error != 0) {
            break;
        }
        ops[op_count++] = op;
    }

    {
    pid_t dollar_pid = shellRuntimeDollarPid();
    /* The child reaches its own exit with a copy of whatever is still buffered. */
    shellFlushStandardStreams();
    pid_t child = fork();
        if (child < 0) {
            prep_error = errno;
            goto spawn_cleanup;
        }

        if (child == 0) {
            ShellPipelineContext *ctx = &gShellRuntime.pipeline;
            pid_t desired_pgid = getpid();
            if (ctx->active && ctx->pgid > 0) {
                desired_pgid = ctx->pgid;
            }
            if (!gShellRuntime.in_subshell && setpgid(0, desired_pgid) != 0) {
                /* best-effort; ignore errors */
            }

            if (ignore_job_signals) {
                signal(SIGINT, SIG_IGN);
                signal(SIGQUIT, SIG_IGN);
            } else {
                signal(SIGINT, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
            }
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            /* The shell ignores it for itself. A closed reader must end a child. */
            signal(SIGPIPE, SIG_DFL);

            if (ctx->active && ctx->pipes) {
                size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
                for (size_t i = 0; i < pipe_count; ++i) {
                    int r = ctx->pipes[i][0];
                    int w = ctx->pipes[i][1];
                    if (r >= 0 && r != stdin_fd && r != stdout_fd && r != stderr_fd) {
                        close(r);
                    }
                    if (w >= 0 && w != stdin_fd && w != stdout_fd && w != stderr_fd) {
                        close(w);
                    }
                }
            }

            if (stdin_fd >= 0 && dup2(stdin_fd, STDIN_FILENO) < 0) {
                int err = errno;
                fprintf(stderr, "exsh: failed to setup stdin: %s\n", strerror(err));
                _exit(126);
            }
            if (stdout_fd >= 0 && dup2(stdout_fd, STDOUT_FILENO) < 0) {
                int err = errno;
                fprintf(stderr, "exsh: failed to setup stdout: %s\n", strerror(err));
                _exit(126);
            }
            if (stderr_fd >= 0 && dup2(stderr_fd, STDERR_FILENO) < 0) {
                int err = errno;
                fprintf(stderr, "exsh: failed to setup stderr: %s\n", strerror(err));
                _exit(126);
            }

            for (size_t i = 0; i < op_count; ++i) {
                ShellRuntimeRedirOp *op = &ops[i];
                if ((op->type == SHELL_REDIR_OP_HEREDOC || op->type == SHELL_REDIR_OP_HERE_STRING) &&
                    op->write_fd >= 0) {
                    close(op->write_fd);
                    op->write_fd = -1;
                }
            }

            for (size_t i = 0; i < op_count; ++i) {
                ShellRuntimeRedirOp *op = &ops[i];
                /* Bash does these in the child too, so the shell never hears of the number. */
                if (op->fd_var && !op->close_target) {
                    op->target_fd = fcntl(op->source_fd, F_DUPFD, 10);
                    if (op->target_fd < 0 || !shellFdVarAssign(op->fd_var, op->target_fd)) {
                        if (op->target_fd < 0) {
                            fprintf(stderr, "exsh: %d: %s\n", op->source_fd, strerror(errno));
                        }
                        _exit(1);
                    }
                    continue;
                }
                switch (op->type) {
                    case SHELL_REDIR_OP_OPEN:
                    case SHELL_REDIR_OP_HEREDOC:
                    case SHELL_REDIR_OP_HERE_STRING:
                        if (dup2(op->source_fd, op->target_fd) < 0) {
                            int err = errno;
                            fprintf(stderr, "exsh: %s: %s\n", cmd->argv[0], strerror(err));
                            _exit(1);
                        }
                        break;
                    case SHELL_REDIR_OP_DUP:
                        if (op->close_target) {
                            close(op->target_fd);
                        } else if (dup2(op->source_fd, op->target_fd) < 0) {
                            int err = errno;
                            fprintf(stderr, "exsh: %d: %s\n", op->source_fd, strerror(err));
                            _exit(1);
                        }
                        break;
                }
            }

            /* After the earlier ones: `cmd 2>log >/missing` reports into log. */
            if (redir_error != 0) {
                fprintf(stderr, "exsh: %s: %s\n", redir_error_target,
                        redir_error == SHELL_REDIR_ERROR_AMBIGUOUS ? "ambiguous redirect" : strerror(redir_error));
                _exit(1);
            }

            if (stdin_fd >= 0 && stdin_fd != STDIN_FILENO) {
                close(stdin_fd);
            }
            if (stdout_fd >= 0 && stdout_fd != STDOUT_FILENO && stdout_fd != stderr_fd) {
                close(stdout_fd);
            }
            if (stderr_fd >= 0 && stderr_fd != STDERR_FILENO) {
                close(stderr_fd);
            }

            for (size_t i = 0; i < op_count; ++i) {
                ShellRuntimeRedirOp *op = &ops[i];
                if ((op->type == SHELL_REDIR_OP_OPEN || op->type == SHELL_REDIR_OP_HEREDOC ||
                     op->type == SHELL_REDIR_OP_HERE_STRING) &&
                    op->source_fd >= 0 && op->source_fd != op->target_fd) {
                    close(op->source_fd);
                    op->source_fd = -1;
                }
            }

            /* They are done. A builtin or a function would open them again: `f 2>&1 > out` sent
             * both streams to out, and under `set -C` the second open failed. */
            shellFreeRedirections((ShellCommand *)cmd);
            /* This child may go on to run shell code: a function or a compound command. */
            shellSubshellResetState(dollar_pid);
            bool builtin_ran = shellInvokeBuiltin(vm ? vm : gShellCurrentVm, (ShellCommand *)cmd);
            if (builtin_ran) {
                int status = gShellRuntime.last_status;
                shellFlushStandardStreams();
                _exit(status);
            }

#if defined(PSCAL_TARGET_IOS)
            const char *shebang_path = cmd->argv[0];
            char *resolved = shellFindExecutableOnPath(cmd->argv[0]);
            if (resolved) {
                shebang_path = resolved;
            }
            shellEnvironmentForChild(cmd);
            int shebang_status = shellMaybeExecShebangTool(shebang_path, cmd->argv);
            if (resolved) {
                free(resolved);
            }
            if (shebang_status >= 0) {
                _exit(shebang_status);
            }
#endif
            int err = 0;
#if !defined(PSCAL_TARGET_IOS)
            char *exec_found = NULL;
            bool exec_lookup_ok = shellLookupForExec(cmd->argv[0], &exec_found, cmd->exec_clear_env);
#endif
            shellEnvironmentForChild(cmd);
#if defined(PSCAL_TARGET_IOS)
            char exec_path[PATH_MAX];
            const char *resolved_exec = cmd->argv[0];
            if (shellResolveExecutionPath(cmd->argv[0], exec_path, sizeof(exec_path))) {
                resolved_exec = exec_path;
            }
            execv(resolved_exec, cmd->argv);
            err = errno;
            if ((err == ENOENT || err == EACCES) && strcmp(resolved_exec, cmd->argv[0]) != 0) {
                execvp(cmd->argv[0], cmd->argv);
                err = errno;
            }
#else
            if (cmd->exec_clear_env) {
                shellEnvironmentClear();
            }
            char *exec_program = cmd->argv[0];
            if (cmd->exec_arg0) {
                cmd->argv[0] = cmd->exec_arg0;
            }
            if (exec_lookup_ok) {
                execvp(exec_found ? exec_found : exec_program, cmd->argv);
                err = errno;
            } else {
                err = ENOENT;
            }
            cmd->argv[0] = exec_program;
#endif
#if defined(PSCAL_TARGET_IOS)
            int script_status = shellTryRunScriptFallback(vm ? vm : gShellCurrentVm, cmd);
#else
            /* A file without execute permission is an error, not a script to read. */
            int script_status = err == ENOEXEC ? shellTryRunScriptFallback(vm ? vm : gShellCurrentVm, cmd) : -1;
#endif
            if (script_status >= 0) {
                _exit(script_status);
            }
            fprintf(stderr, "exsh: %s: %s\n", cmd->argv[0], strerror(err));
            _exit((err == ENOENT) ? 127 : 126);
        }
        for (size_t j = 0; j < op_count; ++j) {
            ShellRuntimeRedirOp *op = &ops[j];
            if (op->type == SHELL_REDIR_OP_OPEN) {
                if (op->source_fd >= 0) {
                    close(op->source_fd);
                    op->source_fd = -1;
                }
            } else if (op->type == SHELL_REDIR_OP_HEREDOC || op->type == SHELL_REDIR_OP_HERE_STRING) {
                if (op->source_fd >= 0) {
                    close(op->source_fd);
                    op->source_fd = -1;
                }
                if (op->write_fd >= 0) {
                    const char *body = op->here_body ? op->here_body : "";
                    size_t remaining = op->here_length;
                    if (remaining == 0 && body) {
                        remaining = strlen(body);
                    }
                    const char *cursor = body;
                    while (remaining > 0) {
                        ssize_t written = write(op->write_fd, cursor, remaining);
                        if (written < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            break;
                        }
                        cursor += written;
                        remaining -= (size_t)written;
                    }
                    if (op->append_newline) {
                        const char newline = '\n';
                        while (true) {
                            ssize_t written = write(op->write_fd, &newline, 1);
                            if (written < 0) {
                                if (errno == EINTR) {
                                    continue;
                                }
                                break;
                            }
                            break;
                        }
                    }
                    close(op->write_fd);
                    op->write_fd = -1;
                }
            }
        }
        if (ops != local_ops) {
            free(ops);
        }
        *child_pid = child;
        return 0;
    }

spawn_cleanup:
    for (size_t j = 0; j < op_count; ++j) {
        if (ops[j].type == SHELL_REDIR_OP_OPEN || ops[j].type == SHELL_REDIR_OP_HEREDOC ||
            ops[j].type == SHELL_REDIR_OP_HERE_STRING) {
            if (ops[j].source_fd >= 0) {
                close(ops[j].source_fd);
                ops[j].source_fd = -1;
            }
        }
        if ((ops[j].type == SHELL_REDIR_OP_HEREDOC || ops[j].type == SHELL_REDIR_OP_HERE_STRING) &&
            ops[j].write_fd >= 0) {
            close(ops[j].write_fd);
            ops[j].write_fd = -1;
        }
    }
    if (ops != local_ops) {
        free(ops);
    }
    return prep_error;
}

typedef struct {
    pid_t pid;
    int status_raw;
    int status_mapped;
    bool stopped;
    bool continued;
} ShellWaitResult;

static int __attribute__((unused)) shellWaitPid(pid_t pid, int *status_out, bool allow_stop, bool *out_stopped) {
    ShellWaitResult res = { .pid = pid, .status_raw = 0, .status_mapped = 0, .stopped = false, .continued = false };
    int err = 0;
    int status = 0;
    int options = allow_stop ? WUNTRACED : 0;
    pid_t waited;
    do {
        waited = vprocWaitPidShim(pid, &status, options);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        err = errno;
    }
    res.status_raw = status;
    if (WIFSTOPPED(status)) {
        res.stopped = true;
        res.status_mapped = 128 + WSTOPSIG(status);
    } else if (WIFEXITED(status)) {
        res.status_mapped = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res.status_mapped = 128 + WTERMSIG(status);
    } else {
        res.status_mapped = status;
    }
    if (status_out) {
        *status_out = res.status_mapped;
    }
    if (out_stopped) {
        *out_stopped = res.stopped;
    }
    return err;
}

static void shellWaitResultFill(ShellWaitResult *res, int status) {
    res->status_raw = status;
    if (WIFSTOPPED(status)) {
        res->stopped = true;
        res->status_mapped = 128 + WSTOPSIG(status);
    } else if (WIFEXITED(status)) {
        res->status_mapped = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res->status_mapped = 128 + WTERMSIG(status);
    } else {
        res->status_mapped = status;
    }
}

static int shellWaitPidDetailed(pid_t pid, bool allow_stop, ShellWaitResult *out) {
    ShellWaitResult tmp;
    ShellWaitResult *res = out ? out : &tmp;
    memset(res, 0, sizeof(*res));
    res->pid = pid;

    int options = allow_stop ? WUNTRACED : 0;
    int status = 0;
    pid_t waited;
    do {
        waited = vprocWaitPidShim(pid, &status, options);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return errno;
    }

    shellWaitResultFill(res, status);
    return 0;
}

/* For the wait builtin, which unlike a foreground command ends early: *signo_out says for which
 * signal, with no result and the job still running. A signal does not wake a blocking wait for
 * this: the trap's handler restarts it, and on iOS nothing reaches it. Polling is slower, so only a
 * shell with a trap does it: on iOS, with none, a signal that ends the shell takes effect when the
 * wait is over. */
static int shellWaitPidForBuiltin(pid_t pid, ShellWaitResult *res, int *signo_out) {
    *signo_out = 0;
    if (!shellRuntimeAnySignalTrapped()) {
        return shellWaitPidDetailed(pid, false, res);
    }
    memset(res, 0, sizeof(*res));
    res->pid = pid;
    useconds_t delay = 200;
    while (true) {
        int status = 0;
        pid_t waited = vprocWaitPidShim(pid, &status, WNOHANG);
        if (waited > 0) {
            shellWaitResultFill(res, status);
            return 0;
        }
        if (waited < 0 && errno != EINTR) {
            return errno;
        }
        *signo_out = shellRuntimeSignalEndingWait();
        if (*signo_out > 0) {
            return 0;
        }
        usleep(delay);
        if (delay < 20000) {
            delay *= 2;
        }
    }
}

#if defined(PSCAL_TARGET_IOS)
static void shellFdPrintf(int fd, const char *fmt, ...);
#endif

#if defined(PSCAL_TARGET_IOS)
static const char *shellFormatCommandSummary(const ShellCommand *cmd, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return NULL;
    }
    buf[0] = '\0';
    if (!cmd || cmd->argc <= 0 || !cmd->argv) {
        return NULL;
    }
    size_t used = 0;
    for (size_t i = 0; i < cmd->argc && used + 1 < buf_len; ++i) {
        const char *part = cmd->argv[i] ? cmd->argv[i] : "";
        size_t len = strlen(part);
        if (used + len + 1 >= buf_len) {
            len = buf_len - used - 1;
        }
        memcpy(buf + used, part, len);
        used += len;
        if (used + 1 < buf_len && i + 1 < cmd->argc) {
            buf[used++] = ' ';
        }
    }
    buf[used] = '\0';
    return (buf[0] != '\0') ? buf : NULL;
}

static void shellPrintJobSuspended(const ShellJob *job, const ShellCommand *cmd) {
    const char *command = (job && job->command) ? job->command : NULL;
    char summary[128];
    if (!command) {
        command = shellFormatCommandSummary(cmd, summary, sizeof(summary));
    }
    if (!command) {
        command = "unknown";
    }
#if defined(PSCAL_TARGET_IOS)
    if (shellRuntimeIsInteractive()) {
        shellFdPrintf(STDERR_FILENO, "^Z\n");
    }
#endif
    shellFdPrintf(STDERR_FILENO, "exsh: suspended  %s\n", command);
}

static void shellPrintJobStarted(const ShellJob *job, pid_t pid) {
    if (pid <= 0) {
        return;
    }
    /* As under bash: a prompt announces its own jobs, on stderr, and a subshell or a substitution
     * announces none. */
    if (!shellRuntimeIsInteractive() || gShellRuntime.in_subshell || gShellCommandSubFrame) {
        return;
    }
    size_t job_id = job ? job->id : 0;
    if (job_id == 0) {
        job_id = (size_t)vprocGetJobId((int)pid);
    }
    if (job_id == 0) {
        return;
    }
    shellFdPrintf(STDERR_FILENO, "[%zu] %d\n", job_id, (int)pid);
}
#endif

#if defined(PSCAL_TARGET_IOS)
static bool shellCommandIsFrontendToolName(const char *name) {
    if (!name || !*name) {
        return false;
    }
    return strcasecmp(name, "pascal") == 0 ||
           strcasecmp(name, "dascal") == 0 ||
           strcasecmp(name, "clike") == 0 ||
           strcasecmp(name, "rea") == 0 ||
           strcasecmp(name, "pscalvm") == 0 ||
           strcasecmp(name, "pscaljson2bc") == 0 ||
           strcasecmp(name, "pscald") == 0 ||
           strcasecmp(name, "pscalasm") == 0;
}

static bool shellCommandResolvesToShebang(const char *name) {
    if (!name || !*name) {
        return false;
    }
    char resolved[PATH_MAX];
    const char *path = name;
    char *resolved_path = NULL;
    if (strchr(name, '/') == NULL) {
        resolved_path = shellFindExecutableOnPath(name);
        if (resolved_path && *resolved_path) {
            path = resolved_path;
        }
    }
    /* Probe the command path directly first; some iOS path rewrites produce
     * display paths that are not suitable for open(2), so we keep the
     * original candidate in the decision. */
    bool shebang = shellPathHasShebang(path);
    if (!shebang && shellResolveExecutableCandidate(path, resolved, sizeof(resolved))) {
        shebang = shellPathHasShebang(resolved) || shellPathHasShebang(path);
    }
    free(resolved_path);
    return shebang;
}

static bool shellShouldRunCommandInForegroundWorker(const ShellCommand *cmd) {
    bool dbg = getenv("PSCALI_PIPE_DEBUG") != NULL;
    if (!cmd || !cmd->argv || !cmd->argv[0]) {
        return false;
    }
    if (cmd->background) {
        if (dbg) {
            fprintf(stderr, "[pipe-worker] prefer=0 reason=background cmd=%s\n", cmd->argv[0]);
        }
        return false;
    }
    if (cmd->body) {
        return true;
    }
    /* A function runs on the shell's own state, so never on a worker, whatever its name. */
    if (!cmd->ignore_functions && shellFunctionIsDefined(cmd->argv[0])) {
        return false;
    }
    const char *name_raw = cmd->argv[0];
    char lookup_buf[PATH_MAX];
    const char *name = shellCommandLookupName(name_raw, lookup_buf, sizeof(lookup_buf));
    if (shellCommandIsFrontendToolName(name)) {
        if (dbg) {
            fprintf(stderr, "[pipe-worker] prefer=1 reason=frontend-tool cmd=%s\n", name);
        }
        return true;
    }
    /* Keep shell builtins (fg/bg/jobs/cd/etc.) on the shell thread; they
     * manipulate shell-global state and must not run inside stage workers. */
    if (shellIsBuiltinName(name)) {
        if (dbg) {
            fprintf(stderr, "[pipe-worker] prefer=0 reason=builtin cmd=%s\n", name);
        }
        return false;
    }
    if (smallclueFindApplet(name) != NULL) {
        /* Keep interactive full-screen editors on the shell thread so
         * SIGWINCH/foreground PTY state tracks the active UI session. */
        if (strcasecmp(name, "micro") == 0 ||
            strcasecmp(name, "nextvi") == 0 ||
            strcasecmp(name, "vi") == 0) {
            if (dbg) {
                fprintf(stderr, "[pipe-worker] prefer=0 reason=interactive-applet cmd=%s\n", name);
            }
            return false;
        }
        if (dbg) {
            fprintf(stderr, "[pipe-worker] prefer=1 reason=smallclue cmd=%s\n", name);
        }
        return true;
    }
    if (shellCommandResolvesToShebang(name_raw)) {
        if (dbg) {
            fprintf(stderr, "[pipe-worker] prefer=1 reason=shebang cmd=%s\n", name_raw);
        }
        return true;
    }
    if (dbg) {
        fprintf(stderr, "[pipe-worker] prefer=0 reason=default cmd=%s\n", name_raw);
    }
    return false;
}
#endif

#if !defined(SHELL_TOOL_THREAD_STACK_SZ)
#define SHELL_TOOL_THREAD_STACK_SZ (8 * 1024 * 1024)
#endif

#if defined(PSCAL_TARGET_IOS)
typedef struct PSCALRuntimeContext PSCALRuntimeContext;

typedef struct {
    int argc;
    char **argv;
} ShellShebangThreadCtx;

typedef struct {
    VM *vm;
    ShellCommand *cmd;
    int synthetic_pid;
#if defined(PSCAL_TARGET_IOS)
    VProc *vp;
    ShellRuntimeState *bg_shell_ctx;
    VM *bg_vm;
    int shell_pid;
    int parent_pid;
    int kernel_pid;
    PSCALRuntimeContext *runtime_ctx;
    PSCALRuntimeContext *prev_runtime_ctx;
    bool runtime_ctx_swapped;
    bool prev_bg_flag;
    bool prev_worker_flag;
    ShellRuntimeState *prev_shell_ctx;
    VM *prev_vm;
    ShellThreadLink link;
#endif
} ShellBackgroundStageCtx;

#if defined(PSCAL_TARGET_IOS)
extern PSCALRuntimeContext *PSCALRuntimeGetCurrentRuntimeContext(void) __attribute__((weak));
extern void PSCALRuntimeSetCurrentRuntimeContext(PSCALRuntimeContext *ctx) __attribute__((weak));

typedef struct {
    VM *vm;
    ShellCommand *cmd;
    VProc *vp;
    VProcSessionStdio *session_stdio;
    PSCALRuntimeContext *runtime_ctx;
    PSCALRuntimeContext *prev_runtime_ctx;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    bool stdin_is_vproc;
    bool stdout_is_vproc;
    bool stderr_is_vproc;
    bool apply_exec_redirs;
    int shell_pid;
    int kernel_pid;
    int status;
    bool prev_bg_flag;
    bool prev_worker_flag;
    bool stopped;
    pthread_mutex_t stop_mu;
    pthread_cond_t stop_cv;
    ShellThreadLink link;
    char shell_cwd[PATH_MAX];
} ShellInProcessWorkerCtx;

typedef struct {
    VM *vm;
    ShellPipelineContext *ctx;
    ShellCommand *cmd;
    size_t stage_pos;
    bool pipeline_head;
    bool pipeline_tail;
    bool owns_ctx_snapshot;
    VProcSessionStdio *session_stdio;
#if defined(PSCAL_TARGET_IOS)
    PSCALRuntimeContext *runtime_ctx;
    ShellThreadLink link;
#endif
} ShellPipelineStageThreadCtx;
#endif

#if defined(PSCAL_TARGET_IOS)
static void shellVprocSigintHandler(int signo) {
    (void)signo;
    pscalRuntimeRequestSigint();
}

static bool shellStatusRepresentsStopSignal(int status, int *out_signal) {
    if (out_signal) {
        *out_signal = 0;
    }
    if (status < 128 || status >= 128 + NSIG) {
        return false;
    }
    int sig = status - 128;
    switch (sig) {
#ifdef SIGTSTP
        case SIGTSTP:
#endif
#ifdef SIGSTOP
        case SIGSTOP:
#endif
#ifdef SIGTTIN
        case SIGTTIN:
#endif
#ifdef SIGTTOU
        case SIGTTOU:
#endif
            if (out_signal) {
                *out_signal = sig;
            }
            return true;
        default:
            return false;
    }
}

/* keep_status leaves the exit for a waiter, who reaps it. */
static void shellFinalizeStageVprocEx(VProc *vp, int status, bool keep_status) {
    if (!vp) {
        return;
    }
    int pid = vprocPid(vp);
    int stop_sig = 0;
    bool stopped = shellStatusRepresentsStopSignal(status, &stop_sig);

    if (stopped && pid > 0) {
        /* Keep stopped tasks in the vproc table, but detach this shell thread
         * identity so later signal routing does not bleed into the shell. */
        vprocUnregisterThread(vp, pthread_self());
        /* Cooperative in-process stages may keep stop_unsupported enabled while
         * running. Re-enable hard-stop semantics only for teardown so a
         * resumable stopped task is published to jobs/wait consumers. */
        vprocSetStopUnsupported(pid, false);
        if (stop_sig > 0) {
            (void)vprocKillShim(pid, stop_sig);
        }
    } else {
        vprocCloseDescriptors(vp);
        vprocMarkExit(vp, status);
        if (pid > 0 && !keep_status) {
            vprocDiscard(pid);
        }
    }
    vprocDestroy(vp);
}

static void shellFinalizeStageVproc(VProc *vp, int status) {
    shellFinalizeStageVprocEx(vp, status, false);
}

/* The thread running this stage was cancelled with the stage's job. Nothing else would end the
 * stage or close what it holds, such as the write end of a pipe. */
static void shellInlineStageCancelled(void *arg) {
    vprocEndAbandoned((VProc *)arg);
    vprocDeactivate();
    shellFinalizeStageVproc((VProc *)arg, 128 + SIGKILL);
}

static void shellInProcessWorkerCleanup(void *arg) {
    ShellInProcessWorkerCtx *ctx = (ShellInProcessWorkerCtx *)arg;
    if (!ctx) {
        return;
    }
    /* This also runs on the normal way out, where a kill could stop it half done. */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_mutex_destroy(&ctx->stop_mu);
    pthread_cond_destroy(&ctx->stop_cv);

    int stdin_fd = ctx->stdin_fd;
    int stdout_fd = ctx->stdout_fd;
    int stderr_fd = ctx->stderr_fd;
    bool stdin_is_vproc = ctx->stdin_is_vproc;
    bool stdout_is_vproc = ctx->stdout_is_vproc;
    bool stderr_is_vproc = ctx->stderr_is_vproc;
    ctx->stdin_fd = -1;
    ctx->stdout_fd = -1;
    ctx->stderr_fd = -1;
    if (stdin_fd >= 0) {
        if (stdin_is_vproc) {
            vprocCloseShim(stdin_fd);
        } else {
            vprocHostClose(stdin_fd);
        }
    }
    if (stdout_fd >= 0 && stdout_fd != stdin_fd) {
        if (stdout_is_vproc) {
            vprocCloseShim(stdout_fd);
        } else {
            vprocHostClose(stdout_fd);
        }
    }
    if (stderr_fd >= 0 && stderr_fd != stdin_fd && stderr_fd != stdout_fd) {
        if (stderr_is_vproc) {
            vprocCloseShim(stderr_fd);
        } else {
            vprocHostClose(stderr_fd);
        }
    }

    vprocDeactivate();
    /* The shell always waits for this one, and it may lose the race to get there. */
    shellFinalizeStageVprocEx(ctx->vp, ctx->status, true);
    ctx->vp = NULL;

    gShellBackgroundWorkerActive = ctx->prev_worker_flag;
    gShellCurrentBuiltinBackground = ctx->prev_bg_flag;
    if (ctx->link.ctx) {
        shellRuntimeDestroyContext(ctx->link.ctx);
        ctx->link.ctx = NULL;
    }
    shellThreadLinkRelease(&ctx->link);

    if (ctx->session_stdio) {
        vprocSessionStdioActivate(NULL);
    }
    if (PSCALRuntimeSetCurrentRuntimeContext && ctx->prev_runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(ctx->prev_runtime_ctx);
    }
    shellFreeCommand(ctx->cmd);
    free(ctx->cmd);
    free(ctx);
}

static void __attribute__((unused)) shellInstallVprocSigintHandler(VProc *vp) {
    if (!vp) {
        return;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = shellVprocSigintHandler;
    (void)vprocSigaction(vprocPid(vp), SIGINT, &sa, NULL);
}

#endif
static void shellRedirectionFailed(const char *target, int err);

#if defined(PSCAL_TARGET_IOS)
static void shellInProcessBackgroundCancelCleanup(void *arg) {
    ShellBackgroundStageCtx *ctx = (ShellBackgroundStageCtx *)arg;
    if (!ctx) {
        return;
    }

    if (ctx->runtime_ctx_swapped && PSCALRuntimeSetCurrentRuntimeContext) {
        PSCALRuntimeSetCurrentRuntimeContext(ctx->prev_runtime_ctx);
        ctx->runtime_ctx_swapped = false;
        ctx->prev_runtime_ctx = NULL;
    }
    if (ctx->prev_vm || ctx->bg_vm) {
        shellRestoreCurrentVm(ctx->prev_vm);
        ctx->prev_vm = NULL;
    }
    if (ctx->prev_shell_ctx) {
        shellRuntimeActivateContext(ctx->prev_shell_ctx);
        ctx->prev_shell_ctx = NULL;
    }
    gShellBackgroundWorkerActive = ctx->prev_worker_flag;
    gShellCurrentBuiltinBackground = ctx->prev_bg_flag;

    if (ctx->vp) {
        vprocEndAbandoned(ctx->vp);
        vprocDeactivate();
        /* `wait` asks for the status, and this thread can get here before it does. The job
         * table reaps it, as on the normal way out. A signal's status is already recorded. */
        shellFinalizeStageVprocEx(ctx->vp, 128 + SIGKILL, true);
        ctx->vp = NULL;
    }
    if (ctx->bg_vm) {
        freeVM(ctx->bg_vm);
        free(ctx->bg_vm);
        ctx->bg_vm = NULL;
    }
    if (ctx->bg_shell_ctx) {
        free(ctx->bg_shell_ctx);
        ctx->bg_shell_ctx = NULL;
    }

    if (ctx->cmd) {
        shellFreeCommand(ctx->cmd);
        free(ctx->cmd);
        ctx->cmd = NULL;
    }
    free(ctx);
}
#endif

static int shellRunBuiltinInProcess(VM *vm,
                                    ShellCommand *cmd,
                                    int stdin_fd,
                                    int stdout_fd,
                                    int stderr_fd,
                                    bool apply_exec_redirs);

static ShellCommand *shellCloneCommandForBackground(const ShellCommand *src) {
    if (!src) {
        return NULL;
    }
    ShellCommand *dst = (ShellCommand *)calloc(1, sizeof(ShellCommand));
    if (!dst) {
        return NULL;
    }

    dst->argc = src->argc;
    if (dst->argc > 0) {
        dst->argv = (char **)calloc(dst->argc + 1, sizeof(char *));
        if (!dst->argv) {
            shellFreeCommand(dst);
            free(dst);
            return NULL;
        }
        for (size_t i = 0; i < dst->argc; ++i) {
            if (src->argv && src->argv[i]) {
                dst->argv[i] = strdup(src->argv[i]);
                if (!dst->argv[i]) {
                    shellFreeCommand(dst);
                    free(dst);
                    return NULL;
                }
            }
        }
        dst->argv[dst->argc] = NULL;
    }

    dst->assignment_count = src->assignment_count;
    if (dst->assignment_count > 0) {
        dst->assignments = (ShellAssignmentEntry *)calloc(dst->assignment_count, sizeof(ShellAssignmentEntry));
        if (!dst->assignments) {
            shellFreeCommand(dst);
            free(dst);
            return NULL;
        }
        for (size_t i = 0; i < dst->assignment_count; ++i) {
            dst->assignments[i].is_array_literal = src->assignments[i].is_array_literal;
            if (src->assignments[i].text) {
                dst->assignments[i].text = strdup(src->assignments[i].text);
                if (!dst->assignments[i].text) {
                    shellFreeCommand(dst);
                    free(dst);
                    return NULL;
                }
            }
        }
    }

    dst->redir_count = src->redir_count;
    if (dst->redir_count > 0) {
        dst->redirs = (ShellRedirection *)calloc(dst->redir_count, sizeof(ShellRedirection));
        if (!dst->redirs) {
            shellFreeCommand(dst);
            free(dst);
            return NULL;
        }
        for (size_t i = 0; i < dst->redir_count; ++i) {
            ShellRedirection *dr = &dst->redirs[i];
            const ShellRedirection *sr = &src->redirs[i];
            *dr = *sr;
            dr->path = sr->path ? strdup(sr->path) : NULL;
            dr->here_doc = sr->here_doc ? strdup(sr->here_doc) : NULL;
            dr->here_string_literal = sr->here_string_literal ? strdup(sr->here_string_literal) : NULL;
            dr->here_string = sr->here_string ? strdup(sr->here_string) : NULL;
            dr->fd_var = sr->fd_var ? strdup(sr->fd_var) : NULL;
            if ((sr->path && !dr->path) ||
                (sr->here_doc && !dr->here_doc) ||
                (sr->here_string_literal && !dr->here_string_literal) ||
                (sr->here_string && !dr->here_string) ||
                (sr->fd_var && !dr->fd_var)) {
                shellFreeCommand(dst);
                free(dst);
                return NULL;
            }
        }
    }

    dst->background = src->background;
    dst->disable_field_splitting = src->disable_field_splitting;
    dst->ignore_functions = src->ignore_functions;
    dst->empty_stage_status = src->empty_stage_status;
    dst->pipeline_index = src->pipeline_index;
    dst->is_pipeline_head = src->is_pipeline_head;
    dst->is_pipeline_tail = src->is_pipeline_tail;
    dst->line = src->line;
    dst->column = src->column;
    dst->body = src->body;
#if defined(PSCAL_TARGET_IOS)
    dst->body_state = src->body_state;
    ((ShellCommand *)src)->body_state = NULL;
    dst->program_env = src->program_env;
    ((ShellCommand *)src)->program_env = NULL;
    dst->program_path = src->program_path;
    ((ShellCommand *)src)->program_path = NULL;
#endif
    return dst;
}

static void shellShebangThreadCtxFree(ShellShebangThreadCtx *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->argv) {
        for (int i = 0; i < ctx->argc; ++i) {
            free(ctx->argv[i]);
        }
        free(ctx->argv);
    }
    free(ctx);
}

static void *shellShebangBackgroundMain(void *arg) {
    ShellShebangThreadCtx *ctx = (ShellShebangThreadCtx *)arg;
    if (!ctx || !ctx->argv || ctx->argc <= 0) {
        shellShebangThreadCtxFree(ctx);
        return NULL;
    }
    shellMaybeExecShebangTool(ctx->argv[0], ctx->argv);
    shellShebangThreadCtxFree(ctx);
    return NULL;
}

static void *__attribute__((unused)) shellInProcessBackgroundMain(void *arg) {
    ShellBackgroundStageCtx *ctx = (ShellBackgroundStageCtx *)arg;
    if (!ctx) {
        return NULL;
    }
#if defined(PSCAL_TARGET_IOS)
    if (ctx->shell_pid > 0) {
        vprocSetShellSelfPid(ctx->shell_pid);
    }
    if (ctx->kernel_pid > 0) {
        vprocSetKernelPid(ctx->kernel_pid);
    }
#endif
    ctx->prev_bg_flag = gShellCurrentBuiltinBackground;
    ctx->prev_worker_flag = gShellBackgroundWorkerActive;
    gShellCurrentBuiltinBackground = true;
    gShellBackgroundWorkerActive = true;
    if (PSCALRuntimeSetCurrentRuntimeContext && ctx->runtime_ctx) {
        ctx->prev_runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext
            ? PSCALRuntimeGetCurrentRuntimeContext()
            : NULL;
        PSCALRuntimeSetCurrentRuntimeContext(ctx->runtime_ctx);
        ctx->runtime_ctx_swapped = true;
    }
    /* `kill` can arrive at any time from here on. Setup and teardown hold vproc state that
     * only they can finish, so a cancel waits until the command runs. */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
#if defined(PSCAL_TARGET_IOS)
    pthread_cleanup_push(shellInProcessBackgroundCancelCleanup, ctx);
    /* Ensure tool frontends are registered in this process before we spin up
     * a fresh runtime/VM for the background worker. */
    static pthread_once_t bg_builtin_once = PTHREAD_ONCE_INIT;
    pthread_once(&bg_builtin_once, registerExtendedBuiltins);
    shellThreadLinkEnter(&ctx->link);
    /* Install isolated shell runtime + VM so background work does not mutate
     * the interactive shell's state or gShellCurrentVm. */
    ShellRuntimeState *bg_ctx = ctx->link.ctx ? ctx->link.ctx : shellRuntimeCreateContext();
    ShellRuntimeState *prev_ctx = NULL;
    VM *prev_vm = NULL;
    VM *bg_vm = NULL;
    VM *vm = ctx->vm ? ctx->vm : gShellCurrentVm;
    if (bg_ctx) {
        /* As in a subshell: what its shell's jobs left is not a stage's to find. */
        bg_ctx->finished_job_count = 0;
        ctx->bg_shell_ctx = bg_ctx;
        prev_ctx = shellRuntimeActivateContext(bg_ctx);
        ctx->prev_shell_ctx = prev_ctx;
        bg_vm = (VM *)calloc(1, sizeof(VM));
        if (bg_vm) {
            initVM(bg_vm);
            bg_vm->frontendContext = bg_ctx;
            prev_vm = shellSwapCurrentVm(bg_vm);
            ctx->prev_vm = prev_vm;
            vm = bg_vm;
            ctx->bg_vm = bg_vm;
        } else {
            shellRuntimeActivateContext(prev_ctx);
            prev_ctx = NULL;
            ctx->prev_shell_ctx = NULL;
            free(bg_ctx);
            ctx->bg_shell_ctx = NULL;
            bg_ctx = NULL;
        }
    }
#else
    VM *vm = ctx->vm ? ctx->vm : gShellCurrentVm;
#endif
    ShellCommand *cmd = ctx->cmd;
    bool tool_dbg = getenv("PSCALI_TOOL_DEBUG") != NULL;
    bool pipe_dbg = getenv("PSCALI_PIPE_DEBUG") != NULL;
#if defined(PSCAL_TARGET_IOS)
    if (tool_dbg) {
        fprintf(stderr, "[bg-stage] start cmd=%s bg=%d stage_count=%zu ctx=%p\n",
                (cmd && cmd->argv && cmd->argc > 0) ? cmd->argv[0] : "<nil>",
                (int)(cmd ? cmd->background : 0),
                (size_t)(cmd ? cmd->argc : 0),
                (void *)ctx);
    }
#endif
    size_t redir_count = cmd ? cmd->redir_count : 0;
    bool redir_dbg = getenv("PSCALI_REDIR_DEBUG") != NULL;
    if (tool_dbg || redir_dbg || pipe_dbg) {
        fprintf(stderr, "[bg-stage] redir_count=%zu\n", redir_count);
        if (cmd && cmd->redirs && (redir_dbg || tool_dbg)) {
            for (size_t ri = 0; ri < redir_count; ++ri) {
                ShellRedirection *r = &cmd->redirs[ri];
                fprintf(stderr, "[bg-stage]   redir[%zu] fd=%d kind=%d path=%s dup=%d flags=0x%x\n",
                        ri, r->fd, (int)r->kind, r->path ? r->path : "<none>", r->dup_target_fd, r->flags);
            }
        }
    }
    int bg_status = 0;
#if defined(PSCAL_TARGET_IOS)
    VProc *vp = ctx->vp;
#endif
    /* Each redirection acts in the order written, on the job's own descriptors, as in a forked
     * child. Nothing is saved, because the job never goes back to what it had: `cmd > /dev/null &`
     * lets go of the caller's stdout. A failure is reported with what came before it in effect,
     * and `wait` gets the status. */
    bool redirected = shellApplyExecRedirections(cmd, NULL, NULL);
    /* They are done, and what runs the command must not open them again. */
    shellFreeRedirections(cmd);
    if (tool_dbg && pipe_dbg) {
        VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
        fprintf(stderr, "[bg-stage] about to run builtin session_out=%d session_err=%d\n",
                session_stdio ? session_stdio->stdout_host_fd : -1,
                session_stdio ? session_stdio->stderr_host_fd : -1);
    }
    if (tool_dbg && pipe_dbg) {
        int host_out = vprocTranslateFd(vp, STDOUT_FILENO);
        int host_err = vprocTranslateFd(vp, STDERR_FILENO);
        int host_in = vprocTranslateFd(vp, STDIN_FILENO);
        fprintf(stderr, "[bg-stage] pre-run fdmap stdin host=%d stdout host=%d stderr host=%d bg_flag=%d\n",
                host_in, host_out, host_err, (int)gShellCurrentBuiltinBackground);
    }
    /* Allow pthread_cancel to interrupt sleeps/IO. A kill that came during setup acts here. */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_testcancel();
    bg_status = redirected ? shellRunBuiltinInProcess(vm, cmd, -1, -1, -1, false) : 1;
    if (tool_dbg && pipe_dbg) {
        fprintf(stderr, "[bg-stage] finished run status=%d\n", bg_status);
    }
    if (tool_dbg && pipe_dbg) {
        int host_out = vprocTranslateFd(vp, STDOUT_FILENO);
        int host_err = vprocTranslateFd(vp, STDERR_FILENO);
        int host_in = vprocTranslateFd(vp, STDIN_FILENO);
        fprintf(stderr, "[bg-stage] post-run fdmap stdin host=%d stdout host=%d stderr host=%d status=%d\n",
                host_in, host_out, host_err, bg_status);
    }
    gShellBackgroundWorkerActive = ctx->prev_worker_flag;

#if defined(PSCAL_TARGET_IOS)
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    if (vp) {
        /* If the synthetic task was terminated via vprocKillShim, reflect that
         * status rather than a default 0 from the builtin dispatcher. */
        size_t snap_cap = vprocSnapshot(NULL, 0);
        if (snap_cap > 0) {
            VProcSnapshot *snaps = (VProcSnapshot *)calloc(snap_cap, sizeof(VProcSnapshot));
            if (snaps) {
                size_t snap_count = vprocSnapshot(snaps, snap_cap);
                for (size_t i = 0; i < snap_count; ++i) {
                    if (snaps[i].pid == vprocPid(vp) && snaps[i].exited) {
                        bg_status = snaps[i].status;
                        break;
                    }
                }
                free(snaps);
            }
        }
    }
    if (getenv("PSCALI_PIPE_DEBUG")) {
        fprintf(stderr, "[pipeline-bg] command '%s' status=%d\n",
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>", bg_status);
    }
    vprocDeactivate();
    if (vp) {
        /* `wait` asks for the status. The job table reaps it, as it would a process. */
        shellFinalizeStageVprocEx(vp, bg_status, true);
        ctx->vp = NULL;
    }
#endif
    shellFreeCommand(cmd);
    free(cmd);
#if defined(PSCAL_TARGET_IOS)
    /* Restore VM/runtime context now that the background task is done. */
    if (bg_vm || prev_vm) {
        shellRestoreCurrentVm(prev_vm);
        ctx->prev_vm = NULL;
    }
    if (prev_ctx) {
        shellRuntimeActivateContext(prev_ctx);
        ctx->prev_shell_ctx = NULL;
    }
    if (bg_vm) {
        freeVM(bg_vm);
        free(bg_vm);
        ctx->bg_vm = NULL;
    }
    if (bg_ctx) {
        if (ctx->link.ctx == bg_ctx) {
            shellRuntimeDestroyContext(bg_ctx);
            ctx->link.ctx = NULL;
        } else {
            free(bg_ctx);
        }
        ctx->bg_shell_ctx = NULL;
    }
    shellThreadLinkRelease(&ctx->link);
    if (ctx->runtime_ctx_swapped && PSCALRuntimeSetCurrentRuntimeContext) {
        PSCALRuntimeSetCurrentRuntimeContext(ctx->prev_runtime_ctx);
        ctx->runtime_ctx_swapped = false;
        ctx->prev_runtime_ctx = NULL;
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    pthread_cleanup_pop(0);
#endif
    gShellCurrentBuiltinBackground = ctx->prev_bg_flag;
    free(ctx);
    return NULL;
}

static void *shellInProcessWorkerMain(void *arg) {
    ShellInProcessWorkerCtx *ctx = (ShellInProcessWorkerCtx *)arg;
    if (!ctx || !ctx->vp || !ctx->cmd) {
        free(ctx);
        return NULL;
    }
    pthread_mutex_init(&ctx->stop_mu, NULL);
    pthread_cond_init(&ctx->stop_cv, NULL);
    ctx->status = W_EXITCODE(128 + SIGINT, 0);
    ctx->prev_runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext
        ? PSCALRuntimeGetCurrentRuntimeContext()
        : NULL;
    if (PSCALRuntimeSetCurrentRuntimeContext && ctx->runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(ctx->runtime_ctx);
    }
    if (ctx->session_stdio) {
        vprocSessionStdioActivate(ctx->session_stdio);
    }
    if (ctx->shell_pid > 0) {
        vprocSetShellSelfPid(ctx->shell_pid);
    }
    if (ctx->kernel_pid > 0) {
        vprocSetKernelPid(ctx->kernel_pid);
    }

    ctx->prev_bg_flag = gShellCurrentBuiltinBackground;
    ctx->prev_worker_flag = gShellBackgroundWorkerActive;
    gShellCurrentBuiltinBackground = false;
    gShellBackgroundWorkerActive = true;
    shellThreadLinkEnter(&ctx->link);
    if (ctx->link.ctx) {
        shellRuntimeActivateContext(ctx->link.ctx);
    }

    pthread_cleanup_push(shellInProcessWorkerCleanup, ctx);

    vprocRegisterThread(ctx->vp, pthread_self());
    vprocActivate(ctx->vp);
#if defined(PSCAL_TARGET_IOS)
    {
        int worker_pid = vprocPid(ctx->vp);
        if (worker_pid > 0) {
            int job_mask = 0;
#ifdef SIGTSTP
            job_mask |= (1 << SIGTSTP);
#endif
#ifdef SIGTTIN
            job_mask |= (1 << SIGTTIN);
#endif
#ifdef SIGTTOU
            job_mask |= (1 << SIGTTOU);
#endif
            if (job_mask != 0) {
                (void)vprocDefaultSignal(worker_pid, job_mask);
                (void)vprocUnblockSignals(worker_pid, job_mask);
            }
        }
    }
    /*
     * Keep worker path resolution aligned with the parent shell session.
     * This prevents reused worker threads from carrying stale cwd between tabs.
     */
    if (ctx->shell_cwd[0] != '\0') {
        (void)vprocChdirShim(ctx->shell_cwd);
    }
#endif

    ctx->status = shellRunBuiltinInProcess(ctx->vm,
                                           ctx->cmd,
                                           ctx->stdin_fd,
                                           ctx->stdout_fd,
                                           ctx->stderr_fd,
                                           ctx->apply_exec_redirs);

    pthread_cleanup_pop(1);
    return NULL;
}
#endif

static int shellFinishPipeline(const ShellCommand *tail_cmd);

#if defined(PSCAL_TARGET_IOS)
static bool shellDupOntoTarget(VProc *vp, int src_fd, int target_fd, bool dbg, const char *label);
static void shellPipelineSessionCloseSpool(ShellPipelineContext *ctx) {
    if (!ctx) {
        return;
    }
    if (PIPE_SPOOL_FD(ctx) >= 0 && PIPE_SPOOL_OWNED(ctx)) {
        vprocHostClose(PIPE_SPOOL_FD(ctx));
    }
    PIPE_SPOOL_FD(ctx) = -1;
    PIPE_SPOOL_SIZE(ctx) = 0;
    PIPE_SPOOL_OWNED(ctx) = false;
}

static void shellPipelineSessionCloseStdStreams(ShellPipelineContext *ctx) {
    if (!ctx) {
        return;
    }
    if (PIPE_STDIN_FD(ctx) >= 0 && PIPE_STDIN_OWNED(ctx)) {
        vprocHostClose(PIPE_STDIN_FD(ctx));
    }
    if (PIPE_STDOUT_FD(ctx) >= 0 && PIPE_STDOUT_OWNED(ctx)) {
        vprocHostClose(PIPE_STDOUT_FD(ctx));
    }
    if (PIPE_STDERR_FD(ctx) >= 0 && PIPE_STDERR_OWNED(ctx)) {
        vprocHostClose(PIPE_STDERR_FD(ctx));
    }
    PIPE_STDIN_FD(ctx) = -1;
    PIPE_STDOUT_FD(ctx) = -1;
    PIPE_STDERR_FD(ctx) = -1;
    PIPE_STDIN_OWNED(ctx) = false;
    PIPE_STDOUT_OWNED(ctx) = false;
    PIPE_STDERR_OWNED(ctx) = false;
}

static void shellPipelineSessionRestoreStdStreams(ShellPipelineContext *ctx) {
    if (!ctx) {
        return;
    }
    if (PIPE_STDIN_FD(ctx) >= 0) {
        vprocHostDup2(PIPE_STDIN_FD(ctx), STDIN_FILENO);
        shellResetStandardStream(STDIN_FILENO);
    }
    if (PIPE_STDOUT_FD(ctx) >= 0) {
        vprocHostDup2(PIPE_STDOUT_FD(ctx), STDOUT_FILENO);
        shellResetStandardStream(STDOUT_FILENO);
    }
    if (PIPE_STDERR_FD(ctx) >= 0) {
        vprocHostDup2(PIPE_STDERR_FD(ctx), STDERR_FILENO);
        shellResetStandardStream(STDERR_FILENO);
    }
    shellPipelineSessionCloseStdStreams(ctx);
}

static void shellPipelineSessionSnapshotStdStreams(ShellPipelineContext *ctx) {
    if (!ctx) {
        return;
    }
    shellPipelineSessionCloseStdStreams(ctx);
    /* A worker's redirections reach only its own slots, so it has nothing to put back. It
     * would be putting back the shell's descriptors as they were when it looked: a pipe of
     * a substitution that has ended, a copy of which keeps that substitution's reader
     * waiting until now. */
    if (gShellBackgroundWorkerActive) {
        return;
    }
    PIPE_STDIN_FD(ctx) = vprocHostDup(STDIN_FILENO);
    PIPE_STDOUT_FD(ctx) = vprocHostDup(STDOUT_FILENO);
    PIPE_STDERR_FD(ctx) = vprocHostDup(STDERR_FILENO);
    PIPE_STDIN_OWNED(ctx) = PIPE_STDIN_FD(ctx) >= 0;
    PIPE_STDOUT_OWNED(ctx) = PIPE_STDOUT_FD(ctx) >= 0;
    PIPE_STDERR_OWNED(ctx) = PIPE_STDERR_FD(ctx) >= 0;
}
#endif

static void shellResetPipeline(void) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return;
    }
#if defined(PSCAL_TARGET_IOS)
    shellPipelineSessionRestoreStdStreams(ctx);
    shellPipelineSessionCloseSpool(ctx);
    ctx->in_process = false;
    ctx->in_process_worker = false;
    ctx->session_stdio = NULL;
    if (ctx->pscal_pipe_read || ctx->pscal_pipe_write || ctx->pscal_pipe_write_hold) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pscal_pipe_read && ctx->pscal_pipe_read[i]) {
                pscal_fd_close(ctx->pscal_pipe_read[i]);
                ctx->pscal_pipe_read[i] = NULL;
            }
            if (ctx->pscal_pipe_write && ctx->pscal_pipe_write[i]) {
                pscal_fd_close(ctx->pscal_pipe_write[i]);
                ctx->pscal_pipe_write[i] = NULL;
            }
            if (ctx->pscal_pipe_write_hold && ctx->pscal_pipe_write_hold[i]) {
                pscal_fd_close(ctx->pscal_pipe_write_hold[i]);
                ctx->pscal_pipe_write_hold[i] = NULL;
            }
        }
        free(ctx->pscal_pipe_read);
        free(ctx->pscal_pipe_write);
        free(ctx->pscal_pipe_write_hold);
        ctx->pscal_pipe_read = NULL;
        ctx->pscal_pipe_write = NULL;
        ctx->pscal_pipe_write_hold = NULL;
    }
    if (ctx->threads) {
        free(ctx->threads);
        ctx->threads = NULL;
    }
    if (ctx->thread_started) {
        free(ctx->thread_started);
        ctx->thread_started = NULL;
    }
    if (ctx->stage_status) {
        free(ctx->stage_status);
        ctx->stage_status = NULL;
    }
#endif
    if (ctx->pipes) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pipes[i][0] >= 0) {
                vprocHostClose(ctx->pipes[i][0]);
                ctx->pipes[i][0] = -1;
            }
            if (ctx->pipes[i][1] >= 0) {
                vprocHostClose(ctx->pipes[i][1]);
                ctx->pipes[i][1] = -1;
            }
        }
        free(ctx->pipes);
        ctx->pipes = NULL;
    }
    free(ctx->pids);
    ctx->pids = NULL;
    free(ctx->merge_stderr);
    ctx->merge_stderr = NULL;
    ctx->active = false;
    ctx->stage_count = 0;
    ctx->launched = 0;
    ctx->background = false;
    ctx->last_status = 0;
    ctx->pgid = -1;
}

static void shellMaybeResetPipeline(void) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return;
    }
    if (ctx->stage_count > 1) {
        shellFinishPipeline(NULL);
        return;
    }
    shellResetPipeline();
}

static void shellAbortPipeline(void) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return;
    }

#if defined(PSCAL_TARGET_IOS)
    shellPipelineSessionRestoreStdStreams(ctx);
    shellPipelineSessionCloseSpool(ctx);
#endif
#if defined(PSCAL_TARGET_IOS)
    if (ctx->pscal_pipe_read || ctx->pscal_pipe_write || ctx->pscal_pipe_write_hold) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pscal_pipe_read && ctx->pscal_pipe_read[i]) {
                pscal_fd_close(ctx->pscal_pipe_read[i]);
                ctx->pscal_pipe_read[i] = NULL;
            }
            if (ctx->pscal_pipe_write && ctx->pscal_pipe_write[i]) {
                pscal_fd_close(ctx->pscal_pipe_write[i]);
                ctx->pscal_pipe_write[i] = NULL;
            }
            if (ctx->pscal_pipe_write_hold && ctx->pscal_pipe_write_hold[i]) {
                pscal_fd_close(ctx->pscal_pipe_write_hold[i]);
                ctx->pscal_pipe_write_hold[i] = NULL;
            }
        }
        free(ctx->pscal_pipe_read);
        free(ctx->pscal_pipe_write);
        free(ctx->pscal_pipe_write_hold);
        ctx->pscal_pipe_read = NULL;
        ctx->pscal_pipe_write = NULL;
        ctx->pscal_pipe_write_hold = NULL;
    }
    if (ctx->thread_started && ctx->threads) {
        for (size_t i = 0; i < ctx->stage_count; ++i) {
            if (ctx->thread_started[i]) {
                pthread_join(ctx->threads[i], NULL);
                ctx->thread_started[i] = false;
            }
        }
    }
#endif
    if (ctx->pipes) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pipes[i][0] >= 0) {
                vprocHostClose(ctx->pipes[i][0]);
                ctx->pipes[i][0] = -1;
            }
            if (ctx->pipes[i][1] >= 0) {
                vprocHostClose(ctx->pipes[i][1]);
                ctx->pipes[i][1] = -1;
            }
        }
    }

    for (size_t i = 0; i < ctx->launched; ++i) {
        pid_t pid = ctx->pids[i];
        if (pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t res = -1;
        do {
            res = vprocWaitPidShim(pid, &status, WNOHANG);
        } while (res < 0 && errno == EINTR);
        if (res == 0) {
            vprocKillShim(pid, SIGTERM);
            do {
                res = vprocWaitPidShim(pid, &status, 0);
            } while (res < 0 && errno == EINTR);
        }
    }

    shellResetPipeline();
}

static bool shellEnsurePipeline(size_t stages, bool negated) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    shellResetPipeline();
        ctx->stage_count = stages;
        ctx->negated = negated;
        ctx->active = true;
        ctx->launched = 0;
        ctx->last_status = 0;
        ctx->background = false;
        ctx->pgid = -1;
#if defined(PSCAL_TARGET_IOS)
        ctx->in_process = true;
        ctx->in_process_worker = false;
        ctx->session_stdio = vprocSessionStdioCurrent();
        /* Asked here, by the vproc's owner. A stage on a thread of its own has only borrowed
         * that vproc, which is gone the moment the owner is killed. */
        ctx->window_cols = pscalRuntimeDetectWindowCols();
        ctx->window_rows = pscalRuntimeDetectWindowRows();
        if (gShellRuntime.in_subshell && vprocCurrent()) {
            /* Its stages stay in the subshell's group, which the terminal signals. */
            ctx->pgid = vprocGetPgid(vprocPid(vprocCurrent()));
        }
        shellPipelineSessionCloseSpool(ctx);
        shellPipelineSessionSnapshotStdStreams(ctx);
//    fprintf(stderr, "[pipe] ensure stages=%zu in_process=1\n", stages);
#endif
    ctx->pids = calloc(stages, sizeof(pid_t));
    if (!ctx->pids) {
        shellResetPipeline();
        return false;
    }
    if (stages > 0) {
        ctx->merge_stderr = (bool *)calloc(stages, sizeof(bool));
        if (!ctx->merge_stderr) {
            shellResetPipeline();
            return false;
        }
    }
    if (stages > 1) {
#if defined(PSCAL_TARGET_IOS)
        /* iOS default: use in-proc PSCAL pipes. Host pipes can be forced with
         * `PSCALI_PIPE_PSCAL=0` for diagnostics. */
        bool use_pscal_pipes = true;
        const char *pipe_mode = getenv("PSCALI_PIPE_PSCAL");
        if (pipe_mode && *pipe_mode) {
            char ch = pipe_mode[0];
            if (ch == '0' || ch == 'n' || ch == 'N' || ch == 'f' || ch == 'F') {
                use_pscal_pipes = false;
            }
        }
        if (getenv("PSCALI_PIPE_DEBUG")) {
            fprintf(stderr, "[pipe] iOS mode=%s stages=%zu\n",
                    use_pscal_pipes ? "pscal" : "host",
                    stages);
        }
        ctx->pscal_pipe_read = NULL;
        ctx->pscal_pipe_write = NULL;
        ctx->pscal_pipe_write_hold = NULL;
        ctx->pipes = calloc(stages - 1, sizeof(int[2]));
        if (!ctx->pipes) {
            shellResetPipeline();
            return false;
        }
        for (size_t i = 0; i < stages - 1; ++i) {
            ctx->pipes[i][0] = -1;
            ctx->pipes[i][1] = -1;
            if (!use_pscal_pipes) {
                if (vprocHostPipe(ctx->pipes[i]) != 0) {
                    shellResetPipeline();
                    return false;
                }
            }
        }
        if (use_pscal_pipes) {
            ctx->pscal_pipe_read = (struct pscal_fd **)calloc(stages - 1, sizeof(struct pscal_fd *));
            ctx->pscal_pipe_write = (struct pscal_fd **)calloc(stages - 1, sizeof(struct pscal_fd *));
            ctx->pscal_pipe_write_hold = (struct pscal_fd **)calloc(stages - 1, sizeof(struct pscal_fd *));
            if (!ctx->pscal_pipe_read || !ctx->pscal_pipe_write || !ctx->pscal_pipe_write_hold) {
                shellResetPipeline();
                return false;
            }
            for (size_t i = 0; i < stages - 1; ++i) {
                if (vprocCreateInprocPipe(&ctx->pscal_pipe_read[i], &ctx->pscal_pipe_write[i]) != 0 ||
                    !ctx->pscal_pipe_read[i] || !ctx->pscal_pipe_write[i]) {
                    shellResetPipeline();
                    return false;
                }
                ctx->pscal_pipe_write_hold[i] = pscal_fd_retain(ctx->pscal_pipe_write[i]);
                if (!ctx->pscal_pipe_write_hold[i]) {
                    shellResetPipeline();
                    return false;
                }
            }
        }
#else
        ctx->pipes = calloc(stages - 1, sizeof(int[2]));
        if (!ctx->pipes) {
            shellResetPipeline();
            return false;
        }
        for (size_t i = 0; i < stages - 1; ++i) {
            ctx->pipes[i][0] = -1;
            ctx->pipes[i][1] = -1;
            if (vprocHostPipe(ctx->pipes[i]) != 0) {
                shellResetPipeline();
                return false;
            }
        }
#endif
    }
#if defined(PSCAL_TARGET_IOS)
    if (stages > 0) {
        ctx->threads = (pthread_t *)calloc(stages, sizeof(pthread_t));
        ctx->thread_started = (bool *)calloc(stages, sizeof(bool));
        ctx->stage_status = (int *)calloc(stages, sizeof(int));
        if (!ctx->threads || !ctx->thread_started || !ctx->stage_status) {
            shellResetPipeline();
            return false;
        }
        for (size_t i = 0; i < stages; ++i) {
            ctx->thread_started[i] = false;
            ctx->stage_status[i] = 0;
        }
    }
#endif
    return true;
}

/* The pipeline's status so far, given the next stage's: the last stage's, or under
 * pipefail the last one's that failed. */
static int shellPipelineNextStatus(int status, int stage_status) {
    return (gShellRuntime.pipefail_enabled && stage_status == 0) ? status : stage_status;
}

static int shellFinishPipeline(const ShellCommand *tail_cmd) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return gShellRuntime.last_status;
    }

    int final_status = ctx->last_status;
    pid_t job_pgid = (ctx->pgid > 0) ? ctx->pgid : ((ctx->launched > 0) ? ctx->pids[0] : -1);

#if defined(PSCAL_TARGET_IOS)
    if (ctx->in_process) {
        if (ctx->in_process_worker && !ctx->background) {
            shellEnsureJobControl();
            bool job_control = gShellRuntime.job_control_enabled && job_pgid > 0;
            bool stopped_job = false;
            bool saw_state = false;

            if (job_control) {
                shellJobControlSetForeground(job_pgid);
            }

            for (size_t i = 0; i < ctx->launched; ++i) {
                pid_t pid = (ctx->pids && i < ctx->stage_count) ? ctx->pids[i] : -1;
                if (pid <= 0) {
                    continue;
                }
                ShellWaitResult wait_res;
                int err = shellWaitPidDetailed(pid, true, &wait_res);
                if (err != 0) {
                    continue;
                }
                saw_state = true;
                final_status = wait_res.status_mapped;
                if (wait_res.stopped) {
                    stopped_job = true;
                } else if (ctx->pids && i < ctx->stage_count) {
                    ctx->pids[i] = -1;
                }
            }

            if (job_control) {
                shellJobControlRestoreForeground();
            }

            if (stopped_job) {
                ShellJob *job = NULL;
                if (tail_cmd) {
                    job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
                }
                if (job) {
                    shellJobMarkStopped(job, final_status);
                }
#if defined(PSCAL_TARGET_IOS)
                shellPrintJobSuspended(job, tail_cmd);
#endif
                ctx->last_status = final_status;
                shellCloseVprocStreams();
                shellResetPipeline();
                shellUpdateStatus(final_status);
                return final_status;
            }

            if (!ctx->background && final_status >= 128 && final_status < 128 + NSIG) {
                shellHandleStatusSignal(final_status - 128);
            }
            if (!saw_state) {
                final_status = shellRuntimeLastStatus();
            }
            if (ctx->negated) {
                final_status = (final_status == 0) ? 1 : 0;
            }
            ctx->last_status = final_status;
            shellCloseVprocStreams();
            shellResetPipeline();
            shellUpdateStatus(final_status);
            return final_status;
        }

        if (ctx->background) {
            /* Register the synthetic in-process job so fg/bg/jobs work. */
            if (ctx->pids && ctx->launched > 0) {
                pid_t bg_pgid = (ctx->pgid > 0) ? ctx->pgid : ctx->pids[0];
                    ShellJob *job = shellRegisterJob(bg_pgid, ctx->pids, ctx->launched, tail_cmd);
                    if (job) {
                        shellJobMarkRunning(job);
                        job->last_status = 0;
                    }
                    gShellRuntime.last_bg_pid = ctx->pids[ctx->launched - 1];
#if defined(PSCAL_TARGET_IOS)
                    shellPrintJobStarted(job, bg_pgid);
#endif
                }
            ShellShebangThreadCtx *bg_ctx = NULL;
            if (tail_cmd && tail_cmd->argc > 0 && tail_cmd->argv) {
                bg_ctx = (ShellShebangThreadCtx *)calloc(1, sizeof(ShellShebangThreadCtx));
                if (bg_ctx) {
                    bg_ctx->argc = tail_cmd->argc;
                    bg_ctx->argv = (char **)calloc((size_t)bg_ctx->argc, sizeof(char *));
                    if (!bg_ctx->argv) {
                        shellShebangThreadCtxFree(bg_ctx);
                        bg_ctx = NULL;
                    }
                }
                if (bg_ctx) {
                    for (int i = 0; i < bg_ctx->argc; ++i) {
                        if (tail_cmd->argv[i]) {
                            bg_ctx->argv[i] = strdup(tail_cmd->argv[i]);
                            if (!bg_ctx->argv[i]) {
                                shellShebangThreadCtxFree(bg_ctx);
                                bg_ctx = NULL;
                                break;
                            }
                        }
                    }
                }
            }
            if (bg_ctx) {
                pthread_t tid;
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
                if (pthread_create(&tid, &attr, shellShebangBackgroundMain, bg_ctx) == 0) {
                    pthread_detach(tid);
                    final_status = 0;
                    ctx->last_status = final_status;
                    shellResetPipeline();
                    shellUpdateStatus(final_status);
                    shellRuntimeProcessPendingSignals();
                    return final_status;
                }
                pthread_attr_destroy(&attr);
                shellShebangThreadCtxFree(bg_ctx);
            }
        }
        if (ctx->stage_status) {
            int combined = 0;
            for (size_t i = 0; i < ctx->stage_count; ++i) {
                combined = shellPipelineNextStatus(combined, ctx->stage_status[i]);
                if (i < SHELL_PIPESTATUS_MAX) {
                    gShellPipeStatus[i] = ctx->stage_status[i];
                }
            }
            gShellPipeStatusCount = ctx->stage_count < SHELL_PIPESTATUS_MAX ? ctx->stage_count : SHELL_PIPESTATUS_MAX;
            gShellPipeStatusFresh = true;
            if (gShellRuntime.pipefail_enabled) {
                final_status = combined;
            }
        }
        bool job_control = false;
        if (!ctx->background) {
            shellEnsureJobControl();
            job_control = gShellRuntime.job_control_enabled && job_pgid > 0;
            if (job_control) {
                shellJobControlRestoreForeground();
            }
            if (final_status >= 128 && final_status < 128 + NSIG) {
                shellHandleStatusSignal(final_status - 128);
            }
        }

        if (!ctx->background && job_control &&
            shellStatusRepresentsStopSignal(final_status, NULL)) {
            ShellJob *job = NULL;
            if (tail_cmd) {
                job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
            }
            if (job) {
                shellJobMarkStopped(job, final_status);
            }
#if defined(PSCAL_TARGET_IOS)
            shellPrintJobSuspended(job, tail_cmd);
#endif
            ctx->last_status = final_status;
            shellCloseVprocStreams();
            shellResetPipeline();
            shellUpdateStatus(final_status);
            return final_status;
        }

        if (ctx->negated) {
            final_status = (final_status == 0) ? 1 : 0;
        }
        ctx->last_status = final_status;
        shellCloseVprocStreams();
        shellResetPipeline();
        shellUpdateStatus(final_status);
        return final_status;
    }
#endif

    if (!ctx->background) {
        shellEnsureJobControl();
        bool job_control = gShellRuntime.job_control_enabled && job_pgid > 0;
        bool stopped_job = false;

        if (job_control) {
            shellJobControlSetForeground(job_pgid);
        }

#if defined(PSCAL_TARGET_IOS)
#endif
        for (size_t i = 0; i < ctx->launched; ++i) {
            pid_t pid = ctx->pids[i];
            if (pid <= 0) {
                continue;
            }
            if (gShellExitRequested || gShellRuntime.break_requested) {
                if (ctx->pgid > 0) {
                    shellJobControlSendPgid(ctx->pgid, SIGTERM);
                } else {
                    shellJobControlSendPid(pid, SIGTERM);
                }
                continue;
            }
            ShellWaitResult wait_res;
            int err = shellWaitPidDetailed(pid, job_control, &wait_res);
            if (err != 0) {
                continue;
            }
            if (wait_res.stopped) {
                stopped_job = true;
                final_status = wait_res.status_mapped;
            } else {
                final_status = shellPipelineNextStatus(final_status, wait_res.status_mapped);
                ctx->pids[i] = -1;
                if (i < SHELL_PIPESTATUS_MAX) {
                    gShellPipeStatus[i] = wait_res.status_mapped;
                    gShellPipeStatusCount = i + 1;
                }
            }
        }
        gShellPipeStatusFresh = ctx->launched > 0;

        if (job_control) {
            shellJobControlRestoreForeground();
        }

        if (!ctx->background && final_status >= 128 && final_status < 128 + NSIG) {
            shellHandleStatusSignal(final_status - 128);
        }

            if (stopped_job) {
                ShellJob *job = NULL;
                if (tail_cmd) {
                    job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
                }
                if (job) {
                    shellJobMarkStopped(job, final_status);
                }
#if defined(PSCAL_TARGET_IOS)
            shellPrintJobSuspended(job, tail_cmd);
#endif
            ctx->last_status = final_status;
            shellResetPipeline();
            shellUpdateStatus(final_status);
            return final_status;
        }
    } else if (ctx->launched > 0) {
        gShellRuntime.last_bg_pid = ctx->pids[ctx->launched - 1];
        ShellJob *job = NULL;
        if (tail_cmd) {
            job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
        }
        if (job) {
            shellJobMarkRunning(job);
            job->last_status = 0;
        }
        final_status = 0;
    }

    if (ctx->negated) {
        final_status = (final_status == 0) ? 1 : 0;
    }

    ctx->last_status = final_status;
    shellResetPipeline();
    shellUpdateStatus(final_status);
    return final_status;
}

static bool shellCommandIsExecBuiltin(const ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0 || !cmd->argv || !cmd->argv[0]) {
        return false;
    }
    const char *name = cmd->argv[0];
    const char *canonical = shellBuiltinCanonicalName(name);
    if (!canonical) {
        canonical = name;
    }
    return strcasecmp(canonical, "exec") == 0;
}

static bool shellEnsureExecRedirBackup(int target_fd,
                                       int avoid_fd,
                                       ShellExecRedirBackup **backups,
                                       size_t *count,
                                       size_t *capacity) {
    if (target_fd < 0 || !backups || !count || !capacity) {
        return false;
    }
    for (size_t i = 0; i < *count; ++i) {
        if ((*backups)[i].target_fd == target_fd) {
            return true;
        }
    }
    ShellExecRedirBackup backup;
    backup.target_fd = target_fd;
    backup.saved_fd = -1;
    backup.saved_valid = false;
    backup.was_closed = false;
    backup.use_vproc = false;
    backup.vproc_saved_fd = -1;
    backup.slot_only = false;
    backup.slot_empty = false;
#if defined(PSCAL_TARGET_IOS)
    /* shellDupOntoTarget() rewrites the fd table of whatever vproc is current,
     * so snapshot that entry against the same vproc -- not `active_vp` below,
     * which is deliberately blanked for in-process pipeline stages. */
    VProc *vp_mutated = vprocCurrent();
    if (vp_mutated) {
        /* Clear of the source too: `>&$fd` with that closed must not find this copy there. */
        backup.vproc_saved_fd = vprocDupAbove(vp_mutated, target_fd, avoid_fd >= 10 ? avoid_fd + 1 : 10);
        backup.slot_only = target_fd > STDERR_FILENO;
        backup.slot_empty = backup.vproc_saved_fd < 0 && vprocTranslateFd(vp_mutated, target_fd) < 0;
    }
    if (backup.slot_only) {
        goto store_backup;
    }
    VProc *active_vp = vp_mutated;
    bool want_vproc_restore = gShellSkipHostStdio;
    if (gShellRuntime.pipeline.in_process && !gShellBackgroundWorkerActive && !want_vproc_restore) {
        /* In-process pipelines run with a synthetic vproc whose stdio does not
         * match the shell's host fds. Backup/restore must target the real host
         * descriptors so interactive input is restored after the stage ends. */
        active_vp = NULL;
    }
#else
    VProc *active_vp = NULL;
    (void)active_vp;
#endif
    /* A saved copy stays clear of the numbers a script can name, as bash's does at 10:
     * `2>/dev/null >&3` must not find a saved stderr in descriptor 3. */
    int min_fd = (target_fd + 1 > 10) ? target_fd + 1 : 10;
    if (avoid_fd >= 0 && avoid_fd >= min_fd) {
        min_fd = avoid_fd + 1;
    }
    int dup_fd = -1;
#if defined(PSCAL_TARGET_IOS)
    int source_fd = target_fd;
    if (want_vproc_restore) {
        VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
        int session_fd = -1;
        if (session_stdio && !vprocSessionStdioIsDefault(session_stdio)) {
            if (target_fd == STDIN_FILENO) {
                session_fd = session_stdio->stdin_host_fd;
            } else if (target_fd == STDOUT_FILENO) {
                session_fd = session_stdio->stdout_host_fd;
            } else if (target_fd == STDERR_FILENO) {
                session_fd = session_stdio->stderr_host_fd;
            }
        }
        if (session_fd >= 0) {
            source_fd = session_fd;
        } else if (vp_mutated) {
            /* Not the host's own number: that is the whole app's, and a substitution moves
             * only the slot. A pty session, the app's, has no host descriptors of its own,
             * so it comes here too. */
            int host_src = vprocTranslateFd(vp_mutated, target_fd);
            if (host_src >= 0) {
                source_fd = host_src;
            }
        }
        backup.use_vproc = true;
    } else if (active_vp) {
        int host_src = vprocTranslateFd(active_vp, target_fd);
        if (host_src >= 0) {
            source_fd = host_src;
            backup.use_vproc = true;
        }
    }
#else
    int source_fd = target_fd;
#endif
    if (source_fd < 0) {
        /* Session stdio can transiently report unknown host fds during tab
         * bootstrap; fall back to the actual target descriptor snapshot. */
        source_fd = target_fd;
    }
    if (source_fd < 0) {
        errno = EBADF;
        return false;
    }
#ifdef F_DUPFD_CLOEXEC
    dup_fd = fcntl(source_fd, F_DUPFD_CLOEXEC, min_fd);
    if (dup_fd < 0 && errno == EINVAL) {
        dup_fd = -1;
    }
#endif
    if (dup_fd < 0) {
        dup_fd = fcntl(source_fd, F_DUPFD, min_fd);
        if (dup_fd >= 0) {
            fcntl(dup_fd, F_SETFD, FD_CLOEXEC);
        }
    }
    if (dup_fd < 0) {
        dup_fd = vprocHostDup(source_fd);
        if (dup_fd >= 0) {
            fcntl(dup_fd, F_SETFD, FD_CLOEXEC);
        }
    }
    if (dup_fd >= 0 && avoid_fd >= 0 && dup_fd == avoid_fd) {
#ifdef F_DUPFD_CLOEXEC
        int alt_fd = fcntl(source_fd, F_DUPFD_CLOEXEC, avoid_fd + 1);
        if (alt_fd >= 0) {
            vprocHostClose(dup_fd);
            dup_fd = alt_fd;
        } else if (errno == EINVAL) {
            alt_fd = -1;
        }
#else
        int alt_fd = -1;
#endif
        if (dup_fd >= 0 && dup_fd == avoid_fd) {
            if (alt_fd < 0) {
                alt_fd = fcntl(source_fd, F_DUPFD, avoid_fd + 1);
                if (alt_fd >= 0) {
                    fcntl(alt_fd, F_SETFD, FD_CLOEXEC);
                }
            }
            if (alt_fd >= 0) {
                vprocHostClose(dup_fd);
                dup_fd = alt_fd;
            } else {
                vprocHostClose(dup_fd);
                dup_fd = -1;
            }
        }
    }
    if (dup_fd >= 0) {
        backup.saved_fd = dup_fd;
        backup.saved_valid = true;
    } else if (errno == EBADF) {
        backup.was_closed = true;
    } else {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
store_backup:
#endif
    if (*count >= *capacity) {
        size_t new_capacity = (*capacity == 0) ? 4 : (*capacity * 2);
        ShellExecRedirBackup *resized =
            (ShellExecRedirBackup *)realloc(*backups, new_capacity * sizeof(ShellExecRedirBackup));
        if (!resized) {
            if (backup.saved_valid && backup.saved_fd >= 0) {
                vprocHostClose(backup.saved_fd);
            }
            return false;
        }
        *backups = resized;
        *capacity = new_capacity;
    }
    (*backups)[*count] = backup;
    (*count)++;
    return true;
}

static void shellResetStandardStream(int fd) {
    /* If stdio fds were remapped (dup2/redirection), drop cached FILE* wrappers
     * so the next access reopens against the current descriptor mapping. */
    shellInvalidateVprocStreamForFd(fd);
    if (fd == STDIN_FILENO) {
        clearerr(stdin);
    } else if (fd == STDOUT_FILENO) {
        clearerr(stdout);
    } else if (fd == STDERR_FILENO) {
        clearerr(stderr);
    }
}

/* Duplicate src onto target, updating both the host fd and (when present) the
 * current vproc's fd table. */
static bool shellDupOntoTarget(VProc *vp, int src_fd, int target_fd, bool dbg, const char *label) {
    if (target_fd < 0) {
        errno = EBADF;
        return false;
    }
    int host_src = src_fd;
#if defined(PSCAL_TARGET_IOS)
    bool skip_host_dup = gShellBackgroundWorkerActive || gShellSkipHostStdio;
    /* In-process pipelines must still duplicate host stdio so downstream stages
     * see the correct descriptors; ignore skip_host_dup in that case. */
    if (gShellRuntime.pipeline.active &&
        gShellRuntime.pipeline.in_process &&
        !gShellBackgroundWorkerActive) {
        skip_host_dup = false;
    }
    /* Only stdio has a host side to keep in step. Above it the number means the slot, and
     * the host's descriptor of that number is someone else's, such as the Go runtime's
     * kqueue: dup2 onto it ends the whole app. */
    if (vp && target_fd > STDERR_FILENO) {
        skip_host_dup = true;
    }
    if (vp) {
        int translated = vprocTranslateFd(vp, src_fd);
        if (translated >= 0) {
            host_src = translated;
        } else if (src_fd > STDERR_FILENO) {
            /* The number names a slot, and that slot has no host side. The host's
             * descriptor of that number is not this one: `>&7` must not write into it. */
            host_src = -1;
        }
    }
#endif
    int hrc = 0;
#if defined(PSCAL_TARGET_IOS)
    if (vp) {
        int vrc = vprocDup2(vp, src_fd, target_fd);
        int dup_errno = errno;
        if (dbg) {
            fprintf(stderr, "[redir] vprocDup2 %d->%d rc=%d host_src=%d (%s)\n",
                    src_fd, target_fd, vrc, host_src, label ? label : "");
        }
        /* That slot is closed. Its number means nothing on the host: `>&2` after `2>&-` must
         * not reach the process's stderr. */
        if (vrc < 0 && dup_errno == EBADF) {
            errno = EBADF;
            return false;
        }
        if (vrc < 0 && skip_host_dup) {
            int restore_rc = -1;
            if (host_src >= 0) {
                restore_rc = vprocRestoreHostFd(vp, target_fd, host_src);
            }
            if (restore_rc >= 0) {
                if (dbg) {
                    fprintf(stderr, "[redir] vprocRestoreHostFd fallback %d->%d rc=%d (%s)\n",
                            host_src, target_fd, restore_rc, label ? label : "");
                }
                shellResetStandardStream(target_fd);
                return true;
            }
            if (dbg) {
                fprintf(stderr, "[redir] vprocDup2 %d->%d failed with skip_host_dup (%s): %s\n",
                        src_fd, target_fd, label ? label : "", strerror(errno));
            }
            return false;
        }
        /* Never touch the process-wide stdio when running in a background worker;
         * the interactive shell shares host fds across threads. */
        if (!skip_host_dup && host_src >= 0) {
            hrc = vprocHostDup2(host_src, target_fd);
            if (dbg) {
                fprintf(stderr, "[redir] vprocHostDup2 %d->%d rc=%d (%s)\n",
                        host_src, target_fd, hrc, label ? label : "");
            }
        }
        if (!skip_host_dup && host_src < 0 && src_fd >= 0) {
            hrc = dup2(src_fd, target_fd);
        }
        if (!skip_host_dup && hrc >= 0 && vrc < 0) {
            /* If host dup succeeded but vproc dup failed, re-sync vproc's fd
             * table from the live target host descriptor so shim writes follow
             * the same destination as process stdio. */
            int restore_rc = vprocRestoreHostFd(vp, target_fd, target_fd);
            if (dbg) {
                fprintf(stderr, "[redir] vprocRestoreHostFd %d rc=%d (%s)\n",
                        target_fd, restore_rc, label ? label : "");
            }
            if (restore_rc < 0) {
                return false;
            }
        }
    } else
#endif
    {
        hrc = dup2((host_src >= 0) ? host_src : src_fd, target_fd);
    }
    if (hrc < 0) {
        if (dbg) {
            fprintf(stderr, "[redir] dup %d->%d failed: %s (%s)\n",
                    (host_src >= 0) ? host_src : src_fd,
                    target_fd, strerror(errno), label ? label : "");
        }
        return false;
    }
    shellResetStandardStream(target_fd);
    return true;
}

static void shellRestoreExecRedirections(ShellExecRedirBackup *backups, size_t count) {
    if (!backups) {
        return;
    }
#if defined(PSCAL_TARGET_IOS)
    VProc *active_vp = vprocCurrent();
    bool skip_host_restore = gShellBackgroundWorkerActive || gShellSkipHostStdio;
    if (gShellRuntime.pipeline.active &&
        gShellRuntime.pipeline.in_process &&
        !gShellBackgroundWorkerActive) {
        skip_host_restore = false;
    }
#else
    VProc *active_vp = NULL;
    bool skip_host_restore = false;
#endif
    for (size_t i = 0; i < count; ++i) {
        ShellExecRedirBackup *backup = &backups[i];
        bool touched = false;
        if (backup->saved_valid && backup->saved_fd >= 0) {
            if (!skip_host_restore) {
                (void)vprocHostDup2(backup->saved_fd, backup->target_fd);
                touched = true;
            }
            if (backup->use_vproc && active_vp && !backup->slot_empty) {
                (void)vprocRestoreHostFd(active_vp, backup->target_fd, backup->saved_fd);
                touched = true;
            }
        } else if (backup->was_closed) {
            if (!skip_host_restore) {
                vprocHostClose(backup->target_fd);
                touched = true;
            }
        }
#if defined(PSCAL_TARGET_IOS)
        /* Put the vproc's own fd entry back. This is the exact inverse of the
         * vprocDup2() in shellDupOntoTarget(), and it is the only restore that
         * works when the entry is a PTY-backed pscal_fd -- an app session's
         * stdio -- because there the host descriptor carries no equivalent and
         * the entry would otherwise keep pointing at the redirection target,
         * silently swallowing every later write. */
        if (backup->vproc_saved_fd >= 0) {
            if (active_vp) {
                if (vprocDup2(active_vp, backup->vproc_saved_fd, backup->target_fd) >= 0) {
                    touched = true;
                }
                (void)vprocClose(active_vp, backup->vproc_saved_fd);
            }
            backup->vproc_saved_fd = -1;
        } else if ((backup->slot_only || backup->slot_empty) && active_vp) {
            (void)vprocClose(active_vp, backup->target_fd);
            touched = true;
        }
#endif
        if (touched) {
            shellResetStandardStream(backup->target_fd);
        }
    }
}

static void shellFreeExecRedirBackups(ShellExecRedirBackup *backups, size_t count) {
    if (!backups) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (backups[i].saved_valid && backups[i].saved_fd >= 0) {
            vprocHostClose(backups[i].saved_fd);
            backups[i].saved_fd = -1;
        }
#if defined(PSCAL_TARGET_IOS)
        /* Drop the vproc snapshot without restoring it. Freeing without a
         * restore is the `exec >file` path, where the redirection is meant to
         * outlive the command, so the slot must be released, not reinstated. */
        if (backups[i].vproc_saved_fd >= 0) {
            VProc *vp = vprocCurrent();
            if (vp) {
                (void)vprocClose(vp, backups[i].vproc_saved_fd);
            }
            backups[i].vproc_saved_fd = -1;
        }
#endif
    }
    free(backups);
}

/* A redirection that fails fails its command and the script goes on, as in bash.
 * runtimeError would stop the VM. The status is 1 whatever the errno was. */
static void shellRedirectionFailed(const char *target, int err) {
    fprintf(stderr, "exsh: %s: %s\n", target, strerror(err ? err : EINVAL));
    shellUpdateStatus(1);
}

static void shellRedirectionFailedForFd(int fd, int err) {
    char target[32];
    snprintf(target, sizeof(target), "%d", fd);
    shellRedirectionFailed(target, err);
}

/* The lowest free descriptor from 10 up, which is where bash starts. */
static int shellFdVarPick(VProc *vp) {
#if defined(PSCAL_TARGET_IOS)
    if (vp) {
        return vprocFreeFdAbove(vp, 10);
    }
#else
    (void)vp;
#endif
    int fd = 10;
    while (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
        fd++;
    }
    return fd;
}

/* On a host only what stays in the shell comes this way: a program, a job and a stage have forked
 * first. On iOS they come too. Under bash each of those is a process, which ends and takes the
 * descriptor and the name with it, and the vproc made for it takes the descriptor. */
static bool shellFdVarOutlivesCommand(const ShellCommand *cmd) {
#if defined(PSCAL_TARGET_IOS)
    /* A compound command's redirections: anything it is a stage of has a state of its own. */
    if (cmd->argc == 0 || cmd->body) {
        return true;
    }
    /* The command knows it is a stage: on another thread this one's pipeline is not active. */
    const ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (cmd->background || (ctx->active && ctx->stage_count > 1) ||
        (cmd->pipeline_index >= 0 && !(cmd->is_pipeline_head && cmd->is_pipeline_tail))) {
        return false;
    }
    if (shellFindCompiledFunction(cmd->argv[0])) {
        return true;
    }
    char lookup_buf[PATH_MAX];
    const char *name = shellCommandLookupName(cmd->argv[0], lookup_buf, sizeof(lookup_buf));
    if (!shellIsRuntimeBuiltin(name) || shellCommandIsFrontendToolName(name) || shellIOSFrontendHandler(name)) {
        return false;
    }
    if (!smallclueFindApplet(name)) {
        return true;
    }
    /* Builtins under bash too, though an applet has the name. */
    static const char *const kBashBuiltins[] = {"echo", "false", "kill", "printf", "pwd", "test", "true", "["};
    for (size_t i = 0; i < sizeof(kBashBuiltins) / sizeof(kBashBuiltins[0]); ++i) {
        if (strcmp(name, kBashBuiltins[i]) == 0) {
            return true;
        }
    }
    return false;
#else
    (void)cmd;
    return true;
#endif
}

/* The descriptor is there. It stays for the script to use, and name says where. */
static bool shellFdVarFinish(const ShellCommand *cmd, const ShellRedirection *redir, int fd) {
    if (redir->kind == SHELL_RUNTIME_REDIR_DUP && redir->close_target) {
#if defined(PSCAL_TARGET_IOS)
        if (shellCommandIsExecBuiltin(cmd)) {
            vprocShellFdFollow(fd);
        }
#endif
        return true;
    }
    if (!shellFdVarOutlivesCommand(cmd)) {
        return true;
    }
    if (!shellFdVarAssign(redir->fd_var, fd)) {
        close(fd);
        return false;
    }
    shellCommandSubKeepFdVar(fd);
#if defined(PSCAL_TARGET_IOS)
    vprocShellFdFollow(fd);
#endif
    return true;
}

/* `{fd}> f` with no command makes the file and leaves nothing behind, as `> f` does. Each is then
 * a redirection of a free number, which goes back to being free. `{fd}>&-` closes nothing either. */
static void shellFdVarsEndWithCommand(ShellCommand *cmd) {
#if defined(PSCAL_TARGET_IOS)
    VProc *vp = vprocCurrent();
#else
    VProc *vp = NULL;
#endif
    for (size_t i = 0; i < cmd->redir_count; ++i) {
        ShellRedirection *redir = &cmd->redirs[i];
        if (!redir->fd_var) {
            continue;
        }
        int fd = (redir->kind == SHELL_RUNTIME_REDIR_DUP && redir->close_target) ? shellFdVarValue(redir->fd_var)
                                                                                  : shellFdVarPick(vp);
        /* Not a number: applied as it is, and that reports it. */
        if (fd >= 0) {
            redir->fd = fd;
            free(redir->fd_var);
            redir->fd_var = NULL;
        }
    }
}

static bool shellApplyExecRedirections(const ShellCommand *cmd,
                                       ShellExecRedirBackup **out_backups,
                                       size_t *out_count) {
    bool dbg = (getenv("PSCALI_REDIR_DEBUG") != NULL) || (getenv("PSCALI_PIPE_DEBUG") != NULL);
#if defined(PSCAL_TARGET_IOS)
    VProc *vp_current = vprocCurrent();
#else
    VProc *vp_current = NULL;
#endif
    if (out_backups) {
        *out_backups = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!cmd || cmd->redir_count == 0) {
        return true;
    }

    if (dbg) {
        fprintf(stderr, "[redir] begin count=%zu\n", cmd ? cmd->redir_count : 0);
    }
    ShellExecRedirBackup *backups = NULL;
    size_t backup_count = 0;
    size_t backup_capacity = 0;
    bool stdout_redirected = false;

    for (size_t i = 0; i < cmd->redir_count; ++i) {
        const ShellRedirection *redir = &cmd->redirs[i];
        int target_fd = redir->fd;
        int avoid_fd = -1;
        if (redir->kind == SHELL_RUNTIME_REDIR_DUP && !redir->close_target) {
            avoid_fd = redir->dup_target_fd;
        }
        bool closes_fd_var = redir->fd_var && redir->kind == SHELL_RUNTIME_REDIR_DUP && redir->close_target;
        if (closes_fd_var) {
            target_fd = shellFdVarValue(redir->fd_var);
            if (target_fd < 0) {
                fprintf(stderr, "exsh: %s: ambiguous redirect\n", redir->fd_var);
                shellUpdateStatus(1);
                goto redir_error;
            }
        } else if (redir->fd_var) {
            /* Nothing to put back: it is the script's to close. */
            target_fd = shellFdVarPick(vp_current);
        }
        /* Closing goes back after the command unless that is exec, as any `3>&-` does. */
        if ((!redir->fd_var || closes_fd_var) &&
            !shellEnsureExecRedirBackup(target_fd, avoid_fd, &backups, &backup_count, &backup_capacity)) {
            int err = errno;
            if (err == 0) {
                err = ENOMEM;
            }
            shellRedirectionFailedForFd(target_fd, err);
            goto redir_error;
        }
        switch (redir->kind) {
            case SHELL_RUNTIME_REDIR_OPEN: {
                if (dbg) {
                    fprintf(stderr, "[redir] open fd=%d path=%s flags=0x%x mode=0%o\n",
                            target_fd, redir->path ? redir->path : "<null>",
                            redir->flags, redir->mode);
                }
                if (!redir->path) {
                    shellRedirectionFailed("redirection", EINVAL);
                    goto redir_error;
                }
#if defined(PSCAL_TARGET_IOS)
                char virtual_path[PATH_MAX];
                const char *open_path = redir->path;
                if (pathTruncateExpand(redir->path, virtual_path, sizeof(virtual_path))) {
                    open_path = virtual_path;
                }
#else
                const char *open_path = redir->path;
#endif
                int fd = -1;
#if defined(PSCAL_TARGET_IOS)
                fd = vprocHostOpen(open_path, redir->flags, redir->mode);
#else
                fd = open(open_path, redir->flags, redir->mode);
#endif
                if (fd < 0) {
                    int err = errno;
                    if (dbg) {
                        fprintf(stderr, "[redir] open failed: %s\n", strerror(err));
                    }
                    shellRedirectionFailed(redir->path, err);
                    goto redir_error;
                }
                int vfd = -1;
#if defined(PSCAL_TARGET_IOS)
                if (vp_current) {
                    vfd = vprocAdoptHostFd(vp_current, fd);
                    if (vfd < 0) {
                        int err = errno;
                        vprocHostClose(fd);
                        shellRedirectionFailed(redir->path, err);
                        goto redir_error;
                    }
                }
#endif
                /* With the target closed, as in `exec 3> f`, open can land right on it.
                 * Then it is done, and closing the source would undo it. */
                if (((vfd >= 0) ? vfd : fd) != target_fd) {
                    if (!shellDupOntoTarget(vp_current, (vfd >= 0) ? vfd : fd, target_fd, dbg, "open")) {
                        int err = errno;
                        if (dbg) {
                            fprintf(stderr, "[redir] dup %d->%d failed: %s\n", fd, target_fd, strerror(err));
                        }
                        shellRedirectionFailed(redir->path, err);
#if defined(PSCAL_TARGET_IOS)
                        if (vp_current && vfd >= 0) {
                            vprocCloseShim(vfd);
                        } else
#endif
                        vprocHostClose(fd);
                        goto redir_error;
                    }
#if defined(PSCAL_TARGET_IOS)
                    if (vp_current && vfd >= 0) {
                        vprocCloseShim(vfd);
                    } else
#endif
                    vprocHostClose(fd);
                }
#if defined(PSCAL_TARGET_IOS)
                else if (vp_current) {
                    vprocMarkFdInherited(vp_current, target_fd);
                }
#endif
                if (target_fd == STDOUT_FILENO) {
                    stdout_redirected = true;
                }
                break;
            }
            case SHELL_RUNTIME_REDIR_DUP: {
                if (dbg) {
                    fprintf(stderr, "[redir] dup fd=%d target=%d close=%d\n",
                            redir->dup_target_fd, target_fd, (int)redir->close_target);
                }
                if (redir->ambiguous) {
                    fprintf(stderr, "exsh: %s: ambiguous redirect\n", redir->path ? redir->path : "");
                    shellUpdateStatus(1);
                    goto redir_error;
                }
                if (target_fd == STDOUT_FILENO && redir->dup_target_fd == STDOUT_FILENO && stdout_redirected) {
                    target_fd = STDERR_FILENO;
                }
                if (redir->close_target) {
                    if (close(target_fd) != 0 && errno != EBADF) {
                        int err = errno;
                        shellRedirectionFailedForFd(target_fd, err);
                        goto redir_error;
                    }
                    shellResetStandardStream(target_fd);
                } else {
                    if (redir->dup_target_fd < 0) {
                        shellRedirectionFailedForFd(redir->dup_target_fd, EBADF);
                        goto redir_error;
                    }
                    if (!shellDupOntoTarget(vp_current, redir->dup_target_fd, target_fd, dbg, "dup")) {
                        int err = errno;
                        if (dbg) {
                            fprintf(stderr, "[redir] dup2 %d->%d failed: %s\n",
                                    redir->dup_target_fd, target_fd, strerror(err));
                        }
                        shellRedirectionFailedForFd(redir->dup_target_fd, err);
                        goto redir_error;
                    }
                }
                break;
            }
            case SHELL_RUNTIME_REDIR_HEREDOC: {
                const char *body = redir->here_doc ? redir->here_doc : "";
                int body_fd = shellOpenHereBody(body, redir->here_doc_length ? redir->here_doc_length : strlen(body), false);
                if (body_fd < 0) {
                    shellRedirectionFailed("here-document", errno);
                    goto redir_error;
                }
                if (body_fd != target_fd && dup2(body_fd, target_fd) < 0) {
                    int err = errno;
                    shellRedirectionFailed("here-document", err);
                    close(body_fd);
                    goto redir_error;
                }
#if defined(PSCAL_TARGET_IOS)
                if (vp_current) {
                    vprocMarkFdInherited(vp_current, target_fd);
                }
#endif
                shellResetStandardStream(target_fd);
                if (body_fd != target_fd) {
                    close(body_fd);
                }
                break;
            }
            case SHELL_RUNTIME_REDIR_HERE_STRING: {
                const char *body = redir->here_string ? redir->here_string
                                                       : (redir->here_string_literal ? redir->here_string_literal : "");
                int body_fd = shellOpenHereBody(body, redir->here_string_length ? redir->here_string_length : strlen(body),
                                                true);
                if (body_fd < 0) {
                    shellRedirectionFailed("here-string", errno);
                    goto redir_error;
                }
                if (body_fd != target_fd && dup2(body_fd, target_fd) < 0) {
                    int err = errno;
                    shellRedirectionFailed("here-string", err);
                    close(body_fd);
                    goto redir_error;
                }
#if defined(PSCAL_TARGET_IOS)
                if (vp_current) {
                    vprocMarkFdInherited(vp_current, target_fd);
                }
#endif
                shellResetStandardStream(target_fd);
                if (body_fd != target_fd) {
                    close(body_fd);
                }
                break;
            }
            default:
                shellRedirectionFailed("redirection", EINVAL);
                goto redir_error;
        }
        if (redir->fd_var && !shellFdVarFinish(cmd, redir, target_fd)) {
            shellUpdateStatus(1);
            goto redir_error;
        }
    }

    if (out_backups) {
        *out_backups = backups;
    } else {
        shellFreeExecRedirBackups(backups, backup_count);
    }
    if (out_count) {
        *out_count = backup_count;
    }
    return true;

redir_error:
    shellRestoreExecRedirections(backups, backup_count);
    shellFreeExecRedirBackups(backups, backup_count);
    if (out_backups) {
        *out_backups = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    return false;
}

#if defined(PSCAL_TARGET_IOS)
static bool __attribute__((unused)) shellPipelineWriteBufferToPipe(const char *data, size_t len, int fd) {
    size_t written = 0;
    while (written < len) {
        ssize_t res = vprocHostWrite(fd, data + written, len - written);
        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += (size_t)res;
    }
    return true;
}

static bool __attribute__((unused)) shellPipelineReadPipeToBuffer(int fd, char **out_data, size_t *out_size) {
    const size_t kChunk = 4096;
    size_t capacity = 0;
    size_t size = 0;
    char *buffer = NULL;
    for (;;) {
        if (size + kChunk > capacity) {
            size_t new_capacity = capacity ? capacity * 2 : kChunk;
            char *new_buf = realloc(buffer, new_capacity);
            if (!new_buf) {
                free(buffer);
                return false;
            }
            buffer = new_buf;
            capacity = new_capacity;
        }
        ssize_t res = vprocHostRead(fd, buffer + size, capacity - size);
        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buffer);
            return false;
        }
        if (res == 0) {
            break;
        }
        size += (size_t)res;
    }

    if (!buffer) {
        buffer = (char *)calloc(1, 1);
        if (!buffer) {
            return false;
        }
    }
    *out_data = buffer;
    *out_size = size;
    return true;
}

static int __attribute__((unused)) shellPipelineCreateSpoolFile(void) {
    const char *tmp_root = getenv("TMPDIR");
    if (!tmp_root || !*tmp_root) {
        tmp_root = "/tmp";
    }
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "%s/pscal-pipe-%ld-XXXXXX",
             tmp_root, (long)getpid());
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        unlink(tmpl);
    }
    return fd;
}

#if defined(PSCAL_TARGET_IOS)
static bool shellDispatchSmallclueAppletInProcess(VM *vm, ShellCommand *cmd) {
    (void)vm;
    if (!cmd || !cmd->argv || !cmd->argv[0] || cmd->argc <= 0) {
        return false;
    }

    char lookup_buf[PATH_MAX];
    const char *cmd_name = shellCommandLookupName(cmd->argv[0], lookup_buf, sizeof(lookup_buf));
    const SmallclueApplet *applet = smallclueFindApplet(cmd_name);
    if (!applet) {
        return false;
    }

    int argc = cmd->argc;
    char **argv_copy = (char **)calloc((size_t)(argc + 1), sizeof(char *));
    if (!argv_copy) {
        shellUpdateStatus(1);
        return true;
    }

    bool ok = true;
    for (int i = 0; i < argc; ++i) {
        const char *src_arg = cmd->argv[i];
        if (i == 0 && cmd_name && *cmd_name) {
            src_arg = cmd_name;
        }
        if (src_arg) {
            argv_copy[i] = strdup(src_arg);
            if (!argv_copy[i]) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        for (int i = 0; i < argc; ++i) {
            free(argv_copy[i]);
        }
        free(argv_copy);
        shellUpdateStatus(1);
        return true;
    }

    int sc_status = 0;
    char label[96] = {0};
    shellVprocFormatLabel(cmd, label, sizeof(label));
    VProcCommandScope vproc_scope;
    memset(&vproc_scope, 0, sizeof(vproc_scope));
    int saved_fg_pgid = -1;
    int session_sid = -1;
    bool inherit_pgid = false;
    bool run_scoped = vprocIsShellSelfThread();
    if (run_scoped) {
        /* Worker-stage applets already have an owning vproc. */
        VProc *active_vp = vprocCurrent();
        int active_pid = active_vp ? vprocPid(active_vp) : -1;
        int shell_pid = vprocGetShellSelfPid();
        if (active_pid > 0 && shell_pid > 0 && active_pid != shell_pid) {
            run_scoped = false;
        }
    }
    bool scoped = false;
    if (run_scoped) {
        scoped = vprocCommandScopeBegin(&vproc_scope,
                                        label[0] ? label : (cmd->argv && cmd->argv[0] ? cmd->argv[0] : "task"),
                                        true,
                                        inherit_pgid);
    }
    if (scoped && vproc_scope.pid > 0) {
        vprocSetStopUnsupported(vproc_scope.pid, true);
        int shell_pid = vprocGetShellSelfPid();
        int scope_pgid = vprocGetPgid(vproc_scope.pid);
        if (scope_pgid <= 0) {
            scope_pgid = vproc_scope.pid;
        }
        if (shell_pid > 0) {
            session_sid = vprocGetSid(shell_pid);
        }
        if (session_sid > 0 && scope_pgid > 0) {
            saved_fg_pgid = vprocGetForegroundPgid(session_sid);
            if (saved_fg_pgid <= 0) {
                saved_fg_pgid = vprocGetPgid(shell_pid);
            }
            (void)vprocSetForegroundPgid(session_sid, scope_pgid);
        }
    }

    ShellProgramEnvScope program_env;
    shellProgramEnvEnter(&program_env, cmd);
    sc_status = smallclueDispatchApplet(applet, argc, argv_copy);
    shellProgramEnvLeave(&program_env);
    if (scoped) {
        vprocCommandScopeEnd(&vproc_scope, sc_status);
    }
    if (session_sid > 0 && saved_fg_pgid > 0) {
        (void)vprocSetForegroundPgid(session_sid, saved_fg_pgid);
    }
    shellUpdateStatus(sc_status);

    for (int i = 0; i < argc; ++i) {
        free(argv_copy[i]);
    }
    free(argv_copy);
    return true;
}
#endif

static int shellRunBuiltinInProcess(VM *vm,
                                    ShellCommand *cmd,
                                    int stdin_fd,
                                    int stdout_fd,
                                    int stderr_fd,
                                    bool apply_exec_redirs) {
    bool tool_dbg = (getenv("PSCALI_TOOL_DEBUG") != NULL) || (cmd && cmd->background);
    bool pipe_dbg = getenv("PSCALI_PIPE_DEBUG") != NULL;
    bool redir_dbg = getenv("PSCALI_REDIR_DEBUG") != NULL;
#if defined(PSCAL_TARGET_IOS)
    bool pipeline_in_process = gShellRuntime.pipeline.active &&
                               gShellRuntime.pipeline.in_process &&
                               !gShellBackgroundWorkerActive;
    VProc *active_vp = vprocCurrent();
    bool prev_skip_host = gShellSkipHostStdio;
    /* Background workers share the interactive stdio; avoid touching host fds. */
    if (gShellBackgroundWorkerActive) {
        gShellSkipHostStdio = true;
    }
    if (pipe_dbg) {
        fprintf(stderr, "[run-builtin] active_vp=%p stdin_fd=%d stdout_fd=%d stderr_fd=%d\n",
                (void *)active_vp, stdin_fd, stdout_fd, stderr_fd);
    }
#else
    bool pipeline_in_process = false;
    VProc *active_vp = NULL;
#endif
    if (redir_dbg) {
        fprintf(stderr, "[redir] cmd=%s argc=%zu redir_count=%zu apply_exec=%d\n",
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                cmd ? cmd->argc : 0,
                cmd ? cmd->redir_count : 0,
                (int)apply_exec_redirs);
        if (cmd && cmd->argv) {
            for (size_t ai = 0; ai < (size_t)cmd->argc; ++ai) {
                fprintf(stderr, "[redir]   argv[%zu]=%s\n", ai, cmd->argv[ai] ? cmd->argv[ai] : "<null>");
            }
        }
        if (cmd && cmd->redirs) {
            for (size_t ri = 0; ri < cmd->redir_count; ++ri) {
                ShellRedirection *r = &cmd->redirs[ri];
                fprintf(stderr, "[redir]   redir[%zu] fd=%d kind=%d path=%s dup_target=%d flags=0x%x\n",
                        ri, r->fd, (int)r->kind, r->path ? r->path : "<none>", r->dup_target_fd, r->flags);
            }
        }
    }
    ShellExecRedirBackup *backups = NULL;
    size_t backup_count = 0;
    size_t backup_capacity = 0;
    int status = 127;
    bool applied_redirs = false;
    bool allow_missing_backup = pipeline_in_process;
    if (stdin_fd >= 0) {
        if (!shellEnsureExecRedirBackup(STDIN_FILENO, -1, &backups, &backup_count, &backup_capacity)) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] backup stdin failed\n");
            if (!allow_missing_backup) goto cleanup;
        }
        if (!shellDupOntoTarget(active_vp, stdin_fd, STDIN_FILENO, pipe_dbg, "stdin")) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] dup stdin failed\n");
            goto cleanup;
        }
    }
    if (stdout_fd >= 0) {
        if (!shellEnsureExecRedirBackup(STDOUT_FILENO, -1, &backups, &backup_count, &backup_capacity)) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] backup stdout failed\n");
            if (!allow_missing_backup) goto cleanup;
        }
        if (!shellDupOntoTarget(active_vp, stdout_fd, STDOUT_FILENO, pipe_dbg, "stdout")) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] dup stdout failed\n");
            goto cleanup;
        }
    }
    if (stderr_fd >= 0) {
        if (!shellEnsureExecRedirBackup(STDERR_FILENO, -1, &backups, &backup_count, &backup_capacity)) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] backup stderr failed\n");
            if (!allow_missing_backup) goto cleanup;
        }
        if (!shellDupOntoTarget(active_vp, stderr_fd, STDERR_FILENO, pipe_dbg, "stderr")) {
            if (tool_dbg && pipe_dbg) fprintf(stderr, "[run-builtin] dup stderr failed\n");
            goto cleanup;
        }
    }

    if (apply_exec_redirs) {
        if (getenv("PSCALI_PIPE_DEBUG")) {
            fprintf(stderr, "[pipeline-bg] apply redirs count=%zu\n", cmd ? cmd->redir_count : 0);
        }
        if (!shellApplyExecRedirections(cmd, &backups, &backup_count)) {
            status = 1;
            goto cleanup;
        }
        applied_redirs = true;
    }

    if (tool_dbg && pipe_dbg) {
        fprintf(stderr, "[run-builtin] invoking cmd=%s argc=%zu redirs=%zu apply_exec_redirs=%d\n",
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                cmd ? cmd->argc : 0,
                cmd ? cmd->redir_count : 0,
                (int)apply_exec_redirs);
    }
    if (pipe_dbg) {
        fprintf(stderr, "[run-builtin] begin lookup for %s\n",
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>");
    }
    /* What was applied above stays applied. The builtin would open each file again, and
     * under set -C the second open finds the file the first one made. */
    size_t applied_redir_count = cmd ? cmd->redir_count : 0;
    if (applied_redirs) {
        cmd->redir_count = 0;
    }
#if defined(PSCAL_TARGET_IOS)
    if (cmd && cmd->program_env) {
        gShellStageProgramEnv = cmd->program_env;
        cmd->program_env = NULL;
    }
#endif
    bool builtin_ran = shellInvokeBuiltin(vm, cmd);
    if (cmd) {
        cmd->redir_count = applied_redir_count;
    }
#if defined(PSCAL_TARGET_IOS)
    if (!builtin_ran) {
        builtin_ran = shellDispatchIOSFrontendBuiltin(vm, cmd);
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (!builtin_ran) {
        /* Try tool-runner execution for shebang-wrapped scripts even in
         * pipeline mode. This mirrors the exec path but stays in-process. */
        bool was_background = gShellRuntime.pipeline.background;
        bool isolate_shebang_runtime =
            gShellRuntime.pipeline.active &&
            gShellRuntime.pipeline.in_process &&
            !gShellBackgroundWorkerActive;
        ShellRuntimeState *shebang_ctx = NULL;
        ShellRuntimeState *prev_ctx = NULL;
        if (was_background &&
            !gShellBackgroundWorkerActive &&
            gShellRuntime.pipeline.stage_count == 1 &&
            getenv("PSCALI_SHEBANG_BG_LEGACY")) {
            /* Legacy detached shebang background path.
             * Disabled by default because it bypasses stage-owned redirection. */
            fprintf(stderr, "[shebang-bg] cloning argv for '%s' (argc=%zu)\n",
                    cmd->argv[0] ? cmd->argv[0] : "<null>",
                    cmd->argc);
            ShellShebangThreadCtx *bg_ctx =
                (ShellShebangThreadCtx *)calloc(1, sizeof(ShellShebangThreadCtx));
            if (bg_ctx) {
                bg_ctx->argc = cmd->argc;
                bg_ctx->argv = (char **)calloc((size_t)bg_ctx->argc, sizeof(char *));
                if (bg_ctx->argv) {
                    bool ok = true;
                    for (int i = 0; i < bg_ctx->argc; ++i) {
                        if (cmd->argv[i]) {
                            bg_ctx->argv[i] = strdup(cmd->argv[i]);
                            if (!bg_ctx->argv[i]) {
                                ok = false;
                                break;
                            }
                        }
                    }
                    if (!ok) {
                        shellShebangThreadCtxFree(bg_ctx);
                        bg_ctx = NULL;
                    }
                } else {
                    shellShebangThreadCtxFree(bg_ctx);
                    bg_ctx = NULL;
                }
            }
            if (bg_ctx) {
                pthread_t tid;
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
                if (pthread_create(&tid, &attr, shellShebangBackgroundMain, bg_ctx) == 0) {
                    pthread_detach(tid);
                    pthread_attr_destroy(&attr);
                    fprintf(stderr, "[shebang-bg] detached thread launched\n");
                    shellUpdateStatus(0);
                    shellResetPipeline();
                    return 0;
                }
                pthread_attr_destroy(&attr);
                fprintf(stderr, "[shebang-bg] failed to spawn thread, falling back inline\n");
                shellShebangThreadCtxFree(bg_ctx);
            }
        }
        const char *shebang_path = cmd->argv[0];
        char *resolved = shellStageFindExecutable(cmd);
        if (resolved) {
            shebang_path = resolved;
        }
        if (isolate_shebang_runtime) {
            shebang_ctx = shellRuntimeCreateContext();
            if (shebang_ctx) {
                prev_ctx = shellRuntimeActivateContext(shebang_ctx);
            }
        }
        /* Only for a script: a stage's environment is for whatever program it turns out to be. */
        ShellProgramEnvScope shebang_env;
        ShellProgramScope shebang_scope;
        bool has_shebang = shellPathHasShebang(shebang_path);
        if (has_shebang) {
            shellProgramEnvEnter(&shebang_env, cmd);
            shellProgramScopeEnter(&shebang_scope);
        }
        int shebang_status = shellMaybeExecShebangTool(shebang_path, cmd->argv);
        if (has_shebang) {
            shellProgramScopeLeave(&shebang_scope);
            shellProgramEnvLeave(&shebang_env);
        }
        if (shebang_ctx) {
            shellRuntimeActivateContext(prev_ctx);
            shellRuntimeDestroyContext(shebang_ctx);
        }
        if (resolved) {
            free(resolved);
        }
        if (shebang_status >= 0) {
            shellUpdateStatus(shebang_status);
            builtin_ran = true;
        }
    }
#endif
    if (!builtin_ran) {
        bool kill_dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
        bool pipe_dbg = getenv("PSCALI_PIPE_DEBUG") != NULL;
        const char *cmd_name_raw = (cmd && cmd->argv) ? cmd->argv[0] : NULL;
        char lookup_buf[PATH_MAX];
        const char *cmd_name = shellCommandLookupName(cmd_name_raw, lookup_buf, sizeof(lookup_buf));
        VmBuiltinFn handler = getVmBuiltinHandler(cmd_name);
#if defined(PSCAL_TARGET_IOS)
        if (handler && cmd_name && *cmd_name) {
            /*
             * Prefer smallclue applets for overlapping command names on iOS.
             * Applets provide user-facing diagnostics and path-virtualized
             * behavior expected by the mobile shell UX.
             */
            if (smallclueFindApplet(cmd_name) != NULL) {
                handler = NULL;
            }
        }
#endif
        if (kill_dbg) {
            fprintf(stderr, "[exec] lookup cmd=%s handler=%p\n",
                    (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                    (void *)handler);
        }
        if (pipe_dbg) {
            fprintf(stderr, "[exec] run cmd=%s lookup=%s argc=%zu handler=%p\n",
                    (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                    cmd_name ? cmd_name : "<nil>",
                    cmd ? cmd->argc : 0,
                    (void *)handler);
        }
        if (handler) {
            int arg_count = (cmd->argc > 0) ? (int)cmd->argc - 1 : 0;
            Value *args = NULL;
            if (arg_count > 0) {
                args = calloc((size_t)arg_count, sizeof(Value));
                for (int i = 0; args && i < arg_count; ++i) {
                    args[i] = makeString(cmd->argv[i + 1]);
                }
            }
            handler(vm ? vm : gShellCurrentVm, arg_count, args);
            if (args) {
                for (int i = 0; i < arg_count; ++i) {
                    freeValue(&args[i]);
                }
                free(args);
            }
            builtin_ran = true;
        } else {
#if defined(PSCAL_TARGET_IOS)
            /* Fall back to smallclue applets executed in-process when no shell builtin exists. */
            const SmallclueApplet *applet = smallclueFindApplet(cmd_name);
            if (pipe_dbg) {
                fprintf(stderr, "[run-builtin] smallclue lookup name=%s lookup=%s applet=%p\n",
                        (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                        cmd_name ? cmd_name : "<nil>",
                        (void *)applet);
            }
            if (applet) {
                int argc = cmd->argc > 0 ? cmd->argc : 0;
                char **argv_copy = (char **)calloc((size_t)(argc + 1), sizeof(char *));
                if (argv_copy) {
                    bool ok = true;
                    for (int i = 0; i < argc; ++i) {
                        const char *src_arg = cmd->argv[i];
                        if (i == 0 && cmd_name && *cmd_name) {
                            src_arg = cmd_name;
                        }
                        if (src_arg) {
                            argv_copy[i] = strdup(src_arg);
                            if (!argv_copy[i]) {
                                ok = false;
                                break;
                            }
                        }
                    }
                    if (ok) {
                        int sc_status = 0;
#if defined(PSCAL_TARGET_IOS)
                        char label[96] = {0};
                        shellVprocFormatLabel(cmd, label, sizeof(label));
                        VProcCommandScope vproc_scope;
                        memset(&vproc_scope, 0, sizeof(vproc_scope));
                        int saved_fg_pgid = -1;
                        int session_sid = -1;
                        bool inherit_pgid = false;
                        bool run_scoped = vprocIsShellSelfThread();
                        if (run_scoped) {
                            /* Worker-stage applets already have an owning vproc.
                             * Never wrap them in an extra shell-scoped vproc,
                             * otherwise Ctrl-Z is forced into cooperative mode
                             * and the stage becomes non-resumable (tid=0). */
                            VProc *active_vp = vprocCurrent();
                            int active_pid = active_vp ? vprocPid(active_vp) : -1;
                            int shell_pid = vprocGetShellSelfPid();
                            if (active_pid > 0 && shell_pid > 0 && active_pid != shell_pid) {
                                run_scoped = false;
                            }
                        }
                        bool scoped = false;
                        if (run_scoped) {
                            scoped = vprocCommandScopeBegin(&vproc_scope,
                                                            label[0] ? label : (cmd->argv && cmd->argv[0] ? cmd->argv[0] : "task"),
                                                            true,
                                                            inherit_pgid);
                        }
                        if (scoped && vproc_scope.pid > 0) {
                            /* Scoped applets run on the shell thread. Keep Ctrl-Z
                             * cooperative so we unwind back to the prompt instead of
                             * parking the shell worker. */
                            vprocSetStopUnsupported(vproc_scope.pid, true);
                            int shell_pid = vprocGetShellSelfPid();
                            int scope_pgid = vprocGetPgid(vproc_scope.pid);
                            if (scope_pgid <= 0) {
                                scope_pgid = vproc_scope.pid;
                            }
                            if (shell_pid > 0) {
                                session_sid = vprocGetSid(shell_pid);
                            }
                            if (session_sid > 0 && scope_pgid > 0) {
                                saved_fg_pgid = vprocGetForegroundPgid(session_sid);
                                if (saved_fg_pgid <= 0) {
                                    saved_fg_pgid = vprocGetPgid(shell_pid);
                                }
                                (void)vprocSetForegroundPgid(session_sid, scope_pgid);
                            }
                        }
#endif
#if defined(PSCAL_TARGET_IOS)
                        ShellProgramEnvScope program_env;
                        shellProgramEnvEnter(&program_env, cmd);
#endif
                        sc_status = smallclueDispatchApplet(applet, argc, argv_copy);
#if defined(PSCAL_TARGET_IOS)
                        shellProgramEnvLeave(&program_env);
                        const char *dbg = getenv("SMALLCLUE_DEBUG");
                        if (dbg && *dbg) {
                            fprintf(stderr, "[smallclue] dispatched applet pid=%d pgid=%d\n",
                                    vproc_scope.pid, vprocGetPgid(vproc_scope.pid));
                        }
                        if (scoped) {
                            vprocCommandScopeEnd(&vproc_scope, sc_status);
                        }
                        if (session_sid > 0 && saved_fg_pgid > 0) {
                            (void)vprocSetForegroundPgid(session_sid, saved_fg_pgid);
                        }
#endif
                        shellUpdateStatus(sc_status);
                        builtin_ran = true;
                    }
                    for (int i = 0; i < argc; ++i) {
                        free(argv_copy[i]);
                    }
                    free(argv_copy);
                }
            }
#endif
        }
        if (!builtin_ran) {
#if defined(PSCAL_TARGET_IOS)
            /* Try to interpret scripts in-process (no fork/exec on iOS). */
            char *resolved = NULL;
            const char *orig0 = cmd->argv ? cmd->argv[0] : NULL;
            const char *temp0 = orig0;
            if (orig0 && strchr(orig0, '/') == NULL) {
                resolved = shellStageFindExecutable(cmd);
                if (resolved) {
                    temp0 = resolved;
                }
            }
            int fb_status = -1;
            if (temp0) {
                char *saved0 = cmd->argv[0];
                cmd->argv[0] = (char *)temp0;
                ShellProgramEnvScope program_env;
                shellProgramEnvEnter(&program_env, cmd);
                fb_status = shellTryRunScriptFallback(vm ? vm : gShellCurrentVm, cmd);
                shellProgramEnvLeave(&program_env);
                cmd->argv[0] = saved0;
            }
            if (resolved) free(resolved);
            if (fb_status >= 0) {
                shellUpdateStatus(fb_status);
                builtin_ran = true;
            }
#endif
        }
        if (!builtin_ran) {
            fprintf(stderr, "exsh: %s: command not found\n", cmd->argv[0]);
            shellUpdateStatus(127);
            if (tool_dbg && pipe_dbg) {
                fprintf(stderr, "[run-builtin] builtin not found for %s\n", cmd && cmd->argv ? cmd->argv[0] : "<nil>");
            }
        }
    }
    shellFlushStandardStreams();
    status = gShellRuntime.last_status;

cleanup:
#if defined(PSCAL_TARGET_IOS)
    vprocEnvRelease(gShellStageProgramEnv);
    gShellStageProgramEnv = NULL;
#endif
    if (tool_dbg && pipe_dbg && status == 127) {
        fprintf(stderr, "[run-builtin] exiting with status=127 cmd=%s apply_exec_redirs=%d stdin_fd=%d stdout_fd=%d stderr_fd=%d\n",
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                (int)apply_exec_redirs,
                stdin_fd, stdout_fd, stderr_fd);
    }
#if defined(PSCAL_TARGET_IOS)
    if (tool_dbg && pipe_dbg) {
        fprintf(stderr, "[run-builtin] final fd map stdout_fd=%d stderr_fd=%d\n", stdout_fd, stderr_fd);
    }
#endif
    if (applied_redirs) {
        shellRestoreExecRedirections(backups, backup_count);
    } else if (backups) {
        shellRestoreExecRedirections(backups, backup_count);
    }
#if defined(PSCAL_TARGET_IOS)
    if (pipe_dbg && backups && backup_count > 0) {
        for (size_t bi = 0; bi < backup_count; ++bi) {
            fprintf(stderr, "[redir-restore] target=%d saved_fd=%d use_vproc=%d was_closed=%d\n",
                    backups[bi].target_fd,
                    backups[bi].saved_fd,
                    (int)backups[bi].use_vproc,
                    (int)backups[bi].was_closed);
        }
    }
#endif
    shellFreeExecRedirBackups(backups, backup_count);
#if defined(PSCAL_TARGET_IOS)
    /* Per-thread stream caches can hold buffered pipeline output; always close
     * them at stage end so data is flushed before the thread exits. */
    shellCloseVprocStreams();
    gShellSkipHostStdio = prev_skip_host;
#endif
    return status;
}

/* A foreground stage's parent is whichever vproc will wait for it. wait only
 * matches its own children, and inside a nested exsh or a script that is not the
 * session shell: the wait failed at once and the next command ran alongside this
 * stage. */
static int shellStageParentPid(int shell_pid) {
    VProc *waiter = vprocCurrent();
    int waiter_pid = waiter ? vprocPid(waiter) : -1;
    return waiter_pid > 0 ? waiter_pid : shell_pid;
}

static int shellPipelineRunStage(VM *vm,
                                 ShellPipelineContext *ctx,
                                 ShellCommand *cmd,
                                 size_t stage_pos,
                                 bool pipeline_head,
                                 bool pipeline_tail) {
    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    struct pscal_fd *stdin_pscal = NULL;
    struct pscal_fd *stdout_pscal = NULL;
    struct pscal_fd *stderr_pscal = NULL;
    int status = 0;
    bool apply_exec_redirs = cmd && cmd->redir_count > 0;
    bool pipe_dbg = getenv("PSCALI_PIPE_DEBUG") != NULL;
    bool adopt_failed = false;

    bool pipeline_in_process = ctx && ctx->in_process;
#if defined(PSCAL_TARGET_IOS)
    /* The last stage runs right here, on the shell's thread. */
    if (ctx && ctx->stage_count > 1 && cmd && !cmd->body_state && shellStageNeedsOwnState(cmd)) {
        cmd->body_state = shellBodyStateCapture();
    }
#endif
    if (pipe_dbg) {
        fprintf(stderr,
                "[pipe-stage] begin stage=%zu cmd=%s head=%d tail=%d in_proc=%d\n",
                stage_pos,
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                (int)pipeline_head,
                (int)pipeline_tail,
                (int)pipeline_in_process);
    }
//    fprintf(stderr,
//            "[pipe] stage=%zu head=%d tail=%d in_process=%d stdin_fd=%d stdout_fd=%d stderr_fd=%d\n",
 //           stage_pos, (int)pipeline_head, (int)pipeline_tail,
  //          (int)pipeline_in_process, stdin_fd, stdout_fd, stderr_fd);

    if (!pipeline_head) {
#if defined(PSCAL_TARGET_IOS)
        if (pipeline_in_process && ctx->pscal_pipe_read) {
            stdin_pscal = ctx->pscal_pipe_read[stage_pos - 1];
            stdin_fd = -2;
        } else
#endif
        {
            /* Duplicate the pipe endpoint for this stage and drop the shared
             * reference so EOF can propagate even while other threads run. */
            int fd = ctx->pipes[stage_pos - 1][0];
            if (fd >= 0) {
                int dup_fd = vprocHostDup(fd);
                if (dup_fd >= 0) {
                    stdin_fd = dup_fd;
                    vprocHostClose(fd);
                } else {
                    /* Fallback: transfer ownership of the original endpoint to
                     * this stage to avoid later double-close from shared state. */
                    stdin_fd = fd;
                }
                ctx->pipes[stage_pos - 1][0] = -1;
            }
        }
    }
    if (!pipeline_tail) {
#if defined(PSCAL_TARGET_IOS)
        if (pipeline_in_process && ctx->pscal_pipe_write) {
            stdout_pscal = ctx->pscal_pipe_write[stage_pos];
            stdout_fd = -2;
        } else
#endif
        {
            int fd = ctx->pipes[stage_pos][1];
            if (fd >= 0) {
                int dup_fd = vprocHostDup(fd);
                if (dup_fd >= 0) {
                    stdout_fd = dup_fd;
                    vprocHostClose(fd);
                } else {
                    /* Fallback: transfer ownership of the original endpoint to
                     * this stage to avoid later double-close from shared state. */
                    stdout_fd = fd;
                }
                ctx->pipes[stage_pos][1] = -1;
            }
        }
    }
    if (ctx->merge_stderr && stage_pos < ctx->stage_count && ctx->merge_stderr[stage_pos]) {
#if defined(PSCAL_TARGET_IOS)
        if (stdout_pscal) {
            stderr_pscal = stdout_pscal;
            stderr_fd = -2;
        //    fprintf(stderr, "[pipe] stage=%zu stderr merged to stdout pscal\n",
         //           stage_pos);
        } else
#endif
        {
            stderr_fd = (stdout_fd >= 0) ? stdout_fd : STDOUT_FILENO;
        }
    }

#if defined(PSCAL_TARGET_IOS)
    /* iOS: in-process pipelines are stable when stage vprocs inherit the
     * session PSCAL stdio context. */
    int pid_hint = -1;
    if (ctx && ctx->pids && stage_pos < ctx->stage_count && ctx->pids[stage_pos] > 0) {
        pid_hint = (int)ctx->pids[stage_pos];
    }
    bool inherit_pscal_stdio = true;
    VProc *vp = shellCreateVProcForStage(stdin_fd,
                                         stdout_fd,
                                         stderr_fd,
                                         ctx->window_cols > 0 ? ctx->window_cols : pscalRuntimeDetectWindowCols(),
                                         ctx->window_rows > 0 ? ctx->window_rows : pscalRuntimeDetectWindowRows(),
                                         pid_hint,
                                         inherit_pscal_stdio);
    if (!vp) {
        runtimeError(vm, "pipeline: failed to allocate vproc");
        shellUpdateStatus(1);
        return 1;
    }
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0) {
        vprocSetParent(vprocPid(vp), shellStageParentPid(shell_pid));
        int shell_sid = vprocGetSid(shell_pid);
        if (shell_sid > 0) {
            vprocSetSid(vprocPid(vp), shell_sid);
        }
    }
    pid_t group = (ctx->pgid > 0) ? ctx->pgid
                : ((ctx->launched > 0 && ctx->pids) ? ctx->pids[0] : -1);
    if (group <= 0) {
        group = vprocPid(vp);
    }
    vprocSetPgid(vprocPid(vp), group);
    ctx->pgid = group;
    if (cmd && cmd->argv && cmd->argc > 0) {
        char label[96] = {0};
        shellVprocFormatLabel(cmd, label, sizeof(label));
        if (!label[0] && cmd->argv[0]) {
            strncpy(label, cmd->argv[0], sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
        }
        if (label[0]) {
            vprocSetCommandLabel(vprocPid(vp), label);
        }
    }
#if defined(PSCAL_TARGET_IOS)
    /* Keep foreground stage groups aligned with job control state. */
    if (!ctx->background && !cmd->background && shellRuntimeIsInteractive()) {
        shellEnsureJobControl();
        if (gShellRuntime.job_control_enabled && ctx->pgid > 0) {
            shellJobControlSetForeground(ctx->pgid);
        }
    }
#endif
    /* A command alone on this thread is the shell at work: `trap ... CHLD` is not for each of its
     * builtins, and its own commands would run the trap again. A subshell and a job are children. */
    if (ctx->stage_count == 1 && cmd && !cmd->body) {
        vprocSetSilentToParent(vprocPid(vp), true);
        /* In a pipeline these run apart, as a subshell does; here `$$` is the shell, as bash has it. */
        const char *stage_name = cmd->argc > 0 ? cmd->argv[0] : NULL;
        if (stage_name && (strcmp(stage_name, ".") == 0 || strcmp(stage_name, "source") == 0 ||
                           strcmp(stage_name, "eval") == 0)) {
            vprocSetNotAProcess(vprocPid(vp));
        }
    }
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    if (stdin_pscal) {
        int arc = vprocAdoptPscalFd(vp, STDIN_FILENO, stdin_pscal);
        if (arc != 0) {
            adopt_failed = true;
            if (pipe_dbg) {
                fprintf(stderr,
                        "[pipe-stage] adopt stdin failed stage=%zu pscal=%p errno=%d (%s)\n",
                        stage_pos, (void *)stdin_pscal, errno, strerror(errno));
            }
        }
    }
    if (stdout_pscal) {
        int arc = vprocAdoptPscalFd(vp, STDOUT_FILENO, stdout_pscal);
        if (arc != 0) {
            adopt_failed = true;
            if (pipe_dbg) {
                fprintf(stderr,
                        "[pipe-stage] adopt stdout failed stage=%zu pscal=%p errno=%d (%s)\n",
                        stage_pos, (void *)stdout_pscal, errno, strerror(errno));
            }
        }
    }
    if (!pipeline_tail && ctx && ctx->pscal_pipe_write_hold &&
        stage_pos < (ctx->stage_count > 0 ? ctx->stage_count - 1 : 0)) {
        if (ctx->pscal_pipe_write_hold[stage_pos]) {
            pscal_fd_close(ctx->pscal_pipe_write_hold[stage_pos]);
            ctx->pscal_pipe_write_hold[stage_pos] = NULL;
        }
    }
    if (stderr_pscal && stderr_pscal != stdout_pscal) {
        int arc = vprocAdoptPscalFd(vp, STDERR_FILENO, stderr_pscal);
        if (arc != 0) {
            adopt_failed = true;
            if (pipe_dbg) {
                fprintf(stderr,
                        "[pipe-stage] adopt stderr failed stage=%zu pscal=%p errno=%d (%s)\n",
                        stage_pos, (void *)stderr_pscal, errno, strerror(errno));
            }
        }
    }

    if (pipeline_in_process) {
        /* PSCAL pipe path: drop host dup immediately. */
        int close_in = (stdin_pscal && stdin_fd >= 0) ? stdin_fd : -1;
        int close_out = (stdout_pscal && stdout_fd >= 0) ? stdout_fd : -1;
        int close_err = (stderr_pscal && stderr_fd >= 0) ? stderr_fd : -1;
        if (close_in >= 0) {
            vprocHostClose(close_in);
            stdin_fd = -1;
            if (close_out == close_in) {
                close_out = -1;
                stdout_fd = -1;
            }
            if (close_err == close_in) {
                close_err = -1;
                stderr_fd = -1;
            }
        }
        if (close_out >= 0) {
            vprocHostClose(close_out);
            stdout_fd = -1;
            if (close_err == close_out) {
                close_err = -1;
                stderr_fd = -1;
            }
        }
        if (close_err >= 0) {
            vprocHostClose(close_err);
            stderr_fd = -1;
        }
    }
#endif

    if (adopt_failed) {
        runtimeError(vm, "pipeline: failed to attach in-process stdio");
        status = 1;
    } else {
#if defined(PSCAL_TARGET_IOS)
        pthread_cleanup_push(shellInlineStageCancelled, vp);
#endif
        status = shellRunBuiltinInProcess(vm, cmd, stdin_fd, stdout_fd, stderr_fd, apply_exec_redirs);
#if defined(PSCAL_TARGET_IOS)
        pthread_cleanup_pop(0);
#endif
    }
#if defined(PSCAL_TARGET_IOS)
    /* The interrupt key ended its input, so it ended as if that were all it had. */
    if (vprocTakeTerminalInterrupted() && status < 128) {
        status = 128 + SIGINT;
    }
#endif

    bool stderr_shared_with_stdout =
        (stderr_pscal && stdout_pscal && stderr_pscal == stdout_pscal);

    if (!pipeline_tail && stdout_fd >= 0) {
        vprocHostClose(stdout_fd);
    }
    if (!pipeline_head && stdin_fd >= 0) {
        vprocHostClose(stdin_fd);
    }
#if defined(PSCAL_TARGET_IOS)
    if (!pipeline_tail && stdout_pscal) {
        pscal_fd_close(stdout_pscal);
        if (ctx->pscal_pipe_write) {
            ctx->pscal_pipe_write[stage_pos] = NULL;
        }
        stdout_pscal = NULL;
    }
    if (!pipeline_head && stdin_pscal) {
        pscal_fd_close(stdin_pscal);
        if (ctx->pscal_pipe_read) {
            ctx->pscal_pipe_read[stage_pos - 1] = NULL;
        }
        stdin_pscal = NULL;
    }
    if (stderr_pscal && !stderr_shared_with_stdout) {
        pscal_fd_close(stderr_pscal);
        stderr_pscal = NULL;
    }
    if (stderr_shared_with_stdout) {
        stderr_pscal = NULL;
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (ctx->pids && stage_pos < ctx->stage_count) {
        ctx->pids[stage_pos] = vprocPid(vp);
        ctx->launched = stage_pos + 1;
    }
    vprocDeactivate();
    /* `eval` or a function alone is the shell going on with its script, no process of its own:
     * the jobs it started are the shell's, and `wait` there has to find them. */
    if (ctx->stage_count == 1 && shellStageRunsShellCode(cmd)) {
        vprocPassChildrenToParent(vprocPid(vp));
    }
    shellFinalizeStageVproc(vp, status);
#else
    if (ctx->pids && stage_pos < ctx->stage_count) {
        ctx->pids[stage_pos] = 0;
        ctx->launched = stage_pos + 1;
    }
#endif
    if (pipe_dbg) {
        fprintf(stderr,
                "[pipe-stage] end stage=%zu cmd=%s status=%d\n",
                stage_pos,
                (cmd && cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<nil>",
                status);
    }
    return status;
}

static ShellPipelineContext *shellCloneSingleStagePipelineContext(const ShellPipelineContext *src) {
    ShellPipelineContext *clone = (ShellPipelineContext *)calloc(1, sizeof(ShellPipelineContext));
    if (!clone) {
        return NULL;
    }
    clone->active = true;
    clone->stage_count = 1;
    clone->negated = src ? src->negated : false;
    clone->background = src ? src->background : true;
    clone->last_status = src ? src->last_status : 0;
    clone->pgid = src ? src->pgid : -1;
#if defined(PSCAL_TARGET_IOS)
    clone->in_process = src ? src->in_process : true;
    clone->in_process_worker = false;
    clone->session_stdio = src ? src->session_stdio : NULL;
    clone->window_cols = src ? src->window_cols : 0;
    clone->window_rows = src ? src->window_rows : 0;
#endif
    clone->pids = (pid_t *)calloc(1, sizeof(pid_t));
    if (!clone->pids) {
        free(clone);
        return NULL;
    }
    if (src && src->pids) {
        clone->pids[0] = src->pids[0];
    }
    return clone;
}

static void shellFreeClonedPipelineContext(ShellPipelineContext *ctx) {
    if (!ctx) {
        return;
    }
    free(ctx->pids);
    free(ctx);
}

static void *shellPipelineStageThreadMain(void *arg) {
    ShellPipelineStageThreadCtx *tctx = (ShellPipelineStageThreadCtx *)arg;
    if (!tctx) {
        return NULL;
    }
#if defined(PSCAL_TARGET_IOS)
    VM *stage_vm = tctx->vm;
    ShellRuntimeState *bg_shell_ctx = NULL;
    ShellRuntimeState *prev_shell_ctx = NULL;
    VM *bg_vm = NULL;
    VM *prev_vm = NULL;
    bool worker_ctx_ready = true;
    static pthread_once_t bg_builtin_once = PTHREAD_ONCE_INIT;
    pthread_once(&bg_builtin_once, registerExtendedBuiltins);
    shellThreadLinkEnter(&tctx->link);
    bg_shell_ctx = tctx->link.ctx ? tctx->link.ctx : shellRuntimeCreateContext();
    if (bg_shell_ctx) {
        /* As in a subshell: what its shell's jobs left is not a stage's to find. */
        bg_shell_ctx->finished_job_count = 0;
        prev_shell_ctx = shellRuntimeActivateContext(bg_shell_ctx);
        bg_vm = (VM *)calloc(1, sizeof(VM));
        if (bg_vm) {
            initVM(bg_vm);
            bg_vm->frontendContext = bg_shell_ctx;
            prev_vm = shellSwapCurrentVm(bg_vm);
            stage_vm = bg_vm;
        } else {
            shellRuntimeActivateContext(prev_shell_ctx);
            prev_shell_ctx = NULL;
            if (tctx->link.ctx == bg_shell_ctx) {
                shellRuntimeDestroyContext(bg_shell_ctx);
                tctx->link.ctx = NULL;
            } else {
                free(bg_shell_ctx);
            }
            bg_shell_ctx = NULL;
            worker_ctx_ready = false;
        }
    } else {
        worker_ctx_ready = false;
    }

    PSCALRuntimeContext *prev_runtime_ctx = NULL;
    bool swapped_runtime = false;
    if (PSCALRuntimeGetCurrentRuntimeContext) {
        prev_runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext();
    }
    if (PSCALRuntimeSetCurrentRuntimeContext && tctx->runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(tctx->runtime_ctx);
        swapped_runtime = true;
    }
    VProcSessionStdio *prev_session_stdio = vprocSessionStdioCurrent();
    bool swapped_stdio = false;
    if (tctx->session_stdio && tctx->session_stdio != prev_session_stdio) {
        vprocSessionStdioActivate(tctx->session_stdio);
        swapped_stdio = true;
    }
#endif
    bool prev_worker_flag = gShellBackgroundWorkerActive;
    bool mark_background_worker = (tctx->ctx && tctx->ctx->background);
    if (mark_background_worker) {
        gShellBackgroundWorkerActive = true;
    }
    int status = 1;
#if defined(PSCAL_TARGET_IOS)
    if (!worker_ctx_ready) {
        if (getenv("PSCALI_PIPE_DEBUG")) {
            fprintf(stderr,
                    "[pipe-thread] stage=%zu cmd=%s: unable to allocate isolated worker context\n",
                    tctx->stage_pos,
                    (tctx->cmd && tctx->cmd->argv && tctx->cmd->argv[0]) ? tctx->cmd->argv[0] : "<nil>");
        }
    } else
#endif
    {
        vprocSetPipelineStage(true);
        status = shellPipelineRunStage(stage_vm,
                                       tctx->ctx,
                                       tctx->cmd,
                                       tctx->stage_pos,
                                       tctx->pipeline_head,
                                       tctx->pipeline_tail);
        vprocSetPipelineStage(false);
    }
    /* Each stage has its own slot, read only once it has been joined. */
    if (!tctx->owns_ctx_snapshot && tctx->ctx && tctx->ctx->stage_status &&
        tctx->stage_pos < tctx->ctx->stage_count) {
        tctx->ctx->stage_status[tctx->stage_pos] = status;
    }
    if (getenv("PSCALI_PIPE_DEBUG")) {
        fprintf(stderr,
                "[pipe-thread] stage=%zu cmd=%s status=%d tail=%d bg=%d worker=%d\n",
                tctx->stage_pos,
                (tctx->cmd && tctx->cmd->argv && tctx->cmd->argv[0]) ? tctx->cmd->argv[0] : "<nil>",
                status,
                (int)tctx->pipeline_tail,
                (int)(tctx->ctx ? tctx->ctx->background : false),
                (int)gShellBackgroundWorkerActive);
    }
#if defined(PSCAL_TARGET_IOS)
    if (bg_vm || prev_vm || bg_shell_ctx || prev_shell_ctx) {
        if (bg_vm || prev_vm) {
            shellRestoreCurrentVm(prev_vm);
        }
        if (prev_shell_ctx) {
            shellRuntimeActivateContext(prev_shell_ctx);
        }
        if (bg_vm) {
            freeVM(bg_vm);
            free(bg_vm);
        }
        if (bg_shell_ctx) {
            if (tctx->link.ctx == bg_shell_ctx) {
                shellRuntimeDestroyContext(bg_shell_ctx);
                tctx->link.ctx = NULL;
            } else {
                free(bg_shell_ctx);
            }
        }
    }
    shellThreadLinkRelease(&tctx->link);
    if (swapped_stdio) {
        vprocSessionStdioActivate(prev_session_stdio);
    }
    if (swapped_runtime && PSCALRuntimeSetCurrentRuntimeContext) {
        PSCALRuntimeSetCurrentRuntimeContext(prev_runtime_ctx);
    }
#endif
    gShellBackgroundWorkerActive = prev_worker_flag;
    /*
     * Do not mutate shared pipeline bookkeeping from worker threads.
     * Background in-process stages can outlive the foreground command loop and
     * race with later pipeline reuse/reset, which risks touching stale pointers.
     * Foreground pipeline completion/teardown is still handled on the caller
     * path where stage joins are explicit.
     */
    if (tctx->cmd) {
        shellFreeCommand(tctx->cmd);
        free(tctx->cmd);
    }
    if (tctx->owns_ctx_snapshot) {
        shellFreeClonedPipelineContext(tctx->ctx);
        tctx->ctx = NULL;
    }
    free(tctx);
    return NULL;
}

#if defined(PSCAL_TARGET_IOS)
/* bash's test for keeping stdin in a job, where `<&0` means what the shell has. An input
 * redirection counts whatever its number, a duplication only on stdin itself. */
static bool shellCommandRedirectsStdin(const ShellCommand *cmd) {
    for (size_t i = 0; i < cmd->redir_count; ++i) {
        const ShellRedirection *r = &cmd->redirs[i];
        if (r->kind == SHELL_RUNTIME_REDIR_DUP ? r->fd == STDIN_FILENO
            : r->kind != SHELL_RUNTIME_REDIR_OPEN || (r->flags & O_ACCMODE) != O_WRONLY) {
            return true;
        }
    }
    return false;
}

/* A job's vproc is made by whatever reaches the `&`, and its thread starts under it. So stdio,
 * the descriptors above it and the directory are copied there and then, as by a fork, and the
 * thread never touches the vproc that started it: that one may end before the thread first
 * runs, as `( cmd & )` and `eval "cmd &"` do. */
static VProc *shellBackgroundStageCreateVProc(ShellBackgroundStageCtx *ctx, const ShellCommand *cmd) {
    /* A simple command's stdin is /dev/null unless it redirects stdin. A compound command keeps
     * its own, unless that is the terminal and it is in a substitution: with no job control
     * there, it would stop holding the pipe. */
    bool keeps_stdin = shellCommandRedirectsStdin(cmd) ||
                       (cmd->body && !(gShellCommandSubFrame && isatty(STDIN_FILENO)));
    VProc *vp = shellCreateVProcForStage(keeps_stdin ? -1 : -2,
                                         -1,
                                         -1,
                                         pscalRuntimeDetectWindowCols(),
                                         pscalRuntimeDetectWindowRows(),
                                         ctx->synthetic_pid > 0 ? ctx->synthetic_pid : -1,
                                         true);
    if (!vp) {
        return NULL;
    }
    int pid = vprocPid(vp);
    int shell_pid = vprocGetShellSelfPid();
    if (ctx->parent_pid > 0) {
        vprocSetParent(pid, ctx->parent_pid);
    } else if (shell_pid > 0) {
        vprocSetParent(pid, shell_pid);
    }
    int shell_sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : -1;
    if (shell_sid > 0) {
        vprocSetSid(pid, shell_sid);
    }
    pid_t group = (ctx->synthetic_pid > 0) ? ctx->synthetic_pid : pid;
    vprocSetPgid(pid, group);
    ctx->synthetic_pid = group;
    /* Label the vproc for lps/ps visibility. */
    if (cmd->argc > 0 && cmd->argv) {
        char label[96] = {0};
        shellVprocFormatLabel(cmd, label, sizeof(label));
        if (label[0]) {
            vprocSetCommandLabel(pid, label);
        }
    }
    return vp;
}
#endif

static bool shellPipelineExecuteStageInProcess(VM *vm,
                                               ShellPipelineContext *ctx,
                                               ShellCommand *cmd,
                                               size_t stage_pos,
                                               bool pipeline_head,
                                               bool pipeline_tail) {
//    fprintf(stderr,
 //           "[pipe] launch stage=%zu head=%d tail=%d in_process=%d read[%zu]=%p write[%zu]=%p\n",
  //          stage_pos,
   //         (int)pipeline_head,
    //        (int)pipeline_tail,
     //       (ctx ? (int)ctx->in_process : 0),
      //      (stage_pos > 0) ? stage_pos - 1 : 0,
       //     (ctx && stage_pos > 0 && ctx->pscal_pipe_read) ? (void *)ctx->pscal_pipe_read[stage_pos - 1] : NULL,
        //    stage_pos,
         //   (ctx && ctx->pscal_pipe_write) ? (void *)ctx->pscal_pipe_write[stage_pos] : NULL);
    if (!ctx || !ctx->active || stage_pos >= ctx->stage_count) {
        runtimeError(vm, "pipeline: invalid context");
        shellUpdateStatus(1);
        if (ctx) {
            shellAbortPipeline();
        }
        return false;
    }
    if (ctx->stage_count > 1 && !ctx->pipes) {
        runtimeError(vm, "pipeline: pipes unavailable");
        shellUpdateStatus(1);
        shellAbortPipeline();
        return false;
    }

    if (cmd && cmd->background) {
        ctx->background = true;
    }
    /* Single-stage background commands must always dispatch asynchronously.
     * Keep this keyed off the pipeline context to avoid parser/clone drift. */
    bool async_tail = pipeline_tail &&
                      ctx->stage_count == 1 &&
                      ctx->background;
    bool foreground_worker_tail = (!ctx->background &&
                                   pipeline_tail &&
                                   ctx->stage_count == 1 &&
                                   shellShouldRunCommandInForegroundWorker(cmd));
    pid_t async_bg_pgid = -1;
    pid_t async_stage_pid = -1;

    if (foreground_worker_tail) {
        ShellCommand *cloned = shellCloneCommandForBackground(cmd);
        if (!cloned) {
            runtimeError(vm, "pipeline: unable to launch foreground worker stage");
            shellUpdateStatus(1);
            shellAbortPipeline();
            return false;
        }

        int pid_hint = -1;
        if (ctx->pids && stage_pos < ctx->stage_count && ctx->pids[stage_pos] > 0) {
            pid_hint = (int)ctx->pids[stage_pos];
        }
        if (pid_hint <= 0) {
            pid_hint = vprocReservePid();
        }

        VProc *vp = shellCreateVProcForStage(-1,
                                             -1,
                                             -1,
                                             pscalRuntimeDetectWindowCols(),
                                             pscalRuntimeDetectWindowRows(),
                                             pid_hint,
                                             true);
        if (!vp) {
            shellFreeCommand(cloned);
            free(cloned);
            runtimeError(vm, "pipeline: failed to allocate foreground stage vproc");
            shellUpdateStatus(1);
            shellAbortPipeline();
            return false;
        }

        pid_t stage_pid = vprocPid(vp);
        if (ctx->pids && stage_pos < ctx->stage_count && stage_pid > 0) {
            ctx->pids[stage_pos] = stage_pid;
            if (ctx->launched < (stage_pos + 1)) {
                ctx->launched = stage_pos + 1;
            }
        }
        if (ctx->pgid <= 0 && stage_pid > 0) {
            ctx->pgid = stage_pid;
        }

        int shell_pid = vprocGetShellSelfPid();
        if (shell_pid > 0 && stage_pid > 0) {
            (void)vprocSetParent((int)stage_pid, shellStageParentPid(shell_pid));
            int shell_sid = vprocGetSid(shell_pid);
            if (shell_sid > 0) {
                (void)vprocSetSid((int)stage_pid, shell_sid);
            }
        }
        pid_t job_pgid = (ctx->pgid > 0) ? ctx->pgid : stage_pid;
        if (job_pgid > 0 && stage_pid > 0) {
            (void)vprocSetPgid((int)stage_pid, (int)job_pgid);
            ctx->pgid = job_pgid;
        }
        if (cmd && cmd->argv && cmd->argc > 0 && stage_pid > 0) {
            char label[96] = {0};
            shellVprocFormatLabel(cmd, label, sizeof(label));
            if (!label[0] && cmd->argv[0]) {
                strncpy(label, cmd->argv[0], sizeof(label) - 1);
                label[sizeof(label) - 1] = '\0';
            }
            if (label[0]) {
                vprocSetCommandLabel((int)stage_pid, label);
            }
        }

        ShellInProcessWorkerCtx *worker = (ShellInProcessWorkerCtx *)calloc(1, sizeof(ShellInProcessWorkerCtx));
        if (!worker) {
            shellFinalizeStageVproc(vp, 1);
            shellFreeCommand(cloned);
            free(cloned);
            runtimeError(vm, "pipeline: unable to allocate worker context");
            shellUpdateStatus(1);
            shellAbortPipeline();
            return false;
        }
        shellStageCaptureState(cloned, cmd, ctx->stage_count);
        worker->vm = vm;
        worker->cmd = cloned;
        worker->vp = vp;
        worker->session_stdio = ctx->session_stdio;
        worker->runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext
            ? PSCALRuntimeGetCurrentRuntimeContext()
            : NULL;
        worker->stdin_fd = -1;
        worker->stdout_fd = -1;
        worker->stderr_fd = -1;
        worker->stdin_is_vproc = false;
        worker->stdout_is_vproc = false;
        worker->stderr_is_vproc = false;
        worker->apply_exec_redirs = (cmd && cmd->redir_count > 0);
        worker->shell_pid = shell_pid;
        worker->kernel_pid = vprocGetKernelPid();
        worker->link = shellThreadLinkCapture();
        /* Asked here: a shell run as a command owns its directory on this thread only. */
        if (vprocShellGetcwdShim(worker->shell_cwd, sizeof(worker->shell_cwd)) == NULL) {
            worker->shell_cwd[0] = '\0';
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t stage_thread = 0;
        int rc = pthread_create(&stage_thread, &attr, shellInProcessWorkerMain, worker);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            if (worker->link.ctx) {
                shellRuntimeDestroyContext(worker->link.ctx);
            }
            shellThreadLinkRelease(&worker->link);
            shellFinalizeStageVproc(vp, 1);
            shellFreeCommand(cloned);
            free(cloned);
            free(worker);
            runtimeError(vm, "pipeline: failed to create foreground worker thread");
            shellUpdateStatus(1);
            shellAbortPipeline();
            return false;
        }
        if (getenv("PSCALI_PIPE_DEBUG")) {
            fprintf(stderr,
                    "[pipe-worker] fg-stage thread launched stage=%zu pid=%d pgid=%d\n",
                    stage_pos,
                    (int)stage_pid,
                    (int)ctx->pgid);
        }
        if (ctx->thread_started && stage_pos < ctx->stage_count) {
            ctx->thread_started[stage_pos] = false;
        }
        ctx->in_process_worker = true;
        return true;
    }

    if (!pipeline_tail || async_tail) {
        if (async_tail) {
            if (ctx->pids && stage_pos < ctx->stage_count) {
                if (ctx->pids[stage_pos] <= 0) {
                    int pid_hint = vprocReservePid();
                    if (pid_hint > 0) {
                        ctx->pids[stage_pos] = (pid_t)pid_hint;
                    }
                }
                async_stage_pid = ctx->pids[stage_pos];
                if (async_stage_pid > 0 && ctx->launched < (stage_pos + 1)) {
                    ctx->launched = stage_pos + 1;
                }
                if (ctx->pgid <= 0 && async_stage_pid > 0) {
                    ctx->pgid = async_stage_pid;
                }
            }
            if (async_stage_pid <= 0) {
                int pid_hint = vprocReservePid();
                if (pid_hint > 0) {
                    async_stage_pid = (pid_t)pid_hint;
                }
            }
            if (async_bg_pgid <= 0 && async_stage_pid > 0) {
                async_bg_pgid = (ctx->pgid > 0) ? ctx->pgid : async_stage_pid;
            }
            if (async_stage_pid > 0) {
                int shell_pid = vprocGetShellSelfPid();
                if (shell_pid > 0 && async_stage_pid != shell_pid) {
                    int shell_sid = vprocGetSid(shell_pid);
                    if (shell_sid > 0) {
                        (void)vprocSetSid((int)async_stage_pid, shell_sid);
                    }
                    /* The parent is the vproc that can wait: `wait` here, in a nested
                     * shell or a subshell, only matches its own children. */
                    (void)vprocSetParent((int)async_stage_pid, shellStageParentPid(shell_pid));
                }
                if (async_bg_pgid > 0) {
                    (void)vprocSetPgid((int)async_stage_pid, (int)async_bg_pgid);
                }
            }
        }
#if defined(PSCAL_TARGET_IOS)
        if (async_tail) {
            ShellCommand *cloned = shellCloneCommandForBackground(cmd);
            ShellBackgroundStageCtx *bg_ctx =
                (ShellBackgroundStageCtx *)calloc(1, sizeof(ShellBackgroundStageCtx));
            if (!cloned || !bg_ctx) {
                if (cloned) {
                    shellFreeCommand(cloned);
                    free(cloned);
                }
                free(bg_ctx);
                runtimeError(vm, "pipeline: unable to launch background stage");
                shellUpdateStatus(1);
                shellAbortPipeline();
                return false;
            }

            if (!cloned->body_state && shellJobNeedsOwnState(cloned)) {
                cloned->body_state = shellBodyStateCapture();
            }
            shellStageCaptureState(cloned, cmd, 1);
            bg_ctx->vm = vm;
            bg_ctx->cmd = cloned;
            bg_ctx->synthetic_pid = -1;
            if (async_stage_pid > 0) {
                bg_ctx->synthetic_pid = (int)async_stage_pid;
            }
            bg_ctx->shell_pid = vprocGetShellSelfPid();
            if (bg_ctx->shell_pid > 0) {
                bg_ctx->parent_pid = shellStageParentPid(bg_ctx->shell_pid);
            }
            bg_ctx->kernel_pid = vprocGetKernelPid();
            bg_ctx->runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext
                ? PSCALRuntimeGetCurrentRuntimeContext()
                : NULL;
            bg_ctx->link = shellThreadLinkCapture();
            bg_ctx->vp = shellBackgroundStageCreateVProc(bg_ctx, cloned);

            pthread_t stage_thread = 0;
            int rc = bg_ctx->vp
                ? vprocSpawnThread(bg_ctx->vp, shellInProcessBackgroundMain, bg_ctx, &stage_thread)
                : ENOMEM;
            if (rc != 0) {
                shellFinalizeStageVproc(bg_ctx->vp, 1);
                shellFreeCommand(cloned);
                free(cloned);
                if (bg_ctx->link.ctx) {
                    shellRuntimeDestroyContext(bg_ctx->link.ctx);
                }
                shellThreadLinkRelease(&bg_ctx->link);
                free(bg_ctx);
                runtimeError(vm, "pipeline: failed to create background stage thread");
                shellUpdateStatus(1);
                shellAbortPipeline();
                return false;
            }
            pthread_detach(stage_thread);

            if (async_stage_pid > 0) {
                pid_t bg_pgid = (async_bg_pgid > 0) ? async_bg_pgid : async_stage_pid;
                pid_t bg_pids[1] = { async_stage_pid };
                ShellJob *job = shellRegisterJob(bg_pgid, bg_pids, 1, cmd);
                if (job) {
                    shellJobMarkRunning(job);
                    job->last_status = 0;
                }
                /* The job's own pid: in a subshell the group is the subshell's. */
                gShellRuntime.last_bg_pid = async_stage_pid;
#if defined(PSCAL_TARGET_IOS)
                shellPrintJobStarted(job, bg_pgid);
#endif
            }

            /* Background single-stage workers run detached from shared pipeline state. */
            shellResetPipeline();
            shellUpdateStatus(0);
            return true;
        }
#endif

        ShellPipelineStageThreadCtx *tctx =
            (ShellPipelineStageThreadCtx *)calloc(1, sizeof(ShellPipelineStageThreadCtx));
        ShellCommand *cloned = shellCloneCommandForBackground(cmd);
        if (!tctx || !cloned) {
            free(tctx);
            shellFreeCommand(cloned);
            free(cloned);
            runtimeError(vm, "pipeline: unable to launch stage");
            shellUpdateStatus(1);
            shellAbortPipeline();
            return false;
        }
        tctx->vm = vm;
        tctx->ctx = ctx;
        tctx->cmd = cloned;
        tctx->stage_pos = stage_pos;
        tctx->pipeline_head = pipeline_head;
        tctx->pipeline_tail = pipeline_tail;
        tctx->owns_ctx_snapshot = false;
#if defined(PSCAL_TARGET_IOS)
        tctx->session_stdio = ctx->session_stdio;
        tctx->runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext
            ? PSCALRuntimeGetCurrentRuntimeContext()
            : NULL;
        tctx->link = shellThreadLinkCapture();
        shellStageCaptureState(cloned, cmd, ctx->stage_count);
#endif
        if (async_tail) {
            ShellPipelineContext *ctx_clone = shellCloneSingleStagePipelineContext(ctx);
            if (!ctx_clone) {
                shellFreeCommand(cloned);
                free(cloned);
                free(tctx);
                runtimeError(vm, "pipeline: unable to snapshot background stage");
                shellUpdateStatus(1);
                shellAbortPipeline();
                return false;
            }
            tctx->ctx = ctx_clone;
            tctx->owns_ctx_snapshot = true;
        }

        pthread_t stage_thread = 0;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
        if (async_tail) {
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        }
        pthread_t *thread_slot = async_tail ? &stage_thread : &ctx->threads[stage_pos];
        int rc = pthread_create(thread_slot, &attr, shellPipelineStageThreadMain, tctx);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            if (tctx->cmd) {
                shellFreeCommand(tctx->cmd);
                free(tctx->cmd);
                tctx->cmd = NULL;
            }
            if (tctx->owns_ctx_snapshot) {
                shellFreeClonedPipelineContext(tctx->ctx);
                tctx->ctx = NULL;
            }
#if defined(PSCAL_TARGET_IOS)
            if (tctx->link.ctx) {
                shellRuntimeDestroyContext(tctx->link.ctx);
            }
            shellThreadLinkRelease(&tctx->link);
#endif
            free(tctx);
            runtimeError(vm, "pipeline: failed to create stage thread");
            shellUpdateStatus(1);
            shellAbortPipeline();
            return false;
        }
        if (!async_tail) {
            ctx->thread_started[stage_pos] = true;
        }
        if (async_tail) {
            if (ctx->pids && stage_pos < ctx->stage_count && ctx->pids[stage_pos] > 0) {
                pid_t bg_pgid = (async_bg_pgid > 0) ? async_bg_pgid : ctx->pids[stage_pos];
                pid_t bg_pids[1] = { ctx->pids[stage_pos] };
                ShellJob *job = shellRegisterJob(bg_pgid, bg_pids, 1, cmd);
                if (job) {
                    shellJobMarkRunning(job);
                    job->last_status = 0;
                }
                gShellRuntime.last_bg_pid = ctx->pids[stage_pos];
#if defined(PSCAL_TARGET_IOS)
                shellPrintJobStarted(job, bg_pgid);
#endif
            }
            /* Background single-stage workers run detached from shared pipeline state. */
            shellResetPipeline();
            shellUpdateStatus(0);
        }
        return true;
    }

    /* Tail stage runs inline so we can join upstream threads afterward. */
    vprocSetPipelineStage(true);
    int status = shellPipelineRunStage(vm, ctx, cmd, stage_pos, pipeline_head, pipeline_tail);
    vprocSetPipelineStage(false);
    ctx->last_status = status;
    if (ctx->stage_status && stage_pos < ctx->stage_count) {
        ctx->stage_status[stage_pos] = status;
    }

    for (size_t i = 0; i < stage_pos; ++i) {
        if (ctx->thread_started && ctx->thread_started[i]) {
            if (getenv("PSCALI_PIPE_DEBUG")) {
                fprintf(stderr, "[pipe-join] waiting stage=%zu\n", i);
            }
            pthread_join(ctx->threads[i], NULL);
            ctx->thread_started[i] = false;
            if (getenv("PSCALI_PIPE_DEBUG")) {
                fprintf(stderr, "[pipe-join] joined stage=%zu\n", i);
            }
        }
    }

    /* Proactively close any remaining pipe endpoints now that the pipeline is
     * complete. This ensures blocked readers see EOF even if an upstream
     * stage bailed before cleaning up. */
    if (ctx->pipes) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pipes[i][0] >= 0) {
                vprocHostClose(ctx->pipes[i][0]);
                ctx->pipes[i][0] = -1;
            }
            if (ctx->pipes[i][1] >= 0) {
                vprocHostClose(ctx->pipes[i][1]);
                ctx->pipes[i][1] = -1;
            }
        }
    }
    return true;
}
#endif /* PSCAL_TARGET_IOS */

/* A trap's `exit` or `return` can end a chunk between its pipeline's stages. Those already running
 * use this state and pick up their pipe ends with no lock, and one may never end, so nothing they
 * can reach is closed, freed or waited for. A context this happened in is left to them for good. */
bool shellRuntimeChunkLeftStages(void) {
#if defined(PSCAL_TARGET_IOS)
    if (gShellRuntime.left_stages_running) {
        return true;
    }
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (ctx->active && ctx->thread_started) {
        for (size_t i = 0; i < ctx->stage_count; ++i) {
            if (ctx->thread_started[i]) {
                gShellRuntime.left_stages_running = true;
                return true;
            }
        }
    }
#endif
    return false;
}

static void shellExitUnlessInteractive(VM *vm, const char *reason) {
    if (shellRuntimeIsInteractive()) {
        return;
    }
    shellRuntimeRequestExit();
    if (vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = reason;
    }
}

#if defined(PSCAL_TARGET_IOS)
/* What bash would find as a file. Nothing execs here, and a program is whatever runs in its
 * place: an applet, a frontend, a script, or a builtin that hosts also have as a file. */
static bool shellProgramExists(const char *name) {
    char resolved[PATH_MAX];
    if (strchr(name, '/')) {
        return shellResolveExecutableCandidate(name, resolved, sizeof(resolved));
    }
    if (smallclueFindApplet(name) || shellIOSFrontendHandler(name)) {
        return true;
    }
    static const char *const kShellProgramBuiltins[] = {"echo", "false", "kill", "printf", "pwd", "sleep", "test", "true", "["};
    for (size_t i = 0; i < sizeof(kShellProgramBuiltins) / sizeof(kShellProgramBuiltins[0]); ++i) {
        if (strcmp(name, kShellProgramBuiltins[i]) == 0 && shellIsRuntimeBuiltin(name)) {
            return true;
        }
    }
    return shellResolveExecutionPath(name, resolved, sizeof(resolved));
}

static bool shellEnvEntryNone(const char *entry, void *arg) {
    (void)entry;
    (void)arg;
    return false;
}

#endif

/* exec's options: false if they are wrong, which is reported unless vm is NULL. */
static bool shellExecParseOptions(VM *vm, char ***argv, const char **out_arg0, bool *out_clear_env, bool *out_login) {
    for (char **cursor = *argv; cursor[0] && cursor[0][0] == '-' && cursor[0][1]; ) {
        if (strcmp(cursor[0], "--") == 0) {
            *argv = cursor + 1;
            return true;
        }
        const char *flag = cursor[0] + 1;
        cursor++;
        for (; *flag; ++flag) {
            if (*flag == 'c') {
                *out_clear_env = true;
            } else if (*flag == 'l') {
                *out_login = true;
            } else if (*flag == 'a' && (flag[1] || cursor[0])) {
                *out_arg0 = flag[1] ? flag + 1 : *cursor++;
                break;
            } else {
                if (vm) {
                    runtimeError(vm, *flag == 'a' ? "exec: -a: option requires an argument" : "exec: -%c: invalid option", *flag);
                    shellUpdateStatus(2);
                }
                return false;
            }
        }
        *argv = cursor;
    }
    return true;
}

static bool shellExecuteExecBuiltin(VM *vm, ShellCommand *cmd) {
    if (!shellCommandIsExecBuiltin(cmd)) {
        return false;
    }
    if (cmd->background) {
        runtimeError(vm, "exec: cannot be used in background");
        shellUpdateStatus(1);
        return true;
    }

    ShellExecRedirBackup *backups = NULL;
    size_t backup_count = 0;
    if (!shellApplyExecRedirections(cmd, &backups, &backup_count)) {
        return true;
    }

    char **argv = &cmd->argv[1];
    const char *arg0 = NULL;
    bool clear_env = false;
    bool login = false;
    if (cmd->argc > 1 && !shellExecParseOptions(vm, &argv, &arg0, &clear_env, &login)) {
        shellRestoreExecRedirections(backups, backup_count);
        shellFreeExecRedirBackups(backups, backup_count);
        return true;
    }

    /* Only redirections: they stay. */
    if (cmd->argc <= 1 || !argv[0]) {
        if (shellCommandSubKeepExecBackups(backups, backup_count)) {
            shellUpdateStatus(0);
            return true;
        }
        shellFreeExecRedirBackups(backups, backup_count);
#if defined(PSCAL_TARGET_IOS)
        for (size_t i = 0; i < cmd->redir_count; ++i) {
            if (cmd->redirs[i].fd > STDERR_FILENO) {
                vprocShellFdFollow(cmd->redirs[i].fd);
            }
        }
#endif
        shellUpdateStatus(0);
        return true;
    }

#if defined(PSCAL_TARGET_IOS)
    /* The program runs as a call, and when it is done so is this shell, as if it had been
     * replaced: no EXIT trap. */
    if (shellProgramExists(argv[0])) {
        ShellCommand program;
        memset(&program, 0, sizeof(program));
        program.argv = argv;
        program.argc = cmd->argc - (size_t)(argv - cmd->argv);
        program.ignore_functions = true;
        program.pipeline_index = -1;
        program.assignments = cmd->assignments;
        program.assignment_count = cmd->assignment_count;
        program.line = cmd->line;
        program.column = cmd->column;
        if (clear_env) {
            program.program_env = vprocEnvCaptureFiltered(shellEnvEntryNone, NULL);
        }
        /* As any stage, or a subshell's `cat < f` stops itself for reading in the background. */
        vprocSetPipelineStage(true);
        shellRunBuiltinInProcess(vm, &program, -1, -1, -1, false);
        vprocSetPipelineStage(false);
        shellFreeExecRedirBackups(backups, backup_count);
        int status = gShellRuntime.last_status;
        shellRuntimeSetExitTrap(SHELL_TRAP_ACTION_DEFAULT, NULL);
        shellUpdateStatus(status);
        shellRuntimeRequestExit();
        if (vm) {
            vm->exit_requested = true;
            vm->current_builtin_name = "exec";
        }
        return true;
    }
    int err = ENOENT;
#else
    /* Not in a child here: a failed exec has to find everything as it was. */
    PscalEnvSnapshot exec_env = {0};
    bool have_exec_env = pscalEnvSnapshotTake(&exec_env);
    char *exec_found = NULL;
    bool exec_lookup_ok = shellLookupForExec(argv[0], &exec_found, clear_env);
    if (clear_env) {
        shellEnvironmentClear();
    } else {
        shellEnvironmentForChild(cmd);
    }
    char *original = argv[0];
    char *shown = shellExecShownName(original, arg0, login);
    if (shown) {
        argv[0] = shown;
    }
    int err = ENOENT;
    if (exec_lookup_ok) {
        execvp(exec_found ? exec_found : original, argv);
        err = errno;
    }
    argv[0] = original;
    free(shown);
    free(exec_found);
    if (have_exec_env) {
        (void)pscalEnvSnapshotRestore(&exec_env);
    }
#endif
    if (err == ENOENT && !strchr(argv[0], '/')) {
        runtimeError(vm, "exec: %s: not found", argv[0]);
    } else {
        runtimeError(vm, "exec: %s: %s", argv[0], strerror(err));
    }
    shellRestoreExecRedirections(backups, backup_count);
    shellFreeExecRedirBackups(backups, backup_count);
    shellUpdateStatus((err == ENOENT) ? 127 : 126);
    /* Neither a script nor a subshell outlives a failed exec, as under bash. */
    if (gShellRuntime.in_subshell) {
        shellRuntimeRequestExit();
        if (vm) {
            vm->exit_requested = true;
            vm->current_builtin_name = "exec";
        }
    } else if (!shellShoptOptionEnabled("execfail")) {
        shellExitUnlessInteractive(vm, "exec");
    }
    return true;
}

/* `PATH=/bin export PATH`: export, readonly, local and declare -x act on the variable the
 * prefix made, and it stays, still exported. Any other prefix ends with its command. */
static void shellFinishAssignments(const ShellCommand *cmd, ShellAssignmentBackup *backups, size_t count) {
    char lookup_buf[PATH_MAX];
    const char *name = (cmd && cmd->argc > 1 && !cmd->background && gShellRuntime.pipeline.stage_count <= 1)
                           ? shellCommandLookupName(cmd->argv[0], lookup_buf, sizeof(lookup_buf))
                           : NULL;
    const char *canonical = name ? shellBuiltinCanonicalName(name) : NULL;
    bool keep = canonical && (strcmp(canonical, "export") == 0 || strcmp(canonical, "readonly") == 0 ||
                              strcmp(canonical, "local") == 0);
    if (canonical && (strcmp(canonical, "declare") == 0 || strcmp(canonical, "typeset") == 0)) {
        for (size_t j = 1; j < cmd->argc; ++j) {
            keep = keep || (cmd->argv[j][0] == '-' && strchr(cmd->argv[j], 'x'));
        }
    }
    for (size_t i = 0; keep && i < count; ++i) {
        for (size_t j = 1; backups[i].name && j < cmd->argc; ++j) {
            if (strcmp(cmd->argv[j], backups[i].name) == 0) {
                free(backups[i].name);
                free(backups[i].previous_value);
                backups[i].name = NULL;
                backups[i].previous_value = NULL;
            }
        }
    }
    shellRestoreAssignments(backups, count);
}

/* `exec cmd` must not replace the process when this shell is only part of one: a
 * substitution runs in the shell's process. */
static bool shellExecRunsAsCommand(const ShellCommand *cmd) {
    return gShellCommandSubFrame && shellCommandIsExecBuiltin(cmd) && cmd->argc > 1 && !cmd->background;
}

static Value shellRunExpandedCommand(VM *vm, ShellCommand *cmd);

/* The command runs as any other does, and then the substitution ends, as it would have. */
static Value shellRunExecAsCommand(VM *vm, ShellCommand *cmd) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    /* A stage has a shell of its own to end. */
    bool ends_shell = !ctx->active || ctx->stage_count <= 1;
    while (shellExecRunsAsCommand(cmd)) {
        char **argv = &cmd->argv[1];
        const char *arg0 = NULL;
        bool clear_env = false;
        bool login = false;
        /* Wrong options or no program: the builtin reports under its redirections, or keeps them. */
        if (!shellExecParseOptions(NULL, &argv, &arg0, &clear_env, &login) || !argv[0] || !argv[0][0]) {
            return shellRunExpandedCommand(vm, cmd);
        }
#if defined(PSCAL_TARGET_IOS)
        if (clear_env) {
            vprocEnvRelease(cmd->program_env);
            cmd->program_env = vprocEnvCaptureFiltered(shellEnvEntryNone, NULL);
        }
#else
        cmd->exec_clear_env = cmd->exec_clear_env || clear_env;
        if (arg0 || login) {
            free(cmd->exec_arg0);
            cmd->exec_arg0 = shellExecShownName(argv[0], arg0, login);
        }
#endif
        size_t skip = (size_t)(argv - cmd->argv);
        for (size_t i = 0; i < skip; ++i) {
            free(cmd->argv[i]);
        }
        memmove(cmd->argv, argv, (cmd->argc - skip + 1) * sizeof(char *));
        cmd->argc -= skip;
    }
    cmd->ignore_functions = true;
    Value result = shellRunExpandedCommand(vm, cmd);
    if (ends_shell) {
        shellRuntimeRequestExit();
        if (vm) {
            vm->exit_requested = true;
            vm->current_builtin_name = "exec";
        }
    }
    return result;
}

static Value shellExecuteCommand(VM *vm, ShellCommand *cmd) {
    if (!cmd) {
        shellScalarCacheInvalidate();
        return makeVoid();
    }
    bool exit_already_requested = gShellExitRequested;
    /* What came during a substitution in its words waits for the command: `x=$(cmd)` assigns, and
     * then the trap runs. A pipeline waits as a whole: the stages before this one have started, and
     * they use its context. */
    if (!cmd->ran_command_substitution && !(gShellRuntime.pipeline.active && cmd->pipeline_index > 0)) {
        shellRuntimeProcessPendingSignals();
    }
    if (gShellExitRequested && !exit_already_requested) {
        /* A trap, or a signal with none, ended the shell before this command. */
        shellFreeCommand(cmd);
        shellScalarCacheInvalidate();
        return makeVoid();
    }
    if (shellLoopSkipActive()) {
        shellFreeCommand(cmd);
        shellScalarCacheInvalidate();
        return makeVoid();
    }
    if (gShellRuntime.xtrace_enabled) {
        shellTraceCommand(cmd);
    }
    if (!shellExpandAliasesIfNeeded(vm, cmd)) {
        shellUpdateStatus(1);
        shellFreeCommand(cmd);
        shellScalarCacheInvalidate();
        return makeVoid();
    }
    if (shellExecRunsAsCommand(cmd)) {
        return shellRunExecAsCommand(vm, cmd);
    }
    return shellRunExpandedCommand(vm, cmd);
}

static Value shellRunExpandedCommand(VM *vm, ShellCommand *cmd) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    /* A stage or background job with no command word, like `x=1 | cat` or `> f &`, runs
     * apart from the shell as any other does: it holds its pipe ends, and its assignments
     * do not reach the shell. `:` gives it a command to run. */
    if (cmd->argc == 0 && (cmd->background || (ctx->active && ctx->stage_count > 1))) {
        char *colon = strdup(":");
        if (!colon || !shellCommandAppendArgOwned(cmd, colon)) {
            runtimeError(vm, "shell exec: out of memory");
            if (ctx->active) {
                shellAbortPipeline();
            }
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            return makeVoid();
        }
        cmd->empty_stage_status = cmd->ran_command_substitution ? gShellRuntime.last_status : 0;
    }
#if defined(PSCAL_TARGET_IOS)
    bool prefer_worker = shellShouldRunCommandInForegroundWorker(cmd);
    bool force_pipeline_builtin = false;
    if (cmd->argc > 0 && !shellCommandIsExecBuiltin(cmd)) {
        const char *cmd_name_raw = (cmd->argv && cmd->argv[0]) ? cmd->argv[0] : NULL;
        char lookup_buf[PATH_MAX];
        const char *cmd_name = shellCommandLookupName(cmd_name_raw, lookup_buf, sizeof(lookup_buf));
        if (shellIsRuntimeBuiltin(cmd_name) || smallclueFindApplet(cmd_name) != NULL) {
            force_pipeline_builtin = true;
            /*
             * Job-control builtins must execute in the shell context so fg/bg/wait
             * can manage and wait on the shell's real job table directly. A trap's
             * handler belongs to the shell's vproc, not to one made for this command.
             */
            const char *canonical = shellBuiltinCanonicalName(cmd_name);
            if (canonical &&
                (strcmp(canonical, "fg") == 0 ||
                 strcmp(canonical, "bg") == 0 ||
                 strcmp(canonical, "jobs") == 0 ||
                 strcmp(canonical, "kill") == 0 ||
                 strcmp(canonical, "wait") == 0 ||
                 strcmp(canonical, "disown") == 0 ||
                 strcmp(canonical, "trap") == 0)) {
                force_pipeline_builtin = false;
            }
        }
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (cmd->background) {
        bool need_new = (!ctx->active) || (!ctx->background) || (ctx->stage_count != 1) ||
                        (!cmd->is_pipeline_head) || (!cmd->is_pipeline_tail);
        if (need_new) {
            shellResetPipeline();
            if (!shellEnsurePipeline(1, false)) {
                runtimeError(vm, "pipeline: unable to allocate background context");
                shellUpdateStatus(1);
                shellFreeCommand(cmd);
                shellScalarCacheInvalidate();
                return makeVoid();
            }
            ctx = &gShellRuntime.pipeline;
            ctx->active = true;
            ctx->in_process = true;
            ctx->stage_count = 1;
            ctx->launched = 0;
            ctx->background = true;
            cmd->pipeline_index = 0;
            cmd->is_pipeline_head = true;
            cmd->is_pipeline_tail = true;
            if (getenv("PSCALI_PIPE_DEBUG")) {
                fprintf(stderr, "[pipeline-bg] auto in-process pipeline reset for background '%s'\n",
                        (cmd->argv && cmd->argv[0]) ? cmd->argv[0] : "<null>");
            }
        }
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (force_pipeline_builtin && !ctx->active && !cmd->background) {
        if (!shellEnsurePipeline(1, false)) {
            force_pipeline_builtin = false;
        } else {
            ctx = &gShellRuntime.pipeline;
            cmd->pipeline_index = 0;
            cmd->is_pipeline_head = true;
            cmd->is_pipeline_tail = true;
        }
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (prefer_worker && !ctx->active && !cmd->background) {
        if (!shellEnsurePipeline(1, false)) {
            prefer_worker = false;
        } else {
            ctx = &gShellRuntime.pipeline;
            cmd->pipeline_index = 0;
            cmd->is_pipeline_head = true;
            cmd->is_pipeline_tail = true;
        }
    }
#endif
    bool pipeline_head = cmd->is_pipeline_head;
    bool pipeline_tail = cmd->is_pipeline_tail;
    size_t stage_pos = 0;
    if (ctx->active) {
        size_t stage_count = ctx->stage_count ? ctx->stage_count : 1;
        if (cmd->pipeline_index >= 0) {
            stage_pos = (size_t)cmd->pipeline_index;
        } else {
            stage_pos = ctx->launched;
        }
        if (stage_pos >= stage_count) {
            stage_pos = stage_count - 1;
        }
        pipeline_head = (stage_pos == 0);
        pipeline_tail = ((stage_pos + 1) >= stage_count);
    }
    ShellAssignmentBackup *assignment_backups = NULL;
    size_t assignment_backup_count = 0;
    bool assignments_applied = false;

    if (cmd->argc == 0) {
        const char *failed_assignment = NULL;
        bool invalid_assignment = false;
        if (cmd->assignment_count > 0) {
            if (!shellApplyAssignmentsPermanently(cmd, &failed_assignment, &invalid_assignment)) {
                if (invalid_assignment) {
                    runtimeError(vm, "shell exec: invalid assignment '%s'",
                                 failed_assignment ? failed_assignment : "<assignment>");
                    shellUpdateStatus(1);
                } else if (shellReadonlyGetErrorName()) {
                    runtimeError(vm, "%s: readonly variable", shellReadonlyGetErrorName());
                    shellUpdateStatus(1);
                } else {
                    runtimeError(vm, "shell exec: failed to apply assignment '%s': %s",
                                 failed_assignment ? failed_assignment : "<assignment>",
                                 strerror(errno));
                    shellUpdateStatus(1);
                }
                /* Readonly and the like: with no command to fail, a script ends here, as under bash. */
                shellExitUnlessInteractive(vm, "assignment");
            } else if (!cmd->ran_command_substitution) {
                /* `d=$(mktemp -d) || exit 1`: otherwise the status stays that of the
                 * last substitution, which shellRunCommandSubstitution has set. */
                shellUpdateStatus(0);
            }
        } else if (!cmd->ran_command_substitution) {
            shellUpdateStatus(0);
        }
        /* The files are opened and the descriptors put back at once: `> f` creates f. */
        if (cmd->redir_count > 0) {
            ShellExecRedirBackup *redir_backups = NULL;
            size_t redir_backup_count = 0;
            shellFdVarsEndWithCommand(cmd);
            shellFlushStandardStreams();
            if (shellApplyExecRedirections(cmd, &redir_backups, &redir_backup_count)) {
                shellRestoreExecRedirections(redir_backups, redir_backup_count);
                shellFreeExecRedirBackups(redir_backups, redir_backup_count);
            }
        }
        if (ctx->active) {
            int status = gShellRuntime.last_status;
            if (ctx->stage_count <= 1 && ctx->negated) {
                status = (status == 0) ? 1 : 0;
                shellUpdateStatus(status);
            }
            ctx->last_status = status;
            if (ctx->stage_count <= 1) {
                shellResetPipeline();
            }
        }
        shellFreeCommand(cmd);
        shellScalarCacheInvalidate();
        return makeVoid();
    }

    const char *failed_assignment = NULL;
    bool invalid_assignment = false;
    /* Allow VAR=val words attached to local/declare/typeset to bind into the
     * current function's local scope instead of the environment. */
    bool previous_local_scope_flag = gShellRuntime.local_scope_active;
    bool localizes_assignments = false;
    if (cmd->argc > 0 && cmd->argv && cmd->argv[0]) {
        char lookup_buf[PATH_MAX];
        const char *lookup_name = shellCommandLookupName(cmd->argv[0], lookup_buf, sizeof(lookup_buf));
        const char *canon = shellBuiltinCanonicalName(lookup_name);
        if (canon && (strcmp(canon, "local") == 0 ||
                      strcmp(canon, "declare") == 0 ||
                      strcmp(canon, "typeset") == 0)) {
            if (shellLocalScopeDepth() > 0) {
                localizes_assignments = true;
                gShellRuntime.local_scope_active = true;
            }
        }
    }
    if (cmd->assignment_count > 0) {
        if (!shellApplyAssignments(cmd, &assignment_backups, &assignment_backup_count,
                                   &failed_assignment, &invalid_assignment, true, true)) {
            if (invalid_assignment) {
                runtimeError(vm, "shell exec: invalid assignment '%s'",
                             failed_assignment ? failed_assignment : "<assignment>");
                shellUpdateStatus(1);
            } else {
                runtimeError(vm, "shell exec: failed to apply assignment '%s': %s",
                             failed_assignment ? failed_assignment : "<assignment>",
                             strerror(errno));
                shellUpdateStatus(1);
            }
            if (ctx->active) {
                shellAbortPipeline();
            }
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            if (localizes_assignments) {
                gShellRuntime.local_scope_active = previous_local_scope_flag;
            }
            return makeVoid();
        }
        assignments_applied = true;
    }
    if (localizes_assignments) {
        gShellRuntime.local_scope_active = previous_local_scope_flag;
    }
    /* A readonly name in the prefix under set -e: nothing more runs. */
    if (assignments_applied && gShellExitRequested) {
        shellRestoreAssignments(assignment_backups, assignment_backup_count);
        if (ctx->active) {
            shellAbortPipeline();
        }
        shellFreeCommand(cmd);
        shellScalarCacheInvalidate();
        return makeVoid();
    }

    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    if (ctx->active) {
        if (ctx->stage_count == 1 && shellCommandIsExecBuiltin(cmd)) {
            shellExecuteExecBuiltin(vm, cmd);
            if (assignments_applied) {
                shellFinishAssignments(cmd, assignment_backups, assignment_backup_count);
                assignments_applied = false;
                assignment_backups = NULL;
                assignment_backup_count = 0;
            }
            int status = gShellRuntime.last_status;
            if (ctx->negated) {
                status = (status == 0) ? 1 : 0;
                shellUpdateStatus(status);
            }
            ctx->last_status = status;
            shellResetPipeline();
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            return makeVoid();
        }
        if (ctx->stage_count == 1 && !ctx->background && !cmd->background
#if defined(PSCAL_TARGET_IOS)
            && !force_pipeline_builtin
#endif
            ) {
#if defined(PSCAL_TARGET_IOS)
            if (!prefer_worker) {
                ShellVprocLabelScope label_scope;
                shellVprocLabelScopeBegin(&label_scope, cmd);
                bool ran_builtin = shellInvokeBuiltin(vm, cmd);
                if (!ran_builtin) {
                    ran_builtin = shellDispatchSmallclueAppletInProcess(vm, cmd);
                }
                shellVprocLabelScopeEnd(&label_scope);
                if (ran_builtin) {
                    if (assignments_applied) {
                        shellFinishAssignments(cmd, assignment_backups, assignment_backup_count);
                        assignments_applied = false;
                        assignment_backups = NULL;
                        assignment_backup_count = 0;
                    }
                    int status = gShellRuntime.last_status;
                    if (ctx->negated) {
                        status = (status == 0) ? 1 : 0;
                        shellUpdateStatus(status);
                    }
                    ctx->last_status = status;
                    shellResetPipeline();
                    shellFreeCommand(cmd);
                    shellScalarCacheInvalidate();
                    return makeVoid();
                }
            }
#else
            bool ran_builtin = !cmd->body && shellInvokeBuiltin(vm, cmd);
            if (ran_builtin) {
                if (assignments_applied) {
                    shellFinishAssignments(cmd, assignment_backups, assignment_backup_count);
                    assignments_applied = false;
                    assignment_backups = NULL;
                    assignment_backup_count = 0;
                }
                int status = gShellRuntime.last_status;
                if (ctx->negated) {
                    status = (status == 0) ? 1 : 0;
                    shellUpdateStatus(status);
                }
                ctx->last_status = status;
                shellResetPipeline();
                shellFreeCommand(cmd);
                shellScalarCacheInvalidate();
                return makeVoid();
            }
#endif
        }
        size_t idx = stage_pos;
        if (idx >= ctx->stage_count) {
            if (assignments_applied) {
                shellFinishAssignments(cmd, assignment_backups, assignment_backup_count);
            }
            assignment_backups = NULL;
            assignment_backup_count = 0;
            runtimeError(vm, "shell exec: pipeline index out of range");
            shellFreeCommand(cmd);
            shellResetPipeline();
            shellScalarCacheInvalidate();
            return makeVoid();
        }
        bool pipeline_uses_pipes = ctx->pipes != NULL;
        if (pipeline_uses_pipes && ctx->stage_count > 1) {
            if (!pipeline_head) {
                stdin_fd = ctx->pipes[idx - 1][0];
            }
            if (!pipeline_tail) {
                stdout_fd = ctx->pipes[idx][1];
            }
        }
        if (pipeline_uses_pipes &&
            ctx->merge_stderr && idx < ctx->stage_count && ctx->merge_stderr[idx]) {
            stderr_fd = stdout_fd;
        }
#if defined(PSCAL_TARGET_IOS)
        if (cmd->background) {
            ctx->background = true;
        }
        if (ctx->in_process) {
            bool stage_ok = shellPipelineExecuteStageInProcess(vm,
                                                               ctx,
                                                               cmd,
                                                               stage_pos,
                                                               pipeline_head,
                                                               pipeline_tail);
            /* A stage on another thread took what it needs while it was launched. */
            if (assignments_applied) {
                shellFinishAssignments(cmd, assignment_backups, assignment_backup_count);
                assignments_applied = false;
                assignment_backups = NULL;
                assignment_backup_count = 0;
            }
            if (!stage_ok) {
                shellAbortPipeline();
            } else if (pipeline_tail) {
                if (ctx->background) {
                    shellScalarCacheInvalidate();
                    return makeVoid();
                }
                /* Foreground in-process pipelines may have non-tail stages
                 * running on worker threads. Always finish via shellFinishPipeline
                 * so those stages are waited/joined before teardown. */
                shellFinishPipeline(cmd);
                shellRuntimeProcessPendingSignals();
            }
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            return makeVoid();
        }
#endif
    } else {
        if (shellCommandIsExecBuiltin(cmd)) {
            shellExecuteExecBuiltin(vm, cmd);
            if (assignments_applied) {
                shellFinishAssignments(cmd, assignment_backups, assignment_backup_count);
                assignments_applied = false;
                assignment_backups = NULL;
                assignment_backup_count = 0;
            }
            shellFreeCommand(cmd);
            shellScalarCacheInvalidate();
            return makeVoid();
        }
        {
#if defined(PSCAL_TARGET_IOS)
            ShellVprocLabelScope label_scope;
            shellVprocLabelScopeBegin(&label_scope, cmd);
#endif
            bool ran_builtin = !cmd->body && shellInvokeBuiltin(vm, cmd);
#if defined(PSCAL_TARGET_IOS)
            if (!ran_builtin && !cmd->body) {
                ran_builtin = shellDispatchSmallclueAppletInProcess(vm, cmd);
            }
            shellVprocLabelScopeEnd(&label_scope);
#endif
            if (ran_builtin) {
                if (assignments_applied) {
                    shellFinishAssignments(cmd, assignment_backups, assignment_backup_count);
                    assignments_applied = false;
                    assignment_backups = NULL;
                    assignment_backup_count = 0;
                }
                shellFreeCommand(cmd);
                shellScalarCacheInvalidate();
                return makeVoid();
            }
        }
    }

    bool background_execution = cmd->background;
    if (ctx->active) {
        if (ctx->background) {
            background_execution = true;
        }
        if (cmd->background) {
            ctx->background = true;
        }
    }

    pid_t child = -1;
    if (!cmd->body && cmd->argv && cmd->argv[0] && cmd->argv[0][0] != '\0') {
        shellHashRecordExecution(cmd->argv[0]);
    }
    bool ignore_job_signals = background_execution &&
                              (!gShellRuntime.job_control_enabled
                               );

    /* A substitution has no job control, whatever shell it is in. At a prompt this command
     * would stop on the terminal, and hold the substitution's pipe for as long as it did. */
    int terminal_stand_in = -1;
    if (background_execution && gShellCommandSubFrame && stdin_fd < 0 && isatty(STDIN_FILENO)) {
        terminal_stand_in = open("/dev/null", O_RDONLY);
        stdin_fd = terminal_stand_in;
    }

    int spawn_err = shellSpawnProcess(vm,
                                      cmd,
                                      stdin_fd,
                                      stdout_fd,
                                      stderr_fd,
                                      &child,
                                      ignore_job_signals);
    if (terminal_stand_in >= 0) {
        close(terminal_stand_in);
    }
    if (assignments_applied) {
        shellFinishAssignments(cmd, assignment_backups, assignment_backup_count);
        assignments_applied = false;
        assignment_backups = NULL;
        assignment_backup_count = 0;
    }
    if (spawn_err != 0) {
        runtimeError(vm, "shell exec: failed to spawn '%s': %s", cmd->argv[0], strerror(spawn_err));
        if (ctx->active) {
            shellAbortPipeline();
        }
        shellFreeCommand(cmd);
        shellUpdateStatus(127);
        shellScalarCacheInvalidate();
        return makeVoid();
    }

    /* Commands in a subshell stay in its process group, the one the terminal knows. */
    if (gShellRuntime.in_subshell) {
        /* nothing to do */
    } else if (ctx->active) {
        pid_t target_pgid = (ctx->pgid > 0) ? ctx->pgid : child;
        if (setpgid(child, target_pgid) != 0) {
            if (errno != EACCES && errno != ESRCH) {
                /* best-effort: ignore errors from lack of job control */
            }
        }
        if (ctx->pgid <= 0) {
            ctx->pgid = target_pgid;
        }
    } else {
        if (setpgid(child, child) != 0) {
            if (errno != EACCES && errno != ESRCH) {
                /* ignore */
            }
        }
    }

    if (ctx->active) {
        if (!pipeline_head && stdin_fd >= 0) {
            close(stdin_fd);
            if (cmd->pipeline_index > 0) {
                ctx->pipes[cmd->pipeline_index - 1][0] = -1;
            }
        }
        if (!pipeline_tail && stdout_fd >= 0) {
            close(stdout_fd);
            ctx->pipes[cmd->pipeline_index][1] = -1;
        }
        ctx->pids[ctx->launched++] = child;
        if (pipeline_tail) {
            ctx->background = cmd->background;
            shellFinishPipeline(cmd);
            shellRuntimeProcessPendingSignals();
        }
    } else {
        int status = 0;
        if (!cmd->background) {
            shellEnsureJobControl();
            bool job_control = gShellRuntime.job_control_enabled;
            bool stopped = false;
            if (job_control) {
                shellJobControlSetForeground(child);
            }
            ShellWaitResult wait_res_child;
            shellWaitPidDetailed(child, job_control, &wait_res_child);
            status = wait_res_child.status_mapped;
            stopped = wait_res_child.stopped;
            if (job_control) {
                shellJobControlRestoreForeground();
            }
            if (!cmd->background && status >= 128 && status < 128 + NSIG) {
                shellHandleStatusSignal(status - 128);
            }
            if (stopped && job_control) {
                pid_t job_pids[1];
                job_pids[0] = child;
                ShellJob *job = shellRegisterJob(child, job_pids, 1, cmd);
                if (job) {
                    shellJobMarkStopped(job, status);
                }
#if defined(PSCAL_TARGET_IOS)
                shellPrintJobSuspended(job, cmd);
#endif
                shellUpdateStatus(status);
                shellFreeCommand(cmd);
                shellScalarCacheInvalidate();
                return makeVoid();
            }
        } else {
        pid_t job_pids[1];
        job_pids[0] = child;
        gShellRuntime.last_bg_pid = child;
        ShellJob *job = shellRegisterJob(child, job_pids, 1, cmd);
        if (job) {
            shellJobMarkRunning(job);
            job->last_status = 0;
        }
    #if defined(PSCAL_TARGET_IOS)
        shellPrintJobStarted(job, child);
    #endif
        status = 0;
        }
        shellUpdateStatus(status);
        shellRuntimeProcessPendingSignals();
    }

    shellFreeCommand(cmd);
    shellScalarCacheInvalidate();
    return makeVoid();
}
