// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0

#include "runtime/vproc/tty/pscal_tty_host.h"

__attribute__((weak)) int pscalTtyCurrentPid(void) {
    return -1;
}

__attribute__((weak)) int pscalTtyCurrentPgid(void) {
    return -1;
}

__attribute__((weak)) int pscalTtyCurrentSid(void) {
    return -1;
}

__attribute__((weak)) bool pscalTtyIsSessionLeader(void) {
    return false;
}

__attribute__((weak)) void pscalTtySetForegroundPgid(int sid, int fg_pgid) {
    (void)sid;
    (void)fg_pgid;
}

__attribute__((weak)) int pscalTtyGetForegroundPgid(int sid) {
    (void)sid;
    return -1;
}

__attribute__((weak)) int pscalTtySendGroupSignal(int pgid, int sig) {
    (void)pgid;
    (void)sig;
    return 0;
}

__attribute__((weak)) bool pscalTtyCallerIsKernel(void) {
    return false;
}

__attribute__((weak)) bool pscalTtySignalIgnoredOrBlocked(int sig) {
    (void)sig;
    return false;
}

__attribute__((weak)) bool pscalTtyCurrentGroupOrphaned(void) {
    return false;
}

__attribute__((weak)) int pscalTtyAfterJobSignal(int sig) {
    (void)sig;
    return _EINTR;
}

__attribute__((weak)) int pscalTtyCheckGroup(int sid, int pgid) {
    (void)sid;
    (void)pgid;
    return 0;
}

__attribute__((weak)) int pscalTtySignalCheck(void) {
    return 0;
}

__attribute__((weak)) void pscalTtyCallerGiveUpTerminal(void) {
}

__attribute__((weak)) bool pscalTtyCallerGaveUpTerminal(void) {
    return false;
}
