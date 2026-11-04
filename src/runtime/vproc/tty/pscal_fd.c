// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0

#include "runtime/vproc/tty/pscal_fd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

#include "runtime/vproc/vproc.h"

struct pscal_fd *pscal_fd_create(const struct pscal_fd_ops *ops) {
    struct pscal_fd *fd = (struct pscal_fd *)calloc(1, sizeof(*fd));
    if (!fd) {
        return NULL;
    }
    if (pthread_mutex_init(&fd->lock, NULL) != 0) {
        free(fd);
        return NULL;
    }
    fd->ops = ops;
    fd->refs = 1;
    return fd;
}

struct pscal_fd *pscal_fd_retain(struct pscal_fd *fd) {
    if (fd) {
        __atomic_add_fetch(&fd->refs, 1, __ATOMIC_RELAXED);
    }
    return fd;
}

int pscal_fd_close(struct pscal_fd *fd) {
    if (!fd) {
        return _EBADF;
    }
    if (__atomic_sub_fetch(&fd->refs, 1, __ATOMIC_ACQ_REL) != 0) {
        return 0;
    }
    /* A kill can be pending on whichever thread happens to let go last, and nothing
     * would finish this file if that thread ended partway through. */
    int cancel_state = 0;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    int result = 0;
    if (fd->ops && fd->ops->close) {
        result = fd->ops->close(fd);
    }
    pthread_mutex_destroy(&fd->lock);
    free(fd);
    pthread_setcancelstate(cancel_state, NULL);
    return result;
}

/* The ends are written once, before gWakeReady goes true, and never change. */
static int gWakeEnds[2] = {-1, -1};
static bool gWakeReady = false;
static pthread_mutex_t gWakeLock = PTHREAD_MUTEX_INITIALIZER;

static bool wakePipeConfigure(int ends[2]) {
    for (int i = 0; i < 2; i++) {
        int status = fcntl(ends[i], F_GETFL);
        if (status < 0 || fcntl(ends[i], F_SETFL, status | O_NONBLOCK) != 0) {
            return false;
        }
        if (fcntl(ends[i], F_SETFD, FD_CLOEXEC) != 0) {
            return false;
        }
    }
#if defined(F_SETNOSIGPIPE)
    if (fcntl(ends[1], F_SETNOSIGPIPE, 1) != 0) {
        return false;
    }
#endif
    return true;
}

/* Whichever thread needs the pipe first makes it, poller or waker: a waker that
 * skipped an absent pipe could lose its event to a poller that is about to make it.
 * A failure is not final, as running out of descriptors can pass. */
static bool wakePipeEnsure(void) {
    if (__atomic_load_n(&gWakeReady, __ATOMIC_ACQUIRE)) {
        return true;
    }
    /* fcntl is a cancellation point on Darwin, and this lock is everybody's. */
    int cancel_state = 0;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    pthread_mutex_lock(&gWakeLock);
    if (!gWakeReady) {
        /* Unowned: a vproc that owned these would close them as it ended, and the
         * numbers kept here would then name somebody else's file. */
        int ends[2] = {-1, -1};
        if (vprocHostPipeUnowned(ends) == 0) {
            if (wakePipeConfigure(ends)) {
                gWakeEnds[0] = ends[0];
                gWakeEnds[1] = ends[1];
                __atomic_store_n(&gWakeReady, true, __ATOMIC_RELEASE);
            } else {
                vprocHostClose(ends[0]);
                vprocHostClose(ends[1]);
            }
        }
    }
    bool ready = gWakeReady;
    pthread_mutex_unlock(&gWakeLock);
    pthread_setcancelstate(cancel_state, NULL);
    return ready;
}

#if defined(F_SETNOSIGPIPE)

static void wakePipeWrite(void) {
    const unsigned char byte = 0;
    (void)vprocHostWrite(gWakeEnds[1], &byte, 1);
}

#else

/* A signal handler belongs to the whole app, so a stray SIGPIPE must not reach one.
 * With no per-descriptor setting, keep it pending on this thread and take it back. */
static void wakePipeWrite(void) {
    const unsigned char byte = 0;
    int cancel_state = 0;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    sigset_t pipe_only;
    sigset_t previous;
    sigset_t pending;
    sigemptyset(&pipe_only);
    sigaddset(&pipe_only, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_only, &previous);
    sigpending(&pending);
    const bool was_pending = sigismember(&pending, SIGPIPE) == 1;
    (void)vprocHostWrite(gWakeEnds[1], &byte, 1);
    if (!was_pending) {
        sigpending(&pending);
        if (sigismember(&pending, SIGPIPE) == 1) {
            int taken = 0;
            sigwait(&pipe_only, &taken);
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    pthread_setcancelstate(cancel_state, NULL);
}

#endif

int pscalPollWakeFd(void) {
    int saved_errno = errno;
    int fd = wakePipeEnsure() ? gWakeEnds[0] : -1;
    errno = saved_errno;
    return fd;
}

void pscalPollDrain(void) {
    int saved_errno = errno;
    if (wakePipeEnsure()) {
        unsigned char sink[64];
        while (vprocHostRead(gWakeEnds[0], sink, sizeof(sink)) > 0) {
        }
    }
    errno = saved_errno;
}

void pscal_fd_poll_wakeup(struct pscal_fd *fd, int events) {
    (void)fd;
    (void)events;
    /* Wakes happen in the middle of other work, whose errno is not ours to change.
     * A full pipe drops the byte, and that is fine: pollers ask every file anyway. */
    int saved_errno = errno;
    if (wakePipeEnsure()) {
        wakePipeWrite();
    }
    errno = saved_errno;
}
