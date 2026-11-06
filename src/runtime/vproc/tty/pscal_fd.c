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

/* Each thread that polls has a wake pipe of its own. With one shared pipe, whichever
 * thread polled next emptied it, and the poller the byte was meant for slept on. */
struct wakeChannel {
    int ends[2];
    bool pending; /* a byte is in the pipe or about to be, so wakers pass it by */
    struct wakeChannel *prev;
    struct wakeChannel *next;
};

/* Wakers write under the lock, so a channel's pipe is closed only once it is off the
 * list. Wakers arrive holding other locks, so this one takes nothing else. */
static struct wakeChannel *gWakeChannels = NULL;
static pthread_mutex_t gWakeLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t gWakeKey;
static bool gWakeKeyReady = false;
static pthread_once_t gWakeKeyOnce = PTHREAD_ONCE_INIT;

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

/* A byte lost to a signal would leave the flag set on an empty pipe, so EINTR tries
 * again. A full pipe is not a failure that matters: its reader is due to wake. */

#if defined(F_SETNOSIGPIPE)

static bool wakePipeWrite(int fd) {
    const unsigned char byte = 0;
    ssize_t written = 0;
    do {
        written = vprocHostWrite(fd, &byte, 1);
    } while (written < 0 && errno == EINTR);
    return written == 1;
}

#else

/* A signal handler belongs to the whole app, so a stray SIGPIPE must not reach one.
 * With no per-descriptor setting, keep it pending on this thread and take it back. */
static bool wakePipeWrite(int fd) {
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
    ssize_t written = 0;
    do {
        written = vprocHostWrite(fd, &byte, 1);
    } while (written < 0 && errno == EINTR);
    if (!was_pending) {
        sigpending(&pending);
        if (sigismember(&pending, SIGPIPE) == 1) {
            int taken = 0;
            sigwait(&pipe_only, &taken);
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    pthread_setcancelstate(cancel_state, NULL);
    return written == 1;
}

#endif

/* Runs as the thread ends, cancelled or not. */
static void wakeChannelEnd(void *value) {
    struct wakeChannel *ch = (struct wakeChannel *)value;
    int cancel_state = 0;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    pthread_mutex_lock(&gWakeLock);
    if (ch->prev) {
        ch->prev->next = ch->next;
    } else {
        gWakeChannels = ch->next;
    }
    if (ch->next) {
        ch->next->prev = ch->prev;
    }
    pthread_mutex_unlock(&gWakeLock);
    vprocHostClose(ch->ends[0]);
    vprocHostClose(ch->ends[1]);
    free(ch);
    pthread_setcancelstate(cancel_state, NULL);
}

static void wakeKeyCreate(void) {
    gWakeKeyReady = pthread_key_create(&gWakeKey, wakeChannelEnd) == 0;
}

static struct wakeChannel *wakeChannelCurrent(void) {
    pthread_once(&gWakeKeyOnce, wakeKeyCreate);
    return gWakeKeyReady ? (struct wakeChannel *)pthread_getspecific(gWakeKey) : NULL;
}

/* The poll loop asks its files before it asks for this pipe, so a new pipe starts with
 * a byte in it: the first poll wakes at once and asks them again, which covers any wake
 * sent before the channel was on the list. That first write is also made off the lock,
 * as the host write sets itself up on its first use. */
static struct wakeChannel *wakeChannelCreate(void) {
    struct wakeChannel *ch = (struct wakeChannel *)calloc(1, sizeof(*ch));
    if (!ch) {
        return NULL;
    }
    /* Unowned: a vproc that owned these would close them as it ended, and the numbers
     * kept here would then name somebody else's file. */
    if (vprocHostPipeUnowned(ch->ends) != 0) {
        free(ch);
        return NULL;
    }
    ch->pending = true;
    if (!wakePipeConfigure(ch->ends) || !wakePipeWrite(ch->ends[1]) ||
        pthread_setspecific(gWakeKey, ch) != 0) {
        vprocHostClose(ch->ends[0]);
        vprocHostClose(ch->ends[1]);
        free(ch);
        return NULL;
    }
    pthread_mutex_lock(&gWakeLock);
    ch->next = gWakeChannels;
    if (gWakeChannels) {
        gWakeChannels->prev = ch;
    }
    gWakeChannels = ch;
    pthread_mutex_unlock(&gWakeLock);
    return ch;
}

int pscalPollWakeFd(void) {
    int saved_errno = errno;
    int fd = -1;
    struct wakeChannel *ch = wakeChannelCurrent();
    if (!ch && gWakeKeyReady) {
        /* fcntl is a cancellation point on Darwin, and a pipe made but not yet kept
         * would leak. A failure is not final, as running out of descriptors can pass. */
        int cancel_state = 0;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
        ch = wakeChannelCreate();
        pthread_setcancelstate(cancel_state, NULL);
    }
    if (ch) {
        fd = ch->ends[0];
    }
    errno = saved_errno;
    return fd;
}

void pscalPollDrain(void) {
    int saved_errno = errno;
    struct wakeChannel *ch = wakeChannelCurrent();
    if (ch) {
        /* A short read means the pipe was empty, and the flag keeps it to a byte or two. */
        unsigned char sink[64];
        while (vprocHostRead(ch->ends[0], sink, sizeof(sink)) == (ssize_t)sizeof(sink)) {
        }
        /* The flag is cleared after the read, never before. Cleared first, a waker could
         * set it and write a byte this read then took, and the flag would stay set on an
         * empty pipe with every later waker passing it by. A waker that finds it still
         * set here is not lost either: the caller asks its files again after this, and
         * the fence pairs with the one in pscal_fd_poll_wakeup, so either that waker
         * sees the flag clear and writes, or the files show what it changed. */
        __atomic_store_n(&ch->pending, false, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    errno = saved_errno;
}

void pscal_fd_poll_wakeup(struct pscal_fd *fd, int events) {
    (void)fd;
    (void)events;
    /* Wakes happen in the middle of other work, whose errno is not ours to change. */
    int saved_errno = errno;
    /* Orders the caller's change to a file before the flags. Pairs with pscalPollDrain. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int cancel_state = 0;
    bool cancel_off = false;
    pthread_mutex_lock(&gWakeLock);
    for (struct wakeChannel *ch = gWakeChannels; ch; ch = ch->next) {
        if (__atomic_load_n(&ch->pending, __ATOMIC_RELAXED) ||
            __atomic_exchange_n(&ch->pending, true, __ATOMIC_RELAXED)) {
            continue;
        }
        /* A write can be a cancellation point, and a waker ended between setting the
         * flag and writing would leave that poller asleep for good. */
        if (!cancel_off) {
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
            cancel_off = true;
        }
        (void)wakePipeWrite(ch->ends[1]);
    }
    pthread_mutex_unlock(&gWakeLock);
    if (cancel_off) {
        pthread_setcancelstate(cancel_state, NULL);
    }
    errno = saved_errno;
}
