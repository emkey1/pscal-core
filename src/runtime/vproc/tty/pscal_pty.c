// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0

#include "runtime/vproc/tty/pscal_pty.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/path_truncate.h"
#include "runtime/vproc/vproc.h"

int pscalHostOpenRaw(const char *path, int flags, mode_t mode);

struct tty_driver {
    const char *name;
};

struct tty_driver pty_master = {"pty master"};
struct tty_driver pty_slave = {"pty slave"};

#define PTY_LIMIT 4096

typedef enum {
    PTY_SLOT_FREE = 0,
    /* Chosen, with nothing made yet. The attributes already apply, and other allocators
     * must see it as taken while host nodes and terminals are being made. */
    PTY_SLOT_RESERVED,
    PTY_SLOT_LIVE,
} PtySlotState;

typedef struct {
    PtySlotState state;
    struct tty *slave; /* has no reference of its own: cleared before the slave is freed */
    mode_t_ perms;
    uid_t_ uid;
    gid_t_ gid;
} PtySlot;

static PtySlot gPtySlots[PTY_LIMIT];
static lock_t gPtySlotsLock = PTHREAD_MUTEX_INITIALIZER;

static bool ptyNumberInRange(int num) {
    return num >= 0 && num < PTY_LIMIT;
}

/* --- placeholders under /dev/pts, so that directory listings show the numbers ---
 *
 * Host calls here go around the shims. Removal runs from the close path, where the
 * caller may hold vproc's locks, so only the removal half may be reached from there. */

static void ptyHostMkdir(const char *virtual_path, mode_t mode) {
    char path[PATH_MAX];
    if (pathTruncateExpand(virtual_path, path, sizeof(path))) {
        vprocInterposeBypassEnter();
        (void)mkdir(path, mode);
        vprocInterposeBypassExit();
    }
}

static void ptyHostTouch(const char *virtual_path, int extra_flags, mode_t mode) {
    char path[PATH_MAX];
    if (pathTruncateExpand(virtual_path, path, sizeof(path))) {
        int fd = pscalHostOpenRaw(path, O_WRONLY | O_CREAT | extra_flags, mode);
        if (fd >= 0) {
            vprocHostClose(fd);
        }
    }
}

static void ptyHostNodeEnsure(int num, mode_t_ perms, uid_t_ uid, gid_t_ gid) {
    if (!pathTruncateEnabled()) {
        return;
    }
    /* open and close are cancellation points, and a number half set up is never undone. */
    int cancel_state = 0;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    ptyHostMkdir("/dev/pts", 0755);
    ptyHostTouch("/dev/pts/ptmx", 0, 0666);
    char node[32];
    snprintf(node, sizeof(node), "/dev/pts/%d", num);
    ptyHostTouch(node, O_EXCL, 0620);
    char path[PATH_MAX];
    if (pathTruncateExpand(node, path, sizeof(path))) {
        vprocInterposeBypassEnter();
        (void)chmod(path, (mode_t)(perms & 0777));
        (void)chown(path, (uid_t)uid, (gid_t)gid);
        vprocInterposeBypassExit();
    }
    pthread_setcancelstate(cancel_state, NULL);
}

static void ptyHostNodeRemove(int num) {
    if (!pathTruncateEnabled()) {
        return;
    }
    char node[32];
    snprintf(node, sizeof(node), "/dev/pts/%d", num);
    char path[PATH_MAX];
    if (pathTruncateExpand(node, path, sizeof(path))) {
        vprocInterposeBypassEnter();
        (void)unlink(path);
        vprocInterposeBypassExit();
    }
}

/* --- numbers --- */

