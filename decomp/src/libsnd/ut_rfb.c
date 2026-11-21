#include "libsnd_private.h"

#ifdef __psyz
/* The public entry points below hold the interrupt lock (sound API vs. the
 * sequencer tick, see libsnd_private.h). */
void Psyz_KernelLockApi(void);
void Psyz_KernelUnlockApi(void);
#define SsUtSetReverbFeedback SsUtSetReverbFeedback_Unlocked
#endif

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/ut_rfb", SsUtSetReverbFeedback);
#else
void SsUtSetReverbFeedback(short feedback) {
    _svm_rattr.mask = SPU_REV_FEEDBACK;
    _svm_rattr.feedback = feedback;
    SpuSetReverbModeParam(&_svm_rattr);
}
#endif

#ifdef __psyz
#undef SsUtSetReverbFeedback
void SsUtSetReverbFeedback(short feedback) {
    Psyz_KernelLockApi();
    SsUtSetReverbFeedback_Unlocked(feedback);
    Psyz_KernelUnlockApi();
}
#endif
