// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// One side of a terminal: its modes, its input queue, its session, and the operations
// of an open of it. Numbers, struct layouts and request codes are Linux's and form an
// ABI, as applets issue these requests themselves and hand in their own copy of the
// structs. The translation from the host's numbers happens in vprocIoctlShim.
#pragma once

#include <sys/types.h>

#include "runtime/vproc/tty/pscal_compat.h"
#include "runtime/vproc/tty/pscal_fd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TTY_CONSOLE_MAJOR 4
#define TTY_ALTERNATE_MAJOR 5
#define TTY_PSEUDO_MASTER_MAJOR 128
#define TTY_PSEUDO_SLAVE_MAJOR 136
#define DEV_TTY_MINOR 0
#define DEV_CONSOLE_MINOR 1
#define DEV_PTMX_MINOR 2

/* Positions in a side's input queue. An end-of-file mark takes one. */
#define TTY_BUF_SIZE 4096

/* Indexes into termios.cc */
#define VINTR_ 0
#define VQUIT_ 1
#define VERASE_ 2
#define VKILL_ 3
#define VEOF_ 4
#define VTIME_ 5
#define VMIN_ 6
#define VSWTC_ 7
#define VSTART_ 8
#define VSTOP_ 9
#define VSUSP_ 10
#define VEOL_ 11
#define VREPRINT_ 12
#define VDISCARD_ 13
#define VWERASE_ 14
#define VLNEXT_ 15
#define VEOL2_ 16

#define ISIG_ (1u << 0)
#define ICANON_ (1u << 1)
#define XCASE_ (1u << 2)
#define ECHO_ (1u << 3)
#define ECHOE_ (1u << 4)
#define ECHOK_ (1u << 5)
#define ECHONL_ (1u << 6)
#define NOFLSH_ (1u << 7)
#define TOSTOP_ (1u << 8)
#define ECHOCTL_ (1u << 9)
#define ECHOPRT_ (1u << 10)
#define ECHOKE_ (1u << 11)
#define FLUSHO_ (1u << 12)
#define PENDIN_ (1u << 14)
#define IEXTEN_ (1u << 15)

#define IGNBRK_ (1u << 0)
#define BRKINT_ (1u << 1)
#define IGNPAR_ (1u << 2)
#define PARMRK_ (1u << 3)
#define INPCK_ (1u << 4)
#define ISTRIP_ (1u << 5)
#define INLCR_ (1u << 6)
#define IGNCR_ (1u << 7)
#define ICRNL_ (1u << 8)
#define IUCLC_ (1u << 9)
#define IXON_ (1u << 10)
#define IXANY_ (1u << 11)
#define IXOFF_ (1u << 12)
#define IMAXBEL_ (1u << 13)
#define IUTF8_ (1u << 14)

#define OPOST_ (1u << 0)
#define OLCUC_ (1u << 1)
#define ONLCR_ (1u << 2)
#define OCRNL_ (1u << 3)
#define ONOCR_ (1u << 4)
#define ONLRET_ (1u << 5)
#define OFILL_ (1u << 6)
#define OFDEL_ (1u << 7)
#define TABDLY_ (3u << 11)
#define TAB1_ (1u << 11)
#define TAB2_ (2u << 11)
#define TAB3_ (3u << 11)

#define CBAUD_ 0x100fu
#define B38400_ 0xfu
#define CSIZE_ (3u << 4)
#define CS5_ (0u << 4)
#define CS6_ (1u << 4)
#define CS7_ (2u << 4)
#define CS8_ (3u << 4)
#define CSTOPB_ (1u << 6)
#define CREAD_ (1u << 7)
#define PARENB_ (1u << 8)
#define PARODD_ (1u << 9)
#define HUPCL_ (1u << 10)
#define CLOCAL_ (1u << 11)

