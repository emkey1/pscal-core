// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Locks, since a mistake here hangs a whole tab or the whole app. A thread holds at
// most one terminal's lock, and never across a wait for another terminal or a call to
// a hook, which can take the task table's lock. Where two are held together the order
// is a terminal's, then gSessionsLock. pscalTtyDropSession arrives with the task
// table already locked, and vprocOpenShim asks the task table with tty->lock held, so
// that path must not take a terminal's lock at all; see the note there.

#include "runtime/vproc/tty/pscal_tty.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "runtime/vproc/tty/pscal_pty.h"
#include "runtime/vproc/tty/pscal_tty_host.h"

_Static_assert(sizeof(struct termios_) == 36, "struct termios_ is an ABI");
_Static_assert(sizeof(struct winsize_) == 8, "struct winsize_ is an ABI");

/* --- the queue --- */

enum {
    TTY_CELL_BYTE = 0,
    TTY_CELL_LINE_END, /* a byte that finished a line */
    TTY_CELL_EOF_MARK, /* no byte: the end-of-file key */
};

static void ttyQueuePush(TtyQueue *q, byte_t value, byte_t kind) {
    TtyCell *cell = &q->cells[(q->head + q->used) % TTY_BUF_SIZE];
    cell->value = value;
    cell->kind = kind;
    q->used++;
    if (kind != TTY_CELL_BYTE) {
        q->released = q->used;
    }
}

static TtyCell ttyQueuePop(TtyQueue *q) {
    TtyCell cell = q->cells[q->head];
    q->head = (q->head + 1) % TTY_BUF_SIZE;
    q->used--;
    if (q->released > 0) {
        q->released--;
    }
    return cell;
}

/* The last position of the line being typed. The caller checks there is one. */
static TtyCell ttyQueueDropLast(TtyQueue *q) {
    q->used--;
    return q->cells[(q->head + q->used) % TTY_BUF_SIZE];
}

static bool ttyQueueHasUnfinished(const TtyQueue *q) {
    return q->used > q->released;
}

static bool ttyQueueHeadIsMark(const TtyQueue *q) {
    return q->used > 0 && q->cells[q->head].kind == TTY_CELL_EOF_MARK;
}

static void ttyQueueClearLocked(struct tty *tty) {
    tty->queue.head = 0;
    tty->queue.used = 0;
    tty->queue.released = 0;
    pthread_cond_broadcast(&tty->writable);
}

/* --- life of a terminal --- */

static const struct termios_ kSlaveTermios = {
    .iflags = ICRNL_ | IXON_,
    .oflags = OPOST_ | ONLCR_,
    .cflags = 0,
    .lflags = ISIG_ | ICANON_ | ECHO_ | ECHOE_ | ECHOK_ | ECHOCTL_ | ECHOKE_ | IEXTEN_,
    .line = 0,
    .cc = {
        [VINTR_] = 0x03,
        [VQUIT_] = 0x1c,
        [VERASE_] = 0x7f,
        [VKILL_] = 0x15,
        [VEOF_] = 0x04,
        [VTIME_] = 0,
        [VMIN_] = 1,
        [VSTART_] = 0x11,
        [VSTOP_] = 0x13,
        [VSUSP_] = 0x1a,
        [VREPRINT_] = 0x12,
        [VDISCARD_] = 0x0f,
        [VWERASE_] = 0x17,
        [VLNEXT_] = 0x16,
    },
};

struct tty *ttyCreate(struct tty_driver *driver, int type, int num) {
    struct tty *tty = (struct tty *)calloc(1, sizeof(*tty));
    if (!tty) {
        return NULL;
    }
    if (pthread_mutex_init(&tty->lock, NULL) != 0) {
        free(tty);
        return NULL;
    }
    if (pthread_cond_init(&tty->readable, NULL) != 0) {
        pthread_mutex_destroy(&tty->lock);
        free(tty);
        return NULL;
    }
    if (pthread_cond_init(&tty->writable, NULL) != 0) {
        pthread_cond_destroy(&tty->readable);
        pthread_mutex_destroy(&tty->lock);
        free(tty);
        return NULL;
    }
    tty->driver = driver;
    tty->type = type;
    tty->num = num;
    tty->refs = 1;
    tty->pins = 1;
    tty->termios = kSlaveTermios;
    if (driver == &pty_master) {
        tty->termios.iflags = 0;
        tty->termios.oflags = 0;
        tty->termios.lflags = 0;
    }
    return tty;
}

void ttyRetain(struct tty *tty) {
    __atomic_add_fetch(&tty->refs, 1, __ATOMIC_RELAXED);
}

