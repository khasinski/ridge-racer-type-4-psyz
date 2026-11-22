#include <common.h>
#include <libcd.h>
#include "libcd_lock.h"
#ifdef __psyz
#include <libetc.h>
#include "libcd_stream.h"
int CdGetSector2(void* madr, int size);
int CdDataSync(int mode);
#endif

typedef struct {
    int num_sectors;
    u_long* buf;
    u_long* cur_buf;
    int mode;
    int size;
    int remaining;
    int start_time2;
    int start_time;
    int pos;
    CdlCB sync_callback;
    CdlCB ready_callback;
    void (*data_callback)();
    int read_mode;
    u_char* result;
    int : 32;
} CD_READ_ATTR;

static CdlCB CD_cbread = NULL;
static volatile CD_READ_ATTR ReadAttr = {0};

/* The LIBCD of Ridge Racer Type 4 ends a read with a Pause whose completion
 * (cd_read_restore_sync, as the sync callback) gives the caller's sync
 * callback back: until then the read is not over, so CdReadSync reports it
 * busy, and CdRead and CdReadBreak wait for it (up to 121 vblanks). */
static volatile int read_pause_pending;

#ifdef __psyz
// The Pause's completion interrupt, handled before a poller sees it.
static volatile int read_pause_irq_owed;

static void read_pause_irq(void) {
    if (read_pause_irq_owed) {
        read_pause_irq_owed = 0;
        Psyz_CdCpu(CD_READ_PAUSE_CALLBACK_CYCLES);
    }
}
#endif

static void cd_read_restore_sync(u_char intr, u_char* result) {
    CdSyncCallback(ReadAttr.sync_callback);
    read_pause_pending = 0;
#ifdef __psyz
    read_pause_irq_owed = 1;
#endif
}

// The read's ending Pause, after the ready and data callbacks are restored.
static void cd_read_end(void) {
    CdSyncCallback(cd_read_restore_sync);
    CdControlF(CdlPause, NULL);
#ifdef __psyz
    Psyz_CdChargeDone(CD_READ_PAUSE_DONE_CLOCKS);
#endif
    read_pause_pending = 1;
}

// Waits for the ending Pause of the previous read; 0 on a timeout, which
// gives the sync callback back without it.
static int cd_read_wait_pause(void) {
    int start = VSync(-1);
    while (read_pause_pending) {
        if ((unsigned)(VSync(-1) - start) >= 121) {
            CD_LOCK();
            CdSyncCallback(ReadAttr.sync_callback);
            read_pause_pending = 0;
            CD_UNLOCK();
            return 0;
        }
#ifdef __psyz
        Psyz_CdPollQuery();
#endif
    }
#ifdef __psyz
    read_pause_irq();
#endif
    return 1;
}

#ifdef __psyz
// The buffer advances by the 32-bit words of a sector, not host u_longs.
#define ADVANCE_SECTOR() \
    (ReadAttr.cur_buf = (u_long*)((char*)ReadAttr.cur_buf + ReadAttr.size * 4))
#else
#define ADVANCE_SECTOR() (ReadAttr.cur_buf += ReadAttr.size)
#endif

void cb_data(void);
int cd_read_retry(int mode);

void cb_read(u_char intr, u_char* result) {
    u_long buf[3];

    ReadAttr.result = result;
    if (intr == CdlDataReady) {
        if (ReadAttr.remaining > 0) {
            if (ReadAttr.size == 0x200) {
                if (ReadAttr.read_mode & 1) {
                    CdDataCallback(NULL);
                    CdGetSector2(buf, 3);
                    CdDataSync(0);
                    CdDataCallback(cb_data);
                } else {
                    CdGetSector(buf, 3);
                }
                if (CdPosToInt((CdlLOC*)buf) != ReadAttr.pos) {
                    puts("CdRead: sector error\n");
                    ReadAttr.remaining = -1;
                }
            }
            if (ReadAttr.read_mode & 1) {
                CdGetSector2(ReadAttr.cur_buf, ReadAttr.size);
            } else {
                CdGetSector(ReadAttr.cur_buf, ReadAttr.size);
                ADVANCE_SECTOR();
                ReadAttr.remaining--;
                ReadAttr.pos++;
            }
        }
    } else {
        ReadAttr.remaining = -1;
    }

    ReadAttr.start_time2 = VSync(-1);
    if (ReadAttr.remaining < 0) {
        cd_read_retry(1);
    }
    if (VSync(-1) > ReadAttr.start_time + 1200) {
        ReadAttr.remaining = -1;
    }
    if (ReadAttr.remaining == 0 || VSync(-1) > ReadAttr.start_time + 1200) {
        CdReadyCallback(ReadAttr.ready_callback);
        if (ReadAttr.read_mode & 1) {
            CdDataCallback(ReadAttr.data_callback);
        }
        cd_read_end();
        if (CD_cbread) {
            CD_cbread(
                ReadAttr.remaining == 0 ? CdlComplete : CdlDiskError, result);
        }
    }
}

