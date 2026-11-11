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
    tty->erase_open = false;
    tty->literal_next = false;
    pthread_cond_broadcast(&tty->writable);
}

/* A cell counted from the head. */
static TtyCell *ttyQueueCell(TtyQueue *q, unsigned index) {
    return &q->cells[(q->head + index) % TTY_BUF_SIZE];
}

/* After a change of mode, what's queued has lost its lines. Going to line mode, all of it
 * is one finished line, so that a reader finds it; going to raw it's plain bytes. An
 * end-of-file mark that didn't end up last is a byte 0, as raw reads have always had it. */
static void ttyQueueRelabelLocked(TtyQueue *q, bool to_line_mode) {
    for (unsigned i = 0; i < q->used; i++) {
        TtyCell *cell = ttyQueueCell(q, i);
        const bool last = i + 1 == q->used;
        if (!to_line_mode) {
            if (cell->kind == TTY_CELL_LINE_END) {
                cell->kind = TTY_CELL_BYTE;
            }
        } else if (last) {
            if (cell->kind != TTY_CELL_EOF_MARK) {
                cell->kind = TTY_CELL_LINE_END;
            }
        } else {
            cell->kind = TTY_CELL_BYTE;
        }
    }
    q->released = to_line_mode ? q->used : 0;
}

/* --- life of a terminal --- */

static const struct termios_ kSlaveTermios = {
    .iflags = ICRNL_ | IXON_,
    .oflags = OPOST_ | ONLCR_,
    .cflags = B38400_ | CS8_ | CREAD_,
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

/* How often a wait looks for a signal on its own, in tenths of a second. vproc wakes waits
 * when it queues one (pscalTtyWakeForSignal), so this is only for a wake that was missed. */
#define TTY_SIGNAL_LOOK_TENTHS 5

/* A wait that a signal can end (L10): 0 when woken, ETIMEDOUT once `until` has passed, or
 * _EINTR when a signal has asked the call to give up. The lock is let go to ask, so whatever
 * was looked at before has to be looked at again. */
static int ttyWaitSignalledLocked(struct tty *tty, pthread_cond_t *cond, const struct timespec *until) {
    const struct timespec look = ttyTimeAfterTenths(TTY_SIGNAL_LOOK_TENTHS);
    const bool until_first = until && (until->tv_sec < look.tv_sec ||
                                       (until->tv_sec == look.tv_sec && until->tv_nsec <= look.tv_nsec));
    const int result = ttyWaitLocked(tty, cond, until_first ? until : &look);
    if (result == ETIMEDOUT && until_first) {
        return ETIMEDOUT;
    }
    unlock(&tty->lock);
    const int signalled = pscalTtySignalCheck();
    lock(&tty->lock);
    return signalled < 0 ? signalled : 0;
}

/* --- output processing --- */

#define TTY_TAB_STOP 8u
/* The most one byte can become: a tab, expanded. */
#define TTY_OUTPUT_MAX TTY_TAB_STOP

/* A terminal's idea of a letter is Latin-1's, whatever the bytes are meant to be. So 0xdf
 * and 0xff count as lower case, and go up to what is 0x20 below them like any other. */
static bool ttyByteIsUpper(byte_t byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 0xc0 && byte <= 0xde && byte != 0xd7);
}

static bool ttyByteIsLower(byte_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 0xdf && byte != 0xf7);
}

static bool ttyByteInWord(byte_t byte) {
    return ttyByteIsUpper(byte) || ttyByteIsLower(byte) || (byte >= '0' && byte <= '9') || byte == '_';
}

static bool ttyByteIsControl(byte_t byte) {
    return byte < 0x20 || byte == 0x7f;
}

static bool ttyByteContinues(const struct termios_ *termios, byte_t byte) {
    return (termios->iflags & IUTF8_) && (byte & 0xc0) == 0x80;
}

static unsigned ttyNextTabStop(unsigned column) {
    return column - column % TTY_TAB_STOP + TTY_TAB_STOP;
}

/* What one byte of output becomes, in `out`, which has room for TTY_OUTPUT_MAX. With OPOST
 * clear it stays as it is and the column doesn't follow it. */
static size_t ttyOutputByteLocked(struct tty *tty, byte_t byte, unsigned char *out) {
    const dword_t oflags = tty->termios.oflags;
    if (!(oflags & OPOST_)) {
        out[0] = byte;
        return 1;
    }
    switch (byte) {
        case '\n':
            if (oflags & (ONLRET_ | ONLCR_)) {
                tty->column = 0;
                tty->line_column = 0;
            }
            if (oflags & ONLCR_) {
                out[0] = '\r';
                out[1] = '\n';
                return 2;
            }
            out[0] = '\n';
            return 1;
        case '\r':
            if ((oflags & ONOCR_) && tty->column == 0) {
                return 0;
            }
            if (!(oflags & OCRNL_) || (oflags & ONLRET_)) {
                tty->column = 0;
                tty->line_column = 0;
            }
            out[0] = (oflags & OCRNL_) ? '\n' : '\r';
            return 1;
        case '\t': {
            const unsigned stop = ttyNextTabStop(tty->column);
            const unsigned width = stop - tty->column;
            tty->column = stop;
            if ((oflags & TABDLY_) == TAB3_) {
                memset(out, ' ', width);
                return width;
            }
            out[0] = '\t';
            return 1;
        }
        case '\b':
            if (tty->column > 0) {
                tty->column--;
            }
            out[0] = '\b';
            return 1;
        default:
            if (!ttyByteIsControl(byte)) {
                if ((oflags & OLCUC_) && ttyByteIsLower(byte)) {
                    byte = (byte_t)(byte - 0x20);
                }
                if (!ttyByteContinues(&tty->termios, byte)) {
                    tty->column++;
                }
            }
            out[0] = byte;
            return 1;
    }
}

/* --- input --- */

/* The work of a tty_input call that can't be done under the lock. Each byte is handled
 * under the lock, with what it leads to collected here until the lock is let go. */
typedef struct {
    struct tty *tty;
    bool wake_pollers;
    bool announced; /* readers were told about the queue before this writer's wait */
    /* One per key, in the order typed (L9): indexes into kKeySignals. */
    unsigned signal_count;
    byte_t signals[16];
    byte_t packet_status; /* for the master, if it wants to hear */
    byte_t packet_clear;  /* what those events end */
    size_t echo_len;
    unsigned char echo[512];
} TtyInputRun;

static const int kKeySignals[] = {SIGINT_, SIGQUIT_, SIGTSTP_};

/* A byte of 0 never matches, which is how a cc entry is switched off. */
static bool ttyByteIs(const struct termios_ *termios, byte_t byte, int index) {
    return byte != 0 && termios->cc[index] == byte;
}

/* Tells a master's reader of an event on its slave. The caller holds no terminal's lock. */
static void ttyPacketEvent(struct tty *master, byte_t set, byte_t clear) {
    lock(&master->lock);
    const bool wanted = master->pty.packet_mode;
    if (wanted) {
        master->pty.packet_status = (byte_t)((master->pty.packet_status & ~clear) | set);
        pthread_cond_broadcast(&master->readable);
    }
    unlock(&master->lock);
    if (wanted) {
        pscal_fd_poll_wakeup(NULL, 0);
    }
}

