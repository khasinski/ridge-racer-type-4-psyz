#include <stdio.h>
#include <psyz.h>
#include <libapi.h>
#include <psyz/log.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <libetc.h>
#include <psyz/system.h>
#include "../internal.h"
#include "../timing_profile.h"

// 1:valid, 0:invalid
static inline int validate_chan(long chan) {
    unsigned int cardNo = (unsigned int)(chan & 15);
    unsigned int portNo = (unsigned int)(chan >> 4);
    if (cardNo >= 4) { // validate multi-tap card support
        return 0;
    }
    if (portNo >= 2) { // PS1 has only two memcard ports
        return 0;
    }
    return 1;
}

long _card_auto(long val) {
    NOT_IMPLEMENTED;
    return val;
}

static long last_chan;

/* Cards live as host directories (bu00, bu10) and are plugged in and
 * formatted unless the host says otherwise: PSYZ_CARD_UNPLUG and
 * PSYZ_CARD_UNFORMATTED list slots (1, 2) that are empty or hold an
 * unformatted card, e.g. PSYZ_CARD_UNPLUG=2; Psyz_CardSetSlotState does the
 * same at run time. Formatting a card makes it formatted. There is no
 * multi-tap, so its extra slots time out like an empty port. */
static int card_slot_state[2] = {-1, -1};
static int card_new[2] = {1, 1}; // the cards' "new card" FLAG bit (below)

static int card_env_lists(const char* name, int slot) {
    const char* list = getenv(name);
    if (!list) {
        return 0;
    }
    for (; *list; list++) {
        if (*list == '0' + slot) {
            return 1;
        }
    }
    return 0;
}

int Psyz_CardSlotState(int port) {
    if (port < 0 || port > 1) {
        return PSYZ_CARD_NONE;
    }
    if (card_slot_state[port] < 0) {
        card_slot_state[port] =
            card_env_lists("PSYZ_CARD_UNPLUG", port + 1) ? PSYZ_CARD_NONE
            : card_env_lists("PSYZ_CARD_UNFORMATTED", port + 1)
                ? PSYZ_CARD_UNFORMATTED
                : PSYZ_CARD_FORMATTED;
    }
    return card_slot_state[port];
}

void Psyz_CardSetSlotState(int port, int state) {
    if (port >= 0 && port <= 1) {
        if (state != PSYZ_CARD_NONE && state != Psyz_CardSlotState(port)) {
            card_new[port] = 1; // another card: its FLAG says new
        }
        card_slot_state[port] = state;
    }
}

static int card_slot_at_blank[2] = {-1, -1};

void Psyz_CardSetSlotStateAtBlank(int port, int state) {
    if (port >= 0 && port <= 1) {
        card_slot_at_blank[port] = state;
    }
}

void Psyz_CardBlankBegin(void) {
    for (int port = 0; port < 2; port++) {
        if (card_slot_at_blank[port] >= 0) {
            Psyz_CardSetSlotState(port, card_slot_at_blank[port]);
            card_slot_at_blank[port] = -1;
        }
    }
}

static int card_is_instant(void);
static uint64_t card_fields_us(unsigned fields);

/* BIOS card driver timing (measured on DuckStation with the SCPH-1001 BIOS
 * and Ridge Racer Type 4, PAL). After StartCARD the driver's vblank handler
 * alternates between the two ports, one per vblank, and starts the transfer
 * a port has pending on that port's vblank: a sector read or write, which
 * the card answers within the field, so back-to-back requests on one port
 * take two fields each. A port without a card leaves the transfer
 * unanswered: the next vblank times it out (EvSpTIMOUT) instead of switching
 * ports, which shifts the alternation by one field.
 *
 * Requests count transfers: _card_info, _card_read, _card_write and
 * _card_clear are one; _card_load reads sector 0 and, when it holds a
 * directory, the directory and broken-sector frames 1..35 (36), and
 * _bu_init also writes sector 0x3F (the new-card flag) after sector 0 (37).
 * A finished request raises its spec on SwCARD and HwCARD at that vblank
 * (before the game's vblank callbacks, as the BIOS handler runs first);
 * _card_load parses the directory in the handler and raises it at the
 * next vblank, which therefore does not switch ports. _bu_init waits for
 * both ports in turn and raises nothing: a blank card in port 1 and an
 * empty port 2 take 76 fields, an unformatted card in port 1 four and two
 * empty ports five. Psyz_CardSetInstantTiming(1) / PSYZ_CARD_TIMING=instant
 * completes every request at once.
 *
 * A 128-byte frame read or write ends when the card has answered its last
 * byte, CARD_SECTOR_CLOCKS after the vblank that started it (DuckStation: a
 * FileRead's last frame raises its event 266.3k system clocks after that
 * vblank). A BIOS file service waiting for such a transfer (firstfile's
 * sector 0 check, a synchronous file read or write) returns then, and an
 * FASYNC file transfer raises its event then. When _bu_init finds a port
 * without a card it clears that port's directory cache (a ROM bzero of its
 * 15 entries and the code around it: 74.2k clocks after the timeout). The
 * handler's bookkeeping when a request ends (its events undelivered and
 * raised) adds 5.4k clocks to that interrupt. */
