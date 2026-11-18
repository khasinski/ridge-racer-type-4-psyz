#include "libsnd_private.h"
#ifdef __psyz
#include "../../../psyz/src/timing_profile.h"
void Psyz_SpendCycles(unsigned cycles);
#define SS_CPU(c) Psyz_SpendCycles(c)
#else
#define SS_CPU(c)
#endif

void SsSetSerialAttr(char s_num, char attr, char mode) {
    SpuCommonAttr param;
    SS_CPU(SS_SET_SERIAL_ATTR_CYCLES);

    if (s_num == 0 && attr == 0) {
        param.mask = SPU_COMMON_CDMIX;
        param.cd.mix = mode;
    }
    if (s_num == 0 && attr == 1) {
        param.mask = SPU_COMMON_CDREV;
        param.cd.reverb = mode;
    }
    if (s_num == 1 && attr == 0) {
        param.mask = SPU_COMMON_EXTMIX;
        param.ext.mix = mode;
    }
    if (s_num == 1 && attr == 1) {
        param.mask = SPU_COMMON_EXTREV;
        param.ext.reverb = mode;
    }
    SpuSetCommonAttr(&param);
}
