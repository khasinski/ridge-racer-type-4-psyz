#include <common.h>
#include <kernel.h>
#include <libapi.h>
#ifdef __psyz
#include "../../../psyz/src/timing_profile.h"
void Psyz_SpendCycles(unsigned cycles);
#endif

static u8 D_800B50E0[] = {0x50, 0x73, 0x03, 0x15, 0x9C, 0x85, 0x40};

void InitCARD2(long val);
int ReadInitPadFlag(void);
void InitCARD(long val) {
#ifdef __psyz
    Psyz_SpendCycles(INIT_CARD_CYCLES);
#endif
    ChangeClearPAD(0);
    EnterCriticalSection();
    if (!ReadInitPadFlag()) {
        val = 0;
    }
    InitCARD2(val);
    ExitCriticalSection();
}

void StartCARD2(void);
long StartCARD(void) {
#ifdef __psyz
    Psyz_SpendCycles(START_CARD_CYCLES);
#endif
    EnterCriticalSection();
    StartCARD2();
    ChangeClearPAD(0);
    ExitCriticalSection();
    return 1;
}

void StopCARD2(void);
void _ExitCard(void);
long StopCARD(void) {
    StopCARD2();
    _ExitCard();
    return 1;
}
