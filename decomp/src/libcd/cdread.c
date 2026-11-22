#include <common.h>
#include <libcd.h>
#include "libcd_lock.h"
#ifdef __psyz
#include <libetc.h>
int CdGetSector2(void* madr, int size);
int CdDataSync(int mode);
/* Delivers what the drive has for its callbacks (libcd.c): a LIBCD loop
 * that waits on them services the drive. */
void Psyz_CdServiceDrive(void);
/* ... after a millisecond on the kernel's clock, for a loop that found the
 * read not done. */
void Psyz_CdWaitDrive(void);
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

static void cd_read_restore_sync(u_char intr, u_char* result) {
    CdSyncCallback(ReadAttr.sync_callback);
    read_pause_pending = 0;
}

// The read's ending Pause, after the ready and data callbacks are restored.
static void cd_read_end(void) {
    CdSyncCallback(cd_read_restore_sync);
    read_pause_pending = 1; // before the Pause, whose completion may come at once
    CdControlF(CdlPause, NULL);
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
        Psyz_CdWaitDrive();
#endif
    }
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
        CdControl(CdlPause, NULL, NULL);
        int set = CdControl(CdlSetloc, (u_char*)CdLastPos(), NULL);
        if (set == 0) {
            ReadAttr.remaining = -1;
            return ReadAttr.remaining;
        }
    }
    CdFlush();
    param = ReadAttr.mode;
    if (param != CdMode() || mode) {
        int set = CdControl(CdlSetmode, &param, NULL);
        if (set == 0) {
            ReadAttr.remaining = -1;
            return ReadAttr.remaining;
        }
    }
    ReadAttr.pos = CdPosToInt(CdLastPos());
    CdReadyCallback(cb_read);
    if (ReadAttr.read_mode & 1) {
        CdDataCallback(cb_data);
    }
    ReadAttr.cur_buf = ReadAttr.buf;
    CdControlF(CdlReadN, NULL);
    ReadAttr.remaining = ReadAttr.num_sectors;
    ReadAttr.start_time2 = VSync(-1);
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

int CdReadSync(int mode, u_char* result) {
    int ret;

    for (int turn = 0;; turn++) {
#ifdef __psyz
        // the drive delivers while the caller polls
        if (turn) {
            Psyz_CdWaitDrive();
        } else {
            Psyz_CdServiceDrive();
        }
#else
        (void)turn;
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
        if (!(mode == 0 && (ret > 0 || (ret == 0 && read_pause_pending)))) {
            break;
        }
    }

    CdReady(1, result);
    if (ret == 0 && read_pause_pending) {
        ret = 1;
    }
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
