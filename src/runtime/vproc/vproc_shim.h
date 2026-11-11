#pragma once

#if defined(VPROC_SHIM_DISABLED) && !defined(VPROC_SHIM_HARD_DISABLED)
#undef VPROC_SHIM_DISABLED
#endif

#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
#include <libgen.h>
#include <locale.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <grp.h>
#include <unistd.h>
#define VPROC_SHIM_PRESENT 1
#include "runtime/vproc/vproc.h"

/* Some third-party components (e.g., OpenSSH) provide their own iOS shims.
 * If their shim header is present, do not redefine the syscall macros here. */
#if !defined(PSCAL_IOS_SHIM_H)

#undef read
#undef write
#undef dup
#undef dup2
#undef close
#undef fsync
#undef pipe
#undef socket
#undef accept
#undef socketpair
#undef fstat
#undef stat
#undef lstat
#undef umask
#undef mkfifo
#undef mkdir
#undef creat
#undef fopen
#undef mknod
#undef chmod
#undef chown
#undef fchmod
#undef fchown
#undef utimes
#undef futimes
#undef ioctl
#undef lseek
#undef isatty
#undef poll
#undef select
#undef waitpid
#undef kill
#undef getpid
#undef getppid
#undef getpgrp
#undef getpgid
#undef setpgid
#undef getsid
#undef setsid
#undef tcgetpgrp
#undef tcsetpgrp
#undef pthread_create
#undef sigaction
#undef sigprocmask
#undef sigpending
#undef sigsuspend
#undef signal
#undef raise
#undef pthread_sigmask
#undef getenv
#undef setenv
#undef unsetenv
#undef environ
#undef basename
#undef dirname
#undef setlocale

#define getenv vprocGetenvShim
#define setenv vprocSetenvShim
#define unsetenv vprocUnsetenvShim
/* `extern char **environ;` after this declares vprocEnvironShim again, compatibly. */
#define environ (*vprocEnvironShim())
/* libc answers these in one buffer for the whole process. */
#define basename(path) vprocBasenameShim((path))
#define dirname(path) vprocDirnameShim((path))
/* libc's is the whole app's, and it crashes if two threads load a locale at once. */
#define setlocale(category, name) vprocSetlocaleShim((category), (name))

#define read  vprocReadShim
#define write vprocWriteShim
#define dup   vprocDupShim
#define dup2  vprocDup2Shim
#define close vprocCloseShim
#define fsync vprocFsyncShim
#define pipe  vprocPipeShim
#define socket vprocSocketShim
#define accept vprocAcceptShim
#define socketpair vprocSocketpairShim
#define fstat vprocFstatShim
#define stat(path, buf) vprocStatShim((path), (buf))
#define lstat(path, buf) vprocLstatShim((path), (buf))
#define umask(mask) vprocUmaskShim((mask))
#define mkfifo(path, mode) vprocMkfifoShim((path), (mode))
/* What makes a file obeys the task's mask, never the host's, which is the session shell's. These
 * keep libc's names: a path is the host's, as it was. */
