#include "libsnd_private.h"
#ifdef __psyz
#include "../../../psyz/src/timing_profile.h"
void Psyz_SpendCycles(unsigned cycles);
#define SS_CPU(c) Psyz_SpendCycles(c)
#else
#define SS_CPU(c)
#endif

void SsSetSerialVol(char s_num, short voll, short volr) {
    SpuCommonAttr attr;
    SS_CPU(SS_SET_SERIAL_VOL_CYCLES);

    if (s_num == 0) {
        attr.mask = SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR;
        if (voll > 0x7F) {
            voll = 0x7F;
        }
        if (volr > 0x7F) {
            volr = 0x7F;
        }
        attr.cd.volume.left = voll * 258;
        attr.cd.volume.right = volr * 258;
    }
    if (s_num == 1) {
        attr.mask = SPU_COMMON_EXTVOLL | SPU_COMMON_EXTVOLR;
        if (voll > 0x7F) {
            voll = 0x7F;
        }
        if (volr > 0x7F) {
            volr = 0x7F;
        }
        attr.ext.volume.left = voll * 258;
        attr.ext.volume.right = volr * 258;
    }
    SpuSetCommonAttr(&attr);
}
