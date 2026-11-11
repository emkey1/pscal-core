#pragma once

#include <sys/resource.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <dirent.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

// Clean-room virtual process abstraction for iOS builds. Maintains a private
// fd table per task and translates basic I/O syscalls through the table.

typedef struct VProc VProc;
struct pscal_fd;
struct termios;

typedef struct {
    int stdin_fd;   // host fd to dup for stdin; -1 to dup host STDIN_FILENO; -2 for /dev/null
    int stdout_fd;  // host fd to dup for stdout; -1 to dup host STDOUT_FILENO
    int stderr_fd;  // host fd to dup for stderr; -1 to dup host STDERR_FILENO
    int winsize_cols;
    int winsize_rows;
    int pid_hint;   // optional fixed synthetic pid; -1 for auto
    int job_id;     // optional job id to stamp on creation
} VProcOptions;

typedef struct {
    int cols;
    int rows;
} VProcWinsize;

typedef enum {
    VPROC_ISOLATION_DOMAIN_NEXTVI = 0,
    VPROC_ISOLATION_DOMAIN_MICRO,
    VPROC_ISOLATION_DOMAIN_COUNT
} VProcIsolationDomain;

typedef struct {
    int pid;
    pthread_t tid;
    int parent_pid;
    int pgid;
    int sid;
    int tty_pty_num;
    bool exited;
    bool stopped;
    bool continued;
    bool zombie;
    int exit_signal;
    int status;
    int stop_signo;
    bool sigchld_pending;
    int rusage_utime;
    int rusage_stime;
    int fg_pgid;
    int job_id;
    char comm[16];
    char command[64];
} VProcSnapshot;

/* One open descriptor of a vproc, as /proc/<pid>/fd shows it. */
typedef struct {
    int fd;
    int open_flags;
    off_t pos;
} VProcFdSnapshot;

#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)

VProcOptions vprocDefaultOptions(void);
VProc *vprocCreate(const VProcOptions *opts);
void vprocDestroy(VProc *vp);
int vprocPid(VProc *vp);
int vprocReservePid(void);

void vprocActivate(VProc *vp);
void vprocDeactivate(void);
VProc *vprocCurrent(void);
bool vprocWaitIfStopped(VProc *vp);

size_t vprocSnapshot(VProcSnapshot *out, size_t capacity);
/* The table of the vproc with this pid, whichever thread asks: its open slots numbered
 * below capacity, lowest first. Returns how many, or -1 if no vproc has the pid. */
ssize_t vprocSnapshotFds(int pid, VProcFdSnapshot *out, size_t capacity);

int vprocTranslateFd(VProc *vp, int fd);
struct pscal_fd *vprocGetPscalFd(VProc *vp, int fd);
int vprocCreateInprocPipe(struct pscal_fd **out_read, struct pscal_fd **out_write);
/* The lowest free slot from next_fd up; the table retains the descriptor. */
int vprocInsertPscalFd(VProc *vp, struct pscal_fd *fd);
int vprocDup(VProc *vp, int fd);
int vprocDup2(VProc *vp, int fd, int target);
int vprocDupAbove(VProc *vp, int fd, int min);
int vprocFreeFdAbove(VProc *vp, int min);
void vprocMarkFdInherited(VProc *vp, int fd);
void vprocShellFdFollow(int fd);
/* Sync vproc table entry to a host fd already duplicated onto target_fd. */
int vprocRestoreHostFd(VProc *vp, int target_fd, int host_src);
/* A stdio slot becomes a copy of host_src for good, as between fork and exec: nothing of what
 * it held stays open. The caller keeps host_src. */
int vprocReplaceStdio(VProc *vp, int target_fd, int host_src);
int vprocClose(VProc *vp, int fd);
int vprocPipe(VProc *vp, int pipefd[2]);
/* Duplicate a host fd onto a target host descriptor, bypassing shim indirection. */
int vprocHostDup2(int host_fd, int target_fd);
/* Duplicate a host descriptor without routing through the shim table. */
int vprocHostDup(int fd);
/* Open a host path without routing through the shim table. */
int vprocHostOpen(const char *path, int flags, ...);
/* Create a host pipe without routing through the shim table. */
int vprocHostPipe(int pipefd[2]);
int vprocHostPipeUnowned(int pipefd[2]);
/* Create a host socket without routing through the shim table. */
int vprocHostSocket(int domain, int type, int protocol);
/* Accept a host socket without routing through the shim table. */
int vprocHostAccept(int fd, struct sockaddr *addr, socklen_t *addrlen);
/* Create a host socketpair without routing through the shim table. */
int vprocHostSocketpair(int domain, int type, int protocol, int sv[2]);
/* Seek on a host descriptor without routing through the shim table. */
off_t vprocHostLseek(int fd, off_t offset, int whence);
/* Sync a host descriptor without routing through the shim table. */
int vprocHostFsync(int fd);
/* Close a host descriptor without routing through the shim table. */
int vprocHostClose(int fd);
/* A host descriptor as the shims should take it, where they would look for a slot of
 * that number. poll, close, fstat, ioctl and the other shims that translate honour it.
 * It does not fit in an fd_set. close still wants a number this vproc holds. */
int vprocTagHostFd(int fd);
/* Read from a host descriptor without routing through the shim table. */
ssize_t vprocHostRead(int fd, void *buf, size_t count);
/* Write to a host descriptor without routing through the shim table. */
ssize_t vprocHostWrite(int fd, const void *buf, size_t count);
/* vprocHostWrite for task PID's output on a pipe, FIFO or socket: it waits where a kill can
 * act, and gives up with EINTR once PID is killed or gone, whichever vproc the thread is in. */
ssize_t vprocHostWriteForTask(int pid, int fd, const void *buf, size_t count);
/* Poll host descriptors without routing through the shim table. */
int vprocHostPoll(struct pollfd *fds, nfds_t nfds, int timeout);
/* The host's signal disposition, not the one the current vproc has recorded. */
int vprocHostSigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
/* fflush(NULL) locks every stream in the process, and a stream keeps its lock through a
 * read or write that blocks. Commands wait for each other through pipes, so that walk can
 * wait for ever: `pascal x | tr a b`, where tr needs pascal's end of the pipe closed. It
 * also writes another command's buffer to the caller's descriptor when the stream goes
 * through vproc slots. A stream that can block or that belongs to one vproc's descriptors
 * stays out of the walk, and whoever writes it flushes it. __SIGN is libc's own flag for
 * this. A refill clears it on a line-buffered or unbuffered stream, so a stream that reads
 * has to be fully buffered. */
static inline void vprocStreamSkipFlushAll(FILE *stream) {
#if defined(__SIGN)
    stream->_flags = (short)(stream->_flags | __SIGN);
#else
    (void)stream;
#endif
}
/* Closes a thread's own stream as the thread ends, from a key's destructor. Every normal
 * end has flushed already (halt, freeVM, a worker's job, a stage's end), so what's left is
 * mostly a kill's, which a real process would lose too. WRITE_OUT says the stream is on a
 * regular file, which can't block: on a pipe a stalled reader would keep this thread from
 * ever ending, and through a vproc slot the vproc may be gone and the data would reach the
 * host's descriptor of that number. Nothing here may use the thread's vproc. */
static inline void vprocStreamEndWithThread(FILE *stream, bool write_out) {
    if (write_out) {
        (void)fflush(stream);
    } else {
#if defined(__APPLE__)
        (void)fpurge(stream);
#endif
    }
    (void)fclose(stream);
}
/* Virtual isatty shim for session-backed TTYs. */
int vprocIsattyShim(int fd);
/* True when the current vproc's fd 0 is a file, pipe, socket or other non-terminal
 * rather than the session's terminal. */
bool vprocStdinIsRedirected(void);
/* One try at reading a redirected fd 0, for a caller that unwinds by itself on Ctrl-C
 * and Ctrl-Z. It never waits. Returns the count, 0 at end of input, or -1 with errno
 * EINTR (SIGINT or SIGTSTP is pending, and stays pending), EAGAIN (nothing to read yet)
 * or the read's own error. */
