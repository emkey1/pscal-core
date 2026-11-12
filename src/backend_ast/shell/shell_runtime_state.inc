/* Auto-generated include: runtime state and globals. Included from shell.c. */
#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "shell/word_encoding.h"
#include "shell/quote_markers.h"
#include "shell/word_scan.h"
#include <dirent.h>
#include <pwd.h>
#include <poll.h>
#include <time.h>
#include "shell/function.h"
#include "shell/builtins.h"
#include "shell/runner.h"
#include "vm/vm.h"
#include "core/globals.h"
#include "pscal_paths.h"
#include "common/runtime_tty.h"
#include "runtime/vproc/vproc.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <regex.h>
#include <stdbool.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/times.h>
#include <termios.h>
#include <unistd.h>
#include <pwd.h>
#include <pthread.h>
#include <time.h>
#include <wctype.h>

#if defined(PSCAL_TARGET_IOS)
typedef struct {
    FILE *fp;
    int host_fd;
    int std_fd;
    dev_t host_dev;
    ino_t host_ino;
    bool host_identity_valid;
    bool write_out_at_thread_end;
} ShellVprocStream;

typedef struct {
    int std_fd;
    bool can_read;
    bool can_write;
} ShellVprocShimCookie;

typedef struct ShellShimCookieNode {
    void *cookie;
    struct ShellShimCookieNode *next;
} ShellShimCookieNode;

/* A thread's streams, closed when it ends. See VmVprocThreadStreams in builtin.c: a stage
 * closes its own as it finishes, but not one ended by a kill or exit(), and no other thread did. */
typedef struct {
    ShellVprocStream out;
    ShellVprocStream err;
    ShellVprocStream in;
} ShellVprocThreadStreams;

#define SHELL_VPROC_THREAD_STREAMS_INIT { \
    .out = { NULL, -1, STDOUT_FILENO }, \
    .err = { NULL, -1, STDERR_FILENO }, \
    .in = { NULL, -1, STDIN_FILENO } }

static __thread ShellVprocThreadStreams *gShellVprocThreadStreams = NULL;
/* Out of memory: the streams work, and stay open. */
static __thread ShellVprocThreadStreams gShellVprocThreadStreamsNoHeap = SHELL_VPROC_THREAD_STREAMS_INIT;
static pthread_key_t gShellVprocThreadStreamsKey;
static pthread_once_t gShellVprocThreadStreamsOnce = PTHREAD_ONCE_INIT;

static void shellVprocThreadStreamEnd(ShellVprocStream *stream) {
    if (stream->fp) {
        vprocStreamEndWithThread(stream->fp, stream->write_out_at_thread_end);
    }
}

static void shellVprocThreadStreamsEnd(void *arg) {
    ShellVprocThreadStreams *streams = (ShellVprocThreadStreams *)arg;
    shellVprocThreadStreamEnd(&streams->out);
    shellVprocThreadStreamEnd(&streams->err);
    shellVprocThreadStreamEnd(&streams->in);
    free(streams);
    /* A destructor after this one that prints gets a new set, and the key runs again. */
    gShellVprocThreadStreams = NULL;
}

static void shellVprocThreadStreamsMakeKey(void) {
    (void)pthread_key_create(&gShellVprocThreadStreamsKey, shellVprocThreadStreamsEnd);
}

static ShellVprocThreadStreams *shellVprocThreadStreams(void) {
    if (gShellVprocThreadStreams) {
        return gShellVprocThreadStreams;
    }
    ShellVprocThreadStreams *streams = (ShellVprocThreadStreams *)malloc(sizeof(ShellVprocThreadStreams));
    if (!streams) {
        return &gShellVprocThreadStreamsNoHeap;
    }
    *streams = (ShellVprocThreadStreams)SHELL_VPROC_THREAD_STREAMS_INIT;
    (void)pthread_once(&gShellVprocThreadStreamsOnce, shellVprocThreadStreamsMakeKey);
    if (pthread_setspecific(gShellVprocThreadStreamsKey, streams) != 0) {
        free(streams);
        return &gShellVprocThreadStreamsNoHeap;
    }
    gShellVprocThreadStreams = streams;
    return streams;
}

#define gShellVprocStdout (shellVprocThreadStreams()->out)
#define gShellVprocStderr (shellVprocThreadStreams()->err)
#define gShellVprocStdin (shellVprocThreadStreams()->in)

static pthread_mutex_t gShellShimCookieLock = PTHREAD_MUTEX_INITIALIZER;
static ShellShimCookieNode *gShellShimCookieHead = NULL;

static void shellShimCookieTrackAdd(void *cookie) {
    if (!cookie) {
        return;
    }
    ShellShimCookieNode *node = (ShellShimCookieNode *)calloc(1, sizeof(ShellShimCookieNode));
    if (!node) {
        return;
    }
    node->cookie = cookie;
    pthread_mutex_lock(&gShellShimCookieLock);
    node->next = gShellShimCookieHead;
    gShellShimCookieHead = node;
    pthread_mutex_unlock(&gShellShimCookieLock);
}

static bool shellShimCookieTrackRemove(void *cookie) {
    if (!cookie) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&gShellShimCookieLock);
    ShellShimCookieNode *prev = NULL;
    ShellShimCookieNode *cur = gShellShimCookieHead;
    while (cur) {
        if (cur->cookie == cookie) {
            if (prev) {
                prev->next = cur->next;
            } else {
                gShellShimCookieHead = cur->next;
            }
            free(cur);
            found = true;
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&gShellShimCookieLock);
    return found;
}

static FILE *shellVprocStreamFallback(int std_fd) {
    if (std_fd == STDIN_FILENO) {
        return stdin;
    }
    if (std_fd == STDOUT_FILENO) {
        return stdout;
    }
    return stderr;
}

static int shellVprocShimRead(void *cookie, char *buf, int len) {
    ShellVprocShimCookie *ctx = (ShellVprocShimCookie *)cookie;
    if (!ctx || !ctx->can_read || !buf || len <= 0) {
        errno = EBADF;
        return -1;
    }
    ssize_t res = vprocReadShim(ctx->std_fd, buf, (size_t)len);
    if (res < 0) {
        return -1;
    }
    if (res > INT_MAX) {
        res = INT_MAX;
    }
    return (int)res;
}

static int shellVprocShimWrite(void *cookie, const char *buf, int len) {
    ShellVprocShimCookie *ctx = (ShellVprocShimCookie *)cookie;
    if (!ctx || !ctx->can_write || !buf || len <= 0) {
        errno = EBADF;
        return -1;
    }
    ssize_t res = vprocWriteShim(ctx->std_fd, buf, (size_t)len);
    if (res < 0) {
        return -1;
    }
    if (res > INT_MAX) {
        res = INT_MAX;
    }
    return (int)res;
}

static int shellVprocShimClose(void *cookie) {
    if (!shellShimCookieTrackRemove(cookie)) {
        return 0;
    }
    free(cookie);
    return 0;
}

static FILE *shellVprocStreamOpenShim(int std_fd,
                                      ShellVprocStream *cache,
                                      const char *mode,
                                      int buf_mode) {
    if (!cache || !mode) {
        return shellVprocStreamFallback(std_fd);
    }

    if (cache->fp && cache->host_fd < 0) {
        return cache->fp;
    }

    if (cache->fp) {
        fflush(cache->fp);
        fclose(cache->fp);
        cache->fp = NULL;
    }
    cache->host_fd = -1;
    cache->host_identity_valid = false;
    cache->write_out_at_thread_end = false;

    ShellVprocShimCookie *cookie = (ShellVprocShimCookie *)calloc(1, sizeof(ShellVprocShimCookie));
    if (!cookie) {
        return shellVprocStreamFallback(std_fd);
    }
    cookie->std_fd = std_fd;
    cookie->can_read = strchr(mode, 'r') != NULL;
    cookie->can_write = (strchr(mode, 'w') != NULL) || (strchr(mode, 'a') != NULL);
    shellShimCookieTrackAdd(cookie);

    FILE *fp = funopen(cookie,
                       cookie->can_read ? shellVprocShimRead : NULL,
                       cookie->can_write ? shellVprocShimWrite : NULL,
                       NULL,
                       shellVprocShimClose);
    if (!fp) {
        (void)shellShimCookieTrackRemove(cookie);
        free(cookie);
        return shellVprocStreamFallback(std_fd);
    }
    if (buf_mode >= 0) {
        setvbuf(fp, NULL, buf_mode, 0);
    }
    vprocStreamSkipFlushAll(fp);
    cache->fp = fp;
    cache->host_fd = -1;
    cache->host_identity_valid = false;
    return fp;
}

static FILE *shellVprocStreamOpen(int std_fd,
                                  ShellVprocStream *cache,
                                  const char *mode,
                                  int buf_mode) {
    if (!cache || !mode) {
        return shellVprocStreamFallback(std_fd);
    }

    int host_fd = std_fd;
    bool use_host_stream = true;
    VProc *vp = vprocCurrent();
    if (vp) {
        int translated = vprocTranslateFd(vp, std_fd);
        if (translated >= 0) {
            host_fd = translated;
        } else {
            use_host_stream = false;
        }
    }

    if (!vp) {
        use_host_stream = true;
    }

    if (!use_host_stream) {
        return shellVprocStreamOpenShim(std_fd, cache, mode, buf_mode);
    }

    if (cache->fp && cache->host_fd == host_fd) {
        if (!cache->host_identity_valid) {
            return cache->fp;
        }
        struct stat current_st;
        /* By slot: fstat is the shim, and a host number is not a slot. It failed, the identity
         * was never set, and when a slot came back on the number the stream was made for, the
         * stream carried on to what that number used to be. Beside another job a host number is
         * soon handed out again, and a job's `x=$(echo hi)` wrote to the real stdout. */
        if (fstat(std_fd, &current_st) == 0 &&
            current_st.st_dev == cache->host_dev &&
            current_st.st_ino == cache->host_ino) {
            return cache->fp;
        }
    }

    if (cache->fp) {
        fflush(cache->fp);
        fclose(cache->fp);
        cache->fp = NULL;
    }
    cache->host_fd = -1;
    cache->host_identity_valid = false;
    cache->write_out_at_thread_end = false;

    int dup_fd = -1;
#ifdef F_DUPFD_CLOEXEC
    dup_fd = fcntl(host_fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0 && errno == EINVAL) {
        dup_fd = -1;
    }
#endif
    if (dup_fd < 0) {
        /* Not dup, whose shim takes a slot, or vprocHostDup: the FILE owns this one. */
        dup_fd = fcntl(host_fd, F_DUPFD, 0);
    }
    if (dup_fd < 0) {
        return shellVprocStreamFallback(std_fd);
    }

    FILE *fp = fdopen(dup_fd, mode);
    if (!fp) {
        vprocHostClose(dup_fd);
        return shellVprocStreamFallback(std_fd);
    }
    if (buf_mode >= 0) {
        setvbuf(fp, NULL, buf_mode, 0);
    }
    vprocStreamSkipFlushAll(fp);
    cache->fp = fp;
    cache->host_fd = host_fd;
    struct stat host_st;
    if (fstat(std_fd, &host_st) == 0) {
        cache->host_dev = host_st.st_dev;
        cache->host_ino = host_st.st_ino;
        cache->host_identity_valid = true;
        cache->write_out_at_thread_end = S_ISREG(host_st.st_mode) && strchr(mode, 'r') == NULL;
    }
    return fp;
}

static FILE *shellVprocStdout(void) {
    int buf_mode = pscalRuntimeStdoutIsInteractive() ? _IOLBF : _IOFBF;
    return shellVprocStreamOpen(STDOUT_FILENO, &gShellVprocStdout, "w", buf_mode);
}

static FILE *shellVprocStderr(void) {
    return shellVprocStreamOpen(STDERR_FILENO, &gShellVprocStderr, "w", _IONBF);
}

static FILE *shellVprocStdin(void) {
    return shellVprocStreamOpen(STDIN_FILENO, &gShellVprocStdin, "r", _IOFBF);
}

static int shellVprocPuts(const char *text) {
    FILE *out = shellVprocStdout();
    const char *value = text ? text : "(null)";
    if (fputs(value, out) == EOF) {
        return EOF;
    }
    return fputc('\n', out);
}

static void shellVprocPerror(const char *label) {
    int saved_errno = errno;
    FILE *errfp = shellVprocStderr();
    if (label && *label) {
        fprintf(errfp, "%s: %s\n", label, strerror(saved_errno));
    } else {
        fprintf(errfp, "%s\n", strerror(saved_errno));
    }
    errno = saved_errno;
}

bool shellRuntimeIsSharedFileStream(FILE *stream) {
    if (!stream) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    if (stream == stdin || stream == stdout || stream == stderr) {
        return true;
    }
    if (stream == gShellVprocStdin.fp ||
        stream == gShellVprocStdout.fp ||
        stream == gShellVprocStderr.fp) {
        return true;
    }
    return false;
#else
    return stream == stdin || stream == stdout || stream == stderr;
#endif
}

static void shellFlushVprocStreams(void) {
    if (gShellVprocStdout.fp) {
        fflush(gShellVprocStdout.fp);
    }
    if (gShellVprocStderr.fp) {
        fflush(gShellVprocStderr.fp);
    }
}

static void shellCloseVprocStream(ShellVprocStream *stream) {
    if (!stream || !stream->fp) {
        return;
    }
    fclose(stream->fp);
    stream->fp = NULL;
    stream->host_fd = -1;
}

static void shellCloseVprocStreams(void) {
    shellCloseVprocStream(&gShellVprocStdout);
    shellCloseVprocStream(&gShellVprocStderr);
    shellCloseVprocStream(&gShellVprocStdin);
}

static void shellInvalidateVprocStreamForFd(int fd) {
    if (fd == STDOUT_FILENO) {
        shellCloseVprocStream(&gShellVprocStdout);
    } else if (fd == STDERR_FILENO) {
        shellCloseVprocStream(&gShellVprocStderr);
    } else if (fd == STDIN_FILENO) {
        shellCloseVprocStream(&gShellVprocStdin);
    }
}
#else
static void shellFlushVprocStreams(void) {}
static void __attribute__((unused)) shellCloseVprocStreams(void) {}
static void __attribute__((unused)) shellInvalidateVprocStreamForFd(int fd) { (void)fd; }
#endif

static void shellFlushStandardStreams(void) {
    shellFlushVprocStreams();
#if defined(PSCAL_TARGET_IOS)
    /* When stdio is virtualised through vproc (e.g. in-process pipeline
     * stages), flushing libc stdout/stderr can hit stale host pipe endpoints
     * and raise SIGPIPE. The vproc stream flush above is sufficient. */
    VProc *vp = vprocCurrent();
    if (vp) {
        int out_host = vprocTranslateFd(vp, STDOUT_FILENO);
        int err_host = vprocTranslateFd(vp, STDERR_FILENO);
        if (out_host < 0 || err_host < 0) {
            return;
        }
    }
#endif
    fflush(stdout);
    fflush(stderr);
}

static void shellResetStandardStream(int fd);

#if defined(__GLIBC__) || defined(__ANDROID__)
#define SHELL_HAVE_SECURE_GETENV 1
extern char *secure_getenv(const char *name);
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#define SHELL_HAVE_ISSETUGID 1
#endif

/* Every tracked variable asks, so every command, and it takes up to 5 system calls, so the
 * answer is kept. A process's ids change only by a call it makes itself: fork keeps them and
 * exec starts over. Here that means applets: su, sudo, login and chroot set ids, exec, and come
 * back if that fails, so the applet dispatcher says to ask again. The two low bits hold the
 * answer and the rest count those calls, so a thread that was asking through one can't keep
 * what it learned before it. An iOS session keeps its own, as its ids will be its vproc's. */
enum {
    SHELL_PRIVILEGED_KNOWN = 1,
    SHELL_PRIVILEGED_YES = 2
};
static PSCAL_THREAD_LOCAL _Atomic unsigned gShellPrivileged = 0;

static bool shellProcessIsPrivileged(void) {
    unsigned state = atomic_load_explicit(&gShellPrivileged, memory_order_relaxed);
    if (state & SHELL_PRIVILEGED_KNOWN) {
        return (state & SHELL_PRIVILEGED_YES) != 0;
    }
    bool privileged = false;
#if defined(SHELL_HAVE_ISSETUGID)
    privileged = issetugid() != 0;
#endif
    if (!privileged) {
        privileged = geteuid() != getuid() || getegid() != getgid();
    }
    unsigned answer = state | SHELL_PRIVILEGED_KNOWN | (privileged ? SHELL_PRIVILEGED_YES : 0);
    atomic_compare_exchange_strong_explicit(&gShellPrivileged, &state, answer,
                                            memory_order_relaxed, memory_order_relaxed);
    return privileged;
}

void shellRuntimeIdsMayHaveChanged(void) {
    unsigned state = atomic_load_explicit(&gShellPrivileged, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&gShellPrivileged, &state, (state | 3u) + 1u,
                                                  memory_order_relaxed, memory_order_relaxed)) {
    }
}

static const char *shellSafeGetenv(const char *name) {
    if (!name) {
        return NULL;
    }
#if defined(SHELL_HAVE_SECURE_GETENV)
    if (shellProcessIsPrivileged()) {
        return NULL;
    }
    const char *value = secure_getenv(name);
    if (!value) {
        return NULL;
    }
    return value;
#else
    if (shellProcessIsPrivileged()) {
        return NULL;
    }
    return getenv(name);
#endif
}

extern char **environ;

/* The debug switches are asked about on every command, and a name that isn't set costs a scan of
 * the whole environment. Each is read once, and again after the shell sets or unsets a PSCALI_
 * name, which is how one gets turned on at a prompt. The low bit is the answer, the rest which
 * generation of names it came from. */
static atomic_uint gShellDebugSwitchGeneration = 1;
static PSCAL_THREAD_LOCAL atomic_uint gShellRedirDebugSwitch = 0;
static PSCAL_THREAD_LOCAL atomic_uint gShellPipeDebugSwitch = 0;
static PSCAL_THREAD_LOCAL atomic_uint gShellToolDebugSwitch = 0;
static PSCAL_THREAD_LOCAL atomic_uint gShellKillDebugSwitch = 0;
#define SHELL_REDIR_DEBUG() shellDebugSwitch(&gShellRedirDebugSwitch, "PSCALI_REDIR_DEBUG")
#define SHELL_PIPE_DEBUG() shellDebugSwitch(&gShellPipeDebugSwitch, "PSCALI_PIPE_DEBUG")
#define SHELL_TOOL_DEBUG() shellDebugSwitch(&gShellToolDebugSwitch, "PSCALI_TOOL_DEBUG")
#define SHELL_KILL_DEBUG() shellDebugSwitch(&gShellKillDebugSwitch, "PSCALI_KILL_DEBUG")

static void shellDebugSwitchesNoteName(const char *name) {
    if (name[0] == 'P' && strncmp(name, "PSCALI_", 7) == 0) {
        atomic_fetch_add_explicit(&gShellDebugSwitchGeneration, 1, memory_order_relaxed);
    }
}

static bool shellDebugSwitch(atomic_uint *slot, const char *name) {
    unsigned generation = atomic_load_explicit(&gShellDebugSwitchGeneration, memory_order_relaxed);
    unsigned state = atomic_load_explicit(slot, memory_order_relaxed);
    if ((state >> 1) == generation) {
        return (state & 1u) != 0;
    }
    bool on = getenv(name) != NULL;
    atomic_store_explicit(slot, (generation << 1) | (on ? 1u : 0u), memory_order_relaxed);
    return on;
}

#define SHELL_ARRAY_ELEMENT_SEP '\x1d'

typedef enum {
    SHELL_ARRAY_KIND_INDEXED,
    SHELL_ARRAY_KIND_ASSOCIATIVE
} ShellArrayKind;

typedef struct {
    char *name;
    char **values;
    char **keys;
    size_t count;
    ShellArrayKind kind;
    /* Keyed only because its indices have gaps: a=([5]=x), unset 'a[1]'. Subscripts are
     * still arithmetic. */
    bool sparse;
} ShellArrayVariable;

typedef struct {
    char *name;
} ShellReadonlyEntry;

typedef struct ShellAlias {
    char *name;
    char *value;
} ShellAlias;

typedef struct {
    char *name;
    char *path;
    unsigned long hits;
} ShellHashEntry;

typedef struct {
    char **entries;
    size_t count;
    size_t capacity;
    size_t dropped;       /* entries HISTSIZE took off the front: `history` numbers from dropped + 1 */
    size_t session_lines; /* added since the last `history -a` or -w, which -a appends (bash's count) */
    size_t lines_in_file; /* lines of the history file this shell has read or written: -n reads past them */
    bool newest_is_prompt_line; /* the newest entry is the line being run, which -s and -p drop */
} ShellHistory;

typedef struct ShellTrapEntry {
    ShellTrapAction action;
    char *command;
    /* Handed to a subshell or a substitution without set -E or -T: `trap` lists it, nothing runs it. */
    bool dormant;
} ShellTrapEntry;

static PSCAL_THREAD_LOCAL ShellTrapEntry gShellErrTrap;
static PSCAL_THREAD_LOCAL ShellTrapEntry gShellDebugTrap;
static PSCAL_THREAD_LOCAL ShellTrapEntry gShellReturnTrap;

typedef struct ShellJob {
    pid_t pgid;
    pid_t *pids;
    size_t pid_count;
    /* What $! gave. pids[] marks what has been reaped, this stays. */
    pid_t last_pid;
    size_t id;
    bool running;
    bool stopped;
    bool disowned;
    /* A substitution's copy of its shell's job. It is listed and can be killed, as under bash,
     * but it is the shell's to wait for: reaping it here left the shell's `wait` hanging. */
    bool inherited;
    int last_status;
    /* What ended it, for `jobs`: last_status cannot tell that from `exit 143`. */
    int term_signal;
    char *command;
    /* As written, from a table that keeps it: a finished job is copied as bytes. NULL for a job
     * no `&` started, which goes by command. */
    const char *text;
    pid_t first_pid;
    /* `jobs` has shown it, for jobs -n. */
    bool notified;
} ShellJob;

/* What `wait` can still find of a job that has left the table. */
typedef struct {
    pid_t pgid;
    pid_t last_pid;
    size_t id;
    int status;
    /* %n still means this job, and no new job takes the number. */
    bool has_id;
    /* wait -n passes it by. */
    bool reported;
    /* wait -n gave it for its pid, and gives it again if asked for that pid. */
    bool next_repeats;
    /* `kill` ended it just now, and the next prompt is too soon to report that. */
    bool killed_now;
    int term_signal;
    const char *text;
    pid_t first_pid;
    /* `wait` calls since one first used it: the number lasts for one more. */
    unsigned char waits_since;
} ShellFinishedJob;

#define SHELL_FINISHED_JOB_MAX 1024

typedef struct {
    pid_t fg_job_id;
    pid_t fg_pgid;
    bool saved_stdio_valid;
    int saved_stdio[3];
    bool saved_termios_valid;
    struct termios saved_termios;
} ShellJobControlContext;

typedef struct ShellLocalScopeFrame ShellLocalScopeFrame;
typedef struct ShellLoopFrame ShellLoopFrame;

#if defined(PSCAL_TARGET_IOS)
typedef struct {
    int spool_fd;
    size_t spool_size;
    bool spool_owned;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    bool stdin_owned;
    bool stdout_owned;
    bool stderr_owned;
} ShellPipelineSession;
#endif

typedef struct {
    bool active;
    size_t stage_count;
    bool negated;
    bool *merge_stderr;
    pid_t *pids;
    int (*pipes)[2];
    size_t launched;
    bool background;
    /* `a | b &`, known from its start. background comes only with the last stage. */
    bool async;
    int last_status;
    pid_t pgid;
    bool interrupt_pending_at_start;
#if defined(PSCAL_TARGET_IOS)
    bool in_process;
    bool in_process_worker;
    VProcSessionStdio *session_stdio;
    ShellPipelineSession session;
    pthread_t *threads;
    bool *thread_started;
    int *stage_status;
    /* The interrupt key ended a stage's input: the whole job ends as if the key's SIGINT
     * had reached every stage, as it does on Linux. */
    bool key_interrupted;
    struct pscal_fd **pscal_pipe_read;
    struct pscal_fd **pscal_pipe_write;
    struct pscal_fd **pscal_pipe_write_hold;
    int window_cols;
    int window_rows;
    bool sigpipe_ignored; /* by the shell's trap, which a stage's own thread can't read */
#endif
} ShellPipelineContext;

typedef struct {
    bool has_job_control;
    bool has_tcsetpgrp;
    bool has_pgid;
    bool has_stop_signals;
    bool has_foreground_control;
    bool has_killpg;
} ShellJobControlCaps;

typedef struct ShellRuntimeState {
    int last_status;
    ShellPipelineContext pipeline;
    pid_t shell_pgid;
    pid_t last_bg_pid;
    int tty_fd;
    bool job_control_enabled;
    /* Commands so far, and which of them last sent a signal: the very next one is too soon to
     * have heard what came of it. */
    size_t commands_run;
    size_t kill_command;
    /* set -m: fg and bg work outside a prompt. Nothing else follows from it yet. */
    bool monitor_enabled;
    /* A subshell or a stage has none, whatever it inherited, until it says set -m itself. */
    bool job_control_gone;
    bool job_control_initialized;
    ShellJobControlCaps job_control_caps;
    bool errexit_enabled;
    bool pipefail_enabled;
    bool nounset_enabled;
    bool noglob_enabled;
    bool noclobber_enabled;
    bool xtrace_enabled;
    bool verbose_enabled;
    bool braceexpand_disabled;
    bool allexport_enabled;
    bool errexit_pending;
    bool errexit_defer_active;
    bool trap_enabled;
    bool posix_mode;
    bool errtrace_enabled; /* set -E: a function sees the ERR trap */
    bool functrace_enabled; /* set -T: a function sees the RETURN trap */
    bool local_scope_active;
    bool break_requested;
    bool continue_requested;
    bool pipeline_subshell_active;
    bool left_stages_running;
    bool in_subshell;
    bool started_as_command; /* a shell inside its caller: the session's jobs are not its own */
    int monitor_request; /* `set -m` is 1, `set +m` -1, otherwise 0: see shellMonitorOn */
    pid_t subshell_parent_pid; /* $$ in a subshell is its shell's */
    bool body_in_loop; /* iOS: a body started inside a loop, which `break` there has only itself to leave */
    unsigned int random_seed; /* $RANDOM: a job goes on from here, and the shell does not see that */
    bool random_seeded;
    int break_requested_levels;
    int continue_requested_levels;
    char **dir_stack;
    size_t dir_stack_count;
    size_t dir_stack_capacity;
    bool dir_stack_initialised;
    ShellHashEntry *hash_entries;
    size_t hash_count;
    size_t hash_capacity;
    char **disabled_builtins;
    size_t disabled_builtin_count;
    size_t disabled_builtin_capacity;
    int getopts_index;
    int getopts_offset;
    unsigned long scalar_cache_hits;
    unsigned long scalar_cache_misses;
    unsigned long scalar_cache_stores;
    size_t scalar_cache_generation;
    size_t scalar_capture_peak_depth;
    unsigned long command_substitutions_run;
    ShellHistory history;
    char *history_path;
    size_t history_size_limit;
    size_t history_file_size_limit;
    bool history_persist_enabled;
    bool history_append_mode;
    bool history_loaded_from_file;
    bool history_loading;
    char *arg0;
    ShellTrapEntry signal_traps[NSIG];
    bool signal_ignored_on_entry[NSIG]; /* ignored when this shell started: no trap can change it */
    ShellTrapEntry exit_trap;
    bool trap_executing;
    int script_depth;
    bool ending_on_signal; /* what this shell is dying of, whatever its EXIT trap says */
    int return_scope_depth; /* functions and sourced files around what runs: where `return` means something */
    int text_depth; /* eval, source and a trap's text around what runs; a substitution counts as one */
    int condition_depth;
    size_t job_next_id;
    ShellJob *jobs;
    size_t job_count;
    /* Oldest first. When it is full the oldest goes, so a long script cannot grow it. */
    ShellFinishedJob finished_jobs[SHELL_FINISHED_JOB_MAX];
    size_t finished_job_count;
    ShellJobControlContext job_control_ctx;
    ShellArrayVariable *array_vars;
    size_t array_var_count;
    size_t array_var_capacity;
    int associative_array_support;
    int bind_interactive_status;
    ShellReadonlyEntry *readonly_vars;
    size_t readonly_var_count;
    size_t readonly_var_capacity;
    char *readonly_error_name;
    /* Per-context locals/loops/history stacks */
    ShellLocalScopeFrame *local_scope_stack;
    size_t local_scope_depth;
    size_t local_scope_capacity;
    ShellLoopFrame *loop_stack;
    size_t loop_stack_size;
    size_t loop_stack_capacity;
    /* The loops around a command substitution. They are the shell's, and it cannot leave them. */
    size_t loops_around;
    /* What the next `&` was written as: __shell_job_text comes just before it. */
    const char *pending_job_text;
    /* A subshell's own definitions: on iOS no fork gives it a copy of these. */
    bool has_private_tables;
    struct ShellFunctionEntry *private_functions;
    size_t private_function_count;
    ShellAlias *private_aliases;
    size_t private_alias_count;
    struct ShellOptionEntry *private_options;
} ShellRuntimeState;

static ShellRuntimeState *shellGetContext(void);
static void shellSetContext(ShellRuntimeState *ctx);

#define SC() (shellGetContext())

#define gShellLocalScopeStack (SC()->local_scope_stack)
#define gShellLocalScopeDepth (SC()->local_scope_depth)
#define gShellLocalScopeCapacity (SC()->local_scope_capacity)
#define gShellArrayVars (SC()->array_vars)
#define gShellArrayVarCount (SC()->array_var_count)
#define gShellArrayVarCapacity (SC()->array_var_capacity)
#define gShellBindInteractiveStatus (SC()->bind_interactive_status)
#define gShellReadonlyVars (SC()->readonly_vars)
#define gShellReadonlyVarCount (SC()->readonly_var_count)
#define gShellReadonlyVarCapacity (SC()->readonly_var_capacity)
#define gShellReadonlyErrorName (SC()->readonly_error_name)
#define gShellHistory (SC()->history)
#define gShellArg0 (SC()->arg0)
#define gShellSignalTraps (SC()->signal_traps)
#define gShellSignalIgnoredOnEntry (SC()->signal_ignored_on_entry)
#define gShellExitTrap (SC()->exit_trap)
#define gShellTrapExecuting (SC()->trap_executing)
#define gShellScriptDepth (SC()->script_depth)
#define gShellEndingOnSignal (SC()->ending_on_signal)
#define gShellReturnScopeDepth (SC()->return_scope_depth)
#define gShellTextDepth (SC()->text_depth)
#define gShellConditionDepth (SC()->condition_depth)
#define gShellJobs (SC()->jobs)
#define gShellJobCount (SC()->job_count)
#define gShellJobControlCaps (SC()->job_control_caps)
#define gShellJobControlCtx (SC()->job_control_ctx)

static const char *kShellCommandDefaultPath =
    "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

typedef enum {
    SHELL_COMMAND_RESULT_ALIAS,
    SHELL_COMMAND_RESULT_KEYWORD,
    SHELL_COMMAND_RESULT_FUNCTION,
    SHELL_COMMAND_RESULT_BUILTIN,
    SHELL_COMMAND_RESULT_APPLET,
    SHELL_COMMAND_RESULT_FILE
} ShellCommandResultKind;

typedef struct {
    ShellCommandResultKind kind;
    char *detail;
} ShellCommandResult;

/* A function being called or a file being sourced, the way bash lists them. Together they make
 * FUNCNAME, BASH_SOURCE and BASH_LINENO, and what caller reads. */
typedef struct {
    char *function; /* "source" for a sourced file */
    char *source;   /* the file its text came from */
    int line;       /* where in its caller's text it was called */
    bool is_function;
} ShellCallFrame;

