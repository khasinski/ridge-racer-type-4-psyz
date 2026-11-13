#include <psyz.h>
#include <kernel.h>
#include <libapi.h>
#include <libcomb.h>
#include <psyz/link.h>
#include <psyz/log.h>
#include <psyz/system.h>
#include <string.h>
#include "../internal.h"
#include "os.h"
#include "link_wire.h"
#include "../timing_profile.h"

/* The serial port (SIO1) and the link cable driver on it.
 *
 * The driver is Ridge Racer Type 4's libsio (the early name of libcomb),
 * ported from its decompilation: _comb_control is its _sio_control, the
 * "sio:" device's synchronous and asynchronous reads and writes and the
 * interrupt handler keep its register sequences, its busy-wait loops and
 * its use of the wait callback. Under it, the port's registers are modelled
 * after psx-spx ("Serial Port (SIO)"):
 *   - the transmitter takes a byte from its data register when TXEN is set
 *     and CTS is on, shifts it out in one character time (start bit, data
 *     bits, parity, stop bits at the reload value's bit rate) and only then
 *     sends it to the other end; TX ready / TX idle follow from that;
 *   - received bytes enter an 8-byte FIFO one character time apart; a ninth
 *     sets the overrun error and is lost; RXEN off drops them;
 *   - RESET clears the registers and the FIFO, ACK the error flags; the RX
 *     interrupt comes when the FIFO holds the RX interrupt mode's 1/2/4/8
 *     bytes, the TX interrupt while TX is ready.
 * The other end of the cable is another PsyZ over the network (link_net.c):
 * DSR and CTS are its DTR and RTS. The driver's flow control is a DTR
 * toggle after each received packet of the receive packet size (the sender
 * waits for DSR to change before the next packet) and RTS while a read
 * runs. Both follow from the sender's own bytes once the receiver has
 * announced its read (LinkLines: where it starts and ends, the packet
 * size), so the sender sees them at once, as on a cable; the receiver's
 * report corrects the view when it did something else meanwhile.
 *
 * Without a connection the port is idle with nothing on the other end: DSR
 * and CTS stay off and nothing is received (the console without a cable). */

enum {
    STAT_TX_READY = 0x001,
    STAT_RX_READY = 0x002,
    STAT_TX_IDLE = 0x004,
    STAT_ERRORS = 0x038, /* parity, overrun, bad stop bit */
    STAT_OVERRUN = 0x010,
    STAT_DSR = 0x080,
    STAT_CTS = 0x100,
    STAT_IRQ = 0x200,
    CTRL_TXEN = 0x001,
    CTRL_DTR = 0x002,
    CTRL_RXEN = 0x004,
    CTRL_ACK = 0x010,
    CTRL_RTS = 0x020,
    CTRL_RESET = 0x040,
    CTRL_RX_MODE = 0x300,
    CTRL_TX_IRQ = 0x400,
    CTRL_RX_IRQ = 0x800,
    FIFO_SIZE = 8,
    RX_QUEUE = 4096,
    WIRE_QUEUE = 1024,
};

#define SIO_CLOCK_HZ 33868800u
/* libsio's _sio_control(1, 3, bps): reload value 2073600 / bps (the 33.8688
 * MHz clock over the MUL16 reload factor of its mode 0xCE, then /1 again:
 * the libsio mode's bit rate is 2073600 / reload, i.e. bps). */
#define SIO_BPS_BASE 2073600u

/* AddSIO's PS1 time is ADD_SIO_DEVICE_CYCLES, a pass of a busy wait
 * SIO_POLL_CYCLES (timing_profile.h). */
#define SIO_POLL_NS ((unsigned)(SIO_POLL_CYCLES * 1000000000ull / SIO_CLOCK_HZ))

/* libsio's receive packet sizes (RX interrupt mode 0..3) and the mode bits
 * for a size of 1..8 (-1: unsupported). */
static const short kRxChunk[4] = {1, 2, 4, 8};
static const short kRxModeBits[9] = {-1, 0x000, 0x100, -1, 0x200, -1, -1, -1, 0x300};

typedef int (*SioWaitCallback)(int, unsigned);

typedef struct {
    uint8_t byte;
    uint64_t due_ns;
} RxByte;