ssize_t vprocReadRedirectedStdin(void *buf, size_t count);
/* Enable/disable the virtual location device (/dev/location). */
void vprocLocationDeviceSetEnabled(bool enabled);
/* Write a payload to the virtual location device. */
ssize_t vprocLocationDeviceWrite(const void *data, size_t len);
typedef void (*VprocLocationReadersChangedFn)(int readers, void *context);
/* Register an observer for reader count changes on /dev/location. */
void vprocLocationDeviceRegisterReaderObserver(VprocLocationReadersChangedFn cb, void *context);
/* Read from the shared session input queue (stdin) on iOS. */
ssize_t vprocSessionReadInputShim(void *buf, size_t count);
/* Read from the shared session input queue with nonblocking support. */
ssize_t vprocSessionReadInputShimMode(void *buf, size_t count, bool nonblocking);
/* Spawn a joinable host thread, bypassing vproc's pthread_create shim. */
int vprocHostPthreadCreate(pthread_t *thread,
                           const pthread_attr_t *attr,
                           void *(*start_routine)(void *),
                           void *arg);
/* Adopt an existing host fd into the vproc table, returning the vproc-local fd. */
int vprocAdoptHostFd(VProc *vp, int host_fd);
/* fileno() whose result the read, fstat, lseek and dup shims accept. The stream
 * keeps owning the descriptor: close it with fclose(), not close(). */
int vprocFilenoShim(FILE *stream);
/* ftruncate and futimens on a vproc fd. */
int vprocFtruncateShim(int fd, off_t length);
int vprocFutimensShim(int fd, const struct timespec times[2]);
int vprocFchdirShim(int fd);
/* fcntl, pread and pwrite on a vproc fd. On one with no host descriptor (an
 * in-process pipe, a PTY) pread and pwrite fail ESPIPE and locks fail EINVAL. A
 * lock on a host file belongs to the process, as every fcntl lock does, so it does
 * not keep two vprocs apart. O_NONBLOCK does not change on the session's own
 * terminal, which the session's reader shares. */
int vprocFcntlShim(int fd, int cmd, ...);
ssize_t vprocPreadShim(int fd, void *buf, size_t count, off_t offset);
ssize_t vprocPwriteShim(int fd, const void *buf, size_t count, off_t offset);
/* Inherit the current vproc into new threads created via pthread_create shim. */
int vprocPthreadCreateShim(pthread_t *thread,
                           const pthread_attr_t *attr,
                           void *(*start_routine)(void *),
                           void *arg);
int vprocIsolationEnter(VProcIsolationDomain domain);
int vprocIsolationTryEnter(VProcIsolationDomain domain);
void vprocIsolationLeave(VProcIsolationDomain domain);
int vprocOpenAt(VProc *vp, const char *path, int flags, int mode);
int vprocSetWinsize(VProc *vp, int cols, int rows);
int vprocGetWinsize(VProc *vp, VProcWinsize *out);

/* Task lifecycle management for virtual processes (iOS clean-room). */
int vprocRegisterTidHint(int pid, pthread_t tid);
int vprocRegisterThread(VProc *vp, pthread_t tid);
void vprocUnregisterThread(VProc *vp, pthread_t tid);
int vprocSpawnThread(VProc *vp, void *(*start_routine)(void *), void *arg, pthread_t *thread_out);
/* For a thread that has only borrowed the current vproc: its owner's vprocDestroy waits for vprocUnpin.
 * NULL when there is none or it is already going. Don't block while holding one. */
VProc *vprocPinCurrent(void);
void vprocUnpin(VProc *vp);
/* Before vprocMarkExit: whoever waits for the task may go on to use the pipes it held. */
void vprocCloseDescriptors(VProc *vp);
void vprocMarkExit(VProc *vp, int status);
/* For a thread being unwound: ends the vprocs it made for itself that are active above keep, or
 * all of them with NULL, and leaves keep current. */
void vprocEndAbandoned(VProc *keep);
void vprocSetParent(int pid, int parent_pid);
/* What pid started becomes its parent's, for a vproc that stands for no process of its own. */
void vprocPassChildrenToParent(int pid);
int vprocSetPgid(int pid, int pgid);
int vprocSetSid(int pid, int sid);
int vprocGetPgid(int pid);
int vprocGetSid(int pid);
bool vprocGetShellJobControlState(int *shell_pid_out,
                                  int *shell_pgid_out,
                                  int *sid_out,
                                  int *fg_pgid_out);
/* Request Ctrl-C/Ctrl-Z style control-signal routing to the foreground job. */
bool vprocRequestControlSignal(int sig);
/* Request control-signal routing using an explicit shell pid hint. */
bool vprocRequestControlSignalForShell(int shell_pid, int sig);
/* Request control-signal routing using a session-id scoped PTY foreground group. */
bool vprocRequestControlSignalForSession(uint64_t session_id, int sig);
void vprocSessionSetControlBytePassthrough(uint64_t session_id, bool enabled);
bool vprocSessionGetControlBytePassthrough(uint64_t session_id);
void vprocSetStopUnsupported(int pid, bool stop_unsupported);
void vprocSetCooperativeStopWait(int pid, bool enabled);
bool vprocGetStopUnsupported(int pid);
void vprocSetShellPromptReadActive(int pid, bool active);
bool vprocShellPromptReadActive(int pid);
void vprocSetPipelineStage(bool active);
/* Did the interrupt key end this thread's reads of the terminal? Asking clears it,
 * so the next command reads again. */
bool vprocTakeTerminalInterrupted(void);
int vprocSetForegroundPgid(int sid, int fg_pgid);
int vprocGetForegroundPgid(int sid);
int vprocNextJobIdSeed(void);
void vprocMarkGroupExit(int pid, int status);
void vprocSetRusage(int pid, int utime, int stime);
int vprocBlockSignals(int pid, int mask);
int vprocUnblockSignals(int pid, int mask);
int vprocIgnoreSignal(int pid, int mask);
int vprocDefaultSignal(int pid, int mask);
int vprocSigaction(int pid, int sig, const struct sigaction *act, struct sigaction *old);

// Shimmed syscalls: respect the active vproc on the current thread when set,
// otherwise fall back to real libc/syscall equivalents.
ssize_t vprocReadShim(int fd, void *buf, size_t count);
ssize_t vprocWriteShim(int fd, const void *buf, size_t count);
/* write() as vprocWriteShim, but a reader that has gone is only EPIPE, never the writing task's
 * SIGPIPE: for host code reached through the app's interposer, which sets its signal dispositions
 * on the host and not on a task. */
ssize_t vprocWriteShimNoSignal(int fd, const void *buf, size_t count);
int vprocDupShim(int fd);
int vprocDup2Shim(int fd, int target);
int vprocCloseShim(int fd);
int vprocFsyncShim(int fd);

// The error flag of stdout or stderr, for this thread: vproc_stdio_shim.h writes past
// the stream, whose own flag never sees a failure. Holds the errno of the failed write.
void vprocStdioSetWriteError(int fd, int err);
int vprocStdioWriteError(int fd);
int vprocPipeShim(int pipefd[2]);
int vprocSocketShim(int domain, int type, int protocol);
int vprocAcceptShim(int fd, struct sockaddr *addr, socklen_t *addrlen);
int vprocSocketpairShim(int domain, int type, int protocol, int sv[2]);
int vprocFstatShim(int fd, struct stat *st);
int vprocStatShim(const char *path, struct stat *st);
int vprocLstatShim(const char *path, struct stat *st);
int vprocChdirShim(const char *path);
char *vprocGetcwdShim(char *buffer, size_t size);
int vprocShellChdirShim(const char *path);
char *vprocShellGetcwdShim(char *buffer, size_t size);
/* A task's file mask, as umask() would have it: the first for the task itself, the second for
 * the shell it is running, as a cd reaches it. The mask is not the host's. */
mode_t vprocUmaskShim(mode_t mask);
mode_t vprocShellUmaskShim(mode_t mask);
bool vprocFileMaskIsHostMask(void);
/* A task's resource limits, kept with its file mask and handed to what it starts. Set for the
 * shell it is running, as a cd reaches it; `restore` is a scope ending, which may raise a hard
 * limit back. Open files holds the task's descriptors to the limit; the rest are only kept. */
int vprocGetrlimitShim(int resource, struct rlimit *limit);
int vprocShellSetrlimitShim(int resource, const struct rlimit *limit, bool restore);
int vprocHostMkdirMasked(const char *path, mode_t mode);
int vprocHostMkfifoMasked(const char *path, mode_t mode);
int vprocHostMknodMasked(const char *path, mode_t mode, dev_t dev);
/* Host descriptors and host paths, as libc's would be, for code past the shims: what they make
 * obeys the calling task's mask, not the host's. */