enum {
    CARD_LOAD_TRANSFERS = 36,
    CARD_BU_INIT_TRANSFERS = 37,
}; // CARD_SECTOR_CLOCKS and the other times: timing_profile.h

enum {
    CARD_RAISE_NONE,
    CARD_RAISE_BOTH, // SwCARD and HwCARD (the driver's own requests)
    CARD_RAISE_SW,   // SwCARD only (file I/O)
};

typedef struct {
    long chan;
    int transfers; // still to make; 0: no request
    int raise;     // CARD_RAISE_*: where the result is raised when done
    int load;      // an unformatted card answers EvSpNEW
    int parse;     // the directory is parsed a vblank after the last read
    int sector;    // the last transfer moves a frame (CARD_SECTOR_CLOCKS)
    int answering; // that frame is still on the wire
    int kind;      // CARD_KIND_*: an info, a write
} CardRequest;

/* A card's FLAG byte has a "new card" bit, set when the card is powered or
 * inserted and cleared by its first write: _card_info on such a card
 * answers EvSpNEW, and the game then clears it (_card_clear writes frame
 * 0x3F) and loads the directory. _bu_init writes 0x3F after reading a
 * formatted card's directory; an unformatted card keeps the bit
 * (DuckStation's FLAG, DUCKSTATION_CARD_TRACE: a blank card's first command
 * sends 0x08, a write clears it; an unformatted card is written first by the
 * game's _card_clear). */
enum { CARD_KIND_INFO = 1, CARD_KIND_WRITE = 2 };
static int card_next_kind; // the kind of the next request (card_request_sectors)

static struct {
    int running;      // StartCARD .. StopCARD
    int port;         // the port served at the last vblank
    int timeout_port; // an unanswered transfer, timed out at the next vblank
    int parse_port;   // a directory read, finished at the next vblank
    int raw_port;     // the raw command's port (-1: none)
    int raw_state;    // 0 none, 1 waiting, 2 transferred, 3 answered
    uint64_t last_vblank_us;
    CardRequest req[2];
} card_drv = {0, 0, -1, -1, -1, 0, 0, {{0}}};

// PSYZ_CARD_TRACE=1 logs the driver's work at each vblank.
static int card_trace(void) {
    static int trace = -1;
    if (trace < 0) {
        const char* env = getenv("PSYZ_CARD_TRACE");
        trace = env && *env && *env != '0';
    }
    return trace;
}

static int card_chan_state(long chan) {
    return (chan & 15) ? PSYZ_CARD_NONE : Psyz_CardSlotState((int)(chan >> 4));
}

static void card_raise(unsigned spec) {
    Psyz_KernelRaise(SwCARD, spec);
    Psyz_KernelRaise(HwCARD, spec);
}

static unsigned card_result(const CardRequest* r) {
    int state = card_chan_state(r->chan);
    return state == PSYZ_CARD_NONE                    ? EvSpTIMOUT
           : r->load && state == PSYZ_CARD_UNFORMATTED ? EvSpNEW
           : (r->kind & CARD_KIND_INFO) && card_new[(r->chan >> 4) & 1] ? EvSpNEW
                                                       : EvSpIOE;
}

static void card_finish(int port, unsigned spec) {
    CardRequest* r = &card_drv.req[port];
    int raise = r->raise;
    int state = card_chan_state(r->chan);
    if ((r->kind & CARD_KIND_WRITE) && state != PSYZ_CARD_NONE &&
        !(r->load && state == PSYZ_CARD_UNFORMATTED)) {
        card_new[port] = 0;
    }
    r->transfers = 0;
    r->answering = 0;
    Psyz_KernelInterruptCycles(CARD_FINISH_ISR_CLOCKS);
    if (raise == CARD_RAISE_SW) {
        Psyz_KernelRaise(SwCARD, spec);
    } else if (raise) {
        card_raise(spec);
    }
}

