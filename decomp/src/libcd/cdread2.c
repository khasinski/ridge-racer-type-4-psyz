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
    CdControl(CdlSetmode, &param, NULL);
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
#ifdef __psyz
    Psyz_CdCpu(CD_READ2_CYCLES);
#endif
    return CdControl(CdlReadS, NULL, NULL);
}
