#include "libsnd_private.h"

#ifdef __psyz
/* The public entry points below hold the interrupt lock (sound API vs. the
 * sequencer tick, see libsnd_private.h). */
void Psyz_KernelLockApi(void);
void Psyz_KernelUnlockApi(void);
#define SsSeqCalledTbyT SsSeqCalledTbyT_Unlocked
#endif

void SsSeqCalledTbyT(void) {
    int seq_index;
    int tIndex;
    int bit;

    if (_snd_ev_flag == 1) {
        return;
    }
    _snd_ev_flag = 1;
    _SsVmFlush();

    for (seq_index = 0; seq_index < _snd_seq_s_max; seq_index++) {
        bit = 1u << seq_index;
        if (!(_snd_openflag & bit)) {
            continue;
        }
        for (tIndex = 0; tIndex < _snd_seq_t_max; tIndex++) {
            if (_ss_score[seq_index][tIndex].flags & 1) {
                _SsSndPlay(seq_index, tIndex);
                if (_ss_score[seq_index][tIndex].flags & 0x10) {
                    _SsSndCrescendo(seq_index, tIndex);
                }
                if (_ss_score[seq_index][tIndex].flags & 0x20) {
                    _SsSndDecrescendo(seq_index, tIndex);
                }
                if (_ss_score[seq_index][tIndex].flags & 0x40) {
                    _SsSndTempo(seq_index, tIndex);
                }
                if (_ss_score[seq_index][tIndex].flags & 0x80) {
                    _SsSndTempo(seq_index, tIndex);
                }
            }
            if (_ss_score[seq_index][tIndex].flags & 2) {
                _SsSndPause(seq_index, tIndex);
            }
            if (_ss_score[seq_index][tIndex].flags & 8) {
                _SsSndReplay(seq_index, tIndex);
            }
            if (_ss_score[seq_index][tIndex].flags & 4) {
                _SsSndStop(seq_index, tIndex);
                _ss_score[seq_index][tIndex].flags = 0;
            }
        }
    }
    _snd_ev_flag = 0;
}

#ifdef __psyz
#undef SsSeqCalledTbyT
/* A tick's PS1 time: its own (SS_TICK_CYCLES), _SsVmFlush's and the
 * sequences' events' (timing_profile.h). */
#include "../../../psyz/src/timing_profile.h"
void Psyz_SpendCycles(unsigned cycles);
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
unsigned long long Psyz_EmulatedClocks(void);
uint64_t Psyz_KernelInterruptedNs(void);
static volatile unsigned long long ev_flag_from, ev_flag_to;
static volatile int ev_flag_open;
void Psyz_SndEvFlagBegin(void) {
    ev_flag_from = Psyz_EmulatedClocks();
    ev_flag_open = 1;
}
// PSYZ_EVFLAG_TRACE=1: the spans and the skipped ticks, in system clocks
static int EvTrace(void) {
    static int trace = -1;
    if (trace < 0) {
        trace = getenv("PSYZ_EVFLAG_TRACE") != NULL;
    }
    return trace;
}
void Psyz_SndEvFlagEnd(void) {
    ev_flag_to = Psyz_EmulatedClocks();
    ev_flag_open = 0;
    if (EvTrace()) {
        fprintf(stderr, "snd ev flag %llu-%llu\n", ev_flag_from, ev_flag_to);
    }
}
/* Whether the main code held _snd_ev_flag where the tick's interrupt took
 * the CPU (a tick deferred over the API's lock comes after it, at its own
 * time) */
static int EvFlagHeld(void) {
    uint64_t at_ns = Psyz_KernelInterruptedNs();
    if (!at_ns) {
        return 0;
    }
    unsigned long long at = at_ns / 1000000000u * 33868800u +
                            at_ns % 1000000000u * 33868800u / 1000000000u;
    return at >= ev_flag_from && (ev_flag_open || at < ev_flag_to);
}
void SsSeqCalledTbyT(void) {
    Psyz_KernelLockApi();
    /* inside a key-on's or key-off's _snd_ev_flag the PS1's tick does
     * nothing (DuckStation: 102 clocks, the Versus card screen's music a
     * tick behind from one at field 6800, a chord's key-ons 564480 clocks
     * later, vsync 7378) [mark] */
    if (EvFlagHeld()) {
        if (EvTrace()) {
            fprintf(stderr, "snd tick skipped %llu\n", Psyz_EmulatedClocks());
        }
        Psyz_SpendCycles(SS_TICK_SKIPPED_CYCLES);
        Psyz_KernelUnlockApi();
        return;
    }
    unsigned own = SS_TICK_CYCLES;
    for (int i = 0; i < _snd_seq_s_max; i++) {
        if (_snd_openflag & (1u << i)) {
            own += SS_TICK_OPEN_SEQUENCE_CYCLES;
        }
    }
    SsSeqCalledTbyT_Unlocked();
    Psyz_SpendCycles(own);
    Psyz_KernelUnlockApi();
}
#endif