/* On iOS a subshell or pipeline stage runs its own interpreter on a thread. */
static PSCAL_THREAD_LOCAL ShellCallFrame *gShellCallStack = NULL;
static PSCAL_THREAD_LOCAL size_t gShellCallStackCount = 0;
static PSCAL_THREAD_LOCAL size_t gShellCallStackCapacity = 0;
/* The script this shell runs: under every frame, as "main". A -c string or a prompt has none. */
static PSCAL_THREAD_LOCAL char *gShellScriptName = NULL;

typedef struct ShellFunctionEntry {
    char *name;
    char *parameter_metadata;
    ShellCompiledFunction *compiled;
    bool traced; /* declare -ft: it sees the caller's RETURN trap */
    char *source; /* the file it was defined in: its entry in BASH_SOURCE */
} ShellFunctionEntry;

static ShellFunctionEntry *gShellFunctionTable = NULL;
static size_t gShellFunctionTableCount = 0;
static ShellAlias *gShellAliasTable = NULL;
static size_t gShellAliasTableCount = 0;
#if defined(PSCAL_TARGET_IOS)
#define gShellFunctions (*(SC()->has_private_tables ? &SC()->private_functions : &gShellFunctionTable))
#define gShellFunctionCount (*(SC()->has_private_tables ? &SC()->private_function_count : &gShellFunctionTableCount))
#else
#define gShellFunctions gShellFunctionTable
#define gShellFunctionCount gShellFunctionTableCount
#endif

static int shellParseEnvBool(const char *value) {
    if (!value) {
        return -1;
    }
    while (*value && isspace((unsigned char)*value)) {
        value++;
    }
    if (*value == '\0') {
        return -1;
    }
    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) {
        return 1;
    }
    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
        return 0;
    }
    return -1;
}

static void shellRuntimeFreeCallFrame(ShellCallFrame *frame) {
    if (!frame) {
        return;
    }
    free(frame->function);
    free(frame->source);
    memset(frame, 0, sizeof(*frame));
}

static bool shellRuntimeEnsureCallStackCapacity(size_t needed) {
    if (gShellCallStackCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellCallStackCapacity ? (gShellCallStackCapacity * 2) : 4;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    ShellCallFrame *resized = (ShellCallFrame *)realloc(gShellCallStack,
                                                        new_capacity * sizeof(ShellCallFrame));
    if (!resized) {
        return false;
    }
    memset(resized + gShellCallStackCapacity, 0, (new_capacity - gShellCallStackCapacity) * sizeof(ShellCallFrame));
    gShellCallStack = resized;
    gShellCallStackCapacity = new_capacity;
    return true;
}

static bool shellRuntimePushCallFrame(const char *function_name, const char *source, int call_line, bool is_function) {
    if (!shellRuntimeEnsureCallStackCapacity(gShellCallStackCount + 1)) {
        return false;
    }
    char *function_copy = strdup(function_name ? function_name : "");
    char *source_copy = strdup(source ? source : "");
    if (!function_copy || !source_copy) {
        free(function_copy);
        free(source_copy);
        return false;
    }
    ShellCallFrame *frame = &gShellCallStack[gShellCallStackCount++];
    frame->function = function_copy;
    frame->source = source_copy;
    frame->line = (call_line >= 0) ? call_line : 0;
    frame->is_function = is_function;
    return true;
}

/* ${BASH_SOURCE[0]}, or what a function defined here keeps: a -c string has $0, a prompt "main". */
static const char *shellRuntimeCurrentSourceName(void) {
    if (gShellCallStackCount > 0) {
        return gShellCallStack[gShellCallStackCount - 1].source;
    }
    if (gShellScriptName) {
        return gShellScriptName;
    }
    if (shellRuntimeIsInteractive()) {
        return "main";
    }
    return gShellArg0 ? gShellArg0 : "exsh";
}

static void shellRuntimePopCallFrame(void) {
    if (gShellCallStackCount == 0) {
        return;
    }
    ShellCallFrame *frame = &gShellCallStack[gShellCallStackCount - 1];
    shellRuntimeFreeCallFrame(frame);
    gShellCallStackCount--;
    if (gShellCallStackCount == 0 && gShellCallStack) {
        free(gShellCallStack);
        gShellCallStack = NULL;
        gShellCallStackCapacity = 0;
    }
}

static size_t shellRuntimeCallStackDepth(void) {
    return gShellCallStackCount;
}

static const ShellCallFrame *shellRuntimeCallFrameAtDepth(size_t depth) {
    if (depth >= gShellCallStackCount) {
        return NULL;
    }
    return &gShellCallStack[gShellCallStackCount - 1 - depth];
}

/* Where a job or a stage was started from, or what a shell started as a command puts aside. On iOS
 * a job runs on a thread of its own, and caller, FUNCNAME and BASH_SOURCE show what a forked child
 * would have. */
typedef struct {
    ShellCallFrame *frames;
    size_t frame_count;
    char *script_name;
} ShellCallStack;

static const char *shellResolveBashPath(void) {
    static char resolved[PATH_MAX];
    static bool initialised = false;
    if (initialised) {
        return resolved[0] ? resolved : "/bin/bash";
    }
    const char *env_bash = shellSafeGetenv("BASH");
    const char *candidates[] = {
        env_bash,
        "/opt/homebrew/bin/bash",
        "/usr/local/bin/bash",
        "/bin/bash",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const char *candidate = candidates[i];
        if (!candidate || *candidate == '\0') {
            continue;
        }
        if (access(candidate, X_OK) == 0) {
            strncpy(resolved, candidate, sizeof(resolved) - 1);
            resolved[sizeof(resolved) - 1] = '\0';
            setenv("BASH", resolved, 1);
            break;
        }
    }
    if (resolved[0] == '\0') {
        strncpy(resolved, "/bin/bash", sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
    initialised = true;
    return resolved;
}

static bool shellAssociativeArraysSupported(void) {
    return shellParseEnvBool(shellSafeGetenv("PSCALSHELL_ASSOC_ARRAYS")) != 0;
}

static bool shellBindRequiresInteractive(void) {
    if (gShellBindInteractiveStatus != -1) {
        return gShellBindInteractiveStatus == 1;
    }

    int override_value = shellParseEnvBool(shellSafeGetenv("PSCALSHELL_BIND_INTERACTIVE"));
    if (override_value == 0 || override_value == 1) {
        gShellBindInteractiveStatus = override_value ? 1 : 0;
        return gShellBindInteractiveStatus == 1;
    }

    bool requires = false;
    const char *bash_path = shellResolveBashPath();
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        gShellBindInteractiveStatus = 0;
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        gShellBindInteractiveStatus = 0;
        return false;
    }
    if (pid == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execl(bash_path,
              bash_path,
              "--noprofile",
              "--norc",
              "-c",
              "bind 'set show-all-if-ambiguous on'",
              (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);
    char buffer[256];
    while (true) {
        ssize_t n = read(pipe_fds[0], buffer, sizeof(buffer));
        if (n > 0) {
            requires = true;
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    close(pipe_fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) >= 0) {
        if (!requires) {
            requires = !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
    }
    gShellBindInteractiveStatus = requires ? 1 : 0;
    return requires;
}

static void shellArrayVariableClear(ShellArrayVariable *var);
static bool shellArrayRegistryEnsureCapacity(size_t needed);
static ShellArrayVariable *shellArrayRegistryFindMutable(const char *name);
static const ShellArrayVariable *shellArrayRegistryFindConst(const char *name);
static bool shellArrayRegistryStore(const char *name,
                                   char **items,
                                   char **keys,
                                   size_t count,
                                   ShellArrayKind kind);
static void shellArrayRegistryRemove(const char *name);
static const ShellArrayVariable *shellArrayRegistryLookup(const char *name, size_t len);
static void shellArrayRegistryAssignFromText(const char *name, const char *value);
static bool shellSetTrackedVariable(const char *name, const char *value, bool is_array_literal);
static void shellUnsetTrackedVariable(const char *name);
static bool shellFdVarAssign(const char *name, int fd);
static char *shellLookupRawEnvironmentValue(const char *name, size_t len);
static char *shellLookupParameterValue(const char *name, size_t len);
static bool shellAssignmentIsArrayLiteral(const char *raw_assignment, uint8_t word_flags);
static bool shellArrayRegistrySetElement(const char *name, const char *subscript, const char *value);
static bool shellExtractArrayNameAndSubscript(const char *text,
                                             char **out_name,
                                             char **out_subscript);
static bool shellParseArrayLiteral(const char *value,
                                   char ***out_items,
                                   char ***out_keys,
                                   size_t *out_count,
                                   ShellArrayKind *out_kind);
static char *shellBuildArrayLiteral(const ShellArrayVariable *var);
static bool shellArrayRegistryInitializeAssociative(const char *name);
static ShellAlias *shellFindAlias(const char *name);
static void shellReportRecoverableError(VM *vm, bool with_location, const char *fmt, ...);
static void shellFdPrintf(int fd, const char *fmt, ...);

static bool shellReadonlyEnsureCapacity(size_t needed);
static ShellReadonlyEntry *shellReadonlyFindMutable(const char *name);
static bool shellReadonlyAdd(const char *name);
static bool shellReadonlyContains(const char *name);
static void shellReadonlySetErrorName(const char *name);
static const char *shellReadonlyGetErrorName(void);
static void shellReadonlyPrintVariables(void);

static bool shellIsValidEnvName(const char *name);
static void shellExportPrintEnvironment(void);
static bool shellParseReturnStatus(const char *text, int *out_status);
static void shellMarkArithmeticError(void);
static const char *shellCurrentIfs(void);
static bool shellArrayRegistryIndexedExists(const char *name);
static char *shellParamSubscriptKey(const char *name, size_t name_len, const char *subscript, size_t subscript_len);
static void shellArraySortSparse(ShellArrayVariable *var);

static PSCAL_THREAD_LOCAL bool gShellPositionalOwned = false;

typedef struct ShellOptionEntry {
    const char *name;
    bool enabled;
} ShellOptionEntry;

static ShellOptionEntry gShellOptionTable[] = {
    {"assoc_expand_once", false},
    {"autocd", false},
    {"cdable_vars", false},
    {"cdspell", false},
    {"checkhash", false},
    {"checkjobs", false},
    {"checkwinsize", false},
    {"cmdhist", true},
    {"compat31", false},
    {"compat32", false},
    {"compat40", false},
    {"compat41", false},
    {"compat42", false},
    {"compat43", false},
    {"complete_fullquote", false},
    {"direxpand", false},
    {"dirspell", false},
    {"dotglob", false},
    {"execfail", false},
    {"expand_aliases", false},
    {"extdebug", false},
    {"extglob", false},
    {"extquote", true},
    {"failglob", false},
    {"force_fignore", true},
    {"globasciiranges", false},
    {"globskipdots", false},
    {"globstar", false},
    {"gnu_errfmt", false},
    {"histappend", false},
    {"histreedit", false},
    {"histverify", false},
    {"hostcomplete", true},
    {"huponexit", false},
    {"inherit_errexit", false},
    {"interactive_comments", true},
    {"lastpipe", false},
    {"lithist", false},
    {"localvar_inherit", false},
    {"localvar_unset", false},
    {"login_shell", false},
    {"mailwarn", false},
    {"no_empty_cmd_completion", false},
    {"nocaseglob", false},
    {"nocasematch", false},
    {"nullglob", false},
    {"progcomp", true},
    {"promptvars", true},
    {"restricted_shell", false},
    {"shift_verbose", false},
    {"sourcepath", true},
    {"varredir_close", false},
    {"xpg_echo", false}
};

static const size_t gShellOptionCount = sizeof(gShellOptionTable) / sizeof(gShellOptionTable[0]);
#if defined(PSCAL_TARGET_IOS)
#define gShellOptions (SC()->has_private_tables ? SC()->private_options : gShellOptionTable)
#else
#define gShellOptions gShellOptionTable
#endif

static bool shellShoptOptionEnabled(const char *name);
static ShellOptionEntry *shellShoptFindOption(const char *name);

typedef struct {
    char *name;
    char *value;
} ShellBindOption;

static ShellBindOption *gShellBindOptions = NULL;
static size_t gShellBindOptionCount = 0;
static PSCAL_THREAD_LOCAL int gShellCurrentCommandLine = 0;
static PSCAL_THREAD_LOCAL int gShellCurrentCommandColumn = 0;
static PSCAL_THREAD_LOCAL const char *gShellCurrentCommandName = NULL;
static pthread_once_t gShellUtf8LocaleOnce = PTHREAD_ONCE_INIT;
static locale_t gShellUtf8Locale = (locale_t)0;

static void shellInitUtf8Locale(void) {
    gShellUtf8Locale = newlocale(LC_CTYPE_MASK, "", (locale_t)0);
}

static void shellRuntimeSetCurrentCommandLocation(int line, int column) {
    gShellCurrentCommandLine = line;
    gShellCurrentCommandColumn = column;
}

static void shellRuntimeSetCurrentCommandName(const char *name) {
    gShellCurrentCommandName = name;
}

static const char *shellRuntimeCurrentCommandName(void) {
    return gShellCurrentCommandName;
}

static void shellFreeParameterArray(char **values, int count) {
    if (!values) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        free(values[i]);
    }
    free(values);
}

/* A nested exsh on iOS points $1 at its own argv. Without this, `set --` in it
 * would free that as the array the caller's `set --` allocated. What the nested
 * `set --` allocates is left to leak, as in shellRunExshShebang. */
bool shellRuntimeExchangePositionalOwned(bool owned) {
    bool previous = gShellPositionalOwned;
    gShellPositionalOwned = owned;
    return previous;
}

static void shellFreeOwnedPositionalParameters(void) {
    if (!gShellPositionalOwned) {
        return;
    }
    if (gParamValues && gParamCount > 0) {
        shellFreeParameterArray(gParamValues, gParamCount);
    } else if (gParamValues) {
        free(gParamValues);
    }
    gParamValues = NULL;
    gParamCount = 0;
    gShellPositionalOwned = false;
}

typedef enum {
    SHELL_PATTERN_NONE = 0,
    SHELL_PATTERN_NOCASE = 1 << 0
} ShellPatternFlags;

static int shellPatternMatchEx(const char *pattern, const char *text, ShellPatternFlags flags);
static int shellPatternMatch(const char *pattern, const char *text);
static int shellExpandGlobPattern(const char *pattern, char ***out_matches, size_t *out_count);
static const char *shellExtglobFind(const char *pattern);

static int shellCompareGlobMatches(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* An extended pattern in the last part of a path: read the directory and test each name. */
static int shellExpandExtglob(const char *pattern, char ***out_matches, size_t *out_count) {
    const char *slash = strrchr(pattern, '/');
    const char *base = slash ? slash + 1 : pattern;
    size_t dir_len = slash ? (size_t)(slash - pattern) + 1 : 0;
    char *dir_name = strndup(pattern, dir_len);
    if (!dir_name) {
        return GLOB_NOSPACE;
    }
    DIR *dir = opendir(dir_len ? dir_name : ".");
    if (!dir) {
        free(dir_name);
        return GLOB_NOMATCH;
    }
    bool hidden_too = base[0] == '.' || shellShoptOptionEnabled("dotglob");
    ShellPatternFlags flags = shellShoptOptionEnabled("nocaseglob") ? SHELL_PATTERN_NOCASE : SHELL_PATTERN_NONE;
    char **matches = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || (name[0] == '.' && !hidden_too)) {
            continue;
        }
        if (shellPatternMatchEx(base, name, flags) != 0) {
            continue;
        }
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            char **grown = (char **)realloc(matches, capacity * sizeof(char *));
            if (!grown) {
                break;
            }
            matches = grown;
        }
        size_t n = strlen(name);
        char *path = (char *)malloc(dir_len + n + 1);
        if (!path) {
            break;
        }
        memcpy(path, dir_name, dir_len);
        memcpy(path + dir_len, name, n + 1);
        matches[count++] = path;
    }
    closedir(dir);
    free(dir_name);
    if (count == 0) {
        free(matches);
        return GLOB_NOMATCH;
    }
    qsort(matches, count, sizeof(char *), shellCompareGlobMatches);
    *out_matches = matches;
    *out_count = count;
    return 0;
}

/* shellExpandGlobPattern and what shopt adds to it, in byte order. A hidden name only matches a
 * pattern that starts with a dot, so dotglob asks again with one and merges the two. */
static int shellExpandGlobInByteOrder(const char *pattern, char ***out_matches, size_t *out_count) {
    *out_matches = NULL;
    *out_count = 0;
    if (shellExtglobFind(pattern)) {
        return shellExpandExtglob(pattern, out_matches, out_count);
    }
    int status = shellExpandGlobPattern(pattern, out_matches, out_count);
    if (!shellShoptOptionEnabled("dotglob")) {
        return status;
    }
    const char *slash = strrchr(pattern, '/');
    const char *base = slash ? slash + 1 : pattern;
    if (base[0] == '.') {
        return status;
    }
    size_t dir_len = (size_t)(base - pattern);
    size_t n = strlen(pattern);
    char *hidden_pattern = (char *)malloc(n + 2);
    if (!hidden_pattern) {
        return status;
    }
    memcpy(hidden_pattern, pattern, dir_len);
    hidden_pattern[dir_len] = '.';
    memcpy(hidden_pattern + dir_len + 1, base, n - dir_len + 1);
    char **hidden = NULL;
    size_t hidden_count = 0;
    int hidden_status = shellExpandGlobPattern(hidden_pattern, &hidden, &hidden_count);
    free(hidden_pattern);
    if (hidden_status != 0) {
        return status;
    }
    size_t count = *out_count;
    char **merged = (char **)realloc(*out_matches, (count + hidden_count) * sizeof(char *));
    if (!merged) {
        return status;
    }
    for (size_t i = 0; i < hidden_count; ++i) {
        const char *name = strrchr(hidden[i], '/');
        name = name ? name + 1 : hidden[i];
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            free(hidden[i]);
        } else {
            merged[count++] = hidden[i];
        }
    }
    free(hidden);
    if (count == 0) {
        free(merged);
        *out_matches = NULL;
        return GLOB_NOMATCH;
    }
    qsort(merged, count, sizeof(char *), shellCompareGlobMatches);
    *out_matches = merged;
    *out_count = count;
    return 0;
}

/* Loaded collations, kept for the life of the process: another thread may be sorting with one.
 * Past the last slot a new name sorts by bytes. A name that won't load is kept too, as no locale. */
static pthread_mutex_t gShellCollateMu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char *name;
    locale_t locale;
} gShellCollateCache[16];
static size_t gShellCollateCount = 0;

/* What the shell's variables choose, exported or not, as they stand now: bash follows an
 * assignment at once. No locale means C. The process's own locale and environment don't say,
 * since nothing sets the one and on iOS the other doesn't hold a tab's LANG. */
static locale_t shellCollateLocale(void) {
    static const char *const kNames[] = {"LC_ALL", "LC_COLLATE", "LANG"};
    char *name = NULL;
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]) && !name; ++i) {
        char *value = shellLookupParameterValue(kNames[i], strlen(kNames[i]));
        if (value && *value) {
            name = value;
        } else {
            free(value);
        }
    }
    if (!name || strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0 || strchr(name, '/')) {
        free(name);
        return (locale_t)0;
    }
    locale_t found = (locale_t)0;
    bool known = false;
    pthread_mutex_lock(&gShellCollateMu);
    for (size_t i = 0; i < gShellCollateCount && !known; ++i) {
        if (strcmp(gShellCollateCache[i].name, name) == 0) {
            found = gShellCollateCache[i].locale;
            known = true;
        }
    }
    if (!known && gShellCollateCount < sizeof(gShellCollateCache) / sizeof(gShellCollateCache[0])) {
#if defined(PSCAL_TARGET_IOS)
        found = (locale_t)vprocLocaleLoad(LC_COLLATE_MASK, name);
#else
        found = newlocale(LC_COLLATE_MASK, name, (locale_t)0);
#endif
        gShellCollateCache[gShellCollateCount].name = name;
        gShellCollateCache[gShellCollateCount].locale = found;
        gShellCollateCount++;
        name = NULL;
    }
    pthread_mutex_unlock(&gShellCollateMu);
    free(name);
    return found;
}

static int shellCompareGlobCollated(const void *a, const void *b) {
    const char *left = *(char *const *)a;
    const char *right = *(char *const *)b;
    int order = strcoll(left, right);
    return order != 0 ? order : strcmp(left, right);
}

/* A glob's matches, sorted as the locale collates them, as bash does. */
static int shellExpandGlobWithOptions(const char *pattern, char ***out_matches, size_t *out_count) {
    int status = shellExpandGlobInByteOrder(pattern, out_matches, out_count);
    if (status != 0 || *out_count < 2) {
        return status;
    }
    locale_t collate = shellCollateLocale();
    if (collate) {
        /* For this thread alone, and qsort has no portable way to pass the locale along. */
        locale_t previous = uselocale(collate);
        qsort(*out_matches, *out_count, sizeof(char *), shellCompareGlobCollated);
        uselocale(previous);
    }
    return status;
}

static bool shellIsCharBoundary(const char *text, size_t index) {
    return ((unsigned char)text[index] & 0xC0) != 0x80;
}

static char *shellRemovePatternPrefix(const char *value, const char *pattern, bool longest) {
    if (!value) {
        return strdup("");
    }
    if (!pattern || !*pattern) {
        return strdup(value);
    }
    size_t len = strlen(value);
    char *work = strdup(value);
    if (!work) {
        return NULL;
    }
    /* A pattern ending in a plain character can only end at one, which saves matching
     * against all of a long value. */
    size_t pattern_len = strlen(pattern);
    char last = pattern[pattern_len - 1];
    bool last_literal = !strchr("*?[]\\", last);
    size_t cut = longest ? len : 0;
    size_t match = SIZE_MAX;
    while (true) {
        if ((cut == len || shellIsCharBoundary(work, cut)) && (!last_literal || (cut > 0 && value[cut - 1] == last))) {
            char saved = work[cut];
            work[cut] = '\0';
            int rc = shellPatternMatch(pattern, work);
            work[cut] = saved;
            if (rc == 0) {
                match = cut;
                break;
            }
        }
        if (longest ? cut == 0 : cut == len) {
            break;
        }
        cut += longest ? (size_t)-1 : 1;
    }
    free(work);
    return strdup(match == SIZE_MAX ? value : value + match);
}

static bool shellUtf8GlobConsumeToken(const char *pattern,
                                      uint32_t *out_codepoint,
                                      size_t *out_advance,
                                      bool *out_wildcard_any,
                                      bool *out_wildcard_single) {
    if (!pattern || !*pattern || !out_advance || !out_wildcard_any || !out_wildcard_single) {
        return false;
    }
    *out_wildcard_any = false;
    *out_wildcard_single = false;
    if (*pattern == '*') {
        *out_wildcard_any = true;
        *out_advance = 1;
        return true;
    }
    if (*pattern == '?') {
        *out_wildcard_single = true;
        *out_advance = 1;
        return true;
    }
    if (*pattern == '\\' && pattern[1] != '\0') {
        if (out_codepoint) {
            *out_codepoint = (unsigned char)pattern[1];
        }
        *out_advance = 2;
        return true;
    }
    if ((unsigned char)*pattern < 0x80) {
        if (out_codepoint) {
            *out_codepoint = (unsigned char)*pattern;
        }
        *out_advance = 1;
        return true;
    }
    return decodeUtf8Codepoint(pattern, strlen(pattern), out_codepoint, out_advance);
}

static int shellUtf8GlobReadLiteralCodepoint(const char *pattern,
                                             uint32_t *out_codepoint,
                                             size_t *out_advance) {
    if (!pattern || !*pattern || !out_codepoint || !out_advance) {
        return -1;
    }
    if (*pattern == '\\' && pattern[1] != '\0') {
        *out_codepoint = (unsigned char)pattern[1];
        *out_advance = 2;
        return 1;
    }
    if ((unsigned char)*pattern < 0x80) {
        *out_codepoint = (unsigned char)*pattern;
        *out_advance = 1;
        return 1;
    }
    return decodeUtf8Codepoint(pattern, strlen(pattern), out_codepoint, out_advance) ? 1 : -1;
}

static uint32_t shellUtf8FoldCaseCodepoint(uint32_t codepoint) {
    if (codepoint <= (uint32_t)WCHAR_MAX) {
        pthread_once(&gShellUtf8LocaleOnce, shellInitUtf8Locale);
        wint_t wc = (wint_t)codepoint;
        if (gShellUtf8Locale) {
            locale_t previous = uselocale(gShellUtf8Locale);
            wint_t lowered = towlower(wc);
            uselocale(previous);
            if ((uint32_t)lowered != codepoint) {
                return (uint32_t)lowered;
            }
        } else {
            wint_t lowered = towlower(wc);
            if ((uint32_t)lowered != codepoint) {
                return (uint32_t)lowered;
            }
        }
    }

    if (codepoint >= 'A' && codepoint <= 'Z') {
        return codepoint + 0x20;
    }
    if (codepoint >= 0x00C0 && codepoint <= 0x00D6) return codepoint + 0x20;
    if (codepoint >= 0x00D8 && codepoint <= 0x00DE) return codepoint + 0x20;
    if (codepoint >= 0x0391 && codepoint <= 0x03AB) return codepoint + 0x20;
    if (codepoint >= 0x0410 && codepoint <= 0x042F) return codepoint + 0x20;
    if (codepoint == 0x0401) return 0x0451;
    return codepoint;
}

static bool shellUtf8CodepointsEqual(uint32_t lhs, uint32_t rhs, bool nocase) {
    if (!nocase) {
        return lhs == rhs;
    }
    return shellUtf8FoldCaseCodepoint(lhs) == shellUtf8FoldCaseCodepoint(rhs);
}

static int shellUtf8StringCompare(const char *lhs, const char *rhs, bool nocase) {
    if (!lhs) lhs = "";
    if (!rhs) rhs = "";
    if (!nocase) {
        return strcmp(lhs, rhs);
    }

    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    bool lhs_utf8 = lhs_len == 0 || isValidUtf8Bytes(lhs, lhs_len);
    bool rhs_utf8 = rhs_len == 0 || isValidUtf8Bytes(rhs, rhs_len);
    if (!lhs_utf8 || !rhs_utf8) {
        return strcasecmp(lhs, rhs);
    }

    const char *lcur = lhs;
    const char *rcur = rhs;
    while (*lcur && *rcur) {
        uint32_t lcp = 0, rcp = 0;
        size_t ladv = 0, radv = 0;
        if (!decodeUtf8Codepoint(lcur, strlen(lcur), &lcp, &ladv) ||
            !decodeUtf8Codepoint(rcur, strlen(rcur), &rcp, &radv)) {
            return strcasecmp(lhs, rhs);
        }
        uint32_t lf = shellUtf8FoldCaseCodepoint(lcp);
        uint32_t rf = shellUtf8FoldCaseCodepoint(rcp);
        if (lf != rf) {
            return (lf < rf) ? -1 : 1;
        }
        lcur += ladv;
        rcur += radv;
    }
    if (*lcur == '\0' && *rcur == '\0') return 0;
    return *lcur ? 1 : -1;
}

static bool shellUtf8CodepointIsUnicodeSpace(uint32_t codepoint) {
    switch (codepoint) {
        case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
        case 0x0020: case 0x0085: case 0x00A0: case 0x1680: case 0x2028:
        case 0x2029: case 0x202F: case 0x205F: case 0x3000:
            return true;
        default:
            return (codepoint >= 0x2000 && codepoint <= 0x200A);
    }
}

static bool shellUtf8CodepointIsUnicodeDigit(uint32_t codepoint) {
    if (codepoint >= '0' && codepoint <= '9') return true;
    if (codepoint >= 0x0660 && codepoint <= 0x0669) return true;
    if (codepoint >= 0x06F0 && codepoint <= 0x06F9) return true;
    if (codepoint >= 0x0966 && codepoint <= 0x096F) return true;
    if (codepoint >= 0x09E6 && codepoint <= 0x09EF) return true;
    if (codepoint >= 0x0A66 && codepoint <= 0x0A6F) return true;
    if (codepoint >= 0x0AE6 && codepoint <= 0x0AEF) return true;
    if (codepoint >= 0x0B66 && codepoint <= 0x0B6F) return true;
    if (codepoint >= 0x0BE6 && codepoint <= 0x0BEF) return true;
    if (codepoint >= 0x0C66 && codepoint <= 0x0C6F) return true;
    if (codepoint >= 0x0CE6 && codepoint <= 0x0CEF) return true;
    if (codepoint >= 0x0D66 && codepoint <= 0x0D6F) return true;
    if (codepoint >= 0x0E50 && codepoint <= 0x0E59) return true;
    if (codepoint >= 0x0ED0 && codepoint <= 0x0ED9) return true;
    if (codepoint >= 0x0F20 && codepoint <= 0x0F29) return true;
    if (codepoint >= 0x1040 && codepoint <= 0x1049) return true;
    if (codepoint >= 0x17E0 && codepoint <= 0x17E9) return true;
    if (codepoint >= 0x1810 && codepoint <= 0x1819) return true;
    if (codepoint >= 0xFF10 && codepoint <= 0xFF19) return true;
    return false;
}

static bool shellUtf8CodepointIsLikelyLetter(uint32_t codepoint) {
    if (codepoint < 0x80) {
        return ((codepoint >= 'A' && codepoint <= 'Z') ||
                (codepoint >= 'a' && codepoint <= 'z'));
    }
    if (shellUtf8CodepointIsUnicodeSpace(codepoint) || shellUtf8CodepointIsUnicodeDigit(codepoint)) {
        return false;
    }
    if ((codepoint >= 0x2000 && codepoint <= 0x206F) ||
        (codepoint >= 0x20A0 && codepoint <= 0x20CF) ||
        (codepoint >= 0x2100 && codepoint <= 0x214F) ||
        (codepoint >= 0x2190 && codepoint <= 0x21FF) ||
        (codepoint >= 0x2200 && codepoint <= 0x27FF) ||
        (codepoint >= 0x2900 && codepoint <= 0x2BFF) ||
        (codepoint >= 0x2E00 && codepoint <= 0x2E7F) ||
        (codepoint >= 0x3000 && codepoint <= 0x303F) ||
        (codepoint >= 0xFE10 && codepoint <= 0xFE1F) ||
        (codepoint >= 0xFE30 && codepoint <= 0xFE6F) ||
        (codepoint >= 0xFF01 && codepoint <= 0xFF0F) ||
        (codepoint >= 0xFF1A && codepoint <= 0xFF20) ||
        (codepoint >= 0xFF3B && codepoint <= 0xFF40) ||
        (codepoint >= 0xFF5B && codepoint <= 0xFF65) ||
        (codepoint >= 0x1F300 && codepoint <= 0x1FAFF)) {
        return false;
    }
    return true;
}