typedef struct {
    int kind; // 0 up, 1 down, 2 lines, 3 data
    LinkLines lines;
    uint8_t data[255];
    int len;
    uint64_t at_us;
} WireEvent;

static struct {
    /* SIO1 registers */
    uint16_t ctrl, mode, baud;
    uint32_t errors;
    int irq;
    uint8_t fifo[FIFO_SIZE];
    int fifo_len;
    int tx_full, tx_shifting, tx_blocked;
    uint8_t tx_data, tx_shift;
    uint64_t tx_written_ns, tx_done_ns;
    /* libsio: SioShadowRegs, the requests and SioPollHook */
    struct {
        uint16_t control, mode, baud;
    } shadow;
    struct {
        int active;
        uint8_t* cursor;
        long remaining;
        uint32_t start, end;
    } rd;
    struct {
        int active;
        const uint8_t* cursor;
        long remaining;
        unsigned expect; // DSR level the next byte waits for
    } wr;
    int sync_read;
    uint32_t sync_start, sync_end;
    SioWaitCallback wait;
    /* the cable */
    int up;
    LinkLines remote;
    uint32_t tx_total; // bytes that left the transmitter on this connection
    uint32_t arrived;  // bytes that came off the wire on this connection
    uint32_t toggles, lost;
    RxByte rxq[RX_QUEUE];
    int rxq_head, rxq_len;
    uint64_t rx_last_ns;
    LinkLines reported;
    uint32_t reported_toggles, reported_lost;
    int report_due;
} sio;

/* Network events, queued by the transport thread for the port model. */
static void* wire_lock;
static WireEvent wire_queue[WIRE_QUEUE];
static int wire_head, wire_len;
static int wire_posted; // an interrupt to take them is on its way

static void SioInterrupt(void);

static void Lock(void) { Psyz_KernelLockInterrupts(); }
static void Unlock(void) { Psyz_KernelUnlockInterrupts(); }

static uint64_t NowNs(void) { return Psyz_KernelNowUs() * 1000; }

/* One character on the wire: start bit, 5..8 data bits, parity, 1/1.5/2
 * stop bits, at the clock over reload value and factor (MUL1/1/16/64). */
static uint64_t ByteNs(void) {
    static const unsigned factor[4] = {1, 1, 16, 64};
    unsigned half_bits = 2 * (1 + 5 + ((sio.mode >> 2) & 3)) +
                         (sio.mode & 0x10 ? 2 : 0) +
                         ((sio.mode >> 6) == 2 ? 3 : (sio.mode >> 6) == 3 ? 4 : 2);
    uint64_t clocks = (uint64_t)(sio.baud ? sio.baud : 1) * factor[sio.mode & 3];
    return clocks * half_bits * 1000000000ull / 2 / SIO_CLOCK_HZ;
}

static int Chunk(void) { return kRxChunk[(sio.shadow.control & CTRL_RX_MODE) >> 8]; }

/* ---- the other end, as this end sees it ---- */

static int RemoteCts(void) {
    if (!sio.up || !sio.remote.rts)
        return 0;
    /* its read ends with the bytes sent into it, and drops RTS there */
    return !(sio.remote.window && (int32_t)(sio.tx_total - sio.remote.end) >= 0);
}

static int RemoteDsr(void) {
    int dsr;
    if (!sio.up)
        return 0;
    dsr = sio.remote.dtr;
    if (sio.remote.window && sio.remote.chunk) {
        /* the toggles for the packets sent into its read since it reported */
        int32_t from = (int32_t)(sio.remote.next - sio.remote.start);
        int32_t to = (int32_t)(sio.tx_total - sio.remote.start);
        int32_t end = (int32_t)(sio.remote.end - sio.remote.start);
        if (to > end)
            to = end;
        if (from < 0)
            from = 0;
        if (to > from)
            dsr ^= (int)((to / sio.remote.chunk - from / sio.remote.chunk) & 1);
    }
    return dsr;
}

/* ---- registers ---- */

static void ResetPort(void) {
    sio.lost += (uint32_t)sio.fifo_len;
    sio.ctrl = 0;
    sio.mode = 0;
    sio.baud = 0;
    sio.errors = 0;
    sio.irq = 0;
    sio.fifo_len = 0;
    sio.tx_full = 0;
    sio.tx_shifting = 0; // a character being shifted out is cut off
}