/* Chooses the lowest number with nothing on it. -1 if all are taken. */
static int ptyReserveNumber(mode_t_ *perms, uid_t_ *uid, gid_t_ *gid) {
    const uid_t_ owner = (uid_t_)geteuid();
    const gid_t_ group = (gid_t_)getegid();
    int num = -1;
    lock(&gPtySlotsLock);
    for (int i = 0; i < PTY_LIMIT; i++) {
        PtySlot *slot = &gPtySlots[i];
        if (slot->state == PTY_SLOT_FREE) {
            slot->state = PTY_SLOT_RESERVED;
            slot->slave = NULL;
            slot->perms = 0620;
            slot->uid = owner;
            slot->gid = group;
            *perms = slot->perms;
            *uid = slot->uid;
            *gid = slot->gid;
            num = i;
            break;
        }
    }
    unlock(&gPtySlotsLock);
    return num;
}

static void ptyGiveBackNumber(int num) {
    ptyHostNodeRemove(num);
    lock(&gPtySlotsLock);
    gPtySlots[num].state = PTY_SLOT_FREE;
    gPtySlots[num].slave = NULL;
    unlock(&gPtySlotsLock);
}

/* --- pairs --- */

int pscalPtyOpenMaster(int flags, struct pscal_fd **out_master, int *out_pty_num) {
    if (!out_master || !out_pty_num) {
        return _EINVAL;
    }
    mode_t_ perms = 0;
    uid_t_ uid = 0;
    gid_t_ gid = 0;
    const int num = ptyReserveNumber(&perms, &uid, &gid);
    if (num < 0) {
        return _ENOSPC;
    }
    ptyHostNodeEnsure(num, perms, uid, gid);

    struct tty *master = ttyCreate(&pty_master, TTY_PSEUDO_MASTER_MAJOR, num);
    struct tty *slave = ttyCreate(&pty_slave, TTY_PSEUDO_SLAVE_MAJOR, num);
    if (!master || !slave) {
        /* Neither is linked or published yet, so this only frees them. */
        if (master) {
            ttyUnpin(master);
        }
        if (slave) {
            ttyUnpin(slave);
        }
        ptyGiveBackNumber(num);
        return _ENOMEM;
    }
    /* The reference ttyCreate gave the slave is the master's from here on. */
    master->pty.other = slave;
    slave->pty.other = master;
    slave->pty.slave_locked = true;

    lock(&gPtySlotsLock);
    gPtySlots[num].slave = slave;
    gPtySlots[num].state = PTY_SLOT_LIVE;
    unlock(&gPtySlotsLock);

    struct pscal_fd *fd = ttyOpenFile(master, flags);
    /* The open holds the master now; without one, this takes the pair down with it. */
    ttyRelease(master);
    if (!fd) {
        return _ENOMEM;
    }
    *out_master = fd;
    *out_pty_num = num;
    return 0;
}

int pscalPtyOpenSlave(int pty_num, int flags, struct pscal_fd **out_slave) {
    if (!out_slave) {
        return _EINVAL;
    }
    if (!ptyNumberInRange(pty_num)) {
        return _ENXIO;
    }
    struct tty *slave = NULL;
    lock(&gPtySlotsLock);
    PtySlot *slot = &gPtySlots[pty_num];
    if (slot->state == PTY_SLOT_LIVE && ttyRetainIfAlive(slot->slave)) {
        slave = slot->slave;
    }
    unlock(&gPtySlotsLock);
    if (!slave) {
        return _ENXIO;
    }
    lock(&slave->lock);
    const bool openable = slave->pty.other != NULL && !slave->pty.slave_locked;
    unlock(&slave->lock);
    int result = _EIO;
    if (openable) {
        struct pscal_fd *fd = ttyOpenFile(slave, flags);
        if (fd) {
            *out_slave = fd;
            result = 0;
        } else {
            result = _ENOMEM;
        }
    }
    ttyRelease(slave);
    return result;
}

int pscalPtyUnlock(struct pscal_fd *master) {
    if (!master || !master->ops || !master->ops->ioctl) {
        return _EINVAL;
    }
    dword_t unlocked = 0;
    return master->ops->ioctl(master, TIOCSPTLCK_, &unlocked);
}

bool pscalPtyIsMaster(struct pscal_fd *fd) {
    return fd && fd->tty && fd->tty->driver == &pty_master;
}