static uint64_t card_clocks_us(uint64_t clocks) {
    return (clocks * 1000000 + 33868800 / 2) / 33868800;
}

static void card_sector_done(int port) {
    CardRequest* r = &card_drv.req[port];
    if (r->answering) {
        card_finish(port, card_result(r));
    }
}
static void card_sector_done0(void) { card_sector_done(0); }
static void card_sector_done1(void) { card_sector_done(1); }

// The BIOS card handler's work at one vblank; a vblank it misses (turn 0)
// leaves its state, and its port, as they are.
/* The vblank where a raw access (Psyz_CardRawBegin) ends: the driver's
 * handler takes the SIO there and libpad makes no exchange (DuckStation:
 * that vblank's exception is 4.9k clocks, libpad's flag stays clear). */
int Psyz_CardRawEndsAt(int turn) {
    return card_drv.running && turn && card_drv.raw_state == 2;
}

/* The vblank where the driver times out a transfer to a port without a
 * card: its handler resets the SIO there too, and libpad makes no exchange
 * (DuckStation: every select of an empty slot in the card scenarios, 82 in
 * Slot 2 Only and 4 after the card is pulled out, is followed by a vblank
 * without libpad's exchange). */
int Psyz_CardTimeoutAt(int turn) {
    return card_drv.running && turn && card_drv.timeout_port >= 0 &&
           card_drv.parse_port < 0;
}

void Psyz_CardVBlank(int turn) {
    if (!card_drv.running) {
        return;
    }
    card_drv.last_vblank_us = Psyz_KernelNowUs();
    if (!turn) {
        return;
    }
    /* A raw command (Psyz_CardRawBegin: libmcx's PocketStation 0x5A) runs
     * on libmcx's own vblank handler, not on the BIOS driver's port turns:
     * it selects the card and sends the command at the first vblank after
     * the call, whichever port the driver serves there, and a plain memory
     * card does not answer it, so the handler times it out at the next
     * vblank (DuckStation, DUCKSTATION_CARD_TRACE: the unformatted
     * scenario's CARDOP 5a at port vsync 270 with the BIOS driver on port
     * 1, libpad's exchange missing at 271; the Save and Load scenario's
     * probe at 5900-5901, the driver's port 1 then 0), a new card's too. */
    if (card_drv.raw_state == 2) {
        card_drv.raw_state = 3;
    } else if (card_drv.raw_state == 1) {
        card_drv.raw_state = 2;
    }
    if (card_drv.parse_port >= 0) {
        // the parse's vblank is the other port's turn too (DuckStation
        // selects port 0 again two turns after its last read)
        int port = card_drv.parse_port;
        card_drv.parse_port = -1;
        card_drv.port ^= 1;
        card_finish(port, card_result(&card_drv.req[port]));
        return;
    }
    if (card_drv.timeout_port >= 0) {
        int port = card_drv.timeout_port;
        card_drv.timeout_port = -1;
        if (card_trace()) {
            fprintf(stderr, "card: vblank port %d times out\n", port);
        }
        card_finish(port, EvSpTIMOUT);
        return;
    }
    card_drv.port ^= 1;
    int port = card_drv.port;
    if (card_trace()) {
        fprintf(stderr, "card: vblank port %d transfers %d raw %d\n", port,
                card_drv.req[port].transfers, card_drv.raw_state);
    }
    CardRequest* r = &card_drv.req[port];
    if (!r->transfers || r->answering) {
        return;
    }
    int state = card_chan_state(r->chan);
    if (state == PSYZ_CARD_NONE) {
        card_drv.timeout_port = port;
        return;
    }
    if (r->load && state == PSYZ_CARD_UNFORMATTED) {
        /* sector 0 holds no directory: the load ends with that frame,
         * once the card has answered it (DuckStation: an unformatted
         * card's _card_load, CARDREAD 0 and its CARDEND 267k clocks after
         * the vblank, the game's next request two fields later) */
        r->transfers = 1;
        r->parse = 0;
        r->sector = 1;
    }
    if (--r->transfers > 0) {
        return;
    }
    if (r->parse) {
        r->transfers = 1; // busy until the directory is parsed
        card_drv.parse_port = port;
    } else if (r->sector && state != PSYZ_CARD_NONE) {
        r->transfers = 1; // busy until the card has answered the frame
        r->answering = 1;
        Psyz_KernelPostAt(card_drv.last_vblank_us +
                              card_clocks_us(CARD_SECTOR_CLOCKS),
                          port ? card_sector_done1 : card_sector_done0);
    } else {
        card_finish(port, card_result(r));
    }
}