void cb_data(void) {
    ADVANCE_SECTOR();
    ReadAttr.remaining--;
    ReadAttr.pos++;
    if (ReadAttr.remaining == 0) {
        CdReadyCallback(ReadAttr.ready_callback);
        if (ReadAttr.read_mode & 1) {
            CdDataCallback(ReadAttr.data_callback);
        }
        cd_read_end();
        if (CD_cbread != NULL) {
            CD_cbread(CdlComplete, (u_char*)ReadAttr.result);
        }
    }
}

/* The PS1 takes the interrupts that come while CD_cw works and waits for
 * an acknowledge (DuckStation: the list DMA's queue exception 734 clocks
 * into CdRead, its ReadN written after it; a root counter's in the
 * Setmode's wait): the commands are not issued under the lock, which
 * would hold those interrupts to its end, nor is CdRead's own CPU time
 * spent under it. */
#ifdef __psyz
#define CD_UNLOCK_FOR_COMMAND() CD_UNLOCK()
#define CD_RELOCK_AFTER_COMMAND() CD_LOCK()
#else
#define CD_UNLOCK_FOR_COMMAND()
#define CD_RELOCK_AFTER_COMMAND()
#endif

static int cd_read_retry_locked(int mode) {
    u_char param;

    CdSyncCallback(0);
    CdReadyCallback(0);
    if (ReadAttr.read_mode & 1) {
        CdDataCallback(0);
    }
    if (CdStatus() & CdlStatShellOpen) {
        if ((VSync(-1) & 0x3f) == 0) {
            puts("CdRead: Shell open...\n");
        }
        CdControlF(CdlNop, NULL);
        ReadAttr.start_time = VSync(-1);
        ReadAttr.remaining = -1;
        return ReadAttr.remaining;
    }
    if (mode) {
        puts("CdRead: retry...\n");
        CD_UNLOCK_FOR_COMMAND();
        CdControl(CdlPause, NULL, NULL);
        int set = CdControl(CdlSetloc, (u_char*)CdLastPos(), NULL);
        CD_RELOCK_AFTER_COMMAND();
        if (set == 0) {
            ReadAttr.remaining = -1;
            return ReadAttr.remaining;
        }
    }
    CdFlush();
    param = ReadAttr.mode;
    if (param != CdMode() || mode) {
        CD_UNLOCK_FOR_COMMAND();
        int set = CdControl(CdlSetmode, &param, NULL);
        CD_RELOCK_AFTER_COMMAND();
        if (set == 0) {
            ReadAttr.remaining = -1;
            return ReadAttr.remaining;
        }
    }
#ifdef __psyz
    else {
        CD_UNLOCK_FOR_COMMAND();
        Psyz_CdCpu(CD_READ_KEEP_MODE_CYCLES);
        CD_RELOCK_AFTER_COMMAND();
    }
#endif
    ReadAttr.pos = CdPosToInt(CdLastPos());
    CdReadyCallback(cb_read);
    if (ReadAttr.read_mode & 1) {
        CdDataCallback(cb_data);
    }
    ReadAttr.cur_buf = ReadAttr.buf;
    CD_UNLOCK_FOR_COMMAND();
    CdControlF(CdlReadN, NULL);
    CD_RELOCK_AFTER_COMMAND();
    ReadAttr.remaining = ReadAttr.num_sectors;
    ReadAttr.start_time2 = VSync(-1);
#ifdef __psyz
    CD_UNLOCK_FOR_COMMAND();
    Psyz_CdCpu(CD_READ_END_CYCLES);
    CD_RELOCK_AFTER_COMMAND();
#endif
    return ReadAttr.remaining;
}

int cd_read_retry(int mode) {
    CD_LOCK();
    int remaining = cd_read_retry_locked(mode);
    CD_UNLOCK();
    return remaining;
}

// Ends the read in progress with its Pause, waited for; a read already
// ending only has its Pause waited for.
void CdReadBreak(void) {
    CD_LOCK();
    if (ReadAttr.read_mode & 1) {
        CdDataSync(0);
    }
    ReadAttr.remaining = 0;
    CD_UNLOCK();
    if (read_pause_pending) {
        cd_read_wait_pause();
        return;
    }
    CD_LOCK();
    CdReadyCallback(ReadAttr.ready_callback);
    if (ReadAttr.read_mode & 1) {
        CdDataCallback(ReadAttr.data_callback);
    }
    cd_read_end();
    CD_UNLOCK();
    cd_read_wait_pause();
}

