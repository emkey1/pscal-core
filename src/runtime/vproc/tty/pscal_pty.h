// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Pseudo-terminal pairs: the master is held by whatever draws the screen and supplies
// keys, and programs see the slave as their terminal. A pair is found by its number.
#pragma once

#include "runtime/vproc/tty/pscal_compat.h"
#include "runtime/vproc/tty/pscal_fd.h"
#include "runtime/vproc/tty/pscal_tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which side a terminal is: compare tty->driver with their addresses. */
extern struct tty_driver pty_master;
extern struct tty_driver pty_slave;

/* The lowest free number gets a new pair. The slave starts locked and is not opened. */
int pscalPtyOpenMaster(int flags, struct pscal_fd **out_master, int *out_pty_num);
int pscalPtyOpenSlave(int pty_num, int flags, struct pscal_fd **out_slave);
int pscalPtyUnlock(struct pscal_fd *master);

bool pscalPtyIsMaster(struct pscal_fd *fd);
bool pscalPtyIsSlave(struct pscal_fd *fd);
bool pscalPtyExists(int pty_num);
int pscalPtyGetLimit(void);

/* These work from the moment a number is reserved. Get returns 0 or -1, which is not a
 * negative code in the usual sense; Set returns 0 or _ENOENT. */
int pscalPtyGetSlaveInfo(int pty_num, mode_t_ *perms, uid_t_ *uid, gid_t_ *gid);
int pscalPtySetSlaveInfo(int pty_num, const mode_t_ *perms, const uid_t_ *uid, const gid_t_ *gid);

/* From pscal_tty.c, as a pair's references and opens run out. */
void pscalPtySlaveLastClosed(struct tty *slave);
void pscalPtyMasterGone(struct tty *master);
void pscalPtySlaveGone(struct tty *slave);

#ifdef __cplusplus
}
#endif