/* For a lookup by number, which can find a slave whose last reference is going. */
bool ttyRetainIfAlive(struct tty *tty) {
    int refs = __atomic_load_n(&tty->refs, __ATOMIC_RELAXED);
    while (refs > 0) {
        if (__atomic_compare_exchange_n(&tty->refs, &refs, refs + 1, true,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return true;
        }
    }
    return false;
}

void ttyRelease(struct tty *tty) {
    if (__atomic_sub_fetch(&tty->refs, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    if (tty->driver == &pty_master) {
        pscalPtyMasterGone(tty);
    } else {
        pscalPtySlaveGone(tty);
    }
    ttyUnpin(tty);
}

/* A thread writing to the other side has no reference to it, and taking one would be
 * wrong: if the writer is blocked on a full queue, the reference would keep the other
 * side from ever being over, and being over is what would unblock the writer. So it
 * pins the memory only. The other side's pointer is read under this side's lock, and
 * is cleared under it before the last pin can go. */
void ttyPin(struct tty *tty) {
    __atomic_add_fetch(&tty->pins, 1, __ATOMIC_RELAXED);
}

void ttyUnpin(struct tty *tty) {
    if (__atomic_sub_fetch(&tty->pins, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    pthread_cond_destroy(&tty->readable);
    pthread_cond_destroy(&tty->writable);
    pthread_mutex_destroy(&tty->lock);
    free(tty);
}

static void ttyUnpinCancelled(void *tty) {
    ttyUnpin((struct tty *)tty);
}

static bool ttyIsMaster(const struct tty *tty) {
    return tty->driver == &pty_master;
}

static bool ttyLineModeLocked(const struct tty *tty) {
    return !ttyIsMaster(tty) && (tty->termios.lflags & ICANON_) != 0;
}

/* A hung-up terminal, or a master whose slave hung up. */
static bool ttyIsDownLocked(const struct tty *tty) {
    return tty->hung_up || tty->pty.peer_hung_up;
}

static void ttyWakeEveryoneLocked(struct tty *tty) {
    pthread_cond_broadcast(&tty->readable);
    pthread_cond_broadcast(&tty->writable);
}

void ttyMarkHungUp(struct tty *tty) {
    lock(&tty->lock);
    tty->hung_up = true;
    ttyWakeEveryoneLocked(tty);
    unlock(&tty->lock);
    pscal_fd_poll_wakeup(NULL, 0);
}

void tty_hangup(struct tty *tty) {
    if (!tty) {
        return;
    }
    tty->hung_up = true;
    ttyWakeEveryoneLocked(tty);
    struct tty *master = NULL;
    if (!ttyIsMaster(tty) && tty->pty.other) {
        master = tty->pty.other;
        ttyPin(master);
    }
    /* The master's lock can't be taken with this one held, and a host write is not
     * something to hold a lock across. The caller holds this lock only for this call,
     * so letting go of it for a moment changes nothing for them. */
    unlock(&tty->lock);
    if (master) {
        lock(&master->lock);
        master->pty.peer_hung_up = true;
        ttyWakeEveryoneLocked(master);
        unlock(&master->lock);
        ttyUnpin(master);
    }
    pscal_fd_poll_wakeup(NULL, 0);
    lock(&tty->lock);
}

/* --- waiting --- */

/* pthread_cond_wait comes back holding the mutex even when cancelled, so a `kill` would
 * leave the lock held by a dead thread, and the terminal hung, without this. */
static void ttyWaitCancelled(void *tty) {
    unlock(&((struct tty *)tty)->lock);
}

static int ttyWaitLocked(struct tty *tty, pthread_cond_t *cond, const struct timespec *until) {
    int result = 0;
    pthread_cleanup_push(ttyWaitCancelled, tty);
    if (until) {
        result = pthread_cond_timedwait(cond, &tty->lock, until);
    } else {
        result = pthread_cond_wait(cond, &tty->lock);
    }
    pthread_cleanup_pop(0);
    return result;
}

static struct timespec ttyTimeAfterTenths(unsigned tenths) {
    struct timespec at;
    clock_gettime(CLOCK_REALTIME, &at);
    at.tv_sec += tenths / 10;
    at.tv_nsec += (long)(tenths % 10) * 100000000L;
    if (at.tv_nsec >= 1000000000L) {
        at.tv_sec += 1;
        at.tv_nsec -= 1000000000L;
    }
    return at;
}

/* --- input --- */

/* The work of a tty_input call that can't be done under the lock. Each byte is handled
 * under the lock, with what it leads to collected here until the lock is let go. */
typedef struct {
    struct tty *tty;
    bool wake_pollers;
    bool announced; /* readers were told about the queue before this writer's wait */
    unsigned signals; /* bits indexing kKeySignals */
    size_t echo_len;
    unsigned char echo[512];
} TtyInputRun;

/* In ascending signal number, on every host this runs on. */
static const int kKeySignals[] = {SIGINT_, SIGQUIT_, SIGTSTP_};

/* A byte of 0 never matches, which is how a cc entry is switched off. */
static bool ttyByteIs(const struct termios_ *termios, byte_t byte, int index) {
    return byte != 0 && termios->cc[index] == byte;
}

/* Drops the lock to do what has been collected. On return the state of the terminal
 * may be anything, so whatever depends on it has to be looked at again. Signals wait
 * for the end of the call if they can, to go once however many keys asked for them. */
static void ttyRunFlushLocked(TtyInputRun *run, bool with_signals) {
    const unsigned signals = with_signals ? run->signals : 0;
    if (run->echo_len == 0 && !run->wake_pollers && signals == 0) {
        return;
    }
    struct tty *tty = run->tty;
    struct tty *peer = NULL;
    if (run->echo_len > 0 && tty->pty.other) {
        peer = tty->pty.other;
        ttyPin(peer);
    }
    const size_t echo_len = run->echo_len;
    const bool wake_pollers = run->wake_pollers;
    const pid_t_ foreground = tty->fg_group;
    run->echo_len = 0;
    run->wake_pollers = false;
    run->signals &= ~signals;
    unlock(&tty->lock);

    if (peer) {
        /* Never waits. If the other side is full the echo is lost, since whoever is
         * typing may be the UI. */
        (void)tty_input(peer, (const char *)run->echo, echo_len, false);
        ttyUnpin(peer);
    }
    if (wake_pollers) {
        pscal_fd_poll_wakeup(NULL, 0);
    }
    if (signals != 0 && foreground > 0) {
        for (size_t i = 0; i < sizeof(kKeySignals) / sizeof(kKeySignals[0]); i++) {
            if (signals & (1u << i)) {
                (void)pscalTtySendGroupSignal(foreground, kKeySignals[i]);
            }
        }
    }
    lock(&tty->lock);
}

static void ttyEchoLocked(TtyInputRun *run, const char *bytes, size_t len) {
    if (run->echo_len + len > sizeof(run->echo)) {
        ttyRunFlushLocked(run, false);
    }
    memcpy(run->echo + run->echo_len, bytes, len);
    run->echo_len += len;
}

static bool ttyShowsAsControl(const struct termios_ *termios, byte_t byte) {
    if (!(termios->lflags & ECHOCTL_)) {
        return false;
    }
    if (byte >= 0x20 && byte != 0x7f) {
        return false;
    }
    if (byte == '\t' || byte == '\n') {
        return false;
    }
    return !ttyByteIs(termios, byte, VSTART_) && !ttyByteIs(termios, byte, VSTOP_);
}

static void ttyEchoByteLocked(TtyInputRun *run, byte_t byte) {
    if (ttyShowsAsControl(&run->tty->termios, byte)) {
        const char shown[2] = {'^', (char)(byte ^ 0x40)};
        ttyEchoLocked(run, shown, sizeof(shown));
    } else {
        const char shown = (char)byte;
        ttyEchoLocked(run, &shown, 1);
    }
}

static void ttyEchoRubOutLocked(TtyInputRun *run, byte_t removed) {
    /* A control byte took two columns to show. */
    const bool twice = ttyShowsAsControl(&run->tty->termios, removed);
    ttyEchoLocked(run, "\b \b", 3);
    if (twice) {
        ttyEchoLocked(run, "\b \b", 3);
    }
}

static void ttyLineEndedLocked(TtyInputRun *run) {
    pthread_cond_broadcast(&run->tty->readable);
    run->wake_pollers = true;
}

/* Can a line end go in? When the queue is full with a finished line in it, a reader
 * will make room. When it is full with none, which only text queued in non-canonical
 * mode can do, no reader ever will, so the line loses its last byte to its end. */
static bool ttyMakeRoomForLineEndLocked(struct tty *tty) {
    TtyQueue *q = &tty->queue;
    if (q->used < TTY_BUF_SIZE) {
        return true;
    }
    if (q->released > 0) {
        return false;
    }
    (void)ttyQueueDropLast(q);
    return true;
}

/* VINTR, VQUIT or VSUSP with ISIG on. The signal goes after the lock is let go,
 * because sending one takes the task table's. */
static void ttySignalKeyLocked(TtyInputRun *run, int signal_index) {
    struct tty *tty = run->tty;
    if (tty->fg_group == 0) {
        return;
    }
    if (!(tty->termios.lflags & NOFLSH_)) {
        ttyQueueClearLocked(tty);
    }
    run->signals |= 1u << signal_index;
}

static int ttyKeySignalIndex(const struct termios_ *termios, byte_t byte) {
    if (ttyByteIs(termios, byte, VINTR_)) {
        return 0;
    }
    if (ttyByteIs(termios, byte, VQUIT_)) {
        return 1;
    }
    if (ttyByteIs(termios, byte, VSUSP_)) {
        return 2;
    }
    return -1;
}

/* A byte in line mode. False if it has to wait for room. Each return path has finished
 * with the terminal's state before its last echo, which can drop the lock. */
static bool ttyInputLineByteLocked(TtyInputRun *run, byte_t byte) {
    struct tty *tty = run->tty;
    TtyQueue *q = &tty->queue;
    const struct termios_ *termios = &tty->termios;
    const dword_t lflags = termios->lflags;
    const bool echo = (lflags & ECHO_) != 0;

    if ((termios->iflags & INLCR_) && byte == '\n') {
        byte = '\r';
    } else if ((termios->iflags & ICRNL_) && byte == '\r') {
        byte = '\n';
    }
    if ((termios->iflags & IGNCR_) && byte == '\r') {
        return true;
    }

    if (ttyByteIs(termios, byte, VERASE_)) {
        if (ttyQueueHasUnfinished(q)) {
            const TtyCell removed = ttyQueueDropLast(q);
            if (echo && (lflags & ECHOE_)) {
                ttyEchoRubOutLocked(run, removed.value);
            }
        }
        return true;
    }
    if (ttyByteIs(termios, byte, VKILL_)) {
        const bool rub_out = echo && (lflags & ECHOK_);
        while (ttyQueueHasUnfinished(q)) {
            const TtyCell removed = ttyQueueDropLast(q);
            if (rub_out) {
                ttyEchoRubOutLocked(run, removed.value);
            }
        }
        return true;
    }
    if (ttyByteIs(termios, byte, VEOF_)) {
        if (!ttyMakeRoomForLineEndLocked(tty)) {
            return false;
        }
        ttyQueuePush(q, 0, TTY_CELL_EOF_MARK);
        ttyLineEndedLocked(run);
        return true;
    }
    if (byte == '\n' || ttyByteIs(termios, byte, VEOL_)) {
        if (!ttyMakeRoomForLineEndLocked(tty)) {
            return false;
        }
        ttyQueuePush(q, byte, TTY_CELL_LINE_END);
        ttyLineEndedLocked(run);
        if (echo) {
            ttyEchoLocked(run, "\r\n", 2);
        }
        return true;
    }

    const bool interrupt_key = ttyByteIs(termios, byte, VINTR_);
    const bool suspend_key = ttyByteIs(termios, byte, VSUSP_);
    if ((interrupt_key || suspend_key) && tty->session_reader && !(lflags & ISIG_)) {
        /* A session's reader only sees finished lines. Left in the line being typed, the
         * key would do nothing until Return, and Ctrl-C at `cat` would look dead. It
         * takes what was typed with it, so that lines typed ahead don't run afterwards. */
        if (!(lflags & NOFLSH_)) {
            ttyQueueClearLocked(tty);
        }
        if (!ttyMakeRoomForLineEndLocked(tty)) {
            return false;
        }
        ttyQueuePush(q, byte, TTY_CELL_LINE_END);
        ttyLineEndedLocked(run);
        if (echo && ttyShowsAsControl(termios, byte)) {
            ttyEchoByteLocked(run, byte);
            if (interrupt_key) {
                /* A real system's shell moves to a new line when its child dies from the
                 * signal. This command is still alive, so nothing else would. */
                ttyEchoLocked(run, "\r\n", 2);
            }
        }
        return true;
    }
    if (lflags & ISIG_) {
        const int signal_index = ttyKeySignalIndex(termios, byte);
        if (signal_index >= 0) {
            ttySignalKeyLocked(run, signal_index);
            if (echo) {
                ttyEchoByteLocked(run, byte);
            }
            return true;
        }
    }

    /* Nothing can drain a line with no end, so one that would leave no position for its
     * end loses the byte. Waiting here would be for ever, and the writer may be the UI. */
    if (q->used + 1 >= TTY_BUF_SIZE) {
        if (q->released == 0) {
            return true;
        }
        if (q->used == TTY_BUF_SIZE) {
            return false;
        }
    }
    ttyQueuePush(q, byte, TTY_CELL_BYTE);
    if (echo) {
        ttyEchoByteLocked(run, byte);
    }
    return true;
}

/* A byte for a master, or for a slave out of line mode: no translation and no echo,
 * since the prompt and the frontends leave ICRNL and ECHO set and don't expect either. */
static bool ttyInputRawByteLocked(TtyInputRun *run, byte_t byte) {
    struct tty *tty = run->tty;
    TtyQueue *q = &tty->queue;
    if (!ttyIsMaster(tty) && (tty->termios.lflags & ISIG_)) {
        const int signal_index = ttyKeySignalIndex(&tty->termios, byte);
        if (signal_index >= 0) {
            ttySignalKeyLocked(run, signal_index);
            return true;
        }
    }
    if (q->used == TTY_BUF_SIZE) {
        return false;
    }
    if (q->used == 0) {
        pthread_cond_broadcast(&tty->readable);
        run->wake_pollers = true;
    }
    ttyQueuePush(q, byte, TTY_CELL_BYTE);
    return true;
}

ssize_t tty_input(struct tty *tty, const char *input, size_t len, bool blocking) {
    if (len == 0) {
        return 0;
    }
    if (!tty || !input) {
        return _EINVAL;
    }
    TtyInputRun run;
    run.tty = tty;
    run.wake_pollers = false;
    run.announced = false;
    run.signals = 0;
    run.echo_len = 0;

    size_t consumed = 0;
    lock(&tty->lock);
    while (consumed < len) {
        const byte_t byte = (byte_t)input[consumed];
        const bool taken = ttyLineModeLocked(tty) ? ttyInputLineByteLocked(&run, byte)
                                                  : ttyInputRawByteLocked(&run, byte);
        if (taken) {
            consumed++;
            continue;
        }
        /* Nobody will read a hung-up terminal, so a wait for room there would never end. */
        if (!blocking || tty->hung_up) {
            break;
        }
        if (!ttyLineModeLocked(tty) && !run.announced) {
            pthread_cond_broadcast(&tty->readable);
            run.wake_pollers = true;
            run.announced = true;
        }
        /* Everything goes out before the wait, as a `kill` may end it. A flush drops the
         * lock, so room may have appeared with no one left to say so: look again. */
        if (run.echo_len > 0 || run.wake_pollers || run.signals != 0) {
            ttyRunFlushLocked(&run, true);
            continue;
        }
        (void)ttyWaitLocked(tty, &tty->writable, NULL);
        run.announced = false;
    }
    if (!ttyLineModeLocked(tty) && tty->queue.used > 0) {
        pthread_cond_broadcast(&tty->readable);
        run.wake_pollers = true;
    }
    const bool hung_up = tty->hung_up;
    ttyRunFlushLocked(&run, true);
    unlock(&tty->lock);

    if (consumed > 0) {
        return (ssize_t)consumed;
    }
    return (blocking && hung_up) ? _EIO : _EAGAIN;
}

/* --- read --- */

/* A line-mode read. At most one line, and a line's end stops it. */
static ssize_t ttyCopyLineLocked(struct tty *tty, unsigned char *out, size_t size) {
    TtyQueue *q = &tty->queue;
    size_t copied = 0;
    bool line_ended = false;
    while (copied < size && q->used > 0 && !ttyQueueHeadIsMark(q)) {
        const TtyCell cell = ttyQueuePop(q);
        out[copied++] = cell.value;
        if (cell.kind == TTY_CELL_LINE_END) {
            line_ended = true;
            break;
        }
    }
    /* A mark with nothing before it is the end of the file, for this one read. One right
     * behind text is what released that text, and its work is done now. One behind a
     * finished line released nothing, and still means the end of the file. */
    if (copied == 0) {
        tty->eof_reads++;
    }
    if (ttyQueueHeadIsMark(q) && !line_ended) {
        (void)ttyQueuePop(q);
    }
    pthread_cond_broadcast(&tty->writable);
    return (ssize_t)copied;
}

/* A non-canonical read, of whatever is there. Line-mode text left behind by a change of
 * mode counts as plain bytes. */
static ssize_t ttyCopyRawLocked(struct tty *tty, unsigned char *out, size_t size) {
    TtyQueue *q = &tty->queue;
    size_t copied = 0;
    bool line_ended = false;
    while (copied < size && q->used > 0) {
        const TtyCell cell = ttyQueuePop(q);
        out[copied++] = cell.value;
        line_ended = cell.kind == TTY_CELL_LINE_END;
    }
    if (copied > 0) {
        if (!line_ended && ttyQueueHeadIsMark(q)) {
            (void)ttyQueuePop(q);
        }
        pthread_cond_broadcast(&tty->writable);
    }
    return (ssize_t)copied;
}

/* A reader outside the foreground group of its own controlling terminal gets SIGTTIN.
 * No lock is held for the hooks, since they take the task table's. */
static int ttyCheckBackgroundRead(struct tty *tty) {
    if (!pscalTtyIsControlling(tty)) {
        return 0;
    }
    lock(&tty->lock);
    const pid_t_ foreground = tty->fg_group;
    unlock(&tty->lock);
    if (foreground == 0) {
        return 0;
    }
    const int group = pscalTtyCurrentPgid();
    if (group == foreground) {
        return 0;
    }
    return pscalTtySendGroupSignal(group, SIGTTIN_) == 0 ? _EINTR : _EIO;
}

static bool ttyFileIsNonBlocking(struct pscal_fd *fd) {
    lock(&fd->lock);
    const bool nonblocking = (fd->flags & O_NONBLOCK) != 0;
    unlock(&fd->lock);
    return nonblocking;
}

static ssize_t ttyFileRead(struct pscal_fd *fd, void *buf, size_t bufsize) {
    if (bufsize == 0) {
        return 0;
    }
    struct tty *tty = fd->tty;
    if (!tty) {
        return _EBADF;
    }
    lock(&tty->lock);
    const bool down = ttyIsDownLocked(tty);
    unlock(&tty->lock);
    if (down) {
        return 0;
    }
    const bool master = ttyIsMaster(tty);
    if (!master) {
        const int err = ttyCheckBackgroundRead(tty);
        if (err < 0) {
            return err;
        }
    }
    const bool nonblocking = ttyFileIsNonBlocking(fd);

    unsigned char *out = (unsigned char *)buf;
    size_t room = bufsize;
    ssize_t prefix = 0;
    lock(&tty->lock);
    if (master && tty->pty.packet_mode) {
        /* The status byte, which no event ever sets. */
        out[0] = 0;
        prefix = 1;
        if (bufsize == 1) {
            unlock(&tty->lock);
            return 1;
        }
        out++;
        room--;
    }

    /* Every wake starts over from the current mode, because a session's reader sits in
     * here while the shell switches modes at every command. */
    TtyQueue *q = &tty->queue;
    ssize_t result = 0;
    bool timed_out = false;
    for (;;) {
        if (ttyIsDownLocked(tty)) {
            result = 0;
            break;
        }
        if (ttyLineModeLocked(tty)) {
            if (q->released > 0) {
                result = ttyCopyLineLocked(tty, out, room);
                break;
            }
            if (nonblocking) {
                result = _EAGAIN;
                break;
            }
            (void)ttyWaitLocked(tty, &tty->readable, NULL);
            continue;
        }
        const unsigned wanted = master ? 1u : tty->termios.cc[VMIN_];
        const unsigned tenths = master ? 0u : tty->termios.cc[VTIME_];
        if (q->used >= wanted || (timed_out && q->used > 0)) {
            result = ttyCopyRawLocked(tty, out, room);
            break;
        }
        if (nonblocking) {
            result = _EAGAIN;
            break;
        }
        if (q->used > 0 && tenths > 0) {
            const struct timespec until = ttyTimeAfterTenths(tenths);
            timed_out = ttyWaitLocked(tty, &tty->readable, &until) == ETIMEDOUT;
        } else {
            timed_out = false;
            (void)ttyWaitLocked(tty, &tty->readable, NULL);
        }
    }
    unlock(&tty->lock);
    return result > 0 ? result + prefix : result;
}

/* --- write --- */

/* Output processing, a bufferful at a time so that nothing needs freeing if a `kill`
 * finds this thread waiting for room on the other side. */
#define TTY_WRITE_CHUNK 1024

static ssize_t ttyWriteThrough(struct tty *peer,
                               dword_t oflags,
                               const unsigned char *in,
                               size_t len,
                               bool blocking) {
    char chunk[TTY_WRITE_CHUNK];
    size_t offset = 0;
    bool delivered = false;
    while (offset < len) {
        size_t out_len = 0;
        /* A byte can become two. */
        while (offset < len && out_len + 2 <= sizeof(chunk)) {
            unsigned char byte = in[offset++];
            if (oflags & OPOST_) {
                if (byte == '\r') {
                    if (oflags & ONLRET_) {
                        continue;
                    }
                    if (oflags & OCRNL_) {
                        byte = '\n';
                    }
                } else if (byte == '\n' && (oflags & ONLCR_)) {
                    chunk[out_len++] = '\r';
                }
            }
            chunk[out_len++] = (char)byte;
        }
        if (out_len == 0) {
            continue;
        }
        const ssize_t taken = tty_input(peer, chunk, out_len, blocking);
        if (taken < 0) {
            return delivered ? (ssize_t)len : taken;
        }
        delivered = true;
        /* Only a queue that stopped a non-blocking write leaves some behind. */
        if ((size_t)taken < out_len) {
            break;
        }
    }
    /* What was asked for, not what was expanded to: more than was asked for would look
     * like a bug to the caller. */
    return (ssize_t)len;
}

static ssize_t ttyFileWrite(struct pscal_fd *fd, const void *buf, size_t bufsize) {
    struct tty *tty = fd->tty;
    if (!tty) {
        return _EBADF;
    }
    const bool blocking = !ttyFileIsNonBlocking(fd);
    lock(&tty->lock);
    if (ttyIsDownLocked(tty) || !tty->pty.other) {
        unlock(&tty->lock);
        return _EIO;
    }
    struct tty *peer = tty->pty.other;
    const dword_t oflags = tty->termios.oflags;
    ttyPin(peer);
    unlock(&tty->lock);

    ssize_t result = 0;
    pthread_cleanup_push(ttyUnpinCancelled, peer);
    result = ttyWriteThrough(peer, oflags, (const unsigned char *)buf, bufsize, blocking);
    pthread_cleanup_pop(1);
    return result;
}

/* --- poll --- */

static int ttyFilePoll(struct pscal_fd *fd) {
    struct tty *tty = fd->tty;
    if (!tty) {
        return POLL_NVAL;
    }
    int events = POLL_WRITE;
    lock(&tty->lock);
    if (tty->hung_up) {
        events |= POLL_READ | POLL_ERR | POLL_HUP;
    } else if (tty->pty.peer_hung_up) {
        events |= POLL_READ | POLL_HUP;
    } else if (ttyLineModeLocked(tty)) {
        /* The unfinished part of a line must not count: callers poll before they read,
         * and would spin on a read that has to wait. */
        if (tty->queue.released > 0) {
            events |= POLL_READ;
        }
    } else if (tty->queue.used > 0) {
        events |= POLL_READ;
    }
    unlock(&tty->lock);
    return events;
}

/* --- the session map: which terminal controls each session --- */

typedef struct {
    pid_t_ sid;
    struct tty *tty; /* holds a reference */
} TtySessionEntry;

static TtySessionEntry *gSessions = NULL;
static size_t gSessionCount = 0;
static size_t gSessionCapacity = 0;
static lock_t gSessionsLock = PTHREAD_MUTEX_INITIALIZER;

static TtySessionEntry *ttySessionFindLocked(pid_t_ sid) {
    for (size_t i = 0; i < gSessionCount; i++) {
        if (gSessions[i].sid == sid) {
            return &gSessions[i];
        }
    }
    return NULL;
}

static bool ttySessionAddLocked(pid_t_ sid, struct tty *tty) {
    if (gSessionCount == gSessionCapacity) {
        const size_t capacity = gSessionCapacity ? gSessionCapacity * 2 : 8;
        TtySessionEntry *grown =
                (TtySessionEntry *)realloc(gSessions, capacity * sizeof(TtySessionEntry));
        if (!grown) {
            return false;
        }
        gSessions = grown;
        gSessionCapacity = capacity;
    }
    gSessions[gSessionCount].sid = sid;
    gSessions[gSessionCount].tty = tty;
    gSessionCount++;
    ttyRetain(tty);
    return true;
}

/* The terminal's reference passes to the caller, to release with no lock held. */
static struct tty *ttySessionRemoveLocked(TtySessionEntry *entry) {
    struct tty *tty = entry->tty;
    *entry = gSessions[--gSessionCount];
    return tty;
}

bool pscalTtyIsControlling(struct tty *tty) {
    if (!tty) {
        return false;
    }
    const int sid = pscalTtyCurrentSid();
    if (sid <= 0) {
        return false;
    }
    lock(&gSessionsLock);
    const TtySessionEntry *entry = ttySessionFindLocked((pid_t_)sid);
    const bool controlling = entry && entry->tty == tty;
    unlock(&gSessionsLock);
    return controlling;
}

void pscalTtySetControlling(struct tty *tty) {
    if (!tty) {
        return;
    }
    lock(&tty->lock);
    const pid_t_ sid = tty->session;
    unlock(&tty->lock);
    if (sid == 0) {
        return;
    }
    struct tty *replaced = NULL;
    lock(&gSessionsLock);
    TtySessionEntry *entry = ttySessionFindLocked(sid);
    if (!entry) {
        (void)ttySessionAddLocked(sid, tty);
    } else if (entry->tty != tty) {
        replaced = entry->tty;
        ttyRetain(tty);
        entry->tty = tty;
    }
    unlock(&gSessionsLock);
    if (replaced) {
        ttyRelease(replaced);
    }
}

void pscalTtyClearControlling(struct tty *tty) {
    if (!tty) {
        return;
    }
    struct tty *removed = NULL;
    lock(&tty->lock);
    if (tty->session != 0) {
        lock(&gSessionsLock);
        TtySessionEntry *entry = ttySessionFindLocked(tty->session);
        if (entry && entry->tty == tty) {
            removed = ttySessionRemoveLocked(entry);
        }
        unlock(&gSessionsLock);
        if (removed) {
            tty->session = 0;
            tty->fg_group = 0;
        }
    }
    unlock(&tty->lock);
    if (removed) {
        ttyRelease(removed);
    }
}

/* Clears a terminal's claim without its lock. The task table is locked when this is
 * called, and vprocOpenShim asks the task table with tty->lock held, so waiting for
 * that lock here could wait for ever. Readers of these two see either value. */
static void ttyForgetOwnerUnlocked(struct tty *tty, pid_t_ sid) {
    pid_t_ owner = sid;
    if (__atomic_compare_exchange_n(&tty->session, &owner, 0, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        __atomic_store_n(&tty->fg_group, 0, __ATOMIC_RELEASE);
    }
}

void pscalTtyDropSession(pid_t_ sid) {
    if (sid <= 0) {
        return;
    }
    struct tty *removed = NULL;
    lock(&gSessionsLock);
    TtySessionEntry *entry = ttySessionFindLocked(sid);
    if (entry) {
        removed = ttySessionRemoveLocked(entry);
    }
    unlock(&gSessionsLock);
    if (removed) {
        ttyForgetOwnerUnlocked(removed, sid);
        ttyRelease(removed);
    }
}

int pscalTtySessionPtyNum(pid_t_ sid) {
    if (sid <= 0) {
        return -1;
    }
    int num = -1;
    lock(&gSessionsLock);
    const TtySessionEntry *entry = ttySessionFindLocked(sid);
    if (entry && (entry->tty->type == TTY_PSEUDO_MASTER_MAJOR ||
                  entry->tty->type == TTY_PSEUDO_SLAVE_MAJOR)) {
        num = entry->tty->num;
    }
    unlock(&gSessionsLock);
    return num;
}

/* An open without O_NOCTTY by a session leader with no terminal takes this one, if it
 * is nobody's. The check of tty->session first saves the hooks on nearly every open. */
static void ttyClaimOnOpen(struct tty *tty) {
    lock(&tty->lock);
    const bool taken = tty->session != 0;
    unlock(&tty->lock);
    if (taken || !pscalTtyIsSessionLeader()) {
        return;
    }
    const int sid = pscalTtyCurrentSid();
    if (sid <= 0) {
        return;
    }
    const int group = pscalTtyCurrentPgid();
    const pid_t_ foreground = (pid_t_)(group > 0 ? group : sid);

    bool claimed = false;
    lock(&tty->lock);
    if (tty->session == 0) {
        lock(&gSessionsLock);
        if (!ttySessionFindLocked((pid_t_)sid) && ttySessionAddLocked((pid_t_)sid, tty)) {
            tty->session = (pid_t_)sid;
            tty->fg_group = foreground;
            claimed = true;
        }
        unlock(&gSessionsLock);
    }
    unlock(&tty->lock);
    if (claimed) {
        pscalTtySetForegroundPgid(sid, foreground);
    }
}

int pscalTtyOpenControlling(int flags, struct pscal_fd **out_fd) {
    if (!out_fd) {
        return _EINVAL;
    }
    *out_fd = NULL;
    const int sid = pscalTtyCurrentSid();
    if (sid <= 0) {
        return _ENXIO;
    }
    struct tty *tty = NULL;
    lock(&gSessionsLock);
    const TtySessionEntry *entry = ttySessionFindLocked((pid_t_)sid);
    if (entry) {
        tty = entry->tty;
        ttyRetain(tty);
    }
    unlock(&gSessionsLock);
    if (!tty) {
        return _ENXIO;
    }
    struct pscal_fd *fd = ttyOpenFile(tty, flags);
    ttyRelease(tty);
    if (!fd) {
        return _ENOMEM;
    }
    *out_fd = fd;
    return 0;
}

/* --- opens --- */

struct pscal_fd *ttyOpenFile(struct tty *tty, int flags) {
    struct pscal_fd *fd = pscal_fd_create(&pscal_tty_fd_ops);
    if (!fd) {
        return NULL;
    }
    fd->flags = (unsigned)flags;
    fd->tty = tty;
    ttyRetain(tty);
    lock(&tty->lock);
    tty->opens++;
    unlock(&tty->lock);
    if (!(flags & O_NOCTTY)) {
        ttyClaimOnOpen(tty);
    }
    return fd;
}

/* Runs on the close path, where the caller may hold a vproc's table lock or the session
 * registry's, so no hooks and no shims from here down. */
static int ttyFileClose(struct pscal_fd *fd) {
    struct tty *tty = fd->tty;
    if (!tty) {
        return 0;
    }
    lock(&tty->lock);
    const bool last_open = --tty->opens == 0;
    unlock(&tty->lock);
    /* The session map's reference doesn't count here: a terminal that became someone's
     * controlling terminal holds one, and the master must still learn the slave went. */
    if (last_open && tty->driver == &pty_slave) {
        pscalPtySlaveLastClosed(tty);
    }
    fd->tty = NULL;
    ttyRelease(tty);
    return 0;
}

/* --- ioctl --- */

/* TIOCSCTTY: the arg is the value itself, not a pointer. */
static int ttySetControllingForCaller(struct tty *tty, bool force) {
    const int sid = pscalTtyCurrentSid();
    if (sid <= 0 || !pscalTtyIsSessionLeader()) {
        return _EPERM;
    }
    const int group = pscalTtyCurrentPgid();
    const pid_t_ foreground = (pid_t_)(group > 0 ? group : sid);
    const pid_t_ caller = (pid_t_)sid;

    int result = 0;
    bool claimed = false;
    struct tty *previous = NULL;
    lock(&tty->lock);
    lock(&gSessionsLock);
    TtySessionEntry *entry = ttySessionFindLocked(caller);
    const pid_t_ owner = tty->session;
    if (entry && entry->tty == tty) {
        /* Already this caller's. */
    } else if (entry && owner != caller) {
        result = _EPERM;
    } else if (owner != 0 && owner != caller && !force) {
        result = _EPERM;
    } else {
        if (entry) {
            previous = ttySessionRemoveLocked(entry);
        }
        if (ttySessionAddLocked(caller, tty)) {
            tty->session = caller;
            tty->fg_group = foreground;
            claimed = true;
        }
    }
    unlock(&gSessionsLock);
    unlock(&tty->lock);
    if (previous) {
        ttyForgetOwnerUnlocked(previous, caller);
        ttyRelease(previous);
    }
    if (claimed) {
        pscalTtySetForegroundPgid(sid, foreground);
    }
    return result;
}

static int ttyGetForeground(struct tty *tty, pid_t_ *out) {
    if (!out) {
        return _EINVAL;
    }
    struct tty *side = tty;
    if (ttyIsMaster(tty)) {
        side = tty->pty.other;
        if (!side) {
            return _ENOTTY;
        }
    } else if (!pscalTtyIsControlling(tty)) {
        return _ENOTTY;
    }
    lock(&side->lock);
    const pid_t_ foreground = side->fg_group;
    unlock(&side->lock);
    if (side == tty && foreground == 0) {
        return _ENOTTY;
    }
    *out = foreground;
    return 0;
}

static int ttySetForeground(struct tty *tty, const dword_t *value) {
    if (!value) {
        return _EINVAL;
    }
    const int sid = pscalTtyCurrentSid();
    if (!pscalTtyIsControlling(tty)) {
        return _ENOTTY;
    }
    const pid_t_ wanted = (pid_t_)*value;
    lock(&tty->lock);
    const pid_t_ session = tty->session;
    const bool allowed = session == (pid_t_)sid;
    if (allowed) {
        tty->fg_group = wanted;
    }
    unlock(&tty->lock);
    if (!allowed) {
        return _ENOTTY;
    }
    pscalTtySetForegroundPgid(session, wanted);
    return 0;
}

static void ttySetTermios(struct tty *side, const struct termios_ *wanted, bool flush_input) {
    lock(&side->lock);
    if (flush_input) {
        ttyQueueClearLocked(side);
    }
    const struct termios_ *now = &side->termios;
    const bool mode_changed = ((now->lflags ^ wanted->lflags) & ICANON_) != 0;
    const bool timing_changed =
            now->cc[VMIN_] != wanted->cc[VMIN_] || now->cc[VTIME_] != wanted->cc[VTIME_];
    side->termios = *wanted;
    if (mode_changed || timing_changed) {
        /* A session's reader is parked in a read of this terminal nearly all the time,
         * and every command changes the mode. A writer may be waiting too: text that
         * would have waited for a raw reader is now a line with no end, which drops. */
        ttyWakeEveryoneLocked(side);
    }
    const bool newly_readable = mode_changed && side->queue.used > 0;
    unlock(&side->lock);
    if (newly_readable) {
        pscal_fd_poll_wakeup(NULL, 0);
    }
}

void tty_set_winsize(struct tty *tty, struct winsize_ size) {
    if (!tty) {
        return;
    }
    lock(&tty->lock);
    tty->winsize = size;
    const pid_t_ session = tty->session;
    pid_t_ target = tty->fg_group;
    unlock(&tty->lock);
    /* A resize can arrive before the session's job control has settled, and a program
     * must still get the signal, so make do with what vproc has recorded, then the
     * session itself. */
    if (target == 0 && session != 0) {
        const int recorded = pscalTtyGetForegroundPgid(session);
        if (recorded > 0) {
            target = (pid_t_)recorded;
            lock(&tty->lock);
            if (tty->fg_group == 0) {
                tty->fg_group = target;
            }
            unlock(&tty->lock);
        } else {
            target = session;
        }
    }
    if (target > 0) {
        (void)pscalTtySendGroupSignal(target, SIGWINCH_);
    }
}

static int ttyFileIoctl(struct pscal_fd *fd, int cmd, void *arg) {
    struct tty *tty = fd->tty;
    if (!tty) {
        return _ENOTTY;
    }
    /* Two requests have the top bit set and arrive negative. */
    const unsigned request = (unsigned)cmd;
    lock(&tty->lock);
    const bool hung_up = tty->hung_up;
    unlock(&tty->lock);
    if (hung_up) {
        return request == TIOCSPGRP_ ? _ENOTTY : _EIO;
    }

    /* A master's modes and size are its slave's, so that the app can configure the
     * terminal from its end. An open of the master keeps the slave there. */
    struct tty *side = ttyIsMaster(tty) ? tty->pty.other : tty;
    const bool master = ttyIsMaster(tty);

    switch (request) {
        case TCGETS_: {
            if (!side) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EINVAL;
            }
            /* Exactly this size: the caller's buffer may be no bigger. */
            lock(&side->lock);
            memcpy(arg, &side->termios, sizeof(struct termios_));
            unlock(&side->lock);
            return 0;
        }
        case TCSETS_:
        case TCSETSW_:
        case TCSETSF_: {
            if (!side) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EINVAL;
            }
            struct termios_ wanted;
            memcpy(&wanted, arg, sizeof(wanted));
            ttySetTermios(side, &wanted, request == TCSETSF_);
            return 0;
        }
        case TIOCGWINSZ_: {
            if (!side) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EINVAL;
            }
            lock(&side->lock);
            memcpy(arg, &side->winsize, sizeof(struct winsize_));
            unlock(&side->lock);
            return 0;
        }
        case TIOCSWINSZ_: {
            if (!side) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EINVAL;
            }
            struct winsize_ wanted;
            memcpy(&wanted, arg, sizeof(wanted));
            tty_set_winsize(side, wanted);
            return 0;
        }
        case TCFLSH_: {
            const intptr_t which = (intptr_t)arg;
            if (which == TCOFLUSH_) {
                /* Output is never held on the writer's side. */
                return 0;
            }
            if (which != TCIFLUSH_ && which != TCIOFLUSH_) {
                return _EINVAL;
            }
            lock(&tty->lock);
            ttyQueueClearLocked(tty);
            unlock(&tty->lock);
            return 0;
        }
        case TIOCSCTTY_:
            return ttySetControllingForCaller(tty, arg != NULL);
        case TIOCGPGRP_:
            return ttyGetForeground(tty, (pid_t_ *)arg);
        case TIOCSPGRP_:
            return ttySetForeground(tty, (const dword_t *)arg);
        case FIONREAD_: {
            if (!arg) {
                return _EINVAL;
            }
            lock(&tty->lock);
            *(dword_t *)arg = tty->queue.used;
            unlock(&tty->lock);
            return 0;
        }
        case TIOCSPTLCK_: {
            if (!master || !side) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EINVAL;
            }
            const bool locked = *(const dword_t *)arg != 0;
            lock(&side->lock);
            side->pty.slave_locked = locked;
            unlock(&side->lock);
            return 0;
        }
        case TIOCGPTN_: {
            if (!master) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EINVAL;
            }
            *(dword_t *)arg = (dword_t)tty->num;
            return 0;
        }
        case TIOCPKT_: {
            if (!master) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EINVAL;
            }
            const bool packet_mode = *(const dword_t *)arg != 0;
            lock(&tty->lock);
            tty->pty.packet_mode = packet_mode;
            unlock(&tty->lock);
            return 0;
        }
        case TIOCGPKT_: {
            if (!master) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EINVAL;
            }
            lock(&tty->lock);
            *(dword_t *)arg = tty->pty.packet_mode ? 1 : 0;
            unlock(&tty->lock);
            return 0;
        }
        default:
            return _ENOTTY;
    }
}

