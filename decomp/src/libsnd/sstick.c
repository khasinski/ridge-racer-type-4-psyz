#include "libsnd_private.h"
#ifdef __psyz
#include "../../../psyz/src/timing_profile.h"
void Psyz_SpendCycles(unsigned cycles);
#define SS_CPU(c) Psyz_SpendCycles(c)
#else
#define SS_CPU(c)
#endif
#include <libetc.h>

void SsSetTickMode(long tick_mode) {
    int video_mode;
    SS_CPU(SS_SET_TICK_MODE_CYCLES);

    video_mode = GetVideoMode();
    if (tick_mode & SS_NOTICK) {
        _snd_seq_tick_env.manual_tick = 1;
        _snd_seq_tick_env.tick_mode = tick_mode & 0xFFF;
    } else {
        _snd_seq_tick_env.manual_tick = 0;
        _snd_seq_tick_env.tick_mode = tick_mode;
    }
    if (_snd_seq_tick_env.tick_mode < 6) {
        switch (_snd_seq_tick_env.tick_mode) {
        case 4:
            VBLANK_MINUS = 50;
            if (video_mode != 1) {
                _snd_seq_tick_env.tick_mode = 50;
            } else {
                _snd_seq_tick_env.tick_mode = SS_TICKVSYNC;
            }
            return;
        case 1:
            VBLANK_MINUS = 60;
            if (video_mode == 0) {
                _snd_seq_tick_env.tick_mode = SS_TICKVSYNC;
            } else {
                _snd_seq_tick_env.tick_mode = 60;
            }
            return;
        case 3:
            VBLANK_MINUS = 120;
            return;
        case 2:
            VBLANK_MINUS = 240;
            return;
        case 5:
            if (video_mode == 0) {
                VBLANK_MINUS = 60;
            } else if (video_mode == 1) {
                VBLANK_MINUS = 50;
            } else {
                VBLANK_MINUS = 60;
            }
            break;
        case 0:
            if (video_mode == 0) {
                VBLANK_MINUS = 60;
            } else if (video_mode == 1) {
                VBLANK_MINUS = 50;
            } else {
                VBLANK_MINUS = 60;
            }
            return;
        default:
            VBLANK_MINUS = 60;
            return;
        }
    } else {
        VBLANK_MINUS = _snd_seq_tick_env.tick_mode;
    }
}
