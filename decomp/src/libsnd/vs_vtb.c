#include "libsnd_private.h"

#ifdef __psyz
void Psyz_SpendCycles(unsigned cycles);
#endif

short SsVabTransBody(u_char* addr, short vabid) {
    unsigned vab_start;
#ifdef __psyz
    Psyz_SpendCycles(991); // PS1 time before the DMA (DuckStation)
#endif

    if (vabid >= 0 && vabid <= NUM_VAB) {
        if (_svm_vab_used[vabid] == 2) {
            vab_start = _svm_vab_start[vabid];
            SpuSetTransferMode(0);
            SpuSetTransferStartAddr(vab_start);
            SpuWrite(addr, (u_long)_svm_vab_total[vabid]);
            _svm_vab_used[vabid] = 1;
            return vabid;
        }
    }
    _spu_setInTransfer(0);
    return -1;
}
