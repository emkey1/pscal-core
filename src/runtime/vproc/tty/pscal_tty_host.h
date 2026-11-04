// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// What the terminal layer asks about the calling process, and the three calls vproc
// makes back to say which terminal controls a session.
#pragma once

#include "runtime/vproc/tty/pscal_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tty;

/* vproc.c supplies these; pscal_tty_host.c has weak do-nothing versions for a build
 * without vproc. All but the first take the task-table lock, and resolving the shell
 * of a thread with no vproc can take the session registry's, so the layer calls them
 * holding no lock of its own. */
int pscalTtyCurrentPid(void);
int pscalTtyCurrentPgid(void);
int pscalTtyCurrentSid(void);
bool pscalTtyIsSessionLeader(void);
void pscalTtySetForegroundPgid(int sid, int fg_pgid);
int pscalTtyGetForegroundPgid(int sid);
int pscalTtySendGroupSignal(int pgid, int sig);

/* pscal_tty.c supplies these. */
bool pscalTtyIsControlling(struct tty *tty);
void pscalTtySetControlling(struct tty *tty);
void pscalTtyClearControlling(struct tty *tty);

#ifdef __cplusplus
}
#endif