static void WriteCtrl(unsigned value) {
    if (value & CTRL_RESET) {
        ResetPort();
        value = 0;
    }
    if (value & CTRL_ACK) {
        sio.errors = 0;
        sio.irq = 0;
    }
    sio.ctrl = (uint16_t)(value & ~(CTRL_ACK | CTRL_RESET));
}

static void DrainWire(void) {
    if (!wire_lock)
        return;
    Psyz_OsLock(wire_lock);
    wire_posted = 0;
    Psyz_OsUnlock(wire_lock);
    for (;;) {
        WireEvent ev;
        Psyz_OsLock(wire_lock);
        if (!wire_len) {
            Psyz_OsUnlock(wire_lock);
            return;
        }
        ev = wire_queue[wire_head];
        wire_head = (wire_head + 1) % WIRE_QUEUE;
        wire_len--;
        Psyz_OsUnlock(wire_lock);
        switch (ev.kind) {
        case 0: { /* a new cable: offsets restart, the FIFO is empty */
            uint32_t next = sio.arrived - (uint32_t)sio.fifo_len;
            sio.rd.start -= next;
            sio.rd.end -= next;
            sio.sync_start -= next;
            sio.sync_end -= next;
            memset(&sio.remote, 0, sizeof(sio.remote));
            sio.up = 1;
            sio.tx_total = sio.arrived = 0;
            sio.fifo_len = 0;
            sio.rxq_len = 0;
            sio.report_due = 1;
            break;
        }
        case 1:
            sio.up = 0;
            memset(&sio.remote, 0, sizeof(sio.remote));
            sio.rxq_len = 0;
            break;
        case 2:
            if (sio.up)
                sio.remote = ev.lines;
            break;
        case 3: {
            uint64_t byte_ns = ByteNs();
            for (int i = 0; sio.up && i < ev.len; i++) {
                uint64_t due = ev.at_us * 1000;
                if (due < sio.rx_last_ns + byte_ns)
                    due = sio.rx_last_ns + byte_ns;
                sio.rx_last_ns = due;
                if (sio.rxq_len == RX_QUEUE) {
                    sio.arrived++; // more than the cable could ever hold
                    sio.lost++;
                    continue;
                }
                RxByte* b = &sio.rxq[(sio.rxq_head + sio.rxq_len) % RX_QUEUE];
                b->byte = ev.data[i];
                b->due_ns = due;
                sio.rxq_len++;
            }
            break;
        }
        }
    }
}

static void IrqHandler(void);

/* The RX interrupt condition with a read waiting for it: the FIFO holds
 * the RX interrupt mode's 1/2/4/8 bytes. */
static int RxIrqDue(void) {
    return (sio.ctrl & CTRL_RX_IRQ) && sio.rd.active && sio.rd.remaining &&
           sio.fifo_len >= kRxChunk[(sio.ctrl & CTRL_RX_MODE) >> 8];
}

/* Moves the port to the emulated time now. Received bytes that came due
 * meanwhile enter the FIFO in order, and the RX interrupt runs as each
 * packet completes, as it would have on time: a host thread that runs late
 * must not overrun a FIFO the console's interrupt would have drained. */
