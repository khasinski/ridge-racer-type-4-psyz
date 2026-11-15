#include <common.h>
#include <libcd.h>
#include "libcd_stream.h"
#include "libcd_lock.h"

u_long StGetNext(u_long** addr, u_long** header) {
    volatile StHEADER* ptr;
#ifdef __psyz
    // The drive keeps filling the ring while the caller polls for a frame.
    Psyz_CdPollDrive();
#endif
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