int CdRead(int sectors, u_long* buf, int mode) {
    cd_read_wait_pause();
#ifdef __psyz
    Psyz_CdCpu(CD_READ_CYCLES);
#endif
    CD_LOCK();
    ReadAttr.mode = mode;
    switch (ReadAttr.mode & (CdlModeSize0 | CdlModeSize1)) {
    case 0:
        ReadAttr.size = 0x200;
        break;
    case CdlModeSize1:
        ReadAttr.size = 0x249;
        break;
    default:
        ReadAttr.size = 0x246;
        break;
    }
    ReadAttr.mode |= CdlModeSize1;
    ReadAttr.buf = buf;
    ReadAttr.num_sectors = sectors;
    ReadAttr.sync_callback = CdSyncCallback(NULL);
    ReadAttr.ready_callback = CdReadyCallback(NULL);
    if (ReadAttr.read_mode & 1) {
        ReadAttr.data_callback = (void (*)())CdDataCallback(NULL);
    }
    ReadAttr.start_time = VSync(-1);
    CD_UNLOCK(); /* no ready callback can mutate this request until retry */
    if (CdStatus() & (CdlStatPlay | CdlStatSeek | CdlStatRead)) {
        CdControlB(CdlPause, NULL, NULL);
    }
    return cd_read_retry(0) > 0;
}

#ifdef __psyz
static void read_sync_poll(unsigned cycles) { Psyz_CdPollCycles(cycles); }

/* ... and VSync(-1)'s second read of the hblank counter, which it repeats
 * (18 clocks) when an hblank comes between the two: about 1% of
 * CdReadSync(0)'s turns (two calls) and 2% of CdReadSync(1)'s calls
 * (four) on DuckStation, charged as their mean (CD_READ_SYNC_*_RETRY) */
static unsigned read_sync_part; // 1/100000 clocks
static unsigned read_sync_retry(unsigned part) {
    read_sync_part += part;
    unsigned whole = read_sync_part / 100000;
    read_sync_part %= 100000;
    return whole;
}
#endif

int CdReadSync(int mode, u_char* result) {
    int ret;

    do {
#ifdef __psyz
        // The drive reads on while the caller polls: a turn up to its look
        // at the read's state (CdReadSync(0)'s at the Pause's flag)
        read_sync_poll(CD_READ_SYNC_TURN_CYCLES +
                       read_sync_retry(mode ? CD_READ_SYNC_CALL_RETRY
                                            : CD_READ_SYNC_TURN_RETRY));
#endif
        CD_LOCK();
        if (VSync(-1) > ReadAttr.start_time + 1200) {
            ret = -1;
        } else if (
            ReadAttr.remaining < 0 || VSync(-1) > ReadAttr.start_time2 + 60) {
            cd_read_retry(1);
            ret = ReadAttr.num_sectors;
        } else {
            ret = ReadAttr.remaining;
        }
        CD_UNLOCK();
        // mode 0 also waits for the read's ending Pause
    } while (mode == 0 && (ret > 0 || (ret == 0 && read_pause_pending)));

#ifdef __psyz
    // the Pause may complete meanwhile: its look comes after CdReady
    read_sync_poll(mode ? CD_READ_SYNC_POLL_END_CYCLES : CD_READ_SYNC_END_CYCLES);
#endif
    CdReady(1, result);
    if (ret == 0 && read_pause_pending) {
        ret = 1;
    }
#ifdef __psyz
    // (a call after the read's last sector, while its Pause is pending,
    // takes LIBCD's other path back: CD_READ_SYNC_PENDING_CYCLES more)
    Psyz_CdCpu(CD_READ_SYNC_RETURN_CYCLES + (ret == 1 && mode && ReadAttr.remaining == 0 &&
                                                     read_pause_pending
                                                 ? CD_READ_SYNC_PENDING_CYCLES
                                                 : 0));
#endif
#ifdef __psyz
    if (ret == 0) {
        read_pause_irq();
    }
#endif
    return ret;
}

CdlCB CdReadCallback(CdlCB func) {
    CD_LOCK();
    CdlCB prev = CD_cbread;
    CD_cbread = func;
    CD_UNLOCK();
    return prev;
}

int CdReadMode(int mode) {
    CD_LOCK();
    int prev = ReadAttr.read_mode;
    ReadAttr.read_mode = mode;
    CD_UNLOCK();
    return prev;
}
