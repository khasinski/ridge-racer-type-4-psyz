#include "libsnd_private.h"
#ifdef __psyz
#include "../../../psyz/src/timing_profile.h"
void Psyz_SpendCycles(unsigned cycles);
#endif

void SsSetMVol(short voll, short volr) {
    SpuCommonAttr attr;
#ifdef __psyz
    Psyz_SpendCycles(SS_SET_MVOL_CYCLES);
#endif

    attr.mask = SPU_VOICE_VOLL | SPU_VOICE_VOLR;
    attr.mvol.left = voll * 129;
    attr.mvol.right = volr * 129;
    SpuSetCommonAttr(&attr);
}