/* A PS1 program spinning on the card driver: spends a scanline of PS1 time.
 * Blanks normally come from that time (the offline clock); a clock that
 * does not raise them in a wait (real time, or no audio clock yet) gets the
 * driver's vblank after a field and a half. */
static void card_spin(void) {
    uint64_t before = Psyz_KernelNowUs();
    Psyz_SpendTime(CARD_SPIN_NS);
    uint64_t now = Psyz_KernelNowUs();
    // half a field of slack: a PAL field of the clock can end a spin late
    if (now == before ||
        now - card_drv.last_vblank_us > card_fields_us(1) * 3 / 2) {
        Psyz_CardVBlank(1);
        card_drv.last_vblank_us = now;
    }
}

static long card_request_sectors(long chan, int transfers, int raise, int load,
                                 int parse, int sector) {
    if (!validate_chan(chan)) {
        return 0;
    }
    last_chan = chan;
    int kind = card_next_kind;
    card_next_kind = 0;
    if (card_is_instant()) {
        CardRequest r = {chan, 0, raise, load, 0, 0, 0, kind};
        if ((kind & CARD_KIND_WRITE) && card_chan_state(chan) == PSYZ_CARD_FORMATTED) {
            card_new[(chan >> 4) & 1] = 0;
        }
        if (raise == CARD_RAISE_SW) {
            Psyz_KernelRaise(SwCARD, card_result(&r));
        } else if (raise) {
            card_raise(card_result(&r));
        }
        return 1;
    }
    CardRequest* r = &card_drv.req[chan >> 4];
    if (r->transfers) {
        return 0; // the port is busy
    }
    r->chan = chan;
    r->transfers = transfers;
    r->raise = raise;
    r->load = load;
    r->parse = parse;
    r->sector = sector;
    r->answering = 0;
    r->kind = kind;
    return 1;
}

static long card_request(long chan, int transfers, int raise, int load, int parse) {
    return card_request_sectors(chan, transfers, raise, load, parse, 0);
}

/* The PS1 program sees the request end once the exception that ended it
 * has returned (the vblank's, a sector's): its spin resumes there, not
 * within the exception (DuckStation: the BIOS's file creation returns
 * 18.1k clocks after the vblank exception of its last turn ends, whatever
 * that exception's length). */
static void card_wait(int port) {
    int spun = 0;
    while (card_drv.running && card_drv.req[port].transfers) {
        card_spin();
        spun = 1;
    }
    if (spun) {
        uint64_t end = Psyz_KernelExceptionEndNs(NULL);
        if (end > Psyz_KernelNowNs()) {
            Psyz_KernelWaitUntilNs(end);
        }
    }
}

long _card_info(long chan) {
    card_next_kind = CARD_KIND_INFO;
    return card_request(chan, 1, CARD_RAISE_BOTH, 0, 0);
}

long _card_load(long chan) {
    return card_request(chan, CARD_LOAD_TRANSFERS, CARD_RAISE_BOTH, 1, 1);
}

void _bu_init(void) {
    // The card directories, where the host keeps them (Psyz_AdjustPathCB;
    // by default bu00 and bu10 in the working directory), are created here.
    char path[4096];
    Psyz_AdjustPath(path, "bu00:*", sizeof(path));
    Psyz_AdjustPath(path, "bu10:*", sizeof(path));
    if (card_is_instant() || !card_drv.running) {
        return;
    }
    for (int port = 0; port < 2; port++) {
        card_next_kind = CARD_KIND_WRITE; // frame 0x3F after the directory
        if (card_request((long)port << 4, CARD_BU_INIT_TRANSFERS,
                         CARD_RAISE_NONE, 1, 0)) {
            card_wait(port);
            if (Psyz_CardSlotState(port) == PSYZ_CARD_NONE) {
                Psyz_SpendCycles(CARD_BU_INIT_EMPTY_PORT_CLOCKS);
            }
        }
    }
}

