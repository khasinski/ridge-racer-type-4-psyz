#include <common.h>
#include <libcd.h>
#include "libcd_stream.h"
#include "libcd_lock.h"

#ifdef __psyz
s32 StMode;
#endif

static inline void StCdInterrupt2(u_char intr, u_char* result) {
    StCdInterrupt();
}

int CdRead2(long mode) {
    u8 param = mode;
#ifdef __psyz
    /* CdRead2's own code up to its ReadS (charged with the Setmode's
     * return: it can be less than CdControl's own return charge) */
    _Static_assert((int)CD_CONTROL_RETURN_CYCLES + (int)CD_READ2_CYCLES >= 0,
                   "CD_READ2_CYCLES below CdControl's return charge");
    Psyz_CdControlThen(CdlSetmode, &param, NULL, CD_READ2_CYCLES);
#else
    CdControl(CdlSetmode, &param, NULL);
#endif
    CD_LOCK();
    if (mode & CdlModeStream) {
        if (mode & CdlModeSize1) {
            StMode = 0;
        } else {
            StMode = 1;
        }
        CdDataCallback(data_ready_callback);
        CdReadyCallback(StCdInterrupt2);
    }
    CD_UNLOCK();
    return CdControl(CdlReadS, NULL, NULL);
}