bool pscalPtyIsSlave(struct pscal_fd *fd) {
    return fd && fd->tty && fd->tty->driver == &pty_slave;
}

bool pscalPtyExists(int pty_num) {
    if (!ptyNumberInRange(pty_num)) {
        return false;
    }
    lock(&gPtySlotsLock);
    const bool taken = gPtySlots[pty_num].state != PTY_SLOT_FREE;
    unlock(&gPtySlotsLock);
    return taken;
}

int pscalPtyGetLimit(void) {
    return PTY_LIMIT;
}

int pscalPtyGetSlaveInfo(int pty_num, mode_t_ *perms, uid_t_ *uid, gid_t_ *gid) {
    if (!ptyNumberInRange(pty_num)) {
        return -1;
    }
    int result = -1;
    lock(&gPtySlotsLock);
    const PtySlot *slot = &gPtySlots[pty_num];
    if (slot->state != PTY_SLOT_FREE) {
        if (perms) {
            *perms = slot->perms;
        }
        if (uid) {
            *uid = slot->uid;
        }
        if (gid) {
            *gid = slot->gid;
        }
        result = 0;
    }
    unlock(&gPtySlotsLock);
    return result;
}

int pscalPtySetSlaveInfo(int pty_num, const mode_t_ *perms, const uid_t_ *uid, const gid_t_ *gid) {
    if (!ptyNumberInRange(pty_num)) {
        return _ENOENT;
    }
    lock(&gPtySlotsLock);
    PtySlot *slot = &gPtySlots[pty_num];
    if (slot->state == PTY_SLOT_FREE) {
        unlock(&gPtySlotsLock);
        return _ENOENT;
    }
    if (perms) {
        slot->perms = *perms;
    }
    if (uid) {
        slot->uid = *uid;
    }
    if (gid) {
        slot->gid = *gid;
    }
    const mode_t_ now_perms = slot->perms;
    const uid_t_ now_uid = slot->uid;
    const gid_t_ now_gid = slot->gid;
    /* A slave can outlive its master, whose going already took the node away. */
    bool node_wanted = true;
    if (slot->slave) {
        lock(&slot->slave->lock);
        node_wanted = slot->slave->pty.other != NULL;
        unlock(&slot->slave->lock);
    }
    unlock(&gPtySlotsLock);
    if (node_wanted) {
        ptyHostNodeEnsure(pty_num, now_perms, now_uid, now_gid);
    }
    return 0;
}

/* --- the end of a pair ---
 *
 * These run on the close path. No hooks and no vproc locks: see the note on
 * pscal_fd_close. */

void pscalPtySlaveLastClosed(struct tty *slave) {
    lock(&slave->lock);
    struct tty *master = slave->pty.other;
    if (master) {
        ttyPin(master);
    }
    unlock(&slave->lock);
    if (master) {
        ttyMarkHungUp(master);
        ttyUnpin(master);
    }
}

void pscalPtyMasterGone(struct tty *master) {
    ttyMarkHungUp(master);
    lock(&master->lock);
    struct tty *slave = master->pty.other;
    master->pty.other = NULL;
    unlock(&master->lock);
    if (!slave) {
        return;
    }
    /* A session's teardown waits for a reader parked in a read of the slave, so this is
     * what lets that finish. */
    lock(&slave->lock);
    slave->pty.other = NULL;
    unlock(&slave->lock);
    ttyMarkHungUp(slave);
    /* Before the release, so that a number cannot be reused with this node still here. */
    ptyHostNodeRemove(master->num);
    ttyRelease(slave);
}

void pscalPtySlaveGone(struct tty *slave) {
    lock(&gPtySlotsLock);
    PtySlot *slot = &gPtySlots[slave->num];
    if (slot->slave == slave) {
        slot->state = PTY_SLOT_FREE;
        slot->slave = NULL;
    }
    unlock(&gPtySlotsLock);
}