/* Drops the lock to do what has been collected. On return the state of the terminal
 * may be anything, so whatever depends on it has to be looked at again. Signals wait
 * for the end of the call if they can, to go once however many keys asked for them. */
static void ttyRunFlushLocked(TtyInputRun *run, bool with_signals) {
    struct tty *tty = run->tty;
    /* Echo is output, and stopped output keeps it until it starts again. What doesn't fit is
     * lost, as nothing can wait here. */
    if (tty->stopped && run->echo_len > 0) {
        const size_t room = sizeof(tty->held) - tty->held_len;
        const size_t kept = run->echo_len < room ? run->echo_len : room;
        memcpy(tty->held + tty->held_len, run->echo, kept);
        tty->held_len += kept;
        run->echo_len = 0;
    }
    unsigned char released[sizeof(tty->held)];
    size_t released_len = 0;
    if (!tty->stopped && tty->held_len > 0) {
        memcpy(released, tty->held, tty->held_len);
        released_len = tty->held_len;
        tty->held_len = 0;
    }
    const unsigned signal_count = with_signals ? run->signal_count : 0;
    byte_t signals[sizeof(run->signals)];
    memcpy(signals, run->signals, signal_count);
    if (run->echo_len == 0 && released_len == 0 && !run->wake_pollers && signal_count == 0 &&
        run->packet_status == 0 && run->packet_clear == 0) {
        return;
    }
    struct tty *peer = NULL;
    if ((run->echo_len > 0 || released_len > 0 || run->packet_status != 0 || run->packet_clear != 0) &&
        tty->pty.other) {
        peer = tty->pty.other;
        ttyPin(peer);
    }
    const size_t echo_len = run->echo_len;
    const bool wake_pollers = run->wake_pollers;
    const byte_t packet_status = run->packet_status;
    const byte_t packet_clear = run->packet_clear;
    const pid_t_ foreground = tty->fg_group;
    run->echo_len = 0;
    run->wake_pollers = false;
    run->packet_status = 0;
    run->packet_clear = 0;
    if (with_signals) {
        run->signal_count = 0;
    }
    unlock(&tty->lock);

    if (peer) {
        if (packet_status != 0 || packet_clear != 0) {
            ttyPacketEvent(peer, packet_status, packet_clear);
        }
        /* Never waits. If the other side is full the echo is lost, since whoever is
         * typing may be the UI. */
        if (released_len > 0) {
            (void)tty_input(peer, (const char *)released, released_len, false);
        }
        if (echo_len > 0) {
            (void)tty_input(peer, (const char *)run->echo, echo_len, false);
        }
        ttyUnpin(peer);
    }
    if (wake_pollers) {
        pscal_fd_poll_wakeup(NULL, 0);
    }
    /* Each to whatever the foreground is now, as the key would have reached it. */
    if (foreground > 0) {
        for (unsigned i = 0; i < signal_count; i++) {
            (void)pscalTtySendGroupSignal(foreground, kKeySignals[signals[i]]);
        }
    }
    lock(&tty->lock);
}

/* Bytes already as they are to appear. */
static void ttyEchoLocked(TtyInputRun *run, const void *bytes, size_t len) {
    if (run->echo_len + len > sizeof(run->echo)) {
        ttyRunFlushLocked(run, false);
    }
    memcpy(run->echo + run->echo_len, bytes, len);
    run->echo_len += len;
}

/* A byte as output would have it. Echo is output, and flags on the way out apply to it. */
static void ttyEchoOutputLocked(TtyInputRun *run, byte_t byte) {
    unsigned char out[TTY_OUTPUT_MAX];
    const size_t len = ttyOutputByteLocked(run->tty, byte, out);
    ttyEchoLocked(run, out, len);
}

static bool ttyShowsAsControl(const struct termios_ *termios, byte_t byte) {
    /* The flow control keys too: with IXON they never get this far, and without it they're
     * bytes like any other. */
    return (termios->lflags & ECHOCTL_) && ttyByteIsControl(byte) && byte != '\t';
}

/* A byte as it shows when typed. The two columns of ^X count with OPOST clear too. */
static void ttyEchoCharLocked(TtyInputRun *run, byte_t byte) {
    if (ttyShowsAsControl(&run->tty->termios, byte)) {
        const char shown[2] = {'^', (char)(byte ^ 0x40)};
        run->tty->column += 2;
        ttyEchoLocked(run, shown, sizeof(shown));
    } else {
        ttyEchoOutputLocked(run, byte);
    }
}

/* ECHOPRT's closing slash, owed since erasing began. */
static void ttyEchoCloseEraseLocked(TtyInputRun *run) {
    if (run->tty->erase_open) {
        run->tty->erase_open = false;
        ttyEchoOutputLocked(run, '/');
    }
}

static void ttyEchoRubOutLocked(TtyInputRun *run) {
    ttyEchoOutputLocked(run, '\b');
    ttyEchoOutputLocked(run, ' ');
    ttyEchoOutputLocked(run, '\b');
}

/* The width of a tab being rubbed out, which depends on all that came before it on the line,
 * from where the line began. Output that landed in the middle doesn't count. */
static unsigned ttyTabWidthAtEndLocked(struct tty *tty, unsigned end) {
    TtyQueue *q = &tty->queue;
    const struct termios_ *termios = &tty->termios;
    unsigned column = tty->line_column;
    for (unsigned i = q->released; i < end; i++) {
        const byte_t byte = ttyQueueCell(q, i)->value;
        if (byte == '\t') {
            column = ttyNextTabStop(column);
        } else if (ttyByteIsControl(byte)) {
            column += (termios->lflags & ECHOCTL_) ? 2u : 0u;
        } else if (!ttyByteContinues(termios, byte)) {
            column++;
        }
    }
    return ttyNextTabStop(column) - column;
}

/* How many positions the last character of the line being typed takes, with IUTF8 more than
 * one. The caller checks there is one. */
static unsigned ttyLastCharSizeLocked(struct tty *tty, byte_t *first) {
    TtyQueue *q = &tty->queue;
    const unsigned unfinished = q->used - q->released;
    unsigned size = 1;
    byte_t byte = ttyQueueCell(q, q->used - 1)->value;
    while (ttyByteContinues(&tty->termios, byte) && size < unfinished) {
        size++;
        byte = ttyQueueCell(q, q->used - size)->value;
    }
    *first = byte;
    return size;
}

