// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// The vocabulary shared by the terminal layer and its callers: fixed-width names,
// error codes in Linux's numbers, poll bits and a mutex. Linux's numbers are an ABI
// here, since applets issue terminal requests and read terminal structs in them.
#pragma once

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t byte_t;
typedef uint16_t word_t;
typedef uint32_t dword_t;
typedef int32_t sdword_t;
typedef uint64_t qword_t;
typedef int64_t sqword_t;

typedef int32_t pid_t_;
typedef uint32_t uid_t_;
typedef uint32_t gid_t_;
typedef uint16_t mode_t_;
typedef int64_t off_t_;
typedef uint64_t sigset_t_;

typedef pthread_mutex_t lock_t;

static inline void lock(lock_t *l) {
    pthread_mutex_lock(l);
}

static inline void unlock(lock_t *l) {
    pthread_mutex_unlock(l);
}

/* Results are a non-negative value or one of these; errno is never touched below the
 * shims. The leading underscore keeps them apart from the host's positive numbers. */
#define _EPERM (-1)
#define _ENOENT (-2)
#define _ESRCH (-3)
#define _EINTR (-4)
#define _EIO (-5)
#define _ENXIO (-6)
#define _EBADF (-9)
#define _EAGAIN (-11)
#define _ENOMEM (-12)
#define _EACCES (-13)
#define _EFAULT (-14)
#define _EBUSY (-16)
#define _EEXIST (-17)
#define _ENODEV (-19)
#define _ENOTDIR (-20)
#define _EISDIR (-21)
#define _EINVAL (-22)
#define _EMFILE (-24)
#define _ENOTTY (-25)
#define _ENOSPC (-28)
#define _EPIPE (-32)
#define _ENOTEMPTY (-39)
#define _ENOTSUP (-95)
#define _ETIMEDOUT (-110)

/* What ops->poll returns. Darwin's <signal.h> uses some of these names for siginfo
 * codes, so it is included above to spend its guard before they are replaced. */
#undef POLL_READ
#undef POLL_PRI
#undef POLL_WRITE
#undef POLL_ERR
#undef POLL_HUP
#undef POLL_NVAL
#define POLL_READ 1
#define POLL_PRI 2
#define POLL_WRITE 4
#define POLL_ERR 8
#define POLL_HUP 16
#define POLL_NVAL 32

#define SIGINT_ SIGINT
#define SIGQUIT_ SIGQUIT
#define SIGTSTP_ SIGTSTP
#define SIGTTIN_ SIGTTIN
#define SIGTTOU_ SIGTTOU
#define SIGWINCH_ SIGWINCH
#define SIGHUP_ SIGHUP
#define SIGCONT_ SIGCONT
#define NUM_SIGS 64

/* The host's errno for a negative result, and 0 for a result that is not an error. A
 * program has to see the error that happened; EIO is only for a code nobody listed. */
int pscalCompatErrno(int err);

#ifdef __cplusplus
}
#endif
