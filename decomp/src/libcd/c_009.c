#include <common.h>
#include <libcd.h>
#include "libcd_stream.h"
#include "libcd_lock.h"

static u_long get_next(u_long** addr, u_long** header) {
    volatile StHEADER* ptr;
    CD_LOCK();
    ptr = &StRingAddr[StRingIdx3];
    if (ptr->id == 1) {
        StRingIdx3 = 0;
        if (StEndFrame != 0) {
            ptr->id = 0;
        }
        ptr = &StRingAddr[StRingIdx3];
    }
    if (ptr->id == 2) {
        ptr->id = 4;
        *addr = (u_long*)(&StRingAddr[StRingSize] + (StRingIdx3 * 0x3F));
        *header = (u_long*)ptr;
        CD_UNLOCK();
        return 0;
    } else {
        CD_UNLOCK();
        return 1;
    }
}

u_long StGetNext(u_long** addr, u_long** header) {
#ifdef __psyz
    /* The drive fills the ring while the caller polls for a frame: without
     * one, the poll waits for the stream's next sector, which comes at the
     * disc's rate (a movie's frames at theirs). */
    Psyz_CdServiceDrive();
    if (get_next(addr, header) == 0) {
        return 0;
    }
    Psyz_CdWaitSector();
#endif
    return get_next(addr, header);
}
