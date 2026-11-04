// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0

#include "runtime/vproc/tty/pscal_compat.h"

#include <errno.h>

int pscalCompatErrno(int err) {
    if (err >= 0) {
        return 0;
    }
    switch (err) {
        case _EAGAIN:
            return EAGAIN;
        case _EINTR:
            return EINTR;
        case _EIO:
            return EIO;
        case _ENOTTY:
            return ENOTTY;
        case _EINVAL:
            return EINVAL;
        case _EPERM:
            return EPERM;
        case _ENOMEM:
            return ENOMEM;
        case _ENXIO:
            return ENXIO;
        case _ENOSPC:
            return ENOSPC;
        case _EPIPE:
            return EPIPE;
        case _ENOTSUP:
            return ENOTSUP;
        case _ETIMEDOUT:
            return ETIMEDOUT;
        case _EACCES:
            return EACCES;
        case _EBADF:
            return EBADF;
        case _ENOENT:
            return ENOENT;
        case _ESRCH:
            return ESRCH;
        case _EFAULT:
            return EFAULT;
        case _EBUSY:
            return EBUSY;
        case _EEXIST:
            return EEXIST;
        case _ENODEV:
            return ENODEV;
        case _ENOTDIR:
            return ENOTDIR;
        case _EISDIR:
            return EISDIR;
        case _EMFILE:
            return EMFILE;
        case _ENOTEMPTY:
            return ENOTEMPTY;
        default:
            return EIO;
    }
}