static bool shellUtf8CodepointMatchesPosixClass(uint32_t codepoint, const char *class_name) {
    if (!class_name || !*class_name || codepoint > (uint32_t)WCHAR_MAX) {
        return false;
    }
    pthread_once(&gShellUtf8LocaleOnce, shellInitUtf8Locale);
    wint_t wc = (wint_t)codepoint;
    if (gShellUtf8Locale) {
        locale_t previous = uselocale(gShellUtf8Locale);
        bool result = false;
        if (strcmp(class_name, "alnum") == 0) result = iswalnum(wc) != 0;
        else if (strcmp(class_name, "alpha") == 0) result = iswalpha(wc) != 0;
        else if (strcmp(class_name, "blank") == 0) result = iswblank(wc) != 0;
        else if (strcmp(class_name, "cntrl") == 0) result = iswcntrl(wc) != 0;
        else if (strcmp(class_name, "digit") == 0) result = iswdigit(wc) != 0;
        else if (strcmp(class_name, "graph") == 0) result = iswgraph(wc) != 0;
        else if (strcmp(class_name, "lower") == 0) result = iswlower(wc) != 0;
        else if (strcmp(class_name, "print") == 0) result = iswprint(wc) != 0;
        else if (strcmp(class_name, "punct") == 0) result = iswpunct(wc) != 0;
        else if (strcmp(class_name, "space") == 0) result = iswspace(wc) != 0;
        else if (strcmp(class_name, "upper") == 0) result = iswupper(wc) != 0;
        else if (strcmp(class_name, "xdigit") == 0) result = iswxdigit(wc) != 0;
        uselocale(previous);
        if (!result && codepoint >= 0x80) {
            if (strcmp(class_name, "alpha") == 0) result = shellUtf8CodepointIsLikelyLetter(codepoint);
            else if (strcmp(class_name, "alnum") == 0) result = shellUtf8CodepointIsLikelyLetter(codepoint) || shellUtf8CodepointIsUnicodeDigit(codepoint);
            else if (strcmp(class_name, "digit") == 0) result = shellUtf8CodepointIsUnicodeDigit(codepoint);
            else if (strcmp(class_name, "space") == 0) result = shellUtf8CodepointIsUnicodeSpace(codepoint);
            else if (strcmp(class_name, "graph") == 0) result = !shellUtf8CodepointIsUnicodeSpace(codepoint);
            else if (strcmp(class_name, "print") == 0) result = true;
        }
        return result;
    }
    if (strcmp(class_name, "alnum") == 0) return iswalnum(wc) != 0;
    if (strcmp(class_name, "alpha") == 0) return iswalpha(wc) != 0;
    if (strcmp(class_name, "blank") == 0) return iswblank(wc) != 0;
    if (strcmp(class_name, "cntrl") == 0) return iswcntrl(wc) != 0;
    if (strcmp(class_name, "digit") == 0) return iswdigit(wc) != 0;
    if (strcmp(class_name, "graph") == 0) return iswgraph(wc) != 0;
    if (strcmp(class_name, "lower") == 0) return iswlower(wc) != 0;
    if (strcmp(class_name, "print") == 0) return iswprint(wc) != 0;
    if (strcmp(class_name, "punct") == 0) return iswpunct(wc) != 0;
    if (strcmp(class_name, "space") == 0) return iswspace(wc) != 0;
    if (strcmp(class_name, "upper") == 0) return iswupper(wc) != 0;
    if (strcmp(class_name, "xdigit") == 0) return iswxdigit(wc) != 0;
    return false;
}

static int shellUtf8GlobMatchBracket(const char *pattern,
                                     uint32_t text_cp,
                                     size_t *out_advance,
                                     ShellPatternFlags flags) {
    if (!pattern || pattern[0] != '[' || !out_advance) {
        return -1;
    }

    size_t offset = 1;
    bool negate = false;
    bool matched = false;
    bool allow_literal_close = true;

    if (pattern[offset] == '!' || pattern[offset] == '^') {
        negate = true;
        offset++;
    }
    if (pattern[offset] == '\0') {
        return -1;
    }

    while (pattern[offset] != '\0') {
        if (pattern[offset] == ']' && !allow_literal_close) {
            *out_advance = offset + 1;
            return (negate ? !matched : matched) ? 1 : 0;
        }

        if (pattern[offset] == '[' && pattern[offset + 1] == ':') {
            const char *class_start = pattern + offset + 2;
            const char *class_end = strstr(class_start, ":]");
            if (!class_end) {
                return -1;
            }
            size_t class_len = (size_t)(class_end - class_start);
            if (class_len == 0 || class_len >= 32) {
                return -1;
            }
            char class_name[32];
            memcpy(class_name, class_start, class_len);
            class_name[class_len] = '\0';
            if (shellUtf8CodepointMatchesPosixClass(text_cp, class_name)) {
                matched = true;
            }
            offset = (size_t)(class_end - pattern) + 2;
            allow_literal_close = false;
            continue;
        }

        if (pattern[offset] == '[' &&
            (pattern[offset + 1] == '.' || pattern[offset + 1] == '=')) {
            return -1;
        }

        uint32_t start_cp = 0;
        size_t start_advance = 0;
        if (shellUtf8GlobReadLiteralCodepoint(pattern + offset, &start_cp, &start_advance) <= 0) {
            return -1;
        }

        size_t next_offset = offset + start_advance;
        if (pattern[next_offset] == '-' &&
            pattern[next_offset + 1] != '\0' &&
            pattern[next_offset + 1] != ']') {
            uint32_t end_cp = 0;
            size_t end_advance = 0;
            if (shellUtf8GlobReadLiteralCodepoint(pattern + next_offset + 1, &end_cp, &end_advance) <= 0) {
                return -1;
            }
            if (flags & SHELL_PATTERN_NOCASE) {
                start_cp = shellUtf8FoldCaseCodepoint(start_cp);
                end_cp = shellUtf8FoldCaseCodepoint(end_cp);
            }
            uint32_t folded_text = (flags & SHELL_PATTERN_NOCASE)
                ? shellUtf8FoldCaseCodepoint(text_cp)
                : text_cp;
            uint32_t lo = start_cp < end_cp ? start_cp : end_cp;
            uint32_t hi = start_cp > end_cp ? start_cp : end_cp;
            if (folded_text >= lo && folded_text <= hi) {
                matched = true;
            }
            offset = next_offset + 1 + end_advance;
        } else {
            if (shellUtf8CodepointsEqual(text_cp, start_cp, (flags & SHELL_PATTERN_NOCASE) != 0)) {
                matched = true;
            }
            offset = next_offset;
        }

        allow_literal_close = false;
    }

    return -1;
}

static int shellUtf8GlobMatchInternal(const char *pattern, const char *text, ShellPatternFlags flags) {
    while (pattern && *pattern) {
        uint32_t pat_cp = 0;
        size_t pat_advance = 0;
        bool wildcard_any = false;
        bool wildcard_single = false;
        bool bracket = false;

        if (*pattern == '[') {
            bracket = true;
            pat_advance = 1;
        } else if (!shellUtf8GlobConsumeToken(pattern,
                                              &pat_cp,
                                              &pat_advance,
                                              &wildcard_any,
                                              &wildcard_single)) {
            return -1;
        }
        if (wildcard_any) {
            pattern += pat_advance;
            while (*pattern == '*') {
                pattern++;
            }
            if (*pattern == '\0') {
                return 1;
            }
            const char *cursor = text;
            while (true) {
                int nested = shellUtf8GlobMatchInternal(pattern, cursor, flags);
                if (nested > 0) {
                    return 1;
                }
                if (nested < 0) {
                    return -1;
                }
                if (!cursor || *cursor == '\0') {
                    break;
                }
                uint32_t text_cp = 0;
                size_t text_advance = 0;
                if (!decodeUtf8Codepoint(cursor, strnlen(cursor, 4), &text_cp, &text_advance)) {
                    return -1;
                }
                cursor += text_advance;
            }
            return 0;
        }
        if (!text || *text == '\0') {
            return 0;
        }
        uint32_t text_cp = 0;
        size_t text_advance = 0;
        if (!decodeUtf8Codepoint(text, strnlen(text, 4), &text_cp, &text_advance)) {
            return -1;
        }
        if (bracket) {
            int bracket_match = shellUtf8GlobMatchBracket(pattern, text_cp, &pat_advance, flags);
            if (bracket_match < 0) {
                return -1;
            }
            if (bracket_match == 0) {
                return 0;
            }
        } else if (!wildcard_single &&
                   !shellUtf8CodepointsEqual(text_cp, pat_cp, (flags & SHELL_PATTERN_NOCASE) != 0)) {
            return 0;
        }
        pattern += pat_advance;
        text += text_advance;
    }
    return (!text || *text == '\0') ? 1 : 0;
}

/* The opening of an extended pattern at or after this point, or NULL: @( ?( *( +( !( */
static const char *shellExtglobFind(const char *pattern) {
    bool in_bracket = false;
    for (const char *p = pattern; *p; ++p) {
        if (*p == '\\' && p[1]) {
            ++p;
        } else if (in_bracket) {
            in_bracket = *p != ']';
        } else if (*p == '[') {
            in_bracket = true;
        } else if (strchr("@?*+!", *p) && p[1] == '(') {
            return p;
        }
    }
    return NULL;
}

static const char *shellExtglobClose(const char *open) {
    int depth = 0;
    for (const char *p = open; *p; ++p) {
        if (*p == '\\' && p[1]) {
            ++p;
        } else if (*p == '(') {
            depth++;
        } else if (*p == ')' && --depth == 0) {
            return p;
        }
    }
    return NULL;
}

static int shellPatternMatchEx(const char *pattern, const char *text, ShellPatternFlags flags);

/* Whether one of the | alternatives in [start, end) matches all of text. */
static bool shellExtglobAnyAlternative(const char *start, const char *end, const char *text, ShellPatternFlags flags) {
    const char *piece = start;
    int depth = 0;
    for (const char *p = start; p <= end; ++p) {
        bool done = p == end;
        if (!done && *p == '\\' && p + 1 < end) {
            ++p;
            continue;
        }
        if (!done && *p == '(') {
            depth++;
        } else if (!done && *p == ')') {
            depth--;
        }
        if (done || (*p == '|' && depth == 0)) {
            size_t n = (size_t)(p - piece);
            char *alternative = strndup(piece, n);
            if (!alternative) {
                return false;
            }
            bool matched = shellPatternMatchEx(alternative, text, flags) == 0;
            free(alternative);
            if (matched) {
                return true;
            }
            piece = p + 1;
        }
    }
    return false;
}

/* op(alternatives)rest against text from here on. times is how many repeats so far. */
static bool shellExtglobMatchFrom(char op, const char *alt_start, const char *alt_end, const char *rest, char *work,
                                  size_t pos, size_t len, ShellPatternFlags flags, int times);

static bool shellExtglobRepeat(char op, const char *alt_start, const char *alt_end, const char *rest, char *work,
                               size_t pos, size_t len, ShellPatternFlags flags, int times) {
    bool may_stop = op == '*' || (op == '+' && times > 0) || (op == '?' );
    if (may_stop && shellPatternMatchEx(rest, work + pos, flags) == 0) {
        return true;
    }
    if (op == '?' && times > 0) {
        return false;
    }
    for (size_t end = pos + 1; end <= len; ++end) {
        char saved = work[end];
        work[end] = '\0';
        bool matched = shellExtglobAnyAlternative(alt_start, alt_end, work + pos, flags);
        work[end] = saved;
        if (matched && shellExtglobRepeat(op, alt_start, alt_end, rest, work, end, len, flags, times + 1)) {
            return true;
        }
    }
    return false;
}

static bool shellExtglobMatchFrom(char op, const char *alt_start, const char *alt_end, const char *rest, char *work,
                                  size_t pos, size_t len, ShellPatternFlags flags, int times) {
    if (op == '@') {
        for (size_t end = pos; end <= len; ++end) {
            char saved = work[end];
            work[end] = '\0';
            bool matched = shellExtglobAnyAlternative(alt_start, alt_end, work + pos, flags);
            work[end] = saved;
            if (matched && shellPatternMatchEx(rest, work + end, flags) == 0) {
                return true;
            }
        }
        return false;
    }
    if (op == '!') {
        for (size_t end = pos; end <= len; ++end) {
            char saved = work[end];
            work[end] = '\0';
            bool matched = shellExtglobAnyAlternative(alt_start, alt_end, work + pos, flags);
            work[end] = saved;
            if (!matched && shellPatternMatchEx(rest, work + end, flags) == 0) {
                return true;
            }
        }
        return false;
    }
    return shellExtglobRepeat(op, alt_start, alt_end, rest, work, pos, len, flags, times);
}

/* prefix op(alternatives) rest: try every place the prefix could end. */
static int shellExtglobMatch(const char *pattern, const char *open, const char *text, ShellPatternFlags flags) {
    const char *close = shellExtglobClose(open + 1);
    if (!close) {
        return FNM_NOMATCH;
    }
    char *prefix = strndup(pattern, (size_t)(open - pattern));
    char *work = strdup(text);
    if (!prefix || !work) {
        free(prefix);
        free(work);
        return FNM_NOMATCH;
    }
    size_t len = strlen(work);
    bool matched = false;
    for (size_t pos = 0; pos <= len && !matched; ++pos) {
        char saved = work[pos];
        work[pos] = '\0';
        bool prefix_ok = shellPatternMatchEx(prefix, work, flags) == 0;
        work[pos] = saved;
        if (prefix_ok) {
            matched = shellExtglobMatchFrom(*open, open + 2, close, close + 1, work, pos, len, flags, 0);
        }
    }
    free(prefix);
    free(work);
    return matched ? 0 : FNM_NOMATCH;
}

static int shellPatternMatchEx(const char *pattern, const char *text, ShellPatternFlags flags) {
    if (!pattern || !text) {
        return FNM_NOMATCH;
    }
    const char *extglob = shellExtglobFind(pattern);
    if (extglob) {
        return shellExtglobMatch(pattern, extglob, text, flags);
    }
    size_t text_len = strlen(text);
    size_t pattern_len = strlen(pattern);
    bool utf8_text = text_len > 0 && isValidUtf8Bytes(text, text_len);
    bool utf8_pattern = pattern_len > 0 && isValidUtf8Bytes(pattern, pattern_len);
    if (utf8_text && utf8_pattern) {
        int native_rc = shellUtf8GlobMatchInternal(pattern, text, flags);
        if (native_rc >= 0) {
            return native_rc > 0 ? 0 : FNM_NOMATCH;
        }
    }
    int fnm_flags = 0;
#ifdef FNM_CASEFOLD
    if (flags & SHELL_PATTERN_NOCASE) {
        fnm_flags |= FNM_CASEFOLD;
    }
#endif
    return fnmatch(pattern, text, fnm_flags);
}

static int shellPatternMatch(const char *pattern, const char *text) {
    return shellPatternMatchEx(pattern, text, SHELL_PATTERN_NONE);
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} ShellGlobMatchList;

static bool shellGlobPatternHasMeta(const char *pattern) {
    if (!pattern) {
        return false;
    }
    for (const char *cursor = pattern; *cursor; ++cursor) {
        if (*cursor == '*' || *cursor == '?' || *cursor == '[') {
            return true;
        }
    }
    return false;
}

static bool shellGlobSegmentAllowsDot(const char *segment) {
    return segment && segment[0] == '.';
}