void Psyz_CardSyncTransfer(long chan) {
    if (card_request_sectors(chan, 1, CARD_RAISE_NONE, 0, 0, 1) &&
        !card_is_instant()) {
        card_wait((int)(chan >> 4));
    }
}

int Psyz_CardRawBegin(long chan) {
    if (!validate_chan(chan) || card_drv.raw_state) {
        return 0;
    }
    card_drv.raw_port = (int)(chan >> 4);
    card_drv.raw_state = card_is_instant() || !card_drv.running ? 3 : 1;
    return 1;
}

int Psyz_CardRawSync(int wait) {
    if (!card_drv.raw_state) {
        return -1;
    }
    while (wait && card_drv.raw_state != 3) {
        card_spin();
    }
    if (card_drv.raw_state != 3) {
        return 0;
    }
    card_drv.raw_state = 0;
    card_drv.raw_port = -1;
    return 1;
}

void _new_card(void) {}

static void card_spend_fields(unsigned fields);

/* Formatting writes and reads back frames 1..0x23 (directory and broken
 * frame list) and then frame 0, a frame per turn of the port: 72 transfers
 * from the port's first turn after the call, synchronously (DuckStation,
 * DUCKSTATION_CARD_TRACE: frames written at 3048..3189 and read at
 * 3050..3191 for a call at 3046). Without a running driver it takes their
 * fields on the clock. The card's directory becomes empty. There is no
 * multi-tap. */
enum { CARD_FORMAT_TRANSFERS = 2 * 36 };

long _card_format(long chan) {
    char fs[] = "bu00:";
    if (!validate_chan(chan) || (chan & 15)) {
        return 0;
    }
    if (Psyz_CardSlotState((int)(chan >> 4)) == PSYZ_CARD_NONE) {
        return 0;
    }
    fs[2] = (char)('0' + (chan >> 4));
    if (!card_is_instant()) {
        card_next_kind = CARD_KIND_WRITE;
        if (card_drv.running &&
            card_request_sectors(chan, CARD_FORMAT_TRANSFERS, CARD_RAISE_NONE, 0, 0, 1)) {
            card_wait((int)(chan >> 4));
        } else {
            card_next_kind = 0;
            card_spend_fields(CARD_FORMAT_TRANSFERS * 2); // two fields a transfer
        }
    }
    if (Psyz_CardSlotState((int)(chan >> 4)) == PSYZ_CARD_NONE || !format(fs)) {
        return 0;
    }
    card_slot_state[chan >> 4] = PSYZ_CARD_FORMATTED;
    card_new[chan >> 4] = 0;
    return 1;
}

// 1 once the port's request is done; a caller polling it spins on the
// driver.
long _card_status(long drv) {
    if (drv < 0 || drv > 1 || !card_drv.req[drv].transfers) {
        return 1;
    }
    card_spin();
    return card_drv.req[drv].transfers ? 0 : 1;
}

long _card_wait(long drv) {
    if (drv >= 0 && drv <= 1) {
        card_wait((int)drv);
    }
    return 1;
}

unsigned long _card_chan(void) { return (unsigned long)last_chan; }

static void card_driver_reset(int running) {
    Psyz_KernelCancel(card_sector_done0);
    Psyz_KernelCancel(card_sector_done1);
    memset(card_drv.req, 0, sizeof(card_drv.req));
    card_drv.running = running;
    card_drv.port = 0;
    card_drv.timeout_port = -1;
    card_drv.parse_port = -1;
    card_drv.raw_port = -1;
    card_drv.raw_state = 0;
    card_drv.last_vblank_us = Psyz_KernelNowUs();
}

/* InitCARD installs the BIOS card driver's handlers in the interrupt
 * chains: every exception runs its test from then on (kernel.c). */
static int card_chain;
int Psyz_CardChainInstalled(void) { return card_chain; }
int Psyz_CardDriverRunning(void) { return card_drv.running; }

void InitCARD2(long val) {
    (void)val;
    card_driver_reset(0);
    card_chain = 1;
}

long StartCARD2(void) {
    card_driver_reset(1);
    return 1;
}

long StopCARD2(void) {
    card_driver_reset(0);
    return 1;
}

void _ExitCard(void) {}

static void _bzero(unsigned char* p, int n) { memset(p, 0, n); }

