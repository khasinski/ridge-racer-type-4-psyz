#ifndef LIBCD_LOCK_H
#define LIBCD_LOCK_H
/* Native callbacks may run on the IRQ thread. Protect the original shared
 * SDK state with the same recursive lock used by interrupt dispatch. */
#ifdef __psyz
void Psyz_KernelLockInterrupts(void);
void Psyz_KernelUnlockInterrupts(void);
#define CD_LOCK() Psyz_KernelLockInterrupts()
#define CD_UNLOCK() Psyz_KernelUnlockInterrupts()
#else
#define CD_LOCK() ((void)0)
#define CD_UNLOCK() ((void)0)
#endif
#endif