static bool shellGlobAppendOwned(ShellGlobMatchList *list, char *value) {
    if (!list || !value) {
        return false;
    }
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity ? (list->capacity * 2) : 8;
        char **new_items = (char **)realloc(list->items, new_capacity * sizeof(char *));
        if (!new_items) {
            return false;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = value;
    return true;
}

static void shellGlobFreeList(ShellGlobMatchList *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static char *shellGlobJoinPath(const char *base, const char *component) {
    const char *lhs = base ? base : "";
    const char *rhs = component ? component : "";
    if (lhs[0] == '\0') {
        return strdup(rhs);
    }
    if (strcmp(lhs, "/") == 0) {
        size_t len = strlen(rhs);
        char *result = (char *)malloc(len + 2);
        if (!result) {
            return NULL;
        }
        result[0] = '/';
        memcpy(result + 1, rhs, len + 1);
        return result;
    }
    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    char *result = (char *)malloc(lhs_len + rhs_len + 2);
    if (!result) {
        return NULL;
    }
    memcpy(result, lhs, lhs_len);
    result[lhs_len] = '/';
    memcpy(result + lhs_len + 1, rhs, rhs_len + 1);
    return result;
}

static int shellGlobResultCompare(const void *lhs, const void *rhs) {
    const char *const *left = (const char *const *)lhs;
    const char *const *right = (const char *const *)rhs;
    const char *a = (left && *left) ? *left : "";
    const char *b = (right && *right) ? *right : "";
    return strcmp(a, b);
}

static int shellExpandUtf8GlobRecursive(const char *fs_base,
                                        const char *display_base,
                                        const char *pattern,
                                        ShellGlobMatchList *results,
                                        ShellPatternFlags flags) {
    if (!pattern || !results) {
        return GLOB_NOMATCH;
    }

    while (*pattern == '/') {
        pattern++;
    }
    if (*pattern == '\0') {
        const char *display = (display_base && *display_base) ? display_base : fs_base;
        if (!display || !*display) {
            display = ".";
        }
        char *dup = strdup(display);
        if (!dup) {
            return GLOB_NOSPACE;
        }
        if (!shellGlobAppendOwned(results, dup)) {
            free(dup);
            return GLOB_NOSPACE;
        }
        return 0;
    }

    const char *slash = strchr(pattern, '/');
    size_t segment_len = slash ? (size_t)(slash - pattern) : strlen(pattern);
    char *segment = (char *)malloc(segment_len + 1);
    if (!segment) {
        return GLOB_NOSPACE;
    }
    memcpy(segment, pattern, segment_len);
    segment[segment_len] = '\0';
    const char *rest = slash ? (slash + 1) : "";

    bool has_meta = shellGlobPatternHasMeta(segment);
    int rc = GLOB_NOMATCH;

    if (!has_meta) {
        char *next_fs = shellGlobJoinPath(fs_base, segment);
        char *next_display = shellGlobJoinPath(display_base, segment);
        if (!next_fs || !next_display) {
            free(segment);
            free(next_fs);
            free(next_display);
            return GLOB_NOSPACE;
        }
        struct stat st;
        if (stat(next_fs, &st) == 0) {
            rc = shellExpandUtf8GlobRecursive(next_fs, next_display, rest, results, flags);
        }
        free(next_fs);
        free(next_display);
        free(segment);
        return rc;
    }

    const char *dir_path = (fs_base && *fs_base) ? fs_base : ".";
    DIR *dir = opendir(dir_path);
    if (!dir) {
        free(segment);
        return GLOB_NOMATCH;
    }

    bool allow_dot = shellGlobSegmentAllowsDot(segment);
    struct dirent *dent = NULL;
    while ((dent = readdir(dir)) != NULL) {
        const char *name = dent->d_name;
        if (!name) {
            continue;
        }
        if (!allow_dot && name[0] == '.') {
            continue;
        }
        if (shellPatternMatchEx(segment, name, flags) != 0) {
            continue;
        }
        char *next_fs = shellGlobJoinPath(fs_base, name);
        char *next_display = shellGlobJoinPath(display_base, name);
        if (!next_fs || !next_display) {
            free(next_fs);
            free(next_display);
            closedir(dir);
            free(segment);
            return GLOB_NOSPACE;
        }
        /* A slash after a pattern takes only directories, and stays in what it matches. */
        if (slash && rest[strspn(rest, "/")] == '\0') {
            struct stat st;
            char *with_slash = NULL;
            if (stat(next_fs, &st) == 0 && S_ISDIR(st.st_mode)) {
                with_slash = shellGlobJoinPath(next_display, "");
            }
            free(next_fs);
            free(next_display);
            if (with_slash && shellGlobAppendOwned(results, with_slash)) {
                rc = 0;
            } else {
                free(with_slash);
            }
            continue;
        }
        int child_rc = shellExpandUtf8GlobRecursive(next_fs, next_display, rest, results, flags);
        if (child_rc == 0) {
            rc = 0;
        } else if (child_rc == GLOB_NOSPACE) {
            free(next_fs);
            free(next_display);
            closedir(dir);
            free(segment);
            return GLOB_NOSPACE;
        }
        free(next_fs);
        free(next_display);
    }

    closedir(dir);
    free(segment);
    return rc;
}

static int shellExpandUtf8GlobFallback(const char *pattern, char ***out_matches, size_t *out_count) {
    if (out_matches) {
        *out_matches = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!pattern || !*pattern) {
        return GLOB_NOMATCH;
    }

    const char *cursor = pattern;
    const char *fs_base = ".";
    const char *display_base = "";
    if (pattern[0] == '/') {
        while (*cursor == '/') {
            cursor++;
        }
        fs_base = "/";
        display_base = "/";
    }

    ShellGlobMatchList results = {0};
    ShellPatternFlags flags = shellShoptOptionEnabled("nocaseglob")
        ? SHELL_PATTERN_NOCASE
        : SHELL_PATTERN_NONE;
    int rc = shellExpandUtf8GlobRecursive(fs_base, display_base, cursor, &results, flags);
    if (rc != 0) {
        shellGlobFreeList(&results);
        return rc;
    }
    if (results.count == 0) {
        shellGlobFreeList(&results);
        return GLOB_NOMATCH;
    }
    qsort(results.items, results.count, sizeof(char *), shellGlobResultCompare);
    if (out_matches) {
        *out_matches = results.items;
    } else {
        shellGlobFreeList(&results);
    }
    if (out_count) {
        *out_count = results.count;
    }
    return 0;
}

static int shellExpandGlobPattern(const char *pattern, char ***out_matches, size_t *out_count) {
    if (out_matches) {
        *out_matches = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!pattern || !*pattern) {
        return GLOB_NOMATCH;
    }

    bool nocaseglob = shellShoptOptionEnabled("nocaseglob");
    glob_t glob_result;
    memset(&glob_result, 0, sizeof(glob_result));
    int glob_status = GLOB_NOMATCH;
    if (!nocaseglob) {
        glob_status =
#if defined(PSCAL_TARGET_IOS)
            pscalPathVirtualized_glob(pattern, 0, NULL, &glob_result);
#else
            glob(pattern, 0, NULL, &glob_result);
#endif
    }
    if (glob_status == 0) {
        char **matches = (char **)calloc(glob_result.gl_pathc, sizeof(char *));
        if (!matches) {
            globfree(&glob_result);
            return GLOB_NOSPACE;
        }
        size_t count = 0;
        for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
            matches[count] = strdup(glob_result.gl_pathv[i]);
            if (!matches[count]) {
                for (size_t j = 0; j < count; ++j) {
                    free(matches[j]);
                }
                free(matches);
                globfree(&glob_result);
                return GLOB_NOSPACE;
            }
            count++;
        }
        globfree(&glob_result);
        if (out_matches) {
            *out_matches = matches;
        } else {
            for (size_t i = 0; i < count; ++i) {
                free(matches[i]);
            }
            free(matches);
        }
        if (out_count) {
            *out_count = count;
        }
        return 0;
    }
    if (glob_status != GLOB_NOMATCH) {
        return glob_status;
    }

    size_t pattern_len = strlen(pattern);
    if (!isValidUtf8Bytes(pattern, pattern_len)) {
        return GLOB_NOMATCH;
    }
#if !defined(PSCAL_TARGET_IOS)
    /* glob(3) counts bytes and the fallback characters, which only `?` and a bracket can tell
     * apart. Plain `*` and ASCII: it has read the directory, and the answer is no. */
    if (!nocaseglob && !strpbrk(pattern, "?[\\")) {
        bool ascii = true;
        for (const char *cursor = pattern; *cursor && ascii; ++cursor) {
            ascii = (unsigned char)*cursor < 0x80;
        }
        if (ascii) {
            return GLOB_NOMATCH;
        }
    }
#endif
    return shellExpandUtf8GlobFallback(pattern, out_matches, out_count);
}

static void shellBufferAppendChar(char **buffer, size_t *length, size_t *capacity, char c);
static void shellBufferAppendString(char **buffer, size_t *length, size_t *capacity, const char *str);
static char *shellExpandParameter(const char *input,
                                  size_t *out_consumed,
                                  bool *out_is_array_expansion,
                                  size_t *out_array_count);

static char *shellRemovePatternSuffix(const char *value, const char *pattern, bool longest) {
    if (!value) {
        return strdup("");
    }
    if (!pattern || !*pattern) {
        return strdup(value);
    }
    size_t len = strlen(value);
    size_t start = longest ? 0 : len;
    bool first_literal = !strchr("*?[\\", pattern[0]);
    while (true) {
        if ((start == len || shellIsCharBoundary(value, start)) && (!first_literal || value[start] == pattern[0])) {
            if (shellPatternMatch(pattern, value + start) == 0) {
                char *out = (char *)malloc(start + 1);
                if (out) {
                    memcpy(out, value, start);
                    out[start] = '\0';
                }
                return out;
            }
        }
        if (longest ? start == len : start == 0) {
            break;
        }
        start += longest ? 1 : (size_t)-1;
    }
    return strdup(value);
}

typedef enum {
    SHELL_RUNTIME_REDIR_OPEN,
    SHELL_RUNTIME_REDIR_DUP,
    SHELL_RUNTIME_REDIR_HEREDOC,
    SHELL_RUNTIME_REDIR_HERE_STRING
} ShellRuntimeRedirectionKind;

typedef struct {
    int fd;
    ShellRuntimeRedirectionKind kind;
    int flags;
    mode_t mode;
    char *path;
    int dup_target_fd;
    bool close_target;
    char *here_doc;
    size_t here_doc_length;
    bool here_doc_quoted;
    char *here_string_literal;
    char *here_string;
    size_t here_string_length;
    /* Fails when applied, in its turn among the command's redirections. */
    bool ambiguous;
    /* {name}> f: the shell picks the descriptor from 10 up and name gets its number. fd is -1. */
    char *fd_var;
    /* The whole redir: text, for a word that expands when this redirection's turn comes. */
    char *takes_turn;
} ShellRedirection;

typedef struct {
    char *text;
    bool is_array_literal;
} ShellAssignmentEntry;

#if defined(PSCAL_TARGET_IOS)
/* What a subshell starts with, taken on the shell's thread when it is started, because
 * the thread that runs it may start after the shell has moved on. */
typedef struct {
    ShellRuntimeState *ctx;
    VProcEnv *env;
    char **params;
    int param_count;
    char cwd[PATH_MAX];
    pid_t dollar_pid;
    struct ShellVariableAttribute *attributes;
    size_t attribute_count;
    bool ifs_unset;
    char *err_trap;
    char *return_trap;
    bool traps_dormant_err;
    bool traps_dormant_return;
    ShellCallStack calls;
    bool in_loop;
    bool lists_jobs;
} ShellBodyState;

/* What a thread started by a shell takes from it, beyond the vproc every new thread starts
 * under. Outside a subshell that is only interactive. */
typedef struct {
    bool interactive;
    ShellRuntimeState *ctx;
    VProcEnv *env;
    int subshell_pid;
    int return_scope; /* this one always */
} ShellThreadLink;
#endif

typedef struct {
    char **argv;
    size_t argc;
    ShellAssignmentEntry *assignments;
    size_t assignment_count;
    ShellRedirection *redirs;
    size_t redir_count;
    bool background;
    bool disable_field_splitting;
    bool ignore_functions;
    bool ran_command_substitution;
    int empty_stage_status;
    int pipeline_index;
    bool is_pipeline_head;
    bool is_pipeline_tail;
    int line;
    int column;
    /* A compound command run apart from the shell; argv[0] only names it. */
    ShellCompiledFunction *body;
    /* A body holding one simple job, whose words had to expand in it. */
    bool body_is_job;
    /* A simple command that is all a body does, so it becomes the job or the stage. */
    bool job_tail;
    /* It is a job on its own: its words see the stdin it was started with, and it does not. */
    bool job_stdin_null;
#if defined(PSCAL_TARGET_IOS)
    ShellBodyState *body_state;
    /* {name}> f on a stage or job with a state of its own: name goes there, once it has one. */
    struct {
        char *name;
        int fd;
    } *stage_fd_vars;
    size_t stage_fd_var_count;
    /* What it gets if it turns out to be a program, and the file that would be: taken while
     * the shell's thread launched it, when what the command sets for itself is in place. */
    VProcEnv *program_env;
    char *program_path;
#else
    /* What a substitution's exec asked of its program: -a or -l, and -c. */
    char *exec_arg0;
    bool exec_clear_env;
#endif
} ShellCommand;

#if defined(PSCAL_TARGET_IOS)
static void shellBodyStateFree(ShellBodyState *state);
#endif

typedef enum {
    SHELL_LOOP_KIND_WHILE,
    SHELL_LOOP_KIND_UNTIL,
    SHELL_LOOP_KIND_FOR,
    SHELL_LOOP_KIND_CFOR
} ShellLoopKind;

typedef enum {
    SHELL_LOOP_CONDITION_NONE = 0,
    SHELL_LOOP_CONDITION_TEST = 1,
    SHELL_LOOP_CONDITION_BRACKET = 2,
    SHELL_LOOP_CONDITION_COLON = 3,
    SHELL_LOOP_CONDITION_TRUE = 4,
    SHELL_LOOP_CONDITION_FALSE = 5,
    SHELL_LOOP_CONDITION_ARITH = 6
} ShellLoopConditionKind;

typedef enum {
    SHELL_LOOP_BODY_KIND_NONE = 0,
    SHELL_LOOP_BODY_KIND_COLON,
    SHELL_LOOP_BODY_KIND_TRUE,
    SHELL_LOOP_BODY_KIND_FALSE,
    SHELL_LOOP_BODY_KIND_TEST,
    SHELL_LOOP_BODY_KIND_BRACKET,
    SHELL_LOOP_BODY_KIND_ARITH,
    SHELL_LOOP_BODY_KIND_TEST_ARITH,
    SHELL_LOOP_BODY_KIND_BRACKET_ARITH
} ShellLoopBodyKind;

typedef struct {
    char *name;
    char *previous_value;
    bool had_previous;
    bool previous_was_array;
    bool was_exported;
    bool was_pending_export;
    bool previous_no_value;
} ShellAssignmentBackup;

static bool shellGlobalIsExported(const char *name);
static bool shellVariableAllExportEnabled(void);

typedef struct {
    char *name;
    char *value;
    bool is_array_literal;
    /* A local exports on its own: the global of that name keeps its own state. */
    bool exported;
    /* `local x`, or `unset x` in the function that made it: no value, and any outer one
     * stays hidden until the function returns. */
    bool no_value;
    /* An array of that name is waiting behind it. */
    bool shadows_array;
    bool shadows_associative;
    /* declare -i, -l and -u on it. */
    unsigned attributes;
} ShellLocalVariable;

typedef struct ShellLocalScopeFrame {
    ShellLocalVariable *variables;
    size_t count;
    size_t capacity;
} ShellLocalScopeFrame;

static void shellLocalVariableDispose(ShellLocalVariable *var) {
    if (!var) {
        return;
    }
    free(var->name);
    free(var->value);
    var->name = NULL;
    var->value = NULL;
    var->is_array_literal = false;
    var->exported = false;
    var->no_value = false;
    var->shadows_array = false;
    var->shadows_associative = false;
    var->attributes = 0;
}

static void shellLocalScopeFrameReset(ShellLocalScopeFrame *frame) {
    if (!frame) {
        return;
    }
    for (size_t i = 0; i < frame->count; ++i) {
        shellLocalVariableDispose(&frame->variables[i]);
    }
    free(frame->variables);
    frame->variables = NULL;
    frame->count = 0;
    frame->capacity = 0;
}

static bool shellLocalScopeEnsureCapacity(ShellLocalScopeFrame *frame, size_t needed) {
    if (!frame) {
        return false;
    }
    if (frame->capacity >= needed) {
        return true;
    }
    size_t new_capacity = frame->capacity ? frame->capacity * 2 : 4;
    if (new_capacity < needed) {
        new_capacity = needed;
    }
    if (new_capacity > SIZE_MAX / sizeof(ShellLocalVariable)) {
        return false;
    }
    ShellLocalVariable *resized =
        (ShellLocalVariable *)realloc(frame->variables, new_capacity * sizeof(ShellLocalVariable));
    if (!resized) {
        return false;
    }
    for (size_t i = frame->capacity; i < new_capacity; ++i) {
        resized[i].name = NULL;
        resized[i].value = NULL;
        resized[i].is_array_literal = false;
    }
    frame->variables = resized;
    frame->capacity = new_capacity;
    return true;
}

static ShellLocalVariable *shellLocalScopeFrameFind(ShellLocalScopeFrame *frame, const char *name) {
    if (!frame || !name) {
        return NULL;
    }
    for (size_t i = 0; i < frame->count; ++i) {
        ShellLocalVariable *var = &frame->variables[i];
        if (var->name && strcmp(var->name, name) == 0) {
            return var;
        }
    }
    return NULL;
}

static ShellLocalVariable *shellLocalScopeFind(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t depth = gShellLocalScopeDepth; depth > 0; --depth) {
        ShellLocalScopeFrame *frame = &gShellLocalScopeStack[depth - 1];
        ShellLocalVariable *found = shellLocalScopeFrameFind(frame, name);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static ShellLocalVariable *shellLocalScopeFindInOtherFrames(const char *name,
                                                           const ShellLocalScopeFrame *exclude) {
    if (!name) {
        return NULL;
    }
    for (size_t depth = gShellLocalScopeDepth; depth > 0; --depth) {
        ShellLocalScopeFrame *frame = &gShellLocalScopeStack[depth - 1];
        if (frame == exclude) {
            continue;
        }
        ShellLocalVariable *found = shellLocalScopeFrameFind(frame, name);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static void shellArrayRegistryRestoreVisibleValue(const char *name,
                                                  const ShellLocalScopeFrame *excluded_frame) {
    if (!name) {
        return;
    }
    ShellLocalVariable *shadow = excluded_frame ? shellLocalScopeFindInOtherFrames(name, excluded_frame)
                                                : shellLocalScopeFind(name);
    if (shadow && shadow->is_array_literal) {
        const char *text = shadow->value ? shadow->value : "";
        shellArrayRegistryAssignFromText(name, text);
        return;
    }
    const char *env_text = shellSafeGetenv(name);
    if (env_text) {
        shellArrayRegistryAssignFromText(name, env_text);
    } else {
        shellArrayRegistryRemove(name);
    }
}

static bool shellLocalScopePush(void) {
    if (gShellLocalScopeDepth == gShellLocalScopeCapacity) {
        size_t new_capacity = gShellLocalScopeCapacity ? gShellLocalScopeCapacity * 2 : 4;
        if (new_capacity > SIZE_MAX / sizeof(ShellLocalScopeFrame)) {
            return false;
        }
        ShellLocalScopeFrame *resized =
            (ShellLocalScopeFrame *)realloc(gShellLocalScopeStack, new_capacity * sizeof(ShellLocalScopeFrame));
        if (!resized) {
            return false;
        }
        for (size_t i = gShellLocalScopeCapacity; i < new_capacity; ++i) {
            resized[i].variables = NULL;
            resized[i].count = 0;
            resized[i].capacity = 0;
        }
        gShellLocalScopeStack = resized;
        gShellLocalScopeCapacity = new_capacity;
    }
    ShellLocalScopeFrame *frame = &gShellLocalScopeStack[gShellLocalScopeDepth];
    frame->variables = NULL;
    frame->count = 0;
    frame->capacity = 0;
    gShellLocalScopeDepth++;
    return true;
}

static void shellLocalScopePop(void) {
    if (gShellLocalScopeDepth == 0) {
        return;
    }
    ShellLocalScopeFrame *frame = &gShellLocalScopeStack[gShellLocalScopeDepth - 1];
    for (size_t i = 0; i < frame->count; ++i) {
        ShellLocalVariable *var = &frame->variables[i];
        if (var->name && (var->is_array_literal || var->shadows_array)) {
            shellArrayRegistryRemove(var->name);
            shellArrayRegistryRestoreVisibleValue(var->name, frame);
            /* Its text says what it is unless it has nothing in it. */
            const ShellArrayVariable *restored = shellArrayRegistryFindConst(var->name);
            if (var->shadows_associative && restored && restored->kind != SHELL_ARRAY_KIND_ASSOCIATIVE) {
                shellArrayRegistryInitializeAssociative(var->name);
            }
        }
    }
    shellLocalScopeFrameReset(frame);
    gShellLocalScopeDepth--;
}

static size_t shellLocalScopeDepth(void) {
    return gShellLocalScopeDepth;
}

static bool shellLocalScopeSetOnCurrentFrame(const char *name, const char *value, bool is_array_literal) {
    if (!name || gShellLocalScopeDepth == 0) {
        return false;
    }
    ShellLocalScopeFrame *frame = &gShellLocalScopeStack[gShellLocalScopeDepth - 1];
    ShellLocalVariable *existing = shellLocalScopeFrameFind(frame, name);
    bool created_new = false;
    if (!existing) {
        /* A local of an exported name stays exported, as under bash. */
        ShellLocalVariable *outer = shellLocalScopeFind(name);
        const ShellArrayVariable *shadowed = shellArrayRegistryFindConst(name);
        bool exported = shellVariableAllExportEnabled() ||
                        (outer ? outer->exported : shellGlobalIsExported(name));
        if (!shellLocalScopeEnsureCapacity(frame, frame->count + 1)) {
            return false;
        }
        existing = &frame->variables[frame->count++];
        existing->name = strdup(name);
        if (!existing->name) {
            frame->count--;
            return false;
        }
        existing->value = NULL;
        existing->is_array_literal = false;
        existing->exported = exported;
        existing->no_value = false;
        existing->shadows_array = shadowed != NULL;
        existing->shadows_associative = shadowed && shadowed->kind == SHELL_ARRAY_KIND_ASSOCIATIVE && !shadowed->sparse;
        existing->attributes = 0;
        created_new = true;
    }
    char *copy = strdup(value ? value : "");
    if (!copy) {
        if (created_new) {
            free(existing->name);
            existing->name = NULL;
            frame->count--;
        }
        return false;
    }
    free(existing->value);
    existing->value = copy;
    existing->is_array_literal = is_array_literal;
    existing->no_value = false;
    if (is_array_literal) {
        const char *text = existing->value ? existing->value : "";
        shellArrayRegistryAssignFromText(existing->name, text);
    } else {
        shellArrayRegistryRemove(existing->name);
    }
    return true;
}

static bool shellLocalScopeAssignExisting(const char *name, const char *value, bool is_array_literal) {
    ShellLocalVariable *existing = shellLocalScopeFind(name);
    if (!existing) {
        return false;
    }
    char *copy = strdup(value ? value : "");
    if (!copy) {
        return false;
    }
    free(existing->value);
    existing->value = copy;
    existing->is_array_literal = is_array_literal;
    existing->no_value = false;
    if (shellVariableAllExportEnabled()) {
        existing->exported = true;
    }
    return true;
}

static bool shellLocalScopeGetCopy(const char *name, char **out_value, bool *out_is_array) {
    if (out_value) {
        *out_value = NULL;
    }
    if (out_is_array) {
        *out_is_array = false;
    }
    ShellLocalVariable *existing = shellLocalScopeFind(name);
    if (!existing) {
        return false;
    }
    if (out_value) {
        *out_value = strdup(existing->value ? existing->value : "");
        if (!*out_value) {
            return false;
        }
    }
    if (out_is_array) {
        *out_is_array = existing->is_array_literal;
    }
    return true;
}

static bool shellLocalScopeUnset(const char *name) {
    if (!name) {
        return false;
    }
    for (size_t depth = gShellLocalScopeDepth; depth > 0; --depth) {
        ShellLocalScopeFrame *frame = &gShellLocalScopeStack[depth - 1];
        for (size_t i = 0; i < frame->count; ++i) {
            ShellLocalVariable *var = &frame->variables[i];
            if (var->name && strcmp(var->name, name) == 0) {
                bool was_array = var->is_array_literal;
                if (was_array) {
                    shellArrayRegistryRemove(var->name);
                }
                /* In the function that made it, the name stays local: what is outside
                 * comes back only when the function returns. From a function it called,
                 * it goes, and the next one out shows. */
                if (depth == gShellLocalScopeDepth) {
                    free(var->value);
                    var->value = NULL;
                    var->shadows_array = var->shadows_array || was_array;
                    var->is_array_literal = false;
                    var->exported = false;
                    var->no_value = true;
                    var->attributes = 0;
                    return true;
                }
                shellLocalVariableDispose(var);
                if (i + 1 < frame->count) {
                    memmove(&frame->variables[i], &frame->variables[i + 1],
                            (frame->count - i - 1) * sizeof(ShellLocalVariable));
                }
                frame->count--;
                if (frame->count < frame->capacity) {
                    frame->variables[frame->count].name = NULL;
                    frame->variables[frame->count].value = NULL;
                    frame->variables[frame->count].is_array_literal = false;
                    frame->variables[frame->count].exported = false;
                    frame->variables[frame->count].no_value = false;
                    frame->variables[frame->count].shadows_array = false;
                    frame->variables[frame->count].attributes = 0;
                }
                if (was_array) {
                    shellArrayRegistryRestoreVisibleValue(name, NULL);
                }
                return true;
            }
        }
    }
    return false;
}

typedef struct {
    int target_fd;
    int saved_fd;
    bool saved_valid;
    bool was_closed;
    bool use_vproc;
    /* Slot in the current vproc's fd table holding a dup of the entry as it
     * was before the redirection. The host descriptor alone is not enough on
     * iOS: a session's stdio can be a PTY-backed pscal_fd that has no
     * meaningful host-fd equivalent, so restoring only the host side leaves
     * the vproc pointing at the redirection target. -1 when unused. */
    int vproc_saved_fd;
    /* iOS, above stdio: the slot is the whole descriptor. Nothing of the host's
     * was saved, and an entry that was closed before goes back to closed. */
    bool slot_only;
    /* iOS, a stdio slot with nothing in it, which means closed. The host's descriptor of that
     * number goes back to what it was, and the slot goes back to empty: it must not become a
     * copy of the host's. */
    bool slot_empty;
} ShellExecRedirBackup;

/* A command substitution runs in the shell's own process, because `tid=$(builtin
 * ThreadSpawnBuiltin ...)` needs the thread to live here. So what a subshell would take
 * with it when it ends has to be put back by hand. One of these for each substitution
 * running on this thread, innermost first. */
typedef struct ShellCommandSubFrame {
    struct ShellCommandSubFrame *outer;
    int read_fd;
    int saved_stdout;
    /* `exec 3> f` in there lasts until the substitution ends. */
    ShellExecRedirBackup *exec_backups;
    size_t exec_backup_count;
    size_t exec_backup_capacity;
    /* Background jobs started in there, which are no job of the shell's afterwards. */
    pid_t *job_pids;
    size_t job_pid_count;
    size_t job_pid_capacity;
#if defined(PSCAL_TARGET_IOS)
    VProc *vp;
#endif
    /* Its context has dropped what bash's fork would. */
    bool jobs_trimmed;
} ShellCommandSubFrame;

static PSCAL_THREAD_LOCAL ShellCommandSubFrame *gShellCommandSubFrame = NULL;

static void shellRestoreExecRedirections(ShellExecRedirBackup *backups, size_t count);
static void shellFreeExecRedirBackups(ShellExecRedirBackup *backups, size_t count);
static void shellFreeRedirections(ShellCommand *cmd);
static void shellAbortPipeline(void);
static void shellResetPipeline(void);

typedef struct ShellLoopFrame {
    ShellLoopKind kind;
    bool skip_body;
    bool break_pending;
    bool continue_pending;
    char *for_variable;
    char **for_values;
    size_t for_count;
    size_t for_index;
    bool for_active;
    char *cfor_init;
    char *cfor_condition;
    char *cfor_update;
    bool cfor_condition_cached;
    bool cfor_condition_value;
    ShellLoopConditionKind cond_kind;
    char **cond_words;
    size_t cond_word_count;
    char *cond_arith_expression;
    ShellLoopBodyKind body_kind;
    char **body_words;
    size_t body_word_count;
    char *body_arith_expression;
    bool redirs_active;
    ShellRedirection *applied_redirs;
    size_t applied_redir_count;
    ShellExecRedirBackup *redir_backups;
    size_t redir_backup_count;
    size_t redir_backup_capacity;
    int status; /* the loop's status: its last body command's, 0 if the body never ran */
    bool keep_status; /* started while skipping, so nothing ran and nothing changes */
    size_t scope_depth; /* redirection scopes open when it started */
} ShellLoopFrame;

#define gShellLoopStack (SC()->loop_stack)
#define gShellLoopStackSize (SC()->loop_stack_size)
#define gShellLoopStackCapacity (SC()->loop_stack_capacity)

static ShellRuntimeState gShellRuntimeDefault = {
    .last_status = 0,
    .pipeline = {0},
    .shell_pgid = 0,
    .last_bg_pid = 0,
    .tty_fd = -1,
    .job_control_enabled = false,
    .job_control_initialized = false,
    .job_control_caps = {false, false, false, false, false, false},
    .job_control_ctx = {0, 0, false, {-1, -1, -1}, false, {0}},
    .errexit_enabled = false,
    .errexit_pending = false,
    .errexit_defer_active = false,
    .trap_enabled = false,
    .posix_mode = false,
    .local_scope_active = false,
    .break_requested = false,
    .continue_requested = false,
    .pipeline_subshell_active = false,
    .break_requested_levels = 0,
    .continue_requested_levels = 0,
    .dir_stack = NULL,
    .dir_stack_count = 0,
    .dir_stack_capacity = 0,
    .dir_stack_initialised = false,
    .hash_entries = NULL,
    .hash_count = 0,
    .hash_capacity = 0,
    .disabled_builtins = NULL,
    .disabled_builtin_count = 0,
    .disabled_builtin_capacity = 0,
    .getopts_index = 1,
    .getopts_offset = 0,
    .scalar_cache_hits = 0,
    .scalar_cache_misses = 0,
    .scalar_cache_stores = 0,
    .scalar_cache_generation = 0,
    .scalar_capture_peak_depth = 0,
    .history = {NULL, 0, 0},
    .history_path = NULL,
    .history_size_limit = 0,
    .history_file_size_limit = 0,
    .history_persist_enabled = false,
    .history_append_mode = true,
    .history_loaded_from_file = false,
    .history_loading = false,
    .arg0 = NULL,
    .signal_traps = { {SHELL_TRAP_ACTION_DEFAULT, NULL} },
    .exit_trap = {SHELL_TRAP_ACTION_DEFAULT, NULL},
    .trap_executing = false,
    .script_depth = 0,
    .condition_depth = 0,
    .job_next_id = 1,
    .jobs = NULL,
    .job_count = 0,
    .array_vars = NULL,
    .array_var_count = 0,
    .array_var_capacity = 0,
    .associative_array_support = -1,
    .bind_interactive_status = -1,
    .readonly_vars = NULL,
    .readonly_var_count = 0,
    .readonly_var_capacity = 0,
    .readonly_error_name = NULL
};

/* Default to the global runtime state to avoid pathological recursion when
 * shellGetContext() is first called before an explicit activation. */
static __thread ShellRuntimeState *gShellRuntimeCtx = &gShellRuntimeDefault;
/* One entry per builtin call and script in progress, so as deep as a script's calls go. It
 * was 8, and a pop past that restored the wrong context: nested calls read the wrong $?. */
static __thread ShellRuntimeState **gShellContextStack = NULL;
static __thread size_t gShellContextCapacity = 0;
static __thread size_t gShellContextDepth = 0;
static __thread bool gShellCurrentBuiltinBackground = false;
static __thread bool gShellBackgroundWorkerActive = false;
static __thread bool gShellSkipHostStdio = false;
/* Per-thread to avoid cross-session stdio restoration on iOS. */
static __thread int gShellSavedStdFds[3] = { -1, -1, -1 };

#define gShellRuntime (*shellGetContext())

static bool shellVariableAllExportEnabled(void) {
    return gShellRuntime.allexport_enabled;
}

static unsigned long gShellStatusVersion = 0;
static PSCAL_THREAD_LOCAL bool gShellStatusSticky = false;

static PSCAL_THREAD_LOCAL bool gShellExitRequested = false;
static volatile sig_atomic_t gShellExitOnSignalFlag = 0;
static PSCAL_THREAD_LOCAL bool gShellArithmeticErrorPending = false;
static PSCAL_THREAD_LOCAL bool gShellExpansionFailed = false;
static PSCAL_THREAD_LOCAL bool gShellExpandingJobWords = false;
static PSCAL_THREAD_LOCAL bool gShellNounsetSuppressed = false;
static PSCAL_THREAD_LOCAL bool gShellProcSubDisabled = false;

/* The stages of the last pipeline, or the one status of a plain command: ${PIPESTATUS[@]}. */
#define SHELL_PIPESTATUS_MAX 128
static PSCAL_THREAD_LOCAL int gShellPipeStatus[SHELL_PIPESTATUS_MAX];
static PSCAL_THREAD_LOCAL size_t gShellPipeStatusCount = 0;

/* Innermost first: ${BASH_SOURCE[0]}. */
static PSCAL_THREAD_LOCAL char **gShellSourceNames = NULL;
static PSCAL_THREAD_LOCAL size_t gShellSourceNameCount = 0;
static PSCAL_THREAD_LOCAL bool gShellIfsUnset = false;
static PSCAL_THREAD_LOCAL bool gShellPipeStatusFresh = false;
static PSCAL_THREAD_LOCAL bool gShellReturnPending = false;
/* What $? was when `return` ran: a RETURN trap sees that, and the caller sees its status. */
static PSCAL_THREAD_LOCAL int gShellReturnPriorStatus = 0;
/* `return 1 2` and the like: everything running goes, down to what the shell last read. */
static PSCAL_THREAD_LOCAL bool gShellDiscardPending = false;
static PSCAL_THREAD_LOCAL bool gShellDiscardInText = false; /* eval and source read on, for arithmetic */
/* $? when the running trap broke in, for a bare `return` in its text. */
static PSCAL_THREAD_LOCAL bool gShellTrapStatusValid = false;
static PSCAL_THREAD_LOCAL int gShellTrapStatus = 0;
/* The call that just ended failed, and its ERR trap was set during it: that one does not answer
 * for it. errexit still does. */
static PSCAL_THREAD_LOCAL bool gShellErrTrapSkipOnce = false;
/* A thread started for a stage or a job may have no copy of the context it started from, and
 * `return` there ends the stage: this says whether there was a call around it. */
static PSCAL_THREAD_LOCAL int gShellThreadReturnScope = 0;
static PSCAL_THREAD_LOCAL int gShellReturnStatus = 0;
static PSCAL_THREAD_LOCAL char *gShellLastWord = NULL;
static PSCAL_THREAD_LOCAL time_t gShellSecondsBase = 0;

/* Innermost function first: ${FUNCNAME[0]}. */
static PSCAL_THREAD_LOCAL char **gShellFunctionNames = NULL;
static PSCAL_THREAD_LOCAL size_t gShellFunctionNameCount = 0;
static PSCAL_THREAD_LOCAL size_t gShellFunctionNameCapacity = 0;

/* Every shell variable lives in the environment, so a program would see them all. These
 * are the ones bash would keep to itself. They belong to the environment they describe: a
 * process has one, and on iOS so does each subshell and each program, which have no
 * process of their own. */
typedef struct {
    char **names;
    size_t count;
    size_t capacity;
} ShellNameSet;
typedef struct {
    ShellNameSet unexported;
    /* `export x` before x has a value. */
    ShellNameSet pending;
} ShellExportFlags;
static ShellExportFlags gShellProcessExportFlags = {{0}};
#if defined(PSCAL_TARGET_IOS)
/* A stage's thread shares its shell's environment, and a builtin there may ask. */
static pthread_mutex_t gShellExportMutex = PTHREAD_MUTEX_INITIALIZER;
#define SHELL_EXPORT_LOCK() pthread_mutex_lock(&gShellExportMutex)
#define SHELL_EXPORT_UNLOCK() pthread_mutex_unlock(&gShellExportMutex)
#else
#define SHELL_EXPORT_LOCK() ((void)0)
#define SHELL_EXPORT_UNLOCK() ((void)0)
#endif

static bool shellNameSetContains(const ShellNameSet *set, const char *name) {
    for (size_t i = 0; i < set->count; ++i) {
        if (strcmp(set->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void shellNameSetAdd(ShellNameSet *set, const char *name) {
    if (shellNameSetContains(set, name)) {
        return;
    }
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2 : 16;
        char **grown = (char **)realloc(set->names, capacity * sizeof(char *));
        if (!grown) {
            return;
        }
        set->names = grown;
        set->capacity = capacity;
    }
    char *copy = strdup(name);
    if (copy) {
        set->names[set->count++] = copy;
    }
}

static bool shellNameSetRemove(ShellNameSet *set, const char *name) {
    for (size_t i = 0; i < set->count; ++i) {
        if (strcmp(set->names[i], name) == 0) {
            free(set->names[i]);
            set->names[i] = set->names[--set->count];
            return true;
        }
    }
    return false;
}

static void shellNameSetClear(ShellNameSet *set) {
    for (size_t i = 0; i < set->count; ++i) {
        free(set->names[i]);
    }
    free(set->names);
    memset(set, 0, sizeof(*set));
}

static void shellNameSetCopy(ShellNameSet *dst, const ShellNameSet *src) {
    for (size_t i = 0; i < src->count; ++i) {
        shellNameSetAdd(dst, src->names[i]);
    }
}

static void shellExportFlagsFree(void *data) {
    ShellExportFlags *flags = (ShellExportFlags *)data;
    if (flags) {
        shellNameSetClear(&flags->unexported);
        shellNameSetClear(&flags->pending);
        free(flags);
    }
}

/* Under SHELL_EXPORT_LOCK. */
static ShellExportFlags *shellExportFlagsCurrent(void) {
#if defined(PSCAL_TARGET_IOS)
    VProcEnv *env = vprocEnvCurrent();
    if (env) {
        ShellExportFlags *flags = (ShellExportFlags *)vprocEnvAttachment(env);
        if (!flags) {
            flags = (ShellExportFlags *)calloc(1, sizeof(ShellExportFlags));
            if (flags) {
                vprocEnvSetAttachment(env, flags, shellExportFlagsFree);
            }
        }
        if (flags) {
            return flags;
        }
    }
#endif
    return &gShellProcessExportFlags;
}

/* What this environment says now, for shellExportFlagsRestore. */
static ShellExportFlags *shellExportFlagsSnapshot(void) {
    ShellExportFlags *copy = (ShellExportFlags *)calloc(1, sizeof(ShellExportFlags));
    if (copy) {
        SHELL_EXPORT_LOCK();
        ShellExportFlags *flags = shellExportFlagsCurrent();
        shellNameSetCopy(&copy->unexported, &flags->unexported);
        shellNameSetCopy(&copy->pending, &flags->pending);
        SHELL_EXPORT_UNLOCK();
    }
    return copy;
}

/* A shell that starts in this environment has exported all of it. */
static void shellExportFlagsClearCurrent(void) {
    SHELL_EXPORT_LOCK();
    ShellExportFlags *flags = shellExportFlagsCurrent();
    shellNameSetClear(&flags->unexported);
    shellNameSetClear(&flags->pending);
    SHELL_EXPORT_UNLOCK();
}

static void shellExportFlagsRestore(ShellExportFlags *snapshot) {
    if (!snapshot) {
        return;
    }
    SHELL_EXPORT_LOCK();
    ShellExportFlags *flags = shellExportFlagsCurrent();
    ShellExportFlags previous = *flags;
    *flags = *snapshot;
    *snapshot = previous;
    SHELL_EXPORT_UNLOCK();
    shellExportFlagsFree(snapshot);
}

static bool shellExportIsHidden(const char *name) {
    SHELL_EXPORT_LOCK();
    bool hidden = shellNameSetContains(&shellExportFlagsCurrent()->unexported, name);
    SHELL_EXPORT_UNLOCK();
    return hidden;
}

static bool shellExportIsPending(const char *name) {
    SHELL_EXPORT_LOCK();
    bool pending = shellNameSetContains(&shellExportFlagsCurrent()->pending, name);
    SHELL_EXPORT_UNLOCK();
    return pending;
}

/* The global of that name, whatever a local may hide it with. */
static bool shellGlobalIsExported(const char *name) {
    if (shellSafeGetenv(name)) {
        return !shellExportIsHidden(name);
    }
    return shellExportIsPending(name);
}

static void shellExportNoteNewVariable(const char *name) {
    SHELL_EXPORT_LOCK();
    ShellExportFlags *flags = shellExportFlagsCurrent();
    if (!shellNameSetRemove(&flags->pending, name)) {
        shellNameSetAdd(&flags->unexported, name);
    }
    SHELL_EXPORT_UNLOCK();
}

static void shellExportMarkExported(const char *name) {
    bool is_set = shellSafeGetenv(name) != NULL;
    SHELL_EXPORT_LOCK();
    ShellExportFlags *flags = shellExportFlagsCurrent();
    if (!shellNameSetRemove(&flags->unexported, name) && !is_set) {
        shellNameSetAdd(&flags->pending, name);
    }
    SHELL_EXPORT_UNLOCK();
}

static void shellExportMarkUnexported(const char *name) {
    bool is_set = shellSafeGetenv(name) != NULL;
    SHELL_EXPORT_LOCK();
    ShellExportFlags *flags = shellExportFlagsCurrent();
    shellNameSetRemove(&flags->pending, name);
    if (is_set) {
        shellNameSetAdd(&flags->unexported, name);
    }
    SHELL_EXPORT_UNLOCK();
}

static void shellExportForget(const char *name) {
    SHELL_EXPORT_LOCK();
    ShellExportFlags *flags = shellExportFlagsCurrent();
    shellNameSetRemove(&flags->pending, name);
    shellNameSetRemove(&flags->unexported, name);
    SHELL_EXPORT_UNLOCK();
}

/* A new shell exports these two whatever they hold, as bash does, and OLDPWD before it
 * has a value. */
void shellRuntimeInitExports(void) {
    shellExportMarkExported("PWD");
    shellExportMarkExported("OLDPWD");
}

/* export, declare -x and the like act on what the name means here. */
static void shellVariableSetExported(const char *name, bool exported) {
    ShellLocalVariable *local = shellLocalScopeFind(name);
    if (local) {
        local->exported = exported;
    } else if (exported) {
        shellExportMarkExported(name);
    } else {
        shellExportMarkUnexported(name);
    }
}

static bool shellVariableIsExported(const char *name) {
    ShellLocalVariable *local = shellLocalScopeFind(name);
    return local ? local->exported : shellGlobalIsExported(name);
}

/* declare -i, -l and -u stay with the name. */
#define SHELL_VAR_ATTR_INTEGER 1u
#define SHELL_VAR_ATTR_LOWER 2u
#define SHELL_VAR_ATTR_UPPER 4u
typedef struct ShellVariableAttribute {
    char *name;
    unsigned flags;
} ShellVariableAttribute;
static PSCAL_THREAD_LOCAL ShellVariableAttribute *gShellVariableAttributes = NULL;
static PSCAL_THREAD_LOCAL size_t gShellVariableAttributeCount = 0;

static bool shellVariableHasAttribute(const char *name, unsigned flag) {
    /* A local has its own, and takes none from what it hides. */
    ShellLocalVariable *local = shellLocalScopeFind(name);
    if (local) {
        return (local->attributes & flag) != 0;
    }
    for (size_t i = 0; i < gShellVariableAttributeCount; ++i) {
        if (strcmp(gShellVariableAttributes[i].name, name) == 0) {
            return (gShellVariableAttributes[i].flags & flag) != 0;
        }
    }
    return false;
}

static void shellVariableSetAttribute(const char *name, unsigned set_flags, unsigned clear_flags) {
    for (size_t i = 0; i < gShellVariableAttributeCount; ++i) {
        if (strcmp(gShellVariableAttributes[i].name, name) == 0) {
            gShellVariableAttributes[i].flags = (gShellVariableAttributes[i].flags & ~clear_flags) | set_flags;
            return;
        }
    }
    if (!set_flags) {
        return;
    }
    ShellVariableAttribute *grown = (ShellVariableAttribute *)realloc(
        gShellVariableAttributes, (gShellVariableAttributeCount + 1) * sizeof(ShellVariableAttribute));
    if (!grown) {
        return;
    }
    gShellVariableAttributes = grown;
    char *copy = strdup(name);
    if (copy) {
        gShellVariableAttributes[gShellVariableAttributeCount].name = copy;
        gShellVariableAttributes[gShellVariableAttributeCount].flags = set_flags;
        gShellVariableAttributeCount++;
    }
}

static bool shellArithmeticEvaluateText(const char *text, int depth, long long *out_value, const char *label);
static bool shellArithmeticEvaluate(const char *expr, long long *out_value, const char *label);
static void shellArithmeticExpansionFail(void);

/* The value after a variable's attributes, or NULL if they leave it as it is. */
static char *shellApplyVariableAttributes(const char *name, const char *value) {
    if (gShellVariableAttributeCount == 0 && gShellLocalScopeDepth == 0) {
        return NULL;
    }
    if (shellVariableHasAttribute(name, SHELL_VAR_ATTR_INTEGER)) {
        long long number = 0;
        if (!shellArithmeticEvaluateText(value, 0, &number, "")) {
            /* Nothing is assigned: it keeps what it had. */
            shellArithmeticExpansionFail();
            char *kept = shellLookupParameterValue(name, strlen(name));
            return kept ? kept : strdup("");
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%lld", number);
        return strdup(buffer);
    }
    bool lower = shellVariableHasAttribute(name, SHELL_VAR_ATTR_LOWER);
    bool upper = shellVariableHasAttribute(name, SHELL_VAR_ATTR_UPPER);
    if (!lower && !upper) {
        return NULL;
    }
    char *copy = strdup(value);
    for (char *cursor = copy; cursor && *cursor; ++cursor) {
        unsigned char c = (unsigned char)*cursor;
        if (c < 0x80) {
            *cursor = (char)(lower ? tolower(c) : toupper(c));
        }
    }
    return copy;
}
static PSCAL_THREAD_LOCAL VM *gShellCurrentVm = NULL;
/* On iOS the task holds them (vprocNoteCaughtSignal): a subshell shares this process, and what
 * it is sent is not its parent's. */
#if !defined(PSCAL_TARGET_IOS)
static volatile sig_atomic_t gShellPendingSignals[NSIG] = {0};
#endif
static _Thread_local uint64_t gShellSignalTrapsRunning = 0;
/* A substitution runs in this process, where under bash it is a process of its own: what is sent
 * to the shell waits until it is done, and the shell takes it then, so what the trap prints and its
 * `exit` are the shell's. `held` is what the shell around it has a trap for, `own` what it has set
 * one for itself, and `touched` what it has changed the disposition of. On iOS a shell run as a
 * command, or a subshell, may share this thread, and `task` says which shell this is about. */
typedef struct {
    int depth;
#if defined(PSCAL_TARGET_IOS)
    int task;
#endif
    uint64_t held;
    uint64_t own;
    uint64_t touched;
} ShellSubstitutionSignals;
static _Thread_local ShellSubstitutionSignals gShellSubstitutionSignals;
void shellResetJobIds(void) {
    gShellRuntimeDefault.job_next_id = 1;
    if (SC()) {
        SC()->job_next_id = 1;
    }
}

static ShellRuntimeState *shellGetContext(void) {
    static __thread bool resolving = false;
    if (resolving) {
        return &gShellRuntimeDefault;
    }
    resolving = true;
    ShellRuntimeState *ctx = gShellRuntimeCtx ? gShellRuntimeCtx : &gShellRuntimeDefault;
    resolving = false;
    return ctx;
}

static void shellSetContext(ShellRuntimeState *ctx) {
    gShellRuntimeCtx = ctx ? ctx : &gShellRuntimeDefault;
}

static void shellContextPush(ShellRuntimeState *ctx) {
    if (gShellContextDepth == gShellContextCapacity) {
        size_t capacity = gShellContextCapacity ? gShellContextCapacity * 2 : 64;
        ShellRuntimeState **grown = (ShellRuntimeState **)realloc(gShellContextStack, capacity * sizeof(*grown));
        if (!grown) {
            abort();
        }
        gShellContextStack = grown;
        gShellContextCapacity = capacity;
    }
    gShellContextStack[gShellContextDepth++] = ctx;
}
static ShellRuntimeState *shellContextPop(void) {
    if (gShellContextDepth == 0) {
        return &gShellRuntimeDefault;
    }
    ShellRuntimeState *ctx = gShellContextStack[gShellContextDepth - 1];
    gShellContextDepth--;
    return ctx ? ctx : &gShellRuntimeDefault;
}

ShellRuntimeState *shellRuntimeCurrentContext(void) {
    return shellGetContext();
}

ShellRuntimeState *shellRuntimeActivateContext(ShellRuntimeState *ctx) {
    ShellRuntimeState *previous = shellGetContext();
    shellSetContext(ctx);
    return previous;
}

#if defined(PSCAL_TARGET_IOS)
/* The task that is this shell. vprocCurrent() may not be: a builtin, eval or a sourced file
 * runs under a vproc that ends with the command. A subshell or a shell run as a command has
 * said which is its own. */
static int shellRuntimeOwnPid(void) {
    int pid = vprocSubshellPid();
    if (pid <= 0) {
        pid = vprocGetShellSelfPid();
    }
    return pid > 0 ? pid : (int)getpid();
}
#endif

static pid_t shellRuntimeDollarPid(void) {
    if (gShellRuntime.in_subshell) {
        return gShellRuntime.subshell_parent_pid;
    }
#if defined(PSCAL_TARGET_IOS)
    return (pid_t)shellRuntimeOwnPid();
#else
    return getpid();
#endif
}

bool shellRuntimeCurrentBuiltinBackground(void) {
    return gShellCurrentBuiltinBackground;
}
/* Whether this shell is at a prompt. Per thread: a shell run as a command shares the process
 * with the prompt that ran it, is not interactive itself, and if killed never puts anything
 * back. The shell's own threads take it from the one that started them (ShellThreadLink). */
static PSCAL_THREAD_LOCAL bool gShellInteractiveMode = false;

static bool shellRuntimeEnsureDisabledCapacity(size_t needed) {
    if (gShellRuntime.disabled_builtin_capacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellRuntime.disabled_builtin_capacity ?
                              gShellRuntime.disabled_builtin_capacity * 2 :
                              4;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    char **entries = realloc(gShellRuntime.disabled_builtins,
                             new_capacity * sizeof(char *));
    if (!entries) {
        return false;
    }
    gShellRuntime.disabled_builtins = entries;
    gShellRuntime.disabled_builtin_capacity = new_capacity;
    return true;
}

static bool shellRuntimeDisableBuiltin(const char *name) {
    if (!name || !*name) {
        return false;
    }
    const char *canonical = shellBuiltinCanonicalName(name);
    const char *target = canonical ? canonical : name;
    if (!shellIsBuiltinName(target)) {
        return false;
    }
    for (size_t i = 0; i < gShellRuntime.disabled_builtin_count; ++i) {
        char *entry = gShellRuntime.disabled_builtins[i];
        if (entry && strcasecmp(entry, target) == 0) {
            return true;
        }
    }
    if (!shellRuntimeEnsureDisabledCapacity(gShellRuntime.disabled_builtin_count + 1)) {
        return false;
    }
    char *copy = strdup(target);
    if (!copy) {
        return false;
    }
    gShellRuntime.disabled_builtins[gShellRuntime.disabled_builtin_count++] = copy;
    return true;
}

static bool shellRuntimeEnableBuiltin(const char *name) {
    if (!name || !*name) {
        return false;
    }
    const char *canonical = shellBuiltinCanonicalName(name);
    const char *target = canonical ? canonical : name;
    if (!shellIsBuiltinName(target)) {
        return false;
    }
    for (size_t i = 0; i < gShellRuntime.disabled_builtin_count; ++i) {
        char *entry = gShellRuntime.disabled_builtins[i];
        if (entry && strcasecmp(entry, target) == 0) {
            free(entry);
            if (i + 1 < gShellRuntime.disabled_builtin_count) {
                memmove(&gShellRuntime.disabled_builtins[i],
                        &gShellRuntime.disabled_builtins[i + 1],
                        (gShellRuntime.disabled_builtin_count - i - 1) * sizeof(char *));
            }
            gShellRuntime.disabled_builtin_count--;
            return true;
        }
    }
    return true;
}

static bool shellRuntimeBuiltinDisabled(const char *name) {
    if (!name || !*name) {
        return false;
    }
    const char *canonical = shellBuiltinCanonicalName(name);
    const char *target = canonical ? canonical : name;
    for (size_t i = 0; i < gShellRuntime.disabled_builtin_count; ++i) {
        char *entry = gShellRuntime.disabled_builtins[i];
        if (entry && strcasecmp(entry, target) == 0) {
            return true;
        }
    }
    return false;
}

static void shellUpdateStatus(int status);
static void shellSignalHandler(int signo);

static void shellTrapClearEntry(ShellTrapEntry *entry) {
    if (!entry) {
        return;
    }
    if (entry->command) {
        free(entry->command);
        entry->command = NULL;
    }
    entry->action = SHELL_TRAP_ACTION_DEFAULT;
    entry->dormant = false;
}

static bool shellTrapSetCommand(ShellTrapEntry *entry, const char *command) {
    if (!entry) {
        return false;
    }
    shellTrapClearEntry(entry);
    if (!command) {
        entry->action = SHELL_TRAP_ACTION_COMMAND;
        entry->command = strdup("");
        return entry->command != NULL;
    }
    char *copy = strdup(command);
    if (!copy) {
        return false;
    }
    entry->action = SHELL_TRAP_ACTION_COMMAND;
    entry->command = copy;
    entry->dormant = false;
    return true;
}

void shellRuntimePushScript(void) {
    gShellScriptDepth++;
}

void shellRuntimePopScript(void) {
    if (gShellScriptDepth > 0) {
        gShellScriptDepth--;
    }
}

bool shellRuntimeIsOutermostScript(void) {
    return gShellScriptDepth == 1;
}

void shellRuntimeEnterCondition(void) {
    gShellConditionDepth++;
}

void shellRuntimeLeaveCondition(void) {
    if (gShellConditionDepth > 0) {
        gShellConditionDepth--;
    }
}

bool shellRuntimeEvaluatingCondition(void) {
    return gShellConditionDepth > 0;
}

void shellRuntimeAbandonConditionEvaluation(void) {
    gShellConditionDepth = 0;
}

void shellRuntimeRequestExit(void) {
    shellRuntimeAbandonConditionEvaluation();
    gShellExitRequested = true;
}

static bool shellTrapIsLive(const ShellTrapEntry *entry) {
    return entry->action == SHELL_TRAP_ACTION_COMMAND && entry->command && !entry->dormant;
}

/* Functions, eval, source and traps each run on a VM, and a script calls them in loops, where
 * building and freeing one costs far more than the call. They take one from here, reset on the way
 * out, and on the way in for what was set on it while it waited. A slot is taken by exchange, so
 * threads can't share a VM and a fork keeps them. Deeper than the pool, they build and free. */
#define SHELL_VM_POOL_SIZE 16
static _Atomic(VM *) gShellVmPool[SHELL_VM_POOL_SIZE];

VM *shellVmAcquire(void) {
    for (size_t i = 0; i < SHELL_VM_POOL_SIZE; ++i) {
        if (atomic_load_explicit(&gShellVmPool[i], memory_order_relaxed)) {
            VM *vm = atomic_exchange_explicit(&gShellVmPool[i], NULL, memory_order_acquire);
            if (vm) {
                vmResetExecutionState(vm);
                return vm;
            }
        }
    }
    VM *vm = (VM *)calloc(1, sizeof(VM));
    if (vm) {
        initVM(vm);
    }
    return vm;
}

void shellVmRelease(VM *vm) {
    if (!vm) {
        return;
    }
    pscalRuntimeVmFlushOutput();
    vmResetExecutionState(vm);
    vm->frontendContext = NULL;
    for (size_t i = 0; i < SHELL_VM_POOL_SIZE; ++i) {
        VM *empty = NULL;
        if (atomic_compare_exchange_strong_explicit(&gShellVmPool[i], &empty, vm, memory_order_release,
                                                    memory_order_relaxed)) {
            return;
        }
    }
    freeVM(vm);
    free(vm);
}

/* $? as a variable. Each command sets this several times, nearly always to what it holds, and
 * rewriting a plain global needs nothing else a tracked variable might. */
static void shellMirrorLastStatus(int status) {
    static const char name[] = "EXSH_LAST_STATUS";
    char buffer[16];
    if (status >= 0 && status <= 9) {
        buffer[0] = (char)('0' + status);
        buffer[1] = '\0';
    } else {
        snprintf(buffer, sizeof(buffer), "%d", status);
    }
    if (!gShellRuntime.allexport_enabled && !gShellRuntime.pipeline_subshell_active &&
        !gShellRuntime.local_scope_active && !shellReadonlyContains(name) &&
        !shellLocalScopeFind(name) && !shellArrayRegistryLookup(name, sizeof(name) - 1)) {
        const char *current = shellSafeGetenv(name);
        if (current) {
            shellReadonlySetErrorName(NULL);
            if (strcmp(current, buffer) != 0) {
                setenv(name, buffer, 1);
            }
            return;
        }
    }
    shellSetTrackedVariable(name, buffer, false);
}

void shellRuntimeSetLastStatus(int status) {
    gShellStatusVersion++;
    /* The pipeline it was in finishes with the same status. */
    if (status != gShellRuntime.last_status) {
        gShellErrTrapSkipOnce = false;
    }
    if (gShellArithmeticErrorPending) {
        status = 1;
        gShellArithmeticErrorPending = false;
    }
    gShellRuntime.last_status = status;
    shellMirrorLastStatus(status);
    bool suppress_errexit = shellRuntimeEvaluatingCondition();
    if (status != 0) {
        if ((gShellRuntime.errexit_enabled || shellTrapIsLive(&gShellErrTrap)) && !suppress_errexit) {
            gShellRuntime.errexit_pending = true;
            gShellRuntime.errexit_defer_active = true;
        }
    } else {
        gShellRuntime.errexit_pending = false;
        gShellRuntime.errexit_defer_active = false;
    }
    gShellStatusSticky = false;
}

void shellRuntimeSetLastStatusSticky(int status) {
    shellRuntimeSetLastStatus(status);
    gShellStatusSticky = true;
}

/* Each line at the prompt runs in a context of its own, copied from this one and
 * thrown away after it, so what it left in $? has to be carried back. Only the
 * value: the line has had its ERR trap and errexit, and a pending one here would be
 * copied into the next line and run them again. */
void shellRuntimeSetPromptStatus(int status) {
    gShellRuntime.last_status = status;
}

/* What such a line leaves for the next: this is the shell's state, not the line's.
 * What it owns is swapped, so that the line's context frees what the shell had.
 * Everything else in there ends with the line, loop, pipeline and condition state
 * above all, which Ctrl-C can leave half done. */
void shellRuntimeKeepLineState(ShellRuntimeState *line_ctx) {
    ShellRuntimeState *shell = shellGetContext();
    if (!line_ctx || line_ctx == shell) {
        return;
    }
#define KEEP(field) shell->field = line_ctx->field
#define KEEP_OWNED(field) \
    do { \
        __typeof__(shell->field) held = shell->field; \
        shell->field = line_ctx->field; \
        line_ctx->field = held; \
    } while (0)
    KEEP(monitor_request);
    KEEP(errexit_enabled);
    KEEP(pipefail_enabled);
    KEEP(nounset_enabled);
    KEEP(random_seed);
    KEEP(random_seeded);
    KEEP(noglob_enabled);
    KEEP(noclobber_enabled);
    KEEP(xtrace_enabled);
    KEEP(verbose_enabled);
    KEEP(braceexpand_disabled);
    KEEP(allexport_enabled);
    KEEP(posix_mode);
    KEEP(errtrace_enabled);
    KEEP(functrace_enabled);
    KEEP(last_bg_pid);
    KEEP(job_next_id);
    KEEP(getopts_index);
    KEEP(getopts_offset);
    KEEP(trap_enabled);
    KEEP(dir_stack_initialised);
    KEEP_OWNED(dir_stack);
    KEEP_OWNED(dir_stack_count);
    KEEP_OWNED(dir_stack_capacity);
    KEEP_OWNED(hash_entries);
    KEEP_OWNED(hash_count);
    KEEP_OWNED(hash_capacity);
    KEEP_OWNED(disabled_builtins);
    KEEP_OWNED(disabled_builtin_count);
    KEEP_OWNED(disabled_builtin_capacity);
    KEEP_OWNED(readonly_vars);
    KEEP_OWNED(readonly_var_count);
    KEEP_OWNED(readonly_var_capacity);
    KEEP_OWNED(array_vars);
    KEEP_OWNED(array_var_count);
    KEEP_OWNED(array_var_capacity);
    KEEP_OWNED(jobs);
    KEEP_OWNED(job_count);
    KEEP(finished_job_count);
    memcpy(shell->finished_jobs, line_ctx->finished_jobs,
           line_ctx->finished_job_count * sizeof(line_ctx->finished_jobs[0]));
    KEEP_OWNED(history);
    KEEP_OWNED(exit_trap);
    for (int i = 0; i < NSIG; ++i) {
        KEEP_OWNED(signal_traps[i]);
    }
#undef KEEP_OWNED
#undef KEEP
}

void shellRuntimeRefreshTrapEnabled(void) {
    gShellRuntime.trap_enabled = false;
#ifdef SIGINT
    if (gShellSignalTraps[SIGINT].action == SHELL_TRAP_ACTION_COMMAND) {
        gShellRuntime.trap_enabled = true;
        return;
    }
#endif
#ifdef SIGQUIT
    if (gShellSignalTraps[SIGQUIT].action == SHELL_TRAP_ACTION_COMMAND) {
        gShellRuntime.trap_enabled = true;
        return;
    }
#endif
#ifdef SIGTSTP
    if (gShellSignalTraps[SIGTSTP].action == SHELL_TRAP_ACTION_COMMAND) {
        gShellRuntime.trap_enabled = true;
        return;
    }
#endif
}

static void shellRuntimeExecuteTrap(const char *command, const char *label) {
    if (!command || gShellTrapExecuting) {
        return;
    }
    gShellTrapExecuting = true;

#if defined(PSCAL_TARGET_IOS)
    /* A stage's thread reads and writes the running pipeline in this context, and the commands in
     * the trap need one of their own. They get a copy of the context to run in, so the pipeline
     * stays where the stages have it. Nothing else here is touched by another thread. */
    ShellRuntimeState *pipeline_owner = NULL;
    if (gShellRuntime.pipeline.active) {
        ShellRuntimeState *trap_context = (ShellRuntimeState *)malloc(sizeof(ShellRuntimeState));
        if (trap_context) {
            pipeline_owner = shellGetContext();
            *trap_context = *pipeline_owner;
            shellSetContext(trap_context);
        }
    }
#endif

    ShellRunOptions opts = (ShellRunOptions){0};
    opts.no_cache = 1;
    opts.quiet = true;
    opts.exit_on_signal = shellRuntimeExitOnSignal();
    opts.suppress_warnings = true;
    /* Like eval: the locals and loops it broke into are its own. */
    opts.share_runtime_context = true;
    const char *frontend = shellRuntimeGetArg0();
    opts.frontend_path = frontend ? frontend : "exsh";
    ShellPipelineContext *caller_pipeline = &gShellRuntime.pipeline;
    ShellPipelineContext saved_pipeline = *caller_pipeline;
    memset(caller_pipeline, 0, sizeof(*caller_pipeline));
    caller_pipeline->pgid = -1;

    bool saved_exit_requested = gShellExitRequested;
    bool saved_break_requested = gShellRuntime.break_requested;
    int saved_break_levels = gShellRuntime.break_requested_levels;
    bool saved_continue_requested = gShellRuntime.continue_requested;
    int saved_continue_levels = gShellRuntime.continue_requested_levels;
    bool saved_errexit_pending = gShellRuntime.errexit_pending;
    bool saved_errexit_defer_active = gShellRuntime.errexit_defer_active;
    bool saved_return_pending = gShellReturnPending;
    bool saved_trap_status_valid = gShellTrapStatusValid;
    int saved_trap_status = gShellTrapStatus;
    VM *current_vm = gShellCurrentVm;
    bool saved_vm_exit = false;
    bool saved_vm_abort = false;
    const char *saved_vm_builtin = NULL;
    if (current_vm) {
        saved_vm_exit = current_vm->exit_requested;
        saved_vm_abort = current_vm->abort_requested;
        saved_vm_builtin = current_vm->current_builtin_name;
        current_vm->exit_requested = false;
        current_vm->abort_requested = false;
        current_vm->current_builtin_name = NULL;
    }
    gShellExitRequested = false;
    gShellRuntime.break_requested = false;
    gShellRuntime.break_requested_levels = 0;
    gShellRuntime.continue_requested = false;
    gShellRuntime.continue_requested_levels = 0;
    gShellRuntime.errexit_pending = false;
    gShellRuntime.errexit_defer_active = false;
    gShellReturnPending = false;
    gShellTrapStatusValid = true;
    gShellTrapStatus = gShellRuntime.last_status;

    bool exit_requested = false;
    int status = EXIT_FAILURE;

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

    if (can_run || shellSymbolTableScopeIsActive()) {
        gShellTextDepth++;
        status = shellRunSource(command, label ? label : "<trap>", &opts, &exit_requested);
        gShellTextDepth--;
    }
    if (!shellRuntimeChunkLeftStages()) {
        shellResetPipeline();
    }
    *caller_pipeline = saved_pipeline;

#if defined(PSCAL_TARGET_IOS)
    if (pipeline_owner) {
        ShellRuntimeState *trap_context = shellGetContext();
        trap_context->pipeline = saved_pipeline;
        /* All but the pipeline, whose stages have gone on meanwhile. */
        size_t pipeline_start = offsetof(ShellRuntimeState, pipeline);
        size_t pipeline_end = pipeline_start + sizeof(ShellPipelineContext);
        memcpy(pipeline_owner, trap_context, pipeline_start);
        memcpy((char *)pipeline_owner + pipeline_end, (char *)trap_context + pipeline_end,
               sizeof(ShellRuntimeState) - pipeline_end);
        shellSetContext(pipeline_owner);
        free(trap_context);
    }
#endif

    if (scope_pushed) {
        shellSymbolTableScopePop(&table_scope);
    }

    if (!can_run && !shellSymbolTableScopeIsActive()) {
        status = EXIT_FAILURE;
    }

    if (exit_requested) {
        shellRuntimeRequestExit();
        if (current_vm) {
            current_vm->exit_requested = true;
        }
    }

    gShellReturnPending = saved_return_pending || gShellReturnPending;
    gShellTrapStatusValid = saved_trap_status_valid;
    gShellTrapStatus = saved_trap_status;

    bool trap_exit_requested = gShellExitRequested;
    bool trap_break_requested = gShellRuntime.break_requested;
    int trap_break_levels = gShellRuntime.break_requested_levels;
    bool trap_continue_requested = gShellRuntime.continue_requested;
    int trap_continue_levels = gShellRuntime.continue_requested_levels;
    bool trap_errexit_pending = gShellRuntime.errexit_pending;
    bool trap_errexit_defer_active = gShellRuntime.errexit_defer_active;
    bool trap_vm_exit = current_vm ? current_vm->exit_requested : false;
    bool trap_vm_abort = current_vm ? current_vm->abort_requested : false;
    const char *trap_vm_builtin = current_vm ? current_vm->current_builtin_name : NULL;

    gShellExitRequested = saved_exit_requested || trap_exit_requested;
    if (saved_break_requested || trap_break_requested) {
        gShellRuntime.break_requested = true;
        if (saved_break_requested && trap_break_requested) {
            gShellRuntime.break_requested_levels =
                (saved_break_levels > trap_break_levels) ? saved_break_levels : trap_break_levels;
        } else if (trap_break_requested) {
            gShellRuntime.break_requested_levels = trap_break_levels;
        } else {
            gShellRuntime.break_requested_levels = saved_break_levels;
        }
    } else {
        gShellRuntime.break_requested = false;
        gShellRuntime.break_requested_levels = 0;
    }
    if (saved_continue_requested || trap_continue_requested) {
        gShellRuntime.continue_requested = true;
        if (saved_continue_requested && trap_continue_requested) {
            gShellRuntime.continue_requested_levels =
                (saved_continue_levels > trap_continue_levels) ? saved_continue_levels : trap_continue_levels;
        } else if (trap_continue_requested) {
            gShellRuntime.continue_requested_levels = trap_continue_levels;
        } else {
            gShellRuntime.continue_requested_levels = saved_continue_levels;
        }
    } else {
        gShellRuntime.continue_requested = false;
        gShellRuntime.continue_requested_levels = 0;
    }
    gShellRuntime.errexit_pending = saved_errexit_pending || trap_errexit_pending;
    gShellRuntime.errexit_defer_active = saved_errexit_defer_active || trap_errexit_defer_active;
    if (current_vm) {
        current_vm->exit_requested = saved_vm_exit || trap_vm_exit;
        current_vm->abort_requested = saved_vm_abort || trap_vm_abort;
        if (!current_vm->current_builtin_name) {
            current_vm->current_builtin_name = trap_vm_builtin ? trap_vm_builtin : saved_vm_builtin;
        }
    }

    shellUpdateStatus(status);

    gShellTrapExecuting = false;
}

/* `return` in a trap's text ends what the trap broke into, and so does what drops a whole
 * command. The caller's chunk stops at its next instruction. */
static bool shellRuntimeTrapEndedChunk(VM *vm) {
    if (gShellExitRequested || (!gShellReturnPending && !gShellDiscardPending)) {
        return false;
    }
    if (gShellReturnPending) {
        gShellRuntime.last_status = gShellReturnStatus;
    }
    gShellRuntime.errexit_pending = false;
    gShellRuntime.errexit_defer_active = false;
    if (vm) {
        vm->exit_requested = true;
        vm->current_builtin_name = "return";
    }
    return true;
}

/* A signal that ended the shell left the loops it was in skipping their bodies, and what the EXIT
 * trap runs would be skipped with them. Those loops are over. Only this trap: under bash a signal's
 * trap that runs during `break` has its commands skipped too. */
static void shellLoopsOverForExitTrap(void) {
    for (size_t i = 0; i < gShellLoopStackSize; ++i) {
        gShellLoopStack[i].skip_body = false;
        gShellLoopStack[i].break_pending = false;
        gShellLoopStack[i].continue_pending = false;
    }
}

/* Nothing unwinds under bash when a script exits or errexit ends it, so every redirection
 * around that point holds while the EXIT trap runs. Here the frames undo them on the way
 * out, so the trap runs first, where it stands. Inside eval, source and a trap's text bash
 * does undo what that text opened, so those wait for their caller. */
static void shellRuntimeRunExitTrapInPlace(void) {
    if (gShellExitTrap.action != SHELL_TRAP_ACTION_COMMAND || !gShellExitTrap.command ||
        gShellTrapExecuting || gShellTextDepth > 0 || gShellRuntime.pipeline.stage_count > 1) {
        return;
    }
    int status = gShellRuntime.last_status;
    bool requested = gShellExitRequested;
    char *command = gShellExitTrap.command;
    gShellExitTrap.command = NULL;
    gShellExitTrap.action = SHELL_TRAP_ACTION_DEFAULT;
    gShellExitRequested = false;
    shellLoopsOverForExitTrap();
    shellRuntimeExecuteTrap(command, "<exit-trap>");
    free(command);
    /* `exit` in the trap gives the status. */
    if (!gShellExitRequested) {
        gShellRuntime.last_status = status;
        gShellRuntime.errexit_pending = false;
        gShellRuntime.errexit_defer_active = false;
    }
    gShellExitRequested = gShellExitRequested || requested;
}

bool shellRuntimeShouldDeferExit(VM *vm) {
    if (!gShellRuntime.errexit_defer_active) {
        return false;
    }
    gShellRuntime.errexit_defer_active = false;
    if (vm) {
        vm->exit_requested = false;
        vm->abort_requested = false;
    }
    if (gShellCurrentVm) {
        gShellCurrentVm->exit_requested = false;
        gShellCurrentVm->abort_requested = false;
        gShellCurrentVm->current_builtin_name = NULL;
    }
    return true;
}

bool shellRuntimeMaybeRequestPendingExit(VM *vm) {
    if (!gShellRuntime.errexit_pending) {
        return false;
    }
    if (gShellRuntime.errexit_defer_active) {
        if (!shellRuntimeEvaluatingCondition()) {
            gShellRuntime.errexit_defer_active = false;
        }
        return false;
    }
    gShellRuntime.errexit_pending = false;
    bool err_trap_skipped = gShellErrTrapSkipOnce;
    gShellErrTrapSkipOnce = false;
    if (shellTrapIsLive(&gShellErrTrap) && !err_trap_skipped) {
        int failed_status = gShellRuntime.last_status;
        shellRuntimeExecuteTrap(gShellErrTrap.command, "<err-trap>");
        gShellRuntime.errexit_pending = false;
        gShellRuntime.errexit_defer_active = false;
        if (gShellExitRequested) {
            /* `exit` in the trap. */
            shellRuntimeRunExitTrapInPlace();
            if (vm) {
                vm->exit_requested = true;
                vm->abort_requested = true;
            }
            return true;
        }
        if (shellRuntimeTrapEndedChunk(vm)) {
            return true;
        }
        /* $? is still the command that failed. */
        gShellRuntime.last_status = failed_status;
    }
    if (!gShellRuntime.errexit_enabled) {
        return false;
    }
    shellRuntimeRunExitTrapInPlace();
    shellRuntimeRequestExit();
    if (vm) {
        vm->exit_requested = true;
        vm->abort_requested = true;
    }
    if (gShellCurrentVm) {
        gShellCurrentVm->exit_requested = true;
        gShellCurrentVm->abort_requested = true;
        if (!gShellCurrentVm->current_builtin_name) {
            gShellCurrentVm->current_builtin_name = "errexit";
        }
    }
    return true;
}

/* A chunk whose last command failed stops before the check that runs its ERR trap, and that
 * trap answers to what happened in there. */
void shellRuntimeFinishChunk(VM *vm) {
    if (!gShellRuntime.errexit_pending || gShellExitRequested || gShellReturnPending || gShellDiscardPending ||
        !shellTrapIsLive(&gShellErrTrap)) {
        return;
    }
    gShellRuntime.errexit_defer_active = false;
    shellRuntimeMaybeRequestPendingExit(vm);
}

/* A `return` ended the chunk, and whoever ran the chunk has yet to see it. */
bool shellRuntimeReturnPending(void) {
    return gShellReturnPending;
}

const volatile bool *shellRuntimePendingExitFlag(void) {
    return &gShellRuntime.errexit_pending;
}

ShellTrapAction shellRuntimeGetSignalTrapAction(int signo) {
    if (signo <= 0 || signo >= NSIG) {
        return SHELL_TRAP_ACTION_DEFAULT;
    }
    return gShellSignalTraps[signo].action;
}

void shellRuntimeRunSignalTrap(int signo) {
    if (signo <= 0 || signo >= NSIG) {
        return;
    }
    ShellTrapEntry *entry = &gShellSignalTraps[signo];
    if (entry->action != SHELL_TRAP_ACTION_COMMAND || !entry->command) {
        return;
    }
    char label[32];
    snprintf(label, sizeof(label), "<trap-%d>", signo);
    int status_before = gShellRuntime.last_status;
    bool errexit_before = gShellRuntime.errexit_pending;
    /* Another signal's trap may break into this one. Its own waits until this one is done. */
    uint64_t own_bit = (uint64_t)1 << (signo % 64);
    bool nested = gShellTrapExecuting;
    gShellSignalTrapsRunning |= own_bit;
    gShellTrapExecuting = false;
    /* The text outlives this call even if the trap changes itself. */
    char *command = strdup(entry->command);
    shellRuntimeExecuteTrap(command, label);
    free(command);
    gShellTrapExecuting = nested;
    gShellSignalTrapsRunning &= ~own_bit;
    if (shellRuntimeTrapEndedChunk(gShellCurrentVm)) {
        return;
    }
    if (gShellExitRequested) {
        shellRuntimeRunExitTrapInPlace();
    }
    /* Otherwise $? is what it was, unless that was the end of the shell. */
    if (!gShellExitRequested) {
        gShellRuntime.last_status = status_before;
        gShellRuntime.errexit_pending = errexit_before;
    }
}

static void shellRuntimeRunDebugTrap(void) {
    if (gShellDebugTrap.action == SHELL_TRAP_ACTION_COMMAND && gShellDebugTrap.command &&
        shellLocalScopeDepth() == 0) {
        shellRuntimeExecuteTrap(gShellDebugTrap.command, "<debug-trap>");
    }
}

/* A function and a sourced file run it as they end. $? in it is what it was before `return`,
 * and the status stays what the call gives, whatever the trap does. */
static void shellRuntimeRunReturnTrap(bool returned, int status) {
    if (!shellTrapIsLive(&gShellReturnTrap)) {
        return;
    }
    if (returned) {
        gShellRuntime.last_status = gShellReturnPriorStatus;
    }
    shellRuntimeExecuteTrap(gShellReturnTrap.command, "<return-trap>");
    gShellReturnPending = false;
    gShellRuntime.last_status = status;
    gShellRuntime.errexit_pending = false;
    gShellRuntime.errexit_defer_active = false;
}

/* A function starts without the caller's ERR and RETURN traps unless set -E and set -T hand
 * them on, and they come back when it ends, if it left none of its own in their place. */
typedef struct {
    ShellTrapEntry err;
    ShellTrapEntry ret;
    ShellTrapEntry debug; /* A substitution's only: a function keeps the DEBUG trap it was called with. */
} ShellCallerTraps;

static void shellCallerTrapsPutAside(ShellCallerTraps *saved, bool traced) {
    memset(saved, 0, sizeof(*saved));
    if (!gShellRuntime.errtrace_enabled && !gShellErrTrap.dormant) {
        saved->err = gShellErrTrap;
        memset(&gShellErrTrap, 0, sizeof(gShellErrTrap));
    }
    if (!gShellRuntime.functrace_enabled && !traced && !gShellReturnTrap.dormant) {
        saved->ret = gShellReturnTrap;
        memset(&gShellReturnTrap, 0, sizeof(gShellReturnTrap));
    }
}

static void shellCallerTrapsBack(ShellCallerTraps *saved) {
    if (gShellErrTrap.action == SHELL_TRAP_ACTION_DEFAULT) {
        gShellErrTrap = saved->err;
    } else {
        shellTrapClearEntry(&saved->err);
    }
    if (gShellReturnTrap.action == SHELL_TRAP_ACTION_DEFAULT) {
        gShellReturnTrap = saved->ret;
    } else {
        shellTrapClearEntry(&saved->ret);
    }
}

void shellRuntimeRunExitTrap(void) {
    if (gShellExitTrap.action != SHELL_TRAP_ACTION_COMMAND || !gShellExitTrap.command) {
        return;
    }
    shellLoopsOverForExitTrap();
    int status = gShellRuntime.last_status;
    shellRuntimeExecuteTrap(gShellExitTrap.command, "<exit-trap>");
    if (gShellEndingOnSignal) {
        gShellRuntime.last_status = status;
    }
}

bool shellRuntimeSetExitTrap(ShellTrapAction action, const char *command) {
    shellTrapClearEntry(&gShellExitTrap);
    if (action == SHELL_TRAP_ACTION_COMMAND) {
        if (!shellTrapSetCommand(&gShellExitTrap, command)) {
            return false;
        }
    } else if (action == SHELL_TRAP_ACTION_IGNORE) {
        gShellExitTrap.action = SHELL_TRAP_ACTION_IGNORE;
    }
    return true;
}

typedef struct {
    const char *name;
    int signo;
} ShellSignalNameEntry;

static const ShellSignalNameEntry kShellSignalNames[] = {
#ifdef SIGHUP
    {"HUP", SIGHUP},
#endif
#ifdef SIGINT
    {"INT", SIGINT},
#endif
#ifdef SIGQUIT
    {"QUIT", SIGQUIT},
#endif
#ifdef SIGILL
    {"ILL", SIGILL},
#endif
#ifdef SIGTRAP
    {"TRAP", SIGTRAP},
#endif
#ifdef SIGABRT
    {"ABRT", SIGABRT},
#endif
#ifdef SIGIOT
    {"IOT", SIGIOT},
#endif
#ifdef SIGBUS
    {"BUS", SIGBUS},
#endif
#ifdef SIGFPE
    {"FPE", SIGFPE},
#endif
#ifdef SIGKILL
    {"KILL", SIGKILL},
#endif
#ifdef SIGUSR1
    {"USR1", SIGUSR1},
#endif
#ifdef SIGSEGV
    {"SEGV", SIGSEGV},
#endif
#ifdef SIGUSR2
    {"USR2", SIGUSR2},
#endif
#ifdef SIGPIPE
    {"PIPE", SIGPIPE},
#endif
#ifdef SIGALRM
    {"ALRM", SIGALRM},
#endif
#ifdef SIGTERM
    {"TERM", SIGTERM},
#endif
#ifdef SIGSTKFLT
    {"STKFLT", SIGSTKFLT},
#endif
#ifdef SIGCHLD
    {"CHLD", SIGCHLD},
#endif
#ifdef SIGCLD
    {"CLD", SIGCLD},
#endif
#ifdef SIGCONT
    {"CONT", SIGCONT},
#endif
#ifdef SIGSTOP
    {"STOP", SIGSTOP},
#endif
#ifdef SIGTSTP
    {"TSTP", SIGTSTP},
#endif
#ifdef SIGTTIN
    {"TTIN", SIGTTIN},
#endif
#ifdef SIGTTOU
    {"TTOU", SIGTTOU},
#endif
#ifdef SIGURG
    {"URG", SIGURG},
#endif
#ifdef SIGXCPU
    {"XCPU", SIGXCPU},
#endif
#ifdef SIGXFSZ
    {"XFSZ", SIGXFSZ},
#endif
#ifdef SIGVTALRM
    {"VTALRM", SIGVTALRM},
#endif
#ifdef SIGPROF
    {"PROF", SIGPROF},
#endif
#ifdef SIGWINCH
    {"WINCH", SIGWINCH},
#endif
#ifdef SIGIO
    {"IO", SIGIO},
#endif
#ifdef SIGPOLL
    {"POLL", SIGPOLL},
#endif
#ifdef SIGPWR
    {"PWR", SIGPWR},
#endif
#ifdef SIGSYS
    {"SYS", SIGSYS},
#endif
};

static bool shellRuntimeIsDigitString(const char *text) {
    if (!text || !*text) {
        return false;
    }
    const unsigned char *p = (const unsigned char *)text;
    if (*p == '+' || *p == '-') {
        ++p;
    }
    if (!*p) {
        return false;
    }
    while (*p) {
        if (!isdigit(*p)) {
            return false;
        }
        ++p;
    }
    return true;
}

bool shellRuntimeParseSignal(const char *text, int *out_signo) {
    if (!text || !out_signo) {
        return false;
    }
    if (strcasecmp(text, "EXIT") == 0 || strcmp(text, "0") == 0) {
        *out_signo = 0;
        return true;
    }
    if (shellRuntimeIsDigitString(text)) {
        long value = strtol(text, NULL, 10);
        if (value >= 0 && value < NSIG) {
            *out_signo = (int)value;
            return true;
        }
        return false;
    }
    const char *name = text;
    if (strncasecmp(name, "SIG", 3) == 0 && name[3] != '\0') {
        name += 3;
    }
    for (size_t i = 0; i < sizeof(kShellSignalNames) / sizeof(kShellSignalNames[0]); ++i) {
        if (strcasecmp(name, kShellSignalNames[i].name) == 0) {
            *out_signo = kShellSignalNames[i].signo;
            return true;
        }
    }
    return false;
}

bool shellRuntimeSignalName(int signo, bool include_prefix, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        return false;
    }
    if (signo == 0) {
        snprintf(buffer, buffer_len, "EXIT");
        return true;
    }
    if (signo < 0 || signo >= NSIG) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kShellSignalNames) / sizeof(kShellSignalNames[0]); ++i) {
        if (kShellSignalNames[i].signo == signo) {
            if (include_prefix) {
                snprintf(buffer, buffer_len, "SIG%s", kShellSignalNames[i].name);
            } else {
                snprintf(buffer, buffer_len, "%s", kShellSignalNames[i].name);
            }
            return true;
        }
    }
#if defined(SIGRTMIN) && defined(SIGRTMAX)
    int rtmin = SIGRTMIN;
    int rtmax = SIGRTMAX;
    if (signo >= rtmin && signo <= rtmax) {
        if (signo == rtmin) {
            snprintf(buffer, buffer_len, include_prefix ? "SIGRTMIN" : "RTMIN");
            return true;
        }
        if (signo == rtmax) {
            snprintf(buffer, buffer_len, include_prefix ? "SIGRTMAX" : "RTMAX");
            return true;
        }
        int offset_from_min = signo - rtmin;
        int offset_from_max = rtmax - signo;
        if (offset_from_min <= offset_from_max) {
            if (include_prefix) {
                snprintf(buffer, buffer_len, "SIGRTMIN+%d", offset_from_min);
            } else {
                snprintf(buffer, buffer_len, "RTMIN+%d", offset_from_min);
            }
        } else {
            if (include_prefix) {
                snprintf(buffer, buffer_len, "SIGRTMAX-%d", offset_from_max);
            } else {
                snprintf(buffer, buffer_len, "RTMAX-%d", offset_from_max);
            }
        }
        return true;
    }
#endif
    return false;
}

/* A signal ignored when a script starts cannot be trapped or reset, and trap lists it. On a
 * host that is the process's disposition. In the app it is the task's: the vproc this shell
 * starts in, which took what was ignored from whatever started it (nohup, `trap '' HUP`). */
void shellRuntimeNoteIgnoredSignals(void) {
#if defined(PSCAL_TARGET_IOS)
    VProc *vp = vprocCurrent();
    int task = vp ? vprocPid(vp) : shellRuntimeOwnPid();
#endif
    for (size_t i = 0; i < sizeof(kShellSignalNames) / sizeof(kShellSignalNames[0]); ++i) {
        int signo = kShellSignalNames[i].signo;
        struct sigaction current;
        memset(&current, 0, sizeof(current));
#if defined(PSCAL_TARGET_IOS)
        /* The keys the shell itself handles are not a disposition anything handed it, and
         * neither is SIGPIPE, which the app ignores in every tab's task so a write gets EPIPE. */
        if (signo == SIGTSTP || signo == SIGTTIN || signo == SIGTTOU || signo == SIGPIPE) {
            continue;
        }
        bool ignored = signo > 0 && signo < NSIG && vprocSigaction(task, signo, NULL, &current) == 0 &&
                       current.sa_handler == SIG_IGN;
#else
        bool ignored = signo > 0 && signo < NSIG && sigaction(signo, NULL, &current) == 0 &&
                       current.sa_handler == SIG_IGN;
#endif
        if (ignored) {
            gShellSignalIgnoredOnEntry[signo] = true;
            gShellSignalTraps[signo].action = SHELL_TRAP_ACTION_IGNORE;
        }
    }
}

static int shellRuntimeInstallSignalAction(int signo, ShellTrapAction action) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    if (action == SHELL_TRAP_ACTION_COMMAND) {
        sa.sa_handler = shellSignalHandler;
        sa.sa_flags |= SA_RESTART;
    } else if (action == SHELL_TRAP_ACTION_IGNORE) {
        sa.sa_handler = SIG_IGN;
    } else {
        bool shell_takes_it = signo == SIGINT || signo == SIGQUIT;
#if defined(PSCAL_TARGET_IOS)
        shell_takes_it = shell_takes_it || signo == SIGTSTP;
#else
        shell_takes_it = shell_takes_it || (signo == SIGTSTP && shellRuntimeIsInteractive());
#endif
        if (shell_takes_it) {
            sa.sa_handler = shellSignalHandler;
            sa.sa_flags |= SA_RESTART;
        } else {
            sa.sa_handler = SIG_DFL;
        }
    }

#if defined(PSCAL_TARGET_IOS)
    return vprocSigaction(shellRuntimeOwnPid(), signo, &sa, NULL);
#else
    return sigaction(signo, &sa, NULL);
#endif
}

static bool shellRuntimeInSubstitution(void) {
#if defined(PSCAL_TARGET_IOS)
    return gShellSubstitutionSignals.depth > 0 && gShellSubstitutionSignals.task == shellRuntimeOwnPid();
#else
    return gShellSubstitutionSignals.depth > 0;
#endif
}

/* Returns what to hand to Leave. */
static ShellSubstitutionSignals shellSubstitutionSignalsEnter(void) {
    ShellSubstitutionSignals saved = gShellSubstitutionSignals;
    gShellSubstitutionSignals.depth++;
#if defined(PSCAL_TARGET_IOS)
    if (gShellSubstitutionSignals.task != shellRuntimeOwnPid()) {
        gShellSubstitutionSignals.task = shellRuntimeOwnPid();
        gShellSubstitutionSignals.held = 0;
    }
#endif
    /* A trap the substitution around this one set is the shell's, to this one. */
    for (int signo = 1; signo < NSIG; ++signo) {
        if (gShellSignalTraps[signo].action == SHELL_TRAP_ACTION_COMMAND) {
            gShellSubstitutionSignals.held |= (uint64_t)1 << (signo % 64);
        }
    }
    gShellSubstitutionSignals.own = 0;
    gShellSubstitutionSignals.touched = 0;
    return saved;
}

/* Dispositions are the process's: the shell's come back. Called in the shell's own context. */
static void shellSubstitutionSignalsLeave(const ShellSubstitutionSignals *saved) {
    uint64_t touched = gShellSubstitutionSignals.touched;
    gShellSubstitutionSignals = *saved;
    for (int signo = 1; touched != 0 && signo < NSIG; ++signo) {
        uint64_t bit = (uint64_t)1 << (signo % 64);
        if (touched & bit) {
            touched &= ~bit;
            (void)shellRuntimeInstallSignalAction(signo, gShellSignalTraps[signo].action);
        }
    }
}

bool shellRuntimeSetSignalTrap(int signo, ShellTrapAction action, const char *command) {
    if (signo <= 0 || signo >= NSIG) {
        return false;
    }
    if (gShellSignalIgnoredOnEntry[signo] && !shellRuntimeIsInteractive()) {
        return true;
    }

    char *command_copy = NULL;
    if (action == SHELL_TRAP_ACTION_COMMAND) {
        const char *text = command ? command : "";
        command_copy = strdup(text);
        if (!command_copy) {
            return false;
        }
    }

    int set_rc = 0;
    if (shellRuntimeInSubstitution()) {
        uint64_t bit = (uint64_t)1 << (signo % 64);
        gShellSubstitutionSignals.touched |= bit;
        if (action == SHELL_TRAP_ACTION_COMMAND) {
            gShellSubstitutionSignals.own |= bit;
        } else {
            gShellSubstitutionSignals.own &= ~bit;
        }
        /* The shell's handler stays, for what is sent to the shell in the meantime. */
        if (action == SHELL_TRAP_ACTION_COMMAND || !(gShellSubstitutionSignals.held & bit)) {
            set_rc = shellRuntimeInstallSignalAction(signo, action);
        }
    } else {
        set_rc = shellRuntimeInstallSignalAction(signo, action);
    }
    if (set_rc != 0) {
        if (command_copy) {
            free(command_copy);
        }
        return false;
    }

    shellTrapClearEntry(&gShellSignalTraps[signo]);
    if (action == SHELL_TRAP_ACTION_COMMAND) {
        gShellSignalTraps[signo].action = SHELL_TRAP_ACTION_COMMAND;
        gShellSignalTraps[signo].command = command_copy;
    } else {
        gShellSignalTraps[signo].action = action;
        if (command_copy) {
            free(command_copy);
        }
    }

    shellRuntimeRefreshTrapEnabled();
    return true;
}

/* Posix mode expands aliases. Leaving it goes back to what this kind of shell starts with. */
static void shellRuntimeApplyPosixMode(bool enabled) {
    if (enabled != gShellRuntime.posix_mode) {
        ShellOptionEntry *aliases = shellShoptFindOption("expand_aliases");
        if (aliases) {
            aliases->enabled = enabled || gShellInteractiveMode;
        }
    }
    gShellRuntime.posix_mode = enabled;
}

static void shellRuntimeInitialisePosixMode(void) __attribute__((constructor));

static void shellRuntimeInitialisePosixMode(void) {
    const char *posix_env = shellSafeGetenv("POSIXLY_CORRECT");
    if (posix_env) {
        shellRuntimeApplyPosixMode(true);
    }
}

void shellRuntimeSetPosixMode(bool enabled) {
    shellRuntimeApplyPosixMode(enabled);
    if (enabled) {
        const char *current = shellSafeGetenv("POSIXLY_CORRECT");
        if (!current) {
            setenv("POSIXLY_CORRECT", "y", 1);
        }
    } else {
        unsetenv("POSIXLY_CORRECT");
    }
}

bool shellRuntimePosixMode(void) {
    return gShellRuntime.posix_mode;
}

#if !defined(PSCAL_TARGET_IOS)
/* Set by shellRuntimeInitSignals. Before that, what it started with is still to be noted. */
static bool gShellKeySignalsInstalled = false;
#endif

void shellRuntimeSetInteractive(bool interactive) {
    /* Scripts expand aliases only after shopt -s expand_aliases; a person at a prompt
     * always starts with them on. */
    if (interactive && !gShellInteractiveMode) {
        ShellOptionEntry *aliases = shellShoptFindOption("expand_aliases");
        if (aliases) {
            aliases->enabled = true;
        }
    }
    gShellInteractiveMode = interactive;
#if defined(PSCAL_TARGET_IOS)
    if (interactive) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        action.sa_handler = shellSignalHandler;
        action.sa_flags |= SA_RESTART;
        sigaction(SIGTSTP, &action, NULL);
        /* A prompt takes these keys whatever its task started with. */
        const int keys[] = {SIGINT, SIGQUIT};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (gShellSignalIgnoredOnEntry[keys[i]]) {
                gShellSignalIgnoredOnEntry[keys[i]] = false;
                gShellSignalTraps[keys[i]].action = SHELL_TRAP_ACTION_DEFAULT;
                (void)shellRuntimeInstallSignalAction(keys[i], SHELL_TRAP_ACTION_DEFAULT);
            }
        }
    }
#else
    /* At a prompt the shell keeps SIGTSTP to itself. Anywhere else it is in the group Ctrl-Z
     * reaches and stops with its commands, so whatever started it gets the terminal back. */
    if (interactive) {
        /* And a prompt takes these keys whatever it started with. */
        const int keys[] = {SIGINT, SIGQUIT, SIGTSTP};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (gShellSignalIgnoredOnEntry[keys[i]]) {
                gShellSignalIgnoredOnEntry[keys[i]] = false;
                gShellSignalTraps[keys[i]].action = SHELL_TRAP_ACTION_DEFAULT;
                (void)shellRuntimeInstallSignalAction(keys[i], SHELL_TRAP_ACTION_DEFAULT);
            }
        }
    }
    if (gShellKeySignalsInstalled) {
        (void)shellRuntimeInstallSignalAction(SIGTSTP, shellRuntimeGetSignalTrapAction(SIGTSTP));
    }
#endif
}

bool shellRuntimeIsInteractive(void) {
    return gShellInteractiveMode;
}

static bool shellMonitorShown(void);
static bool shellMonitorOn(void);

/* $-: the letters in bash's order. These are the options this shell tracks, and the
 * ones it reports as always on, as a bash script sees them (`hB`). */
static char *shellRuntimeOptionFlags(void) {
    char flags[16];
    size_t count = 0;
    if (gShellRuntime.allexport_enabled) {
        flags[count++] = 'a';
    }
    if (gShellRuntime.errexit_enabled) {
        flags[count++] = 'e';
    }
    if (gShellRuntime.noglob_enabled) {
        flags[count++] = 'f';
    }
    flags[count++] = 'h';
    if (shellRuntimeIsInteractive()) {
        flags[count++] = 'i';
    }
    if (shellMonitorShown()) {
        flags[count++] = 'm';
    }
    if (gShellRuntime.nounset_enabled) {
        flags[count++] = 'u';
    }
    if (gShellRuntime.verbose_enabled) {
        flags[count++] = 'v';
    }
    if (gShellRuntime.xtrace_enabled) {
        flags[count++] = 'x';
    }
    if (!gShellRuntime.braceexpand_disabled) {
        flags[count++] = 'B';
    }
    if (gShellRuntime.noclobber_enabled) {
        flags[count++] = 'C';
    }
    if (gShellRuntime.errtrace_enabled) {
        flags[count++] = 'E';
    }
    if (shellRuntimeIsInteractive()) {
        flags[count++] = 'H';
    }
    if (gShellRuntime.functrace_enabled) {
        flags[count++] = 'T';
    }
    flags[count] = '\0';
    return strdup(flags);
}

/* fcntl on 0-2 or on what the dup macro returned. On iOS those are slots in the vproc's
 * table. The shell's other fcntl calls are on host descriptors and stay raw: a
 * stream's translated fd, the substitution reader's pipe, a redirection's saved fd. */
static int shellSlotFcntl(int fd, int cmd, int arg) {
#if defined(PSCAL_TARGET_IOS)
    return vprocFcntlShim(fd, cmd, arg);
#else
    return fcntl(fd, cmd, arg);
#endif
}

static void shellRuntimeRememberStandardFds(void) {
    if (gShellSavedStdFds[0] < 0) gShellSavedStdFds[0] = dup(STDIN_FILENO);
    if (gShellSavedStdFds[1] < 0) gShellSavedStdFds[1] = dup(STDOUT_FILENO);
    if (gShellSavedStdFds[2] < 0) gShellSavedStdFds[2] = dup(STDERR_FILENO);
    for (int i = 0; i < 3; ++i) {
        if (gShellSavedStdFds[i] >= 0) {
            shellSlotFcntl(gShellSavedStdFds[i], F_SETFD, FD_CLOEXEC);
        }
    }
}

void shellRuntimeEnsureStandardFds(void) {
    shellRuntimeRememberStandardFds();
    int targets[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    for (int i = 0; i < 3; ++i) {
        if (targets[i] < 0) {
            continue;
        }
        int rc = shellSlotFcntl(targets[i], F_GETFD, 0);
        if (rc < 0 && errno == EBADF && gShellSavedStdFds[i] >= 0) {
            dup2(gShellSavedStdFds[i], targets[i]);
            shellResetStandardStream(targets[i]);
        }
    }
}

static void shellRandomEnsureSeeded(void) {
    if (gShellRuntime.random_seeded) {
        return;
    }
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    if (seed == 0) {
        seed = 1;
    }
    gShellRuntime.random_seed = seed;
    gShellRuntime.random_seeded = true;
}

static void shellRandomReseed(unsigned int seed) {
    gShellRuntime.random_seed = seed;
    gShellRuntime.random_seeded = true;
}

static void shellRandomAssignFromText(const char *value) {
    if (!value) {
        shellRandomReseed(0u);
        return;
    }
    errno = 0;
    char *endptr = NULL;
    unsigned long parsed = strtoul(value, &endptr, 10);
    if (errno != 0 || endptr == value) {
        parsed = 0;
    }
    shellRandomReseed((unsigned int)parsed);
}

static unsigned int shellRandomNextValue(void) {
    shellRandomEnsureSeeded();
    gShellRuntime.random_seed = gShellRuntime.random_seed * 1103515245u + 12345u;
    return (gShellRuntime.random_seed / 65536u) % 32768u;
}

static bool shellHandleSpecialAssignment(const char *name, const char *value) {
    if (!name) {
        return false;
    }
    if (strcmp(name, "RANDOM") == 0) {
        shellRandomAssignFromText(value);
        return true;
    }
    if (strcmp(name, "SECONDS") == 0) {
        gShellSecondsBase = time(NULL) - (value ? strtol(value, NULL, 10) : 0);
        return true;
    }
    return false;
}

static void shellArrayVariableClear(ShellArrayVariable *var) {
    if (!var) {
        return;
    }
    if (var->values) {
        for (size_t i = 0; i < var->count; ++i) {
            free(var->values[i]);
        }
        free(var->values);
    }
    if (var->keys) {
        for (size_t i = 0; i < var->count; ++i) {
            free(var->keys[i]);
        }
        free(var->keys);
    }
    var->values = NULL;
    var->keys = NULL;
    var->count = 0;
    var->kind = SHELL_ARRAY_KIND_INDEXED;
    var->sparse = false;
}

static bool shellArrayRegistryEnsureCapacity(size_t needed) {
    if (gShellArrayVarCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellArrayVarCapacity ? (gShellArrayVarCapacity * 2) : 8;
    if (new_capacity < needed) {
        new_capacity = needed;
    }
    ShellArrayVariable *resized =
        (ShellArrayVariable *)realloc(gShellArrayVars, new_capacity * sizeof(ShellArrayVariable));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellArrayVarCapacity; i < new_capacity; ++i) {
        resized[i].name = NULL;
        resized[i].values = NULL;
        resized[i].keys = NULL;
        resized[i].count = 0;
        resized[i].kind = SHELL_ARRAY_KIND_INDEXED;
        resized[i].sparse = false;
    }
    gShellArrayVars = resized;
    gShellArrayVarCapacity = new_capacity;
    return true;
}

static ShellArrayVariable *shellArrayRegistryFindMutable(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellArrayVarCount; ++i) {
        ShellArrayVariable *var = &gShellArrayVars[i];
        if (var->name && strcmp(var->name, name) == 0) {
            return var;
        }
    }
    return NULL;
}

static bool shellArrayRegistryIndexedExists(const char *name) {
    const ShellArrayVariable *var = shellArrayRegistryFindConst(name);
    return var && var->kind == SHELL_ARRAY_KIND_INDEXED;
}

static const ShellArrayVariable *shellArrayRegistryFindConst(const char *name) {
    return shellArrayRegistryFindMutable(name);
}

static bool shellArrayRegistryStore(const char *name,
                                   char **items,
                                   char **keys,
                                   size_t count,
                                   ShellArrayKind kind) {
    if (!name) {
        return false;
    }
    ShellArrayVariable *var = shellArrayRegistryFindMutable(name);
    if (!var) {
        if (!shellArrayRegistryEnsureCapacity(gShellArrayVarCount + 1)) {
            return false;
        }
        var = &gShellArrayVars[gShellArrayVarCount++];
        var->name = strdup(name);
        if (!var->name) {
            gShellArrayVarCount--;
            return false;
        }
        var->values = NULL;
        var->keys = NULL;
        var->count = 0;
        var->kind = SHELL_ARRAY_KIND_INDEXED;
        var->sparse = false;
    } else {
        shellArrayVariableClear(var);
    }

    var->kind = kind;
    if (count == 0) {
        return true;
    }

    var->values = (char **)calloc(count, sizeof(char *));
    if (!var->values) {
        shellArrayRegistryRemove(name);
        return false;
    }
    if (kind == SHELL_ARRAY_KIND_ASSOCIATIVE) {
        var->keys = (char **)calloc(count, sizeof(char *));
        if (!var->keys) {
            shellArrayRegistryRemove(name);
            return false;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const char *item = items[i] ? items[i] : "";
        var->values[i] = strdup(item);
        if (!var->values[i]) {
            shellArrayRegistryRemove(name);
            return false;
        }
        if (kind == SHELL_ARRAY_KIND_ASSOCIATIVE) {
            const char *key = (keys && keys[i]) ? keys[i] : "";
            var->keys[i] = strdup(key);
            if (!var->keys[i]) {
                shellArrayRegistryRemove(name);
                return false;
            }
        }
    }
    var->count = count;
    return true;
}

static void shellArrayRegistryRemove(const char *name) {
    if (!name) {
        return;
    }
    for (size_t i = 0; i < gShellArrayVarCount; ++i) {
        ShellArrayVariable *var = &gShellArrayVars[i];
        if (!var->name || strcmp(var->name, name) != 0) {
            continue;
        }
        free(var->name);
        var->name = NULL;
        shellArrayVariableClear(var);
        if (i + 1 < gShellArrayVarCount) {
            gShellArrayVars[i] = gShellArrayVars[gShellArrayVarCount - 1];
        }
        gShellArrayVarCount--;
        return;
    }
}

static const ShellArrayVariable *shellArrayRegistryLookup(const char *name, size_t len) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellArrayVarCount; ++i) {
        ShellArrayVariable *var = &gShellArrayVars[i];
        if (!var->name) {
            continue;
        }
        size_t stored_len = strlen(var->name);
        if (stored_len == len && strncmp(var->name, name, len) == 0) {
            return var;
        }
    }
    return NULL;
}

static bool shellAssignmentIsArrayLiteral(const char *raw_assignment, uint8_t word_flags) {
    if (!raw_assignment) {
        return false;
    }
    const char *eq = strchr(raw_assignment, '=');
    if (!eq) {
        return false;
    }

    bool base_single = (word_flags & SHELL_WORD_FLAG_SINGLE_QUOTED) != 0;
    bool base_double = (word_flags & SHELL_WORD_FLAG_DOUBLE_QUOTED) != 0;
    bool saw_single_marker = false;
    bool saw_double_marker = false;
    bool in_single_segment = false;
    bool in_double_segment = false;

    for (const char *cursor = raw_assignment; cursor < eq && *cursor; ++cursor) {
        if (*cursor == SHELL_QUOTE_MARK_SINGLE) {
            saw_single_marker = true;
            in_single_segment = !in_single_segment;
        } else if (*cursor == SHELL_QUOTE_MARK_DOUBLE) {
            saw_double_marker = true;
            in_double_segment = !in_double_segment;
        }
    }

    const char *value_start = eq + 1;
    const char *first_char = NULL;
    bool first_quoted = false;
    const char *last_char = NULL;
    bool last_quoted = false;

    for (const char *cursor = value_start; *cursor; ++cursor) {
        char ch = *cursor;
        if (ch == SHELL_QUOTE_MARK_SINGLE) {
            saw_single_marker = true;
            in_single_segment = !in_single_segment;
            continue;
        }
        if (ch == SHELL_QUOTE_MARK_DOUBLE) {
            saw_double_marker = true;
            in_double_segment = !in_double_segment;
            continue;
        }

        bool effective_single = in_single_segment || (!saw_single_marker && base_single);
        bool effective_double = in_double_segment || (!saw_double_marker && base_double);
        bool quoted = effective_single || effective_double;

        if (!first_char) {
            if (!quoted && isspace((unsigned char)ch)) {
                continue;
            }
            first_char = cursor;
            first_quoted = quoted;
        }

        if (!quoted && isspace((unsigned char)ch)) {
            continue;
        }

        last_char = cursor;
        last_quoted = quoted;
    }

    if (!first_char || !last_char) {
        return false;
    }
    if (first_quoted || last_quoted) {
        return false;
    }
    return *first_char == '(' && *last_char == ')';
}

static bool shellLoopEnsureCapacity(size_t needed) {
    if (gShellLoopStackCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellLoopStackCapacity ? gShellLoopStackCapacity * 2 : 4;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    ShellLoopFrame *resized = (ShellLoopFrame *)realloc(gShellLoopStack, new_capacity * sizeof(ShellLoopFrame));
    if (!resized) {
        return false;
    }
    gShellLoopStack = resized;
    gShellLoopStackCapacity = new_capacity;
    return true;
}

static void shellLoopFrameFreeData(ShellLoopFrame *frame) {
    if (!frame) {
        return;
    }
    if (frame->for_variable) {
        free(frame->for_variable);
        frame->for_variable = NULL;
    }
    if (frame->for_values) {
        for (size_t i = 0; i < frame->for_count; ++i) {
            free(frame->for_values[i]);
        }
        free(frame->for_values);
        frame->for_values = NULL;
    }
    frame->for_count = 0;
    frame->for_index = 0;
    frame->for_active = false;
    if (frame->cfor_init) {
        free(frame->cfor_init);
        frame->cfor_init = NULL;
    }
    if (frame->cfor_condition) {
        free(frame->cfor_condition);
        frame->cfor_condition = NULL;
    }
    if (frame->cfor_update) {
        free(frame->cfor_update);
        frame->cfor_update = NULL;
    }
    frame->cfor_condition_cached = false;
    frame->cfor_condition_value = false;
    frame->cond_kind = SHELL_LOOP_CONDITION_NONE;
    if (frame->cond_words) {
        for (size_t i = 0; i < frame->cond_word_count; ++i) {
            free(frame->cond_words[i]);
        }
        free(frame->cond_words);
        frame->cond_words = NULL;
    }
    frame->cond_word_count = 0;
    if (frame->cond_arith_expression) {
        free(frame->cond_arith_expression);
        frame->cond_arith_expression = NULL;
    }
    frame->body_kind = SHELL_LOOP_BODY_KIND_NONE;
    if (frame->body_words) {
        for (size_t i = 0; i < frame->body_word_count; ++i) {
            free(frame->body_words[i]);
        }
        free(frame->body_words);
        frame->body_words = NULL;
    }
    frame->body_word_count = 0;
    if (frame->body_arith_expression) {
        free(frame->body_arith_expression);
        frame->body_arith_expression = NULL;
    }
    if (frame->cond_arith_expression) {
        free(frame->cond_arith_expression);
        frame->cond_arith_expression = NULL;
    }
    if (frame->redirs_active) {
        shellRestoreExecRedirections(frame->redir_backups, frame->redir_backup_count);
    }
    if (frame->redir_backups) {
        shellFreeExecRedirBackups(frame->redir_backups, frame->redir_backup_count);
        frame->redir_backups = NULL;
        frame->redir_backup_count = 0;
        frame->redir_backup_capacity = 0;
    }
    if (frame->applied_redirs) {
        ShellCommand temp;
        memset(&temp, 0, sizeof(temp));
        temp.redirs = frame->applied_redirs;
        temp.redir_count = frame->applied_redir_count;
        shellFreeRedirections(&temp);
        frame->applied_redirs = NULL;
        frame->applied_redir_count = 0;
    }
    frame->redirs_active = false;
}

/* A failed copy leaves the table empty: the subshell cannot see the definition. */
static void shellCopyDefinitionTables(ShellRuntimeState *ctx,
                                      const ShellFunctionEntry *functions,
                                      size_t function_count,
                                      const ShellAlias *aliases,
                                      size_t alias_count,
                                      const ShellOptionEntry *options) {
    if (function_count > 0) {
        ctx->private_functions = (ShellFunctionEntry *)calloc(function_count, sizeof(ShellFunctionEntry));
        for (size_t i = 0; ctx->private_functions && i < function_count; ++i) {
            ShellFunctionEntry *dst = &ctx->private_functions[ctx->private_function_count];
            dst->name = strdup(functions[i].name);
            dst->source = functions[i].source ? strdup(functions[i].source) : NULL;
            dst->parameter_metadata = functions[i].parameter_metadata
                                          ? strdup(functions[i].parameter_metadata)
                                          : NULL;
            /* Bodies are never freed: another session may be running them. */
            dst->compiled = functions[i].compiled;
            dst->traced = functions[i].traced;
            if (dst->name) {
                ctx->private_function_count++;
            }
        }
    }
    if (alias_count > 0) {
        ctx->private_aliases = (ShellAlias *)calloc(alias_count, sizeof(ShellAlias));
        for (size_t i = 0; ctx->private_aliases && i < alias_count; ++i) {
            ShellAlias *dst = &ctx->private_aliases[ctx->private_alias_count];
            dst->name = strdup(aliases[i].name);
            dst->value = strdup(aliases[i].value);
            if (dst->name && dst->value) {
                ctx->private_alias_count++;
            } else {
                free(dst->name);
                free(dst->value);
                dst->name = NULL;
                dst->value = NULL;
            }
        }
    }
    ctx->private_options = (ShellOptionEntry *)malloc(sizeof(gShellOptionTable));
    if (ctx->private_options) {
        memcpy(ctx->private_options, options, sizeof(gShellOptionTable));
        ctx->has_private_tables = true;
    }
}

ShellRuntimeState *shellRuntimeCreateContext(void) {
    const ShellRuntimeState *template_ctx = shellRuntimeCurrentContext();
    if (!template_ctx) {
        template_ctx = &gShellRuntimeDefault;
    }
    ShellRuntimeState *ctx = (ShellRuntimeState *)calloc(1, sizeof(ShellRuntimeState));
    if (!ctx) {
        return NULL;
    }
    *ctx = *template_ctx;
    ctx->pipeline = (ShellPipelineContext){0};
    ctx->left_stages_running = false;
    /* Deep-copy owned runtime state so contexts do not share pointers. */
    ctx->dir_stack = NULL;
    ctx->dir_stack_count = 0;
    ctx->dir_stack_capacity = 0;
    ctx->dir_stack_initialised = template_ctx->dir_stack_initialised;
    if (template_ctx->dir_stack_count > 0 &&
        template_ctx->dir_stack) {
        ctx->dir_stack = (char **)calloc(template_ctx->dir_stack_count, sizeof(char *));
        if (ctx->dir_stack) {
            ctx->dir_stack_capacity = template_ctx->dir_stack_count;
            for (size_t i = 0; i < template_ctx->dir_stack_count; ++i) {
                if (template_ctx->dir_stack[i]) {
                    ctx->dir_stack[i] = strdup(template_ctx->dir_stack[i]);
                    if (ctx->dir_stack[i]) {
                        ctx->dir_stack_count++;
                    }
                }
            }
        }
    }

    ctx->hash_entries = NULL;
    ctx->hash_count = 0;
    ctx->hash_capacity = 0;
    if (template_ctx->hash_count > 0 && template_ctx->hash_entries) {
        ctx->hash_entries =
            (ShellHashEntry *)calloc(template_ctx->hash_count, sizeof(ShellHashEntry));
        if (ctx->hash_entries) {
            ctx->hash_capacity = template_ctx->hash_count;
            ctx->hash_count = template_ctx->hash_count;
            for (size_t i = 0; i < ctx->hash_count; ++i) {
                ShellHashEntry *dst = &ctx->hash_entries[i];
                const ShellHashEntry *src = &template_ctx->hash_entries[i];
                dst->hits = src->hits;
                if (src->name) {
                    dst->name = strdup(src->name);
                }
                if (src->path) {
                    dst->path = strdup(src->path);
                }
            }
        }
    }

    ctx->disabled_builtins = NULL;
    ctx->disabled_builtin_count = 0;
    ctx->disabled_builtin_capacity = 0;
    if (template_ctx->disabled_builtin_count > 0 &&
        template_ctx->disabled_builtins) {
        ctx->disabled_builtins = (char **)calloc(template_ctx->disabled_builtin_count,
                                                 sizeof(char *));
        if (ctx->disabled_builtins) {
            ctx->disabled_builtin_capacity = template_ctx->disabled_builtin_count;
            for (size_t i = 0; i < template_ctx->disabled_builtin_count; ++i) {
                if (template_ctx->disabled_builtins[i]) {
                    ctx->disabled_builtins[i] = strdup(template_ctx->disabled_builtins[i]);
                    if (ctx->disabled_builtins[i]) {
                        ctx->disabled_builtin_count++;
                    }
                }
            }
        }
    }

    ctx->history.entries = NULL;
    ctx->history.count = 0;
    ctx->history.capacity = 0;
    ctx->history.dropped = template_ctx->history.dropped;
    ctx->history.session_lines = template_ctx->history.session_lines;
    ctx->history.lines_in_file = template_ctx->history.lines_in_file;
    ctx->history_path = template_ctx->history_path ? strdup(template_ctx->history_path) : NULL;
    ctx->history_size_limit = template_ctx->history_size_limit;
    ctx->history_file_size_limit = template_ctx->history_file_size_limit;
    ctx->history_persist_enabled = template_ctx->history_persist_enabled;
    ctx->history_append_mode = template_ctx->history_append_mode;
    ctx->history_loaded_from_file = false;
    ctx->history_loading = false;
    if (template_ctx->history.count > 0 && template_ctx->history.entries) {
        ctx->history.entries =
            (char **)calloc(template_ctx->history.count, sizeof(char *));
        if (ctx->history.entries) {
            ctx->history.capacity = template_ctx->history.count;
            for (size_t i = 0; i < template_ctx->history.count; ++i) {
                if (template_ctx->history.entries[i]) {
                    ctx->history.entries[i] = strdup(template_ctx->history.entries[i]);
                    if (ctx->history.entries[i]) {
                        ctx->history.count++;
                    }
                }
            }
        }
    }

    ctx->arg0 = template_ctx->arg0 ? strdup(template_ctx->arg0) : NULL;

    for (int i = 0; i < NSIG; ++i) {
        ctx->signal_ignored_on_entry[i] = template_ctx->signal_ignored_on_entry[i];
        ctx->signal_traps[i].action = template_ctx->signal_traps[i].action;
        ctx->signal_traps[i].command = template_ctx->signal_traps[i].command ?
                                           strdup(template_ctx->signal_traps[i].command) :
                                           NULL;
    }
    ctx->exit_trap.action = template_ctx->exit_trap.action;
    ctx->exit_trap.command =
        template_ctx->exit_trap.command ? strdup(template_ctx->exit_trap.command) : NULL;

    ctx->jobs = NULL;
    ctx->job_count = 0;
    if (template_ctx->job_count > 0 && template_ctx->jobs) {
        ctx->jobs = (ShellJob *)calloc(template_ctx->job_count, sizeof(ShellJob));
        if (ctx->jobs) {
            ctx->job_count = template_ctx->job_count;
            for (size_t i = 0; i < ctx->job_count; ++i) {
                const ShellJob *src = &template_ctx->jobs[i];
                ShellJob *dst = &ctx->jobs[i];
                dst->pgid = src->pgid;
                dst->id = src->id;
                dst->last_pid = src->last_pid;
                dst->running = src->running;
                dst->stopped = src->stopped;
                dst->disowned = src->disowned;
                dst->inherited = src->inherited || gShellCommandSubFrame != NULL;
                dst->last_status = src->last_status;
                dst->term_signal = src->term_signal;
                dst->text = src->text;
                dst->first_pid = src->first_pid;
                dst->notified = src->notified;
                if (src->pid_count > 0 && src->pids) {
                    dst->pids = (pid_t *)calloc(src->pid_count, sizeof(pid_t));
                    if (dst->pids) {
                        memcpy(dst->pids, src->pids, src->pid_count * sizeof(pid_t));
                        dst->pid_count = src->pid_count;
                    }
                }
                dst->command = src->command ? strdup(src->command) : NULL;
            }
        }
    }
    /* Under bash a substitution is a fork, whose child drops every job that has ended but the one
     * $! names. Only the substitution's own context: a stage in there sees what it has. */
    if (gShellCommandSubFrame && !gShellCommandSubFrame->jobs_trimmed) {
        gShellCommandSubFrame->jobs_trimmed = true;
        pid_t last = template_ctx->last_bg_pid;
        size_t kept = 0;
        for (size_t i = 0; i < ctx->finished_job_count; ++i) {
            const ShellFinishedJob *entry = &ctx->finished_jobs[i];
            if (last > 0 && (entry->last_pid == last || entry->pgid == last)) {
                ctx->finished_jobs[kept++] = *entry;
            }
        }
        ctx->finished_job_count = kept;
    }

    ctx->array_vars = NULL;
    ctx->array_var_count = 0;
    ctx->array_var_capacity = 0;
    if (template_ctx->array_var_count > 0 && template_ctx->array_vars) {
        ctx->array_vars = (ShellArrayVariable *)calloc(template_ctx->array_var_count,
                                                       sizeof(ShellArrayVariable));
        if (ctx->array_vars) {
            ctx->array_var_capacity = template_ctx->array_var_count;
            for (size_t i = 0; i < template_ctx->array_var_count; ++i) {
                ShellArrayVariable *dst = &ctx->array_vars[i];
                const ShellArrayVariable *src = &template_ctx->array_vars[i];
                dst->name = src->name ? strdup(src->name) : NULL;
                dst->kind = src->kind;
                dst->sparse = src->sparse;
                dst->count = src->count;
                if (src->count > 0 && src->values) {
                    dst->values = (char **)calloc(src->count, sizeof(char *));
                    if (dst->values) {
                        for (size_t j = 0; j < src->count; ++j) {
                            if (src->values[j]) {
                                dst->values[j] = strdup(src->values[j]);
                            }
                        }
                    }
                }
                if (src->kind == SHELL_ARRAY_KIND_ASSOCIATIVE && src->count > 0 && src->keys) {
                    dst->keys = (char **)calloc(src->count, sizeof(char *));
                    if (dst->keys) {
                        for (size_t j = 0; j < src->count; ++j) {
                            if (src->keys[j]) {
                                dst->keys[j] = strdup(src->keys[j]);
                            }
                        }
                    }
                }
                ctx->array_var_count++;
            }
        }
    }

    ctx->readonly_vars = NULL;
    ctx->readonly_var_count = 0;
    ctx->readonly_var_capacity = 0;
    if (template_ctx->readonly_var_count > 0 && template_ctx->readonly_vars) {
        ctx->readonly_vars = (ShellReadonlyEntry *)calloc(template_ctx->readonly_var_count,
                                                          sizeof(ShellReadonlyEntry));
        if (ctx->readonly_vars) {
            ctx->readonly_var_capacity = template_ctx->readonly_var_count;
            for (size_t i = 0; i < template_ctx->readonly_var_count; ++i) {
                if (template_ctx->readonly_vars[i].name) {
                    ctx->readonly_vars[i].name = strdup(template_ctx->readonly_vars[i].name);
                    if (ctx->readonly_vars[i].name) {
                        ctx->readonly_var_count++;
                    }
                }
            }
        }
    }
    ctx->readonly_error_name =
        template_ctx->readonly_error_name ? strdup(template_ctx->readonly_error_name) : NULL;

    ctx->local_scope_stack = NULL;
    ctx->local_scope_depth = 0;
    ctx->local_scope_capacity = 0;
    ctx->loop_stack = NULL;
    ctx->loop_stack_size = 0;
    ctx->loop_stack_capacity = 0;
    ctx->loops_around = template_ctx->loops_around;

    ctx->has_private_tables = false;
    ctx->private_functions = NULL;
    ctx->private_function_count = 0;
    ctx->private_aliases = NULL;
    ctx->private_alias_count = 0;
    ctx->private_options = NULL;
    if (template_ctx->has_private_tables) {
        shellCopyDefinitionTables(ctx,
                                  template_ctx->private_functions,
                                  template_ctx->private_function_count,
                                  template_ctx->private_aliases,
                                  template_ctx->private_alias_count,
                                  template_ctx->private_options);
    }
    return ctx;
}

/* For a shell started as a command where nothing execs: what a new process would not have
 * had is gone, and what it defines goes when it does. The caller activates it. */
static ShellRuntimeState *shellRuntimeCreateProgramContext(void) {
    ShellRuntimeState *ctx = shellRuntimeCreateContext();
    if (!ctx) {
        return NULL;
    }
    /* What is ignored stays so, as across an exec. */
    for (int signo = 1; signo < NSIG; ++signo) {
        if (ctx->signal_traps[signo].action == SHELL_TRAP_ACTION_COMMAND) {
            shellTrapClearEntry(&ctx->signal_traps[signo]);
        }
    }
    if (ctx->exit_trap.action == SHELL_TRAP_ACTION_COMMAND) {
        shellTrapClearEntry(&ctx->exit_trap);
    }
    /* Its script is the outermost one in this shell, and its EXIT trap runs when that ends. */
    ctx->script_depth = 0;
    /* A new shell has no history until it records some: the prompt's list is not its own. */
    for (size_t i = 0; i < ctx->history.count; ++i) {
        free(ctx->history.entries[i]);
        ctx->history.entries[i] = NULL;
    }
    ctx->history.count = 0;
    ctx->history.dropped = 0;
    ctx->history.session_lines = 0;
    ctx->history.lines_in_file = 0;
    ctx->history.newest_is_prompt_line = false;
    for (size_t i = 0; i < ctx->array_var_count; ++i) {
        shellArrayVariableClear(&ctx->array_vars[i]);
    }
    free(ctx->array_vars);
    ctx->array_vars = NULL;
    ctx->array_var_count = 0;
    ctx->array_var_capacity = 0;
    for (size_t i = 0; i < ctx->readonly_var_count; ++i) {
        free(ctx->readonly_vars[i].name);
    }
    free(ctx->readonly_vars);
    ctx->readonly_vars = NULL;
    ctx->readonly_var_count = 0;
    ctx->readonly_var_capacity = 0;
    ctx->local_scope_active = false;
    ctx->pipeline_subshell_active = false;
    /* A program in a subshell is no part of it: `$$` is its own, and `kill $$` does not reach the
     * shell that started the subshell. */
    ctx->in_subshell = false;
    ctx->random_seeded = false;
    ctx->errexit_enabled = false;
    ctx->pipefail_enabled = false;
    ctx->nounset_enabled = false;
    ctx->noglob_enabled = false;
    ctx->noclobber_enabled = false;
    ctx->xtrace_enabled = false;
    ctx->verbose_enabled = false;
    ctx->braceexpand_disabled = false;
    ctx->allexport_enabled = false;
    ctx->errtrace_enabled = false;
    ctx->functrace_enabled = false;
    ctx->return_scope_depth = 0;
    ctx->text_depth = 0;
    /* A signal being ignored stays ignored across exec. A trap does not. */
    for (int i = 0; i < NSIG; ++i) {
        if (ctx->signal_traps[i].action != SHELL_TRAP_ACTION_IGNORE) {
            shellTrapClearEntry(&ctx->signal_traps[i]);
        }
    }
    shellTrapClearEntry(&ctx->exit_trap);
    /* The caller's to wait for, not this shell's. */
    for (size_t i = 0; i < ctx->job_count; ++i) {
        free(ctx->jobs[i].pids);
        free(ctx->jobs[i].command);
    }
    free(ctx->jobs);
    ctx->jobs = NULL;
    ctx->job_count = 0;
    ctx->finished_job_count = 0;
    ctx->last_bg_pid = 0;
    ctx->loops_around = 0;
    ctx->started_as_command = true;
    ctx->monitor_request = 0;
    for (size_t i = 0; i < ctx->dir_stack_count; ++i) {
        free(ctx->dir_stack[i]);
    }
    free(ctx->dir_stack);
    ctx->dir_stack = NULL;
    ctx->dir_stack_count = 0;
    ctx->dir_stack_capacity = 0;
    ctx->dir_stack_initialised = false;
    for (size_t i = 0; i < ctx->private_function_count; ++i) {
        free(ctx->private_functions[i].name);
        free(ctx->private_functions[i].parameter_metadata);
        free(ctx->private_functions[i].source);
    }
    free(ctx->private_functions);
    ctx->private_functions = NULL;
    ctx->private_function_count = 0;
    for (size_t i = 0; i < ctx->private_alias_count; ++i) {
        free(ctx->private_aliases[i].name);
        free(ctx->private_aliases[i].value);
    }
    free(ctx->private_aliases);
    ctx->private_aliases = NULL;
    ctx->private_alias_count = 0;
    free(ctx->private_options);
    ctx->private_options = NULL;
    ctx->has_private_tables = false;
    shellCopyDefinitionTables(ctx, NULL, 0, NULL, 0, gShellOptionTable);
    return ctx;
}

typedef struct {
    ShellRuntimeState *ctx;
    ShellRuntimeState *previous;
    ShellVariableAttribute *attributes;
    size_t attribute_count;
    bool interactive;
} ShellProgramScope;

static void shellProgramScopeEnter(ShellProgramScope *scope) {
    /* A script run as a command is at no prompt, whichever thread runs it. It has no job control. */
    scope->interactive = shellRuntimeIsInteractive();
    shellRuntimeSetInteractive(false);
    scope->ctx = shellRuntimeCreateProgramContext();
    scope->previous = scope->ctx ? shellRuntimeActivateContext(scope->ctx) : NULL;
#if defined(PSCAL_TARGET_IOS)
    /* As a new process would: what its task has ignored, from nohup or a `trap ''`, stays so. */
    if (scope->ctx) {
        shellRuntimeNoteIgnoredSignals();
    }
#endif
    scope->attributes = gShellVariableAttributes;
    scope->attribute_count = gShellVariableAttributeCount;
    gShellVariableAttributes = NULL;
    gShellVariableAttributeCount = 0;
}

static void shellProgramScopeLeave(ShellProgramScope *scope) {
    if (scope->ctx) {
        shellRuntimeActivateContext(scope->previous);
        shellRuntimeDestroyContext(scope->ctx);
        scope->ctx = NULL;
    }
    for (size_t i = 0; i < gShellVariableAttributeCount; ++i) {
        free(gShellVariableAttributes[i].name);
    }
    free(gShellVariableAttributes);
    gShellVariableAttributes = scope->attributes;
    gShellVariableAttributeCount = scope->attribute_count;
    shellRuntimeSetInteractive(scope->interactive);
}

/* A subshell sees the locals of the function it was started in, and what it does to them
 * stays its own. What it sets beyond those goes to its environment: nothing here is a
 * local just because a frame is there. skip_top leaves out frames that only mark it. */
void shellRuntimeContextInheritLocals(ShellRuntimeState *ctx, size_t skip_top) {
    const ShellRuntimeState *from = shellRuntimeCurrentContext();
    if (!ctx || !from) {
        return;
    }
    ctx->local_scope_active = false;
    ctx->pipeline_subshell_active = false;
    if (from->local_scope_depth <= skip_top) {
        return;
    }
    size_t depth = from->local_scope_depth - skip_top;
    ctx->local_scope_stack = (ShellLocalScopeFrame *)calloc(depth, sizeof(ShellLocalScopeFrame));
    if (!ctx->local_scope_stack) {
        return;
    }
    ctx->local_scope_capacity = depth;
    ctx->local_scope_depth = depth;
    for (size_t d = 0; d < depth; ++d) {
        const ShellLocalScopeFrame *src = &from->local_scope_stack[d];
        ShellLocalScopeFrame *dst = &ctx->local_scope_stack[d];
        if (src->count == 0) {
            continue;
        }
        dst->variables = (ShellLocalVariable *)calloc(src->count, sizeof(ShellLocalVariable));
        if (!dst->variables) {
            continue;
        }
        dst->capacity = src->count;
        for (size_t i = 0; i < src->count; ++i) {
            const ShellLocalVariable *var = &src->variables[i];
            ShellLocalVariable *copy = &dst->variables[dst->count];
            copy->name = var->name ? strdup(var->name) : NULL;
            copy->value = var->value ? strdup(var->value) : NULL;
            copy->is_array_literal = var->is_array_literal;
            copy->exported = var->exported;
            copy->no_value = var->no_value;
            copy->shadows_array = var->shadows_array;
            copy->shadows_associative = var->shadows_associative;
            copy->attributes = var->attributes;
            if (copy->name) {
                dst->count++;
            }
        }
    }
}

void shellRuntimeDestroyContext(ShellRuntimeState *ctx) {
    if (!ctx || ctx == &gShellRuntimeDefault) {
        return;
    }
    for (size_t d = 0; d < ctx->local_scope_depth; ++d) {
        shellLocalScopeFrameReset(&ctx->local_scope_stack[d]);
    }
    free(ctx->local_scope_stack);
    ctx->local_scope_stack = NULL;
    ctx->local_scope_depth = 0;
    ctx->local_scope_capacity = 0;
    for (size_t i = 0; i < ctx->private_function_count; ++i) {
        free(ctx->private_functions[i].name);
        free(ctx->private_functions[i].parameter_metadata);
        free(ctx->private_functions[i].source);
    }
    free(ctx->private_functions);
    ctx->private_functions = NULL;
    ctx->private_function_count = 0;
    for (size_t i = 0; i < ctx->private_alias_count; ++i) {
        free(ctx->private_aliases[i].name);
        free(ctx->private_aliases[i].value);
    }
    free(ctx->private_aliases);
    ctx->private_aliases = NULL;
    ctx->private_alias_count = 0;
    free(ctx->private_options);
    ctx->private_options = NULL;
    ctx->has_private_tables = false;
    if (ctx->history.entries) {
        for (size_t i = 0; i < ctx->history.count; ++i) {
            free(ctx->history.entries[i]);
        }
        free(ctx->history.entries);
        ctx->history.entries = NULL;
        ctx->history.count = 0;
        ctx->history.capacity = 0;
    }
    if (ctx->history_path) {
        free(ctx->history_path);
        ctx->history_path = NULL;
    }
    if (ctx->arg0) {
        free(ctx->arg0);
        ctx->arg0 = NULL;
    }
    for (int i = 0; i < NSIG; ++i) {
        shellTrapClearEntry(&ctx->signal_traps[i]);
    }
    shellTrapClearEntry(&ctx->exit_trap);
    if (ctx->jobs) {
        for (size_t i = 0; i < ctx->job_count; ++i) {
            ShellJob *job = &ctx->jobs[i];
            free(job->pids);
            job->pids = NULL;
            job->pid_count = 0;
            free(job->command);
            job->command = NULL;
        }
        free(ctx->jobs);
        ctx->jobs = NULL;
        ctx->job_count = 0;
    }
    if (ctx->array_vars) {
        for (size_t i = 0; i < ctx->array_var_count; ++i) {
            shellArrayVariableClear(&ctx->array_vars[i]);
        }
        free(ctx->array_vars);
        ctx->array_vars = NULL;
        ctx->array_var_count = 0;
        ctx->array_var_capacity = 0;
    }
    if (ctx->readonly_vars) {
        for (size_t i = 0; i < ctx->readonly_var_count; ++i) {
            free(ctx->readonly_vars[i].name);
        }
        free(ctx->readonly_vars);
        ctx->readonly_vars = NULL;
        ctx->readonly_var_count = 0;
        ctx->readonly_var_capacity = 0;
    }
    if (ctx->readonly_error_name) {
        free(ctx->readonly_error_name);
        ctx->readonly_error_name = NULL;
    }
    if (ctx->local_scope_stack) {
        size_t depth = ctx->local_scope_depth;
        for (size_t i = 0; i < depth; ++i) {
            shellLocalScopeFrameReset(&ctx->local_scope_stack[i]);
        }
        free(ctx->local_scope_stack);
        ctx->local_scope_stack = NULL;
        ctx->local_scope_depth = 0;
        ctx->local_scope_capacity = 0;
    }
    if (ctx->loop_stack) {
        size_t size = ctx->loop_stack_size;
        for (size_t i = 0; i < size; ++i) {
            shellLoopFrameFreeData(&ctx->loop_stack[i]);
        }
        free(ctx->loop_stack);
        ctx->loop_stack = NULL;
        ctx->loop_stack_size = 0;
        ctx->loop_stack_capacity = 0;
    }
    if (ctx->dir_stack) {
        for (size_t i = 0; i < ctx->dir_stack_count; ++i) {
            free(ctx->dir_stack[i]);
        }
        free(ctx->dir_stack);
        ctx->dir_stack = NULL;
        ctx->dir_stack_count = 0;
        ctx->dir_stack_capacity = 0;
    }
    if (ctx->disabled_builtins) {
        for (size_t i = 0; i < ctx->disabled_builtin_count; ++i) {
            free(ctx->disabled_builtins[i]);
        }
        free(ctx->disabled_builtins);
        ctx->disabled_builtins = NULL;
        ctx->disabled_builtin_count = 0;
        ctx->disabled_builtin_capacity = 0;
    }
    if (ctx->hash_entries) {
        for (size_t i = 0; i < ctx->hash_count; ++i) {
            free(ctx->hash_entries[i].name);
            free(ctx->hash_entries[i].path);
        }
        free(ctx->hash_entries);
        ctx->hash_entries = NULL;
        ctx->hash_count = 0;
        ctx->hash_capacity = 0;
    }
    free(ctx);
}

typedef enum {
    SHELL_READ_LINE_OK,
    SHELL_READ_LINE_EOF,
    SHELL_READ_LINE_TIMEOUT,
    SHELL_READ_LINE_INTERRUPTED,
    SHELL_READ_LINE_ERROR
} ShellReadLineResult;

static ShellReadLineResult shellReadLineFromFd(int fd,
                                               char **out_line,
                                               size_t *out_length) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_length) {
        *out_length = 0;
    }
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
    while (true) {
        char ch = '\0';
        ssize_t n = read(fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR) {
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
        if (length + 1 >= capacity) {
            size_t new_capacity = capacity * 2;
            char *resized = (char *)realloc(buffer, new_capacity);
            if (!resized) {
                free(buffer);
                return SHELL_READ_LINE_ERROR;
            }
            buffer = resized;
            capacity = new_capacity;
        }
        buffer[length++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    buffer[length] = '\0';
    if (out_line) {
        *out_line = buffer;
    } else {
        free(buffer);
    }
    if (out_length) {
        *out_length = length;
    }
    return SHELL_READ_LINE_OK;
}

static ShellReadLineResult __attribute__((unused))
shellReadLineFromStream(FILE *stream,
                        char **out_line,
                        size_t *out_length) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_length) {
        *out_length = 0;
    }
    if (!stream) {
        return SHELL_READ_LINE_ERROR;
    }

    size_t capacity = 128;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        return SHELL_READ_LINE_ERROR;
    }

    size_t length = 0;
    bool saw_any = false;
    while (true) {
        int ch = fgetc(stream);
        if (ch == EOF) {
            if (ferror(stream)) {
                free(buffer);
                clearerr(stream);
                return SHELL_READ_LINE_ERROR;
            }
            if (!saw_any) {
                free(buffer);
                return SHELL_READ_LINE_EOF;
            }
            break;
        }
        saw_any = true;
        if (length + 1 >= capacity) {
            size_t new_capacity = capacity * 2;
            char *resized = (char *)realloc(buffer, new_capacity);
            if (!resized) {
                free(buffer);
                return SHELL_READ_LINE_ERROR;
            }
            buffer = resized;
            capacity = new_capacity;
        }
        buffer[length++] = (char)ch;
        if (ch == '\n') {
            break;
        }
    }

    buffer[length] = '\0';
    if (out_line) {
        *out_line = buffer;
    } else {
        free(buffer);
    }
    if (out_length) {
        *out_length = length;
    }
    return SHELL_READ_LINE_OK;
}