static int advancing;
static void Advance(void) {
    uint64_t now = NowNs();
    if (advancing)
        return; // the interrupt handler's own register accesses
    advancing = 1;
    DrainWire();
    while (sio.rxq_len && sio.rxq[sio.rxq_head].due_ns <= now) {
        uint8_t byte;
        if (RxIrqDue())
            IrqHandler();
        byte = sio.rxq[sio.rxq_head].byte;
        sio.rxq_head = (sio.rxq_head + 1) % RX_QUEUE;
        sio.rxq_len--;
        sio.arrived++;
        if (!(sio.ctrl & CTRL_RXEN)) {
            sio.lost++;
        } else if (sio.fifo_len < FIFO_SIZE) {
            sio.fifo[sio.fifo_len++] = byte;
        } else {
            sio.errors |= STAT_OVERRUN;
            sio.lost++;
        }
    }
    for (int i = 0; i < FIFO_SIZE && RxIrqDue(); i++)
        IrqHandler();
    for (;;) {
        if (sio.tx_shifting) {
            if (now < sio.tx_done_ns)
                break;
            sio.tx_shifting = 0;
            if (sio.up && Psyz_LinkSend(LINK_MSG_DATA, &sio.tx_shift, 1))
                sio.tx_total++;
        }
        if (!sio.tx_full)
            break;
        if (!(sio.ctrl & CTRL_TXEN) || !RemoteCts()) {
            sio.tx_blocked = 1;
            break;
        }
        /* right after the character before it, or when it was written;
         * one that waited for CTS goes now */
        uint64_t start = sio.tx_written_ns > sio.tx_done_ns ? sio.tx_written_ns : sio.tx_done_ns;
        if (sio.tx_blocked)
            start = now;
        sio.tx_blocked = 0;
        sio.tx_shift = sio.tx_data;
        sio.tx_full = 0;
        sio.tx_shifting = 1;
        sio.tx_done_ns = start + ByteNs();
    }
    advancing = 0;
}

static unsigned StatusNow(void) {
    unsigned st = sio.errors;
    if (!sio.tx_full)
        st |= STAT_TX_READY;
    if (!sio.tx_full && !sio.tx_shifting)
        st |= STAT_TX_IDLE;
    if (sio.fifo_len)
        st |= STAT_RX_READY;
    if (RemoteDsr())
        st |= STAT_DSR;
    if (RemoteCts())
        st |= STAT_CTS;
    if (sio.irq)
        st |= STAT_IRQ;
    return st;
}

static unsigned Status(void) {
    Advance();
    return StatusNow();
}

static uint8_t ReadData(void) {
    uint8_t byte;
    if (!sio.fifo_len)
        return 0xFF; // an empty FIFO reads its last byte; nothing waits on it
    byte = sio.fifo[0];
    memmove(sio.fifo, sio.fifo + 1, (size_t)--sio.fifo_len);
    return byte;
}

static void WriteData(uint8_t byte) {
    Advance();
    sio.tx_data = byte;
    sio.tx_full = 1;
    sio.tx_written_ns = NowNs();
    Advance();
}

static void ToggleDtr(void) {
    WriteCtrl(sio.ctrl ^ CTRL_DTR);
    sio.toggles++;
}

/* ---- telling the other end ---- */

static void CurrentLines(LinkLines* l) {
    memset(l, 0, sizeof(*l));
    l->dtr = (sio.ctrl & CTRL_DTR) != 0;
    l->rts = (sio.ctrl & CTRL_RTS) != 0;
    l->next = sio.arrived - (uint32_t)sio.fifo_len;
    if (sio.sync_read) {
        l->window = 1;
        l->start = sio.sync_start;
        l->end = sio.sync_end;
        l->chunk = (uint8_t)Chunk();
    } else if (sio.rd.active && (sio.ctrl & CTRL_RX_IRQ)) {
        l->window = 1;
        l->start = sio.rd.start;
        l->end = sio.rd.end;
        l->chunk = (uint8_t)Chunk();
    }
}

static void Report(void) {
    LinkLines l;
    uint8_t out[LINK_LINES_BYTES];
    if (!sio.up)
        return;
    CurrentLines(&l);
    /* `next` alone moves with every byte taken; only a toggle or a lost
     * byte makes the other end's view of it stale */
    if (!sio.report_due && l.dtr == sio.reported.dtr && l.rts == sio.reported.rts &&
        l.window == sio.reported.window && l.chunk == sio.reported.chunk &&
        l.start == sio.reported.start && l.end == sio.reported.end &&
        sio.toggles == sio.reported_toggles && sio.lost == sio.reported_lost)
        return;
    LinkLinesEncode(&l, out);
    if (Psyz_LinkSend(LINK_MSG_LINES, out, LINK_LINES_BYTES)) {
        sio.reported = l;
        sio.reported_toggles = sio.toggles;
        sio.reported_lost = sio.lost;
        sio.report_due = 0;
    }
}

/* The next time the port changes by itself (a received byte entering the
 * FIFO, a character shifted out): an interrupt then. */
static uint64_t scheduled_us; // the pending timed interrupt, 0 for none