static ssize_t ttyFileIoctlSize(int cmd) {
    switch ((unsigned)cmd) {
        case TCGETS_:
        case TCSETS_:
        case TCSETSW_:
        case TCSETSF_:
            return sizeof(struct termios_);
        case TIOCGWINSZ_:
        case TIOCSWINSZ_:
            return sizeof(struct winsize_);
        case TIOCGPGRP_:
        case TIOCSPGRP_:
        case TIOCSPTLCK_:
        case TIOCGPTN_:
        case TIOCGPTPEER_:
        case TIOCPKT_:
        case TIOCGPKT_:
        case FIONREAD_:
            return 4;
        case TCFLSH_:
        case TIOCSCTTY_:
            return 0;
        default:
            return -1;
    }
}

const struct pscal_fd_ops pscal_tty_fd_ops = {
    .read = ttyFileRead,
    .write = ttyFileWrite,
    .poll = ttyFilePoll,
    .ioctl_size = ttyFileIoctlSize,
    .ioctl = ttyFileIoctl,
    .close = ttyFileClose,
};

/* --- for the session's reader and queue in vproc.c --- */

void pscalTtySetSessionReader(struct pscal_fd *fd) {
    if (!fd || !fd->tty) {
        return;
    }
    lock(&fd->tty->lock);
    fd->tty->session_reader = true;
    unlock(&fd->tty->lock);
}

bool pscalTtyTakeEofKey(struct pscal_fd *fd) {
    if (!fd || !fd->tty) {
        return false;
    }
    struct tty *tty = fd->tty;
    lock(&tty->lock);
    const bool was_key = tty->eof_reads > 0;
    if (was_key) {
        tty->eof_reads--;
    }
    unlock(&tty->lock);
    return was_key;
}

int pscalTtyCheckBackgroundRead(struct pscal_fd *fd) {
    if (!fd || !fd->tty) {
        return 0;
    }
    return ttyCheckBackgroundRead(fd->tty);
}