static bool shellAssignLoopVariable(const char *name, const char *value) {
    if (!name) {
        return false;
    }
    return shellSetTrackedVariable(name, value, false);
}

/* A loop's or case's own words have no command to say which line they are on. */
static void shellNoteChunkLine(VM *vm) {
    if (vm && vm->chunk && vm->chunk->lines && vm->lastInstruction) {
        int line = vm->chunk->lines[vm->lastInstruction - vm->chunk->code];
        if (line > 0) {
            gShellCurrentCommandLine = line;
        }
    }
}

/* A clause of for ((;;)) is one expression, and a failure is reported as for ((...)). The loop
 * ends with status 1, which set -e does not take for a command that failed. */
static bool shellLoopExecuteCForExpression(const char *expr) {
    long long value = 0;
    shellRuntimeEnterCondition();
    bool ok = shellArithmeticEvaluate(expr, &value, "((");
    shellRuntimeLeaveCondition();
    return ok;
}

static void shellLoopCForFailed(ShellLoopFrame *frame) {
    frame->status = 1;
    shellRuntimeEnterCondition();
    shellUpdateStatus(1);
    shellRuntimeLeaveCondition();
}

static bool shellLoopEvaluateConditionText(const char *expr, bool *out_ready) {
    if (!expr) {
        if (out_ready) {
            *out_ready = true;
        }
        shellUpdateStatus(0);
        return true;
    }
    shellRuntimeEnterCondition();
    /* No condition at all is true. */
    long long value = 1;
    if (*expr && !shellArithmeticEvaluate(expr, &value, "((")) {
        shellRuntimeLeaveCondition();
        return false;
    }
    if (out_ready) {
        *out_ready = value != 0;
    }
    shellUpdateStatus(0);
    if (!gShellExitRequested) {
        gShellRuntime.errexit_pending = false;
        gShellRuntime.errexit_defer_active = false;
        if (gShellCurrentVm) {
            gShellCurrentVm->exit_requested = false;
            gShellCurrentVm->abort_requested = false;
            gShellCurrentVm->current_builtin_name = NULL;
        }
    }
    shellRuntimeLeaveCondition();
    return true;
}

