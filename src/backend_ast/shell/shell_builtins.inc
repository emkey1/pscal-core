/* Auto-generated include: shell builtins exposed to the VM. Included from shell.c. */
#include "shell/runner.h"
#if defined(__GLIBC__)
#include <stdio_ext.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "runtime/vproc/vproc.h"
#include "runtime/vproc/vproc_tree.h"
#include <sys/types.h>
#include <sys/stat.h>
#if defined(PSCAL_TARGET_IOS)
#include "common/path_virtualization.h"
#include "common/path_truncate.h"
#include "smallclue/smallclue.h"
#include "runtime/vproc/vproc.h"
#endif
#if !defined(PSCAL_TAB_TITLE_SUPPORT)
#if defined(PSCAL_TARGET_IOS) || (defined(TARGET_OS_MACCATALYST) && TARGET_OS_MACCATALYST)
#define PSCAL_TAB_TITLE_SUPPORT 1
#endif
#endif
#if defined(PSCAL_TAB_TITLE_SUPPORT)
extern int PSCALRuntimeSetTabTitle(const char *title);
extern int PSCALRuntimeSetTabStartupCommand(const char *command);
#endif
#include <pthread.h>
#include <sys/ioctl.h>
#include <setjmp.h>
static bool shellToolTcgetattr(int fd, struct termios *termios) {
    if (!termios) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    if (vprocIoctlShim(fd, TIOCGETA, termios) == 0) {
        return true;
    }
#ifdef TCGETS
    if (vprocIoctlShim(fd, TCGETS, termios) == 0) {
        return true;
    }
#endif
#endif
    return tcgetattr(fd, termios) == 0;
}

/* Not a flush: a line typed while a tool ran is still waiting in the terminal, on its way
 * to the next prompt. */
#define SHELL_TOOL_RESTORE_ACTION TCSADRAIN

static bool shellToolTcsetattr(int fd, int action, const struct termios *termios) {
    if (!termios) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    /* Not an int: TIOCSETAW has its top bit set, and would arrive sign-extended. */
    unsigned long cmd = 0;
#if defined(TCSETS)
    switch (action) {
        case TCSANOW:
            cmd = TCSETS;
            break;
        case TCSADRAIN:
            cmd = TCSETSW;
            break;
        case TCSAFLUSH:
            cmd = TCSETSF;
            break;
        default:
            cmd = 0;
            break;
    }
#elif defined(TIOCSETA)
    switch (action) {
        case TCSANOW:
            cmd = TIOCSETA;
            break;
        case TCSADRAIN:
            cmd = TIOCSETAW;
            break;
        case TCSAFLUSH:
            cmd = TIOCSETAF;
            break;
        default:
            cmd = 0;
            break;
    }
#endif
    if (cmd != 0 && vprocIoctlShim(fd, cmd, (void *)termios) == 0) {
        return true;
    }
#endif
    return tcsetattr(fd, action, termios) == 0;
}

#ifdef PSCAL_TARGET_IOS
#include <signal.h>
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "common/pascal_state.h"
#include "clike/state.h"
#include "rea/state.h"
#include "rea/frontend_hooks.h"
extern bool pscalRuntimeConsumeSigint(void);
extern bool pscalRuntimeConsumeSigtstp(void);
extern char **environ;
extern void pscalShowGwinMessage(const char *message);
extern void pscalEditorDump(void);

typedef struct {
    struct sigaction sigint;
    struct sigaction sigquit;
    struct sigaction sigterm;
    struct sigaction sigtstp;
    struct sigaction sigabrt;
    struct sigaction sigsegv;
    sigset_t sigmask;
    bool have_sigint;
    bool have_sigquit;
    bool have_sigterm;
    bool have_sigtstp;
    bool have_sigabrt;
    bool have_sigsegv;
    bool have_sigmask;
} ShellSignalBackup;

typedef struct {
    int saved_fds[3];
    bool saved_valid[3];
} ShellStdFdBackup;

static void shellSaveSignalState(ShellSignalBackup *backup) {
    if (!backup) {
        return;
    }
    memset(backup, 0, sizeof(*backup));
    backup->have_sigint = (sigaction(SIGINT, NULL, &backup->sigint) == 0);
    backup->have_sigquit = (sigaction(SIGQUIT, NULL, &backup->sigquit) == 0);
    backup->have_sigterm = (sigaction(SIGTERM, NULL, &backup->sigterm) == 0);
    backup->have_sigtstp = (sigaction(SIGTSTP, NULL, &backup->sigtstp) == 0);
    backup->have_sigabrt = (sigaction(SIGABRT, NULL, &backup->sigabrt) == 0);
    backup->have_sigsegv = (sigaction(SIGSEGV, NULL, &backup->sigsegv) == 0);
    backup->have_sigmask = (pthread_sigmask(SIG_SETMASK, NULL, &backup->sigmask) == 0);
}

static void shellRestoreSignalState(const ShellSignalBackup *backup) {
    if (!backup) {
        return;
    }
    if (backup->have_sigint) {
        sigaction(SIGINT, &backup->sigint, NULL);
    }
    if (backup->have_sigquit) {
        sigaction(SIGQUIT, &backup->sigquit, NULL);
    }
    if (backup->have_sigterm) {
        sigaction(SIGTERM, &backup->sigterm, NULL);
    }
    if (backup->have_sigtstp) {
        sigaction(SIGTSTP, &backup->sigtstp, NULL);
    }
    if (backup->have_sigabrt) {
        sigaction(SIGABRT, &backup->sigabrt, NULL);
    }
    if (backup->have_sigsegv) {
        sigaction(SIGSEGV, &backup->sigsegv, NULL);
    }
    if (backup->have_sigmask) {
        pthread_sigmask(SIG_SETMASK, &backup->sigmask, NULL);
    }
}

static void shellSaveStandardFds(ShellStdFdBackup *backup) {
    if (!backup) {
        return;
    }
    memset(backup, 0, sizeof(*backup));
    int targets[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    for (int i = 0; i < 3; ++i) {
        int fd = dup(targets[i]);
        if (fd >= 0) {
            shellSlotFcntl(fd, F_SETFD, FD_CLOEXEC);
            backup->saved_fds[i] = fd;
            backup->saved_valid[i] = true;
        } else {
            backup->saved_fds[i] = -1;
            backup->saved_valid[i] = false;
        }
    }
}

static void shellRestoreStandardFds(ShellStdFdBackup *backup, bool dbg) {
    if (!backup) {
        return;
    }
    int targets[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    for (int i = 0; i < 3; ++i) {
        if (!backup->saved_valid[i] || backup->saved_fds[i] < 0) {
            continue;
        }
        if (dup2(backup->saved_fds[i], targets[i]) >= 0) {
            shellResetStandardStream(targets[i]);
        } else if (dbg) {
            fprintf(stderr,
                    "[tool-inline] restore fd=%d failed err=%s\n",
                    targets[i],
                    strerror(errno));
        }
        close(backup->saved_fds[i]);
        backup->saved_fds[i] = -1;
        backup->saved_valid[i] = false;
    }
}

static void shellInvalidateFrontendState(FrontendKind kind) {
    switch (kind) {
        case FRONTEND_KIND_PASCAL:
            pascalInvalidateGlobalState();
            break;
        case FRONTEND_KIND_CLIKE:
            clikeInvalidateGlobalState();
            break;
        case FRONTEND_KIND_REA:
        case FRONTEND_KIND_AETHER:
            /* Through the hooks: this thread's Aether run has registries of its own. */
            reaFrontendInvalidateGlobalState();
            break;
        case FRONTEND_KIND_SHELL:
            pascalInvalidateGlobalState();
            break;
        default:
            pascalInvalidateGlobalState();
            break;
    }
}
#endif

#if defined(PSCAL_TARGET_IOS)
#undef stdin
#undef stdout
#undef stderr
#define stdin shellVprocStdin()
#define stdout shellVprocStdout()
#define stderr shellVprocStderr()
#define printf(...) fprintf(stdout, __VA_ARGS__)
#define puts(text) shellVprocPuts((text))
#define putchar(ch) fputc((ch), stdout)
#define perror(label) shellVprocPerror((label))
#endif

/* A job, or a stage of more than one. */
static bool shellMetadataRunsApart(Value meta) {
    if (VALUE_TYPE(meta) != TYPE_STRING || !AS_STRING(meta)) {
        return false;
    }
    return strncmp(AS_STRING(meta), "bg=1;", 5) == 0 ||
           (gShellRuntime.pipeline.active && gShellRuntime.pipeline.stage_count > 1);
}

Value vmBuiltinShellExec(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    /* After break or continue the rest of the body goes by without running, its words
     * included: `x=$(cmd)` must not run cmd. */
    if (shellLoopSkipActive()) {
        goto cleanup;
    }
    shellCollectJobs();
    bool exit_before_trap = gShellExitRequested;
    shellRuntimeRunDebugTrap();
    /* Its `exit` comes before the command, which doesn't run. */
    if (gShellExitRequested && !exit_before_trap) {
        goto cleanup;
    }
    ShellCommand cmd;
    /* A job's or stage's words failing is its own failure: under bash it has forked by then, so it
     * reports, ends with status 1 and the shell goes on. The rest of the pipeline runs, and the
     * shell waits for it: ended here, it would pull the pipeline from under the stages started. */
    gShellExpandingJobWords = arg_count > 0 && shellMetadataRunsApart(args[0]);
    bool built = shellBuildCommand(vm, arg_count, args, &cmd);
    bool job_failed = !built && gShellExpandingJobWords && gShellExpansionFailed;
    gShellExpandingJobWords = false;
    if (job_failed) {
        built = shellBuildCommand(vm, 1, args, &cmd);
        cmd.ran_command_substitution = true;
    }
    if (!built) {
        goto cleanup;
    }
    char *last_word = (cmd.argc > 0 && cmd.argv && cmd.argv[cmd.argc - 1]) ? strdup(cmd.argv[cmd.argc - 1]) : NULL;
    result = shellExecuteCommand(vm, &cmd);
    if (last_word) {
        free(gShellLastWord);
        gShellLastWord = last_word;
    }
    /* A pipeline has left its own stages there. Anything else is a list of one. */
    if (gShellPipeStatusFresh) {
        gShellPipeStatusFresh = false;
    } else if (!gShellRuntime.pipeline.active) {
        gShellPipeStatus[0] = gShellRuntime.last_status;
        gShellPipeStatusCount = 1;
    }
    shellProcessSubstitutionsFinish(vm);
#if defined(PSCAL_TARGET_IOS)
    /* SIGPIPE cannot end one thread, and a builtin writing here is the subshell itself. */
    if (gShellRuntime.in_subshell && vprocConsumeClosedPipe()) {
        shellUpdateStatus(128 + SIGPIPE);
        shellRuntimeRequestExit();
        if (vm) {
            vm->exit_requested = true;
        }
    }
#endif
    /* A trap runs as soon as the command is done, in the function it was in: `kill -USR1 $$`
     * may be the last thing that function does, or the script. */
    if (!gShellRuntime.pipeline.active) {
        shellRuntimeProcessPendingSignals();
    }

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

/* Starts a compound command as one command: see shellRunBody. */
Value vmBuiltinShellExecBody(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    if (arg_count != 2 || VALUE_TYPE(args[0]) != TYPE_POINTER || !AS_POINTER(args[0])) {
        runtimeError(vm, "shell exec: expected a compiled body and metadata");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (shellLoopSkipActive()) {
        goto cleanup;
    }
    shellCollectJobs();
    ShellCommand cmd;
    if (!shellBuildCommand(vm, 1, &args[1], &cmd)) {
        shellUpdateStatus(1);
        goto cleanup;
    }
    char *name = strdup("(subshell)");
    if (!shellCommandAppendArgOwned(&cmd, name)) {
        runtimeError(vm, "shell exec: out of memory");
        shellUpdateStatus(1);
        shellFreeCommand(&cmd);
        goto cleanup;
    }
    cmd.body = (ShellCompiledFunction *)AS_POINTER(args[0]);
#if defined(PSCAL_TARGET_IOS)
    cmd.body_state = shellBodyStateCapture();
    if (!cmd.body_state) {
        runtimeError(vm, "shell exec: out of memory");
        shellUpdateStatus(1);
        shellFreeCommand(&cmd);
        goto cleanup;
    }
#endif
    result = shellExecuteCommand(vm, &cmd);
    if (gShellPipeStatusFresh) {
        gShellPipeStatusFresh = false;
    } else if (!gShellRuntime.pipeline.active) {
        gShellPipeStatus[0] = gShellRuntime.last_status;
        gShellPipeStatusCount = 1;
    }
    if (!gShellRuntime.pipeline.active) {
        shellRuntimeProcessPendingSignals();
    }

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellPipeline(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    char *merge_pattern = NULL;
    if (arg_count != 1 || VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0])) {
        runtimeError(vm, "shell pipeline: expected metadata string");
        goto cleanup;
    }
    if (shellLoopSkipActive()) {
        goto cleanup;
    }
    const char *meta = AS_STRING(args[0]);
    size_t stages = 0;
    size_t debug_traps = 0;
    bool negated = false;
    bool async = false;
    char *copy = strdup(meta);
    if (!copy) {
        runtimeError(vm, "shell pipeline: out of memory");
        goto cleanup;
    }
    char *cursor = copy;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) *next = '\0';
        char *eq = strchr(cursor, '=');
        if (eq) {
            *eq = '\0';
            const char *key = cursor;
            const char *value = eq + 1;
            if (strcmp(key, "stages") == 0) {
                stages = (size_t)strtoul(value, NULL, 10);
            } else if (strcmp(key, "negated") == 0) {
                shellParseBool(value, &negated);
            } else if (strcmp(key, "async") == 0) {
                shellParseBool(value, &async);
            } else if (strcmp(key, "merge") == 0) {
                free(merge_pattern);
                merge_pattern = strdup(value ? value : "");
            } else if (strcmp(key, "dbg") == 0) {
                debug_traps = (size_t)strtoul(value, NULL, 10);
            }
        }
        if (!next) break;
        cursor = next + 1;
    }
        free(copy);
    /* What the one body that a background pipeline runs in on iOS holds: its copy of the trap is
     * not the shell's. */
    for (size_t i = 0; i < debug_traps; ++i) {
        shellRuntimeRunDebugTrap();
    }
    bool skip_pipeline = false;
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (ctx->active && stages == 1 && !negated) {
        bool has_merge = false;
        if (merge_pattern) {
            for (const char *p = merge_pattern; *p; ++p) {
                if (*p != '0') {
                    has_merge = true;
                    break;
                }
            }
        }
        if (!has_merge) {
            skip_pipeline = true;
        }
    }

    if (!skip_pipeline) {
        if (stages == 0) {
            runtimeError(vm, "shell pipeline: invalid stage count");
            free(merge_pattern);
            goto cleanup;
        }
        if (!shellEnsurePipeline(stages, negated)) {
            runtimeError(vm, "shell pipeline: unable to allocate context");
            free(merge_pattern);
            goto cleanup;
        }
        ctx->async = async;

        if (merge_pattern) {
            size_t pattern_len = strlen(merge_pattern);
            for (size_t i = 0; i < stages; ++i) {
                bool merge = false;
                if (i < pattern_len) {
                    merge = (merge_pattern[i] == '1');
                }
                if (ctx->merge_stderr && i < stages) {
                    ctx->merge_stderr[i] = merge;
                }
            }
        }
    }

    free(merge_pattern);
    merge_pattern = NULL;

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellArithmetic(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();

    bool dispatch_mode = (arg_count >= 2 && VALUE_TYPE(args[0]) == TYPE_STRING);

    if (dispatch_mode) {
        shellCollectJobs();
        ShellCommand cmd;
        memset(&cmd, 0, sizeof(cmd));

        const char *meta = AS_STRING(args[0]);
        if (!meta) {
            runtimeError(vm, "((: expected metadata");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }

        shellParseMetadata(meta, &cmd);

        if (VALUE_TYPE(args[1]) != TYPE_STRING) {
            runtimeError(vm, "((: expected expression");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }

        const char *expression = AS_STRING(args[1]) ? AS_STRING(args[1]) : "";

        char *name_copy = strdup("__shell_arithmetic");
        if (!name_copy) {
            runtimeError(vm, "((: out of memory");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }
        if (!shellCommandAppendArgOwned(&cmd, name_copy)) {
            runtimeError(vm, "((: unable to build command");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }

        char *expr_copy = strdup(expression);
        if (!expr_copy) {
            runtimeError(vm, "((: out of memory");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }
        if (!shellCommandAppendArgOwned(&cmd, expr_copy)) {
            runtimeError(vm, "((: unable to build command");
            shellUpdateStatus(1);
            goto dispatch_cleanup;
        }

        for (int i = 2; i < arg_count; ++i) {
            Value entry = args[i];
            if (VALUE_TYPE(entry) != TYPE_STRING || !AS_STRING(entry)) {
                runtimeError(vm, "((: invalid redirection");
                shellUpdateStatus(1);
                goto dispatch_cleanup;
            }
            if (!shellAddRedirection(&cmd, AS_STRING(entry))) {
                runtimeError(vm, "((: invalid redirection");
                shellUpdateStatus(1);
                goto dispatch_cleanup;
            }
        }

        result = shellExecuteCommand(vm, &cmd);
        goto cleanup;

dispatch_cleanup:
        shellFreeCommand(&cmd);
        goto cleanup;
    }

    if (arg_count < 1) {
        runtimeError(vm, "((: expected expression");
        shellUpdateStatus(1);
        goto cleanup;
    }

    if (VALUE_TYPE(args[0]) != TYPE_STRING) {
        runtimeError(vm, "((: expected expression");
        shellUpdateStatus(1);
        goto cleanup;
    }

    const char *expression = AS_STRING(args[0]) ? AS_STRING(args[0]) : "";

    long long expr_value = 0;
    if (!shellArithmeticEvaluate(expression, &expr_value, "((")) {
        shellMarkArithmeticError();
        shellUpdateStatus(1);
        goto cleanup;
    }

    shellUpdateStatus(expr_value != 0 ? 0 : 1);

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellAnd(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    if (gShellRuntime.last_status != 0) {
        shellUpdateStatus(gShellRuntime.last_status);
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellOr(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    if (gShellRuntime.last_status == 0) {
        shellUpdateStatus(0);
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

typedef struct {
    ShellExecRedirBackup *backups;
    size_t backup_count;
    bool failed;
} ShellSubshellFrame;

/* Per thread: on iOS a pipeline stage or a nested exsh runs a VM of its own on a
 * worker thread, and these frames restore that thread's descriptors. */
static _Thread_local ShellSubshellFrame *gShellSubshellStack = NULL;
static _Thread_local size_t gShellSubshellStackSize = 0;
static _Thread_local size_t gShellSubshellStackCapacity = 0;

static bool shellSubshellStackPush(ShellExecRedirBackup *backups, size_t backup_count, bool failed) {
    if (gShellSubshellStackSize >= gShellSubshellStackCapacity) {
        size_t new_cap = (gShellSubshellStackCapacity == 0) ? 4 : (gShellSubshellStackCapacity * 2);
        ShellSubshellFrame *resized = (ShellSubshellFrame *)realloc(gShellSubshellStack, new_cap * sizeof(ShellSubshellFrame));
        if (!resized) {
            return false;
        }
        gShellSubshellStack = resized;
        gShellSubshellStackCapacity = new_cap;
    }
    ShellSubshellFrame *slot = &gShellSubshellStack[gShellSubshellStackSize++];
    slot->backups = backups;
    slot->backup_count = backup_count;
    slot->failed = failed;
    return true;
}

static ShellSubshellFrame shellSubshellStackPop(void) {
    ShellSubshellFrame frame = {0};
    if (gShellSubshellStackSize == 0) {
        return frame;
    }
    frame = gShellSubshellStack[--gShellSubshellStackSize];
    if (gShellSubshellStackSize == 0) {
        free(gShellSubshellStack);
        gShellSubshellStack = NULL;
        gShellSubshellStackCapacity = 0;
    }
    return frame;
}

static void shellSubshellFrameClose(ShellSubshellFrame *frame) {
    if (frame->backups) {
        shellFlushStandardStreams();
        shellRestoreExecRedirections(frame->backups, frame->backup_count);
        shellFreeExecRedirBackups(frame->backups, frame->backup_count);
        frame->backups = NULL;
    }
}

size_t shellRuntimeRedirectScopeDepth(void) {
    return gShellSubshellStackSize;
}

/* return and exit stop a chunk before it reaches a scope's mode=leave, so the
 * caller of the chunk closes what was left open. Each restores what the one inside it
 * replaced, so a loop started inside a scope ends first. */
void shellRuntimeUnwindRedirectScopes(size_t depth) {
    while (gShellSubshellStackSize > depth) {
        while (!gShellExitRequested && gShellLoopStackSize > 0 &&
               shellLoopTop()->scope_depth >= gShellSubshellStackSize) {
            shellLoopPopFrameFlushed();
        }
        ShellSubshellFrame frame = shellSubshellStackPop();
        shellSubshellFrameClose(&frame);
    }
}

Value vmHostShellScopeIsReady(VM *vm) {
    (void)vm;
    return makeBoolean(gShellSubshellStackSize > 0 &&
                       !gShellSubshellStack[gShellSubshellStackSize - 1].failed);
}

/* A body's own redirections are final: nothing runs after them, not even its EXIT trap,
 * so what they replaced is let go of now. A saved copy would keep the caller's stdout open
 * for as long as the body runs, and whoever reads that pipe would wait for a background job
 * that sent its output elsewhere. A host runs a body in a forked child. On iOS the redirections
 * of a worker reach only the slots of its own vproc, which end with it; other threads
 * also change the process's descriptors, and those must be put back. */
static void shellDropFinalRedirBackups(const char *meta, ShellExecRedirBackup **backups, size_t *count) {
    if (!meta || !strstr(meta, "final=1")) {
        return;
    }
#if defined(PSCAL_TARGET_IOS)
    if (!gShellBackgroundWorkerActive) {
        return;
    }
#endif
    shellFreeExecRedirBackups(*backups, *count);
    *backups = NULL;
    *count = 0;
}

/* The redirections of a compound command, around its whole body. A redirection
 * that fails reports the error and fails the command, without running the body
 * and without ending the script. */
Value vmBuiltinShellSubshell(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    const char *meta = (arg_count > 0 && VALUE_TYPE(args[0]) == TYPE_STRING) ? AS_STRING(args[0]) : "";
    bool leave = (meta && strstr(meta, "mode=leave") != NULL);
    if (leave) {
        ShellSubshellFrame frame = shellSubshellStackPop();
        shellSubshellFrameClose(&frame);
        shellRestoreCurrentVm(previous_vm);
        return makeVoid();
    }

    ShellCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    bool ok = true;
    ShellExecRedirBackup *backups = NULL;
    size_t backup_count = 0;
    bool skipping = shellLoopSkipActive();
    for (int i = 1; i < arg_count && !skipping; ++i) {
        Value entry = args[i];
        if (VALUE_TYPE(entry) != TYPE_STRING || !AS_STRING(entry)) {
            ok = false;
            break;
        }
        if (!shellAddRedirection(&cmd, AS_STRING(entry))) {
            ok = false;
            break;
        }
    }
    if (ok && cmd.redir_count > 0) {
        shellFlushStandardStreams();
        if (!shellApplyExecRedirections(&cmd, &backups, &backup_count)) {
            ok = false;
        } else {
            shellDropFinalRedirBackups(meta, &backups, &backup_count);
        }
    }
    if (cmd.redirs) {
        shellFreeRedirections(&cmd);
        cmd.redirs = NULL;
        cmd.redir_count = 0;
    }
    if (!shellSubshellStackPush(backups, backup_count, !ok)) {
        if (backups) {
            shellRestoreExecRedirections(backups, backup_count);
            shellFreeExecRedirBackups(backups, backup_count);
        }
        runtimeError(vm, "shell: out of memory");
        ok = false;
    }
    if (!ok) {
        shellUpdateStatus(1);
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellLoop(VM *vm, int arg_count, Value *args) {
    shellNoteChunkLine(vm);
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    const char *meta = NULL;
    if (arg_count > 0 && VALUE_TYPE(args[0]) == TYPE_STRING && AS_STRING(args[0])) {
        meta = AS_STRING(args[0]);
    }

    ShellLoopKind kind = SHELL_LOOP_KIND_WHILE;
    bool until_flag = false;
    size_t redir_count = 0;
    int condition_kind_value = 0;
    size_t condition_payload_count = 0;
    int body_kind_value = 0;
    size_t body_payload_count = 0;
    if (meta && *meta) {
        char *copy = strdup(meta);
        if (copy) {
            /* strtok keeps its place per process, and on iOS another thread can start a loop now. */
            char *save = NULL;
            for (char *token = strtok_r(copy, ";", &save); token; token = strtok_r(NULL, ";", &save)) {
                char *eq = strchr(token, '=');
                if (eq) {
                    *eq = '\0';
                    const char *key = token;
                    const char *value = eq + 1;
                    if (strcmp(key, "mode") == 0) {
                        if (strcasecmp(value, "for") == 0) {
                            kind = SHELL_LOOP_KIND_FOR;
                        } else if (strcasecmp(value, "cfor") == 0) {
                            kind = SHELL_LOOP_KIND_CFOR;
                        } else if (strcasecmp(value, "until") == 0) {
                            kind = SHELL_LOOP_KIND_UNTIL;
                        } else if (strcasecmp(value, "while") == 0) {
                            kind = SHELL_LOOP_KIND_WHILE;
                        }
                    } else if (strcmp(key, "until") == 0) {
                        until_flag = (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0);
                    } else if (strcmp(key, "redirs") == 0) {
                        char *endptr = NULL;
                        unsigned long parsed = strtoul(value ? value : "0", &endptr, 10);
                        if (endptr && *endptr == '\0') {
                            redir_count = (size_t)parsed;
                        }
                    } else if (strcmp(key, "condkind") == 0) {
                        condition_kind_value = (int)strtol(value ? value : "0", NULL, 10);
                    } else if (strcmp(key, "condwords") == 0) {
                        condition_payload_count = (size_t)strtoul(value ? value : "0", NULL, 10);
                    } else if (strcmp(key, "bodykind") == 0) {
                        body_kind_value = (int)strtol(value ? value : "0", NULL, 10);
                    } else if (strcmp(key, "bodywords") == 0) {
                        body_payload_count = (size_t)strtoul(value ? value : "0", NULL, 10);
                    }
                }
            }
            free(copy);
        }
    }
    if (kind != SHELL_LOOP_KIND_FOR && until_flag) {
        kind = SHELL_LOOP_KIND_UNTIL;
    }

    bool parent_skip = shellLoopSkipActive();
    ShellLoopFrame *frame = shellLoopPushFrame(kind);
    if (!frame) {
        runtimeError(vm, "shell loop: out of memory");
        shellRestoreCurrentVm(previous_vm);
        shellUpdateStatus(1);
        return result;
    }

    frame->scope_depth = gShellSubshellStackSize;
    frame->skip_body = parent_skip;
    frame->keep_status = parent_skip;
    frame->break_pending = false;
    frame->continue_pending = false;
    if (condition_kind_value < SHELL_LOOP_CONDITION_NONE ||
        condition_kind_value > SHELL_LOOP_CONDITION_ARITH) {
        condition_kind_value = SHELL_LOOP_CONDITION_NONE;
    }
    frame->cond_kind = (ShellLoopConditionKind)condition_kind_value;
    if ((frame->cond_kind == SHELL_LOOP_CONDITION_TEST ||
         frame->cond_kind == SHELL_LOOP_CONDITION_BRACKET) &&
        condition_payload_count == 0) {
        frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
    }
    if (frame->cond_kind == SHELL_LOOP_CONDITION_ARITH && condition_payload_count == 0) {
        frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
    }

    if (body_kind_value < SHELL_LOOP_BODY_KIND_NONE ||
        body_kind_value > SHELL_LOOP_BODY_KIND_BRACKET_ARITH) {
        body_kind_value = SHELL_LOOP_BODY_KIND_NONE;
    }
    frame->body_kind = (ShellLoopBodyKind)body_kind_value;
    size_t body_expression_count = 0;
    switch (frame->body_kind) {
        case SHELL_LOOP_BODY_KIND_ARITH:
            body_expression_count = 1;
            break;
        case SHELL_LOOP_BODY_KIND_TEST:
        case SHELL_LOOP_BODY_KIND_BRACKET:
            if (body_payload_count == 0) {
                frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
            }
            break;
        case SHELL_LOOP_BODY_KIND_TEST_ARITH:
        case SHELL_LOOP_BODY_KIND_BRACKET_ARITH:
            if (body_payload_count == 0) {
                frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
            } else {
                body_expression_count = 1;
            }
            break;
        default:
            break;
    }
    if (frame->body_kind == SHELL_LOOP_BODY_KIND_NONE) {
        body_payload_count = 0;
        body_expression_count = 0;
    }

    ShellCommand redir_cmd;
    memset(&redir_cmd, 0, sizeof(redir_cmd));
    ShellExecRedirBackup *redir_backups = NULL;
    size_t redir_backup_count = 0;

    bool ok = true;

    int payload_total = (arg_count > 0) ? arg_count - 1 : 0;
    size_t meta_payload = condition_payload_count + body_payload_count + body_expression_count;
    if ((size_t)payload_total < redir_count + meta_payload) {
        runtimeError(vm, "shell loop: redirection metadata mismatch");
        ok = false;
        if ((size_t)payload_total >= meta_payload) {
            redir_count = (size_t)payload_total - meta_payload;
        } else {
            redir_count = 0;
        }
    }
    int payload_without_redirs = payload_total - (int)redir_count - (int)meta_payload;
    if (payload_without_redirs < 0) {
        payload_without_redirs = 0;
    }
    int payload_cursor = 1;
    int condition_start_index = payload_cursor;
    payload_cursor += (int)condition_payload_count;
    int body_start_index = payload_cursor;
    payload_cursor += (int)body_payload_count;
    int body_expression_index = (body_expression_count > 0) ? payload_cursor : -1;
    payload_cursor += (int)body_expression_count;
    int redir_start_index = arg_count - (int)redir_count;
    if (redir_start_index < payload_cursor) {
        redir_start_index = payload_cursor;
    }

    if (kind == SHELL_LOOP_KIND_FOR) {
        bool bad_for_name = false;
        if (payload_without_redirs < 1 || payload_cursor >= redir_start_index ||
            VALUE_TYPE(args[payload_cursor]) != TYPE_STRING || !AS_STRING(args[payload_cursor])) {
            runtimeError(vm, "shell loop: expected iterator name");
            ok = false;
        } else {
            const char *spec = AS_STRING(args[payload_cursor]);
            const char *text = spec;
            const char *word_meta = NULL;
            size_t word_meta_len = 0;
            uint8_t word_flags = 0;
            if (!shellDecodeWordSpec(spec, &text, &word_flags, &word_meta, &word_meta_len)) {
                text = spec ? spec : "";
            }
            frame->for_variable = strdup(text ? text : "");
            if (!frame->for_variable) {
                ok = false;
            } else if (!parent_skip && !shellIsValidEnvName(frame->for_variable)) {
                /* The loop doesn't run and the script goes on, with status 1 and whatever `set -e`
                 * says. The loop ends at its first advance, as one started under a skip does, and
                 * leaves the status alone. */
                if (vm->chunk && vm->chunk->lines && vm->lastInstruction) {
                    gShellCurrentCommandLine = vm->chunk->lines[vm->lastInstruction - vm->chunk->code];
                }
                shellReportRecoverableError(vm, true, "`%s': not a valid identifier", frame->for_variable);
                shellRuntimeEnterCondition();
                shellUpdateStatus(1);
                shellRuntimeLeaveCondition();
                bad_for_name = true;
                frame->keep_status = true;
            }
        }

        payload_cursor++;

        size_t value_start_index = (size_t)payload_cursor;
        size_t value_count = 0;
        if (redir_start_index > (int)value_start_index) {
            value_count = (size_t)(redir_start_index - (int)value_start_index);
        }
        size_t values_capacity = 0;
        if (ok && value_count > 0 && !parent_skip && !bad_for_name) {
            for (size_t offset = 0; offset < value_count; ++offset) {
                Value *val = &args[value_start_index + offset];
                if (VALUE_TYPE(*val) != TYPE_STRING || !AS_STRING(*val)) {
                    char *empty = strdup("");
                    if (!empty || !shellLoopFrameAppendValue(frame, &values_capacity, empty)) {
                        free(empty);
                        ok = false;
                        break;
                    }
                    continue;
                }
                const char *spec = AS_STRING(*val);
                const char *text = spec;
                const char *word_meta = NULL;
                size_t word_meta_len = 0;
                uint8_t word_flags = 0;
                if (!shellDecodeWordSpec(spec, &text, &word_flags, &word_meta, &word_meta_len)) {
                    text = spec ? spec : "";
                    word_flags = 0;
                }
                ShellStringArray fields = (ShellStringArray){0};
                ok = shellExpandWordFields(text, word_flags, word_meta, word_meta_len, true, &fields);
                for (size_t f = 0; f < fields.count && ok; ++f) {
                    ok = shellLoopFrameAppendValue(frame, &values_capacity, fields.items[f]);
                    fields.items[f] = NULL;
                }
                shellStringArrayFree(&fields);
                if (!ok || gShellExpansionFailed) {
                    ok = false;
                    break;
                }
            }
        }

        if (!ok || !frame->for_variable) {
            frame->skip_body = true;
            frame->break_pending = true;
        } else if (frame->for_count == 0) {
            frame->skip_body = true;
            frame->for_active = false;
        } else {
            if (!shellAssignLoopVariable(frame->for_variable, frame->for_values[0])) {
                runtimeError(vm, "shell loop: failed to assign '%s'", frame->for_variable);
                frame->skip_body = true;
                frame->break_pending = true;
                ok = false;
            } else {
                frame->for_index = 1;
                frame->for_active = true;
            }
        }
    } else if (kind == SHELL_LOOP_KIND_CFOR) {
        if (payload_without_redirs < 3) {
            runtimeError(vm, "shell loop: expected initializer, condition, update");
            ok = false;
        } else {
            if (payload_cursor + 2 >= redir_start_index) {
                runtimeError(vm, "shell loop: expected initializer, condition, update");
                ok = false;
            } else {
                const Value *init_val = &args[payload_cursor];
                const Value *cond_val = &args[payload_cursor + 1];
                const Value *update_val = &args[payload_cursor + 2];
                const char *init_text = (VALUE_TYPE(*init_val) == TYPE_STRING && AS_STRING(*init_val)) ? AS_STRING(*init_val) : "";
                const char *cond_text = (VALUE_TYPE(*cond_val) == TYPE_STRING && AS_STRING(*cond_val)) ? AS_STRING(*cond_val) : "";
                const char *update_text = (VALUE_TYPE(*update_val) == TYPE_STRING && AS_STRING(*update_val)) ? AS_STRING(*update_val) : "";
                frame->cfor_init = strdup(init_text);
                frame->cfor_condition = strdup(cond_text);
                frame->cfor_update = strdup(update_text);
                if (!frame->cfor_init || !frame->cfor_condition || !frame->cfor_update) {
                    ok = false;
                } else if (!parent_skip && !shellLoopExecuteCForInitializer(frame)) {
                    ok = false;
                }
            }
        }
    } else {
        if (ok && condition_payload_count > 0) {
            if ((size_t)arg_count < condition_start_index + condition_payload_count + redir_count) {
                ok = false;
                frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
            } else {
                if (frame->cond_kind == SHELL_LOOP_CONDITION_ARITH) {
                    const Value *entry = &args[condition_start_index];
                    const char *expr = (VALUE_TYPE(*entry) == TYPE_STRING && AS_STRING(*entry)) ? AS_STRING(*entry) : "";
                    frame->cond_arith_expression = strdup(expr);
                    if (!frame->cond_arith_expression) {
                        ok = false;
                        frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
                    }
                } else if (frame->cond_kind == SHELL_LOOP_CONDITION_TEST ||
                           frame->cond_kind == SHELL_LOOP_CONDITION_BRACKET) {
                    frame->cond_words = (char **)calloc(condition_payload_count, sizeof(char *));
                    if (!frame->cond_words) {
                        ok = false;
                        frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
                    } else {
                        for (size_t i = 0; i < condition_payload_count; ++i) {
                            const Value *entry = &args[condition_start_index + i];
                            const char *spec = (VALUE_TYPE(*entry) == TYPE_STRING && AS_STRING(*entry)) ? AS_STRING(*entry) : "";
                            frame->cond_words[i] = strdup(spec);
                            if (!frame->cond_words[i]) {
                                ok = false;
                                for (size_t j = 0; j < i; ++j) {
                                    free(frame->cond_words[j]);
                                }
                                free(frame->cond_words);
                                frame->cond_words = NULL;
                                frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
                                break;
                            }
                        }
                        if (ok) {
                            frame->cond_word_count = condition_payload_count;
                        }
                    }
                } else {
                    frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
                }
            }
        }
        if (ok) {
            switch (frame->body_kind) {
                case SHELL_LOOP_BODY_KIND_ARITH: {
                    if (body_expression_count == 0 || body_expression_index < 0 || body_expression_index >= redir_start_index) {
                        frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                    } else {
                        const Value *entry = &args[body_expression_index];
                        const char *expr = (VALUE_TYPE(*entry) == TYPE_STRING && AS_STRING(*entry)) ? AS_STRING(*entry) : "";
                        frame->body_arith_expression = strdup(expr);
                        if (!frame->body_arith_expression) {
                            ok = false;
                            frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                        }
                    }
                    break;
                }
                case SHELL_LOOP_BODY_KIND_TEST:
                case SHELL_LOOP_BODY_KIND_BRACKET:
                case SHELL_LOOP_BODY_KIND_TEST_ARITH:
                case SHELL_LOOP_BODY_KIND_BRACKET_ARITH: {
                    if (body_payload_count == 0 || body_start_index + (int)body_payload_count > redir_start_index) {
                        frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                        break;
                    }
                    frame->body_words = (char **)calloc(body_payload_count, sizeof(char *));
                    if (!frame->body_words) {
                        frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                        ok = false;
                        break;
                    }
                    frame->body_word_count = body_payload_count;
                    for (size_t i = 0; i < body_payload_count; ++i) {
                        const Value *entry = &args[body_start_index + (int)i];
                        const char *spec = (VALUE_TYPE(*entry) == TYPE_STRING && AS_STRING(*entry)) ? AS_STRING(*entry) : "";
                        frame->body_words[i] = strdup(spec);
                        if (!frame->body_words[i]) {
                            ok = false;
                            frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                            for (size_t j = 0; j < i; ++j) {
                                free(frame->body_words[j]);
                            }
                            free(frame->body_words);
                            frame->body_words = NULL;
                            frame->body_word_count = 0;
                            break;
                        }
                    }
                    if (!ok) {
                        break;
                    }
                    if (frame->body_kind == SHELL_LOOP_BODY_KIND_TEST_ARITH ||
                        frame->body_kind == SHELL_LOOP_BODY_KIND_BRACKET_ARITH) {
                        if (body_expression_count == 0 || body_expression_index < 0 || body_expression_index >= redir_start_index) {
                            frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                            break;
                        }
                        const Value *entry = &args[body_expression_index];
                        const char *expr = (VALUE_TYPE(*entry) == TYPE_STRING && AS_STRING(*entry)) ? AS_STRING(*entry) : "";
                        frame->body_arith_expression = strdup(expr);
                        if (!frame->body_arith_expression) {
                            ok = false;
                            frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                        }
                    }
                    break;
                }
                case SHELL_LOOP_BODY_KIND_COLON:
                case SHELL_LOOP_BODY_KIND_TRUE:
                case SHELL_LOOP_BODY_KIND_FALSE:
                case SHELL_LOOP_BODY_KIND_NONE:
                default:
                    if (body_payload_count != 0 || body_expression_count != 0) {
                        frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
                    }
                    break;
            }
        } else {
            frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
        }
        if (frame->body_kind == SHELL_LOOP_BODY_KIND_NONE) {
            body_payload_count = 0;
            body_expression_count = 0;
        }
        if (payload_without_redirs > 0) {
            runtimeError(vm, "shell loop: unexpected arguments");
            ok = false;
        }
    }

    if (ok && redir_count > 0 && !parent_skip) {
        for (size_t i = 0; i < redir_count; ++i) {
            int arg_index = redir_start_index + (int)i;
            if (arg_index < 0 || arg_index >= arg_count) {
                runtimeError(vm, "shell loop: missing redirection argument");
                ok = false;
                break;
            }
            Value entry = args[arg_index];
            if (VALUE_TYPE(entry) != TYPE_STRING || !AS_STRING(entry)) {
                runtimeError(vm, "shell loop: invalid redirection argument");
                ok = false;
                break;
            }
            if (!shellAddRedirection(&redir_cmd, AS_STRING(entry))) {
                runtimeError(vm, "shell loop: failed to parse redirection");
                ok = false;
                break;
            }
        }
        if (ok && redir_cmd.redir_count > 0) {
            if (!shellApplyExecRedirections(&redir_cmd, &redir_backups, &redir_backup_count)) {
                ok = false;
                frame->skip_body = true;
            } else {
                shellDropFinalRedirBackups(meta, &redir_backups, &redir_backup_count);
                frame->redirs_active = true;
                frame->redir_backups = redir_backups;
                frame->redir_backup_count = redir_backup_count;
                frame->redir_backup_capacity = redir_backup_count;
                frame->applied_redirs = redir_cmd.redirs;
                frame->applied_redir_count = redir_cmd.redir_count;
                redir_backups = NULL;
                redir_backup_count = 0;
                redir_cmd.redirs = NULL;
                redir_cmd.redir_count = 0;
            }
        }
    }

    if (!ok) {
        if (frame->kind == SHELL_LOOP_KIND_CFOR) {
            shellLoopCForFailed(frame);
        } else {
            frame->status = 1;
            shellUpdateStatus(1);
        }
    }

    if (redir_backups) {
        shellRestoreExecRedirections(redir_backups, redir_backup_count);
        shellFreeExecRedirBackups(redir_backups, redir_backup_count);
        redir_backups = NULL;
    }
    if (redir_cmd.redirs) {
        shellFreeRedirections(&redir_cmd);
    }

    shellMaybeResetPipeline();
    shellRestoreCurrentVm(previous_vm);
    return result;
}

/* errexit already had its say about the command that set this status. Only a failure it
 * ignored, as in `false && true`, gets here again, and that does not end the script. */
static void shellLoopRestoreStatus(int status) {
    shellRuntimeEnterCondition();
    shellUpdateStatus(status);
    shellRuntimeLeaveCondition();
}

Value vmBuiltinShellLoopEnd(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    ShellLoopFrame *frame = shellLoopTop();
    if (frame) {
        bool propagate_continue = frame->continue_pending;
        bool propagate_break = frame->break_pending;
        if (!frame->keep_status && !gShellExitRequested) {
            shellLoopRestoreStatus(frame->status);
        }
        shellLoopPopFrame();
        if (gShellLoopStackSize == 0) {
            if (propagate_break) {
                gShellRuntime.break_requested = false;
                gShellRuntime.break_requested_levels = 0;
            }
            if (propagate_continue) {
                gShellRuntime.continue_requested = false;
                gShellRuntime.continue_requested_levels = 0;
            }
        }
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellEnterCondition(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellRuntimeEnterCondition();
    return makeVoid();
}

Value vmBuiltinShellLeaveCondition(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellRuntimeLeaveCondition();
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellLeaveConditionPreserve(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)args;
    /* Its one argument says a compound command under `!` has just finished. */
    if (arg_count == 1) {
        shellUpdateStatus(gShellRuntime.last_status == 0 ? 1 : 0);
    }
    shellRuntimeLeaveCondition();
    return makeVoid();
}

Value vmBuiltinShellIf(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    shellMaybeResetPipeline();
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellCase(VM *vm, int arg_count, Value *args) {
    shellNoteChunkLine(vm);
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    if (arg_count != 2 || VALUE_TYPE(args[1]) != TYPE_STRING || !AS_STRING(args[1])) {
        runtimeError(vm, "shell case: expected metadata and subject word");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (shellLoopSkipActive()) {
        /* After break or continue: no expansion runs, and no clause matches. */
        if (shellCaseStackPush("")) {
            shellCaseStackTop()->skipped = true;
            shellCaseStackTop()->incoming_status = gShellRuntime.last_status;
        }
        goto cleanup;
    }
    const char *subject_spec = AS_STRING(args[1]);
    const char *subject_text = subject_spec;
    const char *subject_meta = NULL;
    size_t subject_meta_len = 0;
    uint8_t subject_flags = 0;
    if (!shellDecodeWordSpec(subject_spec, &subject_text, &subject_flags, &subject_meta, &subject_meta_len)) {
        subject_text = subject_spec ? subject_spec : "";
        subject_flags = 0;
    }
    char *expanded_subject = shellExpandWord(subject_text,
                                             subject_flags,
                                             subject_meta,
                                             subject_meta_len,
                                             NULL,
                                             NULL,
                                             NULL);
    if (!expanded_subject) {
        runtimeError(vm, "shell case: out of memory");
        shellUpdateStatus(1);
        goto cleanup;
    }
    shellFlattenArraySeparators(expanded_subject);
    if (gShellExpansionFailed) {
        /* Reported, and the status stays as it says. */
        gShellExpansionFailed = false;
        free(expanded_subject);
        (void)shellCaseStackPush("");
        goto cleanup;
    }
    if (!shellCaseStackPush(expanded_subject)) {
        free(expanded_subject);
        runtimeError(vm, "shell case: out of memory");
        shellUpdateStatus(1);
        goto cleanup;
    }
    free(expanded_subject);
    shellCaseStackTop()->incoming_status = gShellRuntime.last_status;
    shellUpdateStatus(0);

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellCaseClause(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    shellRuntimeEnterCondition();
    if (arg_count < 1 || VALUE_TYPE(args[0]) != TYPE_STRING) {
        runtimeError(vm, "shell case clause: expected metadata");
        shellUpdateStatus(1);
        goto cleanup;
    }
    ShellCaseContext *ctx = shellCaseStackTop();
    if (!ctx) {
        runtimeError(vm, "shell case clause: no active case");
        shellUpdateStatus(1);
        goto cleanup;
    }
    /* A clause has matched and its body is next. */
    if (AS_STRING(args[0]) && strcmp(AS_STRING(args[0]), "restore") == 0) {
        shellUpdateStatus(ctx->incoming_status);
        goto cleanup;
    }
    if (AS_STRING(args[0]) && strcmp(AS_STRING(args[0]), "rematch") == 0) {
        ctx->matched = false;
        goto cleanup;
    }
    if (ctx->matched || ctx->skipped) {
        shellUpdateStatus(1);
        goto cleanup;
    }
    const char *subject = ctx->subject ? ctx->subject : "";
    bool matched = false;
    for (int i = 1; i < arg_count; ++i) {
        if (VALUE_TYPE(args[i]) != TYPE_STRING || !AS_STRING(args[i])) {
            continue;
        }
        const char *pattern_spec = AS_STRING(args[i]);
        const char *pattern_text = pattern_spec;
        const char *pattern_meta = NULL;
        size_t pattern_meta_len = 0;
        uint8_t pattern_flags = 0;
        if (!shellDecodeWordSpec(pattern_spec, &pattern_text, &pattern_flags, &pattern_meta, &pattern_meta_len)) {
            pattern_text = pattern_spec ? pattern_spec : "";
            pattern_flags = 0;
        }
        bool *pattern_map = NULL;
        size_t pattern_map_len = 0;
        char *expanded_pattern = shellExpandWord(pattern_text,
                                                pattern_flags,
                                                pattern_meta,
                                                pattern_meta_len,
                                                &pattern_map,
                                                &pattern_map_len,
                                                NULL);
        bool marked = expanded_pattern &&
                      (!pattern_map || shellMarkQuotedGlobChars(&expanded_pattern, &pattern_map, &pattern_map_len, NULL));
        free(pattern_map);
        if (!marked) {
            free(expanded_pattern);
            runtimeError(vm, "shell case clause: out of memory");
            shellUpdateStatus(1);
            goto cleanup;
        }
        if (shellWordShouldGlob(pattern_flags, expanded_pattern)) {
            ShellPatternFlags match_flags = shellShoptOptionEnabled("nocasematch")
                ? SHELL_PATTERN_NOCASE
                : SHELL_PATTERN_NONE;
            char *glob_pattern = shellGlobPatternFromField(expanded_pattern);
            int match_rc = glob_pattern ? shellPatternMatchEx(glob_pattern, subject, match_flags) : FNM_NOMATCH;
            free(glob_pattern);
            if (match_rc == 0) {
                free(expanded_pattern);
                matched = true;
                break;
            }
        } else {
            shellStripGlobMarks(expanded_pattern);
            if (shellUtf8StringCompare(expanded_pattern,
                                       subject,
                                       shellShoptOptionEnabled("nocasematch")) == 0) {
                free(expanded_pattern);
                matched = true;
                break;
            }
        }
        free(expanded_pattern);
    }
    if (matched) {
        ctx->matched = true;
        shellUpdateStatus(0);
    } else {
        shellUpdateStatus(1);
    }

cleanup:
    shellRuntimeLeaveCondition();
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellCaseEnd(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    ShellCaseContext *ctx = shellCaseStackTop();
    if (!ctx) {
        runtimeError(vm, "shell case end: no active case");
        shellUpdateStatus(1);
        shellRestoreCurrentVm(previous_vm);
        return makeVoid();
    }
    bool matched = ctx->matched;
    bool skipped = ctx->skipped;
    int incoming_status = ctx->incoming_status;
    shellCaseStackPop();
    if (skipped) {
        shellUpdateStatus(incoming_status);
    } else if (!matched) {
        shellUpdateStatus(0);
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

static bool shellEvaluateNumericComparison(const char *left,
                                           const char *op,
                                           const char *right,
                                           bool *out_result);
static bool shellEvaluateFileUnary(const char *op,
                                   const char *operand,
                                   bool *out_result);
static bool shellEvaluateFileBinary(const char *left,
                                    const char *op,
                                    const char *right,
                                    bool *out_result);
static bool shellSetOptionIsOn(const char *name);
static bool shellParseLong(const char *text, long *out_value);

static bool shellTestIsUnaryOperator(const char *token) {
    if (!token || token[0] == '\0') {
        return false;
    }
    static const char *const kUnaryOps[] = {
        "-a", "-b", "-c", "-d", "-e", "-f", "-g", "-h", "-k", "-o", "-p", "-r", "-s", "-t",
        "-u", "-v", "-w", "-x", "-L", "-S", "-O", "-G", "-N", "-n", "-z"
    };
    for (size_t i = 0; i < sizeof(kUnaryOps) / sizeof(kUnaryOps[0]); ++i) {
        if (strcmp(token, kUnaryOps[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool shellTestHasPatternChars(const char *pattern) {
    if (!pattern) {
        return false;
    }
    for (const char *p = pattern; *p; ++p) {
        if (*p == '*' || *p == '?' || *p == '[' || ((*p == '@' || *p == '+' || *p == '!') && p[1] == '(')) {
            return true;
        }
    }
    return false;
}

/* -v takes a name or a position, so the special parameters and an empty name are not set. */
static bool shellTestVariableIsSet(const char *name) {
    if (!isalnum((unsigned char)name[0]) && name[0] != '_') {
        return false;
    }
    bool is_set = false;
    free(shellLookupParameterValueInternal(name, strlen(name), &is_set));
    return is_set;
}

static bool shellTestApplyUnary(const char *op, const char *operand, bool *out_result) {
    if (!op || !out_result) {
        return false;
    }
    const char *value = operand ? operand : "";
    if (strcmp(op, "-n") == 0) {
        *out_result = (value[0] != '\0');
        return true;
    }
    if (strcmp(op, "-z") == 0) {
        *out_result = (value[0] == '\0');
        return true;
    }
    if (strcmp(op, "-o") == 0) {
        *out_result = shellSetOptionIsOn(value);
        return true;
    }
    bool result = false;
    if (strcmp(op, "-v") == 0) {
        *out_result = shellTestVariableIsSet(value);
        return true;
    }
    if (shellTestIsUnaryOperator(op) && shellEvaluateFileUnary(op, value, &result)) {
        *out_result = result;
        return true;
    }
    return false;
}

static bool shellTestApplyBinary(const char *left,
                                 const char *op,
                                 const char *right,
                                 bool *out_result) {
    if (!left || !op || !right || !out_result) {
        return false;
    }
    /* test and [ compare bytes. Patterns and nocasematch belong to [[ and case. */
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
        *out_result = (strcmp(left, right) == 0);
        return true;
    }
    if (strcmp(op, "!=") == 0) {
        *out_result = (strcmp(left, right) != 0);
        return true;
    }
    if (strcmp(op, "<") == 0) {
        *out_result = (strcmp(left, right) < 0);
        return true;
    }
    if (strcmp(op, ">") == 0) {
        *out_result = (strcmp(left, right) > 0);
        return true;
    }
    bool result = false;
    if (shellEvaluateNumericComparison(left, op, right, &result)) {
        *out_result = result;
        return true;
    }
    if (shellEvaluateFileBinary(left, op, right, &result)) {
        *out_result = result;
        return true;
    }
    return false;
}

static bool shellTestIsBinaryOperator(const char *token) {
    if (!token || token[0] == '\0') {
        return false;
    }
    static const char *const kBinaryOps[] = {
        "=", "==", "!=", "<", ">", "-eq", "-ne", "-gt", "-ge", "-lt", "-le",
        "-nt", "-ot", "-ef"
    };
    for (size_t i = 0; i < sizeof(kBinaryOps) / sizeof(kBinaryOps[0]); ++i) {
        if (strcmp(token, kBinaryOps[i]) == 0) {
            return true;
        }
    }
    return false;
}

typedef struct {
    VM *vm;
    const char *command_name;
    const char **argv;
    int argc;
    int pos;
    bool is_bracket;
    bool error_reported;
} ShellTestParser;

static void shellTestReportError(ShellTestParser *parser, const char *message) {
    if (!parser || parser->error_reported) {
        return;
    }
    const char *name = parser->command_name;
    if (parser->is_bracket) {
        name = "[";
    }
    if (!name || !*name) {
        name = "test";
    }
    shellReportRecoverableError(parser->vm, true, "%s: %s", name, message);
    parser->error_reported = true;
}

static void shellTestReportOperand(ShellTestParser *parser, const char *operand, const char *message) {
    char text[512];
    snprintf(text, sizeof(text), "%s: %s", operand, message);
    shellTestReportError(parser, text);
}

/* The one way a binary operator can fail is a number that is not one. */
static void shellTestReportBadInteger(ShellTestParser *parser, const char *left, const char *right) {
    long ignored = 0;
    shellTestReportOperand(parser, shellParseLong(left, &ignored) ? right : left, "integer expression expected");
}

static bool shellTestParseExpression(ShellTestParser *parser, bool *out_result);

static bool shellTestTryFastEvaluation(const char **argv, int argc, bool *out_result) {
    if (!argv || argc < 0 || !out_result) {
        return false;
    }

    if (argc == 0) {
        *out_result = false;
        return true;
    }

    if (argc == 1) {
        *out_result = (argv[0] && argv[0][0] != '\0');
        return true;
    }

    /* With three words a binary operator comes first, so a value that happens to be ! is a value. */
    if (argc == 3) {
        bool binary_result = false;
        if (shellTestApplyBinary(argv[0], argv[1], argv[2], &binary_result)) {
            *out_result = binary_result;
            return true;
        }
        if (strcmp(argv[1], "-a") == 0) {
            *out_result = (argv[0][0] != '\0' && argv[2][0] != '\0');
            return true;
        }
        if (strcmp(argv[1], "-o") == 0) {
            *out_result = (argv[0][0] != '\0' || argv[2][0] != '\0');
            return true;
        }
        if (shellTestIsBinaryOperator(argv[1])) {
            return false;
        }
    }

    if (strcmp(argv[0], "!") == 0) {
        bool inner = false;
        if (!shellTestTryFastEvaluation(argv + 1, argc - 1, &inner)) {
            return false;
        }
        *out_result = !inner;
        return true;
    }

    if (argc == 2) {
        bool unary_result = false;
        if (shellTestApplyUnary(argv[0], argv[1], &unary_result)) {
            *out_result = unary_result;
            return true;
        }
    }

    return false;
}

bool shellBuiltinTestFastEval(const char **argv, int argc, bool is_bracket, bool *out_result) {
    if (!argv || argc < 0 || !out_result) {
        return false;
    }
    if (is_bracket) {
        if (argc == 0) {
            return false;
        }
        if (strcmp(argv[argc - 1], "]") != 0) {
            return false;
        }
        argc--;
    }
    return shellTestTryFastEvaluation(argv, argc, out_result);
}

bool shellBuiltinArithmeticFastEval(const char *expression, int *out_status) {
    if (!expression || !out_status) {
        return false;
    }
    long long expr_value = 0;
    /* A failure has been reported and has run the expression's side effects: it is the answer,
     * and nothing may evaluate it again. */
    *out_status = (shellArithmeticEvaluate(expression, &expr_value, "((") && expr_value != 0) ? 0 : 1;
    return true;
}

static bool shellTestParsePrimary(ShellTestParser *parser, bool *out_result) {
    if (!parser || !out_result) {
        return false;
    }
    if (parser->pos >= parser->argc) {
        shellTestReportError(parser, "argument expected");
        return false;
    }
    const char *token = parser->argv[parser->pos];
    if (strcmp(token, "(") == 0) {
        parser->pos++;
        bool inner = false;
        if (!shellTestParseExpression(parser, &inner)) {
            return false;
        }
        if (parser->pos >= parser->argc) {
            shellTestReportError(parser, "`)' expected");
            return false;
        }
        if (strcmp(parser->argv[parser->pos], ")") != 0) {
            char text[512];
            snprintf(text, sizeof(text), "`)' expected, found %s", parser->argv[parser->pos]);
            shellTestReportError(parser, text);
            return false;
        }
        parser->pos++;
        *out_result = inner;
        return true;
    }
    if (parser->pos + 2 < parser->argc) {
        const char *op = parser->argv[parser->pos + 1];
        if (shellTestIsBinaryOperator(op)) {
            const char *left = token;
            const char *right = parser->argv[parser->pos + 2];
            bool value = false;
            if (!shellTestApplyBinary(left, op, right, &value)) {
                shellTestReportBadInteger(parser, left, right);
                return false;
            }
            parser->pos += 3;
            *out_result = value;
            return true;
        }
    }
    if (shellTestIsUnaryOperator(token) && parser->pos + 1 < parser->argc) {
        const char *operand = parser->argv[parser->pos + 1];
        bool value = false;
        if (!shellTestApplyUnary(token, operand, &value)) {
            shellTestReportError(parser, "invalid unary operator");
            return false;
        }
        parser->pos += 2;
        *out_result = value;
        return true;
    }
    parser->pos++;
    *out_result = (token && token[0] != '\0');
    return true;
}

static bool shellTestParseNot(ShellTestParser *parser, bool *out_result) {
    if (!parser || !out_result) {
        return false;
    }
    bool negate = false;
    while (parser->pos < parser->argc && strcmp(parser->argv[parser->pos], "!") == 0) {
        negate = !negate;
        parser->pos++;
    }
    bool value = false;
    if (!shellTestParsePrimary(parser, &value)) {
        return false;
    }
    if (negate) {
        value = !value;
    }
    *out_result = value;
    return true;
}

static bool shellTestParseAnd(ShellTestParser *parser, bool *out_result) {
    if (!parser || !out_result) {
        return false;
    }
    bool value = false;
    if (!shellTestParseNot(parser, &value)) {
        return false;
    }
    while (parser->pos < parser->argc && strcmp(parser->argv[parser->pos], "-a") == 0) {
        parser->pos++;
        bool rhs = false;
        if (!shellTestParseNot(parser, &rhs)) {
            return false;
        }
        value = value && rhs;
    }
    *out_result = value;
    return true;
}

static bool shellTestParseExpression(ShellTestParser *parser, bool *out_result) {
    if (!parser || !out_result) {
        return false;
    }
    bool value = false;
    if (!shellTestParseAnd(parser, &value)) {
        return false;
    }
    while (parser->pos < parser->argc && strcmp(parser->argv[parser->pos], "-o") == 0) {
        parser->pos++;
        bool rhs = false;
        if (!shellTestParseAnd(parser, &rhs)) {
            return false;
        }
        value = value || rhs;
    }
    *out_result = value;
    return true;
}

/* What the fast path turned down: a long expression or a malformed one. Returns the status. */
static int shellTestEvaluateSlowly(VM *vm, const char *name, const char **argv, int argc, bool is_bracket) {
    ShellTestParser parser = {
        .vm = vm,
        .command_name = name,
        .argv = argv,
        .argc = argc,
        .pos = 0,
        .is_bracket = is_bracket,
        .error_reported = false
    };

    /* Up to four words what they mean goes by how many there are, so say what the count wanted. */
    int first = 0;
    while (argc <= 4 && first < argc - 1 && strcmp(argv[first], "!") == 0 &&
           !(argc - first == 3 && shellTestIsBinaryOperator(argv[first + 1]))) {
        first++;
    }
    int rest = argc - first;
    if (argc <= 4 && rest == 2) {
        shellTestReportOperand(&parser, argv[first], "unary operator expected");
        return 2;
    }
    if (argc <= 4 && rest == 3) {
        if (shellTestIsBinaryOperator(argv[first + 1])) {
            shellTestReportBadInteger(&parser, argv[first], argv[first + 2]);
            return 2;
        }
        if (strcmp(argv[first], "(") != 0 || strcmp(argv[first + 2], ")") != 0) {
            shellTestReportOperand(&parser, argv[first + 1], "binary operator expected");
            return 2;
        }
    }

    bool result = false;
    bool ok = shellTestParseExpression(&parser, &result);
    if (ok && parser.pos < parser.argc) {
        shellTestReportError(&parser, "too many arguments");
        ok = false;
    }
    if (!ok) {
        return 2;
    }
    return result ? 0 : 1;
}

Value vmBuiltinShellTest(VM *vm, int arg_count, Value *args) {
    const char *name = shellRuntimeCurrentCommandName();
    if (!name || !*name) {
        name = (vm && vm->current_builtin_name) ? vm->current_builtin_name : "test";
    }
    bool is_bracket = (name && strcmp(name, "[") == 0);
    if (!is_bracket) {
        name = "test";
    }

    const char **argv = NULL;
    if (arg_count > 0) {
        argv = (const char **)malloc(sizeof(const char *) * (size_t)arg_count);
        if (!argv) {
            const char *prefix = is_bracket ? "[" : name;
            if (!prefix || !*prefix) {
                prefix = "test";
            }
            runtimeError(vm, "%s: out of memory", prefix);
            shellUpdateStatus(2);
            return makeVoid();
        }
        for (int i = 0; i < arg_count; ++i) {
            if (VALUE_TYPE(args[i]) == TYPE_STRING && AS_STRING(args[i])) {
                argv[i] = AS_STRING(args[i]);
            } else {
                argv[i] = "";
            }
        }
    }

    int argc = arg_count;
    if (is_bracket) {
        if (argc == 0 || strcmp(argv[argc - 1], "]") != 0) {
            runtimeError(vm, "[: missing `]'");
            free(argv);
            shellUpdateStatus(2);
            return makeVoid();
        }
        argc--;
        if (argc == 0) {
            free(argv);
            shellUpdateStatus(1);
            return makeVoid();
        }
    }

    if (argc == 0) {
        free(argv);
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool fast_result = false;
    if (shellTestTryFastEvaluation(argv, argc, &fast_result)) {
        shellUpdateStatus(fast_result ? 0 : 1);
        free(argv);
        return makeVoid();
    }

    shellUpdateStatus(shellTestEvaluateSlowly(vm, name, argv, argc, is_bracket));

    free(argv);
    return makeVoid();
}

Value vmBuiltinShellDefineFunction(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    if (shellLoopSkipActive()) {
        goto cleanup;
    }
    if (arg_count != 3) {
        runtimeError(vm, "shell define function: expected name, parameters, and body");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0]) || AS_STRING(args[0])[0] == '\0') {
        runtimeError(vm, "shell define function: name must be a non-empty string");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (VALUE_TYPE(args[1]) != TYPE_STRING && VALUE_TYPE(args[1]) != TYPE_VOID && VALUE_TYPE(args[1]) != TYPE_NIL) {
        runtimeError(vm, "shell define function: parameter metadata must be a string");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (VALUE_TYPE(args[2]) != TYPE_POINTER || !AS_POINTER(args[2])) {
        runtimeError(vm, "shell define function: missing compiled body");
        shellUpdateStatus(1);
        goto cleanup;
    }
    const char *name = AS_STRING(args[0]);
    const char *param_meta = NULL;
    if (VALUE_TYPE(args[1]) == TYPE_STRING && AS_STRING(args[1])) {
        param_meta = AS_STRING(args[1]);
    }
    ShellCompiledFunction *compiled = (ShellCompiledFunction *)AS_POINTER(args[2]);
    if (!shellStoreFunction(name, param_meta, compiled)) {
        shellDisposeCompiledFunction(compiled);
        runtimeError(vm, "shell define function: failed to store '%s'", name);
        shellUpdateStatus(1);
        goto cleanup;
    }
    AS_POINTER(args[2]) = NULL;
    shellUpdateStatus(0);

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

static bool shellParseLong(const char *text, long *out_value) {
    if (!out_value || !text || *text == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *out_value = value;
    return true;
}

static bool shellParseSignedLongLong(const char *text, long long *out_value) {
    if (!out_value) {
        return false;
    }
    if (!text || *text == '\0') {
        *out_value = 0;
        return true;
    }
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 0);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *out_value = value;
    return true;
}

static bool shellParseUnsignedLongLong(const char *text, unsigned long long *out_value) {
    if (!out_value) {
        return false;
    }
    if (!text || *text == '\0') {
        *out_value = 0;
        return true;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *out_value = value;
    return true;
}

static void shellGetoptsUpdateOptind(int value) {
    if (value < 1) {
        value = 1;
    }
    gShellRuntime.getopts_index = value;
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", value);
    shellSetTrackedVariable("OPTIND", buffer, false);
}

static void shellGetoptsSetOptarg(const char *value) {
    if (value && *value) {
        shellSetTrackedVariable("OPTARG", value, false);
    } else {
        shellUnsetTrackedVariable("OPTARG");
    }
}

static bool shellParseDouble(const char *text, double *out_value) {
    if (!out_value) {
        return false;
    }
    if (!text || *text == '\0') {
        *out_value = 0.0;
        return true;
    }
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *out_value = value;
    return true;
}

static bool shellEvaluateNumericComparison(const char *left,
                                          const char *op,
                                          const char *right,
                                          bool *out_result) {
    if (!left || !op || !right || !out_result) {
        return false;
    }
    long lhs = 0;
    long rhs = 0;
    if (!shellParseLong(left, &lhs) || !shellParseLong(right, &rhs)) {
        return false;
    }
    if (strcmp(op, "-eq") == 0) {
        *out_result = (lhs == rhs);
        return true;
    }
    if (strcmp(op, "-ne") == 0) {
        *out_result = (lhs != rhs);
        return true;
    }
    if (strcmp(op, "-gt") == 0) {
        *out_result = (lhs > rhs);
        return true;
    }
    if (strcmp(op, "-lt") == 0) {
        *out_result = (lhs < rhs);
        return true;
    }
    if (strcmp(op, "-ge") == 0) {
        *out_result = (lhs >= rhs);
        return true;
    }
    if (strcmp(op, "-le") == 0) {
        *out_result = (lhs <= rhs);
        return true;
    }
    return false;
}

static bool shellEvaluateFileUnary(const char *op,
                                   const char *operand,
                                   bool *out_result) {
    if (!op || !out_result) {
        return false;
    }
    const char *path = operand ? operand : "";
    if (strcmp(op, "-t") == 0) {
        long fd = 0;
        if (!shellParseLong(path, &fd)) {
            *out_result = false;
            return true;
        }
        *out_result = (fd >= 0 && pscalRuntimeFdIsInteractive((int)fd));
        return true;
    }

    if (*path == '\0') {
        *out_result = false;
        return true;
    }

    struct stat st;
    int rc = 0;
    bool use_lstat = (strcmp(op, "-h") == 0 || strcmp(op, "-L") == 0);
    if (use_lstat) {
        rc = lstat(path, &st);
    } else {
        rc = stat(path, &st);
    }
    if (rc != 0) {
        *out_result = false;
        return true;
    }

    if (strcmp(op, "-e") == 0 || strcmp(op, "-a") == 0) {
        *out_result = true;
        return true;
    }
    if (strcmp(op, "-f") == 0) {
        *out_result = S_ISREG(st.st_mode);
        return true;
    }
    if (strcmp(op, "-d") == 0) {
        *out_result = S_ISDIR(st.st_mode);
        return true;
    }
    if (strcmp(op, "-b") == 0) {
        *out_result = S_ISBLK(st.st_mode);
        return true;
    }
    if (strcmp(op, "-c") == 0) {
        *out_result = S_ISCHR(st.st_mode);
        return true;
    }
    if (strcmp(op, "-p") == 0) {
        *out_result = S_ISFIFO(st.st_mode);
        return true;
    }
    if (strcmp(op, "-S") == 0) {
        *out_result = S_ISSOCK(st.st_mode);
        return true;
    }
    if (strcmp(op, "-L") == 0 || strcmp(op, "-h") == 0) {
        *out_result = S_ISLNK(st.st_mode);
        return true;
    }
    if (strcmp(op, "-s") == 0) {
        *out_result = (st.st_size > 0);
        return true;
    }
    if (strcmp(op, "-r") == 0) {
        *out_result = (access(path, R_OK) == 0);
        return true;
    }
    if (strcmp(op, "-w") == 0) {
        *out_result = (access(path, W_OK) == 0);
        return true;
    }
    if (strcmp(op, "-x") == 0) {
        *out_result = (access(path, X_OK) == 0);
        return true;
    }
    if (strcmp(op, "-g") == 0) {
        *out_result = ((st.st_mode & S_ISGID) != 0);
        return true;
    }
    if (strcmp(op, "-u") == 0) {
        *out_result = ((st.st_mode & S_ISUID) != 0);
        return true;
    }
    if (strcmp(op, "-k") == 0) {
        *out_result = ((st.st_mode & S_ISVTX) != 0);
        return true;
    }
    if (strcmp(op, "-O") == 0) {
        *out_result = (st.st_uid == geteuid());
        return true;
    }
    if (strcmp(op, "-G") == 0) {
        *out_result = (st.st_gid == getegid());
        return true;
    }
    if (strcmp(op, "-N") == 0) {
        *out_result = (st.st_mtime > st.st_atime);
        return true;
    }

    return false;
}

static bool shellEvaluateFileBinary(const char *left,
                                    const char *op,
                                    const char *right,
                                    bool *out_result) {
    if (!left || !op || !right || !out_result) {
        return false;
    }
    struct stat left_stat;
    struct stat right_stat;
    if (strcmp(op, "-nt") == 0) {
        if (stat(left, &left_stat) != 0 || stat(right, &right_stat) != 0) {
            *out_result = false;
            return true;
        }
        *out_result = (left_stat.st_mtime > right_stat.st_mtime);
        return true;
    }
    if (strcmp(op, "-ot") == 0) {
        if (stat(left, &left_stat) != 0 || stat(right, &right_stat) != 0) {
            *out_result = false;
            return true;
        }
        *out_result = (left_stat.st_mtime < right_stat.st_mtime);
        return true;
    }
    if (strcmp(op, "-ef") == 0) {
        if (stat(left, &left_stat) != 0 || stat(right, &right_stat) != 0) {
            *out_result = false;
            return true;
        }
        *out_result = (left_stat.st_dev == right_stat.st_dev && left_stat.st_ino == right_stat.st_ino);
        return true;
    }
    return false;
}

/* [[ ]]: words with their operators, which only an unquoted operator can be. See
 * shellAddDoubleBracketWord for how an operand is told apart. */
typedef struct {
    Value *args;
    int count;
    int pos;
    int noeval;
    bool error;
} ShellCondParser;

static char *shellCondPeek(const ShellCondParser *parser) {
    if (parser->pos >= parser->count) {
        return NULL;
    }
    Value *value = &parser->args[parser->pos];
    return (VALUE_TYPE(*value) == TYPE_STRING && AS_STRING(*value)) ? AS_STRING(*value) : (char *)"";
}

static bool shellCondPeekIs(const ShellCondParser *parser, const char *op) {
    const char *text = shellCondPeek(parser);
    return text && strcmp(text, op) == 0;
}

static bool shellCondIsBinaryOperator(const char *text) {
    static const char *const kOps[] = {"==", "=",   "!=",  "=~",  "<",   ">",   "-eq", "-ne",
                                       "-lt", "-le", "-gt", "-ge", "-nt", "-ot", "-ef"};
    for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); ++i) {
        if (strcmp(text, kOps[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Sets BASH_REMATCH from a match, or empties it. */
static bool shellCondRegexMatch(const char *left, const char *pattern, bool *out_error) {
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        *out_error = true;
        return false;
    }
    size_t groups = regex.re_nsub + 1;
    regmatch_t *matches = (regmatch_t *)calloc(groups, sizeof(regmatch_t));
    bool matched = matches && regexec(&regex, left, groups, matches, 0) == 0;
    char **items = NULL;
    size_t count = 0;
    if (matched) {
        items = (char **)calloc(groups, sizeof(char *));
        for (size_t i = 0; items && i < groups; ++i) {
            if (matches[i].rm_so >= 0) {
                items[i] = strndup(left + matches[i].rm_so, (size_t)(matches[i].rm_eo - matches[i].rm_so));
            } else {
                items[i] = strdup("");
            }
        }
        count = items ? groups : 0;
    }
    shellArrayRegistryStore("BASH_REMATCH", items, NULL, count, SHELL_ARRAY_KIND_INDEXED);
    shellFreeArrayValues(items, count);
    free(matches);
    regfree(&regex);
    return matched;
}

static bool shellCondEvaluateBinary(ShellCondParser *parser, char *left, const char *op, char *right) {
    bool result = false;
    bool nocasematch = shellShoptOptionEnabled("nocasematch");
    if (strcmp(op, "=~") == 0) {
        shellStripGlobMarks(left);
        bool regex_error = false;
        result = shellCondRegexMatch(left, right, &regex_error);
        parser->error = parser->error || regex_error;
    } else if (strcmp(op, "==") == 0 || strcmp(op, "=") == 0 || strcmp(op, "!=") == 0) {
        shellStripGlobMarks(left);
        char *plain = strdup(right);
        shellGlobMarksToBackslashes(right);
        bool equal;
        if (shellTestHasPatternChars(right)) {
            equal = shellPatternMatchEx(right, left, nocasematch ? SHELL_PATTERN_NOCASE : SHELL_PATTERN_NONE) == 0;
        } else {
            if (plain) {
                shellStripGlobMarks(plain);
            }
            equal = shellUtf8StringCompare(left, plain ? plain : right, nocasematch) == 0;
        }
        free(plain);
        result = (op[0] == '!') ? !equal : equal;
    } else {
        shellStripGlobMarks(left);
        shellStripGlobMarks(right);
        if (strcmp(op, "<") == 0) {
            result = strcmp(left, right) < 0;
        } else if (strcmp(op, ">") == 0) {
            result = strcmp(left, right) > 0;
        } else if (!shellEvaluateFileBinary(left, op, right, &result)) {
            /* [[ -eq ]] takes arithmetic: [[ x -eq 3 ]]. */
            long long lhs = 0;
            long long rhs = 0;
            if (!shellArithmeticEvaluateText(left, 0, &lhs, "[[") || !shellArithmeticEvaluateText(right, 0, &rhs, "[[")) {
                /* Reported, and it is false. The rest of the test goes on. */
                result = false;
            } else if (strcmp(op, "-eq") == 0) {
                result = lhs == rhs;
            } else if (strcmp(op, "-ne") == 0) {
                result = lhs != rhs;
            } else if (strcmp(op, "-lt") == 0) {
                result = lhs < rhs;
            } else if (strcmp(op, "-le") == 0) {
                result = lhs <= rhs;
            } else if (strcmp(op, "-gt") == 0) {
                result = lhs > rhs;
            } else if (strcmp(op, "-ge") == 0) {
                result = lhs >= rhs;
            }
        }
    }
    return result;
}

static bool shellCondParseOr(ShellCondParser *parser);

static bool shellCondParsePrimary(ShellCondParser *parser) {
    char *first = shellCondPeek(parser);
    if (!first) {
        parser->error = true;
        return false;
    }
    if (strcmp(first, "(") == 0) {
        parser->pos++;
        bool result = shellCondParseOr(parser);
        if (!shellCondPeekIs(parser, ")")) {
            parser->error = true;
            return false;
        }
        parser->pos++;
        return result;
    }
    parser->pos++;
    char *next = shellCondPeek(parser);
    if (next && shellCondIsBinaryOperator(next)) {
        parser->pos++;
        char *right = shellCondPeek(parser);
        if (!right) {
            parser->error = true;
            return false;
        }
        parser->pos++;
        return parser->noeval > 0 ? false : shellCondEvaluateBinary(parser, first, next, right);
    }
    bool has_operand = next && strcmp(next, "&&") != 0 && strcmp(next, "||") != 0 && strcmp(next, ")") != 0;
    if (first[0] == '-' && first[1] && !first[2] && has_operand) {
        parser->pos++;
        if (parser->noeval > 0) {
            return false;
        }
        shellStripGlobMarks(next);
        bool result = false;
        if (first[1] == 'z') {
            result = next[0] == '\0';
        } else if (first[1] == 'n') {
            result = next[0] != '\0';
        } else if (first[1] == 'v') {
            result = shellTestVariableIsSet(next);
        } else if (first[1] == 'o') {
            result = shellSetOptionIsOn(next);
        } else if (!shellEvaluateFileUnary(first, next, &result)) {
            result = false;
            parser->error = true;
        }
        return result;
    }
    shellStripGlobMarks(first);
    return first[0] != '\0';
}

static bool shellCondParseNot(ShellCondParser *parser) {
    if (shellCondPeekIs(parser, "!")) {
        parser->pos++;
        return !shellCondParseNot(parser);
    }
    return shellCondParsePrimary(parser);
}

static bool shellCondParseAnd(ShellCondParser *parser) {
    bool result = shellCondParseNot(parser);
    while (shellCondPeekIs(parser, "&&")) {
        parser->pos++;
        if (!result) {
            parser->noeval++;
        }
        bool right = shellCondParseNot(parser);
        if (!result) {
            parser->noeval--;
        }
        result = result && right;
    }
    return result;
}

static bool shellCondParseOr(ShellCondParser *parser) {
    bool result = shellCondParseAnd(parser);
    while (shellCondPeekIs(parser, "||")) {
        parser->pos++;
        if (result) {
            parser->noeval++;
        }
        bool right = shellCondParseAnd(parser);
        if (result) {
            parser->noeval--;
        }
        result = result || right;
    }
    return result;
}

Value vmBuiltinShellDoubleBracket(VM *vm, int arg_count, Value *args) {
    (void)vm;
    ShellCondParser parser;
    memset(&parser, 0, sizeof(parser));
    parser.args = args;
    parser.count = arg_count;
    bool result = arg_count > 0 ? shellCondParseOr(&parser) : false;
    if (parser.pos < parser.count) {
        parser.error = true;
    }
    shellUpdateStatus(parser.error ? 2 : (result ? 0 : 1));
    return makeVoid();
}

Value vmBuiltinShellCd(VM *vm, int arg_count, Value *args) {
    const char *path = NULL;
    int arg_index = 0;

    while (arg_index < arg_count && VALUE_TYPE(args[arg_index]) == TYPE_STRING && AS_STRING(args[arg_index])) {
        const char *option = AS_STRING(args[arg_index]);
        if (strcmp(option, "--") == 0) {
            arg_index++;
            break;
        }
        if (strcmp(option, "-P") != 0 && strcmp(option, "-L") != 0 && strcmp(option, "-e") != 0 &&
            strcmp(option, "-eP") != 0 && strcmp(option, "-Pe") != 0) {
            break;
        }
        gShellCdPhysical = strchr(option, 'P') != NULL;
        arg_index++;
    }
    bool print_new_directory = false;
    if (arg_index < arg_count && VALUE_TYPE(args[arg_index]) == TYPE_STRING && AS_STRING(args[arg_index]) &&
        strcmp(AS_STRING(args[arg_index]), "-") == 0) {
        path = shellSafeGetenv("OLDPWD");
        if (!path) {
            shellReportRecoverableError(vm, true, "cd: OLDPWD not set");
            shellUpdateStatus(1);
            gShellCdPhysical = false;
            return makeVoid();
        }
        print_new_directory = true;
    }

    int remaining = arg_count - arg_index;
    if (print_new_directory) {
        /* cd -: the path is set above. */
    } else if (remaining <= 0) {
        path = shellSafeGetenv("PSCALI_REAL_HOME");
        if (!path || !*path) {
            path = shellSafeGetenv("HOME");
        }
        if (!path) {
            runtimeError(vm, "cd: HOME not set");
            shellUpdateStatus(1);
            return makeVoid();
        }
    } else if (VALUE_TYPE(args[arg_index]) == TYPE_STRING && AS_STRING(args[arg_index])) {
        if (remaining > 1) {
            runtimeError(vm, "cd: too many arguments");
            shellUpdateStatus(1);
            return makeVoid();
        }
        path = AS_STRING(args[arg_index]);
    } else {
        runtimeError(vm, "cd: expected directory path");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (!shellDirectoryStackEnsureInitialised(vm, "cd")) {
        return makeVoid();
    }

    char *new_cwd = NULL;
    char *old_cwd = NULL;
    bool changed = shellDirectoryStackChdir(vm, "cd", path, &new_cwd, &old_cwd);
    gShellCdPhysical = false;
    if (!changed) {
        return makeVoid();
    }
    if (print_new_directory && new_cwd) {
        printf("%s\n", new_cwd);
        fflush(stdout);
    }

    if (!shellDirectoryStackAssignTopOwned(new_cwd)) {
        shellDirectoryStackReportError(vm, "cd", "out of memory");
        shellUpdateStatus(1);
        if (old_cwd) {
            (void)SHELL_CHDIR(old_cwd);
            shellDirectoryStackEnsureInitialised(vm, "cd");
            (void)shellDirectoryStackUpdateEnvironment(NULL, old_cwd);
        }
        free(old_cwd);
        return makeVoid();
    }

    if (!shellDirectoryStackUpdateEnvironment(old_cwd, gShellRuntime.dir_stack[0])) {
        const char *readonly = shellReadonlyGetErrorName();
        if (old_cwd) {
            (void)SHELL_CHDIR(old_cwd);
            if (!shellDirectoryStackAssignTopOwned(old_cwd)) {
                free(old_cwd);
            }
            old_cwd = NULL;
        } else {
            (void)shellDirectoryStackAssignTopOwned(NULL);
        }
        if (errno == EPERM && readonly && *readonly) {
            runtimeError(vm, "cd: %s: readonly variable", readonly);
        } else if (errno != 0) {
            runtimeError(vm, "cd: %s", strerror(errno));
        } else {
            runtimeError(vm, "cd: failed to update environment");
        }
        shellUpdateStatus(1);
        free(old_cwd);
        return makeVoid();
    }
    free(old_cwd);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellPwd(VM *vm, int arg_count, Value *args) {
    bool physical = false;
    for (int i = 0; i < arg_count; ++i) {
        const char *option = (VALUE_TYPE(args[i]) == TYPE_STRING && AS_STRING(args[i])) ? AS_STRING(args[i]) : "";
        if (strcmp(option, "-P") == 0) {
            physical = true;
        } else if (strcmp(option, "-L") == 0) {
            physical = false;
        } else if (option[0] == '-' && option[1]) {
            shellReportRecoverableError(vm, true, "pwd: %s: invalid option", option);
            shellUpdateStatus(2);
            return makeVoid();
        }
    }
    char cwd[PATH_MAX];
    if (!SHELL_GETCWD(cwd, sizeof(cwd))) {
        runtimeError(vm, "pwd: %s", strerror(errno));
        shellUpdateStatus(1);
        return makeVoid();
    }
#if !defined(PSCAL_TARGET_IOS)
    /* $PWD if it still names this directory, which keeps the path a `cd` came by. */
    const char *logical = shellSafeGetenv("PWD");
    if (!physical && logical && logical[0] == '/' && shellPathIsCurrentDirectory(logical)) {
        printf("%s\n", logical);
        shellUpdateStatus(0);
        return makeVoid();
    }
#else
    (void)physical;
#endif
#if defined(PSCAL_TARGET_IOS)
    char display[PATH_MAX];
    const char *visible = pathTruncateStrip(cwd, display, sizeof(display)) ? display : cwd;
    printf("%s\n", visible);
#else
    printf("%s\n", cwd);
#endif
    shellUpdateStatus(0);
    return makeVoid();
}

static void shellCatEmitEscaped(int ch) {
    if (ch == '\n') {
        fputc('\n', stdout);
        return;
    }
    if (ch == '\t') {
        fputs("^I", stdout);
        return;
    }
    if (ch == '\r') {
        fputs("^M", stdout);
        return;
    }
    if (ch < 32) {
        fputc('^', stdout);
        fputc(ch + 64, stdout);
        return;
    }
    if (ch == 127) {
        fputs("^?", stdout);
        return;
    }
    if (ch & 0x80) {
        fputs("M-", stdout);
        shellCatEmitEscaped(ch & 0x7F);
        return;
    }
    fputc(ch, stdout);
}

static int shellCatStream(FILE *stream, bool show_nonprinting) {
    char buffer[4096];
    while (true) {
        size_t read_bytes = fread(buffer, 1, sizeof(buffer), stream);
        if (read_bytes > 0) {
            if (show_nonprinting) {
                for (size_t i = 0; i < read_bytes; ++i) {
                    shellCatEmitEscaped((unsigned char)buffer[i]);
                }
            } else {
                if (fwrite(buffer, 1, read_bytes, stdout) != read_bytes) {
                    return errno ? errno : 1;
                }
            }
        }
        if (read_bytes < sizeof(buffer)) {
            if (feof(stream)) {
                break;
            }
            if (ferror(stream)) {
                return errno ? errno : 1;
            }
        }
    }
    return 0;
}

static int shellCatPath(const char *path, bool show_nonprinting) {
    if (!path || strcmp(path, "-") == 0) {
        return shellCatStream(stdin, show_nonprinting);
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "cat: %s: %s\n", path ? path : "", strerror(errno));
        return errno ? errno : 1;
    }
    int rc = shellCatStream(fp, show_nonprinting);
    fclose(fp);
    if (rc != 0) {
        fprintf(stderr, "cat: %s: %s\n", path, strerror(errno ? errno : EIO));
    }
    return rc;
}

Value vmBuiltinShellCat(VM *vm, int arg_count, Value *args) {
    bool show_nonprinting = false;
    int index = 0;
    (void)vm;

    while (index < arg_count) {
        const Value *arg = &args[index];
        if (VALUE_TYPE(*arg) != TYPE_STRING || !AS_STRING(*arg) || AS_STRING(*arg)[0] != '-') {
            break;
        }
        const char *opt = AS_STRING(*arg) + 1;
        if (*opt == '\0') {
            break;
        }
        for (; *opt; ++opt) {
            if (*opt == 'v') {
                show_nonprinting = true;
            } else if (*opt == '-') {
                opt++;
                break;
            } else {
                runtimeError(vm, "cat: unknown option -- '%c'", *opt);
                shellUpdateStatus(1);
                return makeVoid();
            }
        }
        index++;
        if (opt[-1] == '-') {
            break;
        }
    }
    int status = 0;
    if (index >= arg_count) {
        status = shellCatPath("-", show_nonprinting);
    } else {
        for (int i = index; i < arg_count; ++i) {
            const Value *arg = &args[i];
            if (VALUE_TYPE(*arg) != TYPE_STRING || !AS_STRING(*arg)) {
                runtimeError(vm, "cat: expected string arguments");
                status = 1;
                break;
            }
            int rc = shellCatPath(AS_STRING(*arg), show_nonprinting);
            if (rc != 0 && status == 0) status = rc;
        }
    }
    shellUpdateStatus(status);
    return makeVoid();
}

Value vmBuiltinShellChmod(VM *vm, int arg_count, Value *args) {
#ifdef PSCAL_TARGET_IOS
    if (arg_count < 2) {
        runtimeError(vm, "chmod: usage: chmod MODE file [file ...]");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0]) || AS_STRING(args[0])[0] == '\0') {
        runtimeError(vm, "chmod: invalid mode");
        shellUpdateStatus(1);
        return makeVoid();
    }
    char *endptr = NULL;
    errno = 0;
    long parsed = strtol(AS_STRING(args[0]), &endptr, 8);
    if (errno != 0 || !endptr || *endptr != '\0' || parsed < 0 || parsed > 07777) {
        runtimeError(vm, "chmod: %s: invalid mode", AS_STRING(args[0]));
        shellUpdateStatus(1);
        return makeVoid();
    }
    mode_t mode = (mode_t)parsed;
    int final_status = 0;
    for (int i = 1; i < arg_count; ++i) {
        if (VALUE_TYPE(args[i]) != TYPE_STRING || !AS_STRING(args[i]) || AS_STRING(args[i])[0] == '\0') {
            runtimeError(vm, "chmod: expected file paths");
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (chmod(AS_STRING(args[i]), mode) != 0) {
            fprintf(stderr, "chmod: %s: %s\n", AS_STRING(args[i]), strerror(errno));
            if (final_status == 0) {
                final_status = errno ? errno : 1;
            }
        }
    }
    shellUpdateStatus(final_status);
    return makeVoid();
#else
    runtimeError(vm, "chmod: not supported on this platform");
    shellUpdateStatus(1);
    return makeVoid();
#endif
}

Value vmBuiltinShellClear(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "clear: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }
    /* Clear screen + scrollback to match typical terminal clear behaviour. */
    const char *sequence = "\x1B[3J\x1B[H\x1B[2J";
    if (fputs(sequence, stdout) == EOF) {
        runtimeError(vm, "clear: failed to write escape sequence");
        shellUpdateStatus(1);
        return makeVoid();
    }
    fflush(stdout);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellStdioInfo(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count > 0) {
        if (arg_count == 1 && VALUE_TYPE(args[0]) == TYPE_STRING && AS_STRING(args[0]) &&
            (strcmp(AS_STRING(args[0]), "--help") == 0 || strcmp(AS_STRING(args[0]), "-h") == 0)) {
            printf("stdioinfo\n");
            printf("  Show shell stdio and session routing details.\n");
            shellUpdateStatus(0);
            return makeVoid();
        }
        runtimeError(vm, "stdioinfo: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }

    printf("stdioinfo:\n");
    printf("  fds: stdin=%d stdout=%d stderr=%d\n", STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
    printf("  isatty: stdin=%d stdout=%d stderr=%d\n",
           isatty(STDIN_FILENO) != 0,
           isatty(STDOUT_FILENO) != 0,
           isatty(STDERR_FILENO) != 0);
    printf("  interactive: stdin=%d stdout=%d stderr=%d\n",
           pscalRuntimeFdIsInteractive(STDIN_FILENO) ? 1 : 0,
           pscalRuntimeFdIsInteractive(STDOUT_FILENO) ? 1 : 0,
           pscalRuntimeFdIsInteractive(STDERR_FILENO) ? 1 : 0);
    printf("  real_tty: stdin=%d stdout=%d stderr=%d\n",
           pscalRuntimeFdHasRealTTY(STDIN_FILENO) ? 1 : 0,
           pscalRuntimeFdHasRealTTY(STDOUT_FILENO) ? 1 : 0,
           pscalRuntimeFdHasRealTTY(STDERR_FILENO) ? 1 : 0);
#if defined(PSCAL_TARGET_IOS)
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    bool session_default = !session || vprocSessionStdioIsDefault(session);
    printf("  session: ptr=%p default=%d session_id=%llu kernel_pid=%d\n",
           (void *)session,
           session_default ? 1 : 0,
           session ? (unsigned long long)session->session_id : 0ull,
           session ? session->kernel_pid : 0);
    if (session) {
        printf("  host_fds: stdin=%d stdout=%d stderr=%d\n",
               session->stdin_host_fd,
               session->stdout_host_fd,
               session->stderr_host_fd);
        printf("  pscal_fds: stdin=%p stdout=%p stderr=%p\n",
               (void *)session->stdin_pscal_fd,
               (void *)session->stdout_pscal_fd,
               (void *)session->stderr_pscal_fd);
        printf("  pty: active=%d master=%p slave=%p\n",
               session->pty_active ? 1 : 0,
               (void *)session->pty_master,
               (void *)session->pty_slave);
    }
    VProc *vp = vprocCurrent();
    if (vp) {
        printf("  vproc: ptr=%p pid=%d\n", (void *)vp, vprocPid(vp));
        printf("  shim_map: stdin=%d stdout=%d stderr=%d\n",
               vprocTranslateFd(vp, STDIN_FILENO),
               vprocTranslateFd(vp, STDOUT_FILENO),
               vprocTranslateFd(vp, STDERR_FILENO));
    } else {
        printf("  vproc: <none>\n");
    }
#endif

    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellTabName(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0])) {
        runtimeError(vm, "tabname: expected a single title string");
        shellUpdateStatus(1);
        return makeVoid();
    }
#if defined(PSCAL_TAB_TITLE_SUPPORT)
    const char *title = AS_STRING(args[0]);
    int rc = PSCALRuntimeSetTabTitle(title);
    if (rc != 0) {
        runtimeError(vm, "tabname: unable to update tab title");
        shellUpdateStatus(1);
        return makeVoid();
    }
    shellUpdateStatus(0);
#else
    (void)args;
    runtimeError(vm, "tabname: tab naming is only available on iOS/iPadOS builds");
    shellUpdateStatus(1);
#endif
    return makeVoid();
}

Value vmBuiltinShellTabStartupCommand(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0])) {
        runtimeError(vm, "tscommand: expected a single command string");
        shellUpdateStatus(1);
        return makeVoid();
    }
#if defined(PSCAL_TAB_TITLE_SUPPORT)
    const char *command = AS_STRING(args[0]);
    int rc = PSCALRuntimeSetTabStartupCommand(command);
    if (rc != 0) {
        runtimeError(vm, "tscommand: unable to update tab startup command");
        shellUpdateStatus(1);
        return makeVoid();
    }
    shellUpdateStatus(0);
#else
    (void)args;
    runtimeError(vm, "tscommand: tab startup commands are only available on iOS/iPadOS builds");
    shellUpdateStatus(1);
#endif
    return makeVoid();
}

#ifdef PSCAL_TARGET_IOS
static void shellApplyWindowSize(int rows, int cols) {
    if (rows <= 0 || cols <= 0) {
        return;
    }
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", rows);
    setenv("LINES", buffer, 1);
    snprintf(buffer, sizeof(buffer), "%d", cols);
    setenv("COLUMNS", buffer, 1);
    printf("\x1b[8;%d;%dt", rows, cols);
    fflush(stdout);
}

Value vmBuiltinShellResize(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "resize: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }
    int rows = pscalRuntimeDetectWindowRows();
    int cols = pscalRuntimeDetectWindowCols();
    if (rows <= 0 || cols <= 0) {
        runtimeError(vm, "resize: unable to detect terminal size");
        shellUpdateStatus(1);
        return makeVoid();
    }
    shellApplyWindowSize(rows, cols);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellGwin(VM *vm, int arg_count, Value *args) {
#ifdef PSCAL_TARGET_IOS
    (void)arg_count;
    (void)args;
    runtimeError(vm, "gwin: unavailable on iOS/iPadOS");
    shellUpdateStatus(1);
    return makeVoid();
#else
    char *piped_message = NULL;
    if (!pscalRuntimeStdinIsInteractive()) {
        size_t cap = 1024;
        size_t len = 0;
        piped_message = (char *)malloc(cap);
        if (piped_message) {
            int ch = 0;
            while ((ch = fgetc(stdin)) != EOF) {
                if (len + 1 >= cap) {
                    size_t new_cap = cap * 2;
                    char *resized = (char *)realloc(piped_message, new_cap);
                    if (!resized) {
                        free(piped_message);
                        piped_message = NULL;
                        break;
                    }
                    piped_message = resized;
                    cap = new_cap;
                }
                piped_message[len++] = (char)ch;
            }
            if (piped_message) {
                while (len > 0 && (piped_message[len - 1] == '\n' || piped_message[len - 1] == '\r')) {
                    len--;
                }
                piped_message[len] = '\0';
                if (len == 0) {
                    free(piped_message);
                    piped_message = NULL;
                }
            }
        }
    }

    const char *message = piped_message ? piped_message : "gwin for the win";
    if (arg_count > 0) {
        if (VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0])) {
            runtimeError(vm, "gwin: expected an optional message string");
            shellUpdateStatus(1);
            free(piped_message);
            return makeVoid();
        }
        message = AS_STRING(args[0]);
    }
    pscalShowGwinMessage(message);
    shellUpdateStatus(0);
    if (piped_message) {
        free(piped_message);
    }
    return makeVoid();
#endif
}

Value vmBuiltinShellEditorDump(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "editordump: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }
    pscalEditorDump();
    shellUpdateStatus(0);
    return makeVoid();
}
#endif

#ifdef PSCAL_TARGET_IOS

typedef int (*ShellToolEntryFn)(int argc, char *argv[]);

extern VProcSessionStdio *PSCALRuntimeGetCurrentRuntimeStdio(void) __attribute__((weak));

typedef struct {
    FrontendKind kind;
    bool *reset_flag; /* optional */
    volatile int *exited_flag; /* optional: set last, however the thread ends */
} ShellFrontendCleanupCtx;

static void shellFrontendCleanup(void *arg) {
    ShellFrontendCleanupCtx *ctx = (ShellFrontendCleanupCtx *)arg;
    if (!ctx) {
        return;
    }
    if (!ctx->reset_flag || !*ctx->reset_flag) {
        shellInvalidateFrontendState(ctx->kind);
        if (ctx->reset_flag) {
            *ctx->reset_flag = true;
        }
    }
    if (ctx->exited_flag) {
        *ctx->exited_flag = 1;
    }
}

#if defined(PSCAL_TARGET_IOS)
static bool shellToolStatusRepresentsStop(int status) {
    if (status < 128 || status >= 128 + NSIG) {
        return false;
    }
    int sig = status - 128;
    return sig == SIGTSTP || sig == SIGSTOP || sig == SIGTTIN || sig == SIGTTOU;
}
#endif

#if defined(PSCAL_TARGET_IOS)
typedef void (*VmSuspendWaitHookFn)(void);
extern void vmSetSuspendWaitHooks(VmSuspendWaitHookFn before_hook, VmSuspendWaitHookFn after_hook);
extern void vprocSetStopWaitHooks(void (*before_hook)(void), void (*after_hook)(void));
void shellToolSuspendCheckpointBeforeWait(void);
void shellToolSuspendCheckpointAfterWait(void);

typedef struct ShellToolSuspendCheckpoint {
    PascalGlobalState *shell_state;
    int *shell_param_count;
    char ***shell_param_values;
    bool *shell_param_owned;
    PascalGlobalState worker_state;
    int worker_param_count;
    char **worker_param_values;
    bool worker_param_owned;
    int released_globals_lock_count;
    bool swapped;
    struct ShellToolSuspendCheckpoint *previous;
} ShellToolSuspendCheckpoint;

static __thread ShellToolSuspendCheckpoint *gShellToolSuspendCheckpoint = NULL;

static void shellToolSuspendCheckpointInstall(ShellToolSuspendCheckpoint *ctx,
                                              PascalGlobalState *shell_state,
                                              int *shell_param_count,
                                              char ***shell_param_values,
                                              bool *shell_param_owned) {
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->shell_state = shell_state;
    ctx->shell_param_count = shell_param_count;
    ctx->shell_param_values = shell_param_values;
    ctx->shell_param_owned = shell_param_owned;
    ctx->previous = gShellToolSuspendCheckpoint;
    gShellToolSuspendCheckpoint = ctx;
    vmSetSuspendWaitHooks(shellToolSuspendCheckpointBeforeWait, shellToolSuspendCheckpointAfterWait);
    vprocSetStopWaitHooks(shellToolSuspendCheckpointBeforeWait, shellToolSuspendCheckpointAfterWait);
}

static void shellToolSuspendCheckpointClear(ShellToolSuspendCheckpoint *ctx) {
    if (!ctx) {
        return;
    }
    if (gShellToolSuspendCheckpoint == ctx) {
        gShellToolSuspendCheckpoint = ctx->previous;
    }
    if (!gShellToolSuspendCheckpoint) {
        vmSetSuspendWaitHooks(NULL, NULL);
        vprocSetStopWaitHooks(NULL, NULL);
    }
}

void shellToolSuspendCheckpointBeforeWait(void) {
    ShellToolSuspendCheckpoint *ctx = gShellToolSuspendCheckpoint;
    if (!ctx || ctx->swapped) {
        return;
    }
    if (getenv("PSCALI_TOOL_DEBUG")) {
        fprintf(stderr,
                "[tool-checkpoint] before-wait shell_state=%p shell_params=%d/%p owned=%d pid=%d\n",
                (void *)ctx->shell_state,
                ctx->shell_param_count ? *ctx->shell_param_count : -1,
                (void *)(ctx->shell_param_values ? *ctx->shell_param_values : NULL),
                (int)(ctx->shell_param_owned ? *ctx->shell_param_owned : false),
                vprocGetPidShim());
    }

    pascalPushGlobalState(&ctx->worker_state);
    ctx->worker_param_count = gParamCount;
    ctx->worker_param_values = gParamValues;
    ctx->worker_param_owned = gParamValuesOwned;
    ctx->released_globals_lock_count = 0;
    while (pthread_mutex_unlock(&globals_mutex) == 0) {
        ctx->released_globals_lock_count++;
    }

    if (ctx->shell_state) {
        pascalPopGlobalState(ctx->shell_state);
    }
    if (ctx->shell_param_count) {
        gParamCount = *ctx->shell_param_count;
    }
    if (ctx->shell_param_values) {
        gParamValues = *ctx->shell_param_values;
    }
    if (ctx->shell_param_owned) {
        gParamValuesOwned = *ctx->shell_param_owned;
    }
    ctx->swapped = true;
    if (getenv("PSCALI_TOOL_DEBUG")) {
        fprintf(stderr,
                "[tool-checkpoint] before-wait swapped worker_params=%d/%p owned=%d pid=%d\n",
                ctx->worker_param_count,
                (void *)ctx->worker_param_values,
                (int)ctx->worker_param_owned,
                vprocGetPidShim());
    }
}

void shellToolSuspendCheckpointAfterWait(void) {
    ShellToolSuspendCheckpoint *ctx = gShellToolSuspendCheckpoint;
    if (!ctx || !ctx->swapped) {
        return;
    }
    if (getenv("PSCALI_TOOL_DEBUG")) {
        fprintf(stderr,
                "[tool-checkpoint] after-wait restoring worker_params=%d/%p owned=%d pid=%d\n",
                ctx->worker_param_count,
                (void *)ctx->worker_param_values,
                (int)ctx->worker_param_owned,
                vprocGetPidShim());
    }

    if (ctx->shell_state) {
        pascalPushGlobalState(ctx->shell_state);
    }
    if (ctx->shell_param_count) {
        *ctx->shell_param_count = gParamCount;
    }
    if (ctx->shell_param_values) {
        *ctx->shell_param_values = gParamValues;
    }
    if (ctx->shell_param_owned) {
        *ctx->shell_param_owned = gParamValuesOwned;
    }

    pascalPopGlobalState(&ctx->worker_state);
    gParamCount = ctx->worker_param_count;
    gParamValues = ctx->worker_param_values;
    gParamValuesOwned = ctx->worker_param_owned;
    for (int i = 0; i < ctx->released_globals_lock_count; ++i) {
        pthread_mutex_lock(&globals_mutex);
    }
    ctx->released_globals_lock_count = 0;
    ctx->swapped = false;
    if (getenv("PSCALI_TOOL_DEBUG")) {
        fprintf(stderr,
                "[tool-checkpoint] after-wait restored shell_params=%d/%p owned=%d pid=%d\n",
                ctx->shell_param_count ? *ctx->shell_param_count : -1,
                (void *)(ctx->shell_param_values ? *ctx->shell_param_values : NULL),
                (int)(ctx->shell_param_owned ? *ctx->shell_param_owned : false),
                vprocGetPidShim());
    }
}
#endif

static int shellRunToolInlineWithRestore(ShellToolEntryFn entry,
                                         int argc,
                                         char **argv,
                                         bool dbg,
                                         FrontendKind target_kind,
                                         bool *out_frontend_reset) {
    ShellSignalBackup sig_backup;
    ShellStdFdBackup fd_backup;
    VProcSessionStdio *prev_session = vprocSessionStdioCurrent();
    VProcSessionStdio *active_session = prev_session;
    VProc *prev_vproc = vprocCurrent();
    struct termios stdin_termios;
    bool stdin_termios_valid = false;
    bool pipeline_inline_stage =
        gShellRuntime.pipeline.active &&
        gShellRuntime.pipeline.in_process &&
        !gShellBackgroundWorkerActive;
    bool manage_process_state = !pipeline_inline_stage;
    int shell_pid_for_stop = vprocGetShellSelfPid();
    int stage_pid_for_stop = prev_vproc ? vprocPid(prev_vproc) : -1;
    bool cooperative_stop_scope = false;
    if (prev_vproc &&
        stage_pid_for_stop > 0 &&
        stage_pid_for_stop != shell_pid_for_stop &&
        (pipeline_inline_stage || target_kind != FRONTEND_KIND_SHELL)) {
        /* Frontend stages run inside synthetic vprocs; keep SIGTSTP
         * cooperative so Ctrl-Z can unwind to shell instead of parking the
         * stage thread in a hard-stop wait. */
        vprocSetStopUnsupported(stage_pid_for_stop, true);
        cooperative_stop_scope = true;
        shellRuntimeSetLastStatus(0);
    }
    if (manage_process_state) {
        shellSaveSignalState(&sig_backup);
        shellSaveStandardFds(&fd_backup);
    }
    if (dbg && manage_process_state) {
        int stdin_host = STDIN_FILENO;
        VProc *vp = vprocCurrent();
        if (vp) {
            int translated = vprocTranslateFd(vp, STDIN_FILENO);
            if (translated >= 0) {
                stdin_host = translated;
            }
        }
        fprintf(stderr,
                "[tool-inline] stdio save stdin=%d host=%d tty=%d vproc=%p session=%p\n",
                STDIN_FILENO,
                stdin_host,
                pscalRuntimeStdinHasRealTTY() ? 1 : 0,
                (void *)prev_vproc,
                (void *)prev_session);
    }
    if (PSCALRuntimeGetCurrentRuntimeStdio) {
        VProcSessionStdio *runtime_stdio = PSCALRuntimeGetCurrentRuntimeStdio();
        if (runtime_stdio && !vprocSessionStdioIsDefault(runtime_stdio) &&
            (!active_session || vprocSessionStdioIsDefault(active_session))) {
            active_session = runtime_stdio;
        }
    }
    if (active_session && active_session != prev_session) {
        vprocSessionStdioActivate(active_session);
    }
    if (manage_process_state && shellToolTcgetattr(STDIN_FILENO, &stdin_termios)) {
        stdin_termios_valid = true;
    }
    int status = 0;
    bool frontend_reset = false;
    ShellFrontendCleanupCtx cleanup_ctx = {
        .kind = target_kind,
        .reset_flag = &frontend_reset,
    };
    pthread_cleanup_push(shellFrontendCleanup, &cleanup_ctx);
    status = entry(argc, argv);
    pthread_cleanup_pop(1); /* always run cleanup, even on pthread_exit */
    if (cooperative_stop_scope && status == EXIT_SUCCESS) {
        int runtime_status = shellRuntimeLastStatus();
        if (shellToolStatusRepresentsStop(runtime_status)) {
            status = runtime_status;
        }
    }
    if (out_frontend_reset) {
        *out_frontend_reset = frontend_reset;
    }
    shellFlushStandardStreams();
    if (prev_session) {
        vprocSessionStdioActivate(prev_session);
    } else {
        vprocSessionStdioActivate(NULL);
    }
    VProc *current_vproc = vprocCurrent();
    if (prev_vproc) {
        if (current_vproc != prev_vproc) {
            if (current_vproc) {
                vprocDeactivate();
                current_vproc = vprocCurrent();
            }
            if (current_vproc != prev_vproc) {
                vprocActivate(prev_vproc);
            }
        }
    } else {
        while (vprocCurrent() != NULL) {
            vprocDeactivate();
        }
    }
    if (manage_process_state) {
        shellRestoreStandardFds(&fd_backup, dbg);
        if (stdin_termios_valid) {
            shellToolTcsetattr(STDIN_FILENO, SHELL_TOOL_RESTORE_ACTION, &stdin_termios);
        }
        shellCloseVprocStreams();
        if (dbg) {
            int stdin_host = STDIN_FILENO;
            VProc *vp = vprocCurrent();
            if (vp) {
                int translated = vprocTranslateFd(vp, STDIN_FILENO);
                if (translated >= 0) {
                    stdin_host = translated;
                }
            }
            fprintf(stderr,
                    "[tool-inline] stdio restore stdin=%d host=%d tty=%d vproc=%p session=%p\n",
                    STDIN_FILENO,
                    stdin_host,
                    pscalRuntimeStdinHasRealTTY() ? 1 : 0,
                    (void *)vprocCurrent(),
                    (void *)vprocSessionStdioCurrent());
        }
        shellRestoreSignalState(&sig_backup);
        shellRuntimeEnsureStandardFds();
        shellEnsureJobControl();
        shellJobControlRestoreForeground();
        if (dbg) {
            int shell_pid = vprocGetShellSelfPid();
            int sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : -1;
            int fg = (sid > 0) ? vprocGetForegroundPgid(sid) : -1;
            fprintf(stderr,
                    "[tool-inline] restore complete shell=%d sid=%d fg=%d\n",
                    shell_pid,
                    sid,
                    fg);
        }
    } else if (dbg) {
        fprintf(stderr, "[tool-inline] pipeline-stage: skipped process stdio/signal restore\n");
    }
    if (cooperative_stop_scope && stage_pid_for_stop > 0) {
        vprocSetStopUnsupported(stage_pid_for_stop, false);
    }
    return status;
}

#define SHELL_TOOL_THREAD_STACK_SZ (8 * 1024 * 1024)

typedef struct PSCALRuntimeContext PSCALRuntimeContext;
extern PSCALRuntimeContext *PSCALRuntimeGetCurrentRuntimeContext(void) __attribute__((weak));
extern void PSCALRuntimeSetCurrentRuntimeContext(PSCALRuntimeContext *ctx) __attribute__((weak));

typedef struct {
    ShellToolEntryFn entry;
    int argc;
    char **argv;
    bool argv_owned;
    FrontendKind frontend_kind;
    bool frontend_reset_done;
    VProcSessionStdio *session_stdio;
    VProc *session_vproc;
    PSCALRuntimeContext *runtime_ctx;
#if defined(PSCAL_TARGET_IOS)
    ShellToolSuspendCheckpoint *suspend_checkpoint;
#endif
    int stdout_fd;
    int stderr_fd;
    int status;
    bool entry_returned;
    volatile int finished;
    volatile int worker_exited;
    pthread_t worker_thread;
    pthread_t main_thread;
    bool use_shell_context;
#if defined(PSCAL_TARGET_IOS)
    /* Dedicated SIGINT catcher: sits in sigwait() (SIGINT blocked everywhere) so
     * a foreground Ctrl-C is dequeued reliably and turned into a SIGUSR1 poke to
     * the worker (longjmp interrupt) plus the VM abort flag. */
    pthread_t sigint_catcher_thread;
    volatile sig_atomic_t sigint_catcher_stop;
    bool sigint_catcher_active;
    pthread_t waiter_thread;
#endif
} ShellToolThreadContext;

static __thread sigjmp_buf gShellThreadJumpBuf;
static __thread int gShellThreadLastSignal = 0;

static void shellThreadSigintHandler(int signo) {
    gShellThreadLastSignal = signo;
    siglongjmp(gShellThreadJumpBuf, 1);
}

static void *shellToolThreadMain(void *arg) {
    ShellToolThreadContext *ctx = (ShellToolThreadContext *)arg;
    if (!ctx || !ctx->entry) {
        if (ctx) {
            ctx->status = 127;
            ctx->finished = 1;
        }
        return NULL;
    }
    ShellFrontendCleanupCtx cleanup_ctx = {
        .kind = ctx->frontend_kind,
        .reset_flag = &ctx->frontend_reset_done,
        .exited_flag = &ctx->worker_exited,
    };
    pthread_cleanup_push(shellFrontendCleanup, &cleanup_ctx);
    bool dbg = getenv("PSCALI_TOOL_DEBUG") != NULL;
    int worker_pid = -1;
#if defined(PSCAL_TARGET_IOS)
    ShellToolSuspendCheckpoint *previous_checkpoint = gShellToolSuspendCheckpoint;
    bool installed_suspend_hooks = false;
#endif
    PSCALRuntimeContext *prev_runtime_ctx = NULL;
    if (PSCALRuntimeGetCurrentRuntimeContext) {
        prev_runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext();
    }
    if (PSCALRuntimeSetCurrentRuntimeContext && ctx->runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(ctx->runtime_ctx);
        if (getenv("PSCALI_VPROC_DEBUG")) {
            fprintf(stderr,
                    "[tool-thread] runtime ctx=%p\n",
                    (void *)ctx->runtime_ctx);
        }
    }
    VProcSessionStdio *prev_stdio = vprocSessionStdioCurrent();
    if (ctx->session_stdio) {
        vprocSessionStdioActivate(ctx->session_stdio);
    }
    bool vproc_active = false;
    if (ctx->session_vproc) {
        vprocRegisterThread(ctx->session_vproc, pthread_self());
        vprocActivate(ctx->session_vproc);
        vproc_active = true;
        worker_pid = vprocPid(ctx->session_vproc);
        if (worker_pid > 0 && ctx->frontend_kind != FRONTEND_KIND_SHELL) {
            /* Frontend tools run on a dedicated worker thread. Keep hard-stop
             * semantics enabled so Ctrl-Z parks the worker in-place and fg/bg
             * can resume the same execution context. */
            vprocSetStopUnsupported(worker_pid, false);
            vprocSetCooperativeStopWait(worker_pid, false);
        }
    }
#if defined(PSCAL_TARGET_IOS)
    if (ctx->suspend_checkpoint) {
        gShellToolSuspendCheckpoint = ctx->suspend_checkpoint;
        vmSetSuspendWaitHooks(shellToolSuspendCheckpointBeforeWait, shellToolSuspendCheckpointAfterWait);
        vprocSetStopWaitHooks(shellToolSuspendCheckpointBeforeWait, shellToolSuspendCheckpointAfterWait);
        installed_suspend_hooks = true;
    }
#endif
    if (dbg) {
        fprintf(stderr, "[tool-thread] worker start argc=%d entry=%p vproc=%p\n",
                ctx->argc, (void *)ctx->entry, (void *)ctx->session_vproc);
    }
    (void)ctx->stdout_fd;
    (void)ctx->stderr_fd;

    /* On iOS SIGINT is owned centrally: it stays BLOCKED on every thread and a
     * foreground Ctrl-C is dequeued by the tool-runner's dedicated sigwait
     * catcher, which sets the cooperative abort flag the VM polls
     * per-instruction (a longjmp out of a mid-instruction VM would corrupt it,
     * so we do not interrupt the worker via a signal). Keep SIGINT blocked here
     * and install no SIGINT handler. On other platforms SIGINT drives the
     * longjmp interrupt handler as before. */
    /* VM frontends abort cooperatively via the flag (a longjmp out of a
     * mid-instruction VM would corrupt it). Applets are plain read loops that
     * cannot poll the flag, so they need their blocking read() interrupted. */
    bool vm_frontend = (ctx->frontend_kind == FRONTEND_KIND_PASCAL ||
                        ctx->frontend_kind == FRONTEND_KIND_REA ||
                        ctx->frontend_kind == FRONTEND_KIND_AETHER ||
                        ctx->frontend_kind == FRONTEND_KIND_CLIKE);
    sigset_t unblock_mask;
    sigemptyset(&unblock_mask);
#if !defined(PSCAL_TARGET_IOS)
    sigaddset(&unblock_mask, SIGINT);
#else
    /* On iOS SIGINT stays BLOCKED on every worker (the sigwait catcher owns it).
     * For applets, unblock SIGUSR1 so the catcher's poke fires the longjmp
     * handler and unwinds the blocked read(). VM frontends keep SIGUSR1 blocked
     * (poke is then a harmless no-op) and rely on the flag. */
    if (!vm_frontend) {
        sigaddset(&unblock_mask, SIGUSR1);
    }
#endif
    sigaddset(&unblock_mask, SIGTSTP);
    pthread_sigmask(SIG_UNBLOCK, &unblock_mask, NULL);

    /* Install the cooperative longjmp handler. On iOS it is the SIGINT
     * disposition purely as a safety net (SIGINT stays blocked so it never
     * fires async -- it just guarantees the disposition is never SIG_DFL), and
     * for applets it is ALSO bound to SIGUSR1, which the catcher pokes to
     * interrupt a blocked read(). On other platforms it is the live SIGINT
     * interrupt handler. */
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shellThreadSigintHandler;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART is purposely omitted so we interrupt blocking calls */
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, &old_sa);
#if defined(PSCAL_TARGET_IOS)
    if (!vm_frontend) {
        struct sigaction usr1_old;
        sigaction(SIGUSR1, &sa, &usr1_old);
    }
#endif

    FrontendKind previous_kind = frontendPushKind(ctx->frontend_kind);
    ShellRuntimeState *thread_ctx = NULL;
    ShellRuntimeState *prev_ctx = NULL;
    if (ctx->use_shell_context) {
        thread_ctx = shellRuntimeCreateContext();
        prev_ctx = shellRuntimeActivateContext(thread_ctx);
    }

    gShellThreadLastSignal = 0;
    if (sigsetjmp(gShellThreadJumpBuf, 1) == 0) {
        if (ctx->stdout_fd >= 0) {
            dup2(ctx->stdout_fd, STDOUT_FILENO);
        }
        if (ctx->stderr_fd >= 0) {
            dup2(ctx->stderr_fd, STDERR_FILENO);
        }
        if (ctx->stdout_fd >= 0) {
            close(ctx->stdout_fd);
            ctx->stdout_fd = -1;
        }
        if (ctx->stderr_fd >= 0 && ctx->stderr_fd != ctx->stdout_fd) {
            close(ctx->stderr_fd);
            ctx->stderr_fd = -1;
        }
        ctx->status = ctx->entry(ctx->argc, ctx->argv);
    } else {
        /* Interrupted */
        ctx->status = 130;
    }
    ctx->entry_returned = true;

    if (ctx->use_shell_context) {
        shellRuntimeActivateContext(prev_ctx);
        shellRuntimeDestroyContext(thread_ctx);
    }
    frontendPopKind(previous_kind);

    if (ctx->argv_owned && ctx->argv) {
        for (int i = 0; i < ctx->argc; ++i) {
            free(ctx->argv[i]);
        }
        free(ctx->argv);
        ctx->argv = NULL;
    }

    (void)ctx->status;

    /* Restore signal handler. */
    sigaction(SIGINT, &old_sa, NULL);
    if (ctx->session_stdio) {
        vprocSessionStdioActivate(prev_stdio);
    }
    if (vproc_active) {
        vprocDeactivate();
    }
    if (PSCALRuntimeSetCurrentRuntimeContext && prev_runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(prev_runtime_ctx);
    }
#if defined(PSCAL_TARGET_IOS)
    if (installed_suspend_hooks) {
        gShellToolSuspendCheckpoint = previous_checkpoint;
        if (previous_checkpoint) {
            vmSetSuspendWaitHooks(shellToolSuspendCheckpointBeforeWait, shellToolSuspendCheckpointAfterWait);
            vprocSetStopWaitHooks(shellToolSuspendCheckpointBeforeWait, shellToolSuspendCheckpointAfterWait);
        } else {
            vmSetSuspendWaitHooks(NULL, NULL);
            vprocSetStopWaitHooks(NULL, NULL);
        }
    }
#endif
    if (dbg) {
        fprintf(stderr, "[tool-thread] worker done status=%d\n", ctx->status);
    }
    pthread_cleanup_pop(1);
    return NULL;
}

static void *shellToolWaiterMain(void *arg) {
    ShellToolThreadContext *ctx = (ShellToolThreadContext *)arg;
    void *exit_value = NULL;
    pthread_join(ctx->worker_thread, &exit_value);
    /* exit() is pthread_exit in the app, so Pascal's halt(3) ends the thread before
     * it can store a status. */
    if (!ctx->entry_returned && exit_value != PTHREAD_CANCELED) {
        ctx->status = (int)(intptr_t)exit_value & 0xff;
    }
    ctx->finished = 1;
    pthread_kill(ctx->main_thread, SIGURG);
    if (getenv("PSCALI_TOOL_DEBUG")) {
        fprintf(stderr, "[tool-thread] waiter signaled main\n");
    }
    return NULL;
}

#if defined(PSCAL_TARGET_IOS)
/* Dedicated SIGINT catcher thread. With SIGINT blocked on every thread during
 * tool execution, a foreground Ctrl-C is delivered to whichever thread waits in
 * sigwait() for it -- reliably, and without ever reaching a SIG_DFL async
 * disposition that would kill the shell. Each catch (a) pokes the worker with
 * SIGUSR1 so a blocked applet read() or the VM loop unwinds via siglongjmp, and
 * (b) sets the cooperative abort flag the VM also polls per-instruction, plus a
 * poll-wake for blocked vproc waits. The owner stops it by setting
 * sigint_catcher_stop and delivering one SIGINT to break the final sigwait(). */
static void *shellToolSigintCatcherMain(void *arg) {
    ShellToolThreadContext *ctx = (ShellToolThreadContext *)arg;
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, SIGINT);
    while (!ctx->sigint_catcher_stop) {
        int caught = 0;
        if (sigwait(&wait_set, &caught) != 0) {
            continue;
        }
        if (ctx->sigint_catcher_stop) {
            break;
        }
        if (caught == SIGINT) {
            /* Set the cooperative abort flag the VM polls per-instruction (the
             * safe path for frontends), wake any blocked vproc pipe/poll wait,
             * and poke the worker with SIGUSR1. The poke fires the longjmp
             * handler only on applet workers (which unblock SIGUSR1) to unwind a
             * blocked read(); VM-frontend workers keep SIGUSR1 blocked so the
             * poke is a harmless no-op for them. */
            pscalRuntimeRequestSigint();
            pscal_fd_poll_wakeup(NULL, 0);
            (void)pthread_kill(ctx->worker_thread, SIGUSR1);
        }
    }
    return NULL;
}

static void shellToolSigintCatcherStopHandler(int signo) {
    (void)signo;
}

/* One directed SIGINT breaks the catcher's final sigwait(). The host drops an
 * ignored signal instead of queueing it, and a process started with SIGINT ignored
 * (a script's `cmd &` does that) then waited here for ever. So the signal is caught
 * for as long as this takes. The lock stops two sessions restoring each other's
 * disposition. */
static void shellToolStopSigintCatcher(ShellToolThreadContext *ctx) {
    static pthread_mutex_t stop_mu = PTHREAD_MUTEX_INITIALIZER;
    struct sigaction stop_action;
    struct sigaction previous;
    memset(&stop_action, 0, sizeof(stop_action));
    stop_action.sa_handler = shellToolSigintCatcherStopHandler;
    stop_action.sa_flags = SA_RESTART;
    sigemptyset(&stop_action.sa_mask);

    /* The join below is a cancellation point. A kill there would leave the lock held, and every
     * tool that ended after it would wait here for ever. */
    int cancel_state = PTHREAD_CANCEL_ENABLE;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    pthread_mutex_lock(&stop_mu);
    ctx->sigint_catcher_stop = 1;
    bool ignored = vprocHostSigaction(SIGINT, NULL, &previous) == 0 &&
                   previous.sa_handler == SIG_IGN;
    if (ignored) {
        vprocHostSigaction(SIGINT, &stop_action, NULL);
    }
    pthread_kill(ctx->sigint_catcher_thread, SIGINT);
    pthread_join(ctx->sigint_catcher_thread, NULL);
    if (ignored) {
        vprocHostSigaction(SIGINT, &previous, NULL);
    }
    pthread_mutex_unlock(&stop_mu);
    pthread_setcancelstate(cancel_state, NULL);
    ctx->sigint_catcher_active = false;
}
#endif

#if defined(PSCAL_TARGET_IOS)
/* The thread that started a tool has been cancelled. A kill can reach it and not the
 * worker: killing a background pipeline's subshell cancels the threads started under that
 * subshell, and the worker started under its stage. The worker's suspend checkpoint is in
 * this thread's stack, which went on its way out, and the worker's next read ended the
 * app. So the kill goes on to the worker, and this thread outlasts it. */
static void shellToolStarterCancelled(void *arg) {
    ShellToolThreadContext *context = (ShellToolThreadContext *)arg;
    /* The worker's own flag: the same kill may have cancelled the waiter that sets finished. */
    if (!context->worker_exited) {
        pthread_cancel(context->worker_thread);
    }
    while (!context->worker_exited) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000 * 1000;
        nanosleep(&ts, NULL);
    }
    if (context->sigint_catcher_active) {
        shellToolStopSigintCatcher(context);
    }
    pthread_join(context->waiter_thread, NULL);
    free(context);
}
#endif

static int shellRunToolInThread(ShellToolEntryFn entry,
                                int argc,
                                char **argv,
                                FrontendKind frontend_kind,
                                bool use_shell_context,
                                bool background,
                                bool *out_frontend_reset) {
    if (!entry) {
        return 127;
    }
    if (background) {
        ShellToolThreadContext *context =
            (ShellToolThreadContext *)calloc(1, sizeof(ShellToolThreadContext));
        if (!context) {
            return entry(argc, argv);
        }
        /* Avoid racing with shared argv if the caller frees it after return. */
        context->argv_owned = false;
        if (argv) {
            context->argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
            if (context->argv) {
                context->argv_owned = true;
                for (int i = 0; i < argc; ++i) {
                    if (argv[i]) {
                        context->argv[i] = strdup(argv[i]);
                        if (!context->argv[i]) {
                            context->argv_owned = false;
                            break;
                        }
                    }
                }
                context->argv[argc] = NULL;
            }
        }
        if (context->argv_owned == false) {
            /* Cloning failed; run synchronously to avoid use-after-free. */
            free(context->argv);
            free(context);
            return entry(argc, argv);
        }
        context->entry = entry;
        context->argc = argc;
        context->status = 0;
        context->finished = 0;
        context->main_thread = pthread_self();
        context->frontend_kind = frontend_kind;
        context->use_shell_context = use_shell_context;
        context->session_stdio = vprocSessionStdioCurrent();
        context->session_vproc = vprocCurrent();
        context->runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext
            ? PSCALRuntimeGetCurrentRuntimeContext()
            : NULL;
        /* No suspend checkpoint: it lives in the caller's frame, which ends as this returns. */
        context->stdout_fd = dup(STDOUT_FILENO);
        context->stderr_fd = dup(STDERR_FILENO);
        if (getenv("PSCALI_VPROC_DEBUG")) {
            fprintf(stderr,
                    "[tool-thread] spawn bg stdio=%p vproc=%p stdout_fd=%d stderr_fd=%d\n",
                    (void *)context->session_stdio,
                    (void *)context->session_vproc,
                    context->stdout_fd,
                    context->stderr_fd);
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
        int create_rc = pthread_create(&context->worker_thread, &attr, shellToolThreadMain, context);
        pthread_attr_destroy(&attr);
        if (create_rc != 0) {
            free(context);
            return entry(argc, argv);
        }
        pthread_detach(context->worker_thread);
        return 0;
    }

    /* Heap-allocated rather than a stack local: the worker, waiter and SIGINT
     * catcher threads all receive this pointer, and on the iOS/vproc path their
     * lifetime is not strictly contained by this frame -- a stack local left
     * them dereferencing unmapped memory. The background path above already
     * heap-allocates for the same reason. Freed once the helpers are joined. */
    ShellToolThreadContext *context =
        (ShellToolThreadContext *)calloc(1, sizeof(ShellToolThreadContext));
    if (!context) {
        return entry(argc, argv);
    }
    context->entry = entry;
    context->argc = argc;
    context->argv = argv;
    context->status = 127;
    context->finished = 0;
    context->main_thread = pthread_self();
    context->frontend_kind = frontend_kind;
    context->use_shell_context = use_shell_context;
    context->session_stdio = vprocSessionStdioCurrent();
    context->session_vproc = vprocCurrent();
    context->runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext
        ? PSCALRuntimeGetCurrentRuntimeContext()
        : NULL;
#if defined(PSCAL_TARGET_IOS)
    context->suspend_checkpoint = gShellToolSuspendCheckpoint;
#endif
    /* Foreground runs keep the caller's stdio; use -1 to skip dup2. */
    context->stdout_fd = -1;
    context->stderr_fd = -1;
    if (getenv("PSCALI_VPROC_DEBUG")) {
        fprintf(stderr,
                "[tool-thread] spawn fg stdio=%p vproc=%p\n",
                (void *)context->session_stdio,
                (void *)context->session_vproc);
    }
    struct termios stdin_termios;
    bool stdin_termios_valid = shellToolTcgetattr(STDIN_FILENO, &stdin_termios);

    /* Block SIGINT and SIGURG on Main */
    sigset_t new_mask, old_mask;
    sigemptyset(&new_mask);
    sigaddset(&new_mask, SIGINT);
    sigaddset(&new_mask, SIGURG);
    pthread_sigmask(SIG_BLOCK, &new_mask, &old_mask);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, SHELL_TOOL_THREAD_STACK_SZ);
    int create_rc = pthread_create(&context->worker_thread, &attr, shellToolThreadMain, context);
    pthread_attr_destroy(&attr);
    if (create_rc != 0) {
        pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        if (!use_shell_context) {
            int status = entry(argc, argv);
            if (stdin_termios_valid) {
                shellToolTcsetattr(STDIN_FILENO, SHELL_TOOL_RESTORE_ACTION, &stdin_termios);
            }
            free(context);
            return status;
        }
        ShellRuntimeState *ctx = shellRuntimeCreateContext();
        ShellRuntimeState *prev = shellRuntimeActivateContext(ctx);
        int status = entry(argc, argv);
        shellRuntimeActivateContext(prev);
        shellRuntimeDestroyContext(ctx);
        if (stdin_termios_valid) {
            shellToolTcsetattr(STDIN_FILENO, SHELL_TOOL_RESTORE_ACTION, &stdin_termios);
        }
        free(context);
        return status;
    }

    /* Spawn waiter */
    pthread_t waiter_thread;
    if (pthread_create(&waiter_thread, NULL, shellToolWaiterMain, context) != 0) {
        /* If waiter fails, just join worker directly (SIGINT won't work properly but we can't do much) */
        pthread_join(context->worker_thread, NULL);
        pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        if (stdin_termios_valid) {
            shellToolTcsetattr(STDIN_FILENO, SHELL_TOOL_RESTORE_ACTION, &stdin_termios);
        }
        int waiter_failed_status = context->status;
        free(context);
        return waiter_failed_status;
    }

#if defined(PSCAL_TARGET_IOS)
    context->waiter_thread = waiter_thread;
    /* Dedicated SIGINT catcher (sigwait): owns the foreground Ctrl-C. */
    context->sigint_catcher_stop = 0;
    context->sigint_catcher_active =
        (pthread_create(&context->sigint_catcher_thread, NULL,
                        shellToolSigintCatcherMain, context) == 0);
    pthread_cleanup_push(shellToolStarterCancelled, context);
#endif

    /* Main Loop */
    sigset_t wait_mask;
    sigemptyset(&wait_mask);
    sigaddset(&wait_mask, SIGINT);
    sigaddset(&wait_mask, SIGURG);

    while (!context->finished) {
        sigset_t pending;
        sigemptyset(&pending);
#if defined(PSCAL_TARGET_IOS)
        /* SIGINT is handled by the dedicated catcher thread; here only relay a
         * cooperative SIGTSTP request to vproc job control. */
        if (pscalRuntimeConsumeSigtstp()) {
            (void)vprocRequestControlSignal(SIGTSTP);
        }
#else
        if (pscalRuntimeConsumeSigint()) {
            pthread_kill(context->worker_thread, SIGINT);
        }
        if (pscalRuntimeConsumeSigtstp()) {
            (void)vprocRequestControlSignal(SIGTSTP);
        }
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGINT)) {
            pthread_kill(context->worker_thread, SIGINT);
        }
#endif
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 200 * 1000 * 1000; /* 200ms */
        nanosleep(&ts, NULL);
        /* Poll for completion to avoid hanging if SIGURG never arrives. */
    }

#if defined(PSCAL_TARGET_IOS)
    pthread_cleanup_pop(0);
    if (context->sigint_catcher_active) {
        shellToolStopSigintCatcher(context);
    }
#endif
    pthread_join(waiter_thread, NULL);
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    if (stdin_termios_valid) {
        shellToolTcsetattr(STDIN_FILENO, SHELL_TOOL_RESTORE_ACTION, &stdin_termios);
    }
    if (out_frontend_reset) {
        *out_frontend_reset = context->frontend_reset_done;
    }
    int final_status = context->status;
    free(context);
    return final_status;
}

static const int kShellToolRunnerUnavailable = -32768;

static int __attribute__((unused)) shellSpawnToolProcess(const char *tool_name, int argc, char **argv) {
    const char *runner = getenv("PSCALI_TOOL_RUNNER_PATH");
    if (!runner || !*runner) {
        (void)tool_name;
        return kShellToolRunnerUnavailable;
    }
#if defined(PSCAL_TARGET_IOS)
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (session && !vprocSessionStdioIsDefault(session)) {
        return kShellToolRunnerUnavailable;
    }
#endif

    size_t child_count = (size_t)argc + 1; // runner path + tool argv vector
    char **child_argv = (char **)calloc(child_count + 1, sizeof(char *));
    if (!child_argv) {
        fprintf(stderr, "%s: out of memory launching tool runner\n", tool_name);
        return 1;
    }

    child_argv[0] = (char *)runner;
    for (int i = 0; i < argc; ++i) {
        child_argv[i + 1] = argv[i];
    }

    pid_t pid = 0;
    int spawn_rc = posix_spawn(&pid, runner, NULL, NULL, child_argv, environ);
    free(child_argv);
    if (spawn_rc == EPERM || spawn_rc == ENOSYS) {
        return kShellToolRunnerUnavailable;
    }
    if (spawn_rc != 0) {
        fprintf(stderr, "%s: failed to launch tool runner: %s\n", tool_name, strerror(spawn_rc));
        return 127;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 127;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

static Value shellRunToolInEnvironment(VM *vm,
                                       const char *tool_name,
                                       ShellToolEntryFn entry,
                                       FrontendKind target_kind,
                                       int arg_count,
                                       Value *args) {
    if (getenv("PSCALI_PIPE_DEBUG")) {
        fprintf(stderr, "[tool-dispatch] name=%s argc=%d\n", tool_name ? tool_name : "<null>", arg_count + 1);
    }
#if defined(PSCAL_TARGET_IOS)
    bool tool_dbg = getenv("PSCALI_TOOL_DEBUG") != NULL;
    /* Special-case exsh: on iOS we cannot exec a new process, so run exsh_main inline
     * with the provided arguments to preserve -c/script execution. */
    if (tool_name && strcmp(tool_name, "exsh") == 0 && entry) {
        int argc = arg_count + 1;
        char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
        if (!argv) {
            shellUpdateStatus(127);
            return makeVoid();
        }
        argv[0] = strdup("exsh");
        for (int i = 0; i < arg_count; ++i) {
            const Value *v = &args[i];
            const char *s = (VALUE_TYPE(*v) == TYPE_STRING && AS_STRING(*v)) ? AS_STRING(*v) : "";
            argv[i + 1] = strdup(s);
        }
        int status = entry(argc, argv);
        for (int i = 0; i <= argc; ++i) {
            free(argv[i]);
        }
        free(argv);
        shellUpdateStatus(status);
        return makeVoid();
    }
#endif
    FrontendKind previous_kind = frontendPushKind(target_kind);
    if (!entry) {
        runtimeError(vm, "%s: tool entrypoint unavailable", tool_name);
        shellUpdateStatus(127);
        frontendPopKind(previous_kind);
        return makeVoid();
    }

    PascalGlobalState pascal_state;
    pascalPushGlobalState(&pascal_state);

    int saved_param_count = gParamCount;
    char **saved_param_values = gParamValues;
    bool saved_param_owned = gParamValuesOwned;
#if defined(PSCAL_TARGET_IOS)
    ShellToolSuspendCheckpoint suspend_checkpoint;
    shellToolSuspendCheckpointInstall(&suspend_checkpoint,
                                      &pascal_state,
                                      &saved_param_count,
                                      &saved_param_values,
                                      &saved_param_owned);
#endif

    const int argc = arg_count + 1;
    // Add one extra slot so argv[argc] stays NULL like a real C argv vector.
    char **argv = (char **)calloc((size_t)(argc + 1), sizeof(char *));
    if (!argv) {
        runtimeError(vm, "%s: out of memory", tool_name);
        shellUpdateStatus(1);
        pascalPopGlobalState(&pascal_state);
        frontendPopKind(previous_kind);
        return makeVoid();
    }

    bool ok = true;
    argv[0] = strdup(tool_name);
    if (!argv[0]) {
        ok = false;
    }

    for (int i = 0; ok && i < arg_count; ++i) {
        const Value *arg = &args[i];
        if (VALUE_TYPE(*arg) != TYPE_STRING) {
            runtimeError(vm, "%s: expected string arguments", tool_name);
            ok = false;
            break;
        }
        const char *src = AS_STRING(*arg) ? AS_STRING(*arg) : "";
        argv[i + 1] = strdup(src);
        if (!argv[i + 1]) {
            runtimeError(vm, "%s: out of memory", tool_name);
            ok = false;
        }
    }

    if (ok) {
        int status = kShellToolRunnerUnavailable;
#if !defined(PSCAL_TARGET_IOS)
        status = shellSpawnToolProcess(tool_name, argc, argv);
#endif
        if (status == kShellToolRunnerUnavailable && entry) {
            bool needs_shell_ctx = (target_kind == FRONTEND_KIND_SHELL);
            bool background = shellRuntimeCurrentBuiltinBackground();
#if defined(PSCAL_TARGET_IOS)
            if (getenv("PSCALI_PIPE_DEBUG")) {
                fprintf(stderr, "[tool-dispatch] name=%s bg=%d worker=%d argc=%d\n",
                    tool_name ? tool_name : "<null>", (int)background, (int)gShellBackgroundWorkerActive, argc);
            }
            if (background && gShellBackgroundWorkerActive) {
                if (tool_dbg) {
                    fprintf(stderr, "[tool-inline] %s argc=%d (bg worker)\n", tool_name, argc);
                    for (int i = 0; i < argc; ++i) {
                        fprintf(stderr, "[tool-inline] argv[%d]=%s\n", i, argv[i] ? argv[i] : "(null)");
                    }
                }
                /* Already running inside a background worker: execute inline so redirections
                 * remain active for this thread. */
                bool frontend_reset = false;
                status = shellRunToolInlineWithRestore(entry, argc, argv, tool_dbg, target_kind, &frontend_reset);
                if (tool_dbg) {
                    fprintf(stderr, "[tool-inline] %s exit=%d\n", tool_name, status);
                }
            } else if (background || target_kind == FRONTEND_KIND_SHELL) {
                if (tool_dbg) {
                    fprintf(stderr, "[tool-thread] %s argc=%d bg=%d\n", tool_name, argc, (int)background);
                }
                bool frontend_reset = false;
                status = shellRunToolInThread(entry, argc, argv, target_kind, needs_shell_ctx, background, &frontend_reset);
                if (tool_dbg) {
                    fprintf(stderr, "[tool-thread] %s dispatched status=%d\n", tool_name, status);
                }
                if (!frontend_reset) {
                    shellInvalidateFrontendState(target_kind);
                }
            } else {
                /* Foreground non-shell frontends run on a dedicated worker so
                 * Ctrl-Z can stop/resume the frontend vproc (fg/bg) without
                 * parking the shell execution thread. */
                bool frontend_reset = false;
                status = shellRunToolInThread(entry, argc, argv, target_kind, needs_shell_ctx, false, &frontend_reset);
                if (!frontend_reset) {
                    shellInvalidateFrontendState(target_kind);
                }
            }
            /* For shell frontend, always reset in the caller because the shell
             * runtime structures are persistent. */
            if (target_kind == FRONTEND_KIND_SHELL) {
                shellInvalidateFrontendState(target_kind);
            }
            /* The frontend reset tears down its own symbol tables; reattach the saved
             * shell tables so pascalPopGlobalState does not attempt to free
             * already-destroyed structures. */
            globalSymbols = pascal_state.globalSymbols;
            constGlobalSymbols = pascal_state.constGlobalSymbols;
            localSymbols = pascal_state.localSymbols;
            procedure_table = pascal_state.procedure_table;
            current_procedure_table = pascal_state.current_procedure_table;
            type_table = pascal_state.type_table;
            /* For inline/background runs, return the actual worker status so the shell
             * prompt reflects tool success/failure. */
            gShellRuntime.last_status = status;
#else
            if (needs_shell_ctx || background) {
                status = shellRunToolInThread(entry, argc, argv, target_kind, needs_shell_ctx, background, NULL);
            } else {
                status = entry(argc, argv);
            }
            pascalInvalidateGlobalState();
#endif
        } else if (status == kShellToolRunnerUnavailable) {
            fprintf(stderr, "%s: tool runner unavailable and no builtin entry.\n", tool_name);
            status = 127;
        }
        fflush(NULL);
        shellUpdateStatus(status);
    } else {
        shellUpdateStatus(1);
    }

    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }
    free(argv);

#if defined(PSCAL_TARGET_IOS)
    shellToolSuspendCheckpointClear(&suspend_checkpoint);
#endif
    gParamCount = saved_param_count;
    gParamValues = saved_param_values;
    gParamValuesOwned = saved_param_owned;
    pascalPopGlobalState(&pascal_state);

    Value result = makeVoid();
    /* For synchronous iOS runs, the worker thread will set gShellRuntime.last_status.
     * Use that value when available to avoid a default 127 on success. */
    int final_status = shellRuntimeLastStatus();
    shellUpdateStatus(final_status);
    if (getenv("PSCALI_PIPE_DEBUG")) {
        fprintf(stderr, "[tool-dispatch] %s final status=%d\n", tool_name ? tool_name : "<null>", final_status);
    }
    frontendPopKind(previous_kind);
    return result;
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} ShellStringVector;

static void shellStringVectorFree(ShellStringVector *vec) {
    if (!vec) {
        return;
    }
    for (size_t i = 0; i < vec->count; ++i) {
        free(vec->items[i]);
    }
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

static bool shellStringVectorPush(ShellStringVector *vec, const char *value) {
    if (!vec || !value) {
        return false;
    }
    if (vec->count == vec->capacity) {
        size_t new_cap = vec->capacity ? vec->capacity * 2 : 32;
        char **resized = (char **)realloc(vec->items, new_cap * sizeof(char *));
        if (!resized) {
            return false;
        }
        vec->items = resized;
        vec->capacity = new_cap;
    }
    vec->items[vec->count] = strdup(value);
    if (!vec->items[vec->count]) {
        return false;
    }
    vec->count++;
    return true;
}

static int shellLsCompare(const void *a, const void *b) {
    const char *const *lhs = (const char *const *)a;
    const char *const *rhs = (const char *const *)b;
    return strcasecmp(*lhs, *rhs);
}

static int shellBuiltinListDirectory(const char *path, bool show_all, bool show_almost_all) {
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return errno ? errno : 1;
    }

    ShellStringVector entries = {0};
    errno = 0;
    struct dirent *dent;
    while ((dent = readdir(dir)) != NULL) {
        if (dent->d_name[0] == '.') {
            if (!show_all) {
                if (!show_almost_all) {
                    continue;
                }
                if (strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0) {
                    continue;
                }
            }
        }
        if (!shellStringVectorPush(&entries, dent->d_name)) {
            errno = ENOMEM;
            break;
        }
    }
    int err = errno;
    closedir(dir);

    if (err && err != 0) {
        shellStringVectorFree(&entries);
        fprintf(stderr, "ls: %s: %s\n", path, strerror(err));
        return err;
    }

    qsort(entries.items, entries.count, sizeof(char *), shellLsCompare);
    for (size_t i = 0; i < entries.count; ++i) {
        printf("%s\n", entries.items[i]);
    }
    shellStringVectorFree(&entries);
    return 0;
}

static int shellBuiltinListPath(const char *path, bool show_all, bool show_almost_all, bool print_header, bool needs_spacing) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return errno ? errno : 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        printf("%s\n", path);
        return 0;
    }

    if (print_header) {
        if (needs_spacing) {
            printf("\n");
        }
        printf("%s:\n", path);
    }
    return shellBuiltinListDirectory(path, show_all, show_almost_all);
}

Value vmBuiltinShellLs(VM *vm, int arg_count, Value *args) {
    bool show_all = false;
    bool show_almost_all = false;
    int index = 0;
    while (index < arg_count) {
        if (VALUE_TYPE(args[index]) != TYPE_STRING || !AS_STRING(args[index])) {
            break;
        }
        const char *word = AS_STRING(args[index]);
        if (word[0] != '-') {
            break;
        }
        if (strcmp(word, "--") == 0) {
            index++;
            break;
        }
        for (const char *opt = word + 1; *opt; ++opt) {
            if (*opt == 'a') {
                show_all = true;
                show_almost_all = false;
            } else if (*opt == 'A') {
                if (!show_all) {
                    show_almost_all = true;
                }
            } else {
                runtimeError(vm, "ls: unsupported option -%c", *opt);
                shellUpdateStatus(1);
                return makeVoid();
            }
        }
        index++;
    }

    int path_count = arg_count - index;
    const char *default_path = ".";
    const char **paths = NULL;
    if (path_count <= 0) {
        path_count = 1;
        paths = &default_path;
    } else {
        paths = (const char **)malloc(sizeof(const char *) * (size_t)path_count);
        if (!paths) {
            runtimeError(vm, "ls: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        for (int i = 0; i < path_count; ++i) {
            const Value *arg = &args[index + i];
            if (VALUE_TYPE(*arg) != TYPE_STRING || !AS_STRING(*arg)) {
                runtimeError(vm, "ls: expected string path arguments");
                free(paths);
                shellUpdateStatus(1);
                return makeVoid();
            }
            paths[i] = AS_STRING(*arg);
        }
    }

    int final_status = 0;
    for (int i = 0; i < path_count; ++i) {
        int rc = shellBuiltinListPath(paths[i], show_all, show_almost_all, path_count > 1, i > 0);
        if (rc != 0 && final_status == 0) {
            final_status = rc;
        }
    }
    if (paths != &default_path) {
        free(paths);
    }
    shellUpdateStatus(final_status);
    return makeVoid();
}

extern int pascal_main(int argc, char *argv[]);
extern int clike_main(int argc, char *argv[]);
extern int rea_main(int argc, char *argv[]);
extern int aether_main(int argc, char *argv[]);
extern int pscalvm_main(int argc, char *argv[]);
extern int pscaljson2bc_main(int argc, char **argv);
extern int exsh_main(int argc, char *argv[]);
#ifdef BUILD_DASCAL
extern int dascal_main(int argc, char *argv[]);
#endif
#ifdef BUILD_PSCALD
extern int pscald_main(int argc, char *argv[]);
extern int pscalasm_main(int argc, char *argv[]);
#endif

/* A frontend is a program, with an environment of its own on iOS. */
static Value shellRunToolBuiltin(VM *vm,
                                 const char *tool_name,
                                 ShellToolEntryFn entry,
                                 FrontendKind target_kind,
                                 int arg_count,
                                 Value *args) {
#if defined(PSCAL_TARGET_IOS)
    ShellProgramEnvScope program_env;
    shellProgramEnvEnter(&program_env, NULL);
    /* A shell runs right here with no process of its own, so it needs what a new one has. */
    bool is_shell = target_kind == FRONTEND_KIND_SHELL;
    ShellProgramScope program_scope;
    if (is_shell) {
        shellProgramScopeEnter(&program_scope);
    }
    Value result = shellRunToolInEnvironment(vm, tool_name, entry, target_kind, arg_count, args);
    int status = gShellRuntime.last_status;
    if (is_shell) {
        shellProgramScopeLeave(&program_scope);
    }
    shellProgramEnvLeave(&program_env);
    shellUpdateStatus(status);
    return result;
#else
    return shellRunToolInEnvironment(vm, tool_name, entry, target_kind, arg_count, args);
#endif
}

Value vmBuiltinShellPascal(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "pascal", pascal_main, FRONTEND_KIND_PASCAL, arg_count, args);
}

#ifdef BUILD_DASCAL
Value vmBuiltinShellDascal(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "dascal", dascal_main, FRONTEND_KIND_PASCAL, arg_count, args);
}
#endif

Value vmBuiltinShellClike(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "clike", clike_main, FRONTEND_KIND_CLIKE, arg_count, args);
}

Value vmBuiltinShellRea(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "rea", rea_main, FRONTEND_KIND_REA, arg_count, args);
}

Value vmBuiltinShellAether(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "aether", aether_main, FRONTEND_KIND_AETHER, arg_count, args);
}

Value vmBuiltinShellPscalVm(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "pscalvm", pscalvm_main, FRONTEND_KIND_PASCAL, arg_count, args);
}

Value vmBuiltinShellPscalJson2bc(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "pscaljson2bc", pscaljson2bc_main, FRONTEND_KIND_PASCAL, arg_count, args);
}

Value vmBuiltinShellExshTool(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "exsh", exsh_main, FRONTEND_KIND_SHELL, arg_count, args);
}

#ifdef BUILD_PSCALD
Value vmBuiltinShellPscald(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "pscald", pscald_main, FRONTEND_KIND_PASCAL, arg_count, args);
}

Value vmBuiltinShellPscalasm(VM *vm, int arg_count, Value *args) {
    return shellRunToolBuiltin(vm, "pscalasm", pscalasm_main, FRONTEND_KIND_PASCAL, arg_count, args);
}
#endif

#endif /* PSCAL_TARGET_IOS */

static int shellFlushBuiltinOutput(VM *vm, const char *name);

/* pushd and popd have moved by now: bash reports a failed write and still succeeds. */
static void shellDirectoryStackReport(VM *vm, const char *name) {
    shellDirectoryStackPrint();
    (void)shellFlushBuiltinOutput(vm, name);
}

Value vmBuiltinShellDirs(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count > 0) {
        runtimeError(vm, "dirs: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (!shellDirectoryStackEnsureInitialised(vm, "dirs")) {
        return makeVoid();
    }
    shellDirectoryStackPrint();
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellPushd(VM *vm, int arg_count, Value *args) {
    if (!shellDirectoryStackEnsureInitialised(vm, "pushd")) {
        return makeVoid();
    }

    if (arg_count > 1) {
        runtimeError(vm, "pushd: too many arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (arg_count == 0) {
        if (gShellRuntime.dir_stack_count < 2) {
            runtimeError(vm, "pushd: no other directory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        char *new_cwd = NULL;
        char *old_cwd = NULL;
        if (!shellDirectoryStackChdir(vm, "pushd", gShellRuntime.dir_stack[1], &new_cwd, &old_cwd)) {
            return makeVoid();
        }
        char *old_top = gShellRuntime.dir_stack[0];
        char *second = gShellRuntime.dir_stack[1];
        gShellRuntime.dir_stack[0] = new_cwd;
        gShellRuntime.dir_stack[1] = old_top;
        free(second);
        gShellRuntime.dir_stack_initialised = true;
        shellDirectoryStackUpdateEnvironment(old_cwd, gShellRuntime.dir_stack[0]);
        free(old_cwd);
        shellDirectoryStackReport(vm, "pushd");
        shellUpdateStatus(0);
        return makeVoid();
    }

    if (VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0])) {
        runtimeError(vm, "pushd: expected directory path");
        shellUpdateStatus(1);
        return makeVoid();
    }

    size_t needed = gShellRuntime.dir_stack_count + 1;
    if (!shellDirectoryStackEnsureCapacity(needed)) {
        runtimeError(vm, "pushd: out of memory");
        shellUpdateStatus(1);
        return makeVoid();
    }

    char *new_cwd = NULL;
    char *old_cwd = NULL;
    if (!shellDirectoryStackChdir(vm, "pushd", AS_STRING(args[0]), &new_cwd, &old_cwd)) {
        return makeVoid();
    }

    size_t count = gShellRuntime.dir_stack_count;
    memmove(&gShellRuntime.dir_stack[1], &gShellRuntime.dir_stack[0], count * sizeof(char *));
    gShellRuntime.dir_stack[0] = new_cwd;
    gShellRuntime.dir_stack_count = count + 1;
    gShellRuntime.dir_stack_initialised = true;
    shellDirectoryStackUpdateEnvironment(old_cwd, gShellRuntime.dir_stack[0]);
    free(old_cwd);
    shellDirectoryStackReport(vm, "pushd");
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellPopd(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (!shellDirectoryStackEnsureInitialised(vm, "popd")) {
        return makeVoid();
    }
    if (arg_count > 0) {
        runtimeError(vm, "popd: unexpected arguments");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (gShellRuntime.dir_stack_count <= 1) {
        runtimeError(vm, "popd: directory stack empty");
        shellUpdateStatus(1);
        return makeVoid();
    }

    char *new_cwd = NULL;
    char *old_cwd = NULL;
    if (!shellDirectoryStackChdir(vm, "popd", gShellRuntime.dir_stack[1], &new_cwd, &old_cwd)) {
        return makeVoid();
    }

    size_t count = gShellRuntime.dir_stack_count;
    char *removed = gShellRuntime.dir_stack[0];
    char *target_entry = gShellRuntime.dir_stack[1];
    free(removed);
    memmove(&gShellRuntime.dir_stack[0], &gShellRuntime.dir_stack[1], (count - 1) * sizeof(char *));
    gShellRuntime.dir_stack_count = count - 1;
    gShellRuntime.dir_stack[gShellRuntime.dir_stack_count] = NULL;
    free(target_entry);
    gShellRuntime.dir_stack[0] = new_cwd;
    gShellRuntime.dir_stack_initialised = true;
    shellDirectoryStackUpdateEnvironment(old_cwd, gShellRuntime.dir_stack[0]);
    free(old_cwd);
    shellDirectoryStackReport(vm, "popd");
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellFinger(VM *vm, int arg_count, Value *args) {
    const char *target_user = NULL;
    if (arg_count > 1) {
        runtimeError(vm, "finger: expected at most one username");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (arg_count == 1) {
        if (VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0]) || !*AS_STRING(args[0])) {
            runtimeError(vm, "finger: expected username as string");
            shellUpdateStatus(1);
            return makeVoid();
        }
        target_user = AS_STRING(args[0]);
    }

    struct passwd *pw = NULL;
    if (target_user) {
        pw = getpwnam(target_user);
        if (!pw) {
            runtimeError(vm, "finger: user '%s' not found", target_user);
            shellUpdateStatus(1);
            return makeVoid();
        }
    } else {
        pw = getpwuid(getuid());
        if (!pw) {
            runtimeError(vm, "finger: unable to determine current user");
            shellUpdateStatus(1);
            return makeVoid();
        }
    }

    const char *login = (pw->pw_name && *pw->pw_name) ? pw->pw_name : "(unknown)";
    const char *gecos = (pw->pw_gecos && *pw->pw_gecos) ? pw->pw_gecos : "";
    const char *directory = (pw->pw_dir && *pw->pw_dir) ? pw->pw_dir : "(unknown)";
    const char *shell_path = (pw->pw_shell && *pw->pw_shell) ? pw->pw_shell : "(unknown)";

    char name_buffer[256];
    const char *display_name = gecos;
    if (*display_name) {
        const char *comma = strchr(display_name, ',');
        if (comma) {
            size_t copy_len = (size_t)(comma - display_name);
            if (copy_len >= sizeof(name_buffer)) {
                copy_len = sizeof(name_buffer) - 1;
            }
            memcpy(name_buffer, display_name, copy_len);
            name_buffer[copy_len] = '\0';
            display_name = name_buffer;
        }
    } else {
        display_name = "(unknown)";
    }

    printf("Login: %s\tName: %s\n", login, display_name);
    printf("Directory: %s\n", directory);
    printf("Shell: %s\n", shell_path);
    fflush(stdout);

    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellColon(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellUpdateStatus(0);
    return makeVoid();
}

static int shellEchoHexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static void shellEchoWriteString(const char *text,
                                 bool interpret,
                                 bool *stop_output,
                                 bool *emit_newline) {
    if (!text || !stop_output || !emit_newline) {
        return;
    }
    const char *cursor = text;
    while (!*stop_output && cursor && *cursor) {
        char ch = *cursor++;
        if (interpret && ch == '\\') {
            char esc = *cursor;
            if (esc == '\0') {
                fputc('\\', stdout);
                break;
            }
            cursor++;
            switch (esc) {
                case 'a':
                    fputc('\a', stdout);
                    break;
                case 'b':
                    fputc('\b', stdout);
                    break;
                case 'c':
                    *emit_newline = false;
                    *stop_output = true;
                    return;
                case 'e':
                case 'E':
                    fputc(27, stdout);
                    break;
                case 'f':
                    fputc('\f', stdout);
                    break;
                case 'n':
                    fputc('\n', stdout);
                    break;
                case 'r':
                    fputc('\r', stdout);
                    break;
                case 't':
                    fputc('\t', stdout);
                    break;
                case 'v':
                    fputc('\v', stdout);
                    break;
                case '\\':
                    fputc('\\', stdout);
                    break;
                case '0': {
                    unsigned char value = 0;
                    int digits = 0;
                    while (digits < 3 && *cursor >= '0' && *cursor <= '7') {
                        value = (unsigned char)((value << 3) | (*cursor - '0'));
                        cursor++;
                        digits++;
                    }
                    fputc((char)value, stdout);
                    break;
                }
                case 'x': {
                    unsigned char value = 0;
                    int digits = 0;
                    while (digits < 2) {
                        int hex = shellEchoHexDigit(*cursor);
                        if (hex < 0) {
                            break;
                        }
                        value = (unsigned char)((value << 4) | (unsigned char)hex);
                        cursor++;
                        digits++;
                    }
                    if (digits == 0) {
                        fputs("\\x", stdout);
                    } else {
                        fputc((char)value, stdout);
                    }
                    break;
                }
                default:
                    fputc('\\', stdout);
                    fputc(esc, stdout);
                    break;
            }
        } else {
            fputc(ch, stdout);
        }
    }
}

/* `echo a >&-`: the write fails, the command does, and the text goes with it. */
static int shellFlushBuiltinOutput(VM *vm, const char *name) {
    /* glibc drops the text at the write that failed and keeps only the stream's flag, so the
     * flush after it succeeds. */
    if (fflush(stdout) == 0 && !ferror(stdout)) {
        return 0;
    }
    /* A reader that left would have ended a process with no message. The shell ignores
     * the signal for itself and a stage on a thread cannot take it, so it looks the same
     * from here. */
    int status = 128 + SIGPIPE;
    if (errno != EPIPE) {
        shellReportRecoverableError(vm, true, "%s: write error: %s", name, strerror(errno));
        status = 1;
    }
#if defined(__GLIBC__)
    __fpurge(stdout);
#elif defined(__APPLE__)
    fpurge(stdout);
#endif
    clearerr(stdout);
    return status;
}

/* Bash does this after every builtin that prints. Left in the buffer, the text came out at the
 * next flush, wherever stdout was by then. */
static void shellCheckBuiltinOutput(VM *vm, const char *name) {
    int status = shellFlushBuiltinOutput(vm, name);
    if (status != 0) {
        shellUpdateStatus(status);
    }
}

Value vmBuiltinShellEcho(VM *vm, int arg_count, Value *args) {
    bool emit_newline = true;
    bool interpret_escapes = false;
    bool parsing_options = true;
    int first_arg = 0;

    for (int i = 0; i < arg_count; ++i) {
        if (!parsing_options) {
            first_arg = i;
            break;
        }
        Value val = args[i];
        const char *token = (VALUE_TYPE(val) == TYPE_STRING && AS_STRING(val)) ? AS_STRING(val) : "";
        if (!token) {
            token = "";
        }
        if (token[0] == '-' && token[1] != '\0') {
            bool valid = true;
            bool next_emit_newline = emit_newline;
            bool next_interpret = interpret_escapes;
            for (size_t j = 1; token[j] != '\0'; ++j) {
                char opt = token[j];
                if (opt == 'n') {
                    next_emit_newline = false;
                } else if (opt == 'e') {
                    next_interpret = true;
                } else if (opt == 'E') {
                    next_interpret = false;
                } else {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                emit_newline = next_emit_newline;
                interpret_escapes = next_interpret;
                first_arg = i + 1;
                continue;
            }
        }
        parsing_options = false;
        first_arg = i;
        break;
    }
    if (parsing_options) {
        first_arg = arg_count;
    }

    /* echo -- prints the two dashes: it has no end-of-options marker. */
    bool stop_output = false;
    for (int i = first_arg; i < arg_count && !stop_output; ++i) {
        Value val = args[i];
        if (i > first_arg) {
            fputc(' ', stdout);
        }
        const char *text = (VALUE_TYPE(val) == TYPE_STRING && AS_STRING(val)) ? AS_STRING(val) : "";
        shellEchoWriteString(text, interpret_escapes, &stop_output, &emit_newline);
    }

    if (emit_newline && !stop_output) {
        fputc('\n', stdout);
    }

    shellUpdateStatus(shellFlushBuiltinOutput(vm, "echo"));
    return makeVoid();
}

Value vmBuiltinShellTrue(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellFalse(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellUpdateStatus(1);
    return makeVoid();
}

Value vmBuiltinShellSource(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0])) {
        runtimeError(vm, "source: expected path to script");
        shellUpdateStatus(1);
        return makeVoid();
    }
    const char *path = AS_STRING(args[0]);
    const char *base = strrchr(path, '/');
    base = base ? (base + 1) : path;
    if (base && strcmp(base, ".exshrc.local") == 0 && access(path, F_OK) != 0) {
        /* ~/.exshrc.local is an optional per-device overlay; missing file should
         * not spam startup sessions that source it unconditionally. */
        shellUpdateStatus(0);
        return makeVoid();
    }
    /* A name with no slash is looked for on PATH, and then here. */
    char found_path[PATH_MAX];
    if (!strchr(path, '/')) {
        const char *search = shellSafeGetenv("PATH");
        while (search && *search) {
            const char *colon = strchr(search, ':');
            size_t n = colon ? (size_t)(colon - search) : strlen(search);
            int written = snprintf(found_path, sizeof(found_path), "%.*s/%s", (int)n, n ? search : ".", path);
            struct stat st;
            if (written > 0 && (size_t)written < sizeof(found_path) && stat(found_path, &st) == 0 &&
                S_ISREG(st.st_mode) && access(found_path, R_OK) == 0) {
                path = found_path;
                break;
            }
            search = colon ? colon + 1 : NULL;
        }
    }
    char *source = shellLoadFile(path);
    if (!source) {
        runtimeError(vm, "source: unable to read '%s'", path);
        shellUpdateStatus(1);
        return makeVoid();
    }

    char **saved_params = gParamValues;
    int saved_count = gParamCount;
    bool saved_owned = gShellPositionalOwned;

    int new_count = (arg_count > 1) ? (arg_count - 1) : 0;
    char **new_params = NULL;
    bool replaced_params = false;
    if (new_count > 0) {
        new_params = (char **)calloc((size_t)new_count, sizeof(char *));
        if (!new_params) {
            free(source);
            runtimeError(vm, "source: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        for (int i = 0; i < new_count; ++i) {
            if (VALUE_TYPE(args[i + 1]) != TYPE_STRING || !AS_STRING(args[i + 1])) {
                shellFreeParameterArray(new_params, i);
                free(source);
                runtimeError(vm, "source: arguments must be strings");
                shellUpdateStatus(1);
                return makeVoid();
            }
            new_params[i] = strdup(AS_STRING(args[i + 1]));
            if (!new_params[i]) {
                shellFreeParameterArray(new_params, i);
                free(source);
                runtimeError(vm, "source: out of memory");
                shellUpdateStatus(1);
                return makeVoid();
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
    const char *frontend_path = shellRuntimeGetArg0();
    opts.frontend_path = frontend_path ? frontend_path : "exsh";
    opts.exit_on_signal = shellRuntimeExitOnSignal();

    bool exit_requested = false;
    /* What it sets stays set, as for eval: `. lib.sh` with `set -e` in it. */
    opts.share_runtime_context = true;
    opts.suppress_warnings = true;
    ShellPipelineContext *caller_pipeline = &gShellRuntime.pipeline;
    ShellPipelineContext saved_pipeline = *caller_pipeline;
    bool pipeline_saved = caller_pipeline->stage_count <= 1;
    if (pipeline_saved) {
        memset(caller_pipeline, 0, sizeof(*caller_pipeline));
        caller_pipeline->pgid = -1;
    }
    bool frame_pushed = shellRuntimePushCallFrame("source", path, gShellCurrentCommandLine, false);
    int condition_depth = gShellConditionDepth;
    size_t loop_depth = gShellLoopStackSize;
    size_t case_depth = gShellCaseStack.count;
    bool err_trap_at_call = shellTrapIsLive(&gShellErrTrap);
    bool saved_trap_status_valid = gShellTrapStatusValid;
    gShellTrapStatusValid = false;
    gShellReturnScopeDepth++;
    gShellTextDepth++;
    int status = shellRunSource(source, path, &opts, &exit_requested);
    gShellTextDepth--;
    shellUnwindChunk(loop_depth, case_depth);
    bool returned = gShellReturnPending;
    if (returned || gShellDiscardPending) {
        gShellConditionDepth = condition_depth;
    }
    gShellReturnPending = false;
    if (!exit_requested && !gShellDiscardPending) {
        shellRuntimeRunReturnTrap(returned, status);
    }
    gShellReturnScopeDepth--;
    gShellTrapStatusValid = saved_trap_status_valid;
    if (pipeline_saved) {
        if (!shellRuntimeChunkLeftStages()) {
            shellResetPipeline();
        }
        *caller_pipeline = saved_pipeline;
    }
    if (frame_pushed) {
        shellRuntimePopCallFrame();
    }
    free(source);

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

    if (exit_requested) {
        shellRuntimeRunExitTrapInPlace();
        shellRuntimeRequestExit();
        if (vm) {
            vm->exit_requested = true;
        }
    }
    if (gShellDiscardPending && vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = "return";
    }

    shellUpdateStatus(exit_requested ? shellRuntimeLastStatus() : status);
    gShellErrTrapSkipOnce = !err_trap_at_call;
    return makeVoid();
}

Value vmBuiltinShellEval(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        shellUpdateStatus(0);
        return makeVoid();
    }

    size_t total_len = 0;
    for (int i = 0; i < arg_count; ++i) {
        if (VALUE_TYPE(args[i]) != TYPE_STRING || !AS_STRING(args[i])) {
            runtimeError(vm, "eval: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        total_len += strlen(AS_STRING(args[i]));
        if (i + 1 < arg_count) {
            total_len += 1; // Space separator.
        }
    }

    char *script = (char *)malloc(total_len + 1);
    if (!script) {
        runtimeError(vm, "eval: out of memory");
        shellUpdateStatus(1);
        return makeVoid();
    }

    char *cursor = script;
    for (int i = 0; i < arg_count; ++i) {
        size_t len = strlen(AS_STRING(args[i]));
        memcpy(cursor, AS_STRING(args[i]), len);
        cursor += len;
        if (i + 1 < arg_count) {
            *cursor++ = ' ';
        }
    }
    *cursor = '\0';

    ShellRunOptions opts = {0};
    opts.no_cache = 1;
    opts.quiet = true;
    /* A name a script defines elsewhere is not worth a warning, here or under bash. */
    opts.suppress_warnings = true;
    const char *frontend_path = shellRuntimeGetArg0();
    opts.frontend_path = frontend_path ? frontend_path : "exsh";
    opts.exit_on_signal = shellRuntimeExitOnSignal();

    ShellSymbolTableScope table_scope;
    shellSymbolTableScopeInit(&table_scope);
    bool scope_pushed = false;
    bool can_run = true;
    if (!shellSymbolTableScopeIsActive()) {
        if (!shellSymbolTableScopePush(&table_scope)) {
            fprintf(stderr, "shell: failed to allocate symbol tables.\n");
            can_run = false;
        } else {
            scope_pushed = true;
        }
    }

    /* Its loops, options and locals are the caller's: `eval break`, `eval 'set -e'`. The
     * pipeline that called it is put aside, as for a function. */
    opts.share_runtime_context = true;
    ShellPipelineContext *caller_pipeline = &gShellRuntime.pipeline;
    ShellPipelineContext saved_pipeline = *caller_pipeline;
    bool pipeline_saved = caller_pipeline->stage_count <= 1;
    if (pipeline_saved) {
        memset(caller_pipeline, 0, sizeof(*caller_pipeline));
        caller_pipeline->pgid = -1;
    }
    bool exit_requested = false;
    int status = EXIT_FAILURE;
    bool err_trap_at_call = shellTrapIsLive(&gShellErrTrap);
    if (can_run || shellSymbolTableScopeIsActive()) {
        opts.first_line = gShellCurrentCommandLine;
        size_t loop_depth = gShellLoopStackSize;
        size_t case_depth = gShellCaseStack.count;
        gShellTextDepth++;
        status = shellRunSource(script, "<eval>", &opts, &exit_requested);
        gShellTextDepth--;
        shellUnwindChunk(loop_depth, case_depth);
    }
    if (pipeline_saved) {
        if (!shellRuntimeChunkLeftStages()) {
            shellResetPipeline();
        }
        *caller_pipeline = saved_pipeline;
    }

    if (scope_pushed) {
        shellSymbolTableScopePop(&table_scope);
    }

    free(script);

    if (!can_run && !shellSymbolTableScopeIsActive()) {
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (exit_requested) {
        shellRuntimeRunExitTrapInPlace();
        shellRuntimeRequestExit();
        if (vm) {
            vm->exit_requested = true;
        }
        shellUpdateStatus(shellRuntimeLastStatus());
        return makeVoid();
    }

    if (gShellReturnPending || gShellDiscardPending) {
        /* `return` in the text: the caller's function ends too. */
        shellUpdateStatus(gShellReturnPending ? gShellReturnStatus : shellRuntimeLastStatus());
        if (vm) {
            vm->exit_requested = true;
            vm->current_builtin_name = "return";
        }
        return makeVoid();
    }

    shellUpdateStatus(status);
    gShellErrTrapSkipOnce = !err_trap_at_call;
    return makeVoid();
}

Value vmBuiltinShellLet(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        runtimeError(vm, "let: expected expression");
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool ok = true;
    long long last_value = 0;
    for (int i = 0; i < arg_count; ++i) {
        Value value = args[i];
        if (VALUE_TYPE(value) != TYPE_STRING || !AS_STRING(value)) {
            runtimeError(vm, "let: arguments must be strings");
            shellMarkArithmeticError();
            ok = false;
            break;
        }
        long long expr_value = 0;
        if (!shellLetEvaluateExpression(AS_STRING(value), &expr_value)) {
            ok = false;
            break;
        }
        last_value = expr_value;
    }

    if (ok) {
        shellUpdateStatus(last_value != 0 ? 0 : 1);
    } else {
        shellUpdateStatus(1);
    }
    return makeVoid();
}

Value vmBuiltinShellExit(VM *vm, int arg_count, Value *args) {
    int code = shellRuntimeLastStatus();
    if (arg_count >= 1) {
        Value arg = args[0];
        if (IS_INTLIKE(arg)) {
            code = (int)AS_INTEGER(arg);
        } else if (VALUE_TYPE(arg) == TYPE_STRING && AS_STRING(arg)) {
            if (*AS_STRING(arg) == '\0') {
                code = 0;
            } else {
                int parsed = 0;
                if (shellParseReturnStatus(AS_STRING(arg), &parsed)) {
                    code = parsed;
                } else {
                    fprintf(stderr, "exit: %s: numeric argument required\n", AS_STRING(arg));
                    code = 2;
                }
            }
        } else {
            fprintf(stderr, "exit: numeric argument required\n");
            code = 2;
        }
    }
    shellUpdateStatus(code);
    shellRuntimeRunExitTrapInPlace();
    shellRuntimeRequestExit();
    if (vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = "exit";
    }
    return makeVoid();
}

Value vmBuiltinShellLogout(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);

    if (!shellShoptOptionEnabled("login_shell")) {
        fprintf(stderr, "logout: not login shell: use 'exit'\n");
        shellUpdateStatus(1);
        shellRestoreCurrentVm(previous_vm);
        return makeVoid();
    }

    if (arg_count > 1) {
        fprintf(stderr, "logout: too many arguments\n");
        shellUpdateStatus(1);
        shellRestoreCurrentVm(previous_vm);
        return makeVoid();
    }

    int status = 0;
    if (arg_count == 1) {
        Value v = args[0];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            fprintf(stderr, "logout: status must be a numeric string\n");
            shellUpdateStatus(1);
            shellRestoreCurrentVm(previous_vm);
            return makeVoid();
        }
        int parsed = 0;
        if (!shellParseReturnStatus(AS_STRING(v), &parsed)) {
            fprintf(stderr, "logout: invalid status '%s'\n", AS_STRING(v));
            shellUpdateStatus(1);
            shellRestoreCurrentVm(previous_vm);
            return makeVoid();
        }
        status = parsed;
    }

    shellUpdateStatus(status);
    shellRuntimeRequestExit();
    if (vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = "logout";
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellExecCommand(VM *vm, int arg_count, Value *args) {
    ShellCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.pipeline_index = -1;

    size_t total_args = (arg_count > 0) ? (size_t)arg_count : 0;
    cmd.argv = (char **)calloc(total_args + 2, sizeof(char *));
    if (!cmd.argv) {
        runtimeError(vm, "exec: out of memory");
        shellUpdateStatus(1);
        return makeVoid();
    }

    cmd.argv[0] = strdup("exec");
    if (!cmd.argv[0]) {
        free(cmd.argv);
        runtimeError(vm, "exec: out of memory");
        shellUpdateStatus(1);
        return makeVoid();
    }
    cmd.argc = 1;
    cmd.argv[cmd.argc] = NULL;

    bool ok = true;
    for (int i = 0; i < arg_count && ok; ++i) {
        Value val = args[i];
        if (VALUE_TYPE(val) != TYPE_STRING || !AS_STRING(val)) {
            runtimeError(vm, "exec: arguments must be strings");
            shellUpdateStatus(1);
            ok = false;
            break;
        }
        char *copy = strdup(AS_STRING(val));
        if (!copy) {
            runtimeError(vm, "exec: out of memory");
            shellUpdateStatus(1);
            ok = false;
            break;
        }
        cmd.argv[cmd.argc++] = copy;
        cmd.argv[cmd.argc] = NULL;
    }

    /* `command exec cmd` and `builtin exec cmd` come this way. */
    if (ok && shellExecRunsAsCommand(&cmd)) {
        return shellRunExecAsCommand(vm, &cmd);
    }
    if (ok) {
        if (!shellExecuteExecBuiltin(vm, &cmd)) {
            shellUpdateStatus(1);
        }
    }

    shellFreeCommand(&cmd);
    return makeVoid();
}

/* What bash does with a command it cannot go on with: all that is running goes, down to what
 * the shell last read, and it reads on. In eval, source and a function that is several chunks,
 * and each one's caller stops too. */
static void shellRuntimeDiscardCommand(VM *vm) {
    shellUpdateStatus(2);
    gShellDiscardPending = true;
    if (vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = "return";
    }
}

static int shellSkipDoubleDash(int arg_count, Value *args) {
    return (arg_count > 0 && VALUE_TYPE(args[0]) == TYPE_STRING && AS_STRING(args[0]) &&
            strcmp(AS_STRING(args[0]), "--") == 0) ? 1 : 0;
}

Value vmBuiltinShellReturn(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    /* With no number it is $?, from before the trap if one is running: a trap's own commands
     * do not change it. */
    int status = gShellTrapStatusValid ? gShellTrapStatus : gShellRuntime.last_status;

    int first = shellSkipDoubleDash(arg_count, args);
    if (arg_count > first) {
        const char *text = VALUE_TYPE(args[first]) == TYPE_STRING ? AS_STRING(args[first]) : NULL;
        int parsed = 0;
        if (!text || !shellParseReturnStatus(text, &parsed)) {
            runtimeError(vm, "return: %s: numeric argument required", text ? text : "");
            status = 2;
        } else if (arg_count > first + 1) {
            runtimeError(vm, "return: too many arguments");
            shellRuntimeDiscardCommand(vm);
            shellRestoreCurrentVm(previous_vm);
            return makeVoid();
        } else {
            status = parsed;
        }
    }

    if (gShellReturnScopeDepth == 0 && gShellThreadReturnScope == 0) {
        runtimeError(vm, "return: can only `return' from a function or sourced script");
        shellUpdateStatus(2);
        shellRestoreCurrentVm(previous_vm);
        return makeVoid();
    }

    gShellReturnPriorStatus = gShellRuntime.last_status;
    shellUpdateStatus(status);
    /* An eval runs its text in a VM of its own, and has to pass this on. */
    gShellReturnPending = true;
    gShellReturnStatus = status;
    if (vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = "return";
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

#if defined(PSCAL_TARGET_IOS)
static bool shellReadTakeInterrupt(void) {
    int pid = vprocGetPidShim();
    sigset_t pending;
    sigemptyset(&pending);
    if (pid <= 0 || vprocSigpending(pid, &pending) != 0 || !sigismember(&pending, SIGINT)) {
        return false;
    }
    sigset_t watch;
    sigemptyset(&watch);
    sigaddset(&watch, SIGINT);
    int signo = 0;
    return vprocSigwait(pid, &watch, &signo) == 0;
}
#endif

/* Up to a delimiter (-1 for none) or a number of bytes (-1 for no limit). */
static ShellReadLineResult shellReadUntil(int fd, int delimiter, long limit, double timeout, char **out_line,
                                          size_t *out_length) {
    *out_line = NULL;
    *out_length = 0;
    if (fd < 0) {
        return SHELL_READ_LINE_ERROR;
    }
    size_t capacity = 128;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        return SHELL_READ_LINE_ERROR;
    }
    size_t length = 0;
    bool saw_any = false;
    while (limit < 0 || (long)length < limit) {
        if (timeout >= 0) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
            int ready = poll(&pfd, 1, (int)(timeout * 1000));
            if (ready == 0) {
                free(buffer);
                return SHELL_READ_LINE_TIMEOUT;
            }
        }
        char ch = '\0';
        ssize_t n = read(fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR) {
#if defined(PSCAL_TARGET_IOS)
                /* Out of line mode (read -n) the key does not end the read. */
                if (shellReadTakeInterrupt()) {
                    free(buffer);
                    return SHELL_READ_LINE_INTERRUPTED;
                }
#endif
                continue;
            }
            free(buffer);
            return SHELL_READ_LINE_ERROR;
        }
        if (n == 0) {
            if (!saw_any) {
                free(buffer);
                return SHELL_READ_LINE_EOF;
            }
            break;
        }
        saw_any = true;
        if (length + 2 >= capacity) {
            capacity *= 2;
            char *resized = (char *)realloc(buffer, capacity);
            if (!resized) {
                free(buffer);
                return SHELL_READ_LINE_ERROR;
            }
            buffer = resized;
        }
        buffer[length++] = ch;
        if (delimiter >= 0 && (unsigned char)ch == (unsigned char)delimiter) {
            break;
        }
    }
    buffer[length] = '\0';
    *out_line = buffer;
    *out_length = length;
    return SHELL_READ_LINE_OK;
}

static const char *shellReadResolveIFS(void) {
    const char *ifs = shellCurrentIfs();
    if (!ifs) {
        return " \t\n";
    }
    return ifs;
}

static bool shellReadIsIFSDelimiter(const char *ifs, char ch) {
    if (!ifs) {
        return false;
    }
    for (const char *p = ifs; *p; ++p) {
        if (*p == ch) {
            return true;
        }
    }
    return false;
}

static bool shellReadIsIFSWhitespaceDelimiter(const char *ifs, char ch) {
    if (!ifs || *ifs == '\0') {
        return false;
    }
    if (!shellReadIsIFSDelimiter(ifs, ch)) {
        return false;
    }
    return isspace((unsigned char)ch) != 0;
}

static char *shellReadCopyValue(const char *text, bool raw_mode) {
    if (!text) {
        return strdup("");
    }

    if (raw_mode) {
        return strdup(text);
    }

    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (!copy) {
        return NULL;
    }

    size_t out_index = 0;
    for (size_t i = 0; i < length; ++i) {
        char ch = text[i];
        if (ch == '\\' && i + 1 < length) {
            copy[out_index++] = text[++i];
        } else {
            copy[out_index++] = ch;
        }
    }
    copy[out_index] = '\0';
    return copy;
}

/* Past one field's delimiter: whitespace around it goes with it, so `1 : 2` is two fields. */
static char *shellReadSkipDelimiter(char *scan, const char *ifs) {
    if (*scan == '\0') {
        return scan;
    }
    bool white = shellReadIsIFSWhitespaceDelimiter(ifs, *scan);
    scan++;
    while (*scan && shellReadIsIFSWhitespaceDelimiter(ifs, *scan)) {
        scan++;
    }
    if (white && *scan && shellReadIsIFSDelimiter(ifs, *scan)) {
        scan++;
        while (*scan && shellReadIsIFSWhitespaceDelimiter(ifs, *scan)) {
            scan++;
        }
    }
    return scan;
}

static char *shellReadExtractField(char **cursor,
                                   bool last_field,
                                   bool raw_mode,
                                   const char *ifs) {
    if (!cursor) {
        return strdup("");
    }
    char *text = *cursor;
    if (!text) {
        return strdup("");
    }

    while (*text && shellReadIsIFSWhitespaceDelimiter(ifs, *text)) {
        text++;
    }

    char *scan = text;
    while (*scan) {
        if (!raw_mode && *scan == '\\') {
            if (scan[1] == '\0') {
                break;
            }
            scan += 2;
            continue;
        }
        if (shellReadIsIFSDelimiter(ifs, *scan)) {
            break;
        }
        scan++;
    }
    char *next = shellReadSkipDelimiter(scan, ifs);

    /* The last name takes the rest of the line, less the IFS whitespace at its end, an
     * escaped one included. If that rest is one field and what ends it, it gets the field. */
    if (last_field && *next) {
        char *end = text;
        for (char *p = text; *p; ++p) {
            bool escaped = !raw_mode && *p == '\\' && p[1];
            if (escaped) {
                p++;
            }
            if (!shellReadIsIFSWhitespaceDelimiter(ifs, *p)) {
                end = p + 1;
            }
        }
        scan = end;
        next = text + strlen(text);
    }

    char saved = *scan;
    *scan = '\0';
    char *value = shellReadCopyValue(text, raw_mode);
    *scan = saved;
    *cursor = next;
    return value;
}

/* One backslash escape of printf's format, or of a %b argument, which also reads \0NNN
 * and stops the output at \c. Returns how many bytes it took. */
static size_t shellPrintfWriteEscape(const char *text, FILE *out, bool argument, bool *out_stop) {
    char esc = text[1];
    switch (esc) {
        case 'a': fputc('\a', out); return 2;
        case 'b': fputc('\b', out); return 2;
        case 'e':
        case 'E': fputc(27, out); return 2;
        case 'f': fputc('\f', out); return 2;
        case 'n': fputc('\n', out); return 2;
        case 'r': fputc('\r', out); return 2;
        case 't': fputc('\t', out); return 2;
        case 'v': fputc('\v', out); return 2;
        case '\\': fputc('\\', out); return 2;
        case '"':
            fputc('"', out);
            return 2;
        case '\'':
            fputc('\'', out);
            return 2;
        default:
            break;
    }
    if (argument && esc == 'c') {
        if (out_stop) {
            *out_stop = true;
        }
        return 2;
    }
    if (esc >= '0' && esc <= '7') {
        /* A %b argument allows a leading zero on top of the three digits. */
        size_t i = 1;
        int limit = 3;
        if (argument && esc == '0') {
            i = 2;
        }
        int value = 0;
        int digits = 0;
        while (digits < limit && text[i] >= '0' && text[i] <= '7') {
            value = value * 8 + (text[i] - '0');
            i++;
            digits++;
        }
        fputc((char)value, out);
        return i;
    }
    if (esc == 'x') {
        size_t i = 2;
        int value = 0;
        int digits = 0;
        while (digits < 2 && isxdigit((unsigned char)text[i])) {
            char d = text[i];
            value = value * 16 + (isdigit((unsigned char)d) ? d - '0' : (tolower((unsigned char)d) - 'a' + 10));
            i++;
            digits++;
        }
        if (digits > 0) {
            fputc((char)value, out);
            return i;
        }
    }
    if (esc == 'u' || esc == 'U') {
        size_t i = 2;
        unsigned value = 0;
        int digits = 0;
        int limit = esc == 'u' ? 4 : 8;
        while (digits < limit && isxdigit((unsigned char)text[i])) {
            char d = text[i];
            value = value * 16 + (unsigned)(isdigit((unsigned char)d) ? d - '0' : (tolower((unsigned char)d) - 'a' + 10));
            i++;
            digits++;
        }
        if (digits > 0) {
            if (value < 0x80) {
                fputc((int)value, out);
            } else if (value < 0x800) {
                fputc((int)(0xC0 | (value >> 6)), out);
                fputc((int)(0x80 | (value & 0x3F)), out);
            } else if (value < 0x10000) {
                fputc((int)(0xE0 | (value >> 12)), out);
                fputc((int)(0x80 | ((value >> 6) & 0x3F)), out);
                fputc((int)(0x80 | (value & 0x3F)), out);
            } else {
                fputc((int)(0xF0 | (value >> 18)), out);
                fputc((int)(0x80 | ((value >> 12) & 0x3F)), out);
                fputc((int)(0x80 | ((value >> 6) & 0x3F)), out);
                fputc((int)(0x80 | (value & 0x3F)), out);
            }
            return i;
        }
    }
    fputc('\\', out);
    fputc(esc, out);
    return 2;
}

/* %q: reads back as one word. */
static char *shellPrintfQuote(const char *text) {
    if (!*text) {
        return strdup("''");
    }
    bool control = false;
    for (const char *cursor = text; *cursor; ++cursor) {
        if ((unsigned char)*cursor < 0x20 || *cursor == 0x7f) {
            control = true;
            break;
        }
    }
    char *out = NULL;
    size_t length = 0;
    size_t capacity = 0;
    if (control) {
        shellBufferAppendString(&out, &length, &capacity, "$'");
        for (const char *cursor = text; *cursor; ++cursor) {
            unsigned char c = (unsigned char)*cursor;
            char buffer[8];
            switch (c) {
                case '\n': shellBufferAppendString(&out, &length, &capacity, "\\n"); continue;
                case '\t': shellBufferAppendString(&out, &length, &capacity, "\\t"); continue;
                case '\r': shellBufferAppendString(&out, &length, &capacity, "\\r"); continue;
                case '\'': shellBufferAppendString(&out, &length, &capacity, "\\'"); continue;
                case '\\': shellBufferAppendString(&out, &length, &capacity, "\\\\"); continue;
                default: break;
            }
            if (c < 0x20 || c == 0x7f) {
                snprintf(buffer, sizeof(buffer), "\\%03o", c);
                shellBufferAppendString(&out, &length, &capacity, buffer);
            } else {
                shellBufferAppendChar(&out, &length, &capacity, (char)c);
            }
        }
        shellBufferAppendChar(&out, &length, &capacity, '\'');
        return out;
    }
    for (const char *cursor = text; *cursor; ++cursor) {
        unsigned char c = (unsigned char)*cursor;
        if (c < 0x80 && !isalnum(c) && !strchr("_@%+=:,./-", c)) {
            shellBufferAppendChar(&out, &length, &capacity, '\\');
        }
        shellBufferAppendChar(&out, &length, &capacity, (char)c);
    }
    return out;
}

/* A number, or 'c for the code of c. What parses before bad text still counts. */
static bool shellPrintfParseSigned(const char *text, long long *out_value) {
    if (text[0] == '\'' || text[0] == '"') {
        *out_value = (unsigned char)text[1];
        return true;
    }
    if (shellParseSignedLongLong(text, out_value)) {
        return true;
    }
    char *end = NULL;
    errno = 0;
    *out_value = strtoll(text, &end, 0);
    return false;
}

/* Whether the format has a %n, which asks how much has gone out. */
static bool shellPrintfCountsOutput(const char *fmt) {
    for (const char *cursor = strchr(fmt, '%'); cursor; cursor = strchr(cursor, '%')) {
        cursor++;
        if (*cursor == '%') {
            cursor++;
            continue;
        }
        cursor += strspn(cursor, "-+ #0'123456789*.hlLjzt");
        if (*cursor == 'n') {
            return true;
        }
    }
    return false;
}

Value vmBuiltinShellPrintf(VM *vm, int arg_count, Value *args) {
    int first = 0;
    const char *variable = NULL;
    if (first < arg_count && VALUE_TYPE(args[first]) == TYPE_STRING && AS_STRING(args[first]) &&
        strcmp(AS_STRING(args[first]), "-v") == 0 && first + 1 < arg_count &&
        VALUE_TYPE(args[first + 1]) == TYPE_STRING && AS_STRING(args[first + 1])) {
        variable = AS_STRING(args[first + 1]);
        first += 2;
    }
    if (first < arg_count && VALUE_TYPE(args[first]) == TYPE_STRING && AS_STRING(args[first]) &&
        strcmp(AS_STRING(args[first]), "--") == 0) {
        first++;
    }
    if (first >= arg_count) {
        shellReportRecoverableError(vm, false, "printf: usage: printf [-v var] format [arguments]");
        shellUpdateStatus(2);
        return makeVoid();
    }
    if (VALUE_TYPE(args[first]) != TYPE_STRING || !AS_STRING(args[first])) {
        shellReportRecoverableError(vm, true, "printf: format string required");
        shellUpdateStatus(2);
        return makeVoid();
    }
    const char *fmt = AS_STRING(args[first]);
    FILE *out = stdout;
    char *captured = NULL;
    size_t captured_size = 0;
    /* Only a memory stream can say how much has gone out, so %n goes through one too. */
    bool buffered = variable || shellPrintfCountsOutput(fmt);
    if (buffered) {
        out = open_memstream(&captured, &captured_size);
        if (!out) {
            shellUpdateStatus(1);
            return makeVoid();
        }
    }

    int arg_index = first + 1;
    bool ok = true;
    bool stop_output = false;

    bool repeat = false;
    do {
        bool consumed_conversion = false;
        /* %n counts from the start of this pass over the format, as in bash. */
        size_t pass_start = 0;
        if (buffered) {
            fflush(out);
            pass_start = captured_size;
        }
        for (size_t i = 0; fmt && fmt[i] != '\0'; ++i) {
            char c = fmt[i];
            if (c == '\\' && fmt[i + 1] != '\0') {
                i += shellPrintfWriteEscape(fmt + i, out, false, NULL) - 1;
                continue;
            }
            if (c == '%' && fmt[i + 1] != '\0') {
                if (fmt[i + 1] == '%') {
                    fputc('%', out);
                    i++;
                    continue;
                }

                size_t j = i + 1;
                char flags[8];
                size_t flag_len = 0;
                const char *flag_chars = "-+ #0'";
                while (fmt[j] && strchr(flag_chars, fmt[j]) != NULL) {
                    if (flag_len + 1 < sizeof(flags)) {
                        flags[flag_len++] = fmt[j];
                    }
                    j++;
                }
                flags[flag_len] = '\0';

                bool width_specified = false;
                bool width_from_arg = false;
                int width = 0;
                if (fmt[j] == '*') {
                    width_specified = true;
                    width_from_arg = true;
                    j++;
                } else {
                    while (isdigit((unsigned char)fmt[j])) {
                        width_specified = true;
                        width = width * 10 + (fmt[j] - '0');
                        j++;
                    }
                }

                bool precision_specified = false;
                bool precision_from_arg = false;
                int precision = 0;
                if (fmt[j] == '.') {
                    j++;
                    precision_specified = true;
                    if (fmt[j] == '*') {
                        precision_from_arg = true;
                        j++;
                    } else {
                        precision = 0;
                        while (isdigit((unsigned char)fmt[j])) {
                            precision = precision * 10 + (fmt[j] - '0');
                            j++;
                        }
                    }
                }

                bool mod_h = false;
                bool mod_hh = false;
                char length_mod[3] = {0};
                size_t length_len = 0;
                if (fmt[j] == 'h') {
                    mod_h = true;
                    length_mod[length_len++] = 'h';
                    j++;
                    if (fmt[j] == 'h') {
                        mod_hh = true;
                        mod_h = false;
                        length_mod[length_len++] = 'h';
                        j++;
                    }
                } else if (fmt[j] == 'l') {
                    length_mod[length_len++] = 'l';
                    j++;
                    if (fmt[j] == 'l') {
                        length_mod[length_len++] = 'l';
                        j++;
                    }
                } else {
                    const char *length_mods = "Ljzt";
                    while (fmt[j] && strchr(length_mods, fmt[j]) != NULL) {
                        if (length_len + 1 < sizeof(length_mod)) {
                            length_mod[length_len++] = fmt[j];
                        }
                        j++;
                    }
                }

                char spec = fmt[j];
                if (spec == '\0') {
                    shellReportRecoverableError(vm, true, "printf: incomplete format specifier");
                    ok = false;
                    break;
                }
                /* As bash 5 takes them, not as wide characters in a string that holds none. */
                if (spec == 'S') {
                    spec = 's';
                } else if (spec == 'C') {
                    spec = 'c';
                }

                char fmtbuf[32];
                size_t pos = 0;
                fmtbuf[pos++] = '%';
                consumed_conversion = true;
                if (flag_len > 0 && pos + flag_len < sizeof(fmtbuf)) {
                    memcpy(&fmtbuf[pos], flags, flag_len);
                    pos += flag_len;
                }
                if (width_specified) {
                    if (width_from_arg) {
                        if (pos < sizeof(fmtbuf)) {
                            fmtbuf[pos++] = '*';
                        }
                    } else {
                        int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", width);
                        if (written > 0) {
                            if ((size_t)written >= sizeof(fmtbuf) - pos) {
                                pos = sizeof(fmtbuf) - 1;
                            } else {
                                pos += (size_t)written;
                            }
                        }
                    }
                }
                if (precision_specified && pos < sizeof(fmtbuf)) {
                    fmtbuf[pos++] = '.';
                    if (precision_from_arg) {
                        if (pos < sizeof(fmtbuf)) {
                            fmtbuf[pos++] = '*';
                        }
                    } else {
                        int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", precision);
                        if (written > 0) {
                            if ((size_t)written >= sizeof(fmtbuf) - pos) {
                                pos = sizeof(fmtbuf) - 1;
                            } else {
                                pos += (size_t)written;
                            }
                        }
                    }
                }
                if (length_len > 0 && pos + length_len < sizeof(fmtbuf)) {
                    memcpy(&fmtbuf[pos], length_mod, length_len);
                    pos += length_len;
                }
                if (pos < sizeof(fmtbuf)) {
                    fmtbuf[pos++] = spec;
                }
                fmtbuf[pos < sizeof(fmtbuf) ? pos : (sizeof(fmtbuf) - 1)] = '\0';

                int width_value = width;
                if (width_from_arg && arg_index < arg_count) {
                    Value value = args[arg_index];
                    const char *width_text = "";
                    if (VALUE_TYPE(value) == TYPE_STRING && AS_STRING(value)) {
                        width_text = AS_STRING(value);
                    }
                    if (width_text[0] == '\0') {
                        width_value = 0;
                    } else {
                        long long parsed_width = 0;
                        if (!shellParseSignedLongLong(width_text, &parsed_width)) {
                            shellReportRecoverableError(vm, false, "printf: %s: invalid number", width_text);
                            ok = false;
                            parsed_width = 0;
                        }
                        if (parsed_width > INT_MAX) {
                            parsed_width = INT_MAX;
                        } else if (parsed_width < INT_MIN) {
                            parsed_width = INT_MIN;
                        }
                        width_value = (int)parsed_width;
                    }
                    arg_index++;
                } else if (width_from_arg) {
                    width_value = 0;
                }

                int precision_value = precision;
                if (precision_from_arg && arg_index < arg_count) {
                    Value value = args[arg_index];
                    const char *precision_text = "";
                    if (VALUE_TYPE(value) == TYPE_STRING && AS_STRING(value)) {
                        precision_text = AS_STRING(value);
                    }
                    if (precision_text[0] == '\0') {
                        precision_value = 0;
                    } else {
                        long long parsed_precision = 0;
                        if (!shellParseSignedLongLong(precision_text, &parsed_precision)) {
                            shellReportRecoverableError(vm, false, "printf: %s: invalid number", precision_text);
                            ok = false;
                            parsed_precision = 0;
                        }
                        if (parsed_precision > INT_MAX) {
                            parsed_precision = INT_MAX;
                        } else if (parsed_precision < INT_MIN) {
                            parsed_precision = INT_MIN;
                        }
                        precision_value = (int)parsed_precision;
                    }
                    arg_index++;
                } else if (precision_from_arg) {
                    precision_value = 0;
                }

                const char *arg_text = "";
                if (arg_index < arg_count) {
                    Value value = args[arg_index];
                    if (VALUE_TYPE(value) == TYPE_STRING && AS_STRING(value)) {
                        arg_text = AS_STRING(value);
                    } else if (VALUE_TYPE(value) == TYPE_STRING && !AS_STRING(value)) {
                        arg_text = "";
                    }
                    arg_index++;
                }

                bool has_wide_char_length = false;
                for (size_t n = 0; n < length_len; ++n) {
                    if (length_mod[n] == 'l' || length_mod[n] == 'L') {
                        has_wide_char_length = true;
                        break;
                    }
                }

            switch (spec) {
                case 'd':
                case 'i': {
                    long long iv = 0;
                    if (!shellPrintfParseSigned(arg_text, &iv)) {
                        shellReportRecoverableError(vm, false, "printf: %s: invalid number", arg_text);
                        ok = false;
                    }
                    if (mod_hh) {
                        iv = (signed char)iv;
                    } else if (mod_h) {
                        iv = (short)iv;
                    }
                    /* No fixed buffer anywhere here: a width can be any size. */
                    if (width_from_arg && precision_from_arg) {
                        fprintf(out, fmtbuf, width_value, precision_value, iv);
                    } else if (width_from_arg) {
                        fprintf(out, fmtbuf, width_value, iv);
                    } else if (precision_from_arg) {
                        fprintf(out, fmtbuf, precision_value, iv);
                    } else {
                        fprintf(out, fmtbuf, iv);
                    }
                    break;
                }
                case 'u':
                case 'o':
                case 'x':
                case 'X': {
                    unsigned long long uv = 0;
                    long long signed_uv = 0;
                    if (!shellPrintfParseSigned(arg_text, &signed_uv)) {
                        shellReportRecoverableError(vm, false, "printf: %s: invalid number", arg_text);
                        ok = false;
                    }
                    uv = (unsigned long long)signed_uv;
                    if (mod_hh) {
                        uv = (unsigned char)uv;
                    } else if (mod_h) {
                        uv = (unsigned short)uv;
                    }
                    if (width_from_arg && precision_from_arg) {
                        fprintf(out, fmtbuf, width_value, precision_value, uv);
                    } else if (width_from_arg) {
                        fprintf(out, fmtbuf, width_value, uv);
                    } else if (precision_from_arg) {
                        fprintf(out, fmtbuf, precision_value, uv);
                    } else {
                        fprintf(out, fmtbuf, uv);
                    }
                    break;
                }
                case 'f':
                case 'F':
                case 'e':
                case 'E':
                case 'g':
                case 'G':
                case 'a':
                case 'A': {
                    double dv = 0.0;
                    if (!shellParseDouble(arg_text, &dv)) {
                        shellReportRecoverableError(vm, false, "printf: %s: invalid number", arg_text);
                        dv = 0.0;
                        ok = false;
                    }
                    if (width_from_arg && precision_from_arg) {
                        fprintf(out, fmtbuf, width_value, precision_value, dv);
                    } else if (width_from_arg) {
                        fprintf(out, fmtbuf, width_value, dv);
                    } else if (precision_from_arg) {
                        fprintf(out, fmtbuf, precision_value, dv);
                    } else {
                        fprintf(out, fmtbuf, dv);
                    }
                    break;
                }
                case 'c': {
                    char ch = arg_text ? arg_text[0] : '\0';
                    const char *format = fmtbuf;
                    char safe_fmt[sizeof(fmtbuf)];
                    if (has_wide_char_length) {
                        strncpy(safe_fmt, fmtbuf, sizeof(safe_fmt));
                        safe_fmt[sizeof(safe_fmt) - 1] = '\0';
                        char *mod_pos = strstr(safe_fmt, length_mod);
                        if (mod_pos) {
                            size_t remove_len = strlen(length_mod);
                            memmove(mod_pos, mod_pos + remove_len, strlen(mod_pos + remove_len) + 1);
                            format = safe_fmt;
                        }
                    }
                    if (width_from_arg && precision_from_arg) {
                        fprintf(out, format, width_value, precision_value, ch);
                    } else if (width_from_arg) {
                        fprintf(out, format, width_value, ch);
                    } else if (precision_from_arg) {
                        fprintf(out, format, precision_value, ch);
                    } else {
                        fprintf(out, format, ch);
                    }
                    break;
                }
                case 'b':
                case 'q':
                case 's': {
                    char *converted = NULL;
                    if (spec == 'b') {
                        size_t converted_size = 0;
                        FILE *mem = open_memstream(&converted, &converted_size);
                        if (mem) {
                            for (const char *cursor = arg_text; *cursor && !stop_output;) {
                                if (*cursor == '\\' && cursor[1]) {
                                    cursor += shellPrintfWriteEscape(cursor, mem, true, &stop_output);
                                } else {
                                    fputc(*cursor++, mem);
                                }
                            }
                            fclose(mem);
                        }
                    } else if (spec == 'q') {
                        converted = shellPrintfQuote(arg_text);
                    }
                    const char *sv = converted ? converted : (arg_text ? arg_text : "");
                    char *spec_pos = strrchr(fmtbuf, spec);
                    if (spec_pos) {
                        *spec_pos = 's';
                    }
                    const char *format = fmtbuf;
                    char safe_fmt[sizeof(fmtbuf)];
                    if (has_wide_char_length) {
                        strncpy(safe_fmt, fmtbuf, sizeof(safe_fmt));
                        safe_fmt[sizeof(safe_fmt) - 1] = '\0';
                        char *mod_pos = strstr(safe_fmt, length_mod);
                        if (mod_pos) {
                            size_t remove_len = strlen(length_mod);
                            memmove(mod_pos, mod_pos + remove_len, strlen(mod_pos + remove_len) + 1);
                            format = safe_fmt;
                        }
                    }
                    if (width_from_arg && precision_from_arg) {
                        fprintf(out, format, width_value, precision_value, sv);
                    } else if (width_from_arg) {
                        fprintf(out, format, width_value, sv);
                    } else if (precision_from_arg) {
                        fprintf(out, format, precision_value, sv);
                    } else {
                        fprintf(out, format, sv);
                    }
                    free(converted);
                    if (stop_output) {
                        repeat = false;
                        goto printf_done;
                    }
                    break;
                }
                case 'p': {
                    unsigned long long pv = 0;
                    if (!shellParseUnsignedLongLong(arg_text, &pv)) {
                        shellReportRecoverableError(vm, false, "printf: %s: invalid number", arg_text);
                        pv = 0;
                        ok = false;
                    }
                    if (width_from_arg && precision_from_arg) {
                        fprintf(out, fmtbuf, width_value, precision_value, (void *)(uintptr_t)pv);
                    } else if (width_from_arg) {
                        fprintf(out, fmtbuf, width_value, (void *)(uintptr_t)pv);
                    } else if (precision_from_arg) {
                        fprintf(out, fmtbuf, precision_value, (void *)(uintptr_t)pv);
                    } else {
                        fprintf(out, fmtbuf, (void *)(uintptr_t)pv);
                    }
                    break;
                }
                case 'n': {
                    /* Never libc's: its %n writes through the argument, and Darwin's aborts on
                     * it, which on iOS ends the whole app. */
                    if (!arg_text[0]) {
                        break;
                    }
                    if (!shellIsValidEnvName(arg_text)) {
                        shellReportRecoverableError(vm, false, "printf: `%s': not a valid identifier", arg_text);
                        ok = false;
                        goto printf_done;
                    }
                    fflush(out);
                    char count[32];
                    snprintf(count, sizeof(count), "%zu", captured_size - pass_start);
                    if (!shellSetTrackedVariable(arg_text, count, false)) {
                        ok = false;
                    }
                    break;
                }
                default:
                    /* Nothing else goes to libc with a string for its argument: %D would print
                     * its address. */
                    shellReportRecoverableError(vm, false, "printf: `%c': invalid format character", spec);
                    ok = false;
                    goto printf_done;
            }

            i = j;
            continue;
        }

        fputc(c, out);
    }

        if (!ok) {
            repeat = false;
        } else {
            repeat = fmt && arg_index < arg_count && consumed_conversion;
        }
    } while (repeat);
printf_done:

    fflush(out);
    if (buffered) {
        fclose(out);
    }
    if (variable) {
        char *value = captured ? captured : strdup("");
        if (!value || !shellSetTrackedVariable(variable, value, false)) {
            ok = false;
        }
        free(value);
    } else if (buffered) {
        if (captured) {
            fwrite(captured, 1, captured_size, stdout);
            fflush(stdout);
        }
        free(captured);
    }
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellRead(VM *vm, int arg_count, Value *args) {
    const char *prompt = NULL;
    const char *array_name = NULL;
    const char **variables = NULL;
    size_t variable_count = 0;
    bool parsing_options = true;
    bool ok = true;
    bool raw_mode = false;
    bool read_silent = false;
    int read_delim = '\n';
    long read_chars = -1;
    bool read_exact = false;
    int read_fd = STDIN_FILENO;
    double read_timeout = -1;

    /* On iOS builds, shell builtins are globally registered and can shadow the
     * core VM "read" builtin. Non-shell frontends (e.g. tiny/clike bytecode)
     * pass VAR/file arguments as pointers, so route those calls to vmBuiltinRead. */
    if (arg_count > 0) {
        bool legacy_vm_read_call = true;
        for (int i = 0; i < arg_count; ++i) {
            Value val = args[i];
            if (VALUE_TYPE(val) == TYPE_POINTER && AS_POINTER(val)) {
                continue;
            }
            if (VALUE_TYPE(val) == TYPE_FILE) {
                continue;
            }
            legacy_vm_read_call = false;
            break;
        }
        if (legacy_vm_read_call) {
            return vmBuiltinRead(vm, arg_count, args);
        }
    }

    for (int i = 0; i < arg_count && ok; ++i) {
        Value val = args[i];
        if (VALUE_TYPE(val) != TYPE_STRING || !AS_STRING(val)) {
            runtimeError(vm, "read: arguments must be strings");
            ok = false;
            break;
        }
        const char *token = AS_STRING(val);
        if (parsing_options) {
            if (strcmp(token, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (token[0] == '-' && token[1] != '\0') {
                size_t option_length = strlen(token);
                bool pending_prompt = false;
                bool pending_array = false;
                for (size_t opt_index = 1; opt_index < option_length && ok; ++opt_index) {
                    char opt = token[opt_index];
                    switch (opt) {
                        case 'r':
                            raw_mode = true;
                            break;
                        case 's':
                            read_silent = true;
                            break;
                        case 'e':
                            break;
                        case 'p':
                            pending_prompt = true;
                            break;
                        case 'd':
                        case 'n':
                        case 'N':
                        case 't':
                        case 'u':
                        case 'i': {
                            const char *operand = NULL;
                            if (opt_index + 1 < option_length) {
                                operand = &token[opt_index + 1];
                                opt_index = option_length;
                            } else if (i + 1 < arg_count && VALUE_TYPE(args[i + 1]) == TYPE_STRING &&
                                       AS_STRING(args[i + 1])) {
                                operand = AS_STRING(args[++i]);
                            }
                            if (!operand) {
                                runtimeError(vm, "read: -%c: option requires an argument", opt);
                                ok = false;
                                break;
                            }
                            if (opt == 'd') {
                                read_delim = (unsigned char)operand[0];
                            } else if (opt == 'n' || opt == 'N') {
                                read_chars = strtol(operand, NULL, 10);
                                read_exact = opt == 'N';
                            } else if (opt == 't') {
                                read_timeout = strtod(operand, NULL);
                            } else if (opt == 'u') {
                                read_fd = (int)strtol(operand, NULL, 10);
                            }
                            break;
                        }
                        case 'a':
                            if (array_name) {
                                runtimeError(vm, "read: option -a specified multiple times");
                                ok = false;
                                break;
                            }
                            if (opt_index + 1 < option_length) {
                                array_name = &token[opt_index + 1];
                                opt_index = option_length;
                            } else {
                                pending_array = true;
                            }
                            break;
                        default:
                            runtimeError(vm, "read: unsupported option '-%c'", opt);
                            ok = false;
                            break;
                    }
                }
                if (!ok) {
                    break;
                }
                if (pending_prompt) {
                    if (i + 1 >= arg_count) {
                        runtimeError(vm, "read: option -p requires an argument");
                        ok = false;
                        break;
                    }
                    Value prompt_val = args[++i];
                    if (VALUE_TYPE(prompt_val) != TYPE_STRING || !AS_STRING(prompt_val)) {
                        runtimeError(vm, "read: prompt must be a string");
                        ok = false;
                        break;
                    }
                    prompt = AS_STRING(prompt_val);
                }
                if (!ok) {
                    break;
                }
                if (pending_array) {
                    if (i + 1 >= arg_count) {
                        runtimeError(vm, "read: option -a requires an argument");
                        ok = false;
                        break;
                    }
                    Value array_val = args[++i];
                    if (VALUE_TYPE(array_val) != TYPE_STRING || !AS_STRING(array_val) || AS_STRING(array_val)[0] == '\0') {
                        runtimeError(vm, "read: array name must be a non-empty string");
                        ok = false;
                        break;
                    }
                    array_name = AS_STRING(array_val);
                }
                continue;
            }
            parsing_options = false;
        }
        const char **resized = (const char **)realloc(variables, (variable_count + 1) * sizeof(const char *));
        if (!resized) {
            runtimeError(vm, "read: out of memory");
            ok = false;
            break;
        }
        variables = resized;
        variables[variable_count++] = token;
    }

    bool whole_line = false;
    if (ok && variable_count == 0 && !array_name) {
        variables = (const char **)malloc(sizeof(const char *));
        if (!variables) {
            runtimeError(vm, "read: out of memory");
            ok = false;
        } else {
            variables[0] = "REPLY";
            variable_count = 1;
            whole_line = true;
        }
    }

    /* -N and $REPLY take the text as it came: only names split it. */
    const char *ifs = (read_exact || whole_line) ? "" : shellReadResolveIFS();

    ShellReadLineResult read_result = SHELL_READ_LINE_ERROR;
    char *line = NULL;
    size_t line_length = 0;
    bool read_saw_delimiter = false;
    if (ok) {
        if (prompt) {
            write(STDOUT_FILENO, prompt, strlen(prompt));
        }
        fflush(stdout);
        if (read_timeout == 0) {
            /* Reads nothing: says whether there is something to read. */
            struct pollfd pfd = {.fd = read_fd, .events = POLLIN, .revents = 0};
            int ready = poll(&pfd, 1, 0);
            free(variables);
            shellUpdateStatus(ready > 0 ? 0 : 1);
            return makeVoid();
        }
        /* At a terminal: -s takes the echo away, and a read that does not end at
         * the end of a line gets the keys as they come. */
        const bool read_keys = read_chars >= 0 || read_delim != '\n';
        struct termios read_saved;
        bool read_termios_changed = false;
        if ((read_silent || read_keys) && shellToolTcgetattr(read_fd, &read_saved)) {
            struct termios read_termios = read_saved;
            if (read_silent) {
                read_termios.c_lflag &= ~(tcflag_t)ECHO;
            }
            if (read_keys) {
                read_termios.c_lflag &= ~(tcflag_t)ICANON;
                read_termios.c_cc[VMIN] = 1;
                read_termios.c_cc[VTIME] = 0;
            }
            read_termios_changed = shellToolTcsetattr(read_fd, TCSADRAIN, &read_termios);
        }
        read_result = shellReadUntil(read_fd, read_exact ? -1 : read_delim, read_chars, read_timeout, &line, &line_length);
        if (read_termios_changed) {
            shellToolTcsetattr(read_fd, TCSADRAIN, &read_saved);
        }
        if (read_result == SHELL_READ_LINE_OK && !read_exact && line_length > 0 &&
            (unsigned char)line[line_length - 1] == (unsigned char)read_delim) {
            line[--line_length] = '\0';
            read_saw_delimiter = true;
        }
        if (read_result == SHELL_READ_LINE_ERROR) {
            runtimeError(vm, "read: read error: %d: %s", read_fd, strerror(errno));
        }
    }

    bool assign_ok = ok;
    char *cursor = line;
    if (ok && array_name) {
        char *array_cursor = line;
        char **array_values = NULL;
        size_t array_count = 0;
        size_t array_capacity = 0;
        bool has_non_whitespace = false;
        if (line) {
            for (const char *scan = line; *scan; ++scan) {
                if (!shellReadIsIFSWhitespaceDelimiter(ifs, *scan)) {
                    has_non_whitespace = true;
                    break;
                }
            }
        }
        if (read_result == SHELL_READ_LINE_OK && array_cursor) {
            while (*array_cursor) {
                char *value_copy = shellReadExtractField(&array_cursor, false, raw_mode, ifs);
                if (!value_copy) {
                    runtimeError(vm, "read: out of memory");
                    assign_ok = false;
                    break;
                }
                if (!has_non_whitespace && value_copy[0] == '\0') {
                    free(value_copy);
                    continue;
                }
                if (!shellAppendArrayValue(&array_values, &array_count, &array_capacity, value_copy)) {
                    runtimeError(vm, "read: out of memory");
                    assign_ok = false;
                    break;
                }
            }
        }
        if (assign_ok) {
            if (!shellArrayRegistryStore(array_name, array_values, NULL, array_count, SHELL_ARRAY_KIND_INDEXED)) {
                runtimeError(vm, "read: out of memory");
                assign_ok = false;
            } else {
                const ShellArrayVariable *stored = shellArrayRegistryFindConst(array_name);
                char *literal = shellBuildArrayLiteral(stored);
                if (!literal) {
                    runtimeError(vm, "read: out of memory");
                    shellArrayRegistryRemove(array_name);
                    assign_ok = false;
                } else {
                    if (setenv(array_name, literal, 1) != 0) {
                        int err = errno;
                        runtimeError(vm, "read: unable to set array '%s': %s", array_name, strerror(err));
                        shellArrayRegistryRemove(array_name);
                        assign_ok = false;
                    }
                    free(literal);
                }
            }
        }
        shellFreeArrayValues(array_values, array_count);
        array_values = NULL;
        if (read_result == SHELL_READ_LINE_OK) {
            cursor = array_cursor;
        }
    }

    if (ok && (read_result == SHELL_READ_LINE_OK || read_result == SHELL_READ_LINE_EOF)) {
        for (size_t i = 0; i < variable_count; ++i) {
            bool last = (i + 1 == variable_count);
            char *value_copy = NULL;
            if (read_result == SHELL_READ_LINE_OK) {
                value_copy = shellReadExtractField(&cursor, last, raw_mode, ifs);
            } else {
                value_copy = strdup("");
            }
            if (!value_copy) {
                runtimeError(vm, "read: out of memory");
                assign_ok = false;
                break;
            }
            if (!shellSetTrackedVariable(variables[i], value_copy, false)) {
                runtimeError(vm, "read: unable to set '%s': %s", variables[i], strerror(errno));
                free(value_copy);
                assign_ok = false;
                break;
            }
            free(value_copy);
        }
    }

    free(line);
    free(variables);

    /* The input ended before the delimiter: the value is set and the status says so. */
    bool ended_early = read_result == SHELL_READ_LINE_OK && read_chars < 0 && !read_saw_delimiter;
    if (read_result == SHELL_READ_LINE_TIMEOUT) {
        shellUpdateStatus(128 + SIGALRM);
    } else if (read_result == SHELL_READ_LINE_INTERRUPTED) {
        shellUpdateStatus(128 + SIGINT);
    } else if (!ok || !assign_ok || read_result != SHELL_READ_LINE_OK || ended_early) {
        shellUpdateStatus(1);
    } else {
        shellUpdateStatus(0);
    }
    return makeVoid();
}

static void shellGetoptsSyncIndex(void) {
    char *text = shellLookupParameterValue("OPTIND", strlen("OPTIND"));
    if (text && !*text) {
        /* `local OPTIND` starts over, as a fresh value of 1 would. */
        gShellRuntime.getopts_index = 1;
        gShellRuntime.getopts_offset = 1;
        free(text);
    } else if (text) {
        char *end = NULL;
        long parsed = strtol(text, &end, 10);
        if (end && *end == '\0' && parsed >= 1 && parsed <= INT_MAX) {
            int previous = gShellRuntime.getopts_index;
            gShellRuntime.getopts_index = (int)parsed;
            if (gShellRuntime.getopts_index != previous || gShellRuntime.getopts_offset < 1) {
                gShellRuntime.getopts_offset = 1;
            }
        }
        free(text);
    } else if (gShellRuntime.getopts_index < 1) {
        gShellRuntime.getopts_offset = 1;
        shellGetoptsUpdateOptind(1);
    }
}

Value vmBuiltinShellGetopts(VM *vm, int arg_count, Value *args) {
    if (arg_count < 2) {
        runtimeError(vm, "getopts: expected OPTSTRING NAME [ARGS...]");
        shellUpdateStatus(2);
        return makeVoid();
    }
    if (VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0])) {
        runtimeError(vm, "getopts: option string must be provided");
        shellUpdateStatus(2);
        return makeVoid();
    }
    if (VALUE_TYPE(args[1]) != TYPE_STRING || !AS_STRING(args[1]) || AS_STRING(args[1])[0] == '\0') {
        runtimeError(vm, "getopts: variable name must be a non-empty string");
        shellUpdateStatus(2);
        return makeVoid();
    }

    const char *optstring = AS_STRING(args[0]);
    bool silent_errors = false;
    if (optstring[0] == ':') {
        silent_errors = true;
        optstring++;
    }
    const char *name = AS_STRING(args[1]);

    char **argv_list = NULL;
    int argc_list = 0;
    char **override_args = NULL;
    if (arg_count > 2) {
        argc_list = arg_count - 2;
        override_args = (char **)calloc((size_t)argc_list, sizeof(char *));
        if (!override_args) {
            runtimeError(vm, "getopts: out of memory");
            shellUpdateStatus(2);
            return makeVoid();
        }
        bool valid = true;
        for (int i = 0; i < argc_list; ++i) {
            Value val = args[i + 2];
            if (VALUE_TYPE(val) != TYPE_STRING || !AS_STRING(val)) {
                valid = false;
                break;
            }
            override_args[i] = AS_STRING(val);
        }
        if (!valid) {
            free(override_args);
            runtimeError(vm, "getopts: arguments must be strings");
            shellUpdateStatus(2);
            return makeVoid();
        }
        argv_list = override_args;
    } else {
        argv_list = gParamValues;
        argc_list = gParamCount;
    }

    if (!argv_list || argc_list <= 0) {
        gShellRuntime.getopts_offset = 1;
        shellGetoptsUpdateOptind(1);
        shellUpdateStatus(1);
        return makeVoid();
    }

    shellGetoptsSyncIndex();
    int optind = gShellRuntime.getopts_index;
    if (optind < 1) {
        optind = 1;
    }

    while (optind <= argc_list) {
        const char *current = argv_list[optind - 1];
        if (!current) {
            optind++;
            gShellRuntime.getopts_offset = 1;
            continue;
        }
        if (strcmp(current, "--") == 0) {
            optind++;
            shellGetoptsSetOptarg(NULL);
            gShellRuntime.getopts_offset = 1;
            shellGetoptsUpdateOptind(optind);
            shellUpdateStatus(1);
            free(override_args);
            return makeVoid();
        }
        if (current[0] != '-' || current[1] == '\0') {
            shellGetoptsSetOptarg(NULL);
            gShellRuntime.getopts_offset = 1;
            shellGetoptsUpdateOptind(optind);
            shellUpdateStatus(1);
            free(override_args);
            return makeVoid();
        }

        int offset = gShellRuntime.getopts_offset;
        if (offset < 1) {
            offset = 1;
        }
        char option = current[offset];
        if (option == '\0') {
            optind++;
            gShellRuntime.getopts_offset = 1;
            continue;
        }
        const char *rest = current + offset + 1;
        bool has_more = *rest != '\0';
        const char *match = strchr(optstring, option);
        if (!match || option == ':') {
            char optbuf[2] = {option ? option : '?', '\0'};
            shellSetTrackedVariable(name, "?", false);
            shellGetoptsSetOptarg(optbuf);
            if (!silent_errors && option) {
                fprintf(stderr, "getopts: illegal option -- %c\n", option);
            }
            if (has_more) {
                gShellRuntime.getopts_offset = offset + 1;
            } else {
                optind++;
                gShellRuntime.getopts_offset = 1;
            }
            shellGetoptsUpdateOptind(optind);
            shellUpdateStatus(0);
            free(override_args);
            return makeVoid();
        }

        bool expects_arg = (match[1] == ':');
        char *optarg_value = NULL;
        if (expects_arg) {
            if (has_more) {
                optarg_value = strdup(rest);
                optind++;
                gShellRuntime.getopts_offset = 1;
            } else if (optind < argc_list) {
                const char *next = argv_list[optind];
                optarg_value = strdup(next ? next : "");
                optind += 2;
                gShellRuntime.getopts_offset = 1;
            } else {
                char optbuf[2] = {option, '\0'};
                if (silent_errors) {
                    shellSetTrackedVariable(name, ":", false);
                } else {
                    shellSetTrackedVariable(name, "?", false);
                    fprintf(stderr, "getopts: option requires an argument -- %c\n", option);
                }
                shellGetoptsSetOptarg(optbuf);
                optind++;
                gShellRuntime.getopts_offset = 1;
                shellGetoptsUpdateOptind(optind);
                shellUpdateStatus(0);
                free(override_args);
                return makeVoid();
            }
        } else {
            if (has_more) {
                gShellRuntime.getopts_offset = offset + 1;
            } else {
                optind++;
                gShellRuntime.getopts_offset = 1;
            }
        }

        char optchar[2] = {option, '\0'};
        shellSetTrackedVariable(name, optchar, false);
        if (optarg_value) {
            shellGetoptsSetOptarg(optarg_value);
            free(optarg_value);
        } else {
            shellGetoptsSetOptarg(NULL);
        }
        shellGetoptsUpdateOptind(optind);
        shellUpdateStatus(0);
        free(override_args);
        return makeVoid();
    }

    shellGetoptsSetOptarg(NULL);
    gShellRuntime.getopts_offset = 1;
    shellGetoptsUpdateOptind(optind);
    shellUpdateStatus(1);
    free(override_args);
    return makeVoid();
}


static bool shellMapfileStoreArray(const char *name, char **values, size_t count) {
    if (!name) {
        name = "MAPFILE";
    }
    if (!shellArrayRegistryStore(name, values, NULL, count, SHELL_ARRAY_KIND_INDEXED)) {
        return false;
    }
    const ShellArrayVariable *stored = shellArrayRegistryFindConst(name);
    if (!stored) {
        return false;
    }
    char *literal = shellBuildArrayLiteral(stored);
    if (!literal) {
        shellArrayRegistryRemove(name);
        return false;
    }
    if (setenv(name, literal, 1) != 0) {
        shellArrayRegistryRemove(name);
        free(literal);
        return false;
    }
    free(literal);
    return true;
}

Value vmBuiltinShellMapfile(VM *vm, int arg_count, Value *args) {
    bool strip_newline = false;
    const char *array_name = "MAPFILE";
    int index = 0;

    while (index < arg_count) {
        Value arg = args[index];
        if (VALUE_TYPE(arg) != TYPE_STRING || !AS_STRING(arg)) {
            runtimeError(vm, "mapfile: invalid argument");
            shellUpdateStatus(2);
            return makeVoid();
        }
        if (strcmp(AS_STRING(arg), "-t") == 0) {
            strip_newline = true;
            index++;
            continue;
        }
        if (AS_STRING(arg)[0] == '-') {
            runtimeError(vm, "mapfile: unsupported option '%s'", AS_STRING(arg));
            shellUpdateStatus(2);
            return makeVoid();
        }
        array_name = AS_STRING(arg);
        index++;
        break;
    }

    if (index < arg_count) {
        runtimeError(vm, "mapfile: unexpected argument '%s'", AS_STRING(args[index]));
        shellUpdateStatus(2);
        return makeVoid();
    }

    char **values = NULL;
    size_t count = 0;
    size_t capacity = 0;
    bool ok = true;

    for (;;) {
        char *line = NULL;
        size_t line_length = 0;
        ShellReadLineResult result = shellReadLineFromFd(STDIN_FILENO, &line, &line_length);
        if (result == SHELL_READ_LINE_ERROR) {
            runtimeError(vm, "mapfile: failed to read input");
            ok = false;
            free(line);
            break;
        }
        if (result == SHELL_READ_LINE_EOF) {
            free(line);
            break;
        }
        if (strip_newline) {
            while (line_length > 0 &&
                   (line[line_length - 1] == '\n' || line[line_length - 1] == '\r')) {
                line[--line_length] = '\0';
            }
        }
        if (!shellAppendArrayValue(&values, &count, &capacity, line)) {
            runtimeError(vm, "mapfile: out of memory");
            free(line);
            ok = false;
            break;
        }
    }

    if (ok) {
        if (!shellMapfileStoreArray(array_name, values, count)) {
            runtimeError(vm, "mapfile: unable to store results");
            ok = false;
        }
    }

    shellFreeArrayValues(values, count);
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellShift(VM *vm, int arg_count, Value *args) {
    int shift_count = 1;
    if (arg_count > 1) {
        runtimeError(vm, "shift: expected optional non-negative count");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (arg_count == 1) {
        Value v = args[0];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v) || !*AS_STRING(v)) {
            runtimeError(vm, "shift: expected numeric argument");
            shellUpdateStatus(1);
            return makeVoid();
        }
        char *end = NULL;
        errno = 0;
        long parsed = strtol(AS_STRING(v), &end, 10);
        if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
            runtimeError(vm, "shift: invalid count '%s'", AS_STRING(v));
            shellUpdateStatus(1);
            return makeVoid();
        }
        shift_count = (int)parsed;
    }

    if (shift_count == 0) {
        shellUpdateStatus(0);
        return makeVoid();
    }

    if (shift_count > gParamCount || gParamCount <= 0 || !gParamValues) {
        runtimeError(vm, "shift: count out of range");
        shellUpdateStatus(1);
        return makeVoid();
    }

    for (int i = 0; i + shift_count < gParamCount; ++i) {
        gParamValues[i] = gParamValues[i + shift_count];
    }
    for (int i = gParamCount - shift_count; i < gParamCount; ++i) {
        if (i >= 0) {
            gParamValues[i] = NULL;
        }
    }
    gParamCount -= shift_count;
    if (gParamCount < 0) {
        gParamCount = 0;
    }
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellSetenv(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        void *walk = NULL;
        for (char **env = pscalEnvironWalkBegin(&walk); env && *env; ++env) {
            const char *eq = strchr(*env, '=');
            char *name = strndup(*env, eq ? (size_t)(eq - *env) : strlen(*env));
            if (name && !shellExportIsHidden(name) && !shellArrayRegistryFindConst(name)) {
                puts(*env);
            }
            free(name);
        }
        pscalEnvironWalkEnd(walk);
        shellUpdateStatus(0);
        return makeVoid();
    }
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "setenv: expected NAME [VALUE]");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0]) || AS_STRING(args[0])[0] == '\0') {
        runtimeError(vm, "setenv: variable name must be a non-empty string");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (!shellIsValidEnvName(AS_STRING(args[0]))) {
        runtimeError(vm, "setenv: invalid variable name '%s'", AS_STRING(args[0]));
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (strchr(AS_STRING(args[0]), '=')) {
        runtimeError(vm, "setenv: variable name must not contain '='");
        shellUpdateStatus(1);
        return makeVoid();
    }
    const char *value = "";
    if (arg_count > 1) {
        if (VALUE_TYPE(args[1]) != TYPE_STRING || !AS_STRING(args[1])) {
            runtimeError(vm, "setenv: value must be a string");
            shellUpdateStatus(1);
            return makeVoid();
        }
        value = AS_STRING(args[1]);
    }
    if (!shellSetTrackedVariable(AS_STRING(args[0]), value, false)) {
        runtimeError(vm, "setenv: unable to set '%s': %s", AS_STRING(args[0]), strerror(errno));
        shellUpdateStatus(1);
        return makeVoid();
    }
    shellVariableSetExported(AS_STRING(args[0]), true);
    shellUpdateStatus(0);
    return makeVoid();
}

static ShellBindOption *shellBindFindOption(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellBindOptionCount; ++i) {
        ShellBindOption *entry = &gShellBindOptions[i];
        if (entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static bool shellBindSetOptionOwned(char *name, char *value) {
    if (!name) {
        free(value);
        return false;
    }
    if (!value) {
        value = strdup("");
        if (!value) {
            free(name);
            return false;
        }
    }
    ShellBindOption *existing = shellBindFindOption(name);
    if (existing) {
        free(existing->value);
        existing->value = value;
        free(name);
        return true;
    }
    ShellBindOption *resized = (ShellBindOption *)realloc(
        gShellBindOptions, (gShellBindOptionCount + 1) * sizeof(ShellBindOption));
    if (!resized) {
        free(name);
        free(value);
        return false;
    }
    gShellBindOptions = resized;
    gShellBindOptions[gShellBindOptionCount].name = name;
    gShellBindOptions[gShellBindOptionCount].value = value;
    gShellBindOptionCount++;
    return true;
}

static void shellBindPrintOptions(void) {
    for (size_t i = 0; i < gShellBindOptionCount; ++i) {
        ShellBindOption *entry = &gShellBindOptions[i];
        if (!entry->name) {
            continue;
        }
        const char *value = entry->value ? entry->value : "";
        printf("set %s %s\n", entry->name, value);
    }
}

static ShellOptionEntry *shellShoptFindOption(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellOptionCount; ++i) {
        ShellOptionEntry *entry = &gShellOptions[i];
        if (entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static bool shellShoptOptionEnabled(const char *name) {
    ShellOptionEntry *entry = shellShoptFindOption(name);
    return entry && entry->enabled;
}

static void shellShoptPrintEntry(const ShellOptionEntry *entry) {
    if (!entry || !entry->name) {
        return;
    }
    printf("%s\t%s\n", entry->name, entry->enabled ? "on" : "off");
}

static void shellShoptPrintEntryAsCommand(const ShellOptionEntry *entry) {
    if (!entry || !entry->name) {
        return;
    }
    printf("shopt -%c %s\n", entry->enabled ? 's' : 'u', entry->name);
}

Value vmBuiltinShellBind(VM *vm, int arg_count, Value *args) {
    bool ok = true;
    bool print_bindings = false;
    int index = 0;
    bool parsing_options = true;
    bool interactive = shellRuntimeIsInteractive();
    while (index < arg_count && parsing_options && ok) {
        Value v = args[index];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "bind: arguments must be strings");
            ok = false;
            break;
        }
        const char *token = AS_STRING(v);
        if (strcmp(token, "--") == 0) {
            parsing_options = false;
            index++;
            break;
        }
        if (token[0] == '-' && token[1] != '\0') {
            size_t len = strlen(token);
            for (size_t i = 1; i < len && ok; ++i) {
                char opt = token[i];
                if (opt == 'p') {
                    print_bindings = true;
                } else {
                    runtimeError(vm, "bind: unsupported option '-%c'", opt);
                    ok = false;
                    break;
                }
            }
            index++;
            continue;
        }
        parsing_options = false;
    }

    for (; ok && index < arg_count; ++index) {
        Value v = args[index];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "bind: arguments must be strings");
            ok = false;
            break;
        }
        const char *text = AS_STRING(v);
        if (strncmp(text, "set", 3) == 0) {
            const char *cursor = text + 3;
            if (*cursor && !isspace((unsigned char)*cursor)) {
                continue;
            }
            while (*cursor && isspace((unsigned char)*cursor)) {
                cursor++;
            }
            if (*cursor == '\0') {
                runtimeError(vm, "bind: expected readline option name");
                ok = false;
                break;
            }
            const char *name_start = cursor;
            while (*cursor && !isspace((unsigned char)*cursor)) {
                cursor++;
            }
            size_t name_len = (size_t)(cursor - name_start);
            while (*cursor && isspace((unsigned char)*cursor)) {
                cursor++;
            }
            const char *value_start = cursor;
            const char *value_end = value_start + strlen(value_start);
            while (value_end > value_start && isspace((unsigned char)value_end[-1])) {
                value_end--;
            }
            size_t value_len = (size_t)(value_end - value_start);
            char *name_copy = strndup(name_start, name_len);
            char *value_copy = strndup(value_start, value_len);
            if (!name_copy || !value_copy) {
                free(name_copy);
                free(value_copy);
                runtimeError(vm, "bind: out of memory");
                ok = false;
                break;
            }
            if (!shellBindSetOptionOwned(name_copy, value_copy)) {
                runtimeError(vm, "bind: out of memory");
                ok = false;
                break;
            }
        }
    }

    if (ok && print_bindings) {
        shellBindPrintOptions();
    }

    bool requires_interactive = shellBindRequiresInteractive();

    if (ok && !interactive && requires_interactive) {
        const char *script = shellRuntimeGetArg0();
        if (!script || !*script) {
            script = "exsh";
        }
        int line = shellRuntimeCurrentCommandLine();
        if (line > 0) {
            fprintf(stderr, "%s: line %d: bind: warning: line editing not enabled\n", script, line);
        } else {
            fprintf(stderr, "%s: bind: warning: line editing not enabled\n", script);
        }
    }

    int status = ok ? 0 : 1;
    shellUpdateStatus(status);
    return makeVoid();
}

Value vmBuiltinShellShopt(VM *vm, int arg_count, Value *args) {
    bool ok = true;
    bool quiet = false;
    bool print_format = false;
    bool parsing_options = true;
    int mode = -1; // -1 query, 0 unset, 1 set
    bool restrict_set_options = false;
    int index = 0;

    while (index < arg_count && parsing_options && ok) {
        Value v = args[index];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "shopt: option names must be strings");
            ok = false;
            break;
        }
        const char *token = AS_STRING(v);
        if (strcmp(token, "--") == 0) {
            parsing_options = false;
            index++;
            break;
        }
        if (token[0] == '-' && token[1] != '\0') {
            size_t len = strlen(token);
            for (size_t i = 1; i < len && ok; ++i) {
                char opt = token[i];
                switch (opt) {
                    case 's':
                        mode = 1;
                        break;
                    case 'u':
                        mode = 0;
                        break;
                    case 'q':
                        quiet = true;
                        break;
                    case 'p':
                        print_format = true;
                        break;
                    case 'o':
                        restrict_set_options = true;
                        break;
                    default:
                        runtimeError(vm, "shopt: invalid option '-%c'", opt);
                        ok = false;
                        break;
                }
            }
            index++;
            continue;
        }
        parsing_options = false;
    }

    if (!ok) {
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (restrict_set_options) {
        runtimeError(vm, "shopt: -o is not supported");
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (index >= arg_count) {
        if (mode == 1 || mode == 0) {
            for (size_t i = 0; i < gShellOptionCount; ++i) {
                ShellOptionEntry *entry = &gShellOptions[i];
                if ((mode == 1 && entry->enabled) || (mode == 0 && !entry->enabled)) {
                    if (print_format) {
                        shellShoptPrintEntryAsCommand(entry);
                    } else {
                        shellShoptPrintEntry(entry);
                    }
                }
            }
            shellUpdateStatus(0);
            return makeVoid();
        }
        if (quiet) {
            shellUpdateStatus(0);
            return makeVoid();
        }
        for (size_t i = 0; i < gShellOptionCount; ++i) {
            ShellOptionEntry *entry = &gShellOptions[i];
            if (print_format) {
                shellShoptPrintEntryAsCommand(entry);
            } else {
                shellShoptPrintEntry(entry);
            }
        }
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool query_only = (mode == -1);
    bool all_set = true;

    for (; ok && index < arg_count; ++index) {
        Value v = args[index];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "shopt: option names must be strings");
            ok = false;
            break;
        }
        ShellOptionEntry *entry = shellShoptFindOption(AS_STRING(v));
        if (!entry) {
            runtimeError(vm, "shopt: %s: invalid shell option name", AS_STRING(v));
            ok = false;
            break;
        }
        if (query_only) {
            if (!entry->enabled) {
                all_set = false;
            }
            if (!quiet) {
                if (print_format) {
                    shellShoptPrintEntryAsCommand(entry);
                } else {
                    shellShoptPrintEntry(entry);
                }
            }
        } else {
            entry->enabled = (mode == 1);
        }
    }

    if (!ok) {
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (query_only) {
        shellUpdateStatus(all_set ? 0 : 1);
    } else {
        shellUpdateStatus(0);
    }
    return makeVoid();
}

static void shellDeclarePrintQuoted(const char *value) {
    fputc('"', stdout);
    for (const char *cursor = value; *cursor; ++cursor) {
        if (strchr("\"\\$`", *cursor)) {
            fputc('\\', stdout);
        }
        fputc(*cursor, stdout);
    }
    fputc('"', stdout);
}

/* Control characters only: the rest of a value prints as it is. */
static bool shellValueHasControlChar(const char *value) {
    for (const char *cursor = value; *cursor; ++cursor) {
        unsigned char c = (unsigned char)*cursor;
        if (c < 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

static char *shellAnsiQuoteValue(const char *value) {
    char *quoted = (char *)malloc(strlen(value) * 4 + 4);
    if (!quoted) {
        return NULL;
    }
    char *out = quoted;
    *out++ = '$';
    *out++ = '\'';
    for (const char *cursor = value; *cursor; ++cursor) {
        unsigned char c = (unsigned char)*cursor;
        const char *escapes = "\033E\aa\bb\ff\nn\rr\tt\vv\\\\''";
        const char *found = NULL;
        for (const char *e = escapes; *e && !found; e += 2) {
            found = (unsigned char)*e == c ? e : NULL;
        }
        if (found) {
            *out++ = '\\';
            *out++ = found[1];
        } else if (c < 0x20 || c == 0x7f) {
            out += sprintf(out, "\\%03o", c);
        } else {
            *out++ = (char)c;
        }
    }
    *out++ = '\'';
    *out = '\0';
    return quoted;
}

/* A value as `set` shows it: quoted only if the shell would read something into it. */
static char *shellSetQuoteValue(const char *value) {
    if (shellValueHasControlChar(value)) {
        return shellAnsiQuoteValue(value);
    }
    bool metas = false;
    size_t quotes = 0;
    for (const char *cursor = value; *cursor; ++cursor) {
        if (strchr(" \"\\|&;()<>!{}*[?]^$`", *cursor)) {
            metas = true;
        } else if (*cursor == '\'') {
            metas = true;
            quotes++;
        } else if (*cursor == '~' && (cursor == value || cursor[-1] == '=' || cursor[-1] == ':')) {
            metas = true;
        } else if (*cursor == '#' && cursor == value) {
            metas = true;
        }
    }
    if (!metas) {
        return strdup(value);
    }
    char *quoted = (char *)malloc(strlen(value) + quotes * 3 + 3);
    if (!quoted) {
        return NULL;
    }
    char *out = quoted;
    *out++ = '\'';
    for (const char *cursor = value; *cursor; ++cursor) {
        if (*cursor == '\'') {
            memcpy(out, "'\\''", 4);
            out += 4;
        } else {
            *out++ = *cursor;
        }
    }
    *out++ = '\'';
    *out = '\0';
    return quoted;
}

static void shellDeclarePrintArrayValues(const ShellArrayVariable *array_var) {
    fputc('(', stdout);
    for (size_t i = 0; i < array_var->count; ++i) {
        const char *element = array_var->values[i] ? array_var->values[i] : "";
        if (i > 0) {
            fputc(' ', stdout);
        }
        if (array_var->keys && array_var->keys[i]) {
            printf("[%s]=", array_var->keys[i]);
        } else {
            printf("[%zu]=", i);
        }
        if (shellValueHasControlChar(element)) {
            char *quoted = shellAnsiQuoteValue(element);
            if (quoted) {
                fputs(quoted, stdout);
                free(quoted);
            }
        } else {
            shellDeclarePrintQuoted(element);
        }
    }
    fputc(')', stdout);
}

/* The attributes declare -p shows for a name, and declare's listings choose by. */
static size_t shellDeclareFlags(const char *name, const ShellArrayVariable *array_var, char *flags) {
    size_t count = 0;
    if (array_var) {
        flags[count++] = (array_var->kind == SHELL_ARRAY_KIND_ASSOCIATIVE && !array_var->sparse) ? 'A' : 'a';
    }
    if (shellVariableHasAttribute(name, SHELL_VAR_ATTR_INTEGER)) {
        flags[count++] = 'i';
    }
    if (shellVariableHasAttribute(name, SHELL_VAR_ATTR_LOWER)) {
        flags[count++] = 'l';
    }
    if (shellReadonlyContains(name)) {
        flags[count++] = 'r';
    }
    if (shellVariableHasAttribute(name, SHELL_VAR_ATTR_UPPER)) {
        flags[count++] = 'u';
    }
    if (!array_var && shellVariableIsExported(name)) {
        flags[count++] = 'x';
    }
    flags[count] = '\0';
    return count;
}

/* declare -p: a line that sets it back. False if it is not there. */
static bool shellDeclarePrintVariable(const char *name) {
    bool is_set = false;
    char *value = shellLookupParameterValueInternal(name, strlen(name), &is_set);
    const ShellArrayVariable *array_var = shellArrayRegistryFindConst(name);
    /* Declared with no value yet. */
    bool declared = shellExportIsPending(name) || shellLocalScopeFind(name) != NULL;
    if (!array_var && !is_set && !declared) {
        free(value);
        return false;
    }
    char flags[8];
    size_t count = shellDeclareFlags(name, array_var, flags);
    printf("declare -%s %s", count ? flags : "-", name);
    if (array_var) {
        fputc('=', stdout);
        shellDeclarePrintArrayValues(array_var);
    } else if (is_set) {
        fputc('=', stdout);
        shellDeclarePrintQuoted(value);
    }
    fputc('\n', stdout);
    free(value);
    return true;
}

static void shellExportListAdd(char ***names, size_t *count, size_t *capacity, const char *name, size_t len);

/* Bash's byte order. A name it has set by the time a script starts counts if it has a value
 * here, though nothing in the environment holds it. */
static char **shellVariableNamesSorted(size_t *out_count) {
    static const char *const kShellOwnVariables[] = {
        "BASH_VERSINFO", "BASH_VERSION", "EUID", "HOSTNAME", "HOSTTYPE", "IFS", "MACHTYPE", "OPTERR", "OPTIND",
        "OSTYPE", "PPID", "PS4", "PWD", "UID", "_"
    };
    char **names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    void *walk = NULL;
    for (char **env = pscalEnvironWalkBegin(&walk); env && *env; ++env) {
        const char *eq = strchr(*env, '=');
        if (eq && strncmp(*env, "EXSH_LAST_STATUS=", 17) != 0) {
            shellExportListAdd(&names, &count, &capacity, *env, (size_t)(eq - *env));
        }
    }
    pscalEnvironWalkEnd(walk);
    SHELL_EXPORT_LOCK();
    const ShellNameSet *pending = &shellExportFlagsCurrent()->pending;
    for (size_t i = 0; i < pending->count; ++i) {
        shellExportListAdd(&names, &count, &capacity, pending->names[i], strlen(pending->names[i]));
    }
    SHELL_EXPORT_UNLOCK();
    for (size_t depth = 0; depth < gShellLocalScopeDepth; ++depth) {
        const ShellLocalScopeFrame *frame = &gShellLocalScopeStack[depth];
        for (size_t i = 0; i < frame->count; ++i) {
            if (frame->variables[i].name) {
                shellExportListAdd(&names, &count, &capacity, frame->variables[i].name,
                                   strlen(frame->variables[i].name));
            }
        }
    }
    for (size_t i = 0; i < sizeof(kShellOwnVariables) / sizeof(kShellOwnVariables[0]); ++i) {
        const char *name = kShellOwnVariables[i];
        bool is_set = false;
        free(shellLookupParameterValueInternal(name, strlen(name), &is_set));
        if (is_set) {
            shellExportListAdd(&names, &count, &capacity, name, strlen(name));
        }
    }
    for (size_t i = 0; i < gShellArrayVarCount; ++i) {
        if (gShellArrayVars[i].name) {
            shellExportListAdd(&names, &count, &capacity, gShellArrayVars[i].name, strlen(gShellArrayVars[i].name));
        }
    }
    if (count > 1) {
        qsort(names, count, sizeof(char *), shellParamCompareStrings);
    }
    *out_count = count;
    return names;
}

/* declare -p: all of them. With attribute letters, those that have any of them. */
static void shellDeclarePrintAll(const char *wanted) {
    size_t count = 0;
    char **names = shellVariableNamesSorted(&count);
    for (size_t i = 0; i < count; ++i) {
        char flags[8];
        shellDeclareFlags(names[i], shellArrayRegistryFindConst(names[i]), flags);
        if (!wanted || strpbrk(flags, wanted)) {
            shellDeclarePrintVariable(names[i]);
        }
        free(names[i]);
    }
    free(names);
}

/* `set` and `declare` alone, one line each that sets it back. What has no value stays out. */
static void shellSetPrintAll(void) {
    size_t count = 0;
    char **names = shellVariableNamesSorted(&count);
    for (size_t i = 0; i < count; ++i) {
        const char *name = names[i];
        bool is_set = false;
        char *value = shellLookupParameterValueInternal(name, strlen(name), &is_set);
        const ShellArrayVariable *array_var = shellArrayRegistryFindConst(name);
        if (array_var) {
            printf("%s=", name);
            shellDeclarePrintArrayValues(array_var);
            fputc('\n', stdout);
        } else if (is_set && value) {
            char *quoted = shellSetQuoteValue(value);
            printf("%s=%s\n", name, quoted ? quoted : "");
            free(quoted);
        }
        free(value);
        free(names[i]);
    }
    free(names);
    fflush(stdout);
}

static void shellDeclarePrintFunctions(void) {
    pthread_mutex_lock(&gShellFunctionTableMutex);
    for (size_t i = 0; i < gShellFunctionCount; ++i) {
        if (gShellFunctions[i].name) {
            printf("declare -f %s\n", gShellFunctions[i].name);
        }
    }
    pthread_mutex_unlock(&gShellFunctionTableMutex);
}

Value vmBuiltinShellDeclare(VM *vm, int arg_count, Value *args) {
    const char *builtin_name = shellRuntimeCurrentCommandName();
    if (!builtin_name || !*builtin_name) {
        builtin_name = (vm && vm->current_builtin_name) ? vm->current_builtin_name : "declare";
    }
    bool ok = true;
    bool associative = false;
    bool global_scope = false;
    bool mark_readonly = false;
    bool integer_attribute = false;
    bool indexed_array = false;
    bool mark_exported = false;
    bool mark_unexported = false;
    bool print_mode = false;
    bool functions_mode = false;
    int trace_attribute = 0;
    bool function_names_only = false;
    unsigned set_attributes = 0;
    unsigned clear_attributes = 0;
    bool in_function_scope = shellLocalScopeDepth() > 0;
    bool request_local_scope = gShellRuntime.local_scope_active;
    if (in_function_scope) {
        request_local_scope = true;
    }
    int exit_status = 0;
    int index = 0;
    while (index < arg_count) {
        Value v = args[index];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            break;
        }
        const char *token = AS_STRING(v);
        if (strcmp(token, "--") == 0) {
            index++;
            break;
        }
        if ((token[0] != '-' && token[0] != '+') || token[1] == '\0') {
            break;
        }
        for (size_t i = 1; token[i] != '\0'; ++i) {
            char opt = token[i];
            if (opt == 'A' && token[0] == '-') {
                if (!shellAssociativeArraysSupported()) {
                    shellReportRecoverableError(vm, true, "declare: -%c: invalid option", opt);
                    shellReportRecoverableError(
                        vm,
                        true,
                        "declare: usage: declare [-afFirtx] [-p] [name[=value] ...]");
                    if (exit_status == 0) {
                        exit_status = 2;
                    }
                    continue;
                }
                associative = true;
            } else if (opt == 'A' && token[0] == '+') {
                associative = false;
            } else if (opt == 'g' && token[0] == '-') {
                global_scope = true;
            } else if (opt == 'g' && token[0] == '+') {
                global_scope = false;
            } else if (opt == 'r' && token[0] == '-') {
                mark_readonly = true;
            } else if (opt == 'r' && token[0] == '+') {
                runtimeError(vm, "declare: -%c: unsupported option", opt);
                ok = false;
                break;
            } else if (opt == 'i' && token[0] == '-') {
                integer_attribute = true;
            } else if (opt == 'i' && token[0] == '+') {
                integer_attribute = false;
            } else if (opt == 'a') {
                indexed_array = token[0] == '-';
            } else if (opt == 'x') {
                if (token[0] == '-') {
                    mark_exported = true;
                } else {
                    mark_unexported = true;
                }
            } else if (opt == 'l' || opt == 'u') {
                unsigned flag = opt == 'l' ? SHELL_VAR_ATTR_LOWER : SHELL_VAR_ATTR_UPPER;
                if (token[0] == '-') {
                    set_attributes |= flag;
                } else {
                    clear_attributes |= flag;
                }
            } else if (opt == 'p' && token[0] == '-') {
                print_mode = true;
            } else if (opt == 'f') {
                /* `+f` is taken and means nothing, and `+ft` leaves a function traced. */
                functions_mode = functions_mode || token[0] == '-';
                if (token[0] == '+') {
                    break;
                }
            } else if (opt == 'F' && token[0] == '-') {
                functions_mode = true;
                function_names_only = true;
            } else if (opt == 't') {
                /* On a function: it sees the caller's RETURN trap. */
                trace_attribute = token[0] == '-' ? 1 : -1;
            } else if (opt == 'n') {
                /* Accepted: nothing here uses it. */
            } else {
                shellReportRecoverableError(vm, true, "%s: -%c: invalid option", builtin_name, opt);
                ok = false;
                exit_status = 2;
                break;
            }
        }
        if (!ok) {
            break;
        }
        index++;
    }

    /* No names: `declare` is `set`, and an attribute asks for those that have it. */
    char wanted_flags[8] = "";
    if (ok && index >= arg_count && !functions_mode && exit_status == 0 && !global_scope) {
        size_t wanted = 0;
        if (associative) {
            wanted_flags[wanted++] = 'A';
        }
        if (indexed_array) {
            wanted_flags[wanted++] = 'a';
        }
        if (integer_attribute) {
            wanted_flags[wanted++] = 'i';
        }
        if (set_attributes & SHELL_VAR_ATTR_LOWER) {
            wanted_flags[wanted++] = 'l';
        }
        if (mark_readonly) {
            wanted_flags[wanted++] = 'r';
        }
        if (set_attributes & SHELL_VAR_ATTR_UPPER) {
            wanted_flags[wanted++] = 'u';
        }
        if (mark_exported) {
            wanted_flags[wanted++] = 'x';
        }
        wanted_flags[wanted] = '\0';
        if (wanted > 0) {
            print_mode = true;
        } else if (!print_mode && !mark_unexported && !clear_attributes) {
            shellSetPrintAll();
            gShellRuntime.local_scope_active = false;
            shellUpdateStatus(0);
            return makeVoid();
        }
    }

    if (ok && functions_mode && !print_mode && trace_attribute != 0 && index < arg_count) {
        int trace_status = 0;
        for (int i = index; i < arg_count; ++i) {
            const char *name = VALUE_TYPE(args[i]) == TYPE_STRING ? AS_STRING(args[i]) : NULL;
            if (!name || !shellFunctionSetTraced(name, trace_attribute > 0)) {
                trace_status = 1;
            }
        }
        gShellRuntime.local_scope_active = false;
        shellUpdateStatus(trace_status);
        return makeVoid();
    }

    if (ok && (print_mode || functions_mode)) {
        int print_status = 0;
        bool printed_any = false;
        for (int i = index; i < arg_count; ++i) {
            if (VALUE_TYPE(args[i]) != TYPE_STRING || !AS_STRING(args[i])) {
                continue;
            }
            printed_any = true;
            const char *name = AS_STRING(args[i]);
            if (functions_mode) {
                if (shellFunctionIsDefined(name)) {
                    printf("%s\n", function_names_only ? name : name);
                } else {
                    print_status = 1;
                }
            } else if (!shellDeclarePrintVariable(name)) {
                shellReportRecoverableError(vm, true, "%s: %s: not found", builtin_name, name);
                print_status = 1;
            }
        }
        if (!printed_any) {
            if (functions_mode) {
                shellDeclarePrintFunctions();
            } else {
                shellDeclarePrintAll(wanted_flags[0] ? wanted_flags : NULL);
            }
        }
        fflush(stdout);
        gShellRuntime.local_scope_active = false;
        shellUpdateStatus(print_status);
        return makeVoid();
    }

    bool use_local_scope = false;
    if (request_local_scope && !in_function_scope) {
        runtimeError(vm, "%s: can only be used in functions", builtin_name);
        ok = false;
    } else if (request_local_scope && in_function_scope && !global_scope) {
        use_local_scope = true;
    }

    for (; index < arg_count && ok; ++index) {
        Value v = args[index];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "declare: expected string argument");
            ok = false;
            break;
        }
        const char *spec = AS_STRING(v);
        const char *eq = strchr(spec, '=');
        {
            char *attribute_name = strndup(spec, eq ? (size_t)(eq - spec) : strlen(spec));
            if (attribute_name) {
                unsigned attributes_on = set_attributes | (integer_attribute ? SHELL_VAR_ATTR_INTEGER : 0u);
                if (use_local_scope && (attributes_on || clear_attributes)) {
                    ShellLocalScopeFrame *top = &gShellLocalScopeStack[gShellLocalScopeDepth - 1];
                    ShellLocalVariable *local = shellLocalScopeFrameFind(top, attribute_name);
                    if (!local && shellLocalScopeSetOnCurrentFrame(attribute_name, "", false)) {
                        local = shellLocalScopeFrameFind(top, attribute_name);
                        local->no_value = true;
                    }
                    if (local) {
                        local->attributes = (local->attributes & ~clear_attributes) | attributes_on;
                    }
                } else if (!use_local_scope) {
                    shellVariableSetAttribute(attribute_name, attributes_on, clear_attributes);
                }
                /* A local is not there yet: see the end of this loop's two branches. */
                if (!use_local_scope) {
                    if (mark_exported) {
                        shellVariableSetExported(attribute_name, true);
                    } else if (mark_unexported) {
                        shellVariableSetExported(attribute_name, false);
                    }
                }
                free(attribute_name);
            }
        }
        if (!eq && indexed_array && !use_local_scope && !shellArrayRegistryFindConst(spec) && !shellSafeGetenv(spec)) {
            /* declare -a x: an array with nothing in it yet. */
            shellArrayRegistryStore(spec, NULL, NULL, 0, SHELL_ARRAY_KIND_INDEXED);
            continue;
        }
        if (!eq) {
            if (associative) {
                if (use_local_scope) {
                    runtimeError(vm, "%s: local associative arrays are not supported", builtin_name);
                    ok = false;
                } else {
                    if (!shellArrayRegistryInitializeAssociative(spec)) {
                        runtimeError(vm, "declare: unable to initialise '%s'", spec);
                        ok = false;
                    } else if (setenv(spec, "", 1) != 0) {
                        runtimeError(vm, "declare: unable to set '%s'", spec);
                        ok = false;
                    } else {
                        /* Its copy there is the shell's own, also while a local hides the array. */
                        shellExportMarkUnexported(spec);
                    }
                }
            } else {
                if (use_local_scope) {
                    /* One already in this function keeps its value. */
                    ShellLocalVariable *local =
                        shellLocalScopeFrameFind(&gShellLocalScopeStack[gShellLocalScopeDepth - 1], spec);
                    if (!local) {
                        if (shellLocalScopeSetOnCurrentFrame(spec, "", false)) {
                            local = shellLocalScopeFind(spec);
                            local->no_value = true;
                        } else {
                            runtimeError(vm, "declare: out of memory");
                            ok = false;
                        }
                    }
                    if (local && (mark_exported || mark_unexported)) {
                        local->exported = mark_exported;
                    }
                } else {
                    // Without an assignment, bash preserves the current value of global
                    // variables when applying attributes. Leave the value untouched so
                    // constructs like `typeset -i var` do not reset an existing binding.
                    if (shellReadonlyContains(spec)) {
                        runtimeError(vm, "declare: %s: readonly variable", spec);
                        ok = false;
                    } else {
                        const ShellArrayVariable *array_var = shellArrayRegistryFindConst(spec);
                        bool exists = (array_var != NULL);
                        if (!exists) {
                            const char *existing = shellSafeGetenv(spec);
                            exists = (existing != NULL);
                        }
                        if (!exists) {
                            errno = 0;
                            if (!shellSetTrackedVariable(spec, "", false)) {
                                if (errno != 0) {
                                    runtimeError(vm,
                                                "declare: unable to set '%s': %s",
                                                spec,
                                                strerror(errno));
                                } else {
                                    runtimeError(vm, "declare: unable to set '%s'", spec);
                                }
                                ok = false;
                            }
                        }
                    }
                }
            }
            if (ok && mark_readonly) {
                if (!shellReadonlyAdd(spec)) {
                    runtimeError(vm, "declare: out of memory");
                    ok = false;
                }
            }
            continue;
        }
        size_t name_len = (size_t)(eq - spec);
        char *name = (char *)malloc(name_len + 1);
        if (!name) {
            ok = false;
            break;
        }
        memcpy(name, spec, name_len);
        name[name_len] = '\0';
        const char *value_text = eq + 1;
        char *coerced_value = NULL;
        const char *value_to_store = value_text;
        bool array_literal = value_text[0] == SHELL_ARRAY_LITERAL_MARK;
        if (array_literal) {
            value_text++;
            value_to_store = value_text;
        } else if (!integer_attribute) {
            char *attributed = shellApplyVariableAttributes(name, value_text);
            if (attributed) {
                coerced_value = attributed;
                value_to_store = coerced_value;
            }
        }
        if (integer_attribute && !array_literal) {
            long long number = 0;
            if (!shellArithmeticEvaluateText(value_text, 0, &number, builtin_name)) {
                shellArithmeticExpansionFail();
                if (exit_status == 0) {
                    exit_status = 1;
                }
                free(name);
                ok = false;
                break;
            }
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%lld", number);
            coerced_value = strdup(buffer);
            if (!coerced_value) {
                runtimeError(vm, "declare: out of memory");
                free(name);
                ok = false;
                break;
            }
            value_to_store = coerced_value;
        }
        if (associative) {
            if (use_local_scope) {
                runtimeError(vm, "%s: local associative arrays are not supported", builtin_name);
                free(coerced_value);
                free(name);
                ok = false;
                break;
            }
            if (!shellArrayRegistryInitializeAssociative(name)) {
                runtimeError(vm, "declare: unable to initialise '%s'", name);
                free(coerced_value);
                free(name);
                ok = false;
                break;
            }
            if (!shellSetTrackedVariable(name, value_to_store, true)) {
                const char *readonly = shellReadonlyGetErrorName();
                if (errno == EPERM && readonly && *readonly) {
                    runtimeError(vm, "declare: %s: readonly variable", readonly);
                } else if (errno != 0) {
                    runtimeError(vm, "declare: unable to set '%s': %s", name, strerror(errno));
                } else {
                    runtimeError(vm, "declare: unable to set '%s'", name);
                }
                free(coerced_value);
                free(name);
                ok = false;
                break;
            }
        } else {
            if (use_local_scope) {
                if (!shellLocalScopeSetOnCurrentFrame(name, value_to_store, array_literal)) {
                    runtimeError(vm, "declare: out of memory");
                    free(coerced_value);
                    free(name);
                    ok = false;
                    break;
                }
                if (mark_exported || mark_unexported) {
                    shellLocalScopeFind(name)->exported = mark_exported;
                }
            } else if (!shellSetTrackedVariable(name, value_to_store, array_literal)) {
                const char *readonly = shellReadonlyGetErrorName();
                if (errno == EPERM && readonly && *readonly) {
                    runtimeError(vm, "declare: %s: readonly variable", readonly);
                } else if (errno != 0) {
                    runtimeError(vm, "declare: unable to set '%s': %s", name, strerror(errno));
                } else {
                    runtimeError(vm, "declare: unable to set '%s'", name);
                }
                free(coerced_value);
                free(name);
                ok = false;
                break;
            }
            /* A subscript in its literal has reported, and the rest goes. */
            if (array_literal && gShellExpansionFailed) {
                if (exit_status == 0) {
                    exit_status = 1;
                }
                free(coerced_value);
                free(name);
                ok = false;
                break;
            }
        }
        if (ok && mark_readonly) {
            if (!shellReadonlyAdd(name)) {
                runtimeError(vm, "declare: out of memory");
                free(coerced_value);
                free(name);
                ok = false;
                break;
            }
        }
        free(name);
        if (coerced_value) {
            free(coerced_value);
        }
    }

    gShellRuntime.local_scope_active = false;

    if (!ok && exit_status == 0) {
        exit_status = 1;
    }
    shellUpdateStatus(exit_status);
    return makeVoid();
}

Value vmBuiltinShellReadonly(VM *vm, int arg_count, Value *args) {
    bool print_list = (arg_count == 0);
    bool parsing_options = true;
    bool processed_assignment = false;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "readonly: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = AS_STRING(v);
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (strcmp(text, "-p") == 0) {
                print_list = true;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                runtimeError(vm, "readonly: unsupported option '%s'", text);
                shellUpdateStatus(1);
                return makeVoid();
            }
            parsing_options = false;
        }
        processed_assignment = true;
        const char *eq = strchr(text, '=');
        if (eq) {
            size_t name_len = (size_t)(eq - text);
            if (name_len == 0) {
                runtimeError(vm, "readonly: invalid assignment '%s'", text);
                shellUpdateStatus(1);
                return makeVoid();
            }
            char *name = strndup(text, name_len);
            if (!name) {
                runtimeError(vm, "readonly: out of memory");
                shellUpdateStatus(1);
                return makeVoid();
            }
            if (!shellIsValidEnvName(name)) {
                runtimeError(vm, "readonly: invalid variable name '%s'", name);
                free(name);
                shellUpdateStatus(1);
                return makeVoid();
            }
            const char *value = eq + 1;
            bool literal = value[0] == SHELL_ARRAY_LITERAL_MARK;
            if (literal) {
                value++;
            }
            if (!shellSetTrackedVariable(name, value, literal)) {
                const char *readonly = shellReadonlyGetErrorName();
                if (errno == EPERM && readonly && *readonly) {
                    runtimeError(vm, "readonly: %s: readonly variable", readonly);
                } else if (errno != 0) {
                    runtimeError(vm, "readonly: unable to set '%s': %s", name, strerror(errno));
                } else {
                    runtimeError(vm, "readonly: unable to set '%s'", name);
                }
                free(name);
                shellUpdateStatus(1);
                return makeVoid();
            }
            if (!shellReadonlyAdd(name)) {
                runtimeError(vm, "readonly: out of memory");
                free(name);
                shellUpdateStatus(1);
                return makeVoid();
            }
            free(name);
            continue;
        }
        if (!shellIsValidEnvName(text)) {
            runtimeError(vm, "readonly: invalid variable name '%s'", text);
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (!shellReadonlyContains(text)) {
            const char *existing = shellSafeGetenv(text);
            if (!existing) {
                if (!shellSetTrackedVariable(text, "", false)) {
                    const char *readonly = shellReadonlyGetErrorName();
                    if (errno == EPERM && readonly && *readonly) {
                        runtimeError(vm, "readonly: %s: readonly variable", readonly);
                    } else if (errno != 0) {
                        runtimeError(vm, "readonly: unable to set '%s': %s", text, strerror(errno));
                    } else {
                        runtimeError(vm, "readonly: unable to set '%s'", text);
                    }
                    shellUpdateStatus(1);
                    return makeVoid();
                }
            }
        }
        if (!shellReadonlyAdd(text)) {
            runtimeError(vm, "readonly: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
    }

    if (print_list || (!processed_assignment && arg_count == 0)) {
        shellReadonlyPrintVariables();
    }

    shellUpdateStatus(0);
    return makeVoid();
}

static void shellCommandFreeResults(ShellCommandResult *results, size_t count) {
    if (!results) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(results[i].detail);
        results[i].detail = NULL;
    }
    free(results);
}

static bool shellCommandAppendResult(ShellCommandResult **results,
                                     size_t *count,
                                     ShellCommandResultKind kind,
                                     const char *detail) {
    if (!results || !count) {
        return false;
    }
    char *copy = NULL;
    if (detail && *detail) {
        copy = strdup(detail);
        if (!copy) {
            return false;
        }
    } else if (detail) {
        copy = strdup("");
        if (!copy) {
            return false;
        }
    }
    ShellCommandResult *resized =
        (ShellCommandResult *)realloc(*results, (*count + 1) * sizeof(ShellCommandResult));
    if (!resized) {
        free(copy);
        return false;
    }
    *results = resized;
    ShellCommandResult *entry = &resized[*count];
    entry->kind = kind;
    entry->detail = copy;
    (*count)++;
    return true;
}

static bool shellCommandExecutableExists(const char *path) {
    if (!path || !*path) {
        return false;
    }
    bool runnable = false;
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        return false;
    }
    runnable = (access(path, X_OK) == 0);
#if defined(PSCAL_TARGET_IOS)
    if (!runnable && (st.st_mode & 0111) != 0) {
        runnable = true;
    }
    if (!runnable && access(path, R_OK) == 0) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char header[2];
            ssize_t n = read(fd, header, sizeof(header));
            close(fd);
            if (n == 2 && header[0] == '#' && header[1] == '!') {
                runnable = true;
            }
        }
    }
#endif
    return runnable;
}

static bool shellCommandResolveExecutablePath(const char *path, char *resolved, size_t resolved_size) {
    if (!resolved || resolved_size == 0 || !path || !*path) {
        return false;
    }
    if (shellCommandExecutableExists(path)) {
#if defined(PSCAL_TARGET_IOS)
        if (pathTruncateStrip(path, resolved, resolved_size)) {
            return true;
        }
#endif
        int copied = snprintf(resolved, resolved_size, "%s", path);
        return copied >= 0 && (size_t)copied < resolved_size;
    }

#if defined(PSCAL_TARGET_IOS)
    /* When PATH contains host-side container paths, also probe their
     * app-root-visible equivalent ("/..."). */
    char stripped[PATH_MAX];
    if (pathTruncateStrip(path, stripped, sizeof(stripped)) &&
        strcmp(stripped, path) != 0 &&
        shellCommandExecutableExists(stripped)) {
        int copied = snprintf(resolved, resolved_size, "%s", stripped);
        return copied >= 0 && (size_t)copied < resolved_size;
    }
#endif
    return false;
}

static bool shellCommandResultHasFilePath(const ShellCommandResult *results,
                                          size_t count,
                                          const char *path) {
    if (!results || !path) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (results[i].kind != SHELL_COMMAND_RESULT_FILE || !results[i].detail) {
            continue;
        }
        if (strcmp(results[i].detail, path) == 0) {
            return true;
        }
    }
    return false;
}

static bool shellCommandEnumerateExecutables(const char *name,
                                             const char *path_env,
                                             bool find_all,
                                             ShellCommandResult **results,
                                             size_t *count) {
    if (!name || !*name) {
        return true;
    }
    if (strchr(name, '/')) {
        char resolved[PATH_MAX];
        if (shellCommandResolveExecutablePath(name, resolved, sizeof(resolved))) {
            if (!shellCommandAppendResult(results, count, SHELL_COMMAND_RESULT_FILE, resolved)) {
                return false;
            }
        }
        return true;
    }

    char *path_value = NULL;
    const char *env_value = path_env;
    if (!env_value) {
        path_value = shellLookupParameterValue("PATH", strlen("PATH"));
        if (path_value && *path_value) {
            env_value = path_value;
        } else {
            env_value = shellSafeGetenv("PATH");
        }
    }
    if (!env_value) {
        env_value = "";
    }
    char *copy = strdup(env_value);
    if (!copy) {
        free(path_value);
        return false;
    }

    bool ok = true;
    char *cursor = copy;
    while (true) {
        char *segment = cursor;
        char *sep = cursor ? strchr(cursor, ':') : NULL;
        if (sep) {
            *sep = '\0';
        }
        const char *dir = (segment && *segment) ? segment : ".";
        size_t dir_len = strlen(dir);
        size_t name_len = strlen(name);
        bool need_sep = dir_len > 0 && dir[dir_len - 1] != '/';
        size_t total_len = dir_len + (need_sep ? 1 : 0) + name_len;
        char *candidate = (char *)malloc(total_len + 1);
        if (!candidate) {
            ok = false;
            break;
        }
        size_t pos = 0;
        if (dir_len > 0) {
            memcpy(candidate + pos, dir, dir_len);
            pos += dir_len;
        }
        if (need_sep) {
            candidate[pos++] = '/';
        }
        memcpy(candidate + pos, name, name_len);
        pos += name_len;
        candidate[pos] = '\0';

        char resolved[PATH_MAX];
        if (shellCommandResolveExecutablePath(candidate, resolved, sizeof(resolved))) {
            if (!shellCommandResultHasFilePath(*results, *count, resolved) &&
                !shellCommandAppendResult(results, count, SHELL_COMMAND_RESULT_FILE, resolved)) {
                ok = false;
                free(candidate);
                break;
            }
            if (!find_all) {
                free(candidate);
                break;
            }
        }
        free(candidate);
        if (!sep) {
            break;
        }
        cursor = sep + 1;
        if (!cursor) {
            break;
        }
    }

#if defined(PSCAL_TARGET_IOS)
    /* Keep fallback probing in app-root namespace, not device-root paths. */
    if (find_all || *count == 0) {
        const char *fallback_dirs[] = { "/bin", "/Documents/bin" };
        for (size_t i = 0; i < sizeof(fallback_dirs) / sizeof(fallback_dirs[0]); ++i) {
            char candidate[PATH_MAX];
            int n = snprintf(candidate, sizeof(candidate), "%s/%s", fallback_dirs[i], name);
            char resolved[PATH_MAX];
            if (n > 0 && n < (int)sizeof(candidate) &&
                shellCommandResolveExecutablePath(candidate, resolved, sizeof(resolved))) {
                if (!shellCommandResultHasFilePath(*results, *count, resolved) &&
                    !shellCommandAppendResult(results, count, SHELL_COMMAND_RESULT_FILE, resolved)) {
                    free(copy);
                    free(path_value);
                    return false;
                }
            }
        }
    }
#endif

    free(copy);
    free(path_value);
    return ok;
}

static bool shellCommandCollectInfo(const char *name,
                                    bool collect_all_paths,
                                    bool use_default_path,
                                    ShellCommandResult **out_results,
                                    size_t *out_count) {
    if (out_results) {
        *out_results = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!name || !*name) {
        return true;
    }

    ShellCommandResult *results = NULL;
    size_t count = 0;

    /* These callers ask what would run, so an alias counts only while aliases expand. */
    ShellAlias *alias = shellShoptOptionEnabled("expand_aliases") ? shellFindAlias(name) : NULL;
    if (alias) {
        const char *value = alias->value ? alias->value : "";
        if (!shellCommandAppendResult(&results, &count, SHELL_COMMAND_RESULT_ALIAS, value)) {
            goto error;
        }
    }

    static const char *const kKeywords[] = {"if", "then", "elif", "else", "fi", "for", "while", "until", "do", "done",
                                            "case", "esac", "in", "function", "time", "select", "{", "}", "!", "[[", "]]"};
    for (size_t k = 0; k < sizeof(kKeywords) / sizeof(kKeywords[0]); ++k) {
        if (strcmp(name, kKeywords[k]) == 0) {
            if (!shellCommandAppendResult(&results, &count, SHELL_COMMAND_RESULT_KEYWORD, NULL)) {
                goto error;
            }
            break;
        }
    }

    if (shellFunctionIsDefined(name)) {
        if (!shellCommandAppendResult(&results, &count, SHELL_COMMAND_RESULT_FUNCTION, NULL)) {
            goto error;
        }
    }

    bool is_runtime_builtin = shellIsRuntimeBuiltin(name);
    const char *canonical = NULL;
    bool canonical_override = false;
    if (name && *name) {
        canonical = shellBuiltinCanonicalName(name);
        if (canonical && *canonical && strcmp(canonical, name) != 0 &&
            shellIsRuntimeBuiltin(canonical)) {
            canonical_override = true;
        }
    }
    bool applet_registered = false;
#if defined(PSCAL_TARGET_IOS)
    const SmallclueApplet *applet = smallclueFindApplet(name);
    if (applet) {
        applet_registered = smallclueIsRegisteredBuiltinName(name);
    }
    if (applet && applet_registered && !canonical_override) {
        if (!shellCommandAppendResult(&results, &count, SHELL_COMMAND_RESULT_APPLET, applet->name)) {
            goto error;
        }
    }
#endif

    if (is_runtime_builtin && (!applet_registered || canonical_override)) {
        const char *builtin_name = canonical ? canonical : name;
        if (!shellCommandAppendResult(&results, &count, SHELL_COMMAND_RESULT_BUILTIN, builtin_name)) {
            goto error;
        }
    }

    const char *path_env = use_default_path ? kShellCommandDefaultPath : NULL;
    if (!shellCommandEnumerateExecutables(name,
                                          path_env,
                                          collect_all_paths,
                                          &results,
                                          &count)) {
        goto error;
    }

    if (out_results) {
        *out_results = results;
    } else {
        shellCommandFreeResults(results, count);
    }
    if (out_count) {
        *out_count = count;
    }
    return true;

error:
    shellCommandFreeResults(results, count);
    if (out_results) {
        *out_results = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    return false;
}

static const char *shellCommandResultKindLabel(ShellCommandResultKind kind) {
    switch (kind) {
        case SHELL_COMMAND_RESULT_ALIAS:
            return "alias";
        case SHELL_COMMAND_RESULT_KEYWORD:
            return "keyword";
        case SHELL_COMMAND_RESULT_FUNCTION:
            return "function";
        case SHELL_COMMAND_RESULT_BUILTIN:
            return "builtin";
        case SHELL_COMMAND_RESULT_APPLET:
            return "smallclue applet";
        case SHELL_COMMAND_RESULT_FILE:
            return "file";
    }
    return "file";
}

static void shellCommandPrintVerbose(const char *name, const ShellCommandResult *result) {
    if (!name || !result) {
        return;
    }
    switch (result->kind) {
        case SHELL_COMMAND_RESULT_ALIAS:
            printf("%s is aliased to `%s'\n", name, result->detail ? result->detail : "");
            break;
        case SHELL_COMMAND_RESULT_KEYWORD:
            printf("%s is a shell keyword\n", name);
            break;
        case SHELL_COMMAND_RESULT_FUNCTION:
            printf("%s is a function\n", name);
            break;
        case SHELL_COMMAND_RESULT_BUILTIN:
            printf("%s is a shell builtin\n", name);
            break;
        case SHELL_COMMAND_RESULT_APPLET:
            printf("%s is a smallclue applet\n", name);
            break;
        case SHELL_COMMAND_RESULT_FILE:
            printf("%s is %s\n", name, result->detail ? result->detail : "");
            break;
    }
}

/* In a form that can be read back. Only `alias` itself puts `--` before a name starting with -. */
static void shellPrintAliasDefinition(const char *name, const char *value, bool guard_dash) {
    printf("alias %s%s='", guard_dash && name[0] == '-' ? "-- " : "", name);
    const char *rest = value ? value : "";
    const char *quote;
    while ((quote = strchr(rest, '\'')) != NULL) {
        printf("%.*s'\\''", (int)(quote - rest), rest);
        rest = quote + 1;
    }
    printf("%s'\n", rest);
}

static void shellCommandPrintShort(const char *name, const ShellCommandResult *result) {
    if (!name || !result) {
        return;
    }
    if (result->kind == SHELL_COMMAND_RESULT_FILE) {
        printf("%s\n", result->detail ? result->detail : "");
    } else if (result->kind == SHELL_COMMAND_RESULT_ALIAS) {
        shellPrintAliasDefinition(name, result->detail, false);
    } else {
        printf("%s\n", name);
    }
}

static ShellHashEntry *shellHashFindEntry(const char *name) {
    if (!name || !*name || !gShellRuntime.hash_entries) {
        return NULL;
    }
    for (size_t i = 0; i < gShellRuntime.hash_count; ++i) {
        ShellHashEntry *entry = &gShellRuntime.hash_entries[i];
        if (entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static bool shellHashEnsureCapacity(size_t min_capacity) {
    if (gShellRuntime.hash_capacity >= min_capacity) {
        return true;
    }
    size_t new_capacity = gShellRuntime.hash_capacity ? gShellRuntime.hash_capacity * 2 : 4;
    while (new_capacity < min_capacity) {
        size_t doubled = new_capacity * 2;
        if (doubled <= new_capacity) {
            new_capacity = min_capacity;
            break;
        }
        new_capacity = doubled;
    }
    ShellHashEntry *resized =
        (ShellHashEntry *)realloc(gShellRuntime.hash_entries, new_capacity * sizeof(ShellHashEntry));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellRuntime.hash_capacity; i < new_capacity; ++i) {
        resized[i].name = NULL;
        resized[i].path = NULL;
        resized[i].hits = 0;
    }
    gShellRuntime.hash_entries = resized;
    gShellRuntime.hash_capacity = new_capacity;
    return true;
}

static void shellHashClear(void) {
    if (!gShellRuntime.hash_entries) {
        gShellRuntime.hash_count = 0;
        return;
    }
    for (size_t i = 0; i < gShellRuntime.hash_count; ++i) {
        ShellHashEntry *entry = &gShellRuntime.hash_entries[i];
        free(entry->name);
        free(entry->path);
        entry->name = NULL;
        entry->path = NULL;
        entry->hits = 0;
    }
    gShellRuntime.hash_count = 0;
}

static bool shellHashResolveExecutable(const char *name, char **out_path) {
    if (out_path) {
        *out_path = NULL;
    }
    if (!name || !*name || strchr(name, '/')) {
        return false;
    }
    ShellCommandResult *results = NULL;
    size_t count = 0;
    if (!shellCommandCollectInfo(name, false, false, &results, &count)) {
        return false;
    }
    bool success = false;
    char *path_copy = NULL;
    for (size_t i = 0; i < count; ++i) {
        ShellCommandResult *res = &results[i];
        if (res->kind == SHELL_COMMAND_RESULT_FILE && res->detail && *res->detail) {
            path_copy = strdup(res->detail);
            if (path_copy) {
                success = true;
            }
            break;
        }
    }
    shellCommandFreeResults(results, count);
    if (!success) {
        free(path_copy);
        return false;
    }
    if (out_path) {
        *out_path = path_copy;
    } else {
        free(path_copy);
    }
    return true;
}

static bool shellHashAddName(const char *name) {
    if (!name || !*name) {
        return true;
    }
    if (strchr(name, '/')) {
        return true;
    }
    ShellCommandResult *results = NULL;
    size_t count = 0;
    if (!shellCommandCollectInfo(name, false, false, &results, &count)) {
        return false;
    }
    bool hashed = false;
    bool success = true;
    for (size_t i = 0; i < count; ++i) {
        ShellCommandResult *res = &results[i];
        if (res->kind == SHELL_COMMAND_RESULT_FILE && res->detail && *res->detail) {
            ShellHashEntry *entry = shellHashFindEntry(name);
            bool new_entry = (entry == NULL);
            char *path_copy = strdup(res->detail);
            if (!path_copy) {
                success = false;
                break;
            }
            if (new_entry) {
                if (!shellHashEnsureCapacity(gShellRuntime.hash_count + 1)) {
                    free(path_copy);
                    success = false;
                    break;
                }
                entry = &gShellRuntime.hash_entries[gShellRuntime.hash_count];
                char *name_copy = strdup(name);
                if (!name_copy) {
                    free(path_copy);
                    success = false;
                    break;
                }
                entry->name = name_copy;
                entry->path = path_copy;
                entry->hits = 0;
                gShellRuntime.hash_count++;
            } else {
                free(entry->path);
                entry->path = path_copy;
                entry->hits = 0;
            }
            hashed = true;
            break;
        }
    }
    shellCommandFreeResults(results, count);
    if (!success) {
        return false;
    }
    if (hashed) {
        return true;
    }
    return false;
}

static void shellHashPrintTable(void) {
    if (gShellRuntime.hash_count == 0) {
        printf("hash: hash table empty\n");
        return;
    }
    printf("hits\tcommand\n");
    for (size_t i = 0; i < gShellRuntime.hash_count; ++i) {
        ShellHashEntry *entry = &gShellRuntime.hash_entries[i];
        const char *path = entry->path ? entry->path : "";
        printf("%4lu\t%s\n", entry->hits, path);
    }
}

static void shellHashRecordExecution(const char *name) {
    if (!name || !*name) {
        return;
    }
    if (strchr(name, '/')) {
        return;
    }
    ShellHashEntry *entry = shellHashFindEntry(name);
    if (entry) {
        if (entry->path && access(entry->path, X_OK) == 0) {
            if (entry->hits < ULONG_MAX) {
                entry->hits += 1;
            }
            return;
        }
        char *resolved = NULL;
        if (!shellHashResolveExecutable(name, &resolved)) {
            return;
        }
        free(entry->path);
        entry->path = resolved;
        entry->hits = 1;
        return;
    }
    char *resolved = NULL;
    if (!shellHashResolveExecutable(name, &resolved)) {
        return;
    }
    if (!shellHashEnsureCapacity(gShellRuntime.hash_count + 1)) {
        free(resolved);
        return;
    }
    ShellHashEntry *slot = &gShellRuntime.hash_entries[gShellRuntime.hash_count];
    char *name_copy = strdup(name);
    if (!name_copy) {
        free(resolved);
        return;
    }
    slot->name = name_copy;
    slot->path = resolved;
    slot->hits = 1;
    gShellRuntime.hash_count++;
}

Value vmBuiltinShellExport(VM *vm, int arg_count, Value *args) {
    bool print_env = (arg_count == 0);
    bool parsing_options = true;
    bool remove_export = false;
    bool functions_only = false;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "export: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = AS_STRING(v);
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                for (const char *opt = text + 1; *opt; ++opt) {
                    if (*opt == 'p') {
                        print_env = true;
                    } else if (*opt == 'n') {
                        remove_export = true;
                    } else if (*opt == 'f') {
                        functions_only = true;
                    } else {
                        runtimeError(vm, "export: -%c: invalid option", *opt);
                        shellUpdateStatus(2);
                        return makeVoid();
                    }
                }
                continue;
            }
            parsing_options = false;
        }
        if (functions_only) {
            continue;
        }
        const char *eq = strchr(text, '=');
        size_t name_len = eq ? (size_t)(eq - text) : strlen(text);
        char *name = strndup(text, name_len);
        if (!name) {
            runtimeError(vm, "export: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (name_len == 0 || !shellIsValidEnvName(name)) {
            runtimeError(vm, "export: `%s': not a valid identifier", text);
            free(name);
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (eq) {
            const char *value = eq + 1;
            bool literal = value[0] == SHELL_ARRAY_LITERAL_MARK;
            if (literal) {
                value++;
            }
            if (!shellSetTrackedVariable(name, value, literal)) {
                runtimeError(vm, "export: %s: readonly variable", name);
                free(name);
                shellUpdateStatus(1);
                return makeVoid();
            }
        }
        shellVariableSetExported(name, !remove_export);
        free(name);
    }

    if (print_env) {
        shellExportPrintEnvironment();
    }

    shellUpdateStatus(0);
    return makeVoid();
}

static bool shellParseUmaskValue(const char *text, mode_t *out_mask) {
    if (!text || !*text || !out_mask) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 8);
    if (errno != 0 || !end || *end != '\0' || value < 0 || value > 0777) {
        return false;
    }
    *out_mask = (mode_t)value;
    return true;
}

static void shellUmaskFormatSymbolic(mode_t mask, char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    const struct {
        mode_t read_bit;
        mode_t write_bit;
        mode_t exec_bit;
        char prefix;
    } classes[] = {
        {S_IRUSR, S_IWUSR, S_IXUSR, 'u'},
        {S_IRGRP, S_IWGRP, S_IXGRP, 'g'},
        {S_IROTH, S_IWOTH, S_IXOTH, 'o'},
    };

    size_t offset = 0;
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        if (offset + 6 >= buffer_size) {
            break;
        }
        buffer[offset++] = classes[i].prefix;
        buffer[offset++] = '=';
        if (!(mask & classes[i].read_bit)) {
            buffer[offset++] = 'r';
        }
        if (!(mask & classes[i].write_bit)) {
            buffer[offset++] = 'w';
        }
        if (!(mask & classes[i].exec_bit)) {
            buffer[offset++] = 'x';
        }
        if (i + 1 < sizeof(classes) / sizeof(classes[0])) {
            buffer[offset++] = ',';
        }
    }

    if (offset >= buffer_size) {
        offset = buffer_size - 1;
    }
    buffer[offset] = '\0';
}

/* Bash's list, in its order, of what this system has. Sizes count in 1024 bytes, and -p (read only)
 * in 512. */
typedef struct {
    char option;
    const char *label;
    const char *unit;
    int resource; /* -1: not one the system keeps */
    rlim_t factor;
} ShellUlimitEntry;

static const ShellUlimitEntry kShellUlimits[] = {
    {'c', "core file size", "blocks", RLIMIT_CORE, 1024},
    {'d', "data seg size", "kbytes", RLIMIT_DATA, 1024},
#ifdef RLIMIT_NICE
    {'e', "scheduling priority", NULL, RLIMIT_NICE, 1},
#endif
    {'f', "file size", "blocks", RLIMIT_FSIZE, 1024},
#ifdef RLIMIT_SIGPENDING
    {'i', "pending signals", NULL, RLIMIT_SIGPENDING, 1},
#endif
    {'l', "max locked memory", "kbytes", RLIMIT_MEMLOCK, 1024},
    {'m', "max memory size", "kbytes", RLIMIT_RSS, 1024},
    {'n', "open files", NULL, RLIMIT_NOFILE, 1},
    {'p', "pipe size", "512 bytes", -1, 512},
#ifdef RLIMIT_MSGQUEUE
    {'q', "POSIX message queues", "bytes", RLIMIT_MSGQUEUE, 1},
#endif
#ifdef RLIMIT_RTPRIO
    {'r', "real-time priority", NULL, RLIMIT_RTPRIO, 1},
#endif
#ifdef RLIMIT_RTTIME
    {'R', "real-time non-blocking time", "microseconds", RLIMIT_RTTIME, 1},
#endif
    {'s', "stack size", "kbytes", RLIMIT_STACK, 1024},
    {'t', "cpu time", "seconds", RLIMIT_CPU, 1},
    {'u', "max user processes", NULL, RLIMIT_NPROC, 1},
    {'v', "virtual memory", "kbytes", RLIMIT_AS, 1024},
#ifdef RLIMIT_LOCKS
    {'x', "file locks", NULL, RLIMIT_LOCKS, 1},
#endif
};
#define SHELL_ULIMIT_ENTRIES (sizeof(kShellUlimits) / sizeof(kShellUlimits[0]))

static const ShellUlimitEntry *shellUlimitFind(char option) {
    for (size_t i = 0; i < SHELL_ULIMIT_ENTRIES; ++i) {
        if (kShellUlimits[i].option == option) {
            return &kShellUlimits[i];
        }
    }
    return NULL;
}

/* A substitution or a nested shell runs in this process, where under bash it is one of its own:
 * what it sets ends with it. Most never set any, so this is kept only when one does. */
typedef struct ShellLimitsSaved {
    struct ShellLimitsSaved *outer;
    size_t depth;
    struct rlimit limits[SHELL_ULIMIT_ENTRIES];
} ShellLimitsSaved;

static PSCAL_THREAD_LOCAL ShellLimitsSaved *gShellLimitsSaved = NULL;

static void shellCwdScopeRestoreLimits(size_t depth) {
    while (gShellLimitsSaved && gShellLimitsSaved->depth >= depth) {
        ShellLimitsSaved *saved = gShellLimitsSaved;
        for (size_t i = 0; i < SHELL_ULIMIT_ENTRIES; ++i) {
            if (kShellUlimits[i].resource >= 0) {
                (void)vprocShellSetrlimitShim(kShellUlimits[i].resource, &saved->limits[i], true);
            }
        }
        gShellLimitsSaved = saved->outer;
        free(saved);
    }
}

static void shellCwdScopeSaveLimits(void) {
    if (gShellCwdScopeDepth == 0 || (gShellLimitsSaved && gShellLimitsSaved->depth == gShellCwdScopeDepth)) {
        return;
    }
    ShellLimitsSaved *saved = (ShellLimitsSaved *)calloc(1, sizeof(ShellLimitsSaved));
    if (!saved) {
        return;
    }
    for (size_t i = 0; i < SHELL_ULIMIT_ENTRIES; ++i) {
        if (kShellUlimits[i].resource >= 0) {
            (void)vprocGetrlimitShim(kShellUlimits[i].resource, &saved->limits[i]);
        }
    }
    saved->depth = gShellCwdScopeDepth;
    saved->outer = gShellLimitsSaved;
    gShellLimitsSaved = saved;
}

static void shellUlimitPrint(const ShellUlimitEntry *entry, bool hard, bool labelled) {
    if (labelled) {
        char what[32];
        if (entry->unit) {
            snprintf(what, sizeof(what), "(%s, -%c)", entry->unit, entry->option);
        } else {
            snprintf(what, sizeof(what), "(-%c)", entry->option);
        }
        printf("%-20s%20s ", entry->label, what);
    }
    if (entry->resource < 0) {
        printf("%ld\n", (long)(PIPE_BUF / 512));
        return;
    }
    struct rlimit limit;
    if (vprocGetrlimitShim(entry->resource, &limit) != 0) {
        printf("unlimited\n");
        return;
    }
    rlim_t value = hard ? limit.rlim_max : limit.rlim_cur;
    if (value == RLIM_INFINITY) {
        printf("unlimited\n");
    } else {
        printf("%llu\n", (unsigned long long)(value / entry->factor));
    }
}

static bool shellUlimitSet(VM *vm, const ShellUlimitEntry *entry, const char *text, bool soft, bool hard) {
    struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};
    if (entry->resource >= 0) {
        (void)vprocGetrlimitShim(entry->resource, &limit);
    }
    rlim_t value;
    if (strcmp(text, "unlimited") == 0) {
        value = RLIM_INFINITY;
    } else if (strcmp(text, "hard") == 0) {
        value = limit.rlim_max;
    } else if (strcmp(text, "soft") == 0) {
        value = limit.rlim_cur;
    } else {
        const char *digit = text;
        while (*digit && isdigit((unsigned char)*digit)) {
            digit++;
        }
        errno = 0;
        char *end = NULL;
        unsigned long long parsed = *text ? strtoull(text, &end, 10) : 0;
        if (!*text || *digit || errno != 0 || (end && *end)) {
            runtimeError(vm, "ulimit: %s: invalid number", text);
            return false;
        }
        if (parsed > (unsigned long long)(RLIM_INFINITY / entry->factor)) {
            runtimeError(vm, "ulimit: %s: limit out of range", text);
            return false;
        }
        value = (rlim_t)parsed * entry->factor;
    }
    if (entry->resource < 0) {
        runtimeError(vm, "ulimit: %s: cannot modify limit: %s", entry->label, strerror(EINVAL));
        return false;
    }
    shellCwdScopeSaveLimits();
    if (soft) {
        limit.rlim_cur = value;
    }
#if defined(PSCAL_TARGET_IOS)
    if (hard) {
        limit.rlim_max = value;
    }
#else
    /* Not here in a substitution, which is this process: a hard limit could not come back up. */
    if (hard && gShellCwdScopeDepth == 0) {
        limit.rlim_max = value;
    }
#endif
    if (vprocShellSetrlimitShim(entry->resource, &limit, false) != 0) {
        runtimeError(vm, "ulimit: %s: cannot modify limit: %s", entry->label, strerror(errno));
        return false;
    }
    return true;
}

Value vmBuiltinShellUlimit(VM *vm, int arg_count, Value *args) {
    bool soft = false;
    bool hard = false;
    bool all = false;
    const ShellUlimitEntry *wanted[SHELL_ULIMIT_ENTRIES];
    size_t wanted_count = 0;
    int index = 0;
    for (; index < arg_count; ++index) {
        const char *text = VALUE_TYPE(args[index]) == TYPE_STRING ? AS_STRING(args[index]) : NULL;
        if (!text || text[0] != '-' || !text[1]) {
            break;
        }
        if (strcmp(text, "--") == 0) {
            index++;
            break;
        }
        for (const char *flag = text + 1; *flag; ++flag) {
            const ShellUlimitEntry *entry = shellUlimitFind(*flag);
            if (*flag == 'S') {
                soft = true;
            } else if (*flag == 'H') {
                hard = true;
            } else if (*flag == 'a') {
                all = true;
            } else if (!entry) {
                runtimeError(vm, "ulimit: -%c: invalid option", *flag);
                fprintf(stderr, "ulimit: usage: ulimit [-SHabcdefiklmnpqrstuvxPRT] [limit]\n");
                shellUpdateStatus(2);
                return makeVoid();
            } else {
                bool seen = false;
                for (size_t i = 0; i < wanted_count; ++i) {
                    seen = seen || wanted[i] == entry;
                }
                if (!seen) {
                    wanted[wanted_count++] = entry;
                }
            }
        }
    }
    if (all) {
        for (size_t i = 0; i < SHELL_ULIMIT_ENTRIES; ++i) {
            shellUlimitPrint(&kShellUlimits[i], hard && !soft, true);
        }
        shellUpdateStatus(0);
        return makeVoid();
    }
    if (wanted_count == 0) {
        wanted[wanted_count++] = shellUlimitFind('f');
    }
    const char *value = index < arg_count && VALUE_TYPE(args[index]) == TYPE_STRING ? AS_STRING(args[index]) : NULL;
    if (!value) {
        for (size_t i = 0; i < wanted_count; ++i) {
            shellUlimitPrint(wanted[i], hard && !soft, wanted_count > 1);
        }
        shellUpdateStatus(0);
        return makeVoid();
    }
    bool ok = true;
    for (size_t i = 0; i < wanted_count; ++i) {
        ok = shellUlimitSet(vm, wanted[i], value, soft || !hard, hard || !soft) && ok;
    }
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellUmask(VM *vm, int arg_count, Value *args) {
    bool symbolic = false;
    bool parsing_options = true;
    const char *mask_text = NULL;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "umask: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = AS_STRING(v);
        if (parsing_options && strcmp(text, "--") == 0) {
            parsing_options = false;
            continue;
        }
        if (parsing_options && strcmp(text, "-S") == 0) {
            symbolic = true;
            continue;
        }
        if (parsing_options && text[0] == '-' && text[1] != '\0') {
            runtimeError(vm, "umask: unsupported option '%s'", text);
            shellUpdateStatus(1);
            return makeVoid();
        }
        parsing_options = false;
        if (mask_text) {
            runtimeError(vm, "umask: too many arguments");
            shellUpdateStatus(1);
            return makeVoid();
        }
        mask_text = text;
    }

    if (mask_text) {
        mode_t new_mask = 0;
        if (!shellParseUmaskValue(mask_text, &new_mask)) {
            runtimeError(vm, "umask: invalid mode '%s'", mask_text);
            shellUpdateStatus(1);
            return makeVoid();
        }
        SHELL_UMASK(new_mask);
        shellUpdateStatus(0);
        return makeVoid();
    }

    mode_t current = umask(0);
    umask(current);

    if (symbolic) {
        char formatted[32];
        shellUmaskFormatSymbolic(current, formatted, sizeof(formatted));
        printf("%s\n", formatted);
    } else {
        printf("%04o\n", (unsigned int)current);
    }

    shellUpdateStatus(0);
    return makeVoid();
}

static void shellTimesFormatValue(char *buffer, size_t buffer_size, clock_t ticks, long ticks_per_second) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    if (ticks_per_second <= 0) {
        ticks_per_second = 100;
    }
    double total_seconds = (double)ticks / (double)ticks_per_second;
    if (total_seconds < 0.0) {
        total_seconds = 0.0;
    }
    long minutes = (long)(total_seconds / 60.0);
    double seconds = total_seconds - (double)minutes * 60.0;
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    double rounded = floor(seconds * 1000.0 + 0.5) / 1000.0;
    if (rounded >= 60.0) {
        minutes += 1;
        rounded = 0.0;
    }
    snprintf(buffer, buffer_size, "%ldm%0.3fs", minutes, rounded);
    buffer[buffer_size - 1] = '\0';
}

/* `time` around a pipeline. A frame belongs to the VM that started it: `return` can stop that
 * chunk before it reaches the end, and what it left is passed over. */
typedef struct {
    VM *vm;
    bool skipped;
    struct timeval wall;
    struct rusage self;
    struct rusage children;
} ShellTimeFrame;

static PSCAL_THREAD_LOCAL ShellTimeFrame *gShellTimeFrames = NULL;
static PSCAL_THREAD_LOCAL size_t gShellTimeFrameCount = 0;
static PSCAL_THREAD_LOCAL size_t gShellTimeFrameCapacity = 0;

/* The `&` that comes next was written as this. It leaves $? alone. */
Value vmBuiltinShellJobText(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count == 1 && VALUE_TYPE(args[0]) == TYPE_STRING && AS_STRING(args[0])) {
        gShellRuntime.pending_job_text = shellJobTextIntern(AS_STRING(args[0]));
    }
    return makeVoid();
}

Value vmBuiltinShellTimeBegin(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;
    if (gShellTimeFrameCount == gShellTimeFrameCapacity) {
        size_t capacity = gShellTimeFrameCapacity ? gShellTimeFrameCapacity * 2 : 4;
        ShellTimeFrame *grown = (ShellTimeFrame *)realloc(gShellTimeFrames, capacity * sizeof(ShellTimeFrame));
        if (!grown) {
            return makeVoid();
        }
        gShellTimeFrames = grown;
        gShellTimeFrameCapacity = capacity;
    }
    ShellTimeFrame *frame = &gShellTimeFrames[gShellTimeFrameCount++];
    frame->vm = vm;
    frame->skipped = shellLoopSkipActive();
    gettimeofday(&frame->wall, NULL);
    getrusage(RUSAGE_SELF, &frame->self);
    getrusage(RUSAGE_CHILDREN, &frame->children);
    return makeVoid();
}

static double shellTimevalSeconds(const struct timeval *end, const struct timeval *start) {
    return (double)(end->tv_sec - start->tv_sec) + (double)(end->tv_usec - start->tv_usec) / 1000000.0;
}

/* TIMEFORMAT: %% and %[p][l]R, U and S, %P. Text goes through as it is. */
static void shellTimeReport(const char *format, double real, double user, double sys) {
    char out[1024];
    size_t used = 0;
    for (const char *p = format; *p && used + 64 < sizeof(out); ++p) {
        if (*p != '%') {
            out[used++] = *p;
            continue;
        }
        p++;
        if (*p == '%') {
            out[used++] = '%';
            continue;
        }
        if (*p == 'P') {
            used += (size_t)snprintf(out + used, sizeof(out) - used, "%.2f", real > 0 ? (user + sys) * 100.0 / real : 0.0);
            continue;
        }
        int precision = 3;
        if (*p >= '0' && *p <= '9') {
            precision = *p - '0';
            if (precision > 3) {
                precision = 3;
            }
            p++;
        }
        bool long_form = false;
        if (*p == 'l') {
            long_form = true;
            p++;
        }
        double value = 0;
        if (*p == 'R') {
            value = real;
        } else if (*p == 'U') {
            value = user;
        } else if (*p == 'S') {
            value = sys;
        } else {
            if (!*p) {
                break;
            }
            out[used++] = '%';
            out[used++] = *p;
            continue;
        }
        if (long_form) {
            long minutes = (long)(value / 60.0);
            used += (size_t)snprintf(out + used, sizeof(out) - used, "%ldm%.*fs", minutes, precision, value - (double)minutes * 60.0);
        } else {
            used += (size_t)snprintf(out + used, sizeof(out) - used, "%.*f", precision, value);
        }
    }
    out[used] = '\0';
    shellFdPrintf(STDERR_FILENO, "%s\n", out);
}

/* One argument, its letters: p for -p, e for `time` with no pipeline after it. */
Value vmBuiltinShellTimeEnd(VM *vm, int arg_count, Value *args) {
    const char *mode = (arg_count > 0 && VALUE_TYPE(args[0]) == TYPE_STRING && AS_STRING(args[0])) ? AS_STRING(args[0]) : "";
    while (gShellTimeFrameCount > 0 && gShellTimeFrames[gShellTimeFrameCount - 1].vm != vm) {
        gShellTimeFrameCount--;
    }
    if (gShellTimeFrameCount == 0) {
        return makeVoid();
    }
    ShellTimeFrame frame = gShellTimeFrames[--gShellTimeFrameCount];
    if (frame.skipped) {
        return makeVoid();
    }
    if (strchr(mode, 'e')) {
        shellUpdateStatus(0);
    }
    struct timeval now;
    struct rusage self;
    struct rusage children;
    gettimeofday(&now, NULL);
    getrusage(RUSAGE_SELF, &self);
    getrusage(RUSAGE_CHILDREN, &children);
    double real = shellTimevalSeconds(&now, &frame.wall);
    double user = shellTimevalSeconds(&self.ru_utime, &frame.self.ru_utime) +
                  shellTimevalSeconds(&children.ru_utime, &frame.children.ru_utime);
    double sys = shellTimevalSeconds(&self.ru_stime, &frame.self.ru_stime) +
                 shellTimevalSeconds(&children.ru_stime, &frame.children.ru_stime);
    const char *format = "\nreal\t%3lR\nuser\t%3lU\nsys\t%3lS";
    if (strchr(mode, 'p')) {
        format = "real %2R\nuser %2U\nsys %2S";
    } else {
        ShellLocalVariable *local = shellLocalScopeFind("TIMEFORMAT");
        const char *custom = local ? (local->no_value ? NULL : local->value) : shellSafeGetenv("TIMEFORMAT");
        if (custom) {
            if (!*custom) {
                return makeVoid();
            }
            format = custom;
        }
    }
    shellFlushStandardStreams();
    shellTimeReport(format, real, user, sys);
    return makeVoid();
}

Value vmBuiltinShellTimes(VM *vm, int arg_count, Value *args) {
    (void)args;
    (void)arg_count;
    struct tms time_info;
    clock_t result = times(&time_info);
    if (result == (clock_t)-1) {
        runtimeError(vm, "times: %s", strerror(errno));
        shellUpdateStatus(1);
        return makeVoid();
    }
    long ticks_per_second = sysconf(_SC_CLK_TCK);
    char shell_user[32];
    char shell_system[32];
    char child_user[32];
    char child_system[32];
    shellTimesFormatValue(shell_user, sizeof(shell_user), time_info.tms_utime, ticks_per_second);
    shellTimesFormatValue(shell_system, sizeof(shell_system), time_info.tms_stime, ticks_per_second);
    shellTimesFormatValue(child_user, sizeof(child_user), time_info.tms_cutime, ticks_per_second);
    shellTimesFormatValue(child_system, sizeof(child_system), time_info.tms_cstime, ticks_per_second);
    printf("%s %s\n", shell_user, shell_system);
    printf("%s %s\n", child_user, child_system);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellCommand(VM *vm, int arg_count, Value *args) {
    bool parsing_options = true;
    bool list_all = false;
    bool print_short = false;
    bool print_verbose = false;
    bool use_default_path = false;
    int first_name = arg_count;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "command: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = AS_STRING(v);
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                for (const char *opt = text + 1; *opt; ++opt) {
                    switch (*opt) {
                        case 'a':
                            list_all = true;
                            break;
                        case 'p':
                            use_default_path = true;
                            break;
                        case 'v':
                            if (!print_verbose) {
                                print_short = true;
                            }
                            break;
                        case 'V':
                            print_verbose = true;
                            print_short = false;
                            break;
                        default:
                            runtimeError(vm, "command: unsupported option '-%c'", *opt);
                            shellUpdateStatus(1);
                            return makeVoid();
                    }
                }
                continue;
            }
            parsing_options = false;
        }
        if (parsing_options) {
            continue;
        }
        if (first_name == arg_count) {
            first_name = i;
        }
    }

    if (!print_short && !print_verbose) {
        if (first_name == arg_count) {
            shellUpdateStatus(0);
            return makeVoid();
        }
        size_t exec_argc = (size_t)(arg_count - first_name);
        ShellCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.argc = exec_argc;
        cmd.argv = (char **)calloc(exec_argc + 1, sizeof(char *));
        if (!cmd.argv) {
            runtimeError(vm, "command: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        bool ok = true;
        for (size_t i = 0; i < exec_argc; ++i) {
            const char *source = AS_STRING(args[first_name + (int)i]);
            cmd.argv[i] = strdup(source ? source : "");
            if (!cmd.argv[i]) {
                ok = false;
                for (size_t j = 0; j < i; ++j) {
                    free(cmd.argv[j]);
                }
                free(cmd.argv);
                cmd.argv = NULL;
                break;
            }
        }
        if (!ok) {
            runtimeError(vm, "command: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        /* Only -p looks anywhere but where the command would look by itself. */
        if (use_default_path && cmd.argv && cmd.argv[0] && cmd.argv[0][0] != '\0') {
            ShellCommandResult *results = NULL;
            size_t count = 0;
            if (!shellCommandCollectInfo(cmd.argv[0], false, use_default_path, &results, &count)) {
                runtimeError(vm, "command: out of memory");
                shellUpdateStatus(1);
                for (size_t j = 0; j < exec_argc; ++j) {
                    free(cmd.argv[j]);
                }
                free(cmd.argv);
                return makeVoid();
            }
            for (size_t idx = 0; idx < count; ++idx) {
                ShellCommandResult *entry = &results[idx];
                if (entry->kind == SHELL_COMMAND_RESULT_BUILTIN || entry->kind == SHELL_COMMAND_RESULT_APPLET) {
                    break;
                }
                if (entry->kind == SHELL_COMMAND_RESULT_FILE && entry->detail && entry->detail[0] != '\0') {
                    char *replacement = strdup(entry->detail);
                    if (replacement) {
                        free(cmd.argv[0]);
                        cmd.argv[0] = replacement;
                    }
                    break;
                }
            }
            shellCommandFreeResults(results, count);
        }
        cmd.ignore_functions = true;
        cmd.argv[exec_argc] = NULL;
        VM *previous_vm = shellSwapCurrentVm(vm);
        Value result = shellExecuteCommand(vm, &cmd);
        shellRestoreCurrentVm(previous_vm);
        return result;
    }

    if (first_name == arg_count) {
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool overall_ok = true;
    bool collect_all_paths = list_all;

    for (int i = first_name; i < arg_count; ++i) {
        const char *name = AS_STRING(args[i]);
        ShellCommandResult *results = NULL;
        size_t count = 0;
        if (!shellCommandCollectInfo(name, collect_all_paths, use_default_path, &results, &count)) {
            runtimeError(vm, "command: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }

        if (count == 0) {
            overall_ok = false;
            if (print_verbose) {
                runtimeError(vm, "command: %s: not found", name);
            }
            shellUpdateStatus(1);
            shellCommandFreeResults(results, count);
            continue;
        }

        if (print_verbose) {
            if (list_all) {
                for (size_t j = 0; j < count; ++j) {
                    shellCommandPrintVerbose(name, &results[j]);
                }
            } else {
                shellCommandPrintVerbose(name, &results[0]);
            }
        } else if (print_short) {
            if (list_all) {
                for (size_t j = 0; j < count; ++j) {
                    shellCommandPrintShort(name, &results[j]);
                }
            } else {
                shellCommandPrintShort(name, &results[0]);
            }
        }

        shellCommandFreeResults(results, count);
    }

    shellUpdateStatus(overall_ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellWhich(VM *vm, int arg_count, Value *args) {
    bool parsing_options = true;
    bool list_all = false;
    bool silent = false;
    bool use_default_path = false;
    int first_name = arg_count;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "which: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = AS_STRING(v);
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                for (const char *opt = text + 1; *opt; ++opt) {
                    switch (*opt) {
                        case 'a':
                            list_all = true;
                            break;
                        case 's':
                            silent = true;
                            break;
                        case 'p':
                            use_default_path = true;
                            break;
                        default:
                            runtimeError(vm, "which: invalid option -- '%c'", *opt);
                            shellUpdateStatus(1);
                            return makeVoid();
                    }
                }
                continue;
            }
            parsing_options = false;
        }
        if (first_name == arg_count) {
            first_name = i;
        }
    }

    if (first_name == arg_count) {
        runtimeError(vm, "which: expected name");
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool overall_ok = true;
    bool collect_all_paths = list_all;

    for (int i = first_name; i < arg_count; ++i) {
        const char *name = AS_STRING(args[i]);
        ShellCommandResult *results = NULL;
        size_t count = 0;
        if (!shellCommandCollectInfo(name, collect_all_paths, use_default_path, &results, &count)) {
            runtimeError(vm, "which: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }

        bool printed = false;
        for (size_t j = 0; j < count; ++j) {
            ShellCommandResult *res = &results[j];
            const char *detail = res->detail ? res->detail : "";
            switch (res->kind) {
                case SHELL_COMMAND_RESULT_ALIAS:
                    if (!silent) {
                        printf("%s: aliased to `%s'\n", name, detail);
                    }
                    printed = true;
                    break;
                case SHELL_COMMAND_RESULT_KEYWORD:
                    if (!silent) {
                        printf("%s: shell keyword\n", name);
                    }
                    printed = true;
                    break;
                case SHELL_COMMAND_RESULT_FUNCTION:
                    if (!silent) {
                        printf("%s: shell function\n", name);
                    }
                    printed = true;
                    break;
                case SHELL_COMMAND_RESULT_BUILTIN:
                    if (!silent) {
                        printf("%s: shell builtin\n", name);
                    }
                    printed = true;
                    break;
                case SHELL_COMMAND_RESULT_APPLET:
                    if (!silent) {
                        printf("%s: smallclue applet\n", name);
                    }
                    printed = true;
                    break;
                case SHELL_COMMAND_RESULT_FILE:
                    if (!silent) {
                        printf("%s\n", detail);
                    }
                    printed = true;
                    break;
            }
            if (printed && !list_all) {
                break;
            }
        }

        if (!printed) {
            overall_ok = false;
            if (!silent) {
                fprintf(stderr, "%s: not found\n", name);
            }
        }
        shellCommandFreeResults(results, count);
    }

    shellUpdateStatus(overall_ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellHash(VM *vm, int arg_count, Value *args) {
    bool parsing_options = true;
    bool reset_table = false;
    bool print_paths = false;
    int first_name = arg_count;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "hash: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = AS_STRING(v);
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                for (const char *opt = text + 1; *opt; ++opt) {
                    switch (*opt) {
                        case 'r':
                            reset_table = true;
                            break;
                        case 't':
                            print_paths = true;
                            break;
                        default:
                            runtimeError(vm, "hash: invalid option -- '%c'", *opt);
                            shellUpdateStatus(1);
                            return makeVoid();
                    }
                }
                continue;
            }
            parsing_options = false;
        }
        if (parsing_options) {
            continue;
        }
        if (first_name == arg_count) {
            first_name = i;
        }
    }

    if (reset_table) {
        shellHashClear();
    }

    if (first_name == arg_count) {
        if (print_paths) {
            runtimeError(vm, "hash: -t: option requires an argument");
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (!reset_table) {
            shellHashPrintTable();
        }
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool overall_ok = true;
    for (int i = first_name; i < arg_count; ++i) {
        const char *name = AS_STRING(args[i]) ? AS_STRING(args[i]) : "";
        if (print_paths) {
            ShellHashEntry *entry = shellHashFindEntry(name);
            if (!entry || !entry->path || !*entry->path) {
                runtimeError(vm, "hash: %s: not found", name);
                overall_ok = false;
                continue;
            }
            printf("%s\n", entry->path);
            continue;
        }
        if (!shellHashAddName(name)) {
            runtimeError(vm, "hash: %s: not found", name);
            overall_ok = false;
        }
    }

    shellUpdateStatus(overall_ok ? 0 : 1);
    return makeVoid();
}

static bool shellBuiltinIsInternal(const char *name) {
    return name && name[0] == '_' && name[1] == '_';
}

typedef struct {
    bool print_pretty;
    bool list_all;
    bool list_disabled_only;
    bool disabled_prefix;
} ShellEnableListContext;

static void shellEnableListVisitor(const char *name,
                                   const char *canonical,
                                   int id,
                                   void *ctx_void) {
    (void)id;
    ShellEnableListContext *ctx = (ShellEnableListContext *)ctx_void;
    if (!ctx) {
        return;
    }
    if (!canonical || !*canonical) {
        canonical = name;
    }
    if (!name) {
        name = canonical;
    }
    if (!canonical || !*canonical) {
        return;
    }
    if (shellBuiltinIsInternal(canonical)) {
        return;
    }
    if (strcasecmp(name, canonical) != 0) {
        return;
    }
    bool disabled = shellRuntimeBuiltinDisabled(canonical);
    if (!ctx->list_all) {
        if (ctx->list_disabled_only) {
            if (!disabled) {
                return;
            }
        } else if (disabled) {
            return;
        }
    }
    if (ctx->print_pretty) {
        if (disabled) {
            printf("enable -n %s\n", canonical);
        } else {
            printf("enable %s\n", canonical);
        }
    } else if (ctx->disabled_prefix && disabled) {
        printf("enable -n %s\n", canonical);
    } else {
        printf("%s\n", canonical);
    }
}

Value vmBuiltinShellEnable(VM *vm, int arg_count, Value *args) {
    bool parsing_options = true;
    bool disable_mode = false;
    bool print_pretty = false;
    bool list_all = false;
    int operand_start = arg_count;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "enable: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = AS_STRING(v);
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                operand_start = i + 1;
                break;
            }
            if (text[0] == '-' && text[1] != '\0') {
                for (const char *opt = text + 1; *opt; ++opt) {
                    switch (*opt) {
                        case 'a':
                            list_all = true;
                            break;
                        case 'n':
                            disable_mode = true;
                            break;
                        case 'p':
                            print_pretty = true;
                            break;
                        default:
                            runtimeError(vm, "enable: invalid option -- '%c'", *opt);
                            shellUpdateStatus(1);
                            return makeVoid();
                    }
                }
                continue;
            }
            parsing_options = false;
        }
        if (operand_start == arg_count) {
            operand_start = i;
        }
    }

    if (operand_start >= arg_count) {
        ShellEnableListContext ctx = {
            .print_pretty = print_pretty,
            .list_all = list_all,
            .list_disabled_only = disable_mode && !list_all,
            .disabled_prefix = disable_mode && !list_all,
        };
        shellVisitBuiltins(shellEnableListVisitor, &ctx);
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool ok = true;
    for (int i = operand_start; i < arg_count; ++i) {
        Value v = args[i];
        const char *name = (VALUE_TYPE(v) == TYPE_STRING) ? AS_STRING(v) : NULL;
        if (!name || !*name) {
            runtimeError(vm, "enable: : not a shell builtin");
            ok = false;
            continue;
        }
        if (!shellIsBuiltinName(name)) {
            runtimeError(vm, "enable: %s: not a shell builtin", name);
            ok = false;
            continue;
        }
        if (shellBuiltinIsInternal(name)) {
            runtimeError(vm, "enable: %s: not a shell builtin", name);
            ok = false;
            continue;
        }
        bool success = false;
        if (disable_mode) {
            success = shellRuntimeDisableBuiltin(name);
        } else {
            success = shellRuntimeEnableBuiltin(name);
        }
        if (!success) {
            runtimeError(vm, "enable: out of memory");
            ok = false;
            break;
        }
    }

    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellType(VM *vm, int arg_count, Value *args) {
    bool parsing_options = true;
    bool list_all = false;
    bool print_type = false;
    bool print_path = false;
    bool force_path = false;
    bool skip_functions = false;
    int first_name = arg_count;

    for (int i = 0; i < arg_count; ++i) {
        Value v = args[i];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "type: arguments must be strings");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *text = AS_STRING(v);
        if (parsing_options) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                for (const char *opt = text + 1; *opt; ++opt) {
                    switch (*opt) {
                        case 'a':
                            list_all = true;
                            break;
                        case 'f':
                            skip_functions = true;
                            break;
                        case 't':
                            print_type = true;
                            break;
                        case 'p':
                            print_path = true;
                            break;
                        case 'P':
                            print_path = true;
                            force_path = true;
                            break;
                        default:
                            shellReportRecoverableError(vm, true, "type: -%c: invalid option", *opt);
                            shellReportRecoverableError(vm, false, "type: usage: type [-afptP] name [name ...]");
                            shellUpdateStatus(2);
                            return makeVoid();
                    }
                }
                continue;
            }
            parsing_options = false;
        }
        if (parsing_options) {
            continue;
        }
        if (first_name == arg_count) {
            first_name = i;
        }
    }

    if (first_name == arg_count) {
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool descriptive = !print_type && !print_path;
    bool overall_ok = true;
    bool collect_all_paths = list_all || print_path;

    for (int i = first_name; i < arg_count; ++i) {
        const char *name = AS_STRING(args[i]);
        ShellCommandResult *results = NULL;
        size_t count = 0;
        if (!shellCommandCollectInfo(name, collect_all_paths, false, &results, &count)) {
            runtimeError(vm, "type: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }

        if (skip_functions) {
            size_t kept = 0;
            for (size_t j = 0; j < count; ++j) {
                if (results[j].kind == SHELL_COMMAND_RESULT_FUNCTION) {
                    free(results[j].detail);
                    continue;
                }
                results[kept++] = results[j];
            }
            count = kept;
        }

        if (count == 0) {
            overall_ok = false;
            if (descriptive) {
                shellReportRecoverableError(vm, true, "type: %s: not found", name);
            }
            shellUpdateStatus(1);
            shellCommandFreeResults(results, count);
            continue;
        }

        if (print_path) {
            /* -p names the file that would run, so nothing when something else comes first.
             * -P looks only for a file. */
            bool found = !force_path;
            for (size_t j = 0; j < count; ++j) {
                if (results[j].kind == SHELL_COMMAND_RESULT_FILE) {
                    printf("%s\n", results[j].detail ? results[j].detail : "");
                    found = true;
                    if (!list_all) {
                        break;
                    }
                } else if (!force_path && !list_all) {
                    break;
                }
            }
            if (!found) {
                overall_ok = false;
                shellUpdateStatus(1);
            }
            shellCommandFreeResults(results, count);
            continue;
        }

        if (print_type) {
            if (list_all) {
                for (size_t j = 0; j < count; ++j) {
                    printf("%s\n", shellCommandResultKindLabel(results[j].kind));
                }
            } else {
                printf("%s\n", shellCommandResultKindLabel(results[0].kind));
            }
            shellCommandFreeResults(results, count);
            continue;
        }

        if (list_all) {
            for (size_t j = 0; j < count; ++j) {
                shellCommandPrintVerbose(name, &results[j]);
            }
        } else {
            shellCommandPrintVerbose(name, &results[0]);
        }
        shellCommandFreeResults(results, count);
    }

    shellUpdateStatus(overall_ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellUnset(VM *vm, int arg_count, Value *args) {
    bool ok = true;
    bool functions = false;
    bool variables = false;
    bool parsing_options = true;
    for (int i = 0; i < arg_count; ++i) {
        if (VALUE_TYPE(args[i]) != TYPE_STRING || !AS_STRING(args[i])) {
            runtimeError(vm, "unset: expected variable name");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *name = AS_STRING(args[i]);
        if (parsing_options && name[0] == '-' && name[1]) {
            if (strcmp(name, "--") == 0) {
                parsing_options = false;
                continue;
            }
            for (const char *opt = name + 1; *opt; ++opt) {
                if (*opt == 'f') {
                    functions = true;
                } else if (*opt == 'v') {
                    variables = true;
                } else if (*opt != 'n') {
                    runtimeError(vm, "unset: -%c: invalid option", *opt);
                    shellUpdateStatus(2);
                    return makeVoid();
                }
            }
            continue;
        }
        parsing_options = false;
        if (functions) {
            shellRemoveFunction(name);
            continue;
        }
        /* a[1], which only quotes keep from being a pattern. */
        const char *open = strchr(name, '[');
        size_t name_len = open ? (size_t)(open - name) : strlen(name);
        if (open && name[strlen(name) - 1] == ']') {
            char *base = strndup(name, name_len);
            size_t subscript_len = strlen(open) - 2;
            if (base && strcmp(open + 1, "@]") != 0 && strcmp(open + 1, "*]") != 0) {
                char *key = NULL;
                if (!shellParamSubscriptKeyChecked(base, name_len, open + 1, subscript_len, &key)) {
                    ok = false;
                }
                if (key) {
                    shellArrayRegistryUnsetElement(base, key);
                    free(key);
                }
            } else if (base) {
                shellUnsetTrackedVariable(base);
            }
            free(base);
            continue;
        }
        if (shellReadonlyContains(name)) {
            runtimeError(vm, "unset: %s: readonly variable", name);
            ok = false;
            continue;
        }
        /* With neither option, a variable first and then a function of that name. */
        bool is_variable = shellSafeGetenv(name) != NULL || shellLocalScopeFind(name) != NULL ||
                           shellArrayRegistryFindConst(name) != NULL;
        if (!is_variable && !variables && shellFunctionIsDefined(name)) {
            shellRemoveFunction(name);
            continue;
        }
        shellUnsetTrackedVariable(name);
    }
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellUnsetenv(VM *vm, int arg_count, Value *args) {
    return vmBuiltinShellUnset(vm, arg_count, args);
}

static bool shellParseLoopLevel(const char *text, long *out_level) {
    if (!text || !out_level) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || end == text) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *out_level = value;
    return true;
}

static bool shellIsValidEnvName(const char *name) {
    if (!name || !*name) {
        return false;
    }
    size_t len = strlen(name);
    size_t consumed = 0;
    return consumeShellIdentifier(name, len, &consumed, false) && consumed == len;
}

static int shellCompareEnvStrings(const void *lhs, const void *rhs) {
    const char *const *a = (const char *const *)lhs;
    const char *const *b = (const char *const *)rhs;
    if (!a || !b) {
        return 0;
    }
    if (!*a) {
        return *b ? -1 : 0;
    }
    if (!*b) {
        return 1;
    }
    return strcmp(*a, *b);
}

static void shellPrintExportEntry(const char *name, const char *value) {
    printf("declare -x %s", name);
    if (value) {
        fputc('=', stdout);
        shellDeclarePrintQuoted(value);
    }
    fputc('\n', stdout);
}

static void shellExportListAdd(char ***names, size_t *count, size_t *capacity, const char *name, size_t len) {
    for (size_t i = 0; i < *count; ++i) {
        if (strlen((*names)[i]) == len && strncmp((*names)[i], name, len) == 0) {
            return;
        }
    }
    if (*count == *capacity) {
        size_t grown = *capacity ? *capacity * 2 : 64;
        char **resized = (char **)realloc(*names, grown * sizeof(char *));
        if (!resized) {
            return;
        }
        *names = resized;
        *capacity = grown;
    }
    char *copy = strndup(name, len);
    if (copy) {
        (*names)[(*count)++] = copy;
    }
}

static void shellExportPrintEnvironment(void) {
    char **names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    void *walk = NULL;
    for (char **env = pscalEnvironWalkBegin(&walk); env && *env; ++env) {
        const char *eq = strchr(*env, '=');
        if (eq) {
            shellExportListAdd(&names, &count, &capacity, *env, (size_t)(eq - *env));
        }
    }
    pscalEnvironWalkEnd(walk);
    SHELL_EXPORT_LOCK();
    const ShellNameSet *pending = &shellExportFlagsCurrent()->pending;
    for (size_t i = 0; i < pending->count; ++i) {
        shellExportListAdd(&names, &count, &capacity, pending->names[i], strlen(pending->names[i]));
    }
    SHELL_EXPORT_UNLOCK();
    for (size_t depth = 0; depth < gShellLocalScopeDepth; ++depth) {
        const ShellLocalScopeFrame *frame = &gShellLocalScopeStack[depth];
        for (size_t i = 0; i < frame->count; ++i) {
            if (frame->variables[i].name && frame->variables[i].exported) {
                shellExportListAdd(&names, &count, &capacity, frame->variables[i].name,
                                   strlen(frame->variables[i].name));
            }
        }
    }
    if (count > 1) {
        qsort(names, count, sizeof(char *), shellCompareEnvStrings);
    }
    for (size_t i = 0; i < count; ++i) {
        const char *name = names[i];
        if (strcmp(name, "EXSH_LAST_STATUS") != 0 && !shellArrayRegistryFindConst(name)) {
            ShellLocalVariable *local = shellLocalScopeFind(name);
            if (local && local->exported) {
                shellPrintExportEntry(name, local->no_value ? NULL : local->value);
            } else if (shellGlobalIsExported(name)) {
                shellPrintExportEntry(name, shellSafeGetenv(name));
            }
        }
        free(names[i]);
    }
    free(names);
}

static bool shellParseReturnStatus(const char *text, int *out_status) {
    if (!text || !out_status || *text == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || end == text) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    int status = (int)((unsigned long)value & 0xFFu);
    *out_status = status;
    return true;
}

typedef struct {
    const char *name;
    bool *flag;
    bool inverted;
} ShellSetOption;

/* The names `set -o` knows. One with no flag does nothing here but remember its setting. */
static size_t shellSetOptionTable(ShellSetOption *out) {
    ShellSetOption table[] = {
        {"allexport", &gShellRuntime.allexport_enabled, false},
        {"braceexpand", &gShellRuntime.braceexpand_disabled, true},
        {"emacs", NULL, false},
        {"errexit", &gShellRuntime.errexit_enabled, false},
        {"errtrace", &gShellRuntime.errtrace_enabled, false},
        {"functrace", &gShellRuntime.functrace_enabled, false},
        {"hashall", NULL, false},
        {"histexpand", NULL, false},
        {"history", NULL, false},
        {"ignoreeof", NULL, false},
        {"interactive-comments", NULL, false},
        {"keyword", NULL, false},
        {"monitor", &gShellRuntime.monitor_enabled, false},
        {"noclobber", &gShellRuntime.noclobber_enabled, false},
        {"noexec", NULL, false},
        {"noglob", &gShellRuntime.noglob_enabled, false},
        {"nolog", NULL, false},
        {"notify", NULL, false},
        {"nounset", &gShellRuntime.nounset_enabled, false},
        {"onecmd", NULL, false},
        {"physical", NULL, false},
        {"pipefail", &gShellRuntime.pipefail_enabled, false},
        {"posix", NULL, false},
        {"privileged", NULL, false},
        {"verbose", &gShellRuntime.verbose_enabled, false},
        {"vi", NULL, false},
        {"xtrace", &gShellRuntime.xtrace_enabled, false},
    };
    size_t count = sizeof(table) / sizeof(table[0]);
    if (out) {
        memcpy(out, table, sizeof(table));
    }
    return count;
}

static bool gShellSetRemembered[32];

static bool shellSetOptionByName(const char *name, bool enable) {
    ShellSetOption table[32];
    size_t count = shellSetOptionTable(table);
    for (size_t i = 0; i < count; ++i) {
        if (strcasecmp(table[i].name, name) != 0) {
            continue;
        }
        if (strcmp(table[i].name, "posix") == 0) {
            shellRuntimeSetPosixMode(enable);
            return true;
        }
        if (!table[i].flag) {
            gShellSetRemembered[i] = enable;
            return true;
        }
        *table[i].flag = table[i].inverted ? !enable : enable;
        if (strcmp(table[i].name, "monitor") == 0 && enable) {
            gShellRuntime.job_control_gone = false;
        }
        if (strcmp(table[i].name, "errexit") == 0) {
            gShellRuntime.errexit_pending = false;
            gShellRuntime.errexit_defer_active = false;
        }
        return true;
    }
    return false;
}

static bool shellSetOptionEntryIsOn(const ShellSetOption *table, size_t i) {
    if (table[i].flag) {
        return table[i].inverted ? !*table[i].flag : *table[i].flag;
    }
    if (strcmp(table[i].name, "posix") == 0) {
        return shellRuntimePosixMode();
    }
    return gShellSetRemembered[i];
}

/* test -o: a name `set -o` does not know is simply not on. */
static bool shellSetOptionIsOn(const char *name) {
    ShellSetOption table[32];
    size_t count = shellSetOptionTable(table);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(table[i].name, name) == 0) {
            return shellSetOptionEntryIsOn(table, i);
        }
    }
    return false;
}

/* `set -o` and `set +o`. */
static void shellSetPrintOptions(bool as_table) {
    ShellSetOption table[32];
    size_t count = shellSetOptionTable(table);
    for (size_t i = 0; i < count; ++i) {
        bool on = shellSetOptionEntryIsOn(table, i);
        if (as_table) {
            printf("%-15s\t%s\n", table[i].name, on ? "on" : "off");
        } else {
            printf("set %co %s\n", on ? '-' : '+', table[i].name);
        }
    }
    fflush(stdout);
}

Value vmBuiltinShellSet(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        shellSetPrintAll();
        shellUpdateStatus(0);
        return makeVoid();
    }
    bool ok = true;
    bool parsing_options = true;
    bool replace_positional = false;
    int positional_start = arg_count;
    for (int i = 0; i < arg_count && ok; ++i) {
        Value v = args[i];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "set: expected string argument");
            ok = false;
            break;
        }
        const char *token = AS_STRING(v);
        if (!parsing_options) {
            continue;
        }
        if (strcmp(token, "--") == 0) {
            /* With nothing after it this empties them: `set --`. */
            positional_start = i + 1;
            replace_positional = true;
            parsing_options = false;
            break;
        }
        if (strcmp(token, "-") == 0) {
            positional_start = i + 1;
            parsing_options = false;
            break;
        }
        if (token[0] == '-' || token[0] == '+') {
            /* Letters combine, as in `set -eu` and `set -euo pipefail`, and each `o`
             * takes the next word. */
            bool enable = (token[0] == '-');
            for (const char *flag = token + 1; *flag && ok; ++flag) {
                const char *name = NULL;
                switch (*flag) {
                    case 'a': name = "allexport"; break;
                    case 'e': name = "errexit"; break;
                    case 'f': name = "noglob"; break;
                    case 'm': name = "monitor"; break;
                    case 'u': name = "nounset"; break;
                    case 'x': name = "xtrace"; break;
                    case 'v': name = "verbose"; break;
                    case 'C': name = "noclobber"; break;
                    case 'B': name = "braceexpand"; break;
                    case 'E': name = "errtrace"; break;
                    case 'T': name = "functrace"; break;
                    default: break;
                }
                if (*flag == 'o') {
                    if (i + 1 >= arg_count) {
                        shellSetPrintOptions(enable);
                        shellUpdateStatus(0);
                        return makeVoid();
                    }
                    Value name_val = args[++i];
                    if (VALUE_TYPE(name_val) != TYPE_STRING || !AS_STRING(name_val)) {
                        runtimeError(vm, "set: option name must be a string");
                        ok = false;
                        break;
                    }
                    name = AS_STRING(name_val);
                    if (!shellSetOptionByName(name, enable)) {
                        runtimeError(vm, "set: %s: invalid option name", name);
                        ok = false;
                    }
                } else if (name) {
                    (void)shellSetOptionByName(name, enable);
                }
            }
        } else {
            positional_start = i;
            parsing_options = false;
            break;
        }
    }

    if (ok && (positional_start < arg_count || replace_positional)) {
        int new_count = arg_count - positional_start;
        char **new_params = NULL;
        if (new_count > 0) {
            new_params = (char **)calloc((size_t)new_count, sizeof(char *));
            if (!new_params) {
                runtimeError(vm, "set: out of memory");
                ok = false;
            } else {
                for (int i = 0; i < new_count && ok; ++i) {
                    Value val = args[positional_start + i];
                    if (VALUE_TYPE(val) != TYPE_STRING || !AS_STRING(val)) {
                        runtimeError(vm, "set: positional arguments must be strings");
                        ok = false;
                        break;
                    }
                    new_params[i] = strdup(AS_STRING(val));
                    if (!new_params[i]) {
                        runtimeError(vm, "set: out of memory");
                        ok = false;
                        break;
                    }
                }
            }
        }

        if (ok) {
            if (gShellPositionalOwned) {
                shellFreeOwnedPositionalParameters();
            } else {
                gParamValues = NULL;
                gParamCount = 0;
            }
            if (new_count > 0) {
                gParamValues = new_params;
                gParamCount = new_count;
                gShellPositionalOwned = true;
                new_params = NULL;
            } else {
                gShellPositionalOwned = false;
            }
        }

        if (new_params) {
            shellFreeParameterArray(new_params, new_count);
        }
    }

    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

/* ERR, DEBUG and RETURN are bash's own, and no signal stands behind them. */
static ShellTrapEntry *shellTrapPseudoEntry(const char *name) {
    if (strcasecmp(name, "ERR") == 0) {
        return &gShellErrTrap;
    }
    if (strcasecmp(name, "DEBUG") == 0) {
        return &gShellDebugTrap;
    }
    if (strcasecmp(name, "RETURN") == 0) {
        return &gShellReturnTrap;
    }
    return NULL;
}

static void shellTrapPrintEntry(const ShellTrapEntry *entry, const char *label) {
    if (entry->action == SHELL_TRAP_ACTION_DEFAULT) {
        return;
    }
    printf("trap -- '");
    const char *command = (entry->action == SHELL_TRAP_ACTION_COMMAND && entry->command) ? entry->command : "";
    for (const char *cursor = command; *cursor; ++cursor) {
        if (*cursor == '\'') {
            fputs("'\\''", stdout);
        } else {
            fputc(*cursor, stdout);
        }
    }
    printf("' %s\n", label);
}

/* NULL prints them all. */
static void shellTrapPrint(const char *only) {
    if (only) {
        ShellTrapEntry *pseudo = shellTrapPseudoEntry(only);
        if (pseudo) {
            char label[16];
            snprintf(label, sizeof(label), "%s", only);
            for (char *cursor = label; *cursor; ++cursor) {
                *cursor = (char)toupper((unsigned char)*cursor);
            }
            shellTrapPrintEntry(pseudo, label);
            fflush(stdout);
            return;
        }
        int signo = 0;
        if (!shellRuntimeParseSignal(only, &signo)) {
            return;
        }
        if (signo == 0) {
            shellTrapPrintEntry(&gShellExitTrap, "EXIT");
        } else {
            for (size_t i = 0; i < sizeof(kShellSignalNames) / sizeof(kShellSignalNames[0]); ++i) {
                if (kShellSignalNames[i].signo == signo) {
                    char label[32];
                    snprintf(label, sizeof(label), "SIG%s", kShellSignalNames[i].name);
                    shellTrapPrintEntry(&gShellSignalTraps[signo], label);
                    break;
                }
            }
        }
        fflush(stdout);
        return;
    }
    shellTrapPrintEntry(&gShellExitTrap, "EXIT");
    for (size_t i = 0; i < sizeof(kShellSignalNames) / sizeof(kShellSignalNames[0]); ++i) {
        int signo = kShellSignalNames[i].signo;
        if (signo > 0 && signo < NSIG) {
            char label[32];
            snprintf(label, sizeof(label), "SIG%s", kShellSignalNames[i].name);
            shellTrapPrintEntry(&gShellSignalTraps[signo], label);
        }
    }
    shellTrapPrintEntry(&gShellDebugTrap, "DEBUG");
    shellTrapPrintEntry(&gShellErrTrap, "ERR");
    shellTrapPrintEntry(&gShellReturnTrap, "RETURN");
    fflush(stdout);
}

static void shellTrapListSignals(void) {
    for (size_t i = 0; i < sizeof(kShellSignalNames) / sizeof(kShellSignalNames[0]); ++i) {
        printf("%2d) SIG%s\n", kShellSignalNames[i].signo, kShellSignalNames[i].name);
    }
    fflush(stdout);
}

Value vmBuiltinShellTrap(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        shellTrapPrint(NULL);
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool ok = true;
    int signal_start = 0;
    const char *handler = NULL;
    ShellTrapAction action = SHELL_TRAP_ACTION_COMMAND;

    if (VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0])) {
        runtimeError(vm, "trap: expected string arguments");
        ok = false;
    }

    if (ok) {
        handler = AS_STRING(args[0]);
        signal_start = 1;

        if (strcmp(handler, "--") == 0) {
            if (arg_count < 2 || VALUE_TYPE(args[1]) != TYPE_STRING || !AS_STRING(args[1])) {
                shellUpdateStatus(0);
                return makeVoid();
            }
            handler = AS_STRING(args[1]);
            signal_start = 2;
        }

        if (strcmp(handler, "-l") == 0) {
            shellTrapListSignals();
            shellUpdateStatus(0);
            return makeVoid();
        }
        if (strcmp(handler, "-p") == 0) {
            if (signal_start >= arg_count) {
                shellTrapPrint(NULL);
            }
            for (int i = signal_start; i < arg_count; ++i) {
                if (VALUE_TYPE(args[i]) == TYPE_STRING && AS_STRING(args[i])) {
                    shellTrapPrint(AS_STRING(args[i]));
                }
            }
            shellUpdateStatus(0);
            return makeVoid();
        }

        if (strcmp(handler, "-") == 0) {
            action = SHELL_TRAP_ACTION_DEFAULT;
            handler = NULL;
        } else if (*handler == '\0') {
            action = SHELL_TRAP_ACTION_IGNORE;
            handler = NULL;
        } else {
            action = SHELL_TRAP_ACTION_COMMAND;
        }
    }

    if (!ok) {
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (signal_start >= arg_count) {
        shellUpdateStatus(0);
        return makeVoid();
    }

    for (int i = signal_start; i < arg_count && ok; ++i) {
        if (VALUE_TYPE(args[i]) != TYPE_STRING || !AS_STRING(args[i])) {
            runtimeError(vm, "trap: expected signal name");
            ok = false;
            break;
        }
        ShellTrapEntry *pseudo = shellTrapPseudoEntry(AS_STRING(args[i]));
        if (pseudo) {
            shellTrapClearEntry(pseudo);
            if (action == SHELL_TRAP_ACTION_COMMAND) {
                ok = shellTrapSetCommand(pseudo, handler);
            } else if (action == SHELL_TRAP_ACTION_IGNORE) {
                pseudo->action = SHELL_TRAP_ACTION_IGNORE;
            }
            continue;
        }
        int signo = 0;
        if (!shellRuntimeParseSignal(AS_STRING(args[i]), &signo)) {
            runtimeError(vm, "trap: invalid signal '%s'", AS_STRING(args[i]));
            ok = false;
            break;
        }
        if (signo == 0) {
            if (!shellRuntimeSetExitTrap(action, handler)) {
                runtimeError(vm, "trap: failed to install EXIT handler");
                ok = false;
                break;
            }
        } else {
            if (!shellRuntimeSetSignalTrap(signo, action, handler)) {
                runtimeError(vm, "trap: failed to install handler for %s", AS_STRING(args[i]));
                ok = false;
                break;
            }
        }
    }

    if (ok) {
        shellRuntimeRefreshTrapEnabled();
    }

    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellLocal(VM *vm, int arg_count, Value *args) {
    /*
     * "local" is primarily used inside shell functions to declare variables
     * with function scope. Our previous stub only flipped the
     * local_scope_active flag, which meant invocations like
     *   local dir=$1
     * failed to bind the requested name/value pair. That broke helper
     * functions in ~/.exshrc (e.g. path_prepend) because $dir was always
     * empty and PATH was never updated.
     */

    if (shellLocalScopeDepth() == 0) {
        runtimeError(vm, "local: can only be used in functions");
        shellUpdateStatus(1);
        return makeVoid();
    }

    /* No arguments: what this function has made. */
    if (arg_count == 0) {
        const ShellLocalScopeFrame *frame = &gShellLocalScopeStack[gShellLocalScopeDepth - 1];
        char **names = (char **)calloc(frame->count ? frame->count : 1, sizeof(char *));
        size_t count = 0;
        for (size_t i = 0; names && i < frame->count; ++i) {
            if (frame->variables[i].name) {
                names[count++] = frame->variables[i].name;
            }
        }
        if (count > 1) {
            qsort(names, count, sizeof(char *), shellParamCompareStrings);
        }
        for (size_t i = 0; i < count; ++i) {
            shellDeclarePrintVariable(names[i]);
        }
        free(names);
        fflush(stdout);
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool ok = true;
    bool parsing_options = true;
    bool integer_attribute = false;
    bool lower_attribute = false;
    bool upper_attribute = false;
    bool mark_readonly = false;
    bool mark_exported = false;
    bool mark_unexported = false;
    for (int i = 0; i < arg_count && ok; ++i) {
        Value v = args[i];
        if (VALUE_TYPE(v) != TYPE_STRING || !AS_STRING(v)) {
            runtimeError(vm, "local: arguments must be strings");
            ok = false;
            break;
        }
        const char *text = AS_STRING(v);
        if (parsing_options && (text[0] == '-' || text[0] == '+') && text[1]) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                continue;
            }
            for (const char *opt = text + 1; *opt; ++opt) {
                bool on = text[0] == '-';
                switch (*opt) {
                    case 'i': integer_attribute = on; break;
                    case 'l': lower_attribute = on; break;
                    case 'u': upper_attribute = on; break;
                    case 'r': mark_readonly = on; break;
                    case 'x': mark_exported = on; mark_unexported = !on; break;
                    case 'a':
                    case 'A':
                    case 'n':
                    case 't':
                        break;
                    default:
                        runtimeError(vm, "local: -%c: invalid option", *opt);
                        ok = false;
                        break;
                }
            }
            continue;
        }
        parsing_options = false;
        const char *eq = strchr(text, '=');
        size_t name_len = eq ? (size_t)(eq - text) : strlen(text);
        if (name_len == 0) {
            runtimeError(vm, "local: invalid identifier '%s'", text);
            ok = false;
            break;
        }

        char *name = (char *)malloc(name_len + 1);
        if (!name) {
            runtimeError(vm, "local: out of memory");
            ok = false;
            break;
        }
        memcpy(name, text, name_len);
        name[name_len] = '\0';

        const char *value = eq ? (eq + 1) : "";
        if (!shellIsValidEnvName(name)) {
            runtimeError(vm, "local: invalid identifier '%s'", name);
            free(name);
            ok = false;
            break;
        }

        bool literal = value[0] == SHELL_ARRAY_LITERAL_MARK;
        if (literal) {
            value++;
        }
        /* One already in this function keeps its value. */
        ShellLocalVariable *already = shellLocalScopeFrameFind(&gShellLocalScopeStack[gShellLocalScopeDepth - 1], name);
        if (!eq && already) {
            if (mark_exported || mark_unexported) {
                already->exported = mark_exported;
            }
            free(name);
            continue;
        }
        char *coerced = NULL;
        if (!literal && integer_attribute && eq) {
            long long number = 0;
            if (!shellArithmeticEvaluateText(value, 0, &number, "local")) {
                shellArithmeticExpansionFail();
                free(name);
                ok = false;
                break;
            }
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%lld", number);
            coerced = strdup(buffer);
        } else if (!literal && (lower_attribute || upper_attribute)) {
            coerced = strdup(value);
            for (char *cursor = coerced; cursor && *cursor; ++cursor) {
                unsigned char c = (unsigned char)*cursor;
                if (c < 0x80) {
                    *cursor = (char)(lower_attribute ? tolower(c) : toupper(c));
                }
            }
        }
        if (!shellLocalScopeSetOnCurrentFrame(name, coerced ? coerced : value, literal)) {
            runtimeError(vm, "local: unable to set '%s'", name);
            free(coerced);
            free(name);
            ok = false;
            break;
        }
        ShellLocalVariable *local = shellLocalScopeFind(name);
        local->no_value = !eq;
        local->attributes = (integer_attribute ? SHELL_VAR_ATTR_INTEGER : 0u) |
                            (lower_attribute ? SHELL_VAR_ATTR_LOWER : 0u) |
                            (upper_attribute ? SHELL_VAR_ATTR_UPPER : 0u);
        if (mark_exported || mark_unexported) {
            local->exported = mark_exported;
        }
        if (mark_readonly) {
            (void)shellReadonlyAdd(name);
        }
        free(coerced);
        free(name);
    }

    gShellRuntime.local_scope_active = false;
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

/* Asked to leave a loop around a command substitution: it ends there, and the loop goes on. */
static void shellLoopControlEndSubstitution(VM *vm) {
    shellUpdateStatus(0);
    shellRuntimeRequestExit();
    if (vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = "exit";
    }
}

/* What break and continue say about their argument. False means it is dealt with. */
static bool shellLoopControlLevels(VM *vm, const char *name, int arg_count, Value *args, int *levels) {
    *levels = 1;
    size_t loops_around = gShellRuntime.loops_around;
    if (gShellLoopStackSize == 0 && loops_around == 0) {
        runtimeError(vm, "%s: only meaningful in a `for', `while', or `until' loop", name);
        shellUpdateStatus(0);
        return false;
    }
    int first = shellSkipDoubleDash(arg_count, args);
    if (arg_count <= first) {
        if (gShellLoopStackSize == 0) {
            shellLoopControlEndSubstitution(vm);
            return false;
        }
        return true;
    }
    const char *text = VALUE_TYPE(args[first]) == TYPE_STRING ? AS_STRING(args[first]) : NULL;
    long value = 0;
    if (!text || !shellParseLoopLevel(text, &value)) {
        /* The shell ends here, without running the rest of the line. */
        runtimeError(vm, "%s: %s: numeric argument required", name, text ? text : "");
        shellUpdateStatus(2);
        shellRuntimeRequestExit();
        if (vm) {
            vm->exit_requested = true;
            vm->current_builtin_name = "exit";
        }
        return false;
    }
    if (arg_count > first + 1) {
        runtimeError(vm, "%s: too many arguments", name);
        shellRuntimeDiscardCommand(vm);
        return false;
    }
    if (value <= 0) {
        /* Every loop ends, continue or not. */
        runtimeError(vm, "%s: %s: loop count out of range", name, text);
        if (loops_around > 0) {
            shellLoopControlEndSubstitution(vm);
            shellUpdateStatus(1);
            return false;
        }
        gShellRuntime.break_requested = true;
        gShellRuntime.break_requested_levels = (int)gShellLoopStackSize;
        shellLoopRequestBreakLevels((int)gShellLoopStackSize);
        shellUpdateStatus(1);
        return false;
    }
    if (value > (long)gShellLoopStackSize && loops_around > 0) {
        shellLoopControlEndSubstitution(vm);
        return false;
    }
    /* More than there are means them all. What is left over would stay asked for. */
    *levels = value > (long)gShellLoopStackSize ? (int)gShellLoopStackSize : (int)value;
    return true;
}

Value vmBuiltinShellBreak(VM *vm, int arg_count, Value *args) {
    int levels = 1;
    if (!shellLoopControlLevels(vm, "break", arg_count, args, &levels)) {
        return makeVoid();
    }
    gShellRuntime.break_requested = true;
    gShellRuntime.break_requested_levels = levels;
    shellLoopRequestBreakLevels(levels);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellContinue(VM *vm, int arg_count, Value *args) {
    int levels = 1;
    if (!shellLoopControlLevels(vm, "continue", arg_count, args, &levels)) {
        return makeVoid();
    }
    gShellRuntime.continue_requested = true;
    gShellRuntime.continue_requested_levels = levels;
    shellLoopRequestContinueLevels(levels);
    shellUpdateStatus(0);
    return makeVoid();
}

#if defined(PSCAL_TARGET_IOS)
#define gShellAliases (*(SC()->has_private_tables ? &SC()->private_aliases : &gShellAliasTable))
#define gShellAliasCount (*(SC()->has_private_tables ? &SC()->private_alias_count : &gShellAliasTableCount))
#else
#define gShellAliases gShellAliasTable
#define gShellAliasCount gShellAliasTableCount
#endif

static void shellWriteAllFd(int fd, const char *buf, size_t len) {
    if (fd < 0 || !buf || len == 0) {
        return;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        off += (size_t)n;
    }
}

static void shellFdVprintf(int fd, const char *fmt, va_list ap) {
    if (!fmt) {
        return;
    }
    char stack_buf[768];
    va_list ap2;
    va_copy(ap2, ap);
    int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap2);
    va_end(ap2);
    if (needed < 0) {
        return;
    }
    if ((size_t)needed < sizeof(stack_buf)) {
        shellWriteAllFd(fd, stack_buf, (size_t)needed);
        return;
    }
    size_t cap = (size_t)needed + 1;
    char *heap = (char *)malloc(cap);
    if (!heap) {
        shellWriteAllFd(fd, stack_buf, strlen(stack_buf));
        return;
    }
    vsnprintf(heap, cap, fmt, ap);
    shellWriteAllFd(fd, heap, (size_t)needed);
    free(heap);
}

static void shellFdPrintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    shellFdVprintf(fd, fmt, ap);
    va_end(ap);
}

static void shellReportRecoverableError(VM *vm, bool with_location, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char fixed[512];
    char *message = fixed;
    va_list measure;
    va_copy(measure, args);
    int needed = vsnprintf(fixed, sizeof(fixed), fmt, measure);
    va_end(measure);
    /* An expression can be any length. */
    if (needed >= (int)sizeof(fixed)) {
        char *grown = (char *)malloc((size_t)needed + 1);
        if (grown) {
            vsnprintf(grown, (size_t)needed + 1, fmt, args);
            message = grown;
        }
    }
    va_end(args);

    const char *script = NULL;
    int line = 0;
    if (with_location) {
        script = shellRuntimeGetArg0();
        if (script && *script) {
            line = shellRuntimeCurrentCommandLine();
        } else {
            script = NULL;
        }
    }

    if (script && line > 0) {
        shellFdPrintf(STDERR_FILENO, "%s: line %d: %s\n", script, line, message);
    } else if (script) {
        shellFdPrintf(STDERR_FILENO, "%s: %s\n", script, message);
    } else {
        shellFdPrintf(STDERR_FILENO, "%s\n", message);
    }
    if (message != fixed) {
        free(message);
    }

    if (vm) {
        vm->abort_requested = false;
    }
}

/* The parser asks: `time` is a keyword unless an alias comes first. */
bool shellRuntimeAliasExpands(const char *name) {
    return shellShoptOptionEnabled("expand_aliases") && shellFindAlias(name) != NULL;
}

static void shellFreeAlias(ShellAlias *alias) {
    if (!alias) {
        return;
    }
    free(alias->name);
    free(alias->value);
    alias->name = NULL;
    alias->value = NULL;
}

static ShellAlias *shellFindAlias(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellAliasCount; ++i) {
        if (strcmp(gShellAliases[i].name, name) == 0) {
            return &gShellAliases[i];
        }
    }
    return NULL;
}

static void shellRemoveAliasAt(size_t index) {
    if (index >= gShellAliasCount) {
        return;
    }
    shellFreeAlias(&gShellAliases[index]);
    if (index + 1 < gShellAliasCount) {
        gShellAliases[index] = gShellAliases[gShellAliasCount - 1];
    }
    gShellAliasCount--;
    if (gShellAliasCount == 0) {
        free(gShellAliases);
        gShellAliases = NULL;
    }
}

static bool shellRemoveAlias(const char *name) {
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < gShellAliasCount; ++i) {
        if (strcmp(gShellAliases[i].name, name) == 0) {
            shellRemoveAliasAt(i);
            return true;
        }
    }
    return false;
}

static void shellClearAliases(void) {
    for (size_t i = 0; i < gShellAliasCount; ++i) {
        shellFreeAlias(&gShellAliases[i]);
    }
    free(gShellAliases);
    gShellAliases = NULL;
    gShellAliasCount = 0;
}

static bool shellSetAlias(const char *name, const char *value) {
    if (!name || !value) {
        return false;
    }
    ShellAlias *existing = shellFindAlias(name);
    if (existing) {
        char *copy = strdup(value);
        if (!copy) {
            return false;
        }
        free(existing->value);
        existing->value = copy;
        return true;
    }
    ShellAlias *new_aliases = realloc(gShellAliases, sizeof(ShellAlias) * (gShellAliasCount + 1));
    if (!new_aliases) {
        return false;
    }
    gShellAliases = new_aliases;
    ShellAlias *alias = &gShellAliases[gShellAliasCount++];
    alias->name = strdup(name);
    alias->value = strdup(value);
    if (!alias->name || !alias->value) {
        free(alias->name);
        free(alias->value);
        gShellAliasCount--;
        return false;
    }
    return true;
}

typedef struct {
    const char *name;
    const char *summary;
    const char *usage;
    const char *detail;
    const char *const *aliases;
    size_t alias_count;
} ShellHelpTopic;

static const char *const kShellHelpSourceAliases[] = {"."};

static const ShellHelpTopic kShellHelpTopics[] = {
    {
        "alias",
        "Define or display shell aliases.",
        "alias [-p] [name[=value] ...]",
        "Without arguments, or with -p, prints the stored alias definitions as "
        "alias name='value'. Each NAME=VALUE argument updates or creates an alias, and "
        "each NAME prints that one, or fails if there is none.",
        NULL,
        0
    },
    {
        "unalias",
        "Remove shell aliases.",
        "unalias [-a] [name ...]",
        "Deletes the aliases identified by NAME. With -a all aliases are removed."
        " Providing NAME alongside -a results in an error.",
        NULL,
        0
    },
    {
        "bind",
        "Configure readline behaviour.",
        "bind [-p] [spec ...]",
        "Accepts readline \"set\" directives and remembers their most recent values."
        " The -p flag prints the stored settings in \"set name value\" form. Other"
        " invocations are currently accepted as no-ops.",
        NULL,
        0
    },
    {
        "bg",
        "Resume a stopped job in the background.",
        "bg [job]",
        "Targets the most recently launched job when no job is supplied. Job"
        " specifiers may be numeric indexes or begin with '%'.",
        NULL,
        0
    },
    {
        "break",
        "Exit from the innermost loop(s).",
        "break [n]",
        "Accepts an optional positive integer count; the default of 1 exits only"
        " the innermost active loop.",
        NULL,
        0
    },
    {
        "builtin",
        "Invoke a PSCAL VM builtin directly.",
        "builtin name [args ...]",
        "Arguments are forwarded to the named VM builtin. Prefix an argument with"
        " int:, float:/double:/real:, bool:/boolean:, str:/string:/raw:, or"
        " nil: to coerce the value; other arguments are passed as strings. When"
        " the VM builtin returns a non-void value it is printed to stdout on"
        " success.",
        NULL,
        0
    },
    {
        "command",
        "Query command resolution metadata.",
        "command [-a] [-p] [-v|-V] [name ...]",
        "With -v prints the first match for each NAME, favouring aliases, functions,"
        " builtins, and executable paths. -V prints verbose descriptions. The -a"
        " flag lists every match and -p searches using the default PATH. Execution"
        " without -v or -V is not currently supported.",
        NULL,
        0
    },
#if defined(PSCAL_TARGET_IOS)
    {
        "which",
        "Locate commands and builtins.",
        "which [-a] [-s] [-p] name ...",
        "Prints how each NAME resolves in the current shell. Aliases, functions,"
        " builtins, and executable files are reported in that order. With -a every"
        " match is listed, -s suppresses output and relies on the exit status, and"
        " -p performs the search using the default PATH instead of the current one.",
        NULL,
        0
    },
#endif
    {
        "enable",
        "Enable or disable shell builtins.",
        "enable [-a] [-n] [-p] [name ...]",
        "Without NAME arguments lists builtins that are currently enabled. With"
        " -n and no names the disabled builtins are printed instead, -a lists every"
        " builtin regardless of status, and -p formats output as enable commands."
        " Supplying NAME arguments toggles those builtins on by default or off"
        " when used with -n.",
        NULL,
        0
    },
    {
        "hash",
        "Display or reset cached command lookups.",
        "hash [-r] [-t] [name ...]",
        "Without arguments prints the cached command table including hit counts. "
        "With -r the table is cleared. Each NAME hashes the executable located via PATH "
        "while -t prints the cached path for each NAME instead of updating the table.",
        NULL,
        0
    },
    {
        "cd",
        "Change the current working directory.",
        "cd [dir]",
        "With no arguments cd switches to $HOME. Successful runs update the PWD"
        " environment variable.",
        NULL,
        0
    },
    {
        "dirs",
        "Display the directory stack.",
        "dirs",
        "Prints the current directory stack with the most recent entry first."
        " Options such as -c are not yet supported.",
        NULL,
        0
    },
    {
        "pushd",
        "Push a directory onto the stack and change to it.",
        "pushd [dir]",
        "With DIR changes to the target directory and pushes the previous working"
        " directory onto the stack. Without arguments swaps the top two entries.",
        NULL,
        0
    },
    {
        "popd",
        "Pop the directory stack.",
        "popd",
        "Removes the top stack entry and switches to the new top directory. Fails"
        " when the stack contains only a single entry.",
        NULL,
        0
    },
    {
        "printf",
        "Format and print data to standard output.",
        "printf format [arguments]",
        "Applies the FORMAT string to each argument using printf-style conversions."
        " Missing arguments expand to empty strings while numeric conversions honour width and precision modifiers.",
        NULL,
        0
    },
    {
        "continue",
        "Skip to the next loop iteration.",
        "continue [n]",
        "Accepts an optional positive integer count and marks the requested"
        " number of enclosing loops to continue.",
        NULL,
        0
    },
    {
        "declare",
        "Declare variables and arrays.",
        "declare [-a|-A|-r] [name[=value] ...]",
        "Without arguments prints variables with attributes. The -a flag"
        " initialises indexed arrays, -A initialises associative arrays, and"
        " -r marks the supplied names as read-only.",
        NULL,
        0
    },
    {
        "readonly",
        "Mark variables as read-only.",
        "readonly [-p] [name[=value] ...]",
        "Marks each NAME as read-only. When assignments are provided the "
        "variable is set before being protected. With no arguments or when "
        "invoked with -p the current read-only definitions are printed.",
        NULL,
        0
    },
    {
        "ulimit",
        "Show or set resource limits.",
        "ulimit [-SHabcdefiklmnpqrstuvxPRT] [limit]",
        "With no limit prints the current one, for -f when no resource is named; -a prints them all."
        " A limit is a number, unlimited, hard or soft. -S and -H pick the soft and the hard limit;"
        " setting takes both unless one is named. Sizes count in 1024 bytes, -p in 512.",
        NULL,
        0
    },
    {
        "umask",
        "Set or display the file creation mask.",
        "umask [-S] [mode]",
        "Without MODE prints the current mask as a zero-padded octal value. "
        "With -S the mask is shown symbolically as u=,g=,o=. Supplying MODE "
        "updates the mask using octal digits 0-7.",
        NULL,
        0
    },
    {
        "eval",
        "Execute words as an inline script.",
        "eval [word ...]",
        "Concatenates the provided words with single spaces and executes the"
        " resulting text without caching bytecode.",
        NULL,
        0
    },
    {
        "exit",
        "Request that the shell terminate.",
        "exit [status]",
        "Marks the shell for exit after running cleanup handlers. If an integer"
        " value is supplied it becomes the process exit code; otherwise the"
        " status defaults to 0.",
        NULL,
        0
    },
    {
        "export",
        "Set environment variables or print the environment.",
        "export [-p] [name[=value] ...]",
        "Without arguments (or with -p) prints the environment as export"
        " assignments. Each name or NAME=VALUE argument updates the process"
        " environment. Only -p and -- are recognised options.",
        NULL,
        0
    },
    {
        "fg",
        "Move a job to the foreground.",
        "fg [job]",
        "Targets the most recently launched job when no argument is supplied."
        " Job specifiers may be numeric indexes or begin with '%'.",
        NULL,
        0
    },
    {
        "finger",
        "Display basic account information.",
        "finger [user]",
        "Prints the login, gecos name, home directory, and shell for the"
        " selected account. Defaults to the current user when no argument is"
        " provided.",
        NULL,
        0
    },
    {
        "help",
        "List builtins or describe a specific builtin.",
        "help [builtin]",
        "Without arguments prints the builtin catalog. Supplying a builtin name"
        " shows its usage summary.",
        NULL,
        0
    },
#if defined(PSCAL_TARGET_IOS)
    {
        "smallclue-help",
        "List available smallclue applets.",
        "smallclue-help",
        "Prints the iOS smallclue applet catalog with brief descriptions.",
        NULL,
        0
    },
#endif
    {
        "stdioinfo",
        "Show stdio routing and session state.",
        "stdioinfo",
        "Prints the current stdin/stdout/stderr descriptor mapping, TTY state,"
        " and on iOS the active vproc session/PTY wiring.",
        NULL,
        0
    },
    {
        "caller",
        "Report information about active call frames.",
        "caller [depth]",
        "Prints the line number and caller context for the current function by "
        "default. Providing DEPTH selects a deeper stack frame, mirroring Bash's "
        "caller builtin output.",
        NULL,
        0
    },
    {
        "history",
        "Print the interactive history list.",
        "history",
        "Writes each recorded interactive command with its history index.",
        NULL,
        0
    },
    {
        "jobs",
        "List background jobs.",
        "jobs [-lnprs] [jobspec ...] or jobs -x command [args]",
        "Reports each job with its number, state and command, and a job that has "
        "ended once. -l adds pids, -p gives only pids, -n only what changed since "
        "the last report, -r running, -s stopped. -x runs COMMAND with each job "
        "specifier in its arguments replaced by that job's pid.",
        NULL,
        0
    },
    {
        "lps",
        "Show synthetic vproc tasks on iOS.",
        "lps",
        "Displays the shell-managed virtual processes instead of the host process"
        " table. Columns include the synthetic PID/PGID, state, and associated"
        " command when available. On platforms with fork/exec support the external"
        " ps is still used.",
        NULL,
        0
    },
    {
        "top",
        "Live view of synthetic vprocs on iOS.",
        "top",
        "Prints a one-shot table of synthetic vproc state including pid, pgid,"
        " foreground marker, state, and utime/stime counters. Additional fields"
        " will be added as process emulation grows.",
        NULL,
        0
    },
    {
        "disown",
        "Remove jobs from the shell's job table.",
        "disown [job ...]",
        "Marks the supplied jobs as disowned so future jobs, fg, bg, and wait "
        "commands ignore them. With no arguments the current job is disowned. "
        "Job specifiers are %N, %%, %+, %-, %name and %?text.",
        NULL,
        0
    },
    {
        "kill",
        "Send signals to processes or jobs.",
        "kill [-s signal|-n signum|-SIGNAL] pid|job ... | kill -l [name|number ...]",
        "Delivers SIGNAL (default TERM) to each PID or job specifier. Numeric targets"
        " follow POSIX kill semantics so negative values address process groups and"
        " zero targets the current process group. With -l and no operands the builtin"
        " lists known signals; when arguments follow -l numeric values print their"
        " signal names and textual names print their corresponding signal numbers.",
        NULL,
        0
    },
    {
        "local",
        "Activate the shell's local scope flag.",
        "local",
        "Sets the runtime flag that marks the current function scope as local-aware."
        " Accepts no arguments.",
        NULL,
        0
    },
    {
        "logout",
        "Exit the shell when running as a login session.",
        "logout [status]",
        "Terminates the shell only when the login_shell shopt option is enabled."
        " With STATUS the supplied numeric value becomes the exit status."
        " Outside a login shell the builtin reports an error and leaves the"
        " session running.",
        NULL,
        0
    },
    {
        "let",
        "Evaluate arithmetic expressions and assignments.",
        "let arg [arg ...]",
        "Each ARG is evaluated with the arithmetic parser. Simple expressions return"
        " their numeric value, while assignments such as NAME=EXPR and the compound"
        " forms NAME+=EXPR, NAME-=EXPR, NAME*=EXPR, NAME/=EXPR, and NAME%=EXPR update"
        " shell variables. The exit status is 0 when the final value is non-zero and"
        " 1 otherwise.",
        NULL,
        0
    },
    {
        "pwd",
        "Print the current working directory.",
        "pwd",
        "Outputs the absolute path returned by getcwd(3).",
        NULL,
        0
    },
    {
        "chmod",
        "Change file permissions.",
        "chmod MODE file [file ...]",
        "Applies the octal MODE to each file path. Only numeric modes are supported "
        "on iOS builds.",
        NULL,
        0
    },
    {
        "read",
        "Read a line from standard input.",
        "read [-p prompt] [name ...]",
        "Reads a line, splits it into words, and assigns them to the requested"
        " environment variables. Without explicit names the value is stored in"
        " REPLY. Only the -p prompt option is supported.",
        NULL,
        0
    },
    {
        "return",
        "Return from the current shell function.",
        "return [status]",
        "Exits the innermost shell function. The optional status is parsed as an"
        " integer and limited to the range 0–255.",
        NULL,
        0
    },
    {
        "set",
        "Update shell option flags.",
        "set [--] [-e|+e] [-o errexit|+o errexit]",
        "Toggles the shell's errexit flag. Options other than -e/+e and"
        " -o/+o errexit are rejected.",
        NULL,
        0
    },
    {
        "shopt",
        "Toggle optional shell behaviours.",
        "shopt [-pqsu] [name ...]",
        "Lists available shell options, reports their state, or updates them. The"
        " implementation recognises the standard Bash shopt flags; -p prints in"
        " command form, -q suppresses output, and -s/-u enable or disable the"
        " named options.",
        NULL,
        0
    },
    {
        "setenv",
        "Set or print environment variables.",
        "setenv [name [value]]",
        "With no arguments prints the environment. NAME assigns an empty string"
        " and NAME VALUE assigns the provided string. Invalid names raise an"
        " error.",
        NULL,
        0
    },
    {
        "type",
        "Describe how the shell interprets names.",
        "type [-af] [-p|-P] [-t] name ...",
        "Without options prints the first match for each NAME. -t prints only the"
        " classification (alias, function, builtin, or file). -p prints the path of"
        " the file that would run, if a file would, -P searches PATH for a file"
        " whatever else NAME is, -a includes every result discovered during lookup,"
        " and -f leaves out functions. An alias counts only while aliases expand.",
        NULL,
        0
    },
    {
        "shift",
        "Rotate positional parameters to the left.",
        "shift [count]",
        "Removes COUNT positional parameters (default 1). COUNT must be a"
        " non-negative integer that does not exceed the current parameter"
        " count.",
        NULL,
        0
    },
    {
        "source",
        "Execute a file in the current shell environment.",
        "source file [args ...]",
        "Loads the named file and executes it without spawning a subshell."
        " Positional parameters are temporarily replaced when arguments are"
        " supplied. The '.' builtin is an alias.",
        kShellHelpSourceAliases,
        1
    },
    {
        "trap",
        "Toggle the shell's trap flag.",
        "trap [commands ...]",
        "Calling trap with arguments enables the runtime trap flag; running it"
        " with no arguments clears the flag. Trap handlers are not yet"
        " parameterised per signal.",
        NULL,
        0
    },
    {
        "times",
        "Display accumulated CPU usage statistics.",
        "times",
        "Prints two lines of timing data: the shell's user and system time"
        " followed by the cumulative user and system time of terminated child"
        " processes. Values are reported as minutes and fractional seconds.",
        NULL,
        0
    },
    {
        "unset",
        "Remove variables from the environment.",
        "unset name [name ...]",
        "Clears each named environment variable via unsetenv(3).",
        NULL,
        0
    },
    {
        "unsetenv",
        "Alias for unset.",
        "unsetenv name [name ...]",
        "This is a synonym for unset and removes environment variables via"
        " unsetenv(3).",
        NULL,
        0
    },
    {
        "wait",
        "Wait for a job to change state.",
        "wait [-fn] [-p var] [id ...]",
        "Waits for the specified jobs (or every job) to finish. -n waits for the next"
        " one to end, -p puts its pid in VAR. Job specifiers are %N, %%, %+, %-,"
        " %name and %?text.",
        NULL,
        0
    },
    {
        ":",
        "Do nothing and succeed.",
        ":",
        "A no-op builtin that always reports success.",
        NULL,
        0
    }
};

static const ShellHelpTopic *shellHelpFindTopic(const char *name) {
    if (!name) {
        return NULL;
    }
    size_t topic_count = sizeof(kShellHelpTopics) / sizeof(kShellHelpTopics[0]);
    for (size_t i = 0; i < topic_count; ++i) {
        const ShellHelpTopic *topic = &kShellHelpTopics[i];
        if (strcasecmp(name, topic->name) == 0) {
            return topic;
        }
        for (size_t j = 0; j < topic->alias_count; ++j) {
            if (topic->aliases && strcasecmp(name, topic->aliases[j]) == 0) {
                return topic;
            }
        }
    }
    return NULL;
}

static void shellHelpPrintOverview(void) {
    printf("help\n");
    printf("exsh is the PSCAL shell front end, providing an interactive environment for orchestrating VM builtins and external commands.\n\n");
    printf("exsh can evaluate shell scripts, manage pipelines, and redirect input and output just like a traditional POSIX-style shell. Use '>' to overwrite files, '>>' to append, and '|' to connect commands.\n\n");
    printf("- Source ~/.exshrc to customise prompts, aliases, and startup behaviour.\n");
    printf("- Use bookmark helpers (bookmark, showmarks, jump) to save and revisit directories quickly.\n");
    printf("- Manage jobs with bg, fg, jobs, wait, and trap.\n");
    printf("- exit leaves the shell; builtin invokes PSCAL VM helpers directly.\n\n");
    printf("- exsh loads ~/.exshrc on startup when the file is present.\n\n");
    printf("- Navigate the interface with familiar terminal controls when used in supporting environments.\n");
    printf("- Edit with vim or pico, transfer data via curl, scp, or sftp, and inspect the network with ping, host, or nslookup.\n");
    printf("- Extend the runtime with PSCAL packages and builtins compiled via the toolchain.\n\n");
    printf("- Compiled scripts are cached in ~/.pscal/bc_cache; use --no-cache to force recompilation\n"
           "  and --prune-cache to remove entries whose source file is gone.\n\n");
    printf("Documentation: %s/exsh_overview.md.\n", PSCAL_DOCS_DIR);
    printf("Support: Report issues on the GitHub PSCAL project tracker or Discord community channels.\n\n");
    printf("Type 'help -l' for a list of functions, or 'help <function>' for help on a specific shell function.\n");
}

static void shellHelpPrintCatalog(void) {
    size_t topic_count = sizeof(kShellHelpTopics) / sizeof(kShellHelpTopics[0]);
    size_t width = 10;
    char display[64];

    printf("exsh builtins. Type 'help <function>' for detailed usage.\n\n");
    printf("%-*s  %s\n", (int)width, "Builtin", "Summary");
    printf("%-*s  %s\n", (int)width, "------", "-------");

    for (size_t i = 0; i < topic_count; ++i) {
        const ShellHelpTopic *topic = &kShellHelpTopics[i];
        const char *name = topic->name;
        if (topic->alias_count > 0 && topic->aliases) {
            snprintf(display, sizeof(display), "%s (%s)", name, topic->aliases[0]);
            name = display;
        }
        printf("%-*s  %s\n", (int)width, name, topic->summary);
    }
}

static void shellHelpPrintTopic(const ShellHelpTopic *topic) {
    if (!topic) {
        return;
    }
    printf("%s - %s\n", topic->name, topic->summary);
    if (topic->alias_count > 0 && topic->aliases) {
        printf("Aliases: ");
        for (size_t i = 0; i < topic->alias_count; ++i) {
            printf("%s%s", topic->aliases[i], (i + 1 < topic->alias_count) ? " " : "\n");
        }
    }
    if (topic->usage && *topic->usage) {
        printf("Usage: %s\n", topic->usage);
    }
    if (topic->detail && *topic->detail) {
        printf("\n%s\n", topic->detail);
    }
}

#if defined(PSCAL_TARGET_IOS)
static void shellHelpPrintSmallclueAppletTopic(const SmallclueApplet *applet) {
    if (!applet || !applet->name || !*applet->name) {
        return;
    }
    printf("%s - %s\n",
           applet->name,
           (applet->description && *applet->description) ? applet->description : "smallclue applet");
    const char *usage = smallclueLookupAppletUsage(applet->name);
    if (usage && *usage) {
        printf("Usage: %s\n", usage);
    } else {
        printf("Usage: smallclue-help %s\n", applet->name);
    }
}
#endif

Value vmBuiltinShellHelp(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        shellHelpPrintOverview();
        shellUpdateStatus(0);
        return makeVoid();
    }

    if (arg_count > 1) {
        runtimeError(vm, "help: expected at most one builtin name");
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0]) || AS_STRING(args[0])[0] == '\0') {
        runtimeError(vm, "help: expected builtin name as string");
        shellUpdateStatus(1);
        return makeVoid();
    }

    const char *requested = AS_STRING(args[0]);
    if (strcmp(requested, "-l") == 0) {
        shellHelpPrintCatalog();
        shellUpdateStatus(0);
        return makeVoid();
    }

    const char *canonical = shellBuiltinCanonicalName(requested);
    const ShellHelpTopic *topic = shellHelpFindTopic(canonical);
    if (!topic) {
#if defined(PSCAL_TARGET_IOS)
        const SmallclueApplet *applet = smallclueFindApplet(requested);
        if (!applet && canonical && *canonical) {
            applet = smallclueFindApplet(canonical);
        }
        if (applet) {
            shellHelpPrintSmallclueAppletTopic(applet);
            shellUpdateStatus(0);
            return makeVoid();
        }
#endif
        runtimeError(vm, "help: unknown builtin '%s'", requested);
        shellUpdateStatus(1);
        return makeVoid();
    }

    shellHelpPrintTopic(topic);
    shellUpdateStatus(0);
    return makeVoid();
}

static void shellPrintAlias(const ShellAlias *alias) {
    shellPrintAliasDefinition(alias->name, alias->value, true);
}

static int shellCompareAliasNames(const void *lhs, const void *rhs) {
    const ShellAlias *left = *(const ShellAlias *const *)lhs;
    const ShellAlias *right = *(const ShellAlias *const *)rhs;
    return strcmp(left->name, right->name);
}

static void shellPrintAllAliases(void) {
    size_t count = gShellAliasCount;
    if (count == 0) {
        return;
    }
    const ShellAlias **sorted = (const ShellAlias **)malloc(count * sizeof(*sorted));
    if (sorted) {
        for (size_t i = 0; i < count; ++i) {
            sorted[i] = &gShellAliases[i];
        }
        qsort(sorted, count, sizeof(*sorted), shellCompareAliasNames);
        for (size_t i = 0; i < count; ++i) {
            shellPrintAlias(sorted[i]);
        }
        free(sorted);
    } else {
        for (size_t i = 0; i < count; ++i) {
            shellPrintAlias(&gShellAliases[i]);
        }
    }
}

Value vmBuiltinShellAlias(VM *vm, int arg_count, Value *args) {
    bool list_all = false;
    int first_name = 0;
    for (; first_name < arg_count; ++first_name) {
        if (VALUE_TYPE(args[first_name]) != TYPE_STRING || !AS_STRING(args[first_name])) {
            break;
        }
        const char *arg = AS_STRING(args[first_name]);
        if (strcmp(arg, "--") == 0) {
            first_name++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') {
            break;
        }
        for (const char *opt = arg + 1; *opt; ++opt) {
            if (*opt != 'p') {
                shellReportRecoverableError(vm, true, "alias: -%c: invalid option", *opt);
                shellReportRecoverableError(vm, false, "alias: usage: alias [-p] [name[=value] ... ]");
                shellUpdateStatus(2);
                return makeVoid();
            }
        }
        list_all = true;
    }
    if (list_all || first_name >= arg_count) {
        shellPrintAllAliases();
        shellUpdateStatus(0);
        return makeVoid();
    }
    bool ok = true;
    for (int i = first_name; i < arg_count; ++i) {
        if (VALUE_TYPE(args[i]) != TYPE_STRING || !AS_STRING(args[i])) {
            runtimeError(vm, "alias: expected name=value");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *assignment = AS_STRING(args[i]);
        const char *eq = strchr(assignment, '=');
        if (!eq || eq == assignment) {
            /* Not a definition, so a question: print this one if there is one. */
            const ShellAlias *found = shellFindAlias(assignment);
            if (found) {
                shellPrintAlias(found);
            } else {
                shellReportRecoverableError(vm, true, "alias: %s: not found", assignment);
                ok = false;
            }
            continue;
        }
        size_t name_len = (size_t)(eq - assignment);
        char *name = strndup(assignment, name_len);
        const char *value = eq + 1;
        if (!name) {
            runtimeError(vm, "alias: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (!shellSetAlias(name, value)) {
            free(name);
            runtimeError(vm, "alias: failed to store alias");
            shellUpdateStatus(1);
            return makeVoid();
        }
        free(name);
    }
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellUnalias(VM *vm, int arg_count, Value *args) {
    bool clear_all = false;
    int index = 0;

    while (index < arg_count) {
        if (VALUE_TYPE(args[index]) != TYPE_STRING || !AS_STRING(args[index])) {
            shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
            shellUpdateStatus(2);
            return makeVoid();
        }
        const char *arg = AS_STRING(args[index]);
        if (strcmp(arg, "--") == 0) {
            index++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') {
            break;
        }
        if (strcmp(arg, "-a") == 0) {
            clear_all = true;
            index++;
            continue;
        }
        shellReportRecoverableError(vm, true, "unalias: %s: invalid option", arg);
        shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
        shellUpdateStatus(2);
        return makeVoid();
    }

    if (clear_all) {
        for (; index < arg_count; ++index) {
            if (VALUE_TYPE(args[index]) != TYPE_STRING || !AS_STRING(args[index]) || AS_STRING(args[index])[0] == '\0') {
                shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
                shellUpdateStatus(2);
                return makeVoid();
            }
        }
        shellClearAliases();
        shellUpdateStatus(0);
        return makeVoid();
    }

    if (index >= arg_count) {
        shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
        shellUpdateStatus(2);
        return makeVoid();
    }

    bool ok = true;
    for (; index < arg_count; ++index) {
        if (VALUE_TYPE(args[index]) != TYPE_STRING || !AS_STRING(args[index]) || AS_STRING(args[index])[0] == '\0') {
            shellReportRecoverableError(vm, false, "unalias: usage: unalias [-a] name [name ...]");
            shellUpdateStatus(2);
            return makeVoid();
        }
        const char *name = AS_STRING(args[index]);
        if (!shellRemoveAlias(name)) {
            shellReportRecoverableError(vm, true, "unalias: %s: not found", name);
            ok = false;
        }
    }

    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

static void shellCallerUsage(void) {
    fprintf(stderr, "caller: usage: caller [expr]\n");
}

/* An entry of the list the three arrays show: the frames, then the script under them. */
static bool shellCallStackEntry(size_t index, const char **function, const char **source, int *line) {
    const ShellCallFrame *frame = shellRuntimeCallFrameAtDepth(index);
    if (frame) {
        *function = frame->function;
        *source = frame->source;
        *line = frame->line;
        return true;
    }
    if (index == shellRuntimeCallStackDepth() && gShellScriptName) {
        *function = "main";
        *source = gShellScriptName;
        *line = 0;
        return true;
    }
    return false;
}

/* A line and a file for no operand; a line, a name and a file for how far out to look. So at the
 * top of a script it is `0 NULL`, and nothing at all in a -c string or at a prompt. */
Value vmBuiltinShellCaller(VM *vm, int arg_count, Value *args) {
    int first = shellSkipDoubleDash(arg_count, args);
    bool has_depth = arg_count > first;
    long depth = 0;
    if (has_depth) {
        const char *text = VALUE_TYPE(args[first]) == TYPE_STRING ? AS_STRING(args[first]) : NULL;
        if (first == 0 && text && text[0] == '-' && text[1]) {
            runtimeError(vm, "caller: -%c: invalid option", text[1]);
            shellCallerUsage();
            shellUpdateStatus(2);
            return makeVoid();
        }
        if (!text || !shellParseLong(text, &depth)) {
            runtimeError(vm, "caller: %s: invalid number", text ? text : "");
            shellCallerUsage();
            shellUpdateStatus(2);
            return makeVoid();
        }
    }

    const char *function = NULL;
    const char *source = NULL;
    int line = 0;
    if (depth < 0 || !shellCallStackEntry((size_t)depth, &function, &source, &line)) {
        shellUpdateStatus(1);
        return makeVoid();
    }
    const char *outer_function = NULL;
    const char *outer_source = NULL;
    int outer_line = 0;
    bool outer = shellCallStackEntry((size_t)depth + 1, &outer_function, &outer_source, &outer_line);
    if (!has_depth) {
        printf("%d %s\n", line, outer ? outer_source : "NULL");
    } else if (outer) {
        printf("%d %s %s\n", line, outer_function, outer_source);
    } else {
        shellUpdateStatus(1);
        return makeVoid();
    }

    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellHistory(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    for (size_t i = 0; i < gShellHistory.count; ++i) {
        printf("%zu  %s\n", i + 1, gShellHistory.entries[i]);
    }
    shellUpdateStatus(0);
    return makeVoid();
}

#if defined(PSCAL_TARGET_IOS)
static ShellJob *shellFindJobByPid(pid_t pid) {
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (!job->pids) {
            continue;
        }
        for (size_t j = 0; j < job->pid_count; ++j) {
            if (job->pids[j] == pid) {
                return job;
            }
        }
    }
    return NULL;
}

static const char *shellPsStateForEntry(const ShellJob *job, const VProcSnapshot *snap) {
    if (snap && snap->exited) {
        return "exited";
    }
    if (job && job->disowned) {
        return "disowned";
    }
    if ((snap && snap->stopped) || (job && job->stopped)) {
        return "stopped";
    }
    if (job && job->running) {
        return "running";
    }
    return "running";
}

static const char *shellPsCommandForEntry(const ShellJob *job, const VProcSnapshot *snap) {
    if (snap && snap->command[0]) {
        return snap->command;
    }
    const int shell_pid = vprocGetShellSelfPid();
    if (snap && snap->pid == shell_pid) {
        if (snap->comm[0]) {
            return snap->comm;
        }
        if (job && job->command && job->command[0]) {
            return job->command;
        }
        return "shell";
    }
    if (job && job->command && job->command[0]) {
        return job->command;
    }
    if (snap) {
        if (snap->comm[0]) {
            return snap->comm;
        }
        if (snap->command[0]) {
            return snap->command;
        }
    }
    return "task";
}

static void shellPsPrintLine(pid_t pid, pid_t pgid, const char *state, int utime_cs, int stime_cs, const char *command) {
    char pgid_buf[16];
    if (pgid > 0) {
        snprintf(pgid_buf, sizeof(pgid_buf), "%d", (int)pgid);
    } else {
        snprintf(pgid_buf, sizeof(pgid_buf), "-");
    }
    double utime_s = 0.0, stime_s = 0.0;
    vprocFormatCpuTimes(utime_cs, stime_cs, &utime_s, &stime_s);
    printf("%-6d %-6s %-9s %-6.1f %-6.1f %s\n", (int)pid, pgid_buf,
           state ? state : "running", utime_s, stime_s, command ? command : "");
}

static void shellPsPrintLineTree(const VProcSnapshot *snap,
                                 pid_t pgid,
                                 const char *state,
                                 const char *command,
                                 int depth) {
    if (!snap) {
        return;
    }
    char pgid_buf[16];
    if (pgid > 0) {
        snprintf(pgid_buf, sizeof(pgid_buf), "%d", (int)pgid);
    } else {
        snprintf(pgid_buf, sizeof(pgid_buf), "-");
    }

    double utime_s = 0.0, stime_s = 0.0;
    vprocFormatCpuTimes(snap->rusage_utime, snap->rusage_stime, &utime_s, &stime_s);

    char indent[96];
    size_t used = 0;
    for (int d = 0; d < depth && used + 2 < sizeof(indent); ++d) {
        indent[used++] = ' ';
        indent[used++] = ' ';
    }
    indent[used] = '\0';

    printf("%-6d %-6d %-6s %-6d %-9s %-6.1f %-6.1f %s%s\n",
           (int)snap->pid,
           (int)snap->parent_pid,
           pgid_buf,
           (int)snap->sid,
           state ? state : "running",
           utime_s,
           stime_s,
           indent,
           command ? command : "");
}
#endif

/* Bash's `jobs -x`: the command with each job spec that names a job swapped for its pid. */
static void shellJobsRunWithPids(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        shellUpdateStatus(0);
        return;
    }
    shellCollectJobs();
    size_t capacity = 1;
    for (int i = 0; i < arg_count; ++i) {
        const char *text = VALUE_TYPE(args[i]) == TYPE_STRING ? AS_STRING(args[i]) : NULL;
        capacity += (text ? strlen(text) * 4 : 0) + 32;
    }
    char *script = (char *)malloc(capacity);
    if (!script) {
        runtimeError(vm, "jobs: out of memory");
        shellUpdateStatus(1);
        return;
    }
    size_t length = 0;
    for (int i = 0; i < arg_count; ++i) {
        const char *text = VALUE_TYPE(args[i]) == TYPE_STRING && AS_STRING(args[i]) ? AS_STRING(args[i]) : "";
        char pid_text[32];
        ShellJobRow row;
        if (text[0] == '%' && shellJobSpecFind(text + 1, &row) == SHELL_JOB_SPEC_FOUND) {
            snprintf(pid_text, sizeof(pid_text), "%ld", (long)shellJobRowPid(&row));
            text = pid_text;
        }
        if (i > 0) {
            script[length++] = ' ';
        }
        script[length++] = '\'';
        for (; *text; ++text) {
            if (*text == '\'') {
                memcpy(script + length, "'\\''", 4);
                length += 4;
            } else {
                script[length++] = *text;
            }
        }
        script[length++] = '\'';
    }
    script[length] = '\0';
    Value eval_arg;
    memset(&eval_arg, 0, sizeof(eval_arg));
    SET_VALUE_TYPE(&eval_arg, TYPE_STRING);
    pscalStringEnsureObj(&eval_arg);
    AS_STRING(eval_arg) = script;
    (void)vmBuiltinShellEval(vm, 1, &eval_arg);
    freeValue(&eval_arg);
}

Value vmBuiltinShellJobs(VM *vm, int arg_count, Value *args) {
    bool pids_only = false;
    bool with_pid = false;
    bool running_only = false;
    bool stopped_only = false;
    bool changed_only = false;
    bool with_pids_in_command = false;
    int first = 0;
    for (; first < arg_count; ++first) {
        const char *text = VALUE_TYPE(args[first]) == TYPE_STRING ? AS_STRING(args[first]) : NULL;
        if (!text || text[0] != '-' || !text[1]) {
            break;
        }
        if (strcmp(text, "--") == 0) {
            first++;
            break;
        }
        for (const char *flag = text + 1; *flag; ++flag) {
            if (*flag == 'x') {
                if (pids_only || with_pid || running_only || stopped_only || changed_only) {
                    runtimeError(vm, "jobs: no other options allowed with `-x'");
                    shellUpdateStatus(1);
                    return makeVoid();
                }
                with_pids_in_command = true;
            } else if (*flag == 'p') {
                pids_only = true;
            } else if (*flag == 'l') {
                with_pid = true;
            } else if (*flag == 'r') {
                running_only = true;
            } else if (*flag == 's') {
                stopped_only = true;
            } else if (*flag == 'n') {
                changed_only = true;
            } else {
                runtimeError(vm, "jobs: -%c: invalid option", *flag);
                fprintf(stderr, "jobs: usage: jobs [-lnprs] [jobspec ...] or jobs -x command [args]\n");
                shellUpdateStatus(2);
                return makeVoid();
            }
        }
    }
    arg_count -= first;
    args += first;
    if (with_pids_in_command) {
        shellJobsRunWithPids(vm, arg_count, args);
        return makeVoid();
    }

    shellCollectJobs();
    shellFinishedJobsForgetWaited();
    ShellJobRow *rows = NULL;
    size_t count = shellJobRows(&rows, false);
    size_t current = SIZE_MAX;
    size_t previous = SIZE_MAX;
    shellJobRowMarks(rows, count, &current, &previous);

    /* Those named, in the order named, or all of them by number. */
    bool ok = true;
    size_t capacity = arg_count > 0 ? (size_t)arg_count : count;
    size_t *chosen = capacity > 0 ? (size_t *)malloc(sizeof(size_t) * capacity) : NULL;
    size_t chosen_count = 0;
    if (arg_count > 0 && chosen) {
        for (int i = 0; i < arg_count; ++i) {
            const char *text = VALUE_TYPE(args[i]) == TYPE_STRING && AS_STRING(args[i]) ? AS_STRING(args[i]) : "";
            if (text[0] != '%') {
                runtimeError(vm, "jobs: warning: %s: job specification requires leading `%%'", text);
            }
            size_t index = 0;
            ShellJobSpecResult result = shellJobSpecFindIn(rows, count, text[0] == '%' ? text + 1 : text, &index);
            if (result == SHELL_JOB_SPEC_FOUND) {
                chosen[chosen_count++] = index;
                continue;
            }
            if (result == SHELL_JOB_SPEC_AMBIGUOUS) {
                const char *what = text[0] == '%' ? text + 1 : text;
                runtimeError(vm, "jobs: %s: ambiguous job spec", what[0] == '?' ? what + 1 : what);
            }
            runtimeError(vm, "jobs: %s: no such job", text);
            ok = false;
        }
    } else if (chosen) {
        for (size_t i = 0; i < count; ++i) {
            chosen[chosen_count++] = i;
        }
    }

    for (size_t i = 0; i < chosen_count; ++i) {
        ShellJobRow *row = &rows[chosen[i]];
        bool stopped = row->job && row->job->stopped;
        if ((running_only && (!row->job || stopped)) || (stopped_only && !stopped) ||
            (changed_only && row->job && row->job->notified)) {
            continue;
        }
        if (pids_only) {
            shellFdPrintf(STDOUT_FILENO, "%ld\n", (long)shellJobRowPid(row));
            continue;
        }
        char marker = chosen[i] == current ? '+' : (chosen[i] == previous ? '-' : ' ');
        shellJobRowPrint(STDOUT_FILENO, row, marker, with_pid);
        if (row->ended) {
            shellFinishedJobReported(row->ended);
        } else {
            row->job->notified = true;
        }
    }
    free(chosen);
    free(rows);
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellDisown(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    bool all = false;
    bool running_only = false;
    bool keep = false;
    int first = 0;
    for (; first < arg_count; ++first) {
        const char *text = VALUE_TYPE(args[first]) == TYPE_STRING ? AS_STRING(args[first]) : NULL;
        if (!text || text[0] != '-' || !text[1]) {
            break;
        }
        if (strcmp(text, "--") == 0) {
            first++;
            break;
        }
        for (const char *flag = text + 1; *flag; ++flag) {
            if (*flag == 'a') {
                all = true;
            } else if (*flag == 'r') {
                running_only = true;
            } else if (*flag == 'h') {
                keep = true;
            } else {
                runtimeError(vm, "disown: -%c: invalid option", *flag);
                fprintf(stderr, "disown: usage: disown [-h] [-ar] [jobspec ... | pid ...]\n");
                shellUpdateStatus(2);
                return makeVoid();
            }
        }
    }
    args += first;
    arg_count -= first;

    /* -h leaves it in the table, marked to miss the HUP. Nothing here sends one on the way out. */
    if (arg_count == 0 && (all || running_only)) {
        for (size_t i = 0; !keep && i < gShellJobCount; ++i) {
            if (all || !gShellJobs[i].stopped) {
                gShellJobs[i].disowned = true;
            }
        }
        ShellRuntimeState *ctx = shellGetContext();
        for (size_t i = 0; !keep && all && ctx && i < ctx->finished_job_count; ++i) {
            shellFinishedJobReported(&ctx->finished_jobs[i]);
        }
        shellUpdateStatus(0);
        return makeVoid();
    }
    if (arg_count == 0) {
        ShellJobRow row;
        if (shellJobSpecFind("", &row) != SHELL_JOB_SPEC_FOUND) {
            runtimeError(vm, "disown: current: no such job");
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (row.ended && !keep) {
            shellFinishedJobReported(row.ended);
        } else if (row.job) {
            row.job->disowned = !keep;
        }
        shellUpdateStatus(0);
        return makeVoid();
    }

    bool ok = true;
    size_t resolved_count = 0;
    size_t *indices = malloc(sizeof(size_t) * (size_t)arg_count);
    if (!indices) {
        runtimeError(vm, "disown: out of memory");
        shellUpdateStatus(1);
        return makeVoid();
    }

    for (int i = 0; i < arg_count; ++i) {
        size_t index = 0;
        if (VALUE_TYPE(args[i]) == TYPE_STRING && AS_STRING(args[i]) && AS_STRING(args[i])[0] == '%') {
            ShellJobRow row;
            if (!shellJobSpecResolve(vm, "disown", AS_STRING(args[i]), &row)) {
                ok = false;
            } else if (row.ended) {
                if (!keep) {
                    shellFinishedJobReported(row.ended);
                }
            } else {
                indices[resolved_count++] = (size_t)(row.job - gShellJobs);
            }
            continue;
        }
        if (!shellParseJobSpecifier(vm, "disown", args[i], &index)) {
            ok = false;
            continue;
        }
        indices[resolved_count++] = index;
    }

    for (size_t i = 0; !keep && i < resolved_count; ++i) {
        gShellJobs[indices[i]].disowned = true;
    }

    free(indices);

    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

static bool shellKillParseLong(Value value, long *out_value) {
    if (!out_value) {
        return false;
    }
    if (IS_INTLIKE(value)) {
        *out_value = (long)AS_INTEGER(value);
        return true;
    }
    if (VALUE_TYPE(value) == TYPE_STRING && AS_STRING(value)) {
        errno = 0;
        char *endptr = NULL;
        long parsed = strtol(AS_STRING(value), &endptr, 10);
        if (errno != 0 || !endptr || *endptr != '\0') {
            return false;
        }
        *out_value = parsed;
        return true;
    }
    return false;
}

static bool shellKillParseSignal(Value value, bool numeric_only, int *out_signo) {
    if (!out_signo) {
        return false;
    }
    if (numeric_only) {
        long parsed = 0;
        if (!shellKillParseLong(value, &parsed)) {
            return false;
        }
        if (parsed < 0 || parsed >= NSIG) {
            return false;
        }
        *out_signo = (int)parsed;
        return true;
    }
    if (VALUE_TYPE(value) == TYPE_STRING && AS_STRING(value)) {
        return shellRuntimeParseSignal(AS_STRING(value), out_signo);
    }
    if (IS_INTLIKE(value)) {
        long parsed = (long)AS_INTEGER(value);
        if (parsed < 0 || parsed >= NSIG) {
            return false;
        }
        *out_signo = (int)parsed;
        return true;
    }
    if (VALUE_TYPE(value) == TYPE_STRING && AS_STRING(value)) {
        return shellRuntimeParseSignal(AS_STRING(value), out_signo);
    }
    return false;
}

static void shellKillPrintSignalTable(void) {
    int column = 0;
    char name[32];
    for (int signo = 1; signo < NSIG; ++signo) {
        if (!shellRuntimeSignalName(signo, true, name, sizeof(name))) {
            continue;
        }
        printf("%2d) %-12s", signo, name);
        column++;
        if (column == 5) {
            printf("\n");
            column = 0;
        } else {
            printf(" ");
        }
    }
    if (column != 0) {
        printf("\n");
    }
}

static bool shellKillSignalJob(VM *vm, const char *spec_text, size_t index, int signo) {
    if (index >= gShellJobCount) {
        fprintf(stderr, "kill: invalid job index\n");
        return false;
    }
    ShellJob *job = &gShellJobs[index];
    bool sent = false;
    bool probe = (signo == 0);
    bool terminate = !probe &&
                     !(signo == SIGTSTP || signo == SIGSTOP || signo == SIGTTIN || signo == SIGTTOU || signo == SIGCONT);
    bool allow_headless_jc = getenv("RUN_VPROC_TESTS") != NULL ||
                             getenv("PSCALI_SHELL_VPROC") != NULL;
    if (probe) {
        if (job->pgid > 0 && shellJobControlSendPgid(job->pgid, 0) == 0) {
            return true;
        }
        for (size_t i = 0; i < job->pid_count; ++i) {
            pid_t pid = job->pids[i];
            if (pid <= 0) {
                continue;
            }
#if defined(PSCAL_TARGET_IOS)
            if (shellJobControlSendPid(pid, 0) == 0 || kill(pid, 0) == 0) {
#else
            if (shellJobControlSendPid(pid, 0) == 0) {
#endif
                return true;
            }
        }
        if (allow_headless_jc) {
            /* In headless test mode treat missing tasks as already gone. */
            return true;
        }
        fprintf(stderr, "kill: %s: no associated processes\n", spec_text ? spec_text : "job");
        return false;
    }
    bool use_group_kill = terminate && job->pgid > 0;
#if defined(PSCAL_TARGET_IOS)
    /* iOS/vproc mode should only target tracked job members. Group-directed
     * delivery can leak to non-job tasks when synthetic pgid mappings drift.
     */
    use_group_kill = false;
#endif
    if (use_group_kill) {
        /* Deliver to the whole synthetic group; safer for pipelines and job-spec parity. */
        if (shellJobControlSendPgid(job->pgid, signo) == 0) {
            sent = true;
        }
    }

    for (size_t i = 0; i < job->pid_count; ++i) {
        pid_t pid = job->pids[i];
        if (pid <= 0) {
            continue;
        }
        if (!terminate || !sent) {
            /* If group kill failed or we are delivering non-terminating signals, send per-pid. */
            if (shellJobControlSendPid(pid, signo) != 0) {
#if defined(PSCAL_TARGET_IOS)
                if (errno == ESRCH) {
                    /* If the virtual PID is unknown, try a best-effort host kill;
                     * if that also fails, treat the slot as gone so the job table
                     * doesn't block shell teardown in headless test runs. */
                    if (kill(pid, signo) == 0) {
                        sent = true;
                        continue;
                    }
                    job->pids[i] = -1;
                    sent = true;
                    continue;
                }
#endif
                if (allow_headless_jc && errno == ESRCH) {
                    job->pids[i] = -1;
                    sent = true;
                    continue;
                }
                fprintf(stderr, "kill: %s: %s\n",
                        spec_text ? spec_text : "job",
                        strerror(errno));
                return false;
            }
        }
        sent = true;
        if (!terminate) {
            ShellWaitResult wait_res;
            int err = shellWaitPidDetailed(pid, true, &wait_res);
            if (err != 0) {
                continue;
            }
            if (wait_res.stopped) {
                shellJobMarkStopped(job, wait_res.status_mapped);
            } else if (WIFEXITED(wait_res.status_raw) || WIFSIGNALED(wait_res.status_raw)) {
                job->pids[i] = -1;
            } else {
                job->pids[i] = -1;
            }
        } else {
            int status = 0;
            (void)vprocWaitPidShim(pid, &status, WNOHANG);
        }
    }
    if (!sent) {
        fprintf(stderr, "kill: %s: no associated processes\n",
                spec_text ? spec_text : "job");
        return false;
    }

    /* If every pid was cleared, hide the job so jobs/kill %N no longer see it. */
    if (allow_headless_jc) {
        bool any_pid = false;
        for (size_t i = 0; i < job->pid_count; ++i) {
            if (job->pids[i] > 0) {
                any_pid = true;
                break;
            }
        }
        if (!any_pid) {
            job->running = false;
            job->stopped = false;
            job->disowned = true;
        }
    }

    job->last_status = 128 + signo;
    if (signo == SIGTSTP || signo == SIGSTOP || signo == SIGTTIN || signo == SIGTTOU) {
        shellJobMarkStopped(job, job->last_status);
    } else {
        job->running = false;
        job->stopped = false;
    }
    if (terminate) {
        job->term_signal = signo;
        ShellFinishedJob *entry = shellRememberFinishedJob(job, false);
        if (entry) {
            entry->killed_now = true;
        }
        shellRemoveJobAt(index);
        shellCollectJobs();
        return true;
    }
    return true;
}

Value vmBuiltinShellKill(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    bool kill_dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
    if (kill_dbg) {
        fprintf(stderr, "[kill] job_count=%zu args=%d\n", gShellJobCount, arg_count);
        for (size_t i = 0; i < gShellJobCount; ++i) {
            ShellJob *job = &gShellJobs[i];
            fprintf(stderr, "[kill]   job idx=%zu id=%zu pgid=%d disowned=%d stopped=%d running=%d pid_count=%zu\n",
                    i, job->id, (int)job->pgid, (int)job->disowned, (int)job->stopped, (int)job->running, job->pid_count);
            for (size_t j = 0; j < job->pid_count; ++j) {
                fprintf(stderr, "[kill]     pid[%zu]=%d\n", j, (int)job->pids[j]);
            }
        }
    }

    bool parsing_options = true;
    bool expect_signal_value = false;
    char pending_option = '\0';
    bool list_mode = false;
    int list_start = arg_count;
    int operand_start = arg_count;
    int signal_number = SIGTERM;

    for (int i = 0; i < arg_count;) {
        Value value = args[i];
        const char *text = (VALUE_TYPE(value) == TYPE_STRING) ? AS_STRING(value) : NULL;

        if (expect_signal_value) {
            bool numeric_only = (pending_option == 'n');
            int signo = 0;
            if (!shellKillParseSignal(value, numeric_only, &signo)) {
                fprintf(stderr, "kill: invalid signal\n");
                shellUpdateStatus(1);
                return makeVoid();
            }
            signal_number = signo;
            expect_signal_value = false;
            pending_option = '\0';
            parsing_options = true;
            ++i;
            continue;
        }

        if (list_mode) {
            break;
        }

        if (parsing_options && text) {
            if (strcmp(text, "--") == 0) {
                parsing_options = false;
                operand_start = i + 1;
                ++i;
                break;
            }
            if (strcmp(text, "-l") == 0) {
                list_mode = true;
                list_start = i + 1;
                ++i;
                break;
            }
            if (strcmp(text, "-s") == 0 || strcmp(text, "-n") == 0) {
                pending_option = text[1];
                expect_signal_value = true;
                ++i;
                continue;
            }
            if (text[0] == '-' && text[1] != '\0') {
                int signo = 0;
                if (shellRuntimeParseSignal(text + 1, &signo)) {
                    signal_number = signo;
                    parsing_options = false;
                    operand_start = i + 1;
                    ++i;
                    break;
                }
            }
            parsing_options = false;
            operand_start = i;
            break;
        } else if (parsing_options) {
            parsing_options = false;
            operand_start = i;
            break;
        } else {
            operand_start = i;
            break;
        }
    }

    if (expect_signal_value) {
        fprintf(stderr, "kill: option requires an argument\n");
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (list_mode) {
        bool ok = true;
        if (list_start >= arg_count) {
            shellKillPrintSignalTable();
        } else {
            for (int i = list_start; i < arg_count; ++i) {
                Value value = args[i];
                long numeric = 0;
                if (shellKillParseLong(value, &numeric)) {
                    char name[32];
                    if (!shellRuntimeSignalName((int)numeric, false, name, sizeof(name))) {
                        fprintf(stderr, "kill: %ld: unknown signal\n", numeric);
                        ok = false;
                        continue;
                    }
                    printf("%s\n", name);
                    continue;
                }
                if (VALUE_TYPE(value) != TYPE_STRING || !AS_STRING(value)) {
                    runtimeError(vm, "kill: invalid signal");
                    ok = false;
                    continue;
                }
                int signo = 0;
                if (!shellRuntimeParseSignal(AS_STRING(value), &signo)) {
                    fprintf(stderr, "kill: invalid signal '%s'\n", AS_STRING(value));
                    ok = false;
                    continue;
                }
                printf("%d\n", signo);
            }
        }
        shellUpdateStatus(ok ? 0 : 1);
        return makeVoid();
    }

    if (operand_start >= arg_count) {
        fprintf(stderr, "kill: missing operand\n");
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool overall_ok = true;
    bool signalled = false;
    for (int i = operand_start; i < arg_count; ++i) {
        Value target = args[i];
        if (VALUE_TYPE(target) == TYPE_STRING && AS_STRING(target)) {
            const char *text = AS_STRING(target);
            while (isspace((unsigned char)*text)) {
                ++text;
            }
            if (*text == '%') {
                ShellJobRow row;
                ShellJobSpecResult found = shellJobSpecFind(text + 1, &row);
                if (found == SHELL_JOB_SPEC_FOUND) {
                    /* One that has ended is still there to be named, and takes no signal. */
                    if (row.job) {
                        if (shellKillSignalJob(vm, AS_STRING(target), (size_t)(row.job - gShellJobs),
                                               signal_number)) {
                            signalled = true;
                        } else {
                            overall_ok = false;
                        }
                    }
                } else if (found == SHELL_JOB_SPEC_AMBIGUOUS) {
                    runtimeError(vm, "kill: %s: ambiguous job spec", text[1] == '?' ? text + 2 : text + 1);
                    overall_ok = false;
                } else {
                    runtimeError(vm, "kill: %s: no such job", AS_STRING(target));
                    overall_ok = false;
                }
                continue;
            }
        }
        long pid_value = 0;
        if (!shellKillParseLong(target, &pid_value)) {
            if (VALUE_TYPE(target) == TYPE_STRING && AS_STRING(target)) {
                fprintf(stderr, "kill: invalid pid '%s'\n", AS_STRING(target));
            } else {
                fprintf(stderr, "kill: invalid pid\n");
            }
            overall_ok = false;
            continue;
        }
        int rc = 0;
        pid_t target_pid = (pid_t)pid_value;
        if (target_pid < 0) {
            pid_t pgid = -target_pid;
            bool any = false;
            for (size_t j = 0; j < gShellJobCount; ++j) {
                ShellJob *job = &gShellJobs[j];
                if (job->pgid == pgid) {
                    for (size_t k = 0; k < job->pid_count; ++k) {
                        if (job->pids[k] > 0) {
                            any = true;
                            shellJobControlSendPid(job->pids[k], signal_number);
                        }
                    }
                }
            }
            if (!any) {
                rc = shellJobControlSendPgid(pgid, signal_number);
            }
        }
        /* The shell's own group. shellJobControlSendPid takes 0 for a job that has no group yet. */
        else if (target_pid == 0) {
            rc = kill(0, signal_number);
        }
#if defined(PSCAL_TARGET_IOS)
        /* To itself, with a trap waiting: Ctrl-C's route through the vproc would not get there. */
        else if (target_pid == shellRuntimeOwnPid() && signal_number > 0 && signal_number < 32 &&
                 shellRuntimeGetSignalTrapAction(signal_number) == SHELL_TRAP_ACTION_COMMAND) {
            vprocNoteCaughtSignal(target_pid, signal_number);
        }
#endif
        else {
            rc = shellJobControlSendPid(target_pid, signal_number);
#if !defined(PSCAL_TARGET_IOS)
            /* A thread of this process may be the one that takes it, a moment from now. */
            if (rc == 0 && target_pid == getpid() && signal_number > 0 && signal_number < NSIG &&
                shellRuntimeGetSignalTrapAction(signal_number) == SHELL_TRAP_ACTION_COMMAND) {
                for (int waited = 0; waited < 200 && !gShellPendingSignals[signal_number]; ++waited) {
                    usleep(1000);
                }
            }
#endif
        }
        if (rc != 0) {
            fprintf(stderr, "kill: (%ld) %s\n", pid_value, strerror(errno));
            overall_ok = false;
        } else {
            signalled = true;
        }
    }
    if (signalled && signal_number != 0) {
        gShellRuntime.kill_command = gShellRuntime.commands_run;
    }

    /* Pull in any state changes caused by the signals (stops, exits) so jobs/
     * lps reflect the new status immediately. */
    shellCollectJobs();
    if (overall_ok) {
        /* kill reports success/failure of signal delivery, not the target's
         * eventual exit code. Make sure asynchronous reaping does not leak
         * 128+SIGNO into the command status on successful delivery.
         */
        shellRuntimeSetLastStatus(0);
    } else {
        shellUpdateStatus(1);
    }
    return makeVoid();
}

static bool shellJobCommandLooksLikeWatch(const char *command) {
    if (!command) {
        return false;
    }
    if (strncmp(command, "watch", 5) != 0) {
        return false;
    }
    return command[5] == '\0' || isspace((unsigned char)command[5]);
}

static bool shellJobHasThreadlessSnapshot(const ShellJob *job, bool *out_has_nonresumable) {
    if (!job || !job->pids || job->pid_count == 0) {
        if (out_has_nonresumable) {
            *out_has_nonresumable = false;
        }
        return false;
    }
    bool dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
    size_t snap_cap = vprocSnapshot(NULL, 0);
    if (snap_cap == 0) {
        if (out_has_nonresumable) {
            *out_has_nonresumable = false;
        }
        return false;
    }
    VProcSnapshot *snaps = (VProcSnapshot *)calloc(snap_cap, sizeof(VProcSnapshot));
    if (!snaps) {
        if (out_has_nonresumable) {
            *out_has_nonresumable = false;
        }
        return false;
    }
    size_t snap_count = vprocSnapshot(snaps, snap_cap);
    pthread_t zero_tid;
    memset(&zero_tid, 0, sizeof(zero_tid));
    bool has_live_thread = false;
    bool has_snapshot_without_thread = false;
    bool has_threadless_non_resumable = false;
    bool has_untracked_pid = false;

    for (size_t j = 0; j < job->pid_count; ++j) {
        pid_t pid = job->pids[j];
        if (pid <= 0) {
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < snap_count; ++i) {
            if (snaps[i].pid != (int)pid) {
                continue;
            }
            found = true;
            if (dbg) {
                fprintf(stderr,
                        "[jobs-ios] fg/bg probe pid=%d tid=%p exited=%d stopped=%d continued=%d\n",
                        (int)pid,
                        (void *)snaps[i].tid,
                        (int)snaps[i].exited,
                        (int)snaps[i].stopped,
                        (int)snaps[i].continued);
            }
            if (!snaps[i].exited) {
                if (memcmp(&snaps[i].tid, &zero_tid, sizeof(zero_tid)) != 0) {
                    has_live_thread = true;
                } else {
                    has_snapshot_without_thread = true;
#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)
                    if (!vprocGetStopUnsupported((int)pid)) {
                        has_threadless_non_resumable = true;
                    }
#endif
                }
            }
            break;
        }
        if (!found) {
            if (dbg) {
                fprintf(stderr, "[jobs-ios] fg/bg probe pid=%d missing from snapshot\n", (int)pid);
            }
            has_untracked_pid = true;
        }
    }

    free(snaps);
    if (out_has_nonresumable) {
        *out_has_nonresumable = has_threadless_non_resumable;
    }
    return !has_live_thread && has_snapshot_without_thread && !has_untracked_pid;
}

static bool shellJobIsVirtualStoppedWithoutThread(const ShellJob *job) {
    bool has_nonresumable = false;
    if (!shellJobHasThreadlessSnapshot(job, &has_nonresumable)) {
        return false;
    }
    if (shellJobCommandLooksLikeWatch(job ? job->command : NULL)) {
        return false;
    }
    return has_nonresumable;
}

/* Bash has them at a prompt and under set -m, and says so anywhere else. */
static bool shellJobControlBuiltinAllowed(VM *vm, const char *name) {
    if (!gShellRuntime.job_control_gone && (gShellRuntime.monitor_enabled || shellRuntimeIsInteractive())) {
        return true;
    }
    runtimeError(vm, "%s: no job control", name);
    shellUpdateStatus(1);
    return false;
}

Value vmBuiltinShellFg(VM *vm, int arg_count, Value *args) {
    if (!shellJobControlBuiltinAllowed(vm, "fg")) {
        return makeVoid();
    }
    shellCollectJobs();
    shellEnsureJobControl();
    size_t index = 0;
    if (!shellResolveJobIndex(vm, "fg", arg_count, args, &index)) {
        shellUpdateStatus(1);
        return makeVoid();
    }
    ShellJob *job = &gShellJobs[index];
    if (shellJobCommandLooksLikeWatch(job->command) &&
        shellJobHasThreadlessSnapshot(job, NULL)) {
        char *resume_command = job->command ? strdup(job->command) : NULL;
        if (!resume_command || resume_command[0] == '\0') {
            free(resume_command);
            shellUpdateStatus(1);
            return makeVoid();
        }
        size_t resumed_job_id = job->id;
        shellRemoveJobAt(index);
        shellFdPrintf(STDOUT_FILENO, "\033[2J\033[H");
        shellFdPrintf(STDOUT_FILENO, "[%zu]  + resumed  %s\n", resumed_job_id, resume_command);
        Value eval_arg;
        memset(&eval_arg, 0, sizeof(eval_arg));
        SET_VALUE_TYPE(&eval_arg, TYPE_STRING);
        pscalStringEnsureObj(&eval_arg); // no wrapper yet after memset
        AS_STRING(eval_arg) = resume_command;
        (void)vmBuiltinShellEval(vm, 1, &eval_arg);
        // freeValue (not a raw free(resume_command)) so the StringObj
        // wrapper pscalStringEnsureObj allocated above is released too,
        // not just the buffer it owns.
        freeValue(&eval_arg);
        return makeVoid();
    }
    if (shellJobIsVirtualStoppedWithoutThread(job)) {
        const char *name = (job->command && job->command[0]) ? job->command : "job";
        shellFdPrintf(STDERR_FILENO,
                      "exsh: fg: cannot resume '%s' (VM restore for stopped local vprocs not available yet)\n",
                      name);
        shellJobMarkStopped(job, job->last_status ? job->last_status : (128 + SIGTSTP));
        shellUpdateStatus(1);
        return makeVoid();
    }
    bool job_control = gShellRuntime.job_control_enabled && job->pgid > 0;
    if (job_control) {
        shellJobControlSetForeground(job->pgid);
    }
    /* Prefer group continue; fallback to per-pid to cover synthetic tasks. */
    if (!shellJobContinue(job, true)) {
        if (job_control) {
            shellJobControlRestoreForeground();
        }
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (job_control && shellRuntimeIsInteractive()) {
        shellFdPrintf(STDOUT_FILENO, "\033[2J\033[H");
        if (job->command && job->command[0]) {
            shellFdPrintf(STDOUT_FILENO, "[%zu]  + continued  %s\n", job->id, job->command);
        } else {
            shellFdPrintf(STDOUT_FILENO, "[%zu]  + continued\n", job->id);
        }
    } else {
        shellFdPrintf(STDOUT_FILENO, "%s\n", job->text ? job->text : (job->command ? job->command : ""));
    }
    shellJobMarkRunning(job);
    int final_status = job->last_status;
    bool waited_any = false;
    bool saw_echild = false;
    for (size_t i = 0; i < job->pid_count; ++i) {
        pid_t pid = job->pids[i];
        if (pid <= 0) {
            continue;
        }
        ShellWaitResult wait_res;
        int err = shellWaitPidDetailed(pid, true, &wait_res);
        if (err != 0) {
            if (err == ECHILD) {
                saw_echild = true;
            }
            continue;
        }
        waited_any = true;
        if (wait_res.stopped) {
            shellJobMarkStopped(job, wait_res.status_mapped);
            if (job_control) {
                shellJobControlRestoreForeground();
            }
            shellUpdateStatus(job->last_status);
            return makeVoid();
        }
        final_status = shellStatusFromWait(wait_res.status_raw);
        job->pids[i] = -1;
    }
    if (job_control) {
        shellJobControlRestoreForeground();
    }
    if (!waited_any && saw_echild) {
        /* Keep job state intact when virtual parentage has not converged yet. */
        shellUpdateStatus(1);
        return makeVoid();
    }
    shellRemoveJobAt(index);
    shellUpdateStatus(final_status);
    return makeVoid();
}

Value vmBuiltinShellBg(VM *vm, int arg_count, Value *args) {
    if (!shellJobControlBuiltinAllowed(vm, "bg")) {
        return makeVoid();
    }
    shellCollectJobs();
    shellEnsureJobControl();
    size_t index = 0;
    if (!shellResolveJobIndex(vm, "bg", arg_count, args, &index)) {
        shellUpdateStatus(1);
        return makeVoid();
    }
    ShellJob *job = &gShellJobs[index];
    if (shellJobIsVirtualStoppedWithoutThread(job)) {
        const char *name = (job->command && job->command[0]) ? job->command : "job";
        shellFdPrintf(STDERR_FILENO,
                      "exsh: bg: cannot resume '%s' (VM restore for stopped local vprocs not available yet)\n",
                      name);
        shellJobMarkStopped(job, job->last_status ? job->last_status : (128 + SIGTSTP));
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (!job->stopped) {
        runtimeError(vm, "bg: job %zu already in background", job->id);
        shellUpdateStatus(0);
        return makeVoid();
    }
    shellFdPrintf(STDOUT_FILENO, "[%zu]+ %s &\n", job->id, job->text ? job->text : (job->command ? job->command : ""));
    shellJobContinue(job, true);
    shellUpdateStatus(0);
    return makeVoid();
}

/* wait ended early for this signal: its status is the command's, and the trap runs now. */
static void shellWaitInterrupted(int signo) {
    shellUpdateStatus(128 + signo);
    shellRuntimeProcessPendingSignals();
}

/* A number is a pid and a % spec a job. False if it is neither. */
typedef struct {
    long target;
    bool by_id;
    /* More than one job goes by that name. */
    bool ambiguous;
    const char *text;
} ShellWaitOperand;

static bool shellWaitParseOperand(Value spec, ShellWaitOperand *op) {
    op->target = -1;
    op->by_id = false;
    op->ambiguous = false;
    op->text = "";
    if (VALUE_TYPE(spec) == TYPE_STRING && AS_STRING(spec)) {
        op->text = AS_STRING(spec);
        op->by_id = op->text[0] == '%';
        if (op->by_id) {
            ShellJobRow row;
            ShellJobSpecResult found = shellJobSpecFind(op->text + 1, &row);
            if (found == SHELL_JOB_SPEC_FOUND) {
                op->target = (long)row.id;
                return true;
            }
            if (found == SHELL_JOB_SPEC_AMBIGUOUS) {
                op->ambiguous = true;
                return false;
            }
            /* A number is looked for among the finished too: `wait %1` twice. */
        }
        const char *digits = op->by_id ? op->text + 1 : op->text;
        char *end = NULL;
        long parsed = *digits ? strtol(digits, &end, 10) : -1;
        if (end && *end == '\0' && parsed > 0) {
            op->target = parsed;
        }
    } else if (IS_INTLIKE(spec)) {
        op->target = (long)AS_INTEGER(spec);
    }
    return op->target > 0;
}

static bool shellWaitOperandIsJob(const ShellWaitOperand *op, const ShellJob *job) {
    if (op->by_id) {
        return job->id == (size_t)op->target && !job->disowned;
    }
    if (job->last_pid == (pid_t)op->target) {
        return true;
    }
    for (size_t i = 0; i < job->pid_count; ++i) {
        if (job->pids[i] == (pid_t)op->target) {
            return true;
        }
    }
    return false;
}

static bool shellWaitOperandIsFinished(const ShellWaitOperand *op, const ShellFinishedJob *entry) {
    return op->by_id ? entry->has_id && entry->id == (size_t)op->target
                     : entry->last_pid == (pid_t)op->target || entry->pgid == (pid_t)op->target;
}

/* wait -n's operands that it can do anything with, each reported if not. */
static size_t shellWaitNextOperands(Value *args, int count, ShellWaitOperand *ops) {
    ShellRuntimeState *ctx = shellGetContext();
    size_t kept = 0;
    for (int i = 0; i < count; ++i) {
        ShellWaitOperand op;
        bool parsed = shellWaitParseOperand(args[i], &op);
        bool known = false;
        for (size_t j = 0; parsed && !known && j < gShellJobCount; ++j) {
            known = shellWaitOperandIsJob(&op, &gShellJobs[j]);
        }
        for (size_t j = 0; parsed && !known && ctx && j < ctx->finished_job_count; ++j) {
            const ShellFinishedJob *entry = &ctx->finished_jobs[j];
            known = shellWaitOperandIsFinished(&op, entry) && (!entry->reported || entry->next_repeats);
        }
        if (known) {
            ops[kept++] = op;
        } else if (op.by_id) {
            fprintf(stderr, "wait: %s: no such job\n", op.text);
        } else {
            fprintf(stderr, "wait: `%s': not a pid or valid job spec\n", op.text);
        }
    }
    return kept;
}

/* wait -n: a job that already ended and nothing has reported, or else the next to end. Operands
 * narrow it to those. A pid wait -n gave stays the answer to the next wait -n naming it (5.3). */
static void shellWaitForNextJob(Value *args, int arg_count, pid_t *pid_out) {
    ShellWaitOperand *ops = NULL;
    size_t op_count = 0;
    if (arg_count > 0) {
        ops = (ShellWaitOperand *)calloc((size_t)arg_count, sizeof(ShellWaitOperand));
        if (!ops) {
            shellUpdateStatus(1);
            return;
        }
        op_count = shellWaitNextOperands(args, arg_count, ops);
        if (op_count == 0) {
            free(ops);
            shellUpdateStatus(127);
            return;
        }
    }
    for (;;) {
        int signo = shellRuntimeSignalEndingWait();
        if (signo > 0) {
            shellWaitInterrupted(signo);
            break;
        }
        ShellRuntimeState *ctx = shellGetContext();
        ShellFinishedJob *found = NULL;
        for (int pass = 0; !found && pass < 2; ++pass) {
            for (size_t i = 0; !found && ctx && i < ctx->finished_job_count; ++i) {
                ShellFinishedJob *entry = &ctx->finished_jobs[i];
                if (pass == 0 ? (!entry->next_repeats || op_count == 0) : entry->reported) {
                    continue;
                }
                bool wanted = op_count == 0;
                for (size_t j = 0; !wanted && j < op_count; ++j) {
                    wanted = shellWaitOperandIsFinished(&ops[j], entry);
                }
                found = wanted ? entry : NULL;
            }
        }
        if (found) {
            found->reported = true;
            found->has_id = false;
            found->next_repeats = op_count > 0;
            *pid_out = found->last_pid;
            shellUpdateStatus(found->status);
            break;
        }
        bool running = false;
        int settled = -1;
        for (size_t i = 0; i < gShellJobCount && !running && settled < 0; ++i) {
            ShellJob *job = &gShellJobs[i];
            bool wanted = op_count == 0;
            for (size_t j = 0; !wanted && j < op_count; ++j) {
                wanted = shellWaitOperandIsJob(&ops[j], job);
            }
            if (!wanted) {
                continue;
            }
            if (op_count > 0 && (job->disowned || job->inherited)) {
                /* Bash calls what it has no child for ended, and gives 0. */
                if (job->inherited) {
                    shellRemoveJobAt(i);
                }
                settled = 0;
            } else {
                running = !job->disowned && !job->inherited;
            }
        }
        if (settled >= 0) {
            shellUpdateStatus(settled);
            break;
        }
        if (!running) {
            /* A substitution's copy of a job the shell started is no child of its own. Bash finds
             * it has no child, calls them all ended, and gives 0 for each in turn. */
            int status = 127;
            for (size_t i = 0; op_count == 0 && i < gShellJobCount && status != 0; ++i) {
                if (gShellJobs[i].inherited && !gShellJobs[i].disowned) {
                    shellRemoveJobAt(i);
                    status = 0;
                }
            }
            shellUpdateStatus(status);
            break;
        }
        usleep(5000);
        shellCollectJobs();
    }
    free(ops);
}

/* Waits for what is left of the job, which leaves it for the caller to take off the table.
 * Gives what the job's last command gave. A signal can end this early, with *signo_out set and
 * the job still running. */
static int shellWaitForJobAt(size_t index, int *signo_out) {
    ShellJob *job = &gShellJobs[index];
    int status = job->last_status;
    for (size_t i = 0; i < job->pid_count; ++i) {
        pid_t pid = job->pids[i];
        if (pid <= 0) {
            continue;
        }
        ShellWaitResult wait_res;
        int err = shellWaitPidForBuiltin(pid, &wait_res, signo_out);
        if (*signo_out > 0) {
            return status;
        }
        if (err != 0) {
            continue;
        }
        if (i + 1 == job->pid_count) {
            status = wait_res.status_mapped;
        }
        job->pids[i] = -1;
    }
    job->last_status = status;
    return status;
}

/* One operand, which may have ended long ago. */
static int shellWaitForOperand(VM *vm, Value spec, int *signo_out, pid_t *pid_out) {
    ShellWaitOperand operand;
    bool parsed = shellWaitParseOperand(spec, &operand);
    long target = operand.target;
    bool by_id = operand.by_id;
    const char *text = operand.text;
    if (operand.ambiguous) {
        runtimeError(vm, "wait: %s: ambiguous job spec", text[1] == '?' ? text + 2 : text + 1);
        return 127;
    }
    if (!parsed) {
        if (by_id) {
            runtimeError(vm, "wait: %s: no such job", text);
            return 127;
        }
        runtimeError(vm, "wait: `%s': not a pid or valid job spec", text);
        return 1;
    }

    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (!shellWaitOperandIsJob(&operand, job) || job->inherited) {
            continue;
        }
        if (job->disowned) {
            return 0;
        }
        *pid_out = job->last_pid;
        int status = shellWaitForJobAt(i, signo_out);
        if (*signo_out > 0) {
            return status;
        }
        (void)shellRememberFinishedJob(job, true);
        shellRemoveJobAt(i);
        return status;
    }

    ShellFinishedJob *entry = by_id ? shellFindFinishedJobById((size_t)target)
                                    : shellFindFinishedJobByPid((pid_t)target);
    if (entry) {
        if (!entry->reported) {
            entry->reported = true;
            entry->waits_since = 0;
        }
        entry->next_repeats = false;
        *pid_out = entry->last_pid;
        return entry->status;
    }
    if (by_id) {
        runtimeError(vm, "wait: %s: no such job", text);
    } else {
        runtimeError(vm, "wait: pid %ld is not a child of this shell", target);
    }
    return 127;
}

static void shellWaitUsage(void) {
    fprintf(stderr, "wait: usage: wait [-fn] [-p var] [id ...]\n");
    shellUpdateStatus(2);
}

/* wait -p name: the pid that gave the status, or unset if none did. */
static void shellWaitSetPidVar(const char *name, pid_t pid) {
    if (!name) {
        return;
    }
    if (pid > 0) {
        char text[32];
        snprintf(text, sizeof(text), "%ld", (long)pid);
        shellSetTrackedVariable(name, text, false);
    } else {
        shellUnsetTrackedVariable(name);
    }
}

Value vmBuiltinShellWait(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    bool next = false;
    const char *pid_var = NULL;
    pid_t pid = -1;
    int first = 0;
    /* -f makes no difference: the job's end is what is waited for, and a stopped one stays. */
    for (; first < arg_count; ++first) {
        const char *word = VALUE_TYPE(args[first]) == TYPE_STRING ? AS_STRING(args[first]) : NULL;
        if (!word || word[0] != '-' || !word[1]) {
            break;
        }
        if (strcmp(word, "--") == 0) {
            first++;
            break;
        }
        for (const char *opt = word + 1; *opt; ++opt) {
            if (*opt == 'n') {
                next = true;
            } else if (*opt == 'p') {
                const char *value = opt[1] ? opt + 1 : NULL;
                if (!value && first + 1 < arg_count && VALUE_TYPE(args[first + 1]) == TYPE_STRING) {
                    value = AS_STRING(args[++first]);
                }
                if (!value) {
                    fprintf(stderr, "wait: -p: option requires an argument\n");
                    shellWaitUsage();
                    return makeVoid();
                }
                pid_var = value;
                break;
            } else if (*opt != 'f') {
                fprintf(stderr, "wait: -%c: invalid option\n", *opt);
                shellWaitUsage();
                return makeVoid();
            }
        }
    }
    if (pid_var && !shellIsValidEnvName(pid_var)) {
        fprintf(stderr, "wait: `%s': not a valid identifier\n", pid_var);
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (pid_var && shellReadonlyContains(pid_var)) {
        fprintf(stderr, "wait: %s: readonly variable\n", pid_var);
        shellUpdateStatus(1);
        return makeVoid();
    }
    args += first;
    arg_count -= first;
    /* A number wait has used lasts for one more wait. */
    ShellRuntimeState *wait_ctx = shellGetContext();
    for (size_t i = 0; wait_ctx && i < wait_ctx->finished_job_count; ++i) {
        ShellFinishedJob *entry = &wait_ctx->finished_jobs[i];
        if (entry->reported && entry->has_id && ++entry->waits_since >= 2) {
            entry->has_id = false;
        }
    }
    if (next) {
        shellWaitForNextJob(args, arg_count, &pid);
        shellWaitSetPidVar(pid_var, pid);
        return makeVoid();
    }
    if (arg_count == 0) {
        shellWaitSetPidVar(pid_var, -1);
        /* Bash gives 0 here and forgets every job. A script keeps the one $! names, if that had
         * already ended, and kill as the command before this is too soon for bash to have heard. */
        ShellFinishedJob *ended = gShellRuntime.last_bg_pid > 0
                                      ? shellFindFinishedJobByPid(gShellRuntime.last_bg_pid)
                                      : NULL;
        bool keep_last = ended && gShellRuntime.kill_command + 1 != gShellRuntime.commands_run &&
                         (!shellRuntimeIsInteractive() || shellNumbersItsOwnJobs());
        ShellFinishedJob last;
        if (keep_last) {
            last = *ended;
        }
        while (shellJobVisibleCount() > 0) {
            /* Match Bash's tendency to treat the most recent job as "current". */
            size_t index = 0;
            bool found = false;
            for (size_t i = gShellJobCount; i > 0; --i) {
                ShellJob *job = &gShellJobs[i - 1];
                if (!job->disowned && !job->inherited) {
                    index = i - 1;
                    found = true;
                    break;
                }
            }
            if (!found) {
                break;
            }
            int signo = 0;
            shellWaitForJobAt(index, &signo);
            if (signo > 0) {
                shellWaitInterrupted(signo);
                return makeVoid();
            }
            shellRemoveJobAt(index);
            shellCollectJobs();
        }
        gShellRuntime.finished_job_count = 0;
        if (keep_last) {
            gShellRuntime.finished_jobs[gShellRuntime.finished_job_count++] = last;
        }
        shellUpdateStatus(0);
        return makeVoid();
    }
    int status = 0;
    for (int i = 0; i < arg_count; ++i) {
        int signo = 0;
        pid = -1;
        status = shellWaitForOperand(vm, args[i], &signo, &pid);
        if (signo > 0) {
            shellWaitInterrupted(signo);
            shellWaitSetPidVar(pid_var, -1);
            return makeVoid();
        }
    }
    shellWaitSetPidVar(pid_var, pid);
    shellUpdateStatus(status);
    return makeVoid();
}

Value vmBuiltinShellWaitForThread(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "WaitForThread: expected thread id");
        shellUpdateStatus(1);
        return makeVoid();
    }

    Value thread_value = args[0];
    Value converted = makeVoid();
    bool converted_valid = false;

    if (VALUE_TYPE(thread_value) == TYPE_STRING && AS_STRING(thread_value) && AS_STRING(thread_value)[0] != '\0') {
        const char *raw = AS_STRING(thread_value);
        const char *id_text = raw;
        const char *colon = strchr(raw, ':');
        if (colon && colon[1] != '\0') {
            id_text = colon + 1;
        }

        long long parsed = 0;
        if (!shellParseSignedLongLong(id_text, &parsed)) {
            runtimeError(vm, "WaitForThread: %s: invalid thread id", raw);
            shellUpdateStatus(1);
            return makeVoid();
        }
        converted = makeInt(parsed);
        thread_value = converted;
        converted_valid = true;
    }

    if (!(VALUE_TYPE(thread_value) == TYPE_THREAD || IS_INTLIKE(thread_value))) {
        runtimeError(vm, "WaitForThread: expected thread id");
        if (converted_valid) {
            freeValue(&converted);
        }
        shellUpdateStatus(1);
        return makeVoid();
    }

    long long thread_id = asI64(thread_value);
    if (thread_id <= 0 || thread_id > INT_MAX) {
        runtimeError(vm, "WaitForThread: %lld: invalid thread id", thread_id);
        if (converted_valid) {
            freeValue(&converted);
        }
        shellUpdateStatus(1);
        return makeVoid();
    }

    VM *thread_vm = vm;
    if (vm && vm->threadOwner) {
        thread_vm = vm->threadOwner;
    }

    // Every thread lives in the root VM's pool (thread_vm), whichever VM
    // spawned it; vmJoinThreadById waits as `vm`.
    bool joined = vmJoinThreadById(vm, (int)thread_id);
    if (!joined) {
        runtimeError(vm, "WaitForThread: %lld: invalid thread id", thread_id);
        if (converted_valid) {
            freeValue(&converted);
        }
        shellUpdateStatus(1);
        return makeVoid();
    }

    bool status_flag = true;
    bool consumed = false;
    if (vmThreadTakeResult(thread_vm, (int)thread_id, NULL, false, &status_flag, true)) {
        consumed = true;
    } else if (thread_vm && thread_vm != vm &&
               vmThreadTakeResult(vm, (int)thread_id, NULL, false, &status_flag, true)) {
        consumed = true;
        thread_vm = vm;
    }
    if (consumed) {
        shellUpdateStatus(status_flag ? 0 : 1);
    } else {
        shellUpdateStatus(0);
    }
    if (converted_valid) {
        freeValue(&converted);
    }
    return makeVoid();
}

Value vmBuiltinShellPsThreads(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;

    VM *root = vm && vm->threadOwner ? vm->threadOwner : vm;
    Value stats_json = vmBuiltinThreadStatsJson(root, 0, NULL);
    const char *text = (VALUE_TYPE(stats_json) == TYPE_STRING && AS_STRING(stats_json)) ? AS_STRING(stats_json) : "[]";
    printf("%s\n", text);
    freeValue(&stats_json);
    shellUpdateStatus(0);
    return makeVoid();
}

#if !defined(PSCAL_TARGET_IOS)
Value vmBuiltinShellPs(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;
    runtimeError(vm, "ps: synthetic process view is only available on iOS");
    shellUpdateStatus(1);
    return makeVoid();
}
#else
Value vmBuiltinShellPs(VM *vm, int arg_count, Value *args) {
    (void)vm;
    bool tree = false;
    for (int i = 0; i < arg_count; ++i) {
        if (VALUE_TYPE(args[i]) != TYPE_STRING || !AS_STRING(args[i])) {
            continue;
        }
        const char *opt = AS_STRING(args[i]);
        if (strcmp(opt, "--tree") == 0) {
            tree = true;
        } else if (strcmp(opt, "--flat") == 0) {
            tree = false;
        } else if (strcmp(opt, "--help") == 0 || strcmp(opt, "-h") == 0) {
            printf("lps [--tree|--flat]\n");
            printf("  Show PSCAL synthetic process list.\n");
            printf("  --flat (default) show flat list.\n");
            printf("  --tree render parent/child tree.\n");
            shellUpdateStatus(0);
            return makeVoid();
        } else {
            printf("lps: unknown option '%s'\n", opt);
            shellUpdateStatus(1);
            return makeVoid();
        }
    }
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[lps] begin tree=%d\n", (int)tree);
    }
    shellCollectJobs();
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[lps] jobs-collected count=%zu\n", gShellJobCount);
    }

    size_t snapshot_cap = vprocSnapshot(NULL, 0);
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[lps] snapshot-cap=%zu\n", snapshot_cap);
    }
    VProcSnapshot *snapshots = NULL;
    if (snapshot_cap > 0) {
        snapshots = (VProcSnapshot *)calloc(snapshot_cap, sizeof(VProcSnapshot));
    }
    size_t snapshot_count = snapshots ? vprocSnapshot(snapshots, snapshot_cap) : 0;
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[lps] snapshot-count=%zu\n", snapshot_count);
    }

    size_t seen_cap = snapshot_count + gShellJobCount * 2 + 4;
    pid_t *seen = (pid_t *)calloc(seen_cap ? seen_cap : 1, sizeof(pid_t));
    size_t seen_count = 0;

    if (tree) {
        printf("%-6s %-6s %-6s %-6s %-9s %-6s %-6s %s\n",
               "PID", "PPID", "PGID", "SID", "STATE", "UTIME", "STIME", "CMD");

        size_t row_cap = snapshot_count ? snapshot_count : 1;
        VProcTreeRow *rows = (VProcTreeRow *)calloc(row_cap, sizeof(VProcTreeRow));
        size_t row_count = rows ? vprocBuildTreeRows(snapshots, snapshot_count, rows, row_cap) : 0;
        if (rows && row_count > row_cap) {
            VProcTreeRow *grown = (VProcTreeRow *)realloc(rows, row_count * sizeof(VProcTreeRow));
            if (grown) {
                rows = grown;
                row_cap = row_count;
            }
            row_count = rows ? vprocBuildTreeRows(snapshots, snapshot_count, rows, row_cap) : 0;
        }

        if (rows) {
            for (size_t r = 0; r < row_count && r < row_cap; ++r) {
                const VProcTreeRow *row = &rows[r];
                if (!row || row->snapshot_index >= snapshot_count) {
                    continue;
                }
                const VProcSnapshot *snap = &snapshots[row->snapshot_index];
                if (!snap || snap->pid <= 0) {
                    continue;
                }
                ShellJob *job = shellFindJobByPid((pid_t)snap->pid);
                pid_t pgid = (job && job->pgid > 0) ? job->pgid : snap->pid;
                const char *cmd = shellPsCommandForEntry(job, snap);
                const char *state = shellPsStateForEntry(job, snap);
                shellPsPrintLineTree(snap, pgid, state, cmd, row->depth);
                if (seen && seen_count < seen_cap) {
                    seen[seen_count++] = (pid_t)snap->pid;
                }
            }
            free(rows);
        }
    } else {
        printf("%-6s %-6s %-9s %-6s %-6s %s\n", "PID", "PGID", "STATE", "UTIME", "STIME", "CMD");
        for (size_t i = 0; i < snapshot_count; ++i) {
            const VProcSnapshot *snap = &snapshots[i];
            if (!snap || snap->pid <= 0) {
                continue;
            }
            ShellJob *job = shellFindJobByPid((pid_t)snap->pid);
            pid_t pgid = (job && job->pgid > 0) ? job->pgid : snap->pid;
            const char *cmd = shellPsCommandForEntry(job, snap);
            const char *state = shellPsStateForEntry(job, snap);
            shellPsPrintLine((pid_t)snap->pid, pgid, state, snap->rusage_utime, snap->rusage_stime, cmd);
            if (seen && seen_count < seen_cap) {
                seen[seen_count++] = (pid_t)snap->pid;
            }
        }
    }

    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (!job->pids || job->pid_count == 0) {
            continue;
        }
        for (size_t j = 0; j < job->pid_count; ++j) {
            pid_t pid = job->pids[j];
            if (pid <= 0) {
                continue;
            }
            bool already_seen = false;
            for (size_t k = 0; k < seen_count; ++k) {
                if (seen[k] == pid) {
                    already_seen = true;
                    break;
                }
            }
            if (already_seen) {
                continue;
            }
            pid_t pgid = job->pgid > 0 ? job->pgid : pid;
            const char *state = job->disowned ? "disowned"
                                              : (job->stopped ? "stopped"
                                                              : (job->running ? "running" : "exited"));
            const char *cmd = job->command ? job->command : "";
            shellPsPrintLine(pid, pgid, state, 0, 0, cmd);
        }
    }

    free(snapshots);
    free(seen);
    shellUpdateStatus(0);
    return makeVoid();
}

#endif

Value vmBuiltinShellBuiltin(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || VALUE_TYPE(args[0]) != TYPE_STRING || !AS_STRING(args[0]) || AS_STRING(args[0])[0] == '\0') {
        runtimeError(vm, "builtin: expected VM builtin name");
        shellUpdateStatus(1);
        return makeVoid();
    }

    const char *name = AS_STRING(args[0]);
    if (shellRuntimeBuiltinDisabled(name)) {
        shellReportRecoverableError(vm, false, "builtin: %s: not a shell builtin", name);
        shellUpdateStatus(1);
        return makeVoid();
    }
    /* [ is test wanting its ], which it knows by the name it runs under. */
    bool is_bracket = strcmp(name, "[") == 0;
    VmBuiltinFn handler = getVmBuiltinHandler(is_bracket ? "test" : name);
    if (!handler) {
        runtimeError(vm, "builtin: unknown VM builtin '%s'", name);
        shellUpdateStatus(1);
        return makeVoid();
    }

    int call_argc = arg_count - 1;
    Value *call_args = NULL;
    if (call_argc > 0) {
        call_args = (Value *)calloc((size_t)call_argc, sizeof(Value));
        if (!call_args) {
            runtimeError(vm, "builtin: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        for (int i = 0; i < call_argc; ++i) {
            Value src = args[i + 1];
            if (VALUE_TYPE(src) == TYPE_STRING && AS_STRING(src)) {
                call_args[i] = shellConvertBuiltinArgument(AS_STRING(src));
            } else if (VALUE_TYPE(src) == TYPE_NIL) {
                call_args[i] = makeNil();
            } else {
                call_args[i] = makeString("");
            }
        }
    }

    unsigned long status_version = gShellStatusVersion;
    int previous_status = shellRuntimeLastStatus();
    const char *previous_name = shellRuntimeCurrentCommandName();
    if (is_bracket) {
        shellRuntimeSetCurrentCommandName("[");
    }
    Value result = handler(vm, call_argc, call_args);
    if (is_bracket) {
        shellRuntimeSetCurrentCommandName(previous_name);
    }

    if (call_args) {
        for (int i = 0; i < call_argc; ++i) {
            freeValue(&call_args[i]);
        }
        free(call_args);
    }

    /*
     * Shell builtins historically report success by default, with individual
     * helpers only overriding the exit status when they hit an error.  The
     * shell runtime used to leave gShellRuntime.last_status untouched before
     * dispatching the builtin which meant a prior non-zero status would leak
     * through and make every subsequent builtin appear to fail.  Scripts such
     * as the threaded Sierpinski demo rely on checking the builtin exit code,
     * so we normalise the status to success afterwards when the handler didn't
     * touch it.
     */
    bool status_untouched = (gShellStatusVersion == status_version);
    int status = shellRuntimeLastStatus();
    if (vm && vm->abort_requested && (status_untouched || status == previous_status)) {
        status = 1;
        shellUpdateStatus(1);
    } else if (status_untouched && status != 0) {
        status = 0;
        shellUpdateStatus(0);
    }

    if (status == 0 && VALUE_TYPE(result) != TYPE_VOID) {
        printValueToStream(result, stdout);
        fputc('\n', stdout);
    }
    shellCheckBuiltinOutput(vm, name);

    freeValue(&result);
    return makeVoid();
}

static void shellLoopFreeWordList(char **words, size_t count) {
    if (!words) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(words[i]);
    }
    free(words);
}

static bool shellLoopAppendWord(char ***list, size_t *count, size_t *capacity, const char *value) {
    if (!list || !count || !capacity) {
        return false;
    }
    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? 4 : (*capacity * 2);
        char **resized = (char **)realloc(*list, new_capacity * sizeof(char *));
        if (!resized) {
            return false;
        }
        *list = resized;
        *capacity = new_capacity;
    }
    char *copy = strdup(value ? value : "");
    if (!copy) {
        return false;
    }
    (*list)[*count] = copy;
    (*count)++;
    return true;
}

static bool shellLoopExpandEncodedWords(const char *const *encoded_words,
                                        size_t encoded_count,
                                        char ***out_words,
                                        size_t *out_count) {
    if (out_words) {
        *out_words = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    char **words = NULL;
    size_t count = 0;
    size_t capacity = 0;

    for (size_t i = 0; i < encoded_count; ++i) {
        const char *spec = encoded_words[i] ? encoded_words[i] : "";
        const char *text = spec;
        const char *word_meta = NULL;
        size_t word_meta_len = 0;
        uint8_t word_flags = 0;
        if (!shellDecodeWordSpec(spec, &text, &word_flags, &word_meta, &word_meta_len)) {
            text = spec;
            word_flags = 0;
            word_meta = NULL;
            word_meta_len = 0;
        }
        ShellStringArray fields = (ShellStringArray){0};
        bool ok = shellExpandWordFields(text, word_flags, word_meta, word_meta_len, true, &fields);
        for (size_t f = 0; f < fields.count && ok; ++f) {
            ok = shellLoopAppendWord(&words, &count, &capacity, fields.items[f]);
        }
        shellStringArrayFree(&fields);
        if (!ok) {
            shellLoopFreeWordList(words, count);
            return false;
        }
    }

    if (out_words) {
        *out_words = words;
    } else {
        shellLoopFreeWordList(words, count);
    }
    if (out_count) {
        *out_count = out_words ? count : 0;
    }
    return true;
}

static int shellLoopEvaluateTestWordsEncoded(char **encoded_words,
                                             size_t word_count,
                                             bool is_bracket) {
    char **words = NULL;
    size_t expanded = 0;
    int status = 1;
    if (encoded_words &&
        shellLoopExpandEncodedWords((const char *const *)encoded_words,
                                    word_count,
                                    &words,
                                    &expanded) &&
        expanded >= 1) {
        const char **argv = (const char **)((expanded > 1) ? &words[1] : NULL);
        int argc = (int)((expanded > 1) ? (expanded - 1) : 0);
        bool result = false;
        if (shellBuiltinTestFastEval(argv, argc, is_bracket, &result)) {
            status = result ? 0 : 1;
        } else {
            status = shellTestEvaluateSlowly(gShellCurrentVm, words[0], argv, argc - (is_bracket ? 1 : 0), is_bracket);
        }
    }
    shellLoopFreeWordList(words, expanded);
    return status;
}

static int shellLoopEvaluateArithmeticExpression(const char *expression) {
    int arith_status = 1;
    if (expression && shellBuiltinArithmeticFastEval(expression, &arith_status)) {
        return arith_status;
    }
    return arith_status;
}

static bool shellLoopEvaluateConditionSimple(ShellLoopFrame *frame, int *status_out) {
    int status = 1;
    shellNoteChunkLine(gShellCurrentVm);
    switch (frame->cond_kind) {
        case SHELL_LOOP_CONDITION_TEST:
        case SHELL_LOOP_CONDITION_BRACKET:
            status = shellLoopEvaluateTestWordsEncoded(frame->cond_words,
                                                       frame->cond_word_count,
                                                       frame->cond_kind == SHELL_LOOP_CONDITION_BRACKET);
            break;
        case SHELL_LOOP_CONDITION_COLON:
        case SHELL_LOOP_CONDITION_TRUE:
            status = 0;
            break;
        case SHELL_LOOP_CONDITION_FALSE:
            status = 1;
            break;
        case SHELL_LOOP_CONDITION_ARITH:
            status = shellLoopEvaluateArithmeticExpression(frame->cond_arith_expression);
            break;
        case SHELL_LOOP_CONDITION_NONE:
        default:
            status = 1;
            break;
    }
    if (status_out) {
        *status_out = status;
    }
    bool ready = (status == 0);
    if (frame->kind == SHELL_LOOP_KIND_UNTIL) {
        ready = !ready;
    }
    return ready;
}

static int shellLoopExecuteBodySimple(ShellLoopFrame *frame) {
    switch (frame->body_kind) {
        case SHELL_LOOP_BODY_KIND_NONE:
        case SHELL_LOOP_BODY_KIND_COLON:
        case SHELL_LOOP_BODY_KIND_TRUE:
            return 0;
        case SHELL_LOOP_BODY_KIND_FALSE:
            return 1;
        case SHELL_LOOP_BODY_KIND_TEST:
        case SHELL_LOOP_BODY_KIND_BRACKET:
            return shellLoopEvaluateTestWordsEncoded(frame->body_words,
                                                     frame->body_word_count,
                                                     frame->body_kind == SHELL_LOOP_BODY_KIND_BRACKET);
        case SHELL_LOOP_BODY_KIND_ARITH:
            return shellLoopEvaluateArithmeticExpression(frame->body_arith_expression);
        case SHELL_LOOP_BODY_KIND_TEST_ARITH:
        case SHELL_LOOP_BODY_KIND_BRACKET_ARITH:
            (void)shellLoopEvaluateTestWordsEncoded(frame->body_words,
                                                    frame->body_word_count,
                                                    frame->body_kind == SHELL_LOOP_BODY_KIND_BRACKET_ARITH);
            return shellLoopEvaluateArithmeticExpression(frame->body_arith_expression);
        default:
            return 0;
    }
}

/* A signal's trap acts on the chunk it broke into, and that is this one: a function's body or a
 * subshell's runs in a VM the last command to run around it did not name. */
static void shellLoopTakeSignals(VM *vm) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    shellRuntimeProcessPendingSignals();
    shellRestoreCurrentVm(previous_vm);
}

Value vmHostShellLoopCheckCondition(VM *vm) {
    shellLoopTakeSignals(vm);
    if (gShellExitRequested) {
        /* A trap just ended the shell: the condition would replace the status it ended with. */
        return makeBoolean(false);
    }
    ShellLoopFrame *frame = shellLoopTop();
    if (!frame) {
        shellRuntimeEnterCondition();
        shellRuntimeLeaveCondition();
        shellUpdateStatus(0);
        return makeBoolean(true);
    }
    if (frame->keep_status) {
        return makeBoolean(false);
    }

    shellRuntimeEnterCondition();

    int status = 1;
    bool ready = shellLoopEvaluateConditionSimple(frame, &status);

    shellUpdateStatus(status);

    shellRuntimeLeaveCondition();

    return makeBoolean(ready);
}

Value vmHostShellLoopExecuteBody(VM *vm) {
    shellLoopTakeSignals(vm);
    if (gShellExitRequested) {
        /* A trap just ended the shell: the body would replace the status it ended with. */
        return makeBoolean(false);
    }
    ShellLoopFrame *frame = shellLoopTop();
    if (!frame) {
        shellUpdateStatus(0);
        return makeBoolean(true);
    }

    int status = shellLoopExecuteBodySimple(frame);
    shellUpdateStatus(status);
    Value advance = vmHostShellLoopAdvance(vm);
    return advance;
}

Value vmHostShellLoopCheckBody(VM *vm) {
    shellLoopTakeSignals(vm);
    if (gShellExitRequested) {
        /* A trap just ended the shell: the condition would replace the status it ended with. */
        return makeBoolean(false);
    }
    ShellLoopFrame *frame = shellLoopTop();
    if (!frame) {
        shellRuntimeEnterCondition();
        shellRuntimeLeaveCondition();
        shellUpdateStatus(0);
        return makeBoolean(true);
    }
    if (frame->keep_status) {
        return makeBoolean(false);
    }

    shellRuntimeEnterCondition();
    int cond_status = 1;
    bool ready = shellLoopEvaluateConditionSimple(frame, &cond_status);
    shellUpdateStatus(cond_status);
    shellRuntimeLeaveCondition();

    if (!ready) {
        return makeBoolean(false);
    }

    int body_status = shellLoopExecuteBodySimple(frame);
    shellUpdateStatus(body_status);
    Value advance = vmHostShellLoopAdvance(vm);
    return advance;
}

Value vmHostShellLastStatus(VM *vm) {
    (void)vm;
    if (gShellRuntime.errexit_pending) {
        gShellRuntime.errexit_pending = false;
        gShellRuntime.errexit_defer_active = false;
        shellRuntimeConsumeExitRequested();
        if (gShellCurrentVm) {
            gShellCurrentVm->exit_requested = false;
            gShellCurrentVm->abort_requested = false;
            gShellCurrentVm->current_builtin_name = NULL;
        }
    }
    return makeInt(gShellRuntime.last_status);
}

Value vmHostShellLoopIsReady(VM *vm) {
    shellLoopTakeSignals(vm);
    ShellLoopFrame *frame = shellLoopTop();
    if ((gShellExitRequested || gShellRuntime.break_requested) && frame) {
        frame->break_pending = true;
        frame->continue_pending = false;
        frame->skip_body = true;
        gShellRuntime.break_requested = true;
        gShellRuntime.break_requested_levels = (int)gShellLoopStackSize;
        return makeBoolean(false);
    }
    bool ready = false;
    if (frame) {
        if (frame->break_pending) {
            ready = false;
        } else if (frame->kind == SHELL_LOOP_KIND_FOR) {
            ready = frame->for_active && !frame->skip_body;
        } else if (frame->kind == SHELL_LOOP_KIND_CFOR) {
            /* Skipping: the condition would overwrite the status of what caused it. */
            bool condition_ready = false;
            if (frame->skip_body) {
                ready = false;
            } else if (!shellLoopEvaluateCForCondition(frame, &condition_ready)) {
                frame->skip_body = true;
                frame->break_pending = true;
                shellLoopCForFailed(frame);
                ready = false;
            } else {
                ready = condition_ready;
            }
        } else {
            ready = !frame->skip_body;
        }
    }
    return makeBoolean(ready);
}

Value vmHostShellLoopAdvance(VM *vm) {
    shellLoopTakeSignals(vm);
    ShellLoopFrame *frame = shellLoopTop();
    if (!frame) {
        return makeBoolean(false);
    }
    /* Started while a body around it was being skipped: nothing in it runs, so `while :`
     * would go round for good, `break` included. */
    if (frame->keep_status) {
        frame->for_active = false;
        return makeBoolean(false);
    }
    frame->status = gShellRuntime.last_status;

    if (gShellExitRequested || gShellRuntime.break_requested) {
        frame->break_pending = true;
    }
    if (frame->break_pending) {
        frame->break_pending = false;
        frame->continue_pending = false;
        frame->skip_body = false;
        frame->for_active = false;
        /* This loop has taken its share. It stayed set for as long as any loop was open,
         * which ended the loops around this one and killed what they started. */
        if (gShellRuntime.break_requested && --gShellRuntime.break_requested_levels <= 0) {
            gShellRuntime.break_requested = false;
            gShellRuntime.break_requested_levels = 0;
        }
        shellMaybeResetPipeline();
        return makeBoolean(false);
    }

    if (frame->continue_pending) {
        frame->continue_pending = false;
    }

    bool should_continue = true;
    if (frame->kind == SHELL_LOOP_KIND_FOR) {
        if (frame->for_index < frame->for_count) {
            if (!shellAssignLoopVariable(frame->for_variable, frame->for_values[frame->for_index])) {
                runtimeError(vm, "shell loop: failed to assign '%s'", frame->for_variable ? frame->for_variable : "<var>");
                frame->status = 1;
                shellUpdateStatus(1);
                frame->skip_body = false;
                frame->for_active = false;
                shellMaybeResetPipeline();
                return makeBoolean(false);
            }
            frame->for_index++;
            frame->for_active = true;
            should_continue = true;
        } else {
            frame->for_active = false;
            should_continue = false;
        }
    } else if (frame->kind == SHELL_LOOP_KIND_CFOR) {
        if (!shellLoopExecuteCForUpdate(frame)) {
            shellLoopCForFailed(frame);
            frame->skip_body = false;
            frame->break_pending = true;
            shellMaybeResetPipeline();
            return makeBoolean(false);
        }
        bool condition_ready = false;
        if (!shellLoopEvaluateCForCondition(frame, &condition_ready)) {
            shellLoopCForFailed(frame);
            frame->skip_body = false;
            frame->break_pending = true;
            shellMaybeResetPipeline();
            return makeBoolean(false);
        }
        should_continue = condition_ready;
        shellLoopRestoreStatus(frame->status);
    }

    frame->skip_body = false;
    shellMaybeResetPipeline();
    return makeBoolean(should_continue);
}

Value vmHostShellPollJobs(VM *vm) {
    (void)vm;
    return makeInt(shellCollectJobs());
}

bool shellRuntimeConsumeExitRequested(void) {
    bool requested = gShellExitRequested;
    gShellExitRequested = false;
    return requested;
}

int shellRuntimeLastStatus(void) {
    return gShellRuntime.last_status;
}
