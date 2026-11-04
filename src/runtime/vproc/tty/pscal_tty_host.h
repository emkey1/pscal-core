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

/* A thread that is no process of its own, such as a session's reader: nothing about job
 * control applies to it. */
bool pscalTtyCallerIsKernel(void);
/* Has the calling process ignored or blocked this signal? */
bool pscalTtySignalIgnoredOrBlocked(int sig);
/* Is the calling process's group orphaned: no member with a parent in another group of
 * the same session? */
bool pscalTtyCurrentGroupOrphaned(void);
/* Having sent the caller's own group this signal: 0 to start the call over, which is what
 * a stop that has been continued and a handler with SA_RESTART come to, else _EINTR. */
int pscalTtyAfterJobSignal(int sig);
/* May this group be a foreground group of this session? 0, or _EPERM if it belongs to
 * another, or _ESRCH if there is no such group. */
int pscalTtyCheckGroup(int sid, int pgid);
/* TIOCNOTTY by a process that doesn't lead its session: only that process lets go. */
void pscalTtyCallerGiveUpTerminal(void);
bool pscalTtyCallerGaveUpTerminal(void);

/* pscal_tty.c supplies these. */
bool pscalTtyIsControlling(struct tty *tty);
void pscalTtySetControlling(struct tty *tty);
void pscalTtyClearControlling(struct tty *tty);

#ifdef __cplusplus
}
#endif