static bool shellLoopEvaluateCForCondition(ShellLoopFrame *frame, bool *out_ready) {
    if (!frame || !out_ready) {
        return false;
    }
    if (frame->cfor_condition_cached) {
        *out_ready = frame->cfor_condition_value;
        return true;
    }
    bool ready = false;
    if (!shellLoopEvaluateConditionText(frame->cfor_condition, &ready)) {
        return false;
    }
    frame->cfor_condition_cached = true;
    frame->cfor_condition_value = ready;
    *out_ready = ready;
    return true;
}
static bool shellLoopExecuteCForInitializer(ShellLoopFrame *frame) {
    if (!frame) {
        return false;
    }
    frame->cfor_condition_cached = false;
    if (!frame->cfor_init || frame->cfor_init[0] == '\0') {
        return true;
    }
    if (!shellLoopExecuteCForExpression(frame->cfor_init)) {
        frame->skip_body = true;
        frame->break_pending = true;
        return false;
    }
    return true;
}

static bool shellLoopExecuteCForUpdate(ShellLoopFrame *frame) {
    if (!frame) {
        return false;
    }
    frame->cfor_condition_cached = false;
    if (!frame->cfor_update || frame->cfor_update[0] == '\0') {
        return true;
    }
    return shellLoopExecuteCForExpression(frame->cfor_update);
}