static void Schedule(void) {
    uint64_t next = UINT64_MAX, at;
    if (sio.rxq_len)
        next = sio.rxq[sio.rxq_head].due_ns;
    if (sio.tx_shifting && sio.tx_done_ns < next)
        next = sio.tx_done_ns;
    at = next == UINT64_MAX ? 0 : (next + 999) / 1000;
    if (at == scheduled_us)
        return;
    if (scheduled_us)
        Psyz_KernelCancel(SioInterrupt);
    scheduled_us = at;
    if (at)
        Psyz_KernelPostAt(at, SioInterrupt);
}

/* ---- libsio interrupt handler (_sio_irq_handler) ---- */

static int IrqPending(void) {
    unsigned st = Status();
    if ((st & STAT_ERRORS) && (sio.ctrl & (CTRL_RX_IRQ | CTRL_TX_IRQ)))
        return 1;
    if ((sio.ctrl & CTRL_RX_IRQ) && sio.rd.active && sio.rd.remaining &&
        sio.fifo_len >= kRxChunk[(sio.ctrl & CTRL_RX_MODE) >> 8])
        return 1;
    if ((sio.ctrl & CTRL_TX_IRQ) && sio.wr.active && (st & STAT_TX_READY) &&
        (st & STAT_DSR) == sio.wr.expect)
        return 1;
    return 0;
}

static void IrqHandler(void) {
    unsigned st = StatusNow();
    sio.irq = 1;
    if (st & STAT_ERRORS) {
        WriteCtrl(sio.ctrl & ~(CTRL_RX_IRQ | CTRL_RTS));
        sio.rd.remaining = 0;
        sio.rd.active = 0;
        WriteCtrl(CTRL_ACK | CTRL_RESET);
        sio.mode = sio.shadow.mode;
        sio.baud = sio.shadow.baud;
        WriteCtrl(sio.ctrl | CTRL_ACK);
        DeliverEvent(HwSIO, EvSpERROR);
        return;
    }
    if (sio.rd.active && sio.rd.remaining && (st & STAT_RX_READY)) {
        int count = Chunk();
        for (int i = 0; i < count; i++)
            *sio.rd.cursor++ = ReadData();
        sio.rd.remaining -= count;
        if (sio.rd.remaining <= 0) {
            sio.rd.active = 0;
            WriteCtrl(sio.ctrl & ~(CTRL_RX_IRQ | CTRL_RTS));
            DeliverEvent(HwSIO, EvSpIOER);
        }
        ToggleDtr();
    }
    if (sio.wr.active && (st & STAT_TX_READY) && (StatusNow() & STAT_DSR) == sio.wr.expect) {
        if (sio.wr.remaining == 0) {
            sio.wr.active = 0;
            DeliverEvent(HwSIO, EvSpIOEW);
        } else {
            sio.wr.expect ^= STAT_DSR;
            WriteData(*sio.wr.cursor++);
            sio.wr.remaining--;
            WriteCtrl(sio.ctrl | CTRL_TX_IRQ);
        }
    }
    WriteCtrl(sio.ctrl | CTRL_ACK);
}

/* The port's interrupt, and the network's: runs in interrupt context. */
static void SioInterrupt(void) {
    Lock();
    if (scheduled_us && Psyz_KernelNowUs() >= scheduled_us)
        scheduled_us = 0; // this is it (or it comes with this one)
    Advance();
    for (int i = 0; i < 64 && IrqPending(); i++)
        IrqHandler();
    Schedule();
    Report();
    Unlock();
}

/* After the driver changed the port from the main code: report it and let
 * an interrupt that became due run. */
static void Changed(void) {
    Report();
    Schedule();
    if ((sio.ctrl & (CTRL_RX_IRQ | CTRL_TX_IRQ)) && IrqPending())
        Psyz_KernelPost(SioInterrupt);
}

/* ---- the transport's events ---- */

static void Queue(const WireEvent* ev) {
    int post;
    if (!wire_lock)
        return;
    Psyz_OsLock(wire_lock);
    if (wire_len < WIRE_QUEUE) {
        wire_queue[(wire_head + wire_len) % WIRE_QUEUE] = *ev;
        wire_queue[(wire_head + wire_len) % WIRE_QUEUE].at_us = Psyz_KernelNowUs();
        wire_len++;
    } else {
        WARNF("link: dropped a network event");
    }
    post = !wire_posted;
    wire_posted = 1;
    Psyz_OsUnlock(wire_lock);
    if (post)
        Psyz_KernelPost(SioInterrupt);
}