long _card_sector_write(long chan, long block, unsigned char* buf) {
    unsigned char sp10[0x80];
    u8 checksum;
    s32 retries;
    s32 i;
    u8* var_v1;
    u8* var_v1_2;

    retries = 0;
    var_v1 = buf;
    checksum = 0;
    for (i = 0; i < 0x7F; i++) {
        checksum ^= *var_v1++;
    }
    *var_v1 = checksum;
    while (1) {
        if (retries < 8) {
            _new_card();
            if (_card_write(chan, block, buf) == 1) {
                do {
                } while (!(_card_status(chan >> 4) & 1));
                var_v1_2 = sp10;
                _bzero(var_v1_2, sizeof(sp10));
                _new_card();
                if (_card_read(chan, block, var_v1_2) != 1) {
                    ERRORF("card read error\n");
                } else {
                    do {
                    } while (!(_card_status(chan >> 4) & 1));
                }
                var_v1 = sp10;
                checksum = 0;
                for (i = 0; i < 0x7F; i++) {
                    checksum ^= *var_v1++;
                }
                if (buf[0x7F] == checksum) {
                    return 1;
                }
                retries++;
            } else {
                return 0;
            }
        } else {
            break;
        }
    }
    return 0;
}

long _card_write(long chan, long block, unsigned char* buf) {
    LOG_ONCE("raw sectors are not emulated, only the completion event");
    (void)block;
    (void)buf;
    card_next_kind = CARD_KIND_WRITE;
    // its event comes when the card has taken the frame (DuckStation: the
    // unformatted card's _card_clear of frame 0x3F ends 241.9k clocks after
    // its vblank; frames written by the file layer 251k-269k)
    return card_request_sectors(chan, 1, CARD_RAISE_BOTH, 0, 0, 1);
}

long _card_read(long chan, long block, unsigned char* buf) {
    LOG_ONCE("raw sectors are not emulated, only the completion event");
    (void)block;
    (void)buf;
    return card_request_sectors(chan, 1, CARD_RAISE_BOTH, 0, 0, 1);
}

/* Memory card file timing ("bu" device). The BIOS moves a file's 128-byte
 * frames through the card driver, one transfer per turn of the port (every
 * other vblank), from the port's first turn after the call; a 1-block FASYNC
 * read raises SwCARD/EvSpIOE a sector time after the vblank of its 64th
 * transfer (DuckStation's card commands, DUCKSTATION_CARD_TRACE: Ridge Racer
 * Type 4 loading its save at boot and in Save and Load, frames 0x40..0x7F).
 * A write takes one transfer more. A synchronous transfer returns when
 * the last one has ended. Without a running driver (or with the port busy)
 * the transfer takes the same number of turns on the clock, and creating a
 * file 8 fields (with the driver: its turns, Psyz_CardFileCreate). The
 * BIOS file layer's own CPU time (open, an FASYNC transfer's start, close)
 * is spent too (BIOS_CARD_*).
 * Psyz_CardSetInstantTiming(1) or PSYZ_CARD_TIMING=instant completes card
 * I/O at once. */
enum { CARD_FIELDS_PER_FRAME = 2, CARD_FILE_EXTRA_TRANSFERS = 1, CARD_CREATE_FIELDS = 8 };

static int card_instant = -1;

void Psyz_CardSetInstantTiming(int instant) { card_instant = instant != 0; }

static int card_is_instant(void) {
    if (card_instant < 0) {
        const char* timing = getenv("PSYZ_CARD_TIMING");
        card_instant = timing && !strcmp(timing, "instant");
    }
    return card_instant;
}

static uint64_t card_fields_us(unsigned fields) {
    /* PS1 field: 3406 x 314 (PAL) or 3413 x 263 (NTSC) GPU clocks, the GPU
     * clock being 33.8688 MHz * 709379/451584 (PAL, 53.203425 MHz) or
     * * 715909/451584 (NTSC, 53.693175 MHz). */
    if (GetVideoMode() == MODE_PAL) {
        return (fields * 3406ull * 314 * 1000000 + 26601712) / 53203425;
    }
    return (fields * 3413ull * 263 * 1000000 + 26846587) / 53693175;
}

static int card_file_transfers(long bytes, int write) {
    return (int)((bytes + 127) / 128) + (write ? CARD_FILE_EXTRA_TRANSFERS : 0);
}

static void card_spend_fields(unsigned fields) {
    if (card_is_instant()) {
        return;
    }
    for (; fields; fields--) { // one field at a time keeps ns within 32 bits
        Psyz_SpendTime((unsigned)(card_fields_us(1) * 1000));
    }
}