static ShellLoopFrame *shellLoopPushFrame(ShellLoopKind kind) {
    if (!shellLoopEnsureCapacity(gShellLoopStackSize + 1)) {
        return NULL;
    }
    ShellLoopFrame frame;
    frame.kind = kind;
    frame.status = 0;
    frame.keep_status = false;
    frame.skip_body = false;
    frame.break_pending = false;
    frame.continue_pending = false;
    frame.for_variable = NULL;
    frame.for_values = NULL;
    frame.for_count = 0;
    frame.for_index = 0;
    frame.for_active = false;
    frame.cfor_init = NULL;
    frame.cfor_condition = NULL;
    frame.cfor_update = NULL;
    frame.cfor_condition_cached = false;
    frame.cfor_condition_value = false;
    frame.cond_kind = SHELL_LOOP_CONDITION_NONE;
    frame.cond_words = NULL;
    frame.cond_word_count = 0;
    frame.cond_arith_expression = NULL;
    frame.body_kind = SHELL_LOOP_BODY_KIND_NONE;
    frame.body_words = NULL;
    frame.body_word_count = 0;
    frame.body_arith_expression = NULL;
    frame.redirs_active = false;
    frame.applied_redirs = NULL;
    frame.applied_redir_count = 0;
    frame.redir_backups = NULL;
    frame.redir_backup_count = 0;
    frame.redir_backup_capacity = 0;
    frame.scope_depth = 0;
    gShellLoopStack[gShellLoopStackSize++] = frame;
    return &gShellLoopStack[gShellLoopStackSize - 1];
}

static ShellLoopFrame *shellLoopTop(void) {
    if (gShellLoopStackSize == 0) {
        return NULL;
    }
    return &gShellLoopStack[gShellLoopStackSize - 1];
}

static void shellLoopPopFrame(void) {
    if (gShellLoopStackSize == 0) {
        return;
    }
    ShellLoopFrame *frame = &gShellLoopStack[gShellLoopStackSize - 1];
    shellLoopFrameFreeData(frame);
    gShellLoopStackSize--;
    if (gShellLoopStackSize == 0) {
        gShellRuntime.break_requested = false;
        gShellRuntime.continue_requested = false;
        gShellRuntime.break_requested_levels = 0;
        gShellRuntime.continue_requested_levels = 0;
    }
}

static void shellSignalHandler(int signo) {
    if (signo <= 0 || signo >= NSIG) {
        return;
    }
#if defined(PSCAL_TARGET_IOS)
    /* A stage keeps these two from the shell, and what it holds ends with it. */
    int task = (signo == SIGINT || signo == SIGQUIT) ? 0 : vprocHandlerTaskPid();
    vprocNoteCaughtSignal(task > 0 ? task : shellRuntimeOwnPid(), signo);
#else
    gShellPendingSignals[signo] = 1;
#endif
}