void Psyz_SioWireInit(void) {
    if (!wire_lock)
        wire_lock = Psyz_OsLockCreate();
}

void Psyz_SioWireUp(void) {
    WireEvent ev = {0};
    Queue(&ev);
}

void Psyz_SioWireDown(void) {
    WireEvent ev = {0};
    ev.kind = 1;
    Queue(&ev);
}

void Psyz_SioWireLines(const LinkLines* lines) {
    WireEvent ev = {0};
    ev.kind = 2;
    ev.lines = *lines;
    Queue(&ev);
}

void Psyz_SioWireData(const uint8_t* bytes, int len) {
    while (len > 0) {
        WireEvent ev = {0};
        ev.kind = 3;
        ev.len = len < (int)sizeof(ev.data) ? len : (int)sizeof(ev.data);
        memcpy(ev.data, bytes, (size_t)ev.len);
        Queue(&ev);
        bytes += ev.len;
        len -= ev.len;
    }
}

/* ---- libsio: AddSIO / DelSIO (_sio_init / _sio_deinit) ---- */

void AddCOMB(void) {
    Psyz_SioWireInit();
    Lock();
    sio.shadow.control = CTRL_TXEN | CTRL_RXEN;
    sio.shadow.mode = 0xCE;
    sio.shadow.baud = 0xD8;
    WriteCtrl(CTRL_ACK | CTRL_RESET);
    sio.mode = sio.shadow.mode;
    sio.baud = sio.shadow.baud;
    WriteCtrl(sio.ctrl | CTRL_ACK);
    WriteCtrl(sio.shadow.control);
    sio.rd.active = sio.wr.active = 0;
    sio.rd.cursor = NULL;
    sio.wr.cursor = NULL;
    sio.rd.remaining = sio.wr.remaining = 0;
    sio.wait = NULL;
    Changed();
    Unlock();
    Psyz_SpendCycles(ADD_SIO_DEVICE_CYCLES);
}

void DelCOMB(void) {
    Lock();
    WriteCtrl(sio.ctrl | CTRL_ACK | CTRL_RESET);
    WriteCtrl(sio.shadow.control);
    sio.rd.active = sio.wr.active = 0;
    sio.rd.remaining = sio.wr.remaining = 0;
    Changed();
    Unlock();
}

void ChangeClearSIO(long val) { (void)val; }

/* ---- libsio: _sio_control ---- */

