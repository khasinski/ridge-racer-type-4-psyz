/* PSY-Q 4.x libpad with direct connection (PadInitDirect, PadStartCom).
 *
 * This is the library's channel state machine as the decompiled PSY-Q
 * objects implement it (ridge-racer-type-4 decompilation, src/main/PAL/
 * psyq/libpad: padgate.c PadInitDirect, padenab.c _padIntPad, padinit2.c
 * the receive stages, padrecv.c/padpoll.c the byte transfers, padlist.c,
 * padstop2.c, padstat2.c, padpatch.c, pdresres.c, padtmchk.c the packet
 * handling and the configuration commands, padctl.c the queries). Its
 * vblank handler exchanges with each port a byte at a time; the
 * controllers answer through Psyz_PadsTransfer (pad.c). So, as on the
 * console:
 *  - PadGetState, PadInfoMode, PadInfoAct and PadInfoComb read what the
 *    channel learnt; finding a DUAL SHOCK's modes and actuators takes 17
 *    exchanges (0x43, 0x45 and 0x4C twice, 0x47 twice, 0x4C, 0x46 and 0x47
 *    twice per entry, 0x43), PadSetActAlign three (0x43, 0x4D, 0x43) and
 *    PadSetMainMode four or more; a digital pad refuses 0x43 eleven times
 *    before libpad settles on it.
 *  - A packet reaches the receive buffer when the port next starts a
 *    transfer, or while the other port's packet is clocked (port 1's at the
 *    same vblank when port 2 has a pad); answers in configuration mode
 *    (ID 0xF3) and failed exchanges leave the buffer as it was.
 *  - The actuator bytes go out through the gates of the exchange before
 *    (_padAllocGates runs after the first one is sent), each gate opening
 *    when the actuator's byte is set and the 60-unit budget allows.
 * The SIO register handshakes, baud switches and root counter 2 timeouts
 * only cost time; the handler's time is charged in Psyz_VSyncBlank
 * (libapi.c). The Multi Tap (PadInitMtap) is not implemented. */

#include <psyz.h>
#include <psyz/input.h>
#include <psyz/types.h>
#include <libetc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../internal.h"


/* The tables a channel loads (modes, actuators, combinations) live in a
 * 0x80-byte arena laid out as PS1 libpad lays it out in the channel record:
 * it starts one byte before a word boundary, so the tables start at offset
 * ARENA_ALIGN, and the combinations' button lists end at ARENA_BYTES. */
enum { ARENA_BYTES = 0x80, ARENA_ALIGN = 1, COMB_BYTES = 8 };

typedef struct Channel Channel;
typedef void (*UpdateFn)(Channel*);
typedef int (*CompleteFn)(Channel*);

struct Channel {
    int modes, entries, combs; // arena offsets of the tables, -1 none
    int comb_buttons[ARENA_BYTES / COMB_BYTES]; // buttons04 per combination
    UpdateFn update;     // update14: sets the next command
    CompleteFn complete; // complete18: takes its answer
    const u8* layout;    // layout20: PadSetActAlign table
    u8 argument;         // argument24: one-byte command parameter
    const u8* data28;    // PadSetAct buffer
    const u8* data2C;    // command parameters
    u8* input;           // input30: the receive buffer
    u8 length34, pad35, length36, command37, previous38, flag39;
    u8 packet[0x23];
    u8 b44, b45, phase, state47, flag48, state49, retry4A;
    int type4C;
    u8 b50, modeArgs51[2], modeEqual53;
    u8 output[15]; // gates 3..8, then the six actuator IDs (payload)
    u8 arena[ARENA_BYTES + COMB_BYTES];
    u8 modeCountE3, argumentE4;
    u16 pending;
    u8 responseE8, entryCountE9, combCountEA, scanIndexEB;
    u16 scanOffsetEC, scanSeenEE;
};

#define GATES(c) (&(c)->output[3])
#define PAYLOAD(c) (&(c)->output[9])

