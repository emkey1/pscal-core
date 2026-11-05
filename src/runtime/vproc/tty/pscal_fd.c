#include "runtime/vproc/tty/pscal_fd.h"
#include "runtime/vproc/vproc.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

struct pscal_fd *pscal_fd_create(const struct pscal_fd_ops *ops) {
    struct pscal_fd *fd = (struct pscal_fd *)calloc(1, sizeof(struct pscal_fd));
    if (!fd) {
        return NULL;
    }
    atomic_init(&fd->refcount, 1);
    fd->flags = 0;
    fd->ops = ops;
    list_init(&fd->poll_fds);
    list_init(&fd->tty_other_fds);
    fd->tty = NULL;
    lock_init(&fd->lock);
    return fd;
}

struct pscal_fd *pscal_fd_retain(struct pscal_fd *fd) {
    if (!fd) {
        return NULL;
    }
    atomic_fetch_add(&fd->refcount, 1);
    return fd;
}

int pscal_fd_close(struct pscal_fd *fd) {
    if (!fd) {
        return _EBADF;
    }
    if (atomic_fetch_sub(&fd->refcount, 1) != 1) {
        return 0;
    }
    int rc = 0;
    if (fd->ops && fd->ops->close) {
        rc = fd->ops->close(fd);
    }
    free(fd);
    return rc;
}

/*
 * One wake pipe per thread that polls, and a wakeup goes to all of them. With one pipe
 * for the whole process, whoever polled first emptied it for everyone. A writer that
 * filled an in-app pipe went straight back to poll, found its own wakeup and emptied it,
 * while the reader it was meant for was still on its way out of the kernel. The kernel
 * looks again before it returns, finds nothing and sleeps on, so ssh a | ssh b stopped
 * for good with both ends asleep. A thread only ever empties its own pipe, so a wakeup
 * stays there until that thread has looked.
 *
 * Unowned, as the pipe outlives whichever command the thread is running, and closing
 * one a command owned would leave its numbers to whatever opened next.
 */
typedef struct PscalPollWaiter {
    int fds[2];
    bool signalled; /* A byte is in the pipe or about to be: no need for another. */
    struct PscalPollWaiter *next;
} PscalPollWaiter;

static pthread_mutex_t gPollWaitersLock = PTHREAD_MUTEX_INITIALIZER;
static PscalPollWaiter *gPollWaiters = NULL;
static pthread_key_t gPollWaiterKey;
static pthread_once_t gPollWaiterKeyOnce = PTHREAD_ONCE_INIT;

static void pscalPollWaiterEnd(void *arg) {
    PscalPollWaiter *waiter = (PscalPollWaiter *)arg;
    pthread_mutex_lock(&gPollWaitersLock);
    for (PscalPollWaiter **at = &gPollWaiters; *at; at = &(*at)->next) {
        if (*at == waiter) {
            *at = waiter->next;
            break;
        }
    }
    pthread_mutex_unlock(&gPollWaitersLock);
    close(waiter->fds[0]);
    close(waiter->fds[1]);
    free(waiter);
}

static void pscalPollWaiterKeyInit(void) {
    (void)pthread_key_create(&gPollWaiterKey, pscalPollWaiterEnd);
}

static PscalPollWaiter *pscalPollWaiterCreate(void) {
    PscalPollWaiter *waiter = (PscalPollWaiter *)calloc(1, sizeof(*waiter));
    if (!waiter) {
        return NULL;
    }
    if (vprocHostPipeUnowned(waiter->fds) != 0) {
        free(waiter);
        return NULL;
    }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(waiter->fds[i], F_GETFL, 0);
        if (flags >= 0) {
            fcntl(waiter->fds[i], F_SETFL, flags | O_NONBLOCK);
        }
        int fd_flags = fcntl(waiter->fds[i], F_GETFD, 0);
        if (fd_flags >= 0) {
            fcntl(waiter->fds[i], F_SETFD, fd_flags | FD_CLOEXEC);
        }
    }
#ifdef F_SETNOSIGPIPE
    (void)fcntl(waiter->fds[1], F_SETNOSIGPIPE, 1);
#endif
    waiter->signalled = true;
    pthread_setspecific(gPollWaiterKey, waiter);
    pthread_mutex_lock(&gPollWaitersLock);
    waiter->next = gPollWaiters;
    gPollWaiters = waiter;
    pthread_mutex_unlock(&gPollWaitersLock);
    /* The caller checked its descriptors before it asked for this, so a wakeup could
     * have gone by. This makes its first poll return, for it to check again. */
    (void)vprocHostWrite(waiter->fds[1], "", 1);
    return waiter;
}

static PscalPollWaiter *pscalPollWaiterForThread(void) {
    pthread_once(&gPollWaiterKeyOnce, pscalPollWaiterKeyInit);
    PscalPollWaiter *waiter = (PscalPollWaiter *)pthread_getspecific(gPollWaiterKey);
    if (waiter) {
        return waiter;
    }
    /* fcntl can be a cancellation point, and a cancel before the key is set would leave
     * the pipe open with nothing to close it. */
    int cancel_state;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    waiter = pscalPollWaiterCreate();
    pthread_setcancelstate(cancel_state, NULL);
    return waiter;
}

int pscalPollWakeFd(void) {
    PscalPollWaiter *waiter = pscalPollWaiterForThread();
    return waiter ? waiter->fds[0] : -1;
}

void pscalPollDrain(void) {
    PscalPollWaiter *waiter = pscalPollWaiterForThread();
    if (!waiter) {
        return;
    }
    char buf[64];
    while (vprocHostRead(waiter->fds[0], buf, sizeof(buf)) > 0) {
        /* drain */
    }
    /* Cleared after the read and before the caller looks at its descriptors. A wakeup
     * that finds this still set comes before that look, which sees what it announced. */
    __atomic_store_n(&waiter->signalled, false, __ATOMIC_SEQ_CST);
}

void pscal_fd_poll_wakeup(struct pscal_fd *fd, int UNUSED(events)) {
    (void)fd;
    /* The write can be a cancellation point, and a thread that ended here would hold
     * the lock for good. */
    int cancel_state;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    pthread_mutex_lock(&gPollWaitersLock);
    for (PscalPollWaiter *waiter = gPollWaiters; waiter; waiter = waiter->next) {
        if (!__atomic_exchange_n(&waiter->signalled, true, __ATOMIC_SEQ_CST)) {
            (void)vprocHostWrite(waiter->fds[1], "", 1);
        }
    }
    pthread_mutex_unlock(&gPollWaitersLock);
    pthread_setcancelstate(cancel_state, NULL);
}