long _comb_control(u_long cmd, u_long arg, u_long param) {
    long ret = 0;
    Lock();
    switch (cmd) {
    case 0:
        switch (arg) {
        case 0:
            ret = (long)Status();
            break;
        case 1:
            ret = (sio.ctrl >> 1) & 1;
            if (sio.ctrl & CTRL_RTS)
                ret |= 2;
            break;
        case 2:
            ret = sio.shadow.mode;
            break;
        case 3:
            ret = sio.shadow.baud ? (long)(SIO_BPS_BASE / sio.shadow.baud) : 0;
            break;
        case 4:
            ret = 1; // libsio answers 1 whatever the receive packet size
            break;
        case 5:
            ret = param == 0 ? sio.wr.remaining : sio.rd.remaining;
            break;
        case 6:
            ret = (param == 0 ? sio.wr.active : sio.rd.active) != 0;
            break;
        }
        break;
    case 1:
        switch (arg) {
        case 1:
            sio.shadow.control = (uint16_t)((sio.ctrl & ~(CTRL_DTR | CTRL_RTS)) |
                                            ((param & 1) << 1) | (param & 2 ? CTRL_RTS : 0));
            WriteCtrl(sio.shadow.control);
            break;
        case 2:
            sio.shadow.mode = (uint16_t)param;
            sio.mode = sio.shadow.mode;
            break;
        case 3:
            if (param == 0 || SIO_BPS_BASE % param) {
                ret = -1;
                break;
            }
            sio.shadow.baud = (uint16_t)(SIO_BPS_BASE / param);
            sio.baud = sio.shadow.baud;
            WriteCtrl(sio.ctrl | CTRL_ACK);
            break;
        case 4:
            if (param >= 1 && param <= 8) {
                ret = kRxModeBits[param];
                if (ret < 0)
                    break;
                sio.shadow.control = (uint16_t)((sio.shadow.control & ~CTRL_RX_MODE) | ret);
                WriteCtrl(sio.shadow.control);
            }
            break;
        }
        break;
    case 2:
        switch (arg) {
        case 0: /* reset the port to the driver's settings */
            WriteCtrl(sio.ctrl | CTRL_ACK | CTRL_RESET);
            sio.mode = sio.shadow.mode;
            WriteCtrl(sio.shadow.control);
            sio.baud = sio.shadow.baud;
            WriteCtrl(sio.ctrl | CTRL_ACK);
            break;
        case 1:
            WriteCtrl(sio.ctrl | CTRL_ACK);
            break;
        case 2: /* cancel the asynchronous write */
            WriteCtrl(sio.ctrl & ~CTRL_TX_IRQ);
            sio.wr.active = 0;
            sio.wr.remaining = 0;
            break;
        case 3: /* cancel the asynchronous read */
            WriteCtrl(sio.ctrl & ~(CTRL_RX_IRQ | CTRL_RTS));
            sio.rd.active = 0;
            sio.rd.remaining = 0;
            break;
        }
        break;
    case 3:
        if (arg == 0)
            WriteCtrl(param ? sio.ctrl | CTRL_RTS : sio.ctrl & ~CTRL_RTS);
        else if (arg == 1)
            ret = (Status() & STAT_CTS) != 0;
        break;
    case 4:
        if (arg == 0) {
            ret = (long)(uintptr_t)sio.wait;
            sio.wait = (SioWaitCallback)(uintptr_t)param;
        }
        break;
    case 5:
        /* libsio calls ResetGraph(5) here, which on the PS1 also restarts
         * the GPU's video timing: both consoles call it at the handshake.
         * The host's fields keep their own clock; the game's protocol does
         * not need their phases to agree. */
        break;
    }
    Changed();
    Unlock();
    return ret;
}

/* ---- the "sio:" device: _sio_inout ---- */

/* One pass of a busy-wait loop: the wait callback (0 gives up) and the
 * pass's PS1 time. Called with the port locked; runs the callback
 * unlocked, as the game's code. */
static int WaitPass(int kind, unsigned* polls) {
    SioWaitCallback wait = sio.wait;
    int go = 1;
    Report();
    Unlock();
    if (wait)
        go = wait(kind, (*polls)++) != 0;
    if (go)
        Psyz_SpendTime(SIO_POLL_NS);
    Lock();
    return go;
}

/* Synchronous read (libsio's siosend.c, named _sio_write there): RTS on,
 * a DTR toggle after every packet, RTS off at the end. */
static long SyncRead(uint8_t* buffer, long length) {
    unsigned polls = 0;
    int taken = 0, chunk;
    long result;
    if (sio.rd.active)
        return -1;
    chunk = Chunk();
    sio.rd.remaining = length;
    sio.rd.cursor = buffer;
    sio.rd.active = 0;
    Advance();
    sio.sync_read = 1;
    sio.sync_start = sio.arrived - (uint32_t)sio.fifo_len;
    sio.sync_end = sio.sync_start + (uint32_t)length;
    WriteCtrl(sio.ctrl | CTRL_RTS);
    while (sio.rd.remaining) {
        if (Status() & STAT_ERRORS) {
            unsigned saved = sio.ctrl;
            WriteCtrl(CTRL_ACK | CTRL_RESET);
            sio.mode = sio.shadow.mode;
            sio.baud = sio.shadow.baud;
            WriteCtrl(sio.ctrl | CTRL_ACK);
            WriteCtrl(saved & ~CTRL_RTS);
            DeliverEvent(HwSIO, EvSpERROR);
            goto done;
        }
        while (!(Status() & STAT_RX_READY)) {
            if (!WaitPass(1, &polls)) {
                WriteCtrl(sio.ctrl & ~CTRL_RTS);
                DeliverEvent(HwSIO, EvSpTIMOUT);
                goto done;
            }
        }
        *sio.rd.cursor++ = ReadData();
        sio.rd.remaining--;
        if (++taken == chunk) {
            taken = 0;
            ToggleDtr();
        }
    }
    WriteCtrl(sio.ctrl & ~CTRL_RTS);
done:
    sio.sync_read = 0;
    result = length - sio.rd.remaining;
    return result;
}