#define TCGETS_ 0x5401
#define TCSETS_ 0x5402
#define TCSETSW_ 0x5403
#define TCSETSF_ 0x5404
#define TCSBRK_ 0x5409
#define TCXONC_ 0x540a
#define TCFLSH_ 0x540b
#define TIOCEXCL_ 0x540c
#define TIOCNXCL_ 0x540d
#define TIOCSCTTY_ 0x540e
#define TIOCGPGRP_ 0x540f
#define TIOCSPGRP_ 0x5410
#define TIOCOUTQ_ 0x5411
#define TIOCSTI_ 0x5412
#define TIOCGWINSZ_ 0x5413
#define TIOCSWINSZ_ 0x5414
#define FIONREAD_ 0x541b
#define TIOCPKT_ 0x5420
#define TIOCNOTTY_ 0x5422
#define TCSBRKP_ 0x5425
#define TIOCGSID_ 0x5429
#define TIOCGPTPEER_ 0x5441
#define TIOCSPTLCK_ 0x40045431
/* These have the top bit set, so ops->ioctl receives them as negative ints. */
#define TIOCGPTN_ 0x80045430
#define TIOCGPKT_ 0x80045438
#define TIOCGEXCL_ 0x80045440

/* What a read of a master in packet mode can report, with no data. */
#define TIOCPKT_FLUSHREAD_ 1
#define TIOCPKT_FLUSHWRITE_ 2
#define TIOCPKT_STOP_ 4
#define TIOCPKT_START_ 8
#define TIOCPKT_NOSTOP_ 16
#define TIOCPKT_DOSTOP_ 32

#define TCIFLUSH_ 0
#define TCOFLUSH_ 1
#define TCIOFLUSH_ 2

/* TCXONC's argument */
#define TCOOFF_ 0
#define TCOON_ 1
#define TCIOFF_ 2
#define TCION_ 3

struct winsize_ {
    word_t row;
    word_t col;
    word_t xpixel;
    word_t ypixel;
};

/* Exactly 36 bytes: an ioctl copies this much, and the caller's buffer may be no larger. */
struct termios_ {
    dword_t iflags;
    dword_t oflags;
    dword_t cflags;
    dword_t lflags;
    byte_t line;
    byte_t cc[19];
};

/* A side's input, in the order it will be read. Each position remembers what it was
 * when it went in, because the mode can change while it waits: text queued in line
 * mode is plain bytes to a raw read, and a raw byte does not end a line. */
typedef struct {
    byte_t value;
    byte_t kind;
} TtyCell;

typedef struct {
    TtyCell cells[TTY_BUF_SIZE];
    unsigned head;
    unsigned used;
    /* How many positions from the head are ready for a line-mode read: everything up
     * to and including the last line end or end-of-file mark. The rest is the line
     * still being typed. */
    unsigned released;
} TtyQueue;

struct tty_driver; /* an identity: see pty_master and pty_slave */

struct tty {
    lock_t lock; /* guards everything below unless noted; never held across a wait for
                  * another terminal, and at most one terminal's at a time */
    pid_t_ session;
    pid_t_ fg_group;
    struct winsize_ winsize;
    struct termios_ termios;
    struct tty_driver *driver; /* set at creation */
    int type;                  /* set at creation */
    int num;                   /* set at creation */
    struct {
        struct tty *other;
        bool slave_locked; /* on a slave: opens fail until the master unlocks it */
        bool packet_mode;  /* on a master */
        bool peer_hung_up; /* on a master: its slave has no opens left, or was hung up. Reads
                            * return what's queued, then _EIO, until the slave is opened again */
        byte_t packet_status; /* on a master: the TIOCPKT_ events not yet read */
    } pty;