static bool shellLoopSkipActive(void) {
    for (size_t i = gShellLoopStackSize; i > 0; --i) {
        if (gShellLoopStack[i - 1].skip_body) {
            return true;
        }
    }
    return false;
}

static void shellLoopRequestBreakLevels(int levels) {
    if (levels <= 0) {
        levels = 1;
    }
    size_t idx = gShellLoopStackSize;
    while (idx > 0 && levels > 0) {
        ShellLoopFrame *frame = &gShellLoopStack[idx - 1];
        frame->skip_body = true;
        frame->break_pending = true;
        frame->continue_pending = false;
        idx--;
        levels--;
    }
    if (levels > 0) {
        shellRuntimeRequestExit();
    }
}

static void shellLoopRequestContinueLevels(int levels) {
    if (levels <= 0) {
        levels = 1;
    }
    if (gShellLoopStackSize == 0) {
        return;
    }
    size_t idx = gShellLoopStackSize;
    int remaining = levels;
    while (idx > 0 && remaining > 1) {
        ShellLoopFrame *frame = &gShellLoopStack[idx - 1];
        frame->skip_body = true;
        frame->break_pending = true;
        frame->continue_pending = false;
        idx--;
        remaining--;
    }
    if (idx > 0) {
        ShellLoopFrame *target = &gShellLoopStack[idx - 1];
        target->skip_body = true;
        target->continue_pending = true;
        target->break_pending = false;
    }
}

VM *shellSwapCurrentVm(VM *vm) {
    VM *previous = gShellCurrentVm;
    shellContextPush(shellGetContext());
    gShellCurrentVm = vm;
    ShellRuntimeState *ctx = NULL;
    if (vm && vm->frontendContext) {
        ctx = (ShellRuntimeState *)vm->frontendContext;
    }
    shellSetContext(ctx);
    return previous;
}

void shellRestoreCurrentVm(VM *vm) {
    gShellCurrentVm = vm;
    ShellRuntimeState *restore = shellContextPop();
    shellSetContext(restore);
}

static void shellJobControlDetectCapabilities(void) {
    ShellJobControlCaps caps;
    memset(&caps, 0, sizeof(caps));

    caps.has_job_control = shellRuntimeIsInteractive();
#if defined(PSCAL_TARGET_IOS)
    caps.has_tcsetpgrp = false;
    caps.has_pgid = true;
    caps.has_stop_signals = true;
    caps.has_foreground_control = true;
    caps.has_killpg = true;
#else
    caps.has_tcsetpgrp = true;
    caps.has_pgid = true;
    caps.has_stop_signals = true;
    caps.has_foreground_control = true;
    caps.has_killpg = true;
#endif

    gShellRuntime.job_control_caps = caps;
}

static inline bool shellJobControlSupportsStops(void) {
    return gShellRuntime.job_control_caps.has_stop_signals;
}

#if defined(__GNUC__) || defined(__clang__)
#define SHELL_JC_UNUSED __attribute__((unused))
#else
#define SHELL_JC_UNUSED
#endif

static SHELL_JC_UNUSED void shellJobControlSaveShellIo(void) {
    ShellJobControlContext *jc = &gShellJobControlCtx;
    if (!jc) {
        return;
    }
    if (!jc->saved_stdio_valid) {
        for (int i = 0; i < 3; ++i) {
            jc->saved_stdio[i] = dup(i);
            if (jc->saved_stdio[i] >= 0) {
                shellSlotFcntl(jc->saved_stdio[i], F_SETFD, FD_CLOEXEC);
            }
        }
        jc->saved_stdio_valid = true;
    }
    if (!jc->saved_termios_valid && gShellRuntime.tty_fd >= 0 && tcgetattr(gShellRuntime.tty_fd, &jc->saved_termios) == 0) {
        jc->saved_termios_valid = true;
    }
}

static SHELL_JC_UNUSED void shellJobControlRestoreShellIo(void) {
    ShellJobControlContext *jc = &gShellJobControlCtx;
    if (!jc) {
        return;
    }
    if (jc->saved_stdio_valid) {
        for (int i = 0; i < 3; ++i) {
            if (jc->saved_stdio[i] >= 0) {
                (void)dup2(jc->saved_stdio[i], i);
            }
        }
    }
    if (jc->saved_termios_valid && gShellRuntime.tty_fd >= 0) {
        (void)tcsetattr(gShellRuntime.tty_fd, TCSADRAIN, &jc->saved_termios);
    }
}

static int shellJobControlSendPgid(pid_t pgid, int signo);

static bool shellJobControlDispatchForegroundSignal(int signo) {
    if (!shellMonitorOn() || gShellJobControlCtx.fg_pgid <= 0) {
        return false;
    }
    if (signo == SIGTSTP && !shellJobControlSupportsStops()) {
        return false;
    }
    return shellJobControlSendPgid(gShellJobControlCtx.fg_pgid, signo) == 0;
}

static bool shellJobControlAllowedHeadless(void) {
    return gShellRuntime.monitor_request > 0 || getenv("RUN_VPROC_TESTS") != NULL ||
           getenv("PSCALI_SHELL_VPROC") != NULL;
}

static void shellInitJobControlState(void) {
    if (gShellRuntime.job_control_initialized) {
        return;
    }

    shellJobControlDetectCapabilities();
    gShellRuntime.job_control_initialized = true;
    gShellRuntime.tty_fd = STDIN_FILENO;

    /* In automated harness runs we sometimes need job control without a real
     * TTY (non-interactive pipelines). Allow RUN_VPROC_TESTS/PSCALI_SHELL_VPROC
     * to bypass TTY requirements on all platforms. `set -m` asks for it anywhere. */
    bool allow_headless_jc = shellJobControlAllowedHeadless();

    if (gShellRuntime.tty_fd < 0) {
        gShellRuntime.tty_fd = -1;
        gShellRuntime.job_control_caps.has_job_control = allow_headless_jc;
        gShellRuntime.job_control_caps.has_foreground_control = false;
        if (!allow_headless_jc) return;
    }

    if (!allow_headless_jc && !pscalRuntimeFdHasRealTTY(gShellRuntime.tty_fd)) {
        gShellRuntime.tty_fd = -1;
        gShellRuntime.job_control_caps.has_job_control = false;
        gShellRuntime.job_control_caps.has_foreground_control = false;
        return;
    }

    struct sigaction ignore_action;
    memset(&ignore_action, 0, sizeof(ignore_action));
    sigemptyset(&ignore_action.sa_mask);
    ignore_action.sa_handler = SIG_IGN;
    (void)sigaction(SIGTTIN, &ignore_action, NULL);
    (void)sigaction(SIGTTOU, &ignore_action, NULL);

#if defined(PSCAL_TARGET_IOS)
    pid_t shell_pid = (pid_t)vprocGetShellSelfPid();
    if (shell_pid <= 0) {
        shell_pid = getpid();
    }
    pid_t current_pgid = (shell_pid > 0) ? (pid_t)vprocGetPgid((int)shell_pid) : -1;
    if (current_pgid <= 0) {
        current_pgid = shell_pid;
    }
    gShellRuntime.shell_pgid = current_pgid;
    gShellRuntime.job_control_enabled = true;
    if (shell_pid > 0 && current_pgid > 0) {
        int sid = vprocGetSid((int)shell_pid);
        if (sid > 0) {
            vprocSetForegroundPgid(sid, (int)current_pgid);
        }
    }
    return;
#else
    pid_t shell_pid = getpid();
    pid_t current_pgid = getpgrp();
    if (current_pgid <= 0 && allow_headless_jc) {
        current_pgid = shell_pid > 0 ? shell_pid : getpid();
    } else if (current_pgid != shell_pid) {
        if (setpgid(0, 0) == 0) {
            current_pgid = shell_pid;
        } else {
            current_pgid = getpgrp();
        }
    }

    gShellRuntime.shell_pgid = current_pgid;
#endif
}

static void shellEnsureJobControl(void) {
    bool allow_headless_jc = shellJobControlAllowedHeadless();
#if !defined(PSCAL_TARGET_IOS)
    /* A subshell has none unless it asked with `set -m`, which a script's subshells keep. In one in
     * the background, taking the terminal stopped it for good. */
    if (gShellRuntime.in_subshell && gShellRuntime.monitor_request <= 0) {
        gShellRuntime.job_control_enabled = false;
        gShellRuntime.job_control_caps.has_job_control = false;
        return;
    }
#endif
    if (!shellRuntimeIsInteractive() && !allow_headless_jc) {
        gShellRuntime.job_control_enabled = false;
        gShellRuntime.job_control_caps.has_job_control = false;
        return;
    }

    shellInitJobControlState();

    if (gShellRuntime.tty_fd < 0) {
        gShellRuntime.job_control_enabled = false;
        gShellRuntime.job_control_caps.has_job_control = false;
        return;
    }
#if defined(PSCAL_TARGET_IOS)
    gShellRuntime.job_control_enabled = true;
    if (gShellRuntime.shell_pgid <= 0) {
        int shell_pid = vprocGetShellSelfPid();
        if (shell_pid > 0) {
            gShellRuntime.shell_pgid = vprocGetPgid(shell_pid);
        } else {
            gShellRuntime.shell_pgid = getpgrp();
        }
        if (gShellRuntime.shell_pgid <= 0 && shell_pid > 0) {
            gShellRuntime.shell_pgid = shell_pid;
        }
    }
    return;
#endif

    pid_t pgid = getpgrp();
    if (pgid <= 0) {
        gShellRuntime.job_control_enabled = false;
        gShellRuntime.job_control_caps.has_job_control = false;
        gShellRuntime.job_control_caps.has_foreground_control = false;
        return;
    }

    gShellRuntime.shell_pgid = pgid;
#if defined(PSCAL_TARGET_IOS)
    int shell_pid = vprocGetShellSelfPid();
    int sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : -1;
    if (sid > 0) {
        vprocSetForegroundPgid(sid, (int)pgid);
    }
#endif

    while (true) {
        pid_t foreground = tcgetpgrp(gShellRuntime.tty_fd);
        if (foreground < 0) {
            if (errno == EINTR) {
                continue;
            }
            gShellRuntime.job_control_enabled = false;
            gShellRuntime.job_control_caps.has_job_control = false;
            gShellRuntime.job_control_caps.has_foreground_control = false;
            return;
        }
        if (foreground == pgid) {
            gShellRuntime.job_control_enabled = true;
            return;
        }
        /* A job with job control inside it (`set -m`) stays in the background. */
        if (gShellRuntime.in_subshell) {
            gShellRuntime.job_control_enabled = true;
            return;
        }
        if (tcsetpgrp(gShellRuntime.tty_fd, pgid) != 0) {
            if (errno == EINTR) {
                continue;
            }
            gShellRuntime.job_control_enabled = false;
            gShellRuntime.job_control_caps.has_job_control = false;
            gShellRuntime.job_control_caps.has_foreground_control = false;
            return;
        }
    }
}

/* What `$-` and `set -o` say: what was asked for, or a shell at a prompt on a terminal. */
static bool shellMonitorShown(void) {
    if (gShellRuntime.monitor_request != 0) {
        return gShellRuntime.monitor_request > 0;
    }
    return !gShellRuntime.in_subshell && shellRuntimeIsInteractive() && gShellRuntime.job_control_enabled;
}

/* Job control as bash means it. On, a command gets a group of its own and the terminal. Off, in
 * a script, a subshell or a shell with no terminal, it stays in this shell's group, the one the
 * terminal knows and `kill 0` names, so a key reaches the shell and its command alike.
 * RUN_VPROC_TESTS turns on the rest of the machinery for a script, and not this. */
static bool shellMonitorOn(void) {
    if (gShellRuntime.monitor_request == 0 && !gShellRuntime.in_subshell && shellRuntimeIsInteractive()) {
        shellEnsureJobControl();
    }
    return shellMonitorShown();
}

/* The group this shell's commands are in when they have none of their own. */
static pid_t shellOwnGroup(void) {
#if defined(PSCAL_TARGET_IOS)
    VProc *vp = vprocCurrent();
    int pid = vp ? vprocPid(vp) : vprocGetShellSelfPid();
    return (pid_t)(pid > 0 ? vprocGetPgid(pid) : -1);
#else
    return getpgrp();
#endif
}

/* A job started with no job control shares the shell's group: to reach it, signal its commands. */
static bool shellGroupIsItsOwn(pid_t pgid) {
    return pgid > 0 && pgid != shellOwnGroup();
}

static void shellJobControlSetForeground(pid_t pgid) {
    if (!gShellRuntime.job_control_enabled || !gShellRuntime.job_control_caps.has_foreground_control ||
        gShellRuntime.tty_fd < 0 || pgid <= 0) {
        return;
    }
    gShellJobControlCtx.fg_pgid = pgid;
#if defined(PSCAL_TARGET_IOS)
    int shell_pid = vprocGetShellSelfPid();
    int sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : -1;
    if (sid > 0) {
        vprocSetForegroundPgid(sid, (int)pgid);
    }
#endif
    if (!gShellRuntime.job_control_caps.has_tcsetpgrp) {
        return;
    }
    while (tcsetpgrp(gShellRuntime.tty_fd, pgid) != 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

static void shellJobControlRestoreForeground(void) {
    if (!gShellRuntime.job_control_enabled || !gShellRuntime.job_control_caps.has_foreground_control ||
        gShellRuntime.tty_fd < 0) {
        return;
    }
    gShellJobControlCtx.fg_pgid = 0;
#if defined(PSCAL_TARGET_IOS)
    int shell_pid = vprocGetShellSelfPid();
    int sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : -1;
    /* The shell's group as it is now. The one kept from when job control started can be from
     * before the shell's setsid: a second tab's shell starts in its parent's group, so the
     * terminal went back to the first tab's shell after every command, and the shell's own
     * changes to its terminal were then a background job's (SIGTTOU). */
    const int live_pgid = (shell_pid > 0) ? vprocGetPgid(shell_pid) : -1;
    if (live_pgid > 0) {
        gShellRuntime.shell_pgid = live_pgid;
    }
    if (sid > 0 && gShellRuntime.shell_pgid > 0) {
        vprocSetForegroundPgid(sid, (int)gShellRuntime.shell_pgid);
    }
#endif
    if (!gShellRuntime.job_control_caps.has_tcsetpgrp) {
        return;
    }
    pid_t target = gShellRuntime.shell_pgid > 0 ? gShellRuntime.shell_pgid : getpgrp();
    if (target <= 0) {
        return;
    }
    while (tcsetpgrp(gShellRuntime.tty_fd, target) != 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

static int shellJobControlSendSignal(pid_t target, int signo, bool process_group) {
    if (target == 0 || signo < 0) {
        errno = EINVAL;
        return -1;
    }
#if defined(PSCAL_TARGET_IOS)
    pid_t send_target = process_group ? -target : target;
    int rc = vprocKillShim(send_target, signo);
    if (rc != 0 && errno == ESRCH) {
        bool virtual_only =
            (signo == SIGINT || signo == SIGQUIT || signo == SIGTSTP ||
             signo == SIGTERM || signo == SIGCONT || signo == SIGSTOP ||
             signo == SIGTTIN || signo == SIGTTOU);
        if (!virtual_only) {
            /* Non-control signals may still target host-backed children in some
             * test environments; preserve that compatibility path. */
            rc = kill(send_target, signo);
        }
    }
    return rc;
#else
    pid_t send_target = process_group ? -target : target;
    return kill(send_target, signo);
#endif
}

static int shellJobControlSendPgid(pid_t pgid, int signo) {
    return shellJobControlSendSignal(pgid, signo, true);
}

static int shellJobControlSendPid(pid_t pid, int signo) {
    return shellJobControlSendSignal(pid, signo, false);
}

typedef struct {
    char *subject;
    bool matched;
    int incoming_status;
    bool skipped;
} ShellCaseContext;

typedef struct {
    ShellCaseContext *items;
    size_t count;
    size_t capacity;
} ShellCaseContextStack;

static PSCAL_THREAD_LOCAL ShellCaseContextStack gShellCaseStack = {NULL, 0, 0};

static bool shellCaseStackPush(const char *subject_text) {
    if (!subject_text) {
        subject_text = "";
    }
    char *copy = strdup(subject_text);
    if (!copy) {
        return false;
    }
    if (gShellCaseStack.count + 1 > gShellCaseStack.capacity) {
        size_t new_capacity = gShellCaseStack.capacity ? gShellCaseStack.capacity * 2 : 4;
        ShellCaseContext *items = realloc(gShellCaseStack.items, new_capacity * sizeof(ShellCaseContext));
        if (!items) {
            free(copy);
            return false;
        }
        gShellCaseStack.items = items;
        gShellCaseStack.capacity = new_capacity;
    }
    ShellCaseContext *ctx = &gShellCaseStack.items[gShellCaseStack.count++];
    ctx->subject = copy;
    ctx->matched = false;
    ctx->skipped = false;
    return true;
}

static ShellCaseContext *shellCaseStackTop(void) {
    if (gShellCaseStack.count == 0) {
        return NULL;
    }
    return &gShellCaseStack.items[gShellCaseStack.count - 1];
}

static void shellCaseStackPop(void) {
    if (gShellCaseStack.count == 0) {
        return;
    }
    ShellCaseContext *ctx = &gShellCaseStack.items[gShellCaseStack.count - 1];
    free(ctx->subject);
    ctx->subject = NULL;
    gShellCaseStack.count--;
    if (gShellCaseStack.count == 0 && gShellCaseStack.items) {
        free(gShellCaseStack.items);
        gShellCaseStack.items = NULL;
        gShellCaseStack.capacity = 0;
    }
}

static void shellLoopPopFrameFlushed(void) {
    ShellLoopFrame *frame = shellLoopTop();
    if (frame && frame->redirs_active) {
        shellFlushStandardStreams();
    }
    shellLoopPopFrame();
}

/* return stops a chunk before it reaches __shell_loop_end or __shell_case_end, so the caller
 * of the chunk ends what was left open. A frame left on the stack is the one the loop around
 * it goes on to advance, and that loop then never ends. exit leaves the loops alone: their
 * redirections are still in place when the EXIT trap runs, as under bash. */
static void shellUnwindChunk(size_t loop_depth, size_t case_depth) {
    while (gShellCaseStack.count > case_depth) {
        shellCaseStackPop();
    }
    if (gShellExitRequested) {
        return;
    }
    while (gShellLoopStackSize > loop_depth) {
        shellLoopPopFrameFlushed();
    }
}

bool shellRuntimeConsumeDiscard(void) {
    bool pending = gShellDiscardPending;
    gShellDiscardPending = false;
    gShellDiscardInText = false;
    return pending;
}

bool shellRuntimeDiscardStaysInText(void) {
    return gShellDiscardPending && gShellDiscardInText;
}

void shellRuntimeDiscardBase(ShellDiscardBase *out) {
    out->loop_depth = gShellLoopStackSize;
    out->case_depth = gShellCaseStack.count;
    out->condition_depth = gShellConditionDepth;
}

/* What the shell last read has been dropped, part way through, and it reads on. The outermost
 * chunk of a context comes here with NULL, and all of it was that command's. Eval, a sourced
 * file or a trap's text gives what was there when it started, which goes on running. */
void shellRuntimeAbandonCommand(const ShellDiscardBase *base) {
    shellUnwindChunk(base ? base->loop_depth : 0, base ? base->case_depth : 0);
    gShellConditionDepth = base ? base->condition_depth : 0;
    gShellRuntime.break_requested = false;
    gShellRuntime.break_requested_levels = 0;
    gShellRuntime.continue_requested = false;
    gShellRuntime.continue_requested_levels = 0;
    gShellRuntime.errexit_pending = false;
    gShellRuntime.errexit_defer_active = false;
    if (!base) {
        shellResetPipeline();
    }
}

/* A substitution sees the caller's ERR and RETURN traps only under set -E and set -T, and what
 * it sets stays in there, as a DEBUG trap it sets does. These are per thread rather than per
 * context. */
void *shellRuntimeSubstitutionTrapsPutAside(void) {
    ShellCallerTraps *saved = (ShellCallerTraps *)malloc(sizeof(ShellCallerTraps));
    if (!saved) {
        return NULL;
    }
    saved->err = gShellErrTrap;
    saved->ret = gShellReturnTrap;
    saved->debug = gShellDebugTrap;
    memset(&gShellErrTrap, 0, sizeof(gShellErrTrap));
    memset(&gShellReturnTrap, 0, sizeof(gShellReturnTrap));
    memset(&gShellDebugTrap, 0, sizeof(gShellDebugTrap));
    if (saved->debug.command) {
        (void)shellTrapSetCommand(&gShellDebugTrap, saved->debug.command);
    }
    if (saved->err.command) {
        (void)shellTrapSetCommand(&gShellErrTrap, saved->err.command);
        gShellErrTrap.dormant = saved->err.dormant || !gShellRuntime.errtrace_enabled;
    }
    if (saved->ret.command) {
        (void)shellTrapSetCommand(&gShellReturnTrap, saved->ret.command);
        gShellReturnTrap.dormant = saved->ret.dormant || !gShellRuntime.functrace_enabled;
    }
    return saved;
}

void shellRuntimeSubstitutionTrapsBack(void *saved) {
    if (!saved) {
        return;
    }
    shellTrapClearEntry(&gShellErrTrap);
    shellTrapClearEntry(&gShellReturnTrap);
    shellTrapClearEntry(&gShellDebugTrap);
    gShellErrTrap = ((ShellCallerTraps *)saved)->err;
    gShellReturnTrap = ((ShellCallerTraps *)saved)->ret;
    gShellDebugTrap = ((ShellCallerTraps *)saved)->debug;
    free(saved);
}

/* A script run as a command starts over: there is no call for `return` to end. */
void shellRuntimeContextResetReturnScope(ShellRuntimeState *ctx) {
    if (ctx) {
        ctx->return_scope_depth = 0;
    }
    gShellThreadReturnScope = 0;
}

void shellRuntimeContextSetTextDepth(ShellRuntimeState *ctx, int depth) {
    if (ctx) {
        ctx->text_depth = depth;
    }
}

typedef enum {
    SHELL_META_SUBSTITUTION_DOLLAR,
    SHELL_META_SUBSTITUTION_BACKTICK
} ShellMetaSubstitutionStyle;

typedef struct {
    ShellMetaSubstitutionStyle style;
    size_t span_length;
    char *command;
} ShellMetaSubstitution;

#if defined(__GNUC__) || defined(__clang__)
#define SHELL_STATIC_UNUSED static inline __attribute__((unused))
#else
#define SHELL_STATIC_UNUSED static inline
#endif

typedef struct {
    char *command;
    char *result;
    size_t result_length;
    int status;
    size_t generation;
    unsigned long hits;
    unsigned long stores;
} ShellScalarCacheEntry;

typedef struct {
    ShellScalarCacheEntry *entry;
    char *command;
    char *capture;
    size_t capture_length;
    bool from_cache;
} ShellScalarCaptureFrame;

static __thread ShellScalarCacheEntry *gShellScalarCache = NULL;
static __thread size_t gShellScalarCacheCount = 0;
static __thread size_t gShellScalarCacheCapacity = 0;
static __thread size_t gShellScalarCacheGeneration = 0;

static __thread ShellScalarCaptureFrame *gShellScalarCaptureStack = NULL;
static __thread size_t gShellScalarCaptureDepth = 0;
static __thread size_t gShellScalarCaptureCapacity = 0;

SHELL_STATIC_UNUSED void shellScalarCacheEntryDispose(ShellScalarCacheEntry *entry) {
    if (!entry) {
        return;
    }
    free(entry->command);
    entry->command = NULL;
    free(entry->result);
    entry->result = NULL;
    entry->result_length = 0;
    entry->generation = 0;
    entry->hits = 0;
    entry->stores = 0;
}

SHELL_STATIC_UNUSED bool shellScalarCacheEnsureCapacity(size_t needed) {
    if (gShellScalarCacheCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellScalarCacheCapacity ? (gShellScalarCacheCapacity * 2) : 4;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    ShellScalarCacheEntry *resized =
        (ShellScalarCacheEntry *)realloc(gShellScalarCache, new_capacity * sizeof(ShellScalarCacheEntry));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellScalarCacheCapacity; i < new_capacity; ++i) {
        resized[i].command = NULL;
        resized[i].result = NULL;
        resized[i].result_length = 0;
        resized[i].generation = 0;
        resized[i].hits = 0;
        resized[i].stores = 0;
    }
    gShellScalarCache = resized;
    gShellScalarCacheCapacity = new_capacity;
    return true;
}

SHELL_STATIC_UNUSED bool shellScalarCacheEntryEquals(const ShellScalarCacheEntry *entry,
                                                     const char *command) {
    if (!entry || !command || !entry->command) {
        return false;
    }
    return strcmp(entry->command, command) == 0;
}

SHELL_STATIC_UNUSED ShellScalarCacheEntry *shellScalarCacheFind(const char *command) {
    if (!command || *command == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < gShellScalarCacheCount;) {
        ShellScalarCacheEntry *entry = &gShellScalarCache[i];
        if (entry->generation != gShellScalarCacheGeneration) {
            shellScalarCacheEntryDispose(entry);
            if (i + 1 < gShellScalarCacheCount) {
                memmove(&gShellScalarCache[i],
                        &gShellScalarCache[i + 1],
                        (gShellScalarCacheCount - i - 1) * sizeof(ShellScalarCacheEntry));
            }
            gShellScalarCacheCount--;
            continue;
        }
        if (shellScalarCacheEntryEquals(entry, command)) {
            return entry;
        }
        ++i;
    }
    return NULL;
}

SHELL_STATIC_UNUSED ShellScalarCacheEntry *shellScalarCacheStore(const char *command,
                                                                 const char *result,
                                                                 size_t result_length) {
    if (!command || *command == '\0') {
        return NULL;
    }
    size_t copy_length = 0;
    if (result) {
        copy_length = result_length ? result_length : strlen(result);
    }
    char *copy = (char *)malloc(copy_length + 1);
    if (!copy) {
        return NULL;
    }
    if (copy_length > 0 && result) {
        memcpy(copy, result, copy_length);
    }
    copy[copy_length] = '\0';

    ShellScalarCacheEntry *existing = shellScalarCacheFind(command);
    if (existing) {
        free(existing->result);
        existing->result = copy;
        existing->result_length = copy_length;
        existing->generation = gShellScalarCacheGeneration;
        existing->hits = 0;
        existing->stores++;
        gShellRuntime.scalar_cache_generation = gShellScalarCacheGeneration;
        return existing;
    }

    if (!shellScalarCacheEnsureCapacity(gShellScalarCacheCount + 1)) {
        free(copy);
        return NULL;
    }
    ShellScalarCacheEntry *entry = &gShellScalarCache[gShellScalarCacheCount];
    entry->command = strdup(command);
    if (!entry->command) {
        free(copy);
        return NULL;
    }
    entry->result = copy;
    entry->result_length = copy_length;
    entry->status = 0;
    entry->generation = gShellScalarCacheGeneration;
    entry->hits = 0;
    entry->stores = 1;
    gShellScalarCacheCount++;
    gShellRuntime.scalar_cache_generation = gShellScalarCacheGeneration;
    return entry;
}

SHELL_STATIC_UNUSED void shellScalarCacheRemoveAt(size_t index) {
    if (index >= gShellScalarCacheCount) {
        return;
    }
    shellScalarCacheEntryDispose(&gShellScalarCache[index]);
    if (index + 1 < gShellScalarCacheCount) {
        memmove(&gShellScalarCache[index],
                &gShellScalarCache[index + 1],
                (gShellScalarCacheCount - index - 1) * sizeof(ShellScalarCacheEntry));
    }
    gShellScalarCacheCount--;
}

SHELL_STATIC_UNUSED void shellScalarCacheClear(void) {
    for (size_t i = 0; i < gShellScalarCacheCount; ++i) {
        shellScalarCacheEntryDispose(&gShellScalarCache[i]);
    }
    free(gShellScalarCache);
    gShellScalarCache = NULL;
    gShellScalarCacheCount = 0;
    gShellScalarCacheCapacity = 0;
    gShellScalarCacheGeneration = 0;
    gShellRuntime.scalar_cache_generation = gShellScalarCacheGeneration;
}

SHELL_STATIC_UNUSED void shellScalarCacheInvalidate(void) {
    for (size_t i = 0; i < gShellScalarCacheCount; ++i) {
        shellScalarCacheEntryDispose(&gShellScalarCache[i]);
    }
    gShellScalarCacheCount = 0;
    if (gShellScalarCacheGeneration == SIZE_MAX) {
        gShellScalarCacheGeneration = 0;
    } else {
        gShellScalarCacheGeneration++;
    }
    gShellRuntime.scalar_cache_generation = gShellScalarCacheGeneration;
}

SHELL_STATIC_UNUSED bool shellCaptureStackEnsureCapacity(size_t needed) {
    if (gShellScalarCaptureCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellScalarCaptureCapacity ? (gShellScalarCaptureCapacity * 2) : 4;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    ShellScalarCaptureFrame *resized =
        (ShellScalarCaptureFrame *)realloc(gShellScalarCaptureStack,
                                           new_capacity * sizeof(ShellScalarCaptureFrame));
    if (!resized) {
        return false;
    }
    for (size_t i = gShellScalarCaptureCapacity; i < new_capacity; ++i) {
        resized[i].entry = NULL;
        resized[i].command = NULL;
        resized[i].capture = NULL;
        resized[i].capture_length = 0;
        resized[i].from_cache = false;
    }
    gShellScalarCaptureStack = resized;
    gShellScalarCaptureCapacity = new_capacity;
    return true;
}

SHELL_STATIC_UNUSED bool shellCaptureStackPush(ShellScalarCacheEntry *entry,
                                               const char *command,
                                               const char *capture,
                                               size_t capture_length,
                                               bool from_cache) {
    if (!shellCaptureStackEnsureCapacity(gShellScalarCaptureDepth + 1)) {
        return false;
    }
    ShellScalarCaptureFrame *frame = &gShellScalarCaptureStack[gShellScalarCaptureDepth];
    frame->entry = entry;
    frame->command = command ? strdup(command) : NULL;
    if (command && !frame->command) {
        return false;
    }
    frame->capture = NULL;
    frame->capture_length = 0;
    if (capture) {
        size_t length = capture_length;
        if (length == 0) {
            length = strlen(capture);
        }
        frame->capture = (char *)malloc(length + 1);
        if (!frame->capture) {
            free(frame->command);
            frame->command = NULL;
            return false;
        }
        memcpy(frame->capture, capture, length);
        frame->capture[length] = '\0';
        frame->capture_length = length;
    }
    frame->from_cache = from_cache;
    gShellScalarCaptureDepth++;
    if (gShellScalarCaptureDepth > gShellRuntime.scalar_capture_peak_depth) {
        gShellRuntime.scalar_capture_peak_depth = gShellScalarCaptureDepth;
    }
    return true;
}

SHELL_STATIC_UNUSED void shellCaptureStackPop(void) {
    if (gShellScalarCaptureDepth == 0) {
        return;
    }
    size_t index = gShellScalarCaptureDepth - 1;
    ShellScalarCaptureFrame *frame = &gShellScalarCaptureStack[index];
    free(frame->command);
    frame->command = NULL;
    free(frame->capture);
    frame->capture = NULL;
    frame->capture_length = 0;
    frame->entry = NULL;
    frame->from_cache = false;
    gShellScalarCaptureDepth--;
    if (gShellScalarCaptureDepth == 0) {
        free(gShellScalarCaptureStack);
        gShellScalarCaptureStack = NULL;
        gShellScalarCaptureCapacity = 0;
    }
}

SHELL_STATIC_UNUSED ShellScalarCaptureFrame *shellCaptureStackTop(void) {
    if (gShellScalarCaptureDepth == 0) {
        return NULL;
    }
    return &gShellScalarCaptureStack[gShellScalarCaptureDepth - 1];
}

SHELL_STATIC_UNUSED void shellCaptureStackClear(void) {
    while (gShellScalarCaptureDepth > 0) {
        shellCaptureStackPop();
    }
}

#undef SHELL_STATIC_UNUSED