/* Synchronous write (libsio's siorecv.c, named _sio_read there): a packet
 * of bytes, then wait for the receiver's DTR toggle. */
static long SyncWrite(const uint8_t* buffer, long length) {
    unsigned polls = 0, expect = 0;
    int sent = 0, chunk;
    if (sio.wr.active)
        return -1;
    chunk = Chunk();
    sio.wr.remaining = length;
    sio.wr.cursor = buffer;
    while (sio.wr.remaining) {
        while ((Status() & (STAT_TX_READY | STAT_TX_IDLE)) != (STAT_TX_READY | STAT_TX_IDLE)) {
            if (!WaitPass(2, &polls)) {
                DeliverEvent(HwSIO, EvSpTIMOUT);
                return length - sio.wr.remaining;
            }
        }
        if (sent == 0)
            expect = Status() & STAT_DSR;
        WriteData(*sio.wr.cursor++);
        sio.wr.remaining--;
        if (++sent == chunk) {
            while ((Status() & STAT_DSR) == expect) {
                if (!WaitPass(2, &polls)) {
                    DeliverEvent(HwSIO, EvSpTIMOUT);
                    return length - sio.wr.remaining - 1;
                }
            }
            sent = 0;
        }
    }
    return length - sio.wr.remaining;
}

long Psyz_CombRead(void* buffer, long length, int async) {
    long result = -1;
    Lock();
    if (!async) {
        result = SyncRead(buffer, length);
    } else if (!sio.rd.active) {
        /* _sio_write_async: the RX interrupt fills the buffer */
        Advance();
        sio.rd.remaining = length;
        sio.rd.cursor = buffer;
        sio.rd.active = 1;
        sio.rd.start = sio.arrived - (uint32_t)sio.fifo_len;
        sio.rd.end = sio.rd.start + (uint32_t)length;
        WriteCtrl(sio.ctrl | CTRL_RX_IRQ);
        WriteCtrl(sio.ctrl | CTRL_RTS);
        result = 0;
    }
    Changed();
    Unlock();
    return result;
}

long Psyz_CombWrite(const void* buffer, long length, int async) {
    long result = -1;
    Lock();
    if (!async) {
        result = SyncWrite(buffer, length);
    } else if (!sio.wr.active) {
        /* _sio_read_async: the TX interrupt sends a byte per DSR change */
        sio.wr.remaining = length;
        sio.wr.cursor = buffer;
        sio.wr.active = 1;
        sio.wr.expect = Status() & STAT_DSR;
        WriteCtrl(sio.ctrl | CTRL_TX_IRQ);
        result = 0;
    }
    Changed();
    Unlock();
    return result;
}

/* ---- LinkLines on the wire (little endian) ---- */

static void Put32(uint8_t* o, uint32_t v) {
    for (int i = 0; i < 4; i++)
        o[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t Get32(const uint8_t* i) {
    return (uint32_t)i[0] | (uint32_t)i[1] << 8 | (uint32_t)i[2] << 16 | (uint32_t)i[3] << 24;
}

void LinkLinesEncode(const LinkLines* l, uint8_t* o) {
    o[0] = (uint8_t)((l->dtr ? 1 : 0) | (l->rts ? 2 : 0) | (l->window ? 4 : 0));
    o[1] = l->chunk;
    o[2] = o[3] = 0;
    Put32(o + 4, l->next);
    Put32(o + 8, l->start);
    Put32(o + 12, l->end);
}

int LinkLinesDecode(LinkLines* l, const uint8_t* i, int len) {
    if (len < LINK_LINES_BYTES)
        return 0;
    l->dtr = i[0] & 1;
    l->rts = (i[0] >> 1) & 1;
    l->window = (i[0] >> 2) & 1;
    l->chunk = i[1];
    l->next = Get32(i + 4);
    l->start = Get32(i + 8);
    l->end = Get32(i + 12);
    return 1;
}
