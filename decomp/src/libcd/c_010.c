#include <common.h>
#include <libcd.h>
#include "libcd_stream.h"
#include "libcd_lock.h"

void StSetMask(u_long mask, u_long start, u_long end) {
    CD_LOCK();
    StSTART_FLAG = mask;
    StStartFrame = start;
    StEndFrame = end;
    CD_UNLOCK();
}
