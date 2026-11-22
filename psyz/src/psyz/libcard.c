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

/* The BIOS card driver's requests complete at once: a card's directory and
 * files are host files, read and written inside the calls. A finished
 * request raises its result on SwCARD and HwCARD (the driver's own
 * requests) or SwCARD (a file transfer) before the call returns. */
enum { CARD_KIND_INFO = 1, CARD_KIND_WRITE = 2 };

static long card_request(long chan, int kind, int load, int hw) {
    if (!validate_chan(chan)) {
        return 0;
    }
    last_chan = chan;
    int port = (int)((chan >> 4) & 1);
    int state = (chan & 15) ? PSYZ_CARD_NONE : Psyz_CardSlotState(port);
    unsigned spec = state == PSYZ_CARD_NONE                  ? EvSpTIMOUT
                    : load && state == PSYZ_CARD_UNFORMATTED ? EvSpNEW
                    : (kind & CARD_KIND_INFO) && card_new[port] ? EvSpNEW
                                                              : EvSpIOE;
    /* A card's FLAG byte has a "new card" bit, set when the card is
     * inserted and cleared by its first write: _card_info on such a card
     * answers EvSpNEW, and the game then clears it (_card_clear writes frame
     * 0x3F) and loads the directory. */
    if ((kind & CARD_KIND_WRITE) && state == PSYZ_CARD_FORMATTED) {
        card_new[port] = 0;
    }
    Psyz_KernelRaise(SwCARD, spec);
    if (hw) {
        Psyz_KernelRaise(HwCARD, spec);
    }
    return 1;
}

long _card_info(long chan) { return card_request(chan, CARD_KIND_INFO, 0, 1); }

long _card_load(long chan) { return card_request(chan, 0, 1, 1); }

void _bu_init(void) {
    // The card directories, where the host keeps them (Psyz_AdjustPathCB;
    // by default bu00 and bu10 in the working directory), are created here.
    char path[4096];
    Psyz_AdjustPath(path, "bu00:*", sizeof(path));
    Psyz_AdjustPath(path, "bu10:*", sizeof(path));
    for (int port = 0; port < 2; port++) {
        if (Psyz_CardSlotState(port) == PSYZ_CARD_FORMATTED) {
            card_new[port] = 0; // it writes the formatted cards' FLAG
        }
    }
}

void _new_card(void) {}

/* Formatting empties the card's directory. There is no multi-tap. */
long _card_format(long chan) {
    char fs[] = "bu00:";
    if (!validate_chan(chan) || (chan & 15)) {
        return 0;
    }
    if (Psyz_CardSlotState((int)(chan >> 4)) == PSYZ_CARD_NONE) {
        return 0;
    }
    fs[2] = (char)('0' + (chan >> 4));
    if (!format(fs)) {
        return 0;
    }
    card_slot_state[chan >> 4] = PSYZ_CARD_FORMATTED;
    card_new[chan >> 4] = 0;
    return 1;
}

long _card_status(long drv) {
    (void)drv;
    return 1;
}

long _card_wait(long drv) {
    (void)drv;
    return 1;
}

unsigned long _card_chan(void) { return (unsigned long)last_chan; }

void InitCARD2(long val) { (void)val; }

long StartCARD2(void) { return 1; }

long StopCARD2(void) { return 1; }

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
    return card_request(chan, CARD_KIND_WRITE, 0, 1);
}

long _card_read(long chan, long block, unsigned char* buf) {
    LOG_ONCE("raw sectors are not emulated, only the completion event");
    (void)block;
    (void)buf;
    return card_request(chan, 0, 0, 1);
}

/* A memory card ("bu") file transfer of the BIOS file layer: the host file
 * I/O is done; an FASYNC one raises SwCARD/EvSpIOE, its end. */
void Psyz_CardFileIoDone(void) { Psyz_KernelRaise(SwCARD, EvSpIOE); }

/* A BIOS open of a card file on `chan`: whether a card answers (a card
 * taken out leaves its host directory, but the open fails). */
int Psyz_CardFileOpen(long chan) {
    return validate_chan(chan) && !(chan & 15) &&
           Psyz_CardSlotState((int)(chan >> 4)) != PSYZ_CARD_NONE;
}