static Channel channels[2];
static u8 dummy_input[2][PSYZ_PAD_BUF_LEN];
static int com_active;       // PadComActive
static int com_started;      // the handler is in the interrupt queue
static int first_channel;    // PadFirstChannel
static int last_channel = 1; // PadLastChannel
static int active_channel;   // PadActiveChannel
static int slot_index[2] = {-1, -1}; // PadMultitapSlotIndex: 0 a packet waits
static int bytes_left;       // PadPacketBytesLeft
static int gate_cost;        // PadTotalGateCost
static int timeout_counts[2];
static int vsync_flag; // D_800ABCD0: the handler ran (PadChkVsync)
static u8* scan_ptr;   // PadPatchScanPtr

/* The bus: the reply to the latest byte sent and whether it was ACKed. */
static u8 rx_latch;
static int ack_latch;
static int bus_port;

static void Send(u8 byte) {
    ack_latch = Psyz_PadsTransfer(bus_port, Psyz_PadsFrame(bus_port), byte,
                                  &rx_latch);
}

/* ---- tables in the arena ---- */

static unsigned ModeAt(const Channel* c, int i) {
    const u8* p = &c->arena[c->modes + 2 * i];
    return p[0] | (unsigned)p[1] << 8;
}

static void SetModeAt(Channel* c, int i, unsigned v) {
    u8* p = &c->arena[c->modes + 2 * i];
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static u8* EntryAt(Channel* c, int i) { return &c->arena[c->entries + 5 * i]; }
static u8* CombAt(Channel* c, int i) {
    return &c->arena[c->combs + COMB_BYTES * i];
}

/* ---- padlist.c ---- */

static void ResetChannel(Channel* c) {
    if (c->state49 != 0) {
        c->state49 = 0;
        c->phase = 0;
        c->pending = 0;
        c->update = NULL;
        c->complete = NULL;
        c->modeCountE3 = 0;
        c->argumentE4 = 0;
        c->entryCountE9 = 0;
        c->combCountEA = 0;
        c->modes = c->entries = c->combs = -1;
        memset(PAYLOAD(c), 0xFF, 6);
    }
}

static void SaveCommand(Channel* c) {
    c->previous38 = c->command37;
    c->command37 = 0;
}

static u8 TxByte(const Channel* c) {
    int index = c->b45 - 3;
    switch (c->command37) {
    case 0:
        if (index < 6 && GATES(c)[index] == 0)
            return 0;
        if (index >= c->length34)
            return 0;
        return c->data28[index];
    case 0x4D:
        if (index >= c->length36)
            return 0xFF;
        return c->data2C[index];
    default:
        if (index >= c->length36)
            return 0;
        return c->data2C[index];
    }
}

/* ---- pdresres.c: the commands ---- */

static void SetCommand(Channel* c, u8 cmd, const u8* data, u8 len) {
    c->command37 = cmd;
    c->data2C = data;
    c->length36 = len;
}

static void CmdWithArgument(Channel* c, u8 cmd, int value) {
    c->argument = (u8)value;
    SetCommand(c, cmd, &c->argument, 1);
}

static void CmdParaMode(Channel* c, int value) { CmdWithArgument(c, 0x43, value); }

static void MainModeUpdate(Channel* c) {
    switch (c->phase) {
    case 2:
        SetCommand(c, 0x44, c->modeArgs51, 2);
        break;
    case 3:
        SetCommand(c, 0x4D, PAYLOAD(c), 6);
        break;
    }
}

static int MainModeComplete(Channel* c) {
    if (c->modeEqual53) {
        if (c->phase == 2)
            return 1;
        c->phase = 0xFE;
        return 0;
    }
    ResetChannel(c);
    return 0;
}

/* ---- padstop.c ---- */

static int ChannelBusy(const Channel* c) {
    return c->pending == 0 || c->phase != 0xFF;
}

/* ---- padstat2.c: modes, actuators, combinations ---- */

static int GetActSize(const Channel* c) {
    int modes = ((c->modeCountE3 + 1) >> 1) * 4;
    int entries = ((c->entryCountE9 * 5 + 3) & 0xFFC) + 4;
    return modes + entries + c->scanOffsetEC;
}

static void SendActInfoCmd(Channel* c);
static int RecvActInfoReply(Channel* c);

static int LoadActInfo(Channel* c) {
    if (c->entries >= 0 || ChannelBusy(c))
        return 0;
    c->state49 = 4;
    c->modes = ARENA_ALIGN;
    c->entries = c->modes + ((c->modeCountE3 + 1) >> 1 << 2);
    c->combs = c->entries + ((c->entryCountE9 * 5 + 3) & 0xFFC);
    c->phase = 1;
    c->update = SendActInfoCmd;
    c->state47 = 0;
    c->complete = RecvActInfoReply;
    return 1;
}

static void SendAtLoadInfo(Channel* c) {
    switch (c->phase) {
    case 2:
        SetCommand(c, 0x45, NULL, 0);
        break;
    case 3:
        CmdWithArgument(c, 0x4C, c->argumentE4);
        break;
    case 4:
        CmdWithArgument(c, 0x47, c->state47);
        break;
    }
}

static int RecvAtLoadInfo(Channel* c) {
    const u8* q = c->packet;
    switch (c->phase) {
    case 2:
        if (q[7] != 0)
            return 0;
        c->scanSeenEE = c->modeCountE3 == q[3] && c->argumentE4 == q[4] &&
                                c->entryCountE9 == q[5] && c->combCountEA == q[6]
                            ? 0
                            : 0xFFFF;
        c->modeCountE3 = q[3];
        c->argumentE4 = q[4];
        c->pending = 0;
        c->entryCountE9 = q[5];
        c->combCountEA = q[6];
        c->scanOffsetEC = 0;
        if (c->scanSeenEE != 0)
            return 0;
        c->scanIndexEB = 0;
        return 1;
    case 3: {
        u16 t;
        if (q[2] != 0 || q[3] != 0)
            return 0;
        t = (u16)(q[5] + (q[4] << 8));
        c->pending = t;
        if (c->scanSeenEE != t) {
            c->scanSeenEE = t;
            return 0;
        }
        c->scanSeenEE = 0xFFFF;
        c->scanIndexEB = 0;
        c->state47 = 0;
        return 1;
    }
    case 4:
        if (q[2] != 0 || q[3] != 0)
            return 0;
        c->scanOffsetEC = (u16)(c->scanOffsetEC + 8 + ((q[4] + 3) & 0x1FC));
        if (++c->state47 < c->combCountEA)
            return 0;
        if (GetActSize(c) >= 0x81) {
            ResetChannel(c);
            c->phase = 0xFE;
            c->state49 = 2;
            return 0;
        }
        if (c->scanSeenEE != c->scanOffsetEC) {
            c->scanSeenEE = c->scanOffsetEC;
            c->state47 = 0;
            c->scanOffsetEC = 0;
            return 0;
        }
        c->scanSeenEE = 0;
        c->scanIndexEB = 0;
        c->phase = 0xFF;
        LoadActInfo(c);
        c->phase = 2;
        return 0;
    default:
        return 1;
    }
}

/* ---- padpatch.c ---- */

static void SendActInfoCmd(Channel* c) {
    switch (c->phase) {
    case 2:
        CmdWithArgument(c, 0x4C, c->state47);
        break;
    case 3:
        CmdWithArgument(c, 0x46, c->state47);
        break;
    case 4:
        if (c->flag48 == 0)
            CmdWithArgument(c, 0x47, c->state47);
        else
            SetCommand(c, 0x4B, NULL, 0);
        break;
    }
}

static int RecvActInfoReply(Channel* c) {
    const u8* q = c->packet;
    switch (c->phase) {
    case 2: {
        unsigned mode;
        if (q[2] != 0 || q[3] != 0)
            return 0;
        SetModeAt(c, c->state47, q[5] + (q[4] << 8));
        mode = ModeAt(c, c->state47);
        if (c->scanSeenEE != mode) {
            c->scanSeenEE = (u16)mode;
            return 0;
        }
        c->scanSeenEE = 0;
        c->scanIndexEB = 0;
        if (++c->state47 < c->modeCountE3)
            return 0;
        c->state47 = 0;
        return 1;
    }
    case 3: {
        u8* e;
        if (q[2] != 0 || q[3] != 0)
            return 0;
        e = EntryAt(c, c->state47);
        c->scanSeenEE = e[0] == q[4] && e[1] == (q[5] & 0x7F) &&
                                e[2] == q[6] && e[3] == q[7] && e[4] == q[5] >> 7
                            ? 0
                            : 0xFFFF;
        e[0] = q[4];
        e[1] = q[5] & 0x7F;
        e[2] = q[6];
        e[3] = q[7];
        e[4] = q[5] >> 7;
        if (c->scanSeenEE != 0)
            return 0;
        c->scanIndexEB = 0;
        if (++c->state47 < c->entryCountE9)
            return 0;
        c->state47 = 0;
        c->flag48 = 0;
        return 1;
    }
    case 4: {
        u8* comb;
        const u8* src;
        const u8* limit = &c->arena[ARENA_BYTES];
        int i;
        if (q[2] != 0) {
            c->flag48 = 0;
            return 0;
        }
        comb = CombAt(c, c->state47);
        if (c->flag48 == 0) {
            int buttons;
            i = 3;
            comb[0] = c->flag48 = q[4];
            src = &q[5];
            if (c->state47 == 0) {
                buttons = c->combs + COMB_BYTES * c->combCountEA;
            } else {
                buttons = c->comb_buttons[c->state47 - 1] +
                          ((CombAt(c, c->state47 - 1)[0] + 3) & 0x1FC);
            }
            c->comb_buttons[c->state47] = buttons;
            scan_ptr = &c->arena[buttons < ARENA_BYTES ? buttons : ARENA_BYTES];
        } else {
            i = 5;
            src = &q[3];
        }
        while (i-- != 0) {
            if (c->flag48 == 0)
                break;
            if (scan_ptr >= limit) {
                c->state47 = 0;
                c->flag48 = 0;
                return 0;
            }
            if (*scan_ptr != *src)
                c->scanSeenEE = 0xFFFF;
            *scan_ptr++ = *src++;
            c->flag48--;
        }
        if (c->flag48 == 0) {
            if (c->scanSeenEE != 0) {
                c->scanSeenEE = 0;
                c->flag48 = 0;
                return 0;
            }
            if (++c->state47 >= c->combCountEA) {
                c->state49 = 6;
                c->phase = 0xFE;
                c->scanIndexEB = 0;
                return 0;
            }
            c->flag48 = 0;
            c->scanIndexEB = 0;
        }
        return 0;
    }
    default:
        return 1;
    }
}

static void ActAlignUpdate(Channel* c) { SetCommand(c, 0x4D, c->layout, 6); }

static int ActAlignComplete(Channel* c) {
    for (int group = 0; group < c->entryCountE9; group++) {
        int remaining = 0, minimum;
        for (int i = 0; i < 6; i++) {
            remaining += c->layout[i] == group;
        }
        minimum = EntryAt(c, group)[2];
        if (minimum == 0)
            minimum = 1;
        for (int i = 0; i < 6; i++) {
            if (c->layout[i] == group) {
                if (remaining < minimum) {
                    PAYLOAD(c)[i] = 0xFF;
                    remaining--;
                } else {
                    PAYLOAD(c)[i] = (u8)group;
                }
            }
        }
    }
    c->phase = 0xFE;
    return 0;
}

/* ---- padstop2.c: the command for the next exchange ---- */

static void Dispatch(Channel* c) {
    if (c->packet[0] == 0xF3) {
        if (!c->responseE8 || c->phase == 0xFF) {
            CmdParaMode(c, 0);
            return;
        }
        if (c->state49 == 2)
            ResetChannel(c);
    }
    switch (c->phase) {
    case 1:
        CmdParaMode(c, 1);
        return;
    case 0xFE:
        CmdParaMode(c, 0);
        return;
    case 0:
    case 0xFF:
        return;
    default:
        if (c->update)
            c->update(c);
        else
            SendAtLoadInfo(c);
    }
}

/* ---- padtmchk.c: a packet reaches the receive buffer ---- */

static void PacketDone(Channel* c) {
    unsigned t = c->packet[0];
    int previous;
    u8 b;
    if ((t & 0xF0) == 0) {
        c->input[0] = 0xFF;
        c->input[1] = 0;
        c->responseE8 = 0;
        c->pad35 = 0;
        ResetChannel(c);
        return;
    }
    previous = c->responseE8;
    c->responseE8 = (u8)(t >> 4);
    if (t >> 4 == 0xF) {
        c->responseE8 = (u8)previous; // configuration mode: not stored
    } else {
        c->input[0] = 0;
        c->input[1] = c->packet[0];
        c->pad35 = c->b44;
        for (int i = 2; i < c->b44 && i < PSYZ_PAD_BUF_LEN; i++)
            c->input[i] = c->packet[i];
    }
    if (c->packet[1] != 0 || (c->phase == 1 && c->update == NULL) || c->b50) {
        if (c->responseE8 != previous)
            ResetChannel(c);
    } else {
        ResetChannel(c);
    }
    b = c->phase;
    c->retry4A = 0;
    if (b == 0xFF)
        return;
    if (b != 0 && c->command37 == 0)
        return;
    if ((u8)(b - 2) < 0xFC && c->packet[0] != 0xF3) {
        ResetChannel(c);
        return;
    }
    switch (c->phase) {
    case 0:
        c->state49 = 1;
        c->phase++;
        break;
    case 1:
        c->state47 = 0;
        c->phase++;
        break;
    case 0xFE:
        c->phase = 0xFF;
        break;
    default: {
        // the callback may set the phase; its result is added after it
        int step = c->complete ? c->complete(c) : RecvAtLoadInfo(c);
        c->phase = (u8)(c->phase + step);
        break;
    }
    }
}

/* An exchange that failed. */
static void FailAuto(Channel* c) {
    c->type4C++;
    if (c->phase != 0) {
        if (c->phase == 1) {
            if (c->retry4A >= 0xB) {
                c->state49 = 2;
                c->phase = 0xFF;
                return;
            }
            c->retry4A++;
            return;
        }
        if (c->retry4A < 0xB) {
            c->retry4A++;
            return;
        }
        if (c->state49 != 0)
            ResetChannel(c);
    }
    if (c->packet[0] != 0xF3) {
        c->input[0] = 0xFF;
        c->input[1] = 0;
        c->responseE8 = 0;
        c->pad35 = 0;
    }
}

/* ---- padchk.c: which actuator bytes go out ---- */

static void AllocGates(Channel* c) {
    u8* gates = GATES(c);
    memset(gates, 0, 6);
    if (c->pending && c->data28) {
        int length = c->length34 < 7 ? c->length34 : 6;
        for (int i = 0; i < c->entryCountE9; i++) {
            u8* e = EntryAt(c, i);
            int mask = e[2] ? 0xFF : 1;
            int found = 0;
            for (int j = 0; j < length; j++) {
                if (PAYLOAD(c)[j] == i && (c->data28[j] & mask)) {
                    found = 1;
                    break;
                }
            }
            if (found && gate_cost + e[3] < 61) {
                gate_cost += e[3];
                for (int j = 0; j < length; j++) {
                    if (PAYLOAD(c)[j] == i)
                        gates[j] = 1;
                }
            }
        }
        return;
    }
    if (c->responseE8 - 4u < 2u || c->responseE8 == 7) {
        if (!c->pending && c->length34 >= 2 && c->data28) {
            // SCPH-1150 legacy motor
            if ((c->data28[0] & 0xC0) == 0x40 && (c->data28[1] & 1) &&
                gate_cost + 10 < 61) {
                gates[0] = gates[1] = 1;
                gate_cost += 10;
            }
            return;
        }
    }
    if (c->responseE8 == 3) {
        gates[0] = 1;
        return;
    }
    if (!c->pending)
        memset(gates, 1, 6);
}

/* ---- padrecv.c / padpoll.c: one byte each way ---- */

/* _padSioRW: needs the ACK of the byte before; stores its reply. */
static int SioRW(Channel* c, u8 out) {
    int in = rx_latch;
    if (!ack_latch)
        return -0x14;
    Send(out);
    c->b45++;
    c->packet[c->b44++] = (u8)in;
    return in;
}

/* _padSioRW2: the same in the receive stages; the reply to the address
 * byte is not stored. */
static int SioRW2(Channel* c, u8 out) {
    int in = rx_latch;
    if (!ack_latch)
        return -2;
    Send(out);
    c->b45++;
    if (c->b44 != 0xFF)
        c->packet[c->b44] = (u8)in;
    c->b44++;
    return in;
}

/* ---- padopen.c _padInitSioMode: a waiting packet of this port ---- */

static void InitSioMode(Channel* c, int port) {
    if (slot_index[port] == 0) {
        slot_index[port] = -1;
        PacketDone(c);
        SaveCommand(c);
    }
}

/* ---- padinit2.c: the receive stages and the packet ---- */

static int Exchange(Channel* c, int port) {
    Channel* other = &channels[port ^ 1];
    int r;

    // stage 0: the command for this exchange, the address byte
    Dispatch(c);
    c->packet[0] = 0;
    c->b44 = 0xFF;
    c->b45 = 1;
    Send(0x01);
    // stage 1: the command byte
    r = SioRW2(c, c->command37 ? c->command37 : 0x42);
    if (r < 0)
        return r;
    // stage 2: the ID; the Multi Tap byte
    r = SioRW2(c, 0);
    if (r < 0) {
        // a command the controller does not know (a digital pad's 0x43
        // while libpad probes for analog modes) is not acknowledged
        return r;
    }
    bytes_left = (r & 0xF) * 2;
    if (bytes_left == 0)
        bytes_left = 0x20;
    // stage 3: 0x5A; the first data byte, through the gates before
    r = SioRW2(c, TxByte(c));
    if (r < 0)
        return r;
    if (r != 0x5A && r != 0)
        return -4;
    // _padTransferPacket
    AllocGates(c);
    while (bytes_left >= 2) {
        if (slot_index[port ^ 1] < 0)
            break;
        if (slot_index[port ^ 1] == 0) {
            PacketDone(other);
            SaveCommand(other);
            slot_index[port ^ 1] = -1;
        }
        r = SioRW(c, TxByte(c));
        if (r < 0)
            return r;
        bytes_left--;
    }
    while (--bytes_left > 0) {
        r = SioRW(c, TxByte(c));
        if (r < 0)
            return r;
    }
    if (c->b44 < sizeof(c->packet))
        c->packet[c->b44++] = rx_latch;
    return 0;
}

static int Trace(void) {
    static int trace = -1;
    if (trace < 0) {
        const char* v = getenv("PSYZ_PAD_TRACE");
        trace = v && *v == '1';
    }
    return trace;
}

/* padenab.c _padIntPad: libpad's vblank handler. */
static void IntPad(void) {
    vsync_flag = 1;
    if (first_channel && timeout_counts[0] < 150)
        timeout_counts[0]++;
    if (!last_channel && timeout_counts[1] < 150)
        timeout_counts[1]++;
    if (!com_active || last_channel < first_channel)
        return;
    gate_cost = 0;
    for (active_channel = first_channel; active_channel <= last_channel;
         active_channel++) {
        int port = active_channel, r;
        Channel* c = &channels[port];
        InitSioMode(c, port);
        bus_port = port;
        r = Exchange(c, port);
        if (Trace()) {
            fprintf(stderr,
                    "libpad: port %d command %02x result %d packet %02x %02x "
                    "phase %02x state %d\n",
                    port + 1, c->command37 ? c->command37 : 0x42, r,
                    c->packet[0], c->packet[1], c->phase, c->state49);
        }
        if (r < 0) {
            FailAuto(c);
            SaveCommand(c);
        } else {
            slot_index[port] = 0;
        }
        Psyz_PadsDeselect(port);
    }
}

void Psyz_LibpadVBlank(void) {
    if (com_started)
        IntPad();
}

int Psyz_PadDriverRunning(void) { return com_started; }

/* ---- the library's entry points ---- */

static Channel* PortToChannel(int selector) {
    return &channels[(selector & 0xF0) ? 1 : 0];
}

void PadInitDirect(u_char* pad1, u_char* pad2) {
    PadInit(0); // the host's controllers
    com_active = 0;
    memset(channels, 0, sizeof(channels));
    for (int i = 0; i < 2; i++) {
        Channel* c = &channels[i];
        u_char* buf = i ? pad2 : pad1;
        c->input = buf ? buf : dummy_input[i];
        c->modes = c->entries = c->combs = -1;
        c->input[0] = 0xFF;
        c->input[1] = 0;
        memset(PAYLOAD(c), 0xFF, 6);
    }
    com_active = 1;
}

void PadStartCom(void) {
    com_started = 1;
    ResetChannel(&channels[0]);
    ResetChannel(&channels[1]);
    timeout_counts[0] = timeout_counts[1] = 0;
    com_active = 1;
}

void PadStopCom(void) { com_started = 0; }

int PadChkVsync(void) {
    int previous = vsync_flag;
    vsync_flag = 0;
    return previous;
}

unsigned PadEnableCom(unsigned mode) {
    unsigned previous = (last_channel * 2) | (first_channel == 0);
    if (previous == mode)
        return previous;
    com_active = 0;
    if (mode & 1) {
        first_channel = 0;
        if (timeout_counts[0] >= 150)
            ResetChannel(&channels[0]);
        timeout_counts[0] = 0;
    } else {
        first_channel = 1;
    }
    if (mode & 2) {
        last_channel = 1;
        if (timeout_counts[1] >= 150)
            ResetChannel(&channels[1]);
        timeout_counts[1] = 0;
    } else {
        last_channel = 0;
    }
    com_active = 1;
    return previous;
}

int PadGetState(int port) {
    Channel* c = PortToChannel(port);
    if (c->command37 == 0 && c->previous38 == 0 && c->input[0] == 0)
        return c->state49;
    switch (c->state49) {
    case 2:
    case 3:
        return 1;
    case 6:
        return 4;
    }
    return c->state49;
}

int PadInfoMode(int port, int term, int offs) {
    Channel* c = PortToChannel(port);
    switch (term) {
    case 1: // InfoModeCurID
        return c->responseE8;
    case 2: // InfoModeCurExID
        return c->pending;
    case 3: // InfoModeCurExOffs
        return c->argumentE4;
    case 4: // InfoModeIdTable
        if (offs < 0)
            return c->modeCountE3;
        if (offs < c->modeCountE3 && c->modes >= 0)
            return (int)ModeAt(c, offs);
        return 0;
    case 100:
        return c->type4C;
    }
    return 0;
}

int PadInfoAct(int port, int actno, int term) {
    Channel* c = PortToChannel(port);
    if (actno < 0)
        return c->entryCountE9;
    if (actno < c->entryCountE9 && c->entries >= 0 && term >= 1 && term <= 5)
        return EntryAt(c, actno)[term - 1];
    return 0;
}

int PadInfoComb(int port, int listno, int offs) {
    Channel* c = PortToChannel(port);
    const u8* comb;
    if (listno < 0)
        return c->combCountEA;
    if (listno >= c->combCountEA || c->combs < 0)
        return 0;
    comb = CombAt(c, listno);
    if (offs < 0)
        return comb[0];
    if (offs >= comb[0])
        return 0;
    {
        int at = c->comb_buttons[listno] + offs;
        return at < ARENA_BYTES ? c->arena[at] : 0;
    }
}

int PadSetActAlign(int port, char* data) {
    Channel* c = PortToChannel(port);
    if (ChannelBusy(c))
        return 0;
    c->phase = 1;
    c->update = ActAlignUpdate;
    c->layout = (const u8*)data;
    c->complete = ActAlignComplete;
    return 1;
}

int PadSetMainMode(int port, int offs, int lock) {
    Channel* c = PortToChannel(port);
    if (ChannelBusy(c))
        return 0;
    c->phase = 1;
    c->update = MainModeUpdate;
    c->complete = MainModeComplete;
    c->modeArgs51[0] = (u8)offs;
    c->modeArgs51[1] = (u8)lock;
    c->modeEqual53 = (u8)offs == c->argumentE4;
    return 1;
}

void PadSetAct(int port, u_char* data, int len) {
    Channel* c = PortToChannel(port);
    c->data28 = data;
    c->length34 = (u8)len;
}

void PadInitMtap(u_char* pad1, u_char* pad2) {
    (void)pad1;
    (void)pad2;
    NOT_IMPLEMENTED;
}