/* Takes the last character off the line being typed, showing that or not. */
static void ttyEraseCharLocked(TtyInputRun *run, bool show) {
    struct tty *tty = run->tty;
    TtyQueue *q = &tty->queue;
    byte_t first = 0;
    const unsigned size = ttyLastCharSizeLocked(tty, &first);
    const unsigned tab_width = first == '\t' ? ttyTabWidthAtEndLocked(tty, q->used - size) : 0;
    /* Only as much of a character as could be one, however many stray continuation bytes
     * there are. It's all out of the queue before anything shows, as showing can let go
     * of the lock. */
    byte_t bytes[8];
    const unsigned shown = size < sizeof(bytes) ? size : (unsigned)sizeof(bytes);
    for (unsigned i = 0; i < shown; i++) {
        bytes[i] = ttyQueueCell(q, q->used - shown + i)->value;
    }
    q->used -= size;
    if (!show) {
        return;
    }
    if (tty->termios.lflags & ECHOPRT_) {
        if (!tty->erase_open) {
            tty->erase_open = true;
            ttyEchoOutputLocked(run, '\\');
        }
        for (unsigned i = 0; i < shown; i++) {
            ttyEchoCharLocked(run, bytes[i]);
        }
    } else if (first == '\t') {
        const char back = '\b';
        for (unsigned i = 0; i < tab_width; i++) {
            if (tty->column > 0) {
                tty->column--;
            }
            ttyEchoLocked(run, &back, 1);
        }
    } else if (ttyByteIsControl(first)) {
        if (tty->termios.lflags & ECHOCTL_) {
            ttyEchoRubOutLocked(run);
            ttyEchoRubOutLocked(run);
        }
    } else {
        ttyEchoRubOutLocked(run);
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
static void ttyOutputStartLocked(TtyInputRun *run);

static void ttySignalKeyLocked(TtyInputRun *run, int signal_index) {
    struct tty *tty = run->tty;
    /* Stopped output starts again, and echo not yet out goes with the input. What is
     * already on its way to the master stays. */
    ttyOutputStartLocked(run);
    if (tty->fg_group == 0) {
        return;
    }
    if (!(tty->termios.lflags & NOFLSH_)) {
        ttyQueueClearLocked(tty);
        run->echo_len = 0;
        tty->held_len = 0;
        run->packet_status |= TIOCPKT_FLUSHREAD_ | TIOCPKT_FLUSHWRITE_;
    }
    if (run->signal_count == sizeof(run->signals)) {
        /* More keys than a run keeps: those before go now, in order. */
        ttyRunFlushLocked(run, true);
    }
    run->signals[run->signal_count++] = (byte_t)signal_index;
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

/* Output stopping and starting, from a key. The rest happens when the run lets go of the lock:
 * held echo goes out then, and a writer waiting for output to start wakes now. */
static void ttyOutputStopLocked(TtyInputRun *run) {
    struct tty *tty = run->tty;
    if (tty->stopped) {
        return;
    }
    tty->stopped = true;
    run->packet_status = (byte_t)((run->packet_status & ~TIOCPKT_START_) | TIOCPKT_STOP_);
    run->packet_clear = (byte_t)((run->packet_clear & ~TIOCPKT_STOP_) | TIOCPKT_START_);
}

static void ttyOutputStartLocked(TtyInputRun *run) {
    struct tty *tty = run->tty;
    if (!tty->stopped) {
        return;
    }
    tty->stopped = false;
    pthread_cond_broadcast(&tty->writable);
    run->wake_pollers = true;
    run->packet_status = (byte_t)((run->packet_status & ~TIOCPKT_STOP_) | TIOCPKT_START_);
    run->packet_clear = (byte_t)((run->packet_clear & ~TIOCPKT_START_) | TIOCPKT_STOP_);
}

/* IXON's keys, in either mode. True for one of them, which is used up and never data. With
 * the two keys the same, it only starts. With IXANY any other key starts output too, and
 * goes on to be what it is. */
static bool ttyFlowKeyLocked(TtyInputRun *run, byte_t byte) {
    struct tty *tty = run->tty;
    const struct termios_ *termios = &tty->termios;
    if (ttyIsMaster(tty) || !(termios->iflags & IXON_)) {
        return false;
    }
    if (ttyByteIs(termios, byte, VSTART_)) {
        ttyOutputStartLocked(run);
        return true;
    }
    if (ttyByteIs(termios, byte, VSTOP_)) {
        ttyOutputStopLocked(run);
        return true;
    }
    if ((termios->iflags & IXANY_) && tty->stopped) {
        ttyOutputStartLocked(run);
    }
    return false;
}

/* What a byte is before anything looks at it, in either mode. */
static byte_t ttyInputMapped(const struct termios_ *termios, byte_t byte) {
    if (termios->iflags & ISTRIP_) {
        byte &= 0x7f;
    }
    if ((termios->iflags & IUCLC_) && (termios->lflags & IEXTEN_) && ttyByteIsUpper(byte)) {
        byte = (byte_t)(byte + 0x20);
    }
    return byte;
}

/* PARMRK has 0xff mean the start of a mark, so a real one comes twice. */
static bool ttyInputDoubles(const struct termios_ *termios, byte_t byte) {
    return (termios->iflags & PARMRK_) && byte == 0xff;
}

/* A byte with no meaning in line mode, or whose meaning has been taken off it. */
static bool ttyInputPlainByteLocked(TtyInputRun *run, byte_t byte) {
    struct tty *tty = run->tty;
    TtyQueue *q = &tty->queue;
    const struct termios_ *termios = &tty->termios;
    const bool line_empty = !ttyQueueHasUnfinished(q);
    /* Nothing can drain a line with no end, so one that would leave no position for its
     * end loses the byte. Waiting here would be for ever, and the writer may be the UI. It
     * still shows: echo goes by what was typed, not by what was kept. */
    bool keep = true;
    if (q->used + 1 >= TTY_BUF_SIZE) {
        if (q->released == 0) {
            keep = false;
        } else if (q->used == TTY_BUF_SIZE) {
            return false;
        }
    }
    if (keep) {
        ttyQueuePush(q, byte, TTY_CELL_BYTE);
        if (ttyInputDoubles(termios, byte) && q->used + 1 < TTY_BUF_SIZE) {
            ttyQueuePush(q, byte, TTY_CELL_BYTE);
        }
    }
    if (termios->lflags & ECHO_) {
        if (line_empty) {
            tty->line_column = tty->column;
        }
        ttyEchoCloseEraseLocked(run);
        ttyEchoCharLocked(run, byte);
    }
    return true;
}

/* The erase, kill and word erase keys. Each path finishes with the terminal's state before
 * it shows anything that could let go of the lock, or looks again after. */
static void ttyInputEraseKeyLocked(TtyInputRun *run, byte_t key) {
    struct tty *tty = run->tty;
    const dword_t lflags = tty->termios.lflags;
    const bool echo = (lflags & ECHO_) != 0;
    if (!ttyQueueHasUnfinished(&tty->queue)) {
        return;
    }
    if (echo && !(lflags & (ECHOE_ | ECHOPRT_))) {
        ttyEraseCharLocked(run, false);
        ttyEchoCharLocked(run, key);
    } else {
        ttyEraseCharLocked(run, echo);
    }
}

static void ttyInputKillKeyLocked(TtyInputRun *run, byte_t key) {
    struct tty *tty = run->tty;
    TtyQueue *q = &tty->queue;
    const dword_t lflags = tty->termios.lflags;
    const bool echo = (lflags & ECHO_) != 0;
    if (!ttyQueueHasUnfinished(q)) {
        return;
    }
    const dword_t rub_out = ECHOE_ | ECHOK_ | ECHOKE_;
    if (echo && (lflags & rub_out) == rub_out) {
        while (ttyQueueHasUnfinished(q)) {
            ttyEraseCharLocked(run, true);
        }
        return;
    }
    q->used = q->released;
    if (echo) {
        ttyEchoCloseEraseLocked(run);
        ttyEchoCharLocked(run, key);
        if (lflags & ECHOK_) {
            ttyEchoOutputLocked(run, '\n');
        }
    }
}

static void ttyInputWordEraseKeyLocked(TtyInputRun *run) {
    struct tty *tty = run->tty;
    TtyQueue *q = &tty->queue;
    const bool echo = (tty->termios.lflags & ECHO_) != 0;
    bool in_word = false;
    while (ttyQueueHasUnfinished(q)) {
        byte_t first = 0;
        (void)ttyLastCharSizeLocked(tty, &first);
        if (ttyByteInWord(first)) {
            in_word = true;
        } else if (in_word) {
            break;
        }
        ttyEraseCharLocked(run, echo);
    }
}

static void ttyInputReprintKeyLocked(TtyInputRun *run, byte_t key) {
    struct tty *tty = run->tty;
    TtyQueue *q = &tty->queue;
    ttyEchoCloseEraseLocked(run);
    ttyEchoCharLocked(run, key);
    ttyEchoOutputLocked(run, '\n');
    for (unsigned i = q->released; i < q->used; i++) {
        ttyEchoCharLocked(run, ttyQueueCell(q, i)->value);
    }
}

/* A byte in line mode. False if it has to wait for room. Each return path has finished
 * with the terminal's state before its last echo, which can drop the lock. */
static bool ttyInputLineByteLocked(TtyInputRun *run, byte_t byte) {
    struct tty *tty = run->tty;
    TtyQueue *q = &tty->queue;
    const struct termios_ *termios = &tty->termios;
    const dword_t lflags = termios->lflags;
    const bool echo = (lflags & ECHO_) != 0;
    const bool extended = (lflags & IEXTEN_) != 0;

    byte = ttyInputMapped(termios, byte);
    if (tty->literal_next) {
        const bool taken = ttyInputPlainByteLocked(run, byte);
        if (taken) {
            tty->literal_next = false;
        }
        return taken;
    }
    if (ttyFlowKeyLocked(run, byte)) {
        return true;
    }
    if (byte == '\r') {
        if (termios->iflags & IGNCR_) {
            return true;
        }
        if (termios->iflags & ICRNL_) {
            byte = '\n';
        }
    } else if (byte == '\n' && (termios->iflags & INLCR_)) {
        byte = '\r';
    }

    if (ttyByteIs(termios, byte, VERASE_)) {
        ttyInputEraseKeyLocked(run, byte);
    } else if (ttyByteIs(termios, byte, VKILL_)) {
        ttyInputKillKeyLocked(run, byte);
    } else if (extended && ttyByteIs(termios, byte, VWERASE_)) {
        ttyInputWordEraseKeyLocked(run);
    } else if (extended && echo && ttyByteIs(termios, byte, VREPRINT_)) {
        ttyInputReprintKeyLocked(run, byte);
        return true;
    } else if (extended && ttyByteIs(termios, byte, VLNEXT_)) {
        tty->literal_next = true;
        if (echo && (lflags & ECHOCTL_)) {
            ttyEchoCloseEraseLocked(run);
            ttyEchoOutputLocked(run, '^');
            ttyEchoOutputLocked(run, '\b');
        }
        return true;
    } else {
        goto not_an_editing_key;
    }
    /* A bracket opened for rubbing out closes as soon as there's nothing left to rub out. */
    if (!ttyQueueHasUnfinished(q)) {
        ttyEchoCloseEraseLocked(run);
    }
    return true;

not_an_editing_key:
    if (ttyByteIs(termios, byte, VEOF_)) {
        if (!ttyMakeRoomForLineEndLocked(tty)) {
            return false;
        }
        ttyQueuePush(q, 0, TTY_CELL_EOF_MARK);
        ttyLineEndedLocked(run);
        return true;
    }
    if (byte == '\n' || ttyByteIs(termios, byte, VEOL_) ||
        (extended && ttyByteIs(termios, byte, VEOL2_))) {
        if (!ttyMakeRoomForLineEndLocked(tty)) {
            return false;
        }
        const bool line_empty = !ttyQueueHasUnfinished(q);
        ttyQueuePush(q, byte, TTY_CELL_LINE_END);
        ttyLineEndedLocked(run);
        if (byte == '\n') {
            if (echo || (lflags & ECHONL_)) {
                ttyEchoOutputLocked(run, '\n');
            }
        } else if (echo) {
            if (line_empty) {
                tty->line_column = tty->column;
            }
            ttyEchoCharLocked(run, byte);
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
            ttyEchoCharLocked(run, byte);
            if (interrupt_key) {
                /* A real system's shell moves to a new line when its child dies from the
                 * signal. This command is still alive, so nothing else would. */
                ttyEchoOutputLocked(run, '\n');
            }
        }
        return true;
    }
    if (lflags & ISIG_) {
        const int signal_index = ttyKeySignalIndex(termios, byte);
        if (signal_index >= 0) {
            ttySignalKeyLocked(run, signal_index);
            if (echo) {
                ttyEchoCharLocked(run, byte);
            }
            return true;
        }
    }

    return ttyInputPlainByteLocked(run, byte);
}

/* A byte for a master, or for a slave out of line mode. A master's input is its slave's
 * output and is taken as it comes. A slave out of line mode has no line editing, but its
 * flow control, signal keys, translation and echo go by their own flags, as in line mode:
 * a program that wants none of them clears them. */
static bool ttyInputRawByteLocked(TtyInputRun *run, byte_t byte) {
    struct tty *tty = run->tty;
    TtyQueue *q = &tty->queue;
    const struct termios_ *termios = &tty->termios;
    const bool slave = !ttyIsMaster(tty);
    bool from_return = false;
    if (slave) {
        byte = ttyInputMapped(termios, byte);
        if (ttyFlowKeyLocked(run, byte)) {
            return true;
        }
        if (termios->lflags & ISIG_) {
            const int signal_index = ttyKeySignalIndex(termios, byte);
            if (signal_index >= 0) {
                ttySignalKeyLocked(run, signal_index);
                if (termios->lflags & ECHO_) {
                    ttyEchoCharLocked(run, byte);
                }
                return true;
            }
        }
        if (byte == '\r') {
            if (termios->iflags & IGNCR_) {
                return true;
            }
            if (termios->iflags & ICRNL_) {
                byte = '\n';
                from_return = true;
            }
        } else if (byte == '\n' && (termios->iflags & INLCR_)) {
            byte = '\r';
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
    if (slave && ttyInputDoubles(termios, byte) && q->used < TTY_BUF_SIZE) {
        ttyQueuePush(q, byte, TTY_CELL_BYTE);
    }
    if (slave && (termios->lflags & ECHO_)) {
        /* Out of line mode a newline is no line's end, and shows as ^J like other control
         * bytes. One that Return became shows as the new line it was typed as. */
        if (from_return) {
            ttyEchoOutputLocked(run, '\n');
        } else {
            ttyEchoCharLocked(run, byte);
        }
    }
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
    run.signal_count = 0;
    run.packet_status = 0;
    run.packet_clear = 0;
    run.echo_len = 0;

    size_t consumed = 0;
    bool interrupted = false;
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
        if (run.echo_len > 0 || run.wake_pollers || run.signal_count != 0 || run.packet_status != 0 ||
            run.packet_clear != 0) {
            ttyRunFlushLocked(&run, true);
            continue;
        }
        const int waited = ttyWaitSignalledLocked(tty, &tty->writable, NULL);
        run.announced = false;
        if (waited < 0) {
            interrupted = true;
            break;
        }
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
    if (interrupted) {
        return _EINTR;
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
 * mode counts as plain bytes, and an end-of-file mark as the byte 0. */
static ssize_t ttyCopyRawLocked(struct tty *tty, unsigned char *out, size_t size) {
    TtyQueue *q = &tty->queue;
    size_t copied = 0;
    while (copied < size && q->used > 0) {
        out[copied++] = ttyQueuePop(q).value;
    }
    if (copied > 0) {
        pthread_cond_broadcast(&tty->writable);
    }
    return (ssize_t)copied;
}

/* Job control for a read or a write of the terminal. A process outside the foreground group
 * of its own controlling terminal is sent the signal and goes no further: when it is
 * continued, or a handler with SA_RESTART returns, it comes back to look again. A signal
 * nobody would act on doesn't go. No lock is held for the hooks, since they take the task
 * table's. */
static int ttyJobControlCheck(struct tty *tty, int sig) {
    if (pscalTtyCallerIsKernel()) {
        return 0;
    }
    while (pscalTtyIsControlling(tty)) {
        lock(&tty->lock);
        const pid_t_ foreground = tty->fg_group;
        unlock(&tty->lock);
        const int group = pscalTtyCurrentPgid();
        if (foreground == 0 || group == foreground) {
            return 0;
        }
        const bool refused = pscalTtySignalIgnoredOrBlocked(sig);
        if (sig == SIGTTOU_ && refused) {
            return 0;
        }
        if (refused || pscalTtyCurrentGroupOrphaned()) {
            return _EIO;
        }
        if (pscalTtySendGroupSignal(group, sig) != 0) {
            return _EIO;
        }
        const int after = pscalTtyAfterJobSignal(sig);
        if (after != 0) {
            return after;
        }
    }
    return 0;
}

static bool ttyFileIsNonBlocking(struct pscal_fd *fd) {
    lock(&fd->lock);
    const bool nonblocking = (fd->flags & O_NONBLOCK) != 0;
    unlock(&fd->lock);
    return nonblocking;
}

/* A master's read: an event in packet mode comes first, as one byte, and data has a byte
 * of 0 in front of it. */
static ssize_t ttyReadMasterLocked(struct tty *tty, unsigned char *out, size_t size, bool nonblocking) {
    TtyQueue *q = &tty->queue;
    for (;;) {
        if (tty->hung_up) {
            return 0;
        }
        const bool packet = tty->pty.packet_mode;
        if (packet && tty->pty.packet_status != 0) {
            out[0] = tty->pty.packet_status;
            tty->pty.packet_status = 0;
            return 1;
        }
        if (q->used > 0) {
            if (!packet) {
                return ttyCopyRawLocked(tty, out, size);
            }
            out[0] = 0;
            return 1 + (size > 1 ? ttyCopyRawLocked(tty, out + 1, size - 1) : 0);
        }
        /* What the slave wrote before its last close has been read. Until it's opened
         * again there's nothing behind this end. */
        if (tty->pty.peer_hung_up) {
            return _EIO;
        }
        if (nonblocking) {
            return _EAGAIN;
        }
        const int waited = ttyWaitSignalledLocked(tty, &tty->readable, NULL);
        if (waited < 0) {
            return waited;
        }
    }
}

/* A slave's read. Every wake starts over from the current mode, because a session's reader
 * sits in here while the shell switches modes at every command. */
static ssize_t ttyReadSlaveLocked(struct tty *tty, unsigned char *out, size_t size, bool nonblocking) {
    TtyQueue *q = &tty->queue;
    struct timespec until;
    bool timing = false;
    unsigned timed_from = 0;
    for (;;) {
        if (ttyIsDownLocked(tty)) {
            return 0;
        }
        if (ttyLineModeLocked(tty)) {
            if (q->released > 0) {
                return ttyCopyLineLocked(tty, out, size);
            }
            if (nonblocking) {
                return _EAGAIN;
            }
            const int waited = ttyWaitSignalledLocked(tty, &tty->readable, NULL);
            if (waited < 0) {
                return waited;
            }
            continue;
        }
        const unsigned minimum = tty->termios.cc[VMIN_];
        const unsigned tenths = tty->termios.cc[VTIME_];
        if (minimum == 0) {
            /* VTIME's timer runs from the start of the read, and a byte ends it. */
            if (q->used > 0) {
                return ttyCopyRawLocked(tty, out, size);
            }
            if (tenths == 0) {
                return 0;
            }
            if (nonblocking) {
                return _EAGAIN;
            }
            if (!timing) {
                timing = true;
                until = ttyTimeAfterTenths(tenths);
            }
            const int waited = ttyWaitSignalledLocked(tty, &tty->readable, &until);
            if (waited < 0) {
                return waited;
            }
            if (waited == ETIMEDOUT) {
                return ttyCopyRawLocked(tty, out, size);
            }
            continue;
        }
        /* A read asking for fewer than VMIN bytes has what it asked for at that many. */
        const size_t enough = minimum < size ? minimum : size;
        if (q->used >= enough) {
            return ttyCopyRawLocked(tty, out, size);
        }
        if (nonblocking) {
            return q->used > 0 ? ttyCopyRawLocked(tty, out, size) : _EAGAIN;
        }
        if (q->used > 0 && tenths > 0) {
            /* VTIME's timer is between bytes here: it starts with the first, and again with
             * each that comes. */
            if (!timing || q->used != timed_from) {
                timing = true;
                timed_from = q->used;
                until = ttyTimeAfterTenths(tenths);
            }
            const int waited = ttyWaitSignalledLocked(tty, &tty->readable, &until);
            if (waited < 0) {
                return waited;
            }
            if (waited == ETIMEDOUT) {
                return ttyCopyRawLocked(tty, out, size);
            }
        } else {
            timing = false;
            const int waited = ttyWaitSignalledLocked(tty, &tty->readable, NULL);
            if (waited < 0) {
                return waited;
            }
        }
    }
}

static ssize_t ttyFileRead(struct pscal_fd *fd, void *buf, size_t bufsize) {
    struct tty *tty = fd->tty;
    if (!tty) {
        return bufsize == 0 ? 0 : _EBADF;
    }
    const bool master = ttyIsMaster(tty);
    lock(&tty->lock);
    const bool down = master ? tty->hung_up : ttyIsDownLocked(tty);
    unlock(&tty->lock);
    if (down) {
        return 0;
    }
    if (!master) {
        /* Asking for nothing is still asking. */
        const int err = ttyJobControlCheck(tty, SIGTTIN_);
        if (err < 0) {
            return err;
        }
    }
    if (bufsize == 0) {
        return 0;
    }
    const bool nonblocking = ttyFileIsNonBlocking(fd);
    lock(&tty->lock);
    const ssize_t result = master ? ttyReadMasterLocked(tty, (unsigned char *)buf, bufsize, nonblocking)
                                  : ttyReadSlaveLocked(tty, (unsigned char *)buf, bufsize, nonblocking);
    /* A writer polling the other end waits for room, which this may have made. */
    const bool had_been_full = result > 0 && tty->queue.used + (size_t)result >= TTY_BUF_SIZE;
    unlock(&tty->lock);
    if (had_been_full) {
        pscal_fd_poll_wakeup(NULL, 0);
    }
    return result;
}

/* --- write --- */

/* Output processing, a bufferful at a time so that nothing needs freeing if a `kill`
 * finds this thread waiting for room on the other side. */
#define TTY_WRITE_CHUNK 1024

/* The count of what was asked for that went, or the error if none did. */
static ssize_t ttyWriteResult(size_t written, ssize_t err) {
    return written > 0 ? (ssize_t)written : err;
}

/* Returns how many of `len` went, which a non-blocking write can leave short: what was asked
 * for, never what it was expanded to. A byte whose expansion only partly fitted counts as
 * gone, as its first part has. */
static ssize_t ttyWriteThrough(struct tty *tty,
                               struct tty *peer,
                               const unsigned char *in,
                               size_t len,
                               bool blocking) {
    char chunk[TTY_WRITE_CHUNK];
    /* Where each input byte's output ends in the chunk. */
    unsigned short ends[TTY_WRITE_CHUNK];
    size_t offset = 0;
    while (offset < len) {
        size_t out_len = 0;
        size_t count = 0;
        const size_t start = offset;
        lock(&tty->lock);
        /* Stopped output: a writer waits for it to start, or goes no further. */
        while (tty->stopped && !tty->hung_up) {
            if (!blocking) {
                unlock(&tty->lock);
                return ttyWriteResult(start, _EAGAIN);
            }
            const int waited = ttyWaitSignalledLocked(tty, &tty->writable, NULL);
            if (waited < 0) {
                unlock(&tty->lock);
                return ttyWriteResult(start, waited);
            }
        }
        if (tty->hung_up) {
            unlock(&tty->lock);
            return ttyWriteResult(start, _EIO);
        }
        while (offset < len && out_len + TTY_OUTPUT_MAX <= sizeof(chunk)) {
            out_len += ttyOutputByteLocked(tty, in[offset++], (unsigned char *)chunk + out_len);
            ends[count++] = (unsigned short)out_len;
        }
        unlock(&tty->lock);
        if (out_len == 0) {
            continue;
        }
        const ssize_t taken = tty_input(peer, chunk, out_len, blocking);
        if (taken < 0) {
            return ttyWriteResult(start, taken);
        }
        /* Only a queue that stopped a non-blocking write leaves some behind. */
        if ((size_t)taken < out_len) {
            size_t gone = 0;
            while (gone < count && (gone == 0 ? 0u : ends[gone - 1]) < (size_t)taken) {
                gone++;
            }
            return ttyWriteResult(start + gone, _EAGAIN);
        }
    }
    return (ssize_t)len;
}

static ssize_t ttyFileWrite(struct pscal_fd *fd, const void *buf, size_t bufsize) {
    struct tty *tty = fd->tty;
    if (!tty) {
        return _EBADF;
    }
    const bool blocking = !ttyFileIsNonBlocking(fd);
    lock(&tty->lock);
    /* A master can still write with its slave closed: the keys wait there for its next open. */
    const bool down = tty->hung_up || !tty->pty.other;
    const bool stoppable = !ttyIsMaster(tty) && (tty->termios.lflags & TOSTOP_);
    unlock(&tty->lock);
    if (down) {
        return _EIO;
    }
    if (stoppable) {
        const int err = ttyJobControlCheck(tty, SIGTTOU_);
        if (err < 0) {
            return err;
        }
    }
    /* A stop can have been a long one. */
    lock(&tty->lock);
    struct tty *peer = tty->hung_up ? NULL : tty->pty.other;
    if (peer) {
        ttyPin(peer);
    }
    unlock(&tty->lock);
    if (!peer) {
        return _EIO;
    }

    ssize_t result = 0;
    pthread_cleanup_push(ttyUnpinCancelled, peer);
    result = ttyWriteThrough(tty, peer, (const unsigned char *)buf, bufsize, blocking);
    pthread_cleanup_pop(1);
    return result;
}

/* --- poll --- */

/* Is there room in a master's slave for a key? Asked with no lock held. */
static bool ttyPeerHasRoom(struct tty *master) {
    lock(&master->lock);
    struct tty *slave = master->pty.other;
    if (slave) {
        ttyPin(slave);
    }
    unlock(&master->lock);
    if (!slave) {
        return true;
    }
    lock(&slave->lock);
    const bool room = slave->queue.used < TTY_BUF_SIZE;
    unlock(&slave->lock);
    ttyUnpin(slave);
    return room;
}

static int ttyFilePoll(struct pscal_fd *fd) {
    struct tty *tty = fd->tty;
    if (!tty) {
        return POLL_NVAL;
    }
    /* A slave says it can be written whenever output isn't stopped, as Linux has it; a write
     * to one that is full takes what fits. A master says so only with room for a key. */
    const bool master = ttyIsMaster(tty);
    const bool room = !master || ttyPeerHasRoom(tty);
    int events = 0;
    lock(&tty->lock);
    if (!tty->stopped && room) {
        events |= POLL_WRITE;
    }
    if (tty->hung_up) {
        events |= POLL_WRITE | POLL_READ | POLL_ERR | POLL_HUP;
    } else if (master) {
        if (tty->pty.packet_mode && tty->pty.packet_status != 0) {
            events |= POLL_READ | POLL_PRI;
        } else if (tty->queue.used > 0) {
            events |= POLL_READ;
        }
        if (tty->pty.peer_hung_up) {
            events |= POLL_HUP;
        }
    } else if (tty->pty.peer_hung_up) {
        events |= POLL_READ | POLL_HUP;
    } else if (ttyLineModeLocked(tty)) {
        /* The unfinished part of a line must not count: callers poll before they read,
         * and would spin on a read that has to wait. */
        if (tty->queue.released > 0) {
            events |= POLL_READ;
        }
    } else {
        /* With no timer, what would make a read return is VMIN bytes. */
        const unsigned minimum = tty->termios.cc[VMIN_];
        const unsigned enough = (tty->termios.cc[VTIME_] == 0 && minimum > 0) ? minimum : 1;
        if (tty->queue.used >= enough) {
            events |= POLL_READ;
        }
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
    if (sid <= 0 || pscalTtyCallerGaveUpTerminal()) {
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

void pscalTtySetSessionForeground(pid_t_ sid, pid_t_ pgid) {
    if (sid <= 0) {
        return;
    }
    struct tty *tty = NULL;
    lock(&gSessionsLock);
    const TtySessionEntry *entry = ttySessionFindLocked(sid);
    if (entry) {
        tty = entry->tty;
        ttyRetain(tty);
    }
    unlock(&gSessionsLock);
    if (!tty) {
        return;
    }
    lock(&tty->lock);
    if (tty->session == sid) {
        tty->fg_group = pgid;
    }
    unlock(&tty->lock);
    ttyRelease(tty);
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
    if (sid <= 0 || pscalTtyCallerGaveUpTerminal()) {
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
    lock(&tty->lock);
    const bool exclusive = tty->exclusive;
    unlock(&tty->lock);
    if (exclusive) {
        ttyRelease(tty);
        return _EBUSY;
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
    struct tty *master = tty->driver == &pty_slave ? tty->pty.other : NULL;
    if (master) {
        ttyPin(master);
    }
    unlock(&tty->lock);
    /* A slave opened again is a terminal behind its master once more. */
    if (master) {
        lock(&master->lock);
        master->pty.peer_hung_up = false;
        unlock(&master->lock);
        ttyUnpin(master);
    }
    /* A master is the terminal's other end, and never anyone's controlling terminal. */
    if (!(flags & O_NOCTTY) && !ttyIsMaster(tty)) {
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

/* TIOCSCTTY: the arg is the value itself, not a pointer. Taking a terminal from another
 * session leaves that session with none. */
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
    struct tty *taken = NULL;
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
        if (owner != 0 && owner != caller) {
            TtySessionEntry *other = ttySessionFindLocked(owner);
            if (other && other->tty == tty) {
                taken = ttySessionRemoveLocked(other);
            }
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
    if (taken) {
        ttyRelease(taken);
    }
    if (claimed) {
        pscalTtySetForegroundPgid(sid, foreground);
    }
    return result;
}

/* Through a slave these are for the process it controls. A master just answers. */
static int ttyGetForeground(struct tty *tty, struct tty *side, int *out) {
    if (!side) {
        return _ENOTTY;
    }
    if (side == tty && !pscalTtyIsControlling(tty)) {
        return _ENOTTY;
    }
    if (!out) {
        return _EFAULT;
    }
    lock(&side->lock);
    *out = (int)side->fg_group;
    unlock(&side->lock);
    return 0;
}

static int ttyGetSession(struct tty *tty, struct tty *side, int *out) {
    if (!side) {
        return _ENOTTY;
    }
    if (side == tty && !pscalTtyIsControlling(tty)) {
        return _ENOTTY;
    }
    lock(&side->lock);
    const pid_t_ session = side->session;
    unlock(&side->lock);
    if (session == 0) {
        return _ENOTTY;
    }
    if (!out) {
        return _EFAULT;
    }
    *out = (int)session;
    return 0;
}

static int ttySetForeground(struct tty *side, const dword_t *value) {
    if (!side || !pscalTtyIsControlling(side)) {
        return _ENOTTY;
    }
    const int sid = pscalTtyCurrentSid();
    lock(&side->lock);
    const pid_t_ session = side->session;
    unlock(&side->lock);
    if (session != (pid_t_)sid) {
        return _ENOTTY;
    }
    if (!value) {
        return _EFAULT;
    }
    const pid_t_ wanted = (pid_t_)*value;
    if (wanted < 0) {
        return _EINVAL;
    }
    const int err = wanted == 0 ? _ESRCH : pscalTtyCheckGroup(sid, wanted);
    if (err < 0) {
        return err;
    }
    lock(&side->lock);
    side->fg_group = wanted;
    unlock(&side->lock);
    pscalTtySetForegroundPgid(session, wanted);
    return 0;
}

/* The session's terminal is given up for good by its leader, and for itself by anyone
 * else. What else a leader's TIOCNOTTY does, to the foreground group, is yet to come. */
static int ttyGiveUp(struct tty *tty) {
    if (ttyIsMaster(tty) || !pscalTtyIsControlling(tty)) {
        return _ENOTTY;
    }
    if (pscalTtyIsSessionLeader()) {
        pscalTtyClearControlling(tty);
    } else {
        pscalTtyCallerGiveUpTerminal();
    }
    return 0;
}

/* Whether a master's reader would be told of stopping and starting output: IXON, and the
 * keys it's always been. */
static bool ttyFlowIsStandard(const struct termios_ *termios) {
    return (termios->iflags & IXON_) && termios->cc[VSTART_] == 0x11 && termios->cc[VSTOP_] == 0x13;
}

/* For a slave's master, from a call holding no lock. */
static void ttyTellMaster(struct tty *slave, byte_t set, byte_t clear) {
    lock(&slave->lock);
    struct tty *master = ttyIsMaster(slave) ? NULL : slave->pty.other;
    if (master) {
        ttyPin(master);
    }
    unlock(&slave->lock);
    if (master) {
        ttyPacketEvent(master, set, clear);
        ttyUnpin(master);
    }
}

/* Stops or starts this side's output, from a call holding no lock: TCXONC, or IXON going.
 * Starting sends what echo was held, wakes writers, and tells a packet-mode master. */
static void ttySetOutputStopped(struct tty *tty, bool stop) {
    unsigned char released[sizeof(tty->held)];
    size_t released_len = 0;
    lock(&tty->lock);
    const bool changed = tty->stopped != stop;
    tty->stopped = stop;
    if (!stop) {
        memcpy(released, tty->held, tty->held_len);
        released_len = tty->held_len;
        tty->held_len = 0;
        if (changed) {
            pthread_cond_broadcast(&tty->writable);
        }
    }
    struct tty *peer = released_len > 0 ? tty->pty.other : NULL;
    if (peer) {
        ttyPin(peer);
    }
    unlock(&tty->lock);
    if (peer) {
        (void)tty_input(peer, (const char *)released, released_len, false);
        ttyUnpin(peer);
    }
    if (changed && !ttyIsMaster(tty)) {
        ttyTellMaster(tty, stop ? TIOCPKT_STOP_ : TIOCPKT_START_, stop ? TIOCPKT_START_ : TIOCPKT_STOP_);
    }
    if (changed && !stop) {
        pscal_fd_poll_wakeup(NULL, 0);
    }
}

/* TCIOFF and TCION: a flow control key goes out as this side's output, stopped or not. */
static void ttySendFlowKey(struct tty *tty, int index) {
    lock(&tty->lock);
    struct tty *side = ttyIsMaster(tty) ? tty->pty.other : tty;
    struct tty *peer = tty->pty.other;
    byte_t key = 0;
    if (side == tty) {
        key = tty->termios.cc[index];
    }
    if (peer) {
        ttyPin(peer);
    }
    unlock(&tty->lock);
    if (side && side != tty) {
        lock(&side->lock);
        key = side->termios.cc[index];
        unlock(&side->lock);
    }
    if (peer) {
        if (key != 0) {
            (void)tty_input(peer, (const char *)&key, 1, false);
        }
        ttyUnpin(peer);
    }
}

static void ttySetTermios(struct tty *side, const struct termios_ *wanted_in, bool flush_input) {
    struct termios_ wanted = *wanted_in;
    /* There's no line for these to mean anything on. */
    wanted.cflags = (wanted.cflags & ~(CSIZE_ | PARENB_ | 0x20000000u)) | CS8_ | CREAD_;
    lock(&side->lock);
    if (flush_input) {
        ttyQueueClearLocked(side);
    }
    const struct termios_ *now = &side->termios;
    const bool mode_changed = ((now->lflags ^ wanted.lflags) & ICANON_) != 0;
    const bool timing_changed =
            now->cc[VMIN_] != wanted.cc[VMIN_] || now->cc[VTIME_] != wanted.cc[VTIME_];
    const bool flow_was = ttyFlowIsStandard(now);
    const bool flow_is = ttyFlowIsStandard(&wanted);
    side->termios = wanted;
    if (mode_changed) {
        ttyQueueRelabelLocked(&side->queue, (wanted.lflags & ICANON_) != 0);
        side->erase_open = false;
        side->literal_next = false;
    }
    if (mode_changed || timing_changed) {
        /* A session's reader is parked in a read of this terminal nearly all the time,
         * and every command changes the mode. A writer may be waiting too: text that
         * would have waited for a raw reader is now a line with no end, which drops. */
        ttyWakeEveryoneLocked(side);
    }
    const bool newly_readable = mode_changed && side->queue.used > 0;
    /* Without IXON there's nothing to start stopped output again. */
    const bool restart = side->stopped && !(wanted.iflags & IXON_);
    unlock(&side->lock);
    if (newly_readable) {
        pscal_fd_poll_wakeup(NULL, 0);
    }
    if (restart) {
        ttySetOutputStopped(side, false);
    }
    byte_t status = flush_input ? TIOCPKT_FLUSHREAD_ : 0;
    byte_t clear = 0;
    if (flow_was != flow_is) {
        status |= flow_is ? TIOCPKT_DOSTOP_ : TIOCPKT_NOSTOP_;
        clear = flow_is ? TIOCPKT_NOSTOP_ : TIOCPKT_DOSTOP_;
    }
    if (status != 0) {
        ttyTellMaster(side, status, clear);
    }
}

void tty_set_winsize(struct tty *tty, struct winsize_ size) {
    if (!tty) {
        return;
    }
    lock(&tty->lock);
    const bool changed = memcmp(&tty->winsize, &size, sizeof(size)) != 0;
    tty->winsize = size;
    const pid_t_ target = tty->fg_group;
    unlock(&tty->lock);
    /* With no foreground group nobody is told: a program reads the size when it starts. */
    if (changed && target > 0) {
        (void)pscalTtySendGroupSignal(target, SIGWINCH_);
    }
}

static int ttyFileIoctl(struct pscal_fd *fd, int cmd, void *arg) {
    struct tty *tty = fd->tty;
    if (!tty) {
        return _ENOTTY;
    }
    /* Some requests have the top bit set and arrive negative. */
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

    /* Changing the terminal from the background stops the group, as writing does with
     * TOSTOP, but whatever TOSTOP says. A shell ignores SIGTTOU around its own changes. */
    if (!master) {
        switch (request) {
            case TCSETS_:
            case TCSETSW_:
            case TCSETSF_:
            case TCFLSH_:
            case TCXONC_:
            case TIOCSPGRP_:
            case TCSBRK_:
            case TCSBRKP_: {
                const int err = ttyJobControlCheck(tty, SIGTTOU_);
                if (err < 0) {
                    return err;
                }
                break;
            }
            default:
                break;
        }
    }

    switch (request) {
        case TCGETS_: {
            if (!side) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EFAULT;
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
                return _EFAULT;
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
                return _EFAULT;
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
                return _EFAULT;
            }
            struct winsize_ wanted;
            memcpy(&wanted, arg, sizeof(wanted));
            tty_set_winsize(side, wanted);
            return 0;
        }
        case TCFLSH_: {
            const intptr_t which = (intptr_t)arg;
            if (which != TCIFLUSH_ && which != TCOFLUSH_ && which != TCIOFLUSH_) {
                return _EINVAL;
            }
            /* Output is never held on the writer's side, but for echo kept while output is
             * stopped. */
            lock(&tty->lock);
            if (which != TCOFLUSH_) {
                ttyQueueClearLocked(tty);
            }
            if (which != TCIFLUSH_) {
                tty->held_len = 0;
            }
            unlock(&tty->lock);
            if (!master) {
                byte_t status = 0;
                status |= which != TCOFLUSH_ ? TIOCPKT_FLUSHREAD_ : 0;
                status |= which != TCIFLUSH_ ? TIOCPKT_FLUSHWRITE_ : 0;
                ttyTellMaster(tty, status, 0);
            }
            return 0;
        }
        case TIOCSCTTY_:
            return side ? ttySetControllingForCaller(side, arg != NULL) : _ENOTTY;
        case TIOCGPGRP_:
            return ttyGetForeground(tty, side, (int *)arg);
        case TIOCSPGRP_:
            return ttySetForeground(side, (const dword_t *)arg);
        case TIOCGSID_:
            return ttyGetSession(tty, side, (int *)arg);
        case TIOCNOTTY_:
            return ttyGiveUp(tty);
        case TIOCEXCL_:
        case TIOCNXCL_:
            lock(&tty->lock);
            tty->exclusive = request == TIOCEXCL_;
            unlock(&tty->lock);
            return 0;
        case TIOCGEXCL_: {
            if (!arg) {
                return _EFAULT;
            }
            lock(&tty->lock);
            *(dword_t *)arg = tty->exclusive ? 1 : 0;
            unlock(&tty->lock);
            return 0;
        }
        case TIOCOUTQ_: {
            /* Nothing is ever waiting on the way out: it's in the other side's queue. */
            if (!arg) {
                return _EFAULT;
            }
            *(dword_t *)arg = 0;
            return 0;
        }
        case TCSBRK_:
        case TCSBRKP_:
            /* There's no line to send a break on, and nothing to wait for. */
            return 0;
        case TCXONC_:
            switch ((intptr_t)arg) {
                case TCOOFF_:
                    ttySetOutputStopped(tty, true);
                    return 0;
                case TCOON_:
                    ttySetOutputStopped(tty, false);
                    return 0;
                case TCIOFF_:
                    ttySendFlowKey(tty, VSTOP_);
                    return 0;
                case TCION_:
                    ttySendFlowKey(tty, VSTART_);
                    return 0;
                default:
                    return _EINVAL;
            }
        case TIOCSTI_:
            /* Refused on a Linux install left as it comes, unless to a privileged process,
             * and nothing here is one yet. */
            return _EIO;
        case FIONREAD_: {
            if (!arg) {
                return _EFAULT;
            }
            lock(&tty->lock);
            if (ttyLineModeLocked(tty)) {
                /* What a read could return: not the line still being typed, and an
                 * end-of-file mark is no byte. */
                dword_t count = 0;
                for (unsigned i = 0; i < tty->queue.released; i++) {
                    if (ttyQueueCell(&tty->queue, i)->kind != TTY_CELL_EOF_MARK) {
                        count++;
                    }
                }
                *(dword_t *)arg = count;
            } else {
                *(dword_t *)arg = tty->queue.used;
            }
            unlock(&tty->lock);
            return 0;
        }
        case TIOCSPTLCK_: {
            if (!master || !side) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EFAULT;
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
                return _EFAULT;
            }
            *(dword_t *)arg = (dword_t)tty->num;
            return 0;
        }
        case TIOCPKT_: {
            if (!master) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EFAULT;
            }
            const bool packet_mode = *(const dword_t *)arg != 0;
            lock(&tty->lock);
            if (packet_mode && !tty->pty.packet_mode) {
                tty->pty.packet_status = 0;
            }
            tty->pty.packet_mode = packet_mode;
            unlock(&tty->lock);
            return 0;
        }
        case TIOCGPKT_: {
            if (!master) {
                return _ENOTTY;
            }
            if (!arg) {
                return _EFAULT;
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
        case TIOCGSID_:
        case TIOCGEXCL_:
        case TIOCOUTQ_:
        case TIOCSPTLCK_:
        case TIOCGPTN_:
        case TIOCGPTPEER_:
        case TIOCPKT_:
        case TIOCGPKT_:
        case FIONREAD_:
            return 4;
        case TIOCSTI_:
            return 1;
        case TCXONC_:
        case TCFLSH_:
        case TIOCSCTTY_:
        case TIOCNOTTY_:
        case TIOCEXCL_:
        case TIOCNXCL_:
        case TCSBRK_:
        case TCSBRKP_:
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
    return ttyJobControlCheck(fd->tty, SIGTTIN_);
}
