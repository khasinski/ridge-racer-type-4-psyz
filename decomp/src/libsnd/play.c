#include "libsnd_private.h"
#ifdef __psyz
#include "../../../psyz/src/timing_profile.h"
void Psyz_SpendCycles(unsigned cycles);
#define SS_CPU(c) Psyz_SpendCycles(c)
#else
#define SS_CPU(c)
#endif

void _SsSndPlay(short seq_access_num, short arg1) {
    SS_CPU(SS_SND_PLAY_CYCLES);
    _SsSeqPlay(seq_access_num, arg1);
}
