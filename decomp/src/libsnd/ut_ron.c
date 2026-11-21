#include <common.h>
#include <libsnd.h>
#include <libspu.h>

#ifdef __psyz
/* The public entry points below hold the interrupt lock (sound API vs. the
 * sequencer tick, see libsnd_private.h). */
void Psyz_KernelLockApi(void);
void Psyz_KernelUnlockApi(void);
#define SsUtReverbOn SsUtReverbOn_Unlocked
#endif

void SsUtReverbOn(void) { SpuSetReverb(1); }

#ifdef __psyz
#undef SsUtReverbOn
void SsUtReverbOn(void) {
    Psyz_KernelLockApi();
    SsUtReverbOn_Unlocked();
    Psyz_KernelUnlockApi();
}
#endif