int vprocHostOpenMasked(const char *path, int flags, ...);
int vprocHostOpenatMasked(int dirfd, const char *path, int flags, ...);
int vprocHostMkdiratMasked(int dirfd, const char *path, mode_t mode);
FILE *vprocHostFopenMasked(const char *path, const char *mode);
/* For a thread no task runs on, such as the Go runtime's: what it makes obeys `mask`, the file mask of
 * the task that started its program, not the host's. A negative mask is the host's. */
int vprocHostOpenatWithMask(int mask, int dirfd, const char *path, int flags, mode_t mode);
int vprocHostMkdiratWithMask(int mask, int dirfd, const char *path, mode_t mode);
int vprocMkfifoShim(const char *path, mode_t mode);
int vprocMknodShim(const char *path, mode_t mode, dev_t dev);
/* A task's user and group ids, kept with its file mask and handed to what it starts, by POSIX's
 * rules. A session starts as the login account: the host's uid, with its group and groups from
 * /etc/passwd and /etc/group. Root is uid 0, which only exec of a set-user-id program gives. */
uid_t vprocGetuidShim(void);
uid_t vprocGeteuidShim(void);
gid_t vprocGetgidShim(void);
gid_t vprocGetegidShim(void);
int vprocGetgroupsShim(int size, gid_t list[]);
int vprocSetuidShim(uid_t uid);
int vprocSeteuidShim(uid_t uid);
int vprocSetreuidShim(uid_t ruid, uid_t euid);
int vprocSetgidShim(gid_t gid);
int vprocSetegidShim(gid_t gid);
int vprocSetregidShim(gid_t rgid, gid_t egid);
int vprocSetgroupsShim(int size, const gid_t list[]);
int vprocInitgroupsShim(const char *user, gid_t gid);
/* What a normal user may not touch. host_path is the host's, after expansion; op is
 * VPROC_PERM_READ, VPROC_PERM_WRITE (a file's contents or attributes) or VPROC_PERM_ENTRY (making,
 * removing or renaming the name itself, which doesn't follow a final symlink). 0, or -1 with EACCES.
 * Only a task is checked: the app's own threads have none. */
#define VPROC_PERM_READ 1
#define VPROC_PERM_WRITE 2
#define VPROC_PERM_ENTRY 4
int vprocHostPathPermit(const char *host_path, int op);
/* The same for open's flags: what they read and write. */
int vprocHostPathPermitOpen(const char *host_path, int flags);
/* open's flags for an fopen mode. */
int vprocFopenModeFlags(const char *mode);
/* The owner a file shows: what the installer put in /etc belongs to root. */
void vprocHostPathOwner(const char *host_path, struct stat *st);
/* exec, which never returns once the program can run: the program runs as a call on this task, which
 * then ends with its status. -1 with errno, as exec sets it, if it can't run. The app says how a
 * program runs; nothing can replace the app. */
typedef bool (*VProcExecCanRunFn)(const char *path);
typedef int (*VProcExecRunFn)(int argc, char **argv);
void vprocSetExecHooks(VProcExecCanRunFn can_run, VProcExecRunFn run);
/* As exec does for a program the app starts some other way: a set-user-id one gets root. */
void vprocExecApplyCredentials(const char *program);
int vprocExecveShim(const char *path, char *const argv[], char *const envp[]);
int vprocExecvShim(const char *path, char *const argv[]);
int vprocExecvpShim(const char *file, char *const argv[]);
int vprocExeclShim(const char *path, const char *arg0, ...);
int vprocExeclpShim(const char *file, const char *arg0, ...);
struct VProcProgramFrame;
/* What a program frame's setjmp returned, made a status: an exec's own status, or 128 + a signal. */
int vprocProgramFrameStatus(const struct VProcProgramFrame *frame, int jumped);
/* The host name, the kernel's: /etc/hostname at launch, otherwise iDevice. Only root may set it,
 * and a set lasts until the app ends. */
int vprocGethostnameShim(char *name, size_t len);
int vprocSethostnameShim(const char *name, size_t len);
struct utsname;
int vprocUnameShim(struct utsname *buf);
/* A number for this launch of the app, which a file left by an earlier one can't have. */
uint64_t vprocBootId(void);
/* A subshell's environment and directory. There is no fork here, so a subshell is a
 * thread and a vproc, and these keep what a process would keep for itself. A thread with
 * no environment of its own sees the process's, and one with no subshell pid the
 * session shell's directory. */
typedef struct VProcEnv VProcEnv;
VProcEnv *vprocEnvCapture(void);
/* NULL-terminated, the shape of environ, and only safe to walk on a copy no one else changes. */
char **vprocEnvEntries(VProcEnv *env);
/* A copy of only the entries keep says yes to, so one left out costs nothing. */
VProcEnv *vprocEnvCaptureFiltered(bool (*keep)(const char *entry, void *arg), void *arg);
/* What the shell knows about this environment's names. Freed with it. */
void *vprocEnvAttachment(VProcEnv *env);
void vprocEnvSetAttachment(VProcEnv *env, void *data, void (*free_data)(void *));
VProcEnv *vprocEnvRetain(VProcEnv *env);
void vprocEnvRelease(VProcEnv *env);
VProcEnv *vprocEnvCurrent(void);
VProcEnv *vprocEnvActivate(VProcEnv *env);
/* As basename and dirname, with an answer that is the calling thread's until it next asks. */
char *vprocBasenameShim(char *path);
char *vprocDirnameShim(char *path);
/* As setlocale, for the calling thread alone. libc's is the whole app's, and it crashes if two
 * threads load a locale at once. */
char *vprocSetlocaleShim(int category, const char *name);
/* As newlocale with no base, under the lock vprocSetlocaleShim loads with. Returns a locale_t. */
void *vprocLocaleLoad(int mask, const char *name);
/* A program on the shell's thread has its locale end with it. */
void *vprocLocaleSave(void);
void vprocLocaleRestore(void *saved);
char *vprocGetenvShim(const char *name);
int vprocSetenvShim(const char *name, const char *value, int overwrite);
int vprocUnsetenvShim(const char *name);
char ***vprocEnvironShim(void);
int vprocSubshellPid(void);
void vprocSetSubshellPid(int pid);
/* True once since the last call if a write on this thread found its reader gone. */
bool vprocConsumeClosedPipe(void);
int vprocAccessShim(const char *path, int mode);
int vprocChmodShim(const char *path, mode_t mode);
int vprocChownShim(const char *path, uid_t uid, gid_t gid);
int vprocFchmodShim(int fd, mode_t mode);
int vprocFchownShim(int fd, uid_t uid, gid_t gid);
int vprocUtimesShim(const char *path, const struct timeval times[2]);
int vprocFutimesShim(int fd, const struct timeval times[2]);
int vprocMkdirShim(const char *path, mode_t mode);
int vprocRmdirShim(const char *path);
int vprocUnlinkShim(const char *path);
int vprocRemoveShim(const char *path);
int vprocRenameShim(const char *oldpath, const char *newpath);
DIR *vprocOpendirShim(const char *name);
int vprocSymlinkShim(const char *target, const char *linkpath);
ssize_t vprocReadlinkShim(const char *path, char *buf, size_t size);
char *vprocRealpathShim(const char *path, char *resolved_path);
char *vprocTtynameShim(int fd);
int vprocIoctlShim(int fd, unsigned long request, ...);
off_t vprocLseekShim(int fd, off_t offset, int whence);
int vprocOpenShim(const char *path, int flags, ...);
/* The descriptor a path names, however it is spelled: /dev/stdin, /dev/stdout, /dev/stderr,
 * /dev/fd/N, /proc/self/fd/N and /proc/<pid>/fd/N of any vproc. -1 for any other path, and with
 * no vproc. Only tells that it is one, and which: the slot is this vproc's unless the path has
 * another's pid. Opening one acts as Linux's /proc links do: see vprocOpenNamedFd. */
int vprocPathNamedFd(const char *path);
/* A terminal vproc answers itself: /dev/tty, /dev/console, /dev/ttyN, /dev/ptmx, /dev/pts/N.
 * The host's own device of that name is never the one meant. */