    /* The rest belongs to pscal_tty.c and pscal_pty.c. */
    int refs; /* atomic: opens, a session map entry, a master for its slave. The
               * moment this reaches 0 the terminal is over. */
    int pins; /* atomic: keeps the memory for a thread passing through, without being
               * a reason for the terminal to stay open. See ttyPin. */
    unsigned opens;
    bool hung_up;
    bool session_reader;
    bool exclusive;    /* TIOCEXCL */
    bool literal_next; /* the VLNEXT key has been typed and the next byte is plain */
    bool erase_open;   /* ECHOPRT has shown a backslash and owes a slash */
    unsigned column;      /* where output has left the cursor */
    unsigned line_column; /* where the line being typed began, for rubbing out a tab */
    unsigned eof_reads;
    /* Flow control: this side's output is stopped, by the stop key or TCOOFF. Writers wait
     * (or get _EAGAIN), and echo is held here until output starts again. */
    bool stopped;
    size_t held_len;
    unsigned char held[1024];
    pthread_cond_t readable;
    pthread_cond_t writable;
    TtyQueue queue;
};

extern const struct pscal_fd_ops pscal_tty_fd_ops;

/* Feeds bytes in as if typed. Returns how many were consumed, counting any discarded
 * or turned into signals, or _EAGAIN if none fit in a non-blocking call. Blocking
 * waits for room with no lock held; non-blocking never waits at all, as it runs on
 * threads serving the app's UI. The caller holds no terminal lock. */
ssize_t tty_input(struct tty *tty, const char *input, size_t len, bool blocking);

/* A read of a terminal that says for itself whether to wait, whatever the open's O_NONBLOCK:
 * for callers that poll the session's terminal without changing how it was opened. */
ssize_t pscalTtyReadMode(struct pscal_fd *fd, void *buf, size_t bufsize, bool nonblocking);

/* Stores the size, and if that is a change sends SIGWINCH to the foreground group, if
 * there is one. May be called with the session registry's lock held and before any
 * session exists. */
void tty_set_winsize(struct tty *tty, struct winsize_ size);

/* Marks the terminal hung up and wakes everyone waiting on it. The caller holds
 * tty->lock; it is released and retaken inside, around the wakes. */
void tty_hangup(struct tty *tty);

int pscalTtyOpenControlling(int flags, struct pscal_fd **out_fd);
int pscalTtySessionPtyNum(pid_t_ sid);

/* Called with the task-table lock held, so nothing under here may ask for it. */
void pscalTtyDropSession(pid_t_ sid);
/* A session leader ended: its terminal is no longer the session's. Returns the terminal's
 * foreground group then, for the caller to send SIGHUP, or 0 if the session had none. */
pid_t_ pscalTtySessionLeaderGone(pid_t_ sid);

/* vproc saying which group is the foreground of a session whose terminal this layer knows.
 * Asks nothing about the caller, sends no signal, and doesn't call back: vproc holds no lock
 * of its own for this, and has already written its side down. */
void pscalTtySetSessionForeground(pid_t_ sid, pid_t_ pgid);

/* Marks the terminal as read by a session's reader thread, for good. Such a reader
 * only sees finished lines, so line mode has to hand it the control keys at once. */
void pscalTtySetSessionReader(struct pscal_fd *fd);

/* Was the read of 0 just returned the end-of-file key? Other reads return 0 too, and a
 * session's reader must not take those for the end of the user's input. */
bool pscalTtyTakeEofKey(struct pscal_fd *fd);

/* The check a slave read starts with, for reads served from vproc's session queue
 * instead. 0 to go ahead, which a stopped caller gets once it has been continued; _EINTR
 * for a caller a handler interrupted; _EIO for one that can't be stopped. */
int pscalTtyCheckBackgroundRead(struct pscal_fd *fd);

/* Between pscal_tty.c and pscal_pty.c; nothing else should call these. */
struct tty *ttyCreate(struct tty_driver *driver, int type, int num);
struct pscal_fd *ttyOpenFile(struct tty *tty, int flags);
void ttyRetain(struct tty *tty);
bool ttyRetainIfAlive(struct tty *tty);
void ttyRelease(struct tty *tty);
void ttyPin(struct tty *tty);
void ttyUnpin(struct tty *tty);
void ttyMarkHungUp(struct tty *tty);

#ifdef __cplusplus
}
#endif