#define mkdir(path, mode) vprocHostMkdirMasked((path), (mode))
#define creat(path, mode) vprocOpenShim((path), O_CREAT | O_WRONLY | O_TRUNC, (mode))
#define fopen(path, mode) vprocHostFopenMasked((path), (mode))
#define mknod(path, mode, dev) vprocMknodShim((path), (mode), (dev))
#define chmod(path, mode) vprocChmodShim((path), (mode))
#define chown(path, uid, gid) vprocChownShim((path), (uid), (gid))
#define fchmod(fd, mode) vprocFchmodShim((fd), (mode))
#define fchown(fd, uid, gid) vprocFchownShim((fd), (uid), (gid))
#define utimes(path, times) vprocUtimesShim((path), (times))
#define lchown(path, uid, gid) vprocLchownShim((path), (uid), (gid))
#define utimensat(dirfd, path, times, flags) vprocUtimensatShim((dirfd), (path), (times), (flags))
#define linkat(olddirfd, oldpath, newdirfd, newpath, flags) vprocLinkatShim((olddirfd), (oldpath), (newdirfd), (newpath), (flags))
#define mkstemp(templ) vprocMkstempShim((templ))
#define mkdtemp(templ) vprocMkdtempShim((templ))
#define futimes(fd, times) vprocFutimesShim((fd), (times))
#define ioctl vprocIoctlShim
#define lseek vprocLseekShim
#define isatty vprocIsattyShim
#define poll vprocPollShim
#define select vprocSelectShim
#ifndef open
#define open  vprocOpenShim
#endif
#define waitpid vprocWaitPidShim
#define kill  vprocKillShim
#define getpid vprocGetPidShim
#define getppid vprocGetPpidShim
#define getpgrp vprocGetpgrpShim
#define getpgid vprocGetpgidShim
#define setpgid vprocSetpgidShim
#define getsid vprocGetsidShim
#define setsid vprocSetsidShim
#define tcgetpgrp vprocTcgetpgrpShim
#define tcsetpgrp vprocTcsetpgrpShim
#define pthread_create vprocPthreadCreateShim
#define sigaction(sig, act, oldact) vprocSigactionShim((sig), (act), (oldact))
#define sigprocmask(how, set, oldset) vprocSigprocmaskShim((how), (set), (oldset))
#define sigpending(set) vprocSigpendingShim((set))
#define sigsuspend(mask) vprocSigsuspendShim((mask))
#define signal(sig, handler) vprocSignalShim((sig), (handler))
#define raise(sig) vprocRaiseShim((sig))
#define pthread_sigmask(how, set, oldset) vprocPthreadSigmaskShim((how), (set), (oldset))
/* Who a task is, and exec, are the kernel's: see vproc.h. */
#undef getuid
#undef geteuid
#undef getgid
#undef getegid
#undef getgroups
#undef setuid
#undef seteuid
#undef setreuid
#undef setgid
#undef setegid
#undef setregid
#undef setgroups
#undef initgroups
#undef execve
#undef execv
#undef execvp
#undef execl
#undef execlp
#undef gethostname
#undef sethostname
#undef uname
#define getuid() vprocGetuidShim()
#define geteuid() vprocGeteuidShim()
#define getgid() vprocGetgidShim()
#define getegid() vprocGetegidShim()
#define getgroups(...) vprocGetgroupsShim(__VA_ARGS__)
#define setuid(...) vprocSetuidShim(__VA_ARGS__)
#define seteuid(...) vprocSeteuidShim(__VA_ARGS__)
#define setreuid(...) vprocSetreuidShim(__VA_ARGS__)
#define setgid(...) vprocSetgidShim(__VA_ARGS__)
#define setegid(...) vprocSetegidShim(__VA_ARGS__)
#define setregid(...) vprocSetregidShim(__VA_ARGS__)
#define setgroups(...) vprocSetgroupsShim(__VA_ARGS__)
#define initgroups(...) vprocInitgroupsShim(__VA_ARGS__)
#define execve(...) vprocExecveShim(__VA_ARGS__)
#define execv(...) vprocExecvShim(__VA_ARGS__)
#define execvp(...) vprocExecvpShim(__VA_ARGS__)
#define execl(...) vprocExeclShim(__VA_ARGS__)
#define execlp(...) vprocExeclpShim(__VA_ARGS__)
#define gethostname(...) vprocGethostnameShim(__VA_ARGS__)
#define sethostname(...) vprocSethostnameShim(__VA_ARGS__)
#define uname(...) vprocUnameShim(__VA_ARGS__)
#define alarm(seconds) vprocAlarmShim((seconds))
#define ualarm(useconds, interval_useconds) vprocUalarmShim((useconds), (interval_useconds))
#define setitimer(which, new_value, old_value) vprocSetitimerShim((which), (new_value), (old_value))
#define getitimer(which, curr_value) vprocGetitimerShim((which), (curr_value))

#endif /* !PSCAL_IOS_SHIM_H */

#include "runtime/vproc/vproc_stdio_shim.h"

#endif /* PSCAL_TARGET_IOS */