static void card_async_done(void) { Psyz_KernelRaise(SwCARD, EvSpIOE); }

void Psyz_CardFileTransfer(long chan, long bytes, int async, int write) {
    if (card_is_instant()) {
        if (async) {
            card_async_done();
        }
        return;
    }
    int transfers = card_file_transfers(bytes, write);
    if (async) {
        Psyz_SpendCycles(write ? BIOS_CARD_WRITE_START_CYCLES : BIOS_CARD_READ_START_CYCLES);
    }
    if (card_drv.running && validate_chan(chan) &&
        (card_next_kind = write ? CARD_KIND_WRITE : 0,
         card_request_sectors(chan, transfers, async ? CARD_RAISE_SW : CARD_RAISE_NONE,
                              0, 0, 1))) {
        if (!async) {
            card_wait((int)(chan >> 4));
        }
        return;
    }
    uint64_t us = card_fields_us((unsigned)(transfers * CARD_FIELDS_PER_FRAME)) +
                  card_clocks_us(CARD_SECTOR_CLOCKS);
    if (async) {
        Psyz_KernelPostAt(Psyz_KernelNowUs() + us, card_async_done);
    } else {
        Psyz_SpendTime((unsigned)(card_clocks_us(CARD_SECTOR_CLOCKS) * 1000));
        card_spend_fields((unsigned)(transfers * CARD_FIELDS_PER_FRAME));
    }
}

/* Creating a file on a card with the driver running: on DuckStation
 * (DUCKSTATION_CARD_TRACE: Save and Load's save, port vsyncs 4406-4414;
 * the Garage's, 5335-5343) the BIOS reads directory frame 0 at the port's
 * next turn, probes the card at the one after, writes the file's directory
 * frame at the third, probes at the fourth and returns 12.5k clocks after
 * that probe's last SIO interrupt (BIOS_CARD_CREATE_END_CYCLES after the
 * turn's exception); the data's first frame goes out at the turn after.
 * The fixed 8 fields from the call put the game's next frame a field late
 * when the call came just before the port's turn (Save and Load); the four
 * turns without the BIOS's and the menu frame's CPU time put the Garage's
 * VSync(0) in the blank of an interlaced field, a field late too. */
enum { CARD_CREATE_TURNS = 4 };

void Psyz_CardFileCreate(long chan) {
    if (card_is_instant()) {
        return;
    }
    if (!card_drv.running || !validate_chan(chan)) {
        card_spend_fields(CARD_CREATE_FIELDS);
        return;
    }
    int port = (int)(chan >> 4);
    for (int i = 0; i < CARD_CREATE_TURNS; i++) {
        int frame = i % 2 == 0; // frame 0's read, the directory frame's write
        card_next_kind = i == 2 ? CARD_KIND_WRITE : 0;
        if (!card_request_sectors(chan, 1, CARD_RAISE_NONE, 0, 0, frame)) {
            break;
        }
        card_wait(port);
    }
    Psyz_SpendCycles(BIOS_CARD_CREATE_END_CYCLES);
}

/* DuckStation, the card pulled out during a save (DUCKSTATION_CARD_TRACE):
 * each of the game's three retries of open(create) selects the empty slot
 * once (port vsyncs 4475, 4478, 4483), times out at the next vblank and
 * fails. */
int Psyz_CardFileOpen(long chan, int mode) {
    if (!validate_chan(chan) || card_chan_state(chan) != PSYZ_CARD_NONE) {
        if (!card_is_instant() && validate_chan(chan) && !(mode & PSYZ_CARD_FILE_CREATE)) {
            Psyz_SpendCycles(mode & PSYZ_CARD_FILE_WRITE ? BIOS_CARD_OPEN_WRITE_CYCLES
                                                         : BIOS_CARD_OPEN_READ_CYCLES);
        }
        return 1;
    }
    Psyz_CardSyncTransfer(chan);
    return 0;
}

void Psyz_CardFileClose(long chan, int mode) {
    if (card_is_instant() || !validate_chan(chan)) {
        return;
    }
    Psyz_SpendCycles(mode & PSYZ_CARD_FILE_CREATE  ? BIOS_CARD_CLOSE_CREATED_CYCLES
                     : mode & PSYZ_CARD_FILE_WRITE ? BIOS_CARD_CLOSE_WRITE_CYCLES
                                                   : BIOS_CARD_CLOSE_READ_CYCLES);
}