bool vprocPathIsTerminal(const char *path);
/* What /proc/<pid>/fd/<fd> reads as on Linux: a path (the host's, for a file), pipe:[N] or
 * socket:[N]. False if that vproc has no such descriptor. */
bool vprocFdLinkTarget(int pid, int fd, char *out, size_t size);
int vprocPollShim(struct pollfd *fds, nfds_t nfds, int timeout);
int vprocSelectShim(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
pid_t vprocWaitPidShim(pid_t pid, int *status_out, int options);
int vprocKillShim(pid_t pid, int sig);
/* kill(), for the kernel's own use: a task that has ended doesn't count, where to a program it does.
 * Ctrl-C and Ctrl-Z have other ways to stop a command when no one in a group is alive to take them. */
int vprocKillLive(pid_t pid, int sig);
/* kill() for a task and everything under it, for what a host does with a process group: a
 * stage's group is its own here, so a group kill does not reach what a nested shell runs. */
int vprocKillTree(pid_t pid, int sig);
pid_t vprocGetPidShim(void);
pid_t vprocGetPpidShim(void);
pid_t vprocGetpgrpShim(void);
pid_t vprocGetpgidShim(pid_t pid);
int vprocSetpgidShim(pid_t pid, pid_t pgid);
pid_t vprocSetsidShim(void);
pid_t vprocGetsidShim(pid_t pid);
pid_t vprocTcgetpgrpShim(int fd);
int vprocTcsetpgrpShim(int fd, pid_t pgid);
void vprocSetShellSelfPid(int pid);
int vprocGetShellSelfPid(void);
void vprocSetShellSelfTid(pthread_t tid);
bool vprocIsShellSelfThread(void);
/* Resolve account names from container passwd/group databases on iOS. */
int vprocLookupPasswdName(uid_t uid, char *buffer, size_t buffer_len);
int vprocLookupGroupName(gid_t gid, char *buffer, size_t buffer_len);
/* Optional: identify a per-session "kernel" vproc that acts as adoptive parent. */
void vprocSetKernelPid(int pid);
int vprocGetKernelPid(void);
void vprocClearKernelPidGlobal(void);
int vprocGetSessionKernelPid(void);
void vprocSetSessionKernelPid(int pid);
/* Ensure a shared kernel vproc exists and return its pid. */
int vprocEnsureKernelPid(void);
#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void vprocResetStartupFstabStateForTests(void);
void vprocRunPathTruncationDefaultForTests(void);
#endif

/* Per-session stdio ownership: duplicated host fds that define the
 * controlling stdio for a given shell window/session. */
typedef struct VProcSessionInput {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned char *buf;
    size_t off;
    size_t len;
    size_t cap;
    int reader_fd;
    uint64_t reader_generation;
    bool inited;
    bool eof;
    bool reader_active;
    bool interrupt_pending;
    bool interrupt_key;   /* interrupt_pending is there for the interrupt key */
    bool stop_requested;
    /* End-of-file keys not yet read, oldest first: how many queued bytes come
     * before each. Each one reads as 0 bytes, once, and input goes on after it. */
#define VPROC_SESSION_EOF_MARKS 8
    size_t eof_mark[VPROC_SESSION_EOF_MARKS];
    size_t eof_mark_count;
} VProcSessionInput;

typedef struct VProcSessionStdio {
    int stdin_host_fd;
    int stdout_host_fd;
    int stderr_host_fd;
    int kernel_pid;
    int shell_pid;
    VProcSessionInput *input;
    struct pscal_fd *stdin_pscal_fd;
    struct pscal_fd *stdout_pscal_fd;
    struct pscal_fd *stderr_pscal_fd;
    struct pscal_fd *pty_master;
    struct pscal_fd *pty_slave;
    pthread_t pty_out_thread;
    bool pty_active;
    bool control_bytes_passthrough;
    uint64_t session_id;
} VProcSessionStdio;

typedef void (*VProcSessionOutputHandler)(uint64_t session_id,
                                          const unsigned char *data,
                                          size_t len,
                                          void *context);

/* Obtain the current session stdio (per window/session). */
VProcSessionStdio *vprocSessionStdioCurrent(void);
/* Initialize session stdio from the current host stdio and kernel pid. */
void vprocSessionStdioInit(VProcSessionStdio *stdio_ctx, int kernel_pid);
/* Initialize session stdio from explicit host fds (duplicates are taken). */
void vprocSessionStdioInitWithFds(VProcSessionStdio *stdio_ctx,
                                  int stdin_fd,
                                  int stdout_fd,
                                  int stderr_fd,
                                  int kernel_pid);
int vprocSessionStdioInitWithPty(VProcSessionStdio *stdio_ctx,
                                 struct pscal_fd *pty_slave,
                                 struct pscal_fd *pty_master,
                                 uint64_t session_id,
                                 int kernel_pid);
int vprocAdoptPscalStdio(VProc *vp,
                         struct pscal_fd *stdin_fd,
                         struct pscal_fd *stdout_fd,
                         struct pscal_fd *stderr_fd);
int vprocAdoptPscalFd(VProc *vp, int target_fd, struct pscal_fd *pscal_fd);
int vprocSetSessionWinsize(uint64_t session_id, int cols, int rows);
int vprocGetSessionWinsize(uint64_t session_id, int *cols_out, int *rows_out);
bool vprocSessionStdioNeedsRefresh(VProcSessionStdio *stdio_ctx);
void vprocSessionStdioRefresh(VProcSessionStdio *stdio_ctx, int kernel_pid);
void vprocSessionDebugDumpShim(const char *tag);
bool vprocSessionStdioIsDefault(VProcSessionStdio *stdio_ctx);
/* Activate a session stdio context for the calling thread (per window). */
void vprocSessionStdioActivate(VProcSessionStdio *stdio_ctx);
/* Termios helpers that always target the active session PTY (if present). */
bool vprocSessionStdioFetchTermios(int fd, struct termios *termios);
bool vprocSessionStdioApplyTermios(int fd, int action, const struct termios *termios);
/* The terminal fd names now, held for use where this thread's vproc and session are gone, such
 * as a pthread key destructor. NULL when fd is no terminal. */
typedef struct VProcTerminalHold VProcTerminalHold;
VProcTerminalHold *vprocTerminalHold(int fd);
bool vprocTerminalHoldFetchTermios(VProcTerminalHold *hold, struct termios *termios);
bool vprocTerminalHoldApplyTermios(VProcTerminalHold *hold, const struct termios *termios);
void vprocTerminalHoldRelease(VProcTerminalHold *hold);
/* Create/destroy a heap-backed session stdio for custom sessions. */
VProcSessionStdio *vprocSessionStdioCreate(void);
void vprocSessionStdioDestroy(VProcSessionStdio *stdio_ctx);
/* Replace the default (main) session stdio snapshot. */
void vprocSessionStdioSetDefault(VProcSessionStdio *stdio_ctx);
/* Register/unregister per-session PTY output callbacks. */
void vprocSessionSetOutputHandler(uint64_t session_id,
                                  VProcSessionOutputHandler handler,
                                  void *context);
void vprocSessionClearOutputHandler(uint64_t session_id);
void vprocSessionSetOutputPaused(uint64_t session_id, bool paused);
/* Emit output bytes to a session's registered output handler/backlog. */
ssize_t vprocSessionEmitOutput(uint64_t session_id, const void *buf, size_t len);
/* Write input data directly to a session PTY master. */
ssize_t vprocSessionWriteToMaster(uint64_t session_id, const void *buf, size_t len);
ssize_t vprocSessionWriteToMasterMode(uint64_t session_id, const void *buf, size_t len, bool blocking);
/* Ensure session input is initialized for the current session. */
VProcSessionInput *vprocSessionInputEnsureShim(void);
/* The session's reader stops, waiting up to a second for it: a shell ending does this before it
 * closes its copies of the terminal. */
void vprocSessionInputStop(VProcSessionStdio *session);
/* Inject input into the current session input queue. */
bool vprocSessionInjectInputShim(const void *data, size_t len);

/* Terminate and discard all vprocs in the given session (sid). */
void vprocTerminateSession(int sid);
/* Terminate and discard all vprocs associated with a runtime session id. */
bool vprocTerminateSessionById(uint64_t session_id);
/* Minimal signal queries/suspension helpers. */
int vprocSigpending(int pid, sigset_t *set);
int vprocSigsuspend(int pid, const sigset_t *mask);
int vprocSigprocmask(int pid, int how, const sigset_t *set, sigset_t *oldset);
int vprocSigwait(int pid, const sigset_t *set, int *sig);
int vprocSigtimedwait(int pid, const sigset_t *set, const struct timespec *timeout, int *sig);

/* Optional synthetic metadata: store/retrieve a stable job id for a vproc pid. */
void vprocSetJobId(int pid, int job_id);
int vprocGetJobId(int pid);
void vprocSetCommandLabel(int pid, const char *label);
bool vprocGetCommandLabel(int pid, char *buf, size_t buf_len);
void vprocDiscard(int pid);
bool vprocSigchldPending(int pid);
int vprocSetSigchldBlocked(int pid, bool block);
void vprocClearSigchldPending(int pid);
/* Runs the handlers of what waits on this task, if the caller is its main thread. Returns a
 * signal that would end a task that unwinds itself, taken off the queue, or 0. */
int vprocDeliverTaskSignals(int pid);
/* For a handler that only notes its signal, as the shell's does: any of a task's threads may run
 * it, so it notes on vprocHandlerTaskPid(), and the note stays with the task until its main thread
 * takes it. Masks hold 1u << sig. */
int vprocHandlerTaskPid(void);
void vprocNoteCaughtSignal(int pid, int sig);
uint32_t vprocCaughtSignals(int pid);
bool vprocTakeCaughtSignal(int pid, int sig);
/* SIGINT stays blocked across the app for what polls it, on every task. A shell that is not at a
 * prompt takes its own at a command boundary with this, on its main thread. */
bool vprocTakePendingSignal(int pid, int sig);
/* What exec does: a caught signal goes back to its default, an ignored one stays. For a command the
 * shell starts. SIGINT and SIGQUIT stay as they are: the shell's commands poll SIGINT. */
void vprocDefaultCaughtActions(int pid);
/* The signal that killed the current task, if its thread is still to unwind, or 0. A command on the
 * shell's own thread is never cancelled, so it has to ask. */
int vprocCurrentDyingSignal(void);
/* A shell's task: a signal that would end it waits for vprocDeliverTaskSignals, since cancelling
 * the thread would leave whatever the shell holds, and what started it waiting. */
void vprocSetUnwindsItself(int pid, bool enabled);
/* halt, poweroff and reboot in the app, which can't stop its device: the caller's session ends. Its
 * leader takes SIGKILL, and vprocTaskHalting tells it that this is an end it was asked for, not a death. */
int vprocHaltSession(void);
bool vprocTaskHalting(int pid);
/* For a long-running program that makes no other shim call: what has come for it acts here. */
void vprocDeliverSignalsHere(void);
/* The VM asks after vprocWaitIfStopped: its task has been killed and its thread has not been cancelled,
 * as for a frontend on the shell's thread. */
bool vprocTaskKilledHere(void);
/* What ended this task, or 0. A stage on a thread that can't be cancelled may not have stopped for it. */
int vprocTaskExitSignal(int pid);
/* Where a program goes that has ended itself with a signal on a thread that has to go on, as with a
 * stage on the shell's thread. setjmp on env, then push: setjmp returns 128 + the signal. */
typedef struct VProcProgramFrame {
    jmp_buf env;
    int pid;
    int handler_task;
    bool pushed;
    void *cleanup_stack;
    bool exec_ended;  /* the program exec'd another, which ended with exec_status */
    int exec_status;
    struct VProcProgramFrame *prev;
} VProcProgramFrame;
void vprocProgramFramePush(VProcProgramFrame *frame);
/* Safe on a frame that was not pushed, if pushed starts false. */
void vprocProgramFramePop(VProcProgramFrame *frame);
/* A shell started on another shell's thread, as a script run inline is: it can't be cancelled either. */
void vprocUnwindAsThreadDoes(int pid);
/* A shell runs under this task, off the prompt's thread. A fatal signal it sends itself waits like
 * the prompt's own: for vprocDeliverTaskSignals, as does one from elsewhere when a thread that can't
 * be cancelled carries it. Otherwise that cancels the thread as usual. Returns what it was. */
bool vprocSetShellTask(int pid, bool enabled);
/* What this task does is the parent's own work, so no SIGCHLD for it. */
void vprocSetSilentToParent(int pid, bool silent);
/* A stage that only does the shell's own work, as `.` and `eval` do: getppid in what it starts gives
 * the shell. The stage is still the parent that waits for them. */
void vprocSetNotAProcess(int pid);
/* As exec does: what the task caught goes back to default and what it ignored stays ignored.
 * SIGINT and SIGQUIT stay: an in-process stage gets Ctrl-C by polling, not by being ended. */
void vprocResetCaughtSignals(int pid);

typedef struct {
    VProc *prev;
    VProc *vp;
    int pid;
} VProcCommandScope;

typedef int (*VProcExecEntryFn)(int argc, char **argv);

/* Utility for iOS-hosted tools (smallclue applets, in-process exec, etc):
 * optionally create and activate a child vproc to represent the invoked command,
 * then tear it down while preserving stop semantics for stop-like statuses. */
bool vprocCommandScopeBegin(VProcCommandScope *scope,
                            const char *label,
                            bool force_new_vproc,
                            bool inherit_parent_pgid);
/* A new scope's vproc takes the descriptors of the one it began under, pipes included, as a forked
 * process has its parent's: a program that is a process of its own, run in a pipeline's stage. */
bool vprocCommandScopeTakeDescriptors(VProcCommandScope *scope);
void vprocCommandScopeEnd(VProcCommandScope *scope, int exit_code);
/* Simulated fork/exec helper for single-process iOS runtime. */
pid_t vprocSimulatedFork(const char *label, bool inherit_parent_pgid);
pid_t vprocSimulatedForkWithEnv(const char *label,
                                bool inherit_parent_pgid,
                                sigjmp_buf *parent_env);
pid_t vprocSimulatedForkParentResume(void);
int vprocSimulatedExec(VProcExecEntryFn entry, char *const argv[]);
/* fork and exec as one call, for a caller with nothing to do between them: a new vproc with
 * the caller's descriptors, environment and directory, running entry on a thread of its own.
 * The pid is the caller's child, for the shims' waitpid and kill. */
pid_t vprocSpawnEntry(const char *label,
                      bool inherit_parent_pgid,
                      VProcExecEntryFn entry,
                      char *const argv[]);

/* Signal API shims: allow vproc_shim.h to virtualize signal dispositions when
 * a vproc is active on the current thread. */
typedef void (*VProcSigHandler)(int);
int vprocSigactionShim(int sig, const struct sigaction *act, struct sigaction *oldact);
int vprocSigprocmaskShim(int how, const sigset_t *set, sigset_t *oldset);
int vprocSigpendingShim(sigset_t *set);
int vprocSigsuspendShim(const sigset_t *mask);
int vprocPthreadSigmaskShim(int how, const sigset_t *set, sigset_t *oldset);
int vprocRaiseShim(int sig);
VProcSigHandler vprocSignalShim(int sig, VProcSigHandler handler);
unsigned int vprocAlarmShim(unsigned int seconds);
useconds_t vprocUalarmShim(useconds_t useconds, useconds_t interval_useconds);
int vprocSetitimerShim(int which, const struct itimerval *new_value, struct itimerval *old_value);
int vprocGetitimerShim(int which, struct itimerval *curr_value);
/* Indicates when it is safe for interposed libc calls to enter vproc shims. */
int vprocInterposeReady(void);
/* True when the thread has been registered as part of a vproc task. */
int vprocThreadIsRegistered(pthread_t tid);
/* Nonblocking variant for interposer gates (returns 0 if registry is busy). */
int vprocThreadIsRegisteredNonblocking(pthread_t tid);
/* True when the calling thread has an active vproc context (TLS). */
int vprocThreadHasActiveVproc(void);
/* True when the thread should bypass interposed libc calls entirely. */
int vprocThreadIsInterposeBypassed(pthread_t tid);
void vprocRegisterInterposeBypassThread(pthread_t tid);
void vprocUnregisterInterposeBypassThread(pthread_t tid);
/* Kernel-managed path truncation (present sandbox as "/"). */
void vprocApplyPathTruncation(const char *prefix);
/* Temporarily disable interposed libc calls while performing host syscalls. */
void vprocInterposeBypassEnter(void);
void vprocInterposeBypassExit(void);
int vprocInterposeBypassActive(void);
/* Temporarily protect kqueue descriptors from accidental close paths. */
void vprocProtectKqueueCloseEnter(void);
void vprocProtectKqueueCloseExit(void);
int vprocProtectKqueueCloseActive(void);

static inline void vprocFormatCpuTimes(int utime_cs, int stime_cs, double *utime_s, double *stime_s) {
    if (utime_s) {
        *utime_s = (double)utime_cs / 100.0;
    }
    if (stime_s) {
        *stime_s = (double)stime_cs / 100.0;
    }
}

#else /* desktop stubs */
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>
typedef struct VProcSessionStdio {
    int dummy;
} VProcSessionStdio;
/* Desktop stubs: map to host syscalls or no-ops so non-iOS builds compile. */
static inline VProcOptions vprocDefaultOptions(void) {
    VProcOptions o = {.stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1, .winsize_cols = 80, .winsize_rows = 24, .pid_hint = -1, .job_id = 0};
    return o;
}
static inline VProc *vprocCreate(const VProcOptions *opts) { (void)opts; return NULL; }
static inline void vprocDestroy(VProc *vp) { (void)vp; }
static inline int vprocPid(VProc *vp) { (void)vp; return -1; }
static inline int vprocReservePid(void) { return (int)getpid(); }
static inline void vprocActivate(VProc *vp) { (void)vp; }
static inline void vprocDeactivate(void) {}
static inline VProc *vprocCurrent(void) { return NULL; }
static inline bool vprocWaitIfStopped(VProc *vp) { (void)vp; return false; }
static inline size_t vprocSnapshot(VProcSnapshot *out, size_t capacity) { (void)out; (void)capacity; return 0; }
static inline int vprocTranslateFd(VProc *vp, int fd) { (void)vp; (void)fd; return -1; }
static inline struct pscal_fd *vprocGetPscalFd(VProc *vp, int fd) { (void)vp; (void)fd; return NULL; }
static inline int vprocDup(VProc *vp, int fd) { (void)vp; (void)fd; return -1; }
static inline void vprocMarkFdInherited(VProc *vp, int fd) { (void)vp; (void)fd; }
static inline void vprocShellFdFollow(int fd) { (void)fd; }
static inline int vprocDup2(VProc *vp, int fd, int target) { (void)vp; (void)fd; (void)target; return -1; }
static inline int vprocReplaceStdio(VProc *vp, int target_fd, int host_src) {
    (void)vp;
    (void)target_fd;
    (void)host_src;
    return 0;
}
static inline int vprocRestoreHostFd(VProc *vp, int target_fd, int host_src) {
    (void)vp;
    (void)target_fd;
    (void)host_src;
    return 0;
}
static inline int vprocClose(VProc *vp, int fd) { (void)vp; (void)fd; return close(fd); }
static inline int vprocPipe(VProc *vp, int pipefd[2]) { (void)vp; return pipe(pipefd); }
static inline int vprocHostDup2(int host_fd, int target_fd) { return dup2(host_fd, target_fd); }
static inline int vprocHostDup(int fd) { return dup(fd); }
static inline int vprocHostOpen(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return open(path, flags, mode);
}
static inline int vprocHostPipe(int pipefd[2]) { return pipe(pipefd); }
static inline int vprocHostPipeUnowned(int pipefd[2]) { return pipe(pipefd); }
static inline off_t vprocHostLseek(int fd, off_t offset, int whence) { return lseek(fd, offset, whence); }
static inline int vprocHostFsync(int fd) { return fsync(fd); }
static inline int vprocHostClose(int fd) { return close(fd); }
static inline int vprocTagHostFd(int fd) { return fd; }
static inline ssize_t vprocHostRead(int fd, void *buf, size_t count) { return read(fd, buf, count); }
static inline ssize_t vprocHostWrite(int fd, const void *buf, size_t count) { return write(fd, buf, count); }
static inline ssize_t vprocHostWriteForTask(int pid, int fd, const void *buf, size_t count) {
    (void)pid;
    return write(fd, buf, count);
}
static inline int vprocHostPoll(struct pollfd *fds, nfds_t nfds, int timeout) { return poll(fds, nfds, timeout); }
static inline int vprocHostSigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    return sigaction(sig, act, oldact);
}
static inline int vprocIoctlShim(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    uintptr_t arg = va_arg(ap, uintptr_t);
    va_end(ap);
    return ioctl(fd, request, arg);
}
static inline int vprocHostPthreadCreate(pthread_t *thread,
                                         const pthread_attr_t *attr,
                                         void *(*start_routine)(void *),
                                         void *arg) {
    return pthread_create(thread, attr, start_routine, arg);
}
static inline int vprocAdoptHostFd(VProc *vp, int host_fd) { (void)vp; return host_fd; }
static inline int vprocPthreadCreateShim(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) { return pthread_create(t, a, fn, arg); }
static inline int vprocIsolationEnter(VProcIsolationDomain domain) { (void)domain; return 0; }
static inline int vprocIsolationTryEnter(VProcIsolationDomain domain) { (void)domain; return 0; }
static inline void vprocIsolationLeave(VProcIsolationDomain domain) { (void)domain; }
static inline int vprocOpenAt(VProc *vp, const char *path, int flags, int mode) { (void)vp; return open(path, flags, mode); }
static inline int vprocSetWinsize(VProc *vp, int cols, int rows) { (void)vp; (void)cols; (void)rows; return 0; }
static inline int vprocGetWinsize(VProc *vp, VProcWinsize *out) { if (out) { out->cols = 80; out->rows = 24; } return 0; }
static inline int vprocRegisterThread(VProc *vp, pthread_t tid) { (void)vp; (void)tid; return 0; }
static inline void vprocEndAbandoned(VProc *keep) { (void)keep; }
static inline VProc *vprocPinCurrent(void) { return NULL; }
static inline void vprocUnpin(VProc *vp) { (void)vp; }
static inline void vprocCloseDescriptors(VProc *vp) { (void)vp; }
static inline void vprocMarkExit(VProc *vp, int status) { (void)vp; (void)status; }
static inline void vprocSetParent(int pid, int parent_pid) { (void)pid; (void)parent_pid; }
static inline void vprocPassChildrenToParent(int pid) { (void)pid; }
static inline int vprocSetPgid(int pid, int pgid) { (void)pid; (void)pgid; return 0; }
static inline int vprocSetSid(int pid, int sid) { (void)pid; (void)sid; return 0; }
static inline int vprocGetPgid(int pid) { (void)pid; return -1; }
static inline int vprocGetSid(int pid) { (void)pid; return -1; }
static inline bool vprocGetShellJobControlState(int *shell_pid_out,
                                                 int *shell_pgid_out,
                                                 int *sid_out,
                                                 int *fg_pgid_out) {
    if (shell_pid_out) *shell_pid_out = -1;
    if (shell_pgid_out) *shell_pgid_out = -1;
    if (sid_out) *sid_out = -1;
    if (fg_pgid_out) *fg_pgid_out = -1;
    return false;
}
static inline int vprocSetForegroundPgid(int sid, int fg) { (void)sid; (void)fg; return 0; }
static inline int vprocGetForegroundPgid(int sid) { (void)sid; return -1; }
static inline void vprocMarkGroupExit(int pid, int status) { (void)pid; (void)status; }
static inline void vprocSetRusage(int pid, int utime, int stime) { (void)pid; (void)utime; (void)stime; }
static inline int vprocBlockSignals(int pid, int mask) { (void)pid; (void)mask; return 0; }
static inline int vprocUnblockSignals(int pid, int mask) { (void)pid; (void)mask; return 0; }
static inline int vprocIgnoreSignal(int pid, int mask) { (void)pid; (void)mask; return 0; }
static inline int vprocDefaultSignal(int pid, int mask) { (void)pid; (void)mask; return 0; }
static inline int vprocSigaction(int pid, int sig, const struct sigaction *act, struct sigaction *old) {
    (void)pid; (void)sig; (void)act; (void)old; return sigaction(sig, act, old);
}
static inline ssize_t vprocReadShim(int fd, void *buf, size_t count) { return read(fd, buf, count); }
static inline ssize_t vprocWriteShim(int fd, const void *buf, size_t count) { return write(fd, buf, count); }
static inline ssize_t vprocWriteShimNoSignal(int fd, const void *buf, size_t count) { return write(fd, buf, count); }
static inline int vprocDupShim(int fd) { return dup(fd); }
static inline int vprocDup2Shim(int fd, int target) { return dup2(fd, target); }
static inline int vprocCloseShim(int fd) { return close(fd); }
static inline int vprocFsyncShim(int fd) { return fsync(fd); }
static inline int vprocPipeShim(int pipefd[2]) { return pipe(pipefd); }
static inline int vprocFstatShim(int fd, struct stat *st) { return fstat(fd, st); }
static inline char *vprocTtynameShim(int fd) { return ttyname(fd); }
static inline int vprocStatShim(const char *path, struct stat *st) { return stat(path, st); }
static inline int vprocLstatShim(const char *path, struct stat *st) { return lstat(path, st); }
static inline int vprocChdirShim(const char *path) { return chdir(path); }
static inline char *vprocGetcwdShim(char *buffer, size_t size) { return getcwd(buffer, size); }
static inline int vprocShellChdirShim(const char *path) { return chdir(path); }
static inline char *vprocShellGetcwdShim(char *buffer, size_t size) { return getcwd(buffer, size); }
static inline mode_t vprocUmaskShim(mode_t mask) { return umask(mask); }
static inline mode_t vprocShellUmaskShim(mode_t mask) { return umask(mask); }
static inline int vprocGetrlimitShim(int resource, struct rlimit *limit) { return getrlimit(resource, limit); }
static inline int vprocShellSetrlimitShim(int resource, const struct rlimit *limit, bool restore) {
    (void)restore;
    return setrlimit(resource, limit);
}
static inline int vprocAccessShim(const char *path, int mode) { return access(path, mode); }
static inline int vprocChmodShim(const char *path, mode_t mode) { return chmod(path, mode); }
static inline int vprocChownShim(const char *path, uid_t uid, gid_t gid) { return chown(path, uid, gid); }
static inline int vprocFchmodShim(int fd, mode_t mode) { return fchmod(fd, mode); }
static inline int vprocFchownShim(int fd, uid_t uid, gid_t gid) { return fchown(fd, uid, gid); }
static inline int vprocUtimesShim(const char *path, const struct timeval times[2]) { return utimes(path, times); }
static inline int vprocFutimesShim(int fd, const struct timeval times[2]) { return futimes(fd, times); }
static inline int vprocMkdirShim(const char *path, mode_t mode) { return mkdir(path, mode); }
static inline int vprocRmdirShim(const char *path) { return rmdir(path); }
static inline int vprocUnlinkShim(const char *path) { return unlink(path); }
static inline int vprocRemoveShim(const char *path) { return remove(path); }
static inline int vprocRenameShim(const char *oldpath, const char *newpath) { return rename(oldpath, newpath); }
static inline DIR *vprocOpendirShim(const char *name) { return opendir(name); }
static inline int vprocSymlinkShim(const char *target, const char *linkpath) { return symlink(target, linkpath); }
static inline ssize_t vprocReadlinkShim(const char *path, char *buf, size_t size) { return readlink(path, buf, size); }
static inline char *vprocRealpathShim(const char *path, char *resolved_path) { return realpath(path, resolved_path); }
static inline off_t vprocLseekShim(int fd, off_t offset, int whence) { return lseek(fd, offset, whence); }
static inline int vprocIsattyShim(int fd) { return isatty(fd); }
static inline void vprocLocationDeviceSetEnabled(bool enabled) { (void)enabled; }
static inline ssize_t vprocLocationDeviceWrite(const void *data, size_t len) {
    (void)data;
    (void)len;
    errno = ENODEV;
    return -1;
}
typedef void (*VprocLocationReadersChangedFn)(int readers, void *context);
static inline void vprocLocationDeviceRegisterReaderObserver(VprocLocationReadersChangedFn cb, void *context) {
    (void)cb;
    (void)context;
}
static inline int vprocOpenShim(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
        return open(path, flags, mode);
    }
    return open(path, flags);
}
static inline pid_t vprocWaitPidShim(pid_t pid, int *status_out, int options) { return waitpid(pid, status_out, options); }
static inline int vprocKillShim(pid_t pid, int sig) { return kill(pid, sig); }
static inline int vprocKillLive(pid_t pid, int sig) { return kill(pid, sig); }
static inline int vprocKillTree(pid_t pid, int sig) { return kill(pid, sig); }
static inline pid_t vprocGetPidShim(void) { return getpid(); }
static inline pid_t vprocGetPpidShim(void) { return getppid(); }
static inline pid_t vprocGetpgrpShim(void) { return getpgrp(); }
static inline pid_t vprocGetpgidShim(pid_t pid) { return getpgid(pid); }
static inline int vprocSetpgidShim(pid_t pid, pid_t pgid) { return setpgid(pid, pgid); }
static inline pid_t vprocSetsidShim(void) { return setsid(); }
static inline pid_t vprocGetsidShim(pid_t pid) { return getsid(pid); }
static inline pid_t vprocTcgetpgrpShim(int fd) { return tcgetpgrp(fd); }
static inline int vprocTcsetpgrpShim(int fd, pid_t pgid) { return tcsetpgrp(fd, pgid); }
static inline int vprocPollShim(struct pollfd *fds, nfds_t nfds, int timeout) { return poll(fds, nfds, timeout); }
static inline int vprocSelectShim(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    return select(nfds, readfds, writefds, exceptfds, timeout);
}
static inline void vprocSetShellSelfPid(int pid) { (void)pid; }
static inline int vprocGetShellSelfPid(void) { return (int)getpid(); }
static inline void vprocSetShellSelfTid(pthread_t tid) { (void)tid; }
static inline bool vprocIsShellSelfThread(void) { return false; }
static inline int vprocInterposeReady(void) { return 0; }
static inline int vprocThreadIsRegistered(pthread_t tid) { (void)tid; return 0; }
static inline int vprocThreadIsRegisteredNonblocking(pthread_t tid) { (void)tid; return 0; }
static inline int vprocThreadHasActiveVproc(void) { return 0; }
static inline int vprocThreadIsInterposeBypassed(pthread_t tid) { (void)tid; return 0; }
static inline void vprocRegisterInterposeBypassThread(pthread_t tid) { (void)tid; }
static inline void vprocUnregisterInterposeBypassThread(pthread_t tid) { (void)tid; }
static inline void vprocApplyPathTruncation(const char *prefix) { (void)prefix; }
static inline void vprocInterposeBypassEnter(void) {}
static inline void vprocInterposeBypassExit(void) {}
static inline int vprocInterposeBypassActive(void) { return 0; }
static inline void vprocProtectKqueueCloseEnter(void) {}
static inline void vprocProtectKqueueCloseExit(void) {}
static inline int vprocProtectKqueueCloseActive(void) { return 0; }
static inline void vprocSetKernelPid(int pid) { (void)pid; }
static inline int vprocGetKernelPid(void) { return 0; }
static inline void vprocClearKernelPidGlobal(void) {}
static inline bool vprocSessionStdioIsDefault(VProcSessionStdio *stdio_ctx) { (void)stdio_ctx; return true; }
static inline bool vprocSessionStdioFetchTermios(int fd, struct termios *termios) {
    (void)fd;
    (void)termios;
    return false;
}
static inline bool vprocSessionStdioApplyTermios(int fd, int action, const struct termios *termios) {
    (void)fd;
    (void)action;
    (void)termios;
    return false;
}
typedef struct VProcTerminalHold VProcTerminalHold;
static inline VProcTerminalHold *vprocTerminalHold(int fd) {
    (void)fd;
    return NULL;
}
static inline bool vprocTerminalHoldFetchTermios(VProcTerminalHold *hold, struct termios *termios) {
    (void)hold;
    (void)termios;
    return false;
}
static inline bool vprocTerminalHoldApplyTermios(VProcTerminalHold *hold, const struct termios *termios) {
    (void)hold;
    (void)termios;
    return false;
}
static inline void vprocTerminalHoldRelease(VProcTerminalHold *hold) { (void)hold; }
static inline int vprocSessionStdioInitWithPty(VProcSessionStdio *stdio_ctx,
                                               struct pscal_fd *pty_slave,
                                               struct pscal_fd *pty_master,
                                               uint64_t session_id,
                                               int kernel_pid) {
    (void)stdio_ctx;
    (void)pty_slave;
    (void)pty_master;
    (void)session_id;
    (void)kernel_pid;
    errno = ENOTSUP;
    return -1;
}
static inline int vprocAdoptPscalStdio(VProc *vp,
                                       struct pscal_fd *stdin_fd,
                                       struct pscal_fd *stdout_fd,
                                       struct pscal_fd *stderr_fd) {
    (void)vp;
    (void)stdin_fd;
    (void)stdout_fd;
    (void)stderr_fd;
    return 0;
}
static inline int vprocAdoptPscalFd(VProc *vp, int target_fd, struct pscal_fd *pscal_fd) {
    (void)vp;
    (void)target_fd;
    (void)pscal_fd;
    return 0;
}
static inline int vprocSetSessionWinsize(uint64_t session_id, int cols, int rows) {
    (void)session_id;
    (void)cols;
    (void)rows;
    errno = ENOTSUP;
    return -1;
}
static inline int vprocGetSessionWinsize(uint64_t session_id, int *cols_out, int *rows_out) {
    (void)session_id;
    if (cols_out) {
        *cols_out = 0;
    }
    if (rows_out) {
        *rows_out = 0;
    }
    errno = ENOTSUP;
    return -1;
}
static inline int vprocSigpending(int pid, sigset_t *set) { (void)pid; return sigpending(set); }
static inline int vprocSigsuspend(int pid, const sigset_t *mask) { (void)pid; return sigsuspend(mask); }
static inline int vprocSigprocmask(int pid, int how, const sigset_t *set, sigset_t *oldset) {
    (void)pid;
    return sigprocmask(how, set, oldset);
}
static inline int vprocSigwait(int pid, const sigset_t *set, int *sig) { (void)pid; return sigwait(set, sig); }
static inline int vprocSigtimedwait(int pid, const sigset_t *set, const struct timespec *timeout, int *sig) {
    (void)pid;
#if defined(__APPLE__)
    (void)set;
    (void)timeout;
    (void)sig;
    errno = ENOSYS;
    return -1;
#else
    siginfo_t info;
    int rc = sigtimedwait(set, &info, timeout);
    if (rc > 0) {
        if (sig) *sig = rc;
        return 0;
    }
    return (rc == -1) ? errno : rc;
#endif
}
static inline unsigned int vprocAlarmShim(unsigned int seconds) {
    return alarm(seconds);
}
static inline useconds_t vprocUalarmShim(useconds_t useconds, useconds_t interval_useconds) {
    return ualarm(useconds, interval_useconds);
}
static inline int vprocSetitimerShim(int which, const struct itimerval *new_value, struct itimerval *old_value) {
    return setitimer(which, new_value, old_value);
}
static inline int vprocGetitimerShim(int which, struct itimerval *curr_value) {
    return getitimer(which, curr_value);
}
static inline void vprocSetJobId(int pid, int job_id) { (void)pid; (void)job_id; }
static inline int vprocGetJobId(int pid) { (void)pid; return 0; }
static inline void vprocSetCommandLabel(int pid, const char *label) { (void)pid; (void)label; }
static inline bool vprocGetCommandLabel(int pid, char *buf, size_t buf_len) { (void)pid; (void)buf; (void)buf_len; return false; }
static inline void vprocDiscard(int pid) { (void)pid; }
static inline bool vprocTerminateSessionById(uint64_t session_id) {
    (void)session_id;
    return false;
}
static inline bool vprocSigchldPending(int pid) { (void)pid; return false; }
static inline void vprocSetShellPromptReadActive(int pid, bool active) { (void)pid; (void)active; }
static inline bool vprocShellPromptReadActive(int pid) { (void)pid; return false; }
static inline void vprocSetCooperativeStopWait(int pid, bool enabled) { (void)pid; (void)enabled; }
static inline bool vprocRequestControlSignal(int sig) { (void)sig; return false; }
static inline bool vprocRequestControlSignalForShell(int shell_pid, int sig) {
    (void)shell_pid;
    (void)sig;
    return false;
}
static inline bool vprocRequestControlSignalForSession(uint64_t session_id, int sig) {
    (void)session_id;
    (void)sig;
    return false;
}
static inline void vprocSessionSetControlBytePassthrough(uint64_t session_id, bool enabled) {
    (void)session_id;
    (void)enabled;
}
static inline bool vprocSessionGetControlBytePassthrough(uint64_t session_id) {
    (void)session_id;
    return false;
}
typedef struct {
    VProc *prev;
    VProc *vp;
    int pid;
} VProcCommandScope;
typedef int (*VProcExecEntryFn)(int argc, char **argv);
static inline bool vprocCommandScopeBegin(VProcCommandScope *scope,
                                         const char *label,
                                         bool force_new_vproc,
                                         bool inherit_parent_pgid) {
    (void)scope;
    (void)label;
    (void)force_new_vproc;
    (void)inherit_parent_pgid;
    return false;
}
static inline void vprocCommandScopeEnd(VProcCommandScope *scope, int exit_code) {
    (void)scope;
    (void)exit_code;
}
static inline pid_t vprocSimulatedFork(const char *label, bool inherit_parent_pgid) {
    (void)label;
    (void)inherit_parent_pgid;
    errno = ENOSYS;
    return (pid_t)-1;
}
static inline pid_t vprocSimulatedForkWithEnv(const char *label,
                                              bool inherit_parent_pgid,
                                              sigjmp_buf *parent_env) {
    (void)label;
    (void)inherit_parent_pgid;
    (void)parent_env;
    errno = ENOSYS;
    return (pid_t)-1;
}
static inline pid_t vprocSimulatedForkParentResume(void) {
    errno = ENOSYS;
    return (pid_t)-1;
}
static inline int vprocSimulatedExec(VProcExecEntryFn entry, char *const argv[]) {
    (void)entry;
    (void)argv;
    errno = ENOSYS;
    return -1;
}
static inline pid_t vprocSpawnEntry(const char *label,
                                    bool inherit_parent_pgid,
                                    VProcExecEntryFn entry,
                                    char *const argv[]) {
    (void)label;
    (void)inherit_parent_pgid;
    (void)entry;
    (void)argv;
    errno = ENOSYS;
    return (pid_t)-1;
}
static inline int vprocSetSigchldBlocked(int pid, bool block) { (void)pid; (void)block; return 0; }
static inline void vprocClearSigchldPending(int pid) { (void)pid; }
static inline int vprocDeliverTaskSignals(int pid) { (void)pid; return 0; }
static inline int vprocHandlerTaskPid(void) { return 0; }
static inline void vprocNoteCaughtSignal(int pid, int sig) { (void)pid; (void)sig; }
static inline uint32_t vprocCaughtSignals(int pid) { (void)pid; return 0; }
static inline bool vprocTakeCaughtSignal(int pid, int sig) { (void)pid; (void)sig; return false; }
static inline bool vprocTakePendingSignal(int pid, int sig) { (void)pid; (void)sig; return false; }
static inline void vprocDefaultCaughtActions(int pid) { (void)pid; }
static inline int vprocCurrentDyingSignal(void) { return 0; }
static inline void vprocSetUnwindsItself(int pid, bool enabled) { (void)pid; (void)enabled; }
static inline int vprocHaltSession(void) { return -1; }
static inline bool vprocTaskHalting(int pid) { (void)pid; return false; }
static inline void vprocDeliverSignalsHere(void) {}
static inline bool vprocTaskKilledHere(void) { return false; }
static inline int vprocTaskExitSignal(int pid) { (void)pid; return 0; }
static inline void vprocUnwindAsThreadDoes(int pid) { (void)pid; }
static inline bool vprocSetShellTask(int pid, bool enabled) { (void)pid; (void)enabled; return false; }
static inline void vprocSetSilentToParent(int pid, bool silent) { (void)pid; (void)silent; }
static inline void vprocSetNotAProcess(int pid) { (void)pid; }
static inline void vprocResetCaughtSignals(int pid) { (void)pid; }
static inline void vprocFormatCpuTimes(int utime_cs, int stime_cs, double *utime_s, double *stime_s) {
    if (utime_s) {
        *utime_s = (double)utime_cs / 100.0;
    }
    if (stime_s) {
        *stime_s = (double)stime_cs / 100.0;
    }
}
#endif /* PSCAL_TARGET_IOS || VPROC_ENABLE_STUBS_FOR_TESTS */

#ifdef __cplusplus
}
#endif
