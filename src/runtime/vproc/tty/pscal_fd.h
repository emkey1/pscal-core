// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// An open file that lives inside the process: a terminal, an in-process pipe, the
// location device. It has no host descriptor, so a poller cannot wait on it directly.
// Each polling thread waits on a host wake pipe of its own instead, and every event a
// poller might want makes all of them readable.
#pragma once

#include <sys/types.h>

#include "runtime/vproc/tty/pscal_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pscal_fd;
struct tty;

/* Any entry may be NULL. Results follow pscalCompatErrno's convention. */
struct pscal_fd_ops {
    ssize_t (*read)(struct pscal_fd *fd, void *buf, size_t bufsize);
    ssize_t (*write)(struct pscal_fd *fd, const void *buf, size_t bufsize);
    int (*poll)(struct pscal_fd *fd);
    ssize_t (*ioctl_size)(int cmd);
    int (*ioctl)(struct pscal_fd *fd, int cmd, void *arg);
    int (*close)(struct pscal_fd *fd);
};

struct pscal_fd {
    unsigned flags; /* as given at open; change under lock */
    const struct pscal_fd_ops *ops;
    struct tty *tty; /* NULL when this is not a terminal */
    lock_t lock;
    void *userdata; /* belongs to the ops */
    int refs; /* private; atomic builtins, since C++ sources include this header */
};

struct pscal_fd *pscal_fd_create(const struct pscal_fd_ops *ops);
struct pscal_fd *pscal_fd_retain(struct pscal_fd *fd);

/* Drops a reference, and on the last one runs ops->close and frees the file. The
 * caller may hold a vproc's table lock or the session registry's lock. */
int pscal_fd_close(struct pscal_fd *fd);

/* The calling thread's wake descriptor, which a poller adds to its host poll, or -1
 * if no pipe could be made. It is made on first use, already readable, and closed
 * when the thread ends. When it is readable the poller calls pscalPollDrain, which
 * empties only this thread's pipe, and then asks every one of its files again. */
int pscalPollWakeFd(void);
void pscalPollDrain(void);

/* Tells every poller to ask its files again. Both arguments are accepted for the
 * callers' sake and ignored, since a poller asks all of its files on any wake. */
void pscal_fd_poll_wakeup(struct pscal_fd *fd, int events);

#ifdef __cplusplus
}
#endif
