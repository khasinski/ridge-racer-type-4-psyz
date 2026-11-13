/* PS1 time costs of BIOS, SDK and interrupt work, in one table.
 *
 * Native code runs in no emulated time. On the offline clock
 * (PSYZ_AUDIO_CLOCK=video) PsyZ spends the PS1 time of the work whose length
 * decides on which field something happens (Psyz_SpendCycles,
 * Psyz_SpendTime, Psyz_KernelInterruptCycles); on a real-time clock these
 * costs do nothing. Every value here was measured on DuckStation (the
 * modified regtest build on upstream 2c527f4f2, SCPH-1001 BIOS) running
 * Ridge Racer Type 4 (PAL, SCES-017.06), unless its entry says otherwise.
 * Units are system clocks (33.8688 MHz) unless the name says _NS or _US.
 *
 * Each entry names how it was fitted and the PsyZ commit that has the run:
 *
 *   [call]   entry-to-return ticks of the call on DuckStation, measured with
 *            GDB breakpoints at the call and its return. Any breakpoint
 *            slows DuckStation's CPU by about 1%, so these may read up to
 *            about 1% high; refit them from field timing without
 *            breakpoints when one decides a field.
 *   [field]  fitted so that the port's mode entries, FrameCounter and rand
 *            land on DuckStation's fields, in runs without breakpoints.
 *   [model]  taken from DuckStation's device model (its source), not
 *            measured.
 *   [unrecorded] no measurement is recorded for it (a poll or spin step
 *            whose exact length has not decided a field so far).
 *
 * Changing a value changes on which field things happen: check the
 * per-field game state against DuckStation (docs/ARCHITECTURE.md of the
 * game, PS1 time model) before and after. */
#ifndef PSYZ_TIMING_PROFILE_H
#define PSYZ_TIMING_PROFILE_H

/* ---- BIOS (libapi.c, libetc.c, libcomb.c) ---- */
enum {
    /* A(2Fh) rand / A(30h) srand with the A0 stub. [call] 686859c */
    BIOS_RAND_CYCLES = 433,
    BIOS_SRAND_CYCLES = 119,
    /* A(28h) bzero and A(2Bh) memset: 328 + 132 a byte; A(2Ah) memcpy:
     * 258 + 204 a byte (the uncached 8-bit ROM). bzero(0x620) = 207300,
     * memcpy(0x5C) = 19019, memset(8) = 1384. [call] 686859c */
    BIOS_FILL_CYCLES = 328,
    BIOS_FILL_BYTE_CYCLES = 132,
    BIOS_COPY_CYCLES = 258,
    BIOS_COPY_BYTE_CYCLES = 204,
    /* firstfile's directory search after its sector 0 check, 6.2k more when
     * it returns an entry; a nextfile. [call] 8a79d5e */
    BIOS_CARD_FIRSTFILE_CYCLES = 25800,
    BIOS_CARD_ENTRY_CYCLES = 6200,
    BIOS_CARD_NEXTFILE_CYCLES = 13700,
    /* libsio's AddSIO: AddDevice and the driver's setup, in the game's
     * SystemInit. [call] 50487e6 */
    ADD_SIO_DEVICE_CYCLES = 66100,
};

/* ---- Interrupts (kernel.c, libapi.c) ---- */
enum {
    /* The BIOS exception handler and libetc's dispatcher around a callback:
     * 2.9k for an RCnt interrupt with an empty callback. [call] 686859c */
    KERNEL_IRQ_CYCLES = 2850,
    /* The vblank handler besides the kernel's: libetc's VSync handler and
     * callbacks dispatch, the BIOS card driver's turn. [call] 686859c */
    VBLANK_HANDLER_CYCLES = 2387,
    /* libpad's controller exchange at each vblank once PadStartCom ran: its
     * SIO busy waits for a digital pad in port 1, port 2 empty [call]
     * 686859c; 4.8k more with a pad in port 2 (26.2k against 21.4k for the
     * whole handler) [call] f0e3704. */
    LIBPAD_VBLANK_CYCLES = 16084,
    LIBPAD_VBLANK_PORT2_CYCLES = 4790,
};

/* ---- libpad (libetc.c) ---- */
enum {
    /* PadInitDirect clears its driver state (a 0x1E0-byte BIOS bzero) and
     * sets up the exchange; PadStartCom. [call] 50487e6 */
    PAD_INIT_DIRECT_CYCLES = 73316,
    PAD_START_COM_CYCLES = 1589,
};

/* ---- libgpu (decomp libgpu/sys.c, prim.c) ---- */
enum {
    /* The library code of the calls, without the BIOS memcpy of the PS1
     * environment (BIOS_COPY_*: DRAWENV 0x5C, DISPENV 0x14 bytes) and the
     * DMA the CPU waits for (17 clocks per 16 words: the ordering table
     * clear and an image upload, which the next DrawSync(0) finishes).
     * [call] 686859c */
    PUT_DRAW_ENV_CYCLES = 1165,
    PUT_DISP_ENV_CYCLES = 396, // unchanged display ranges
    GET_ENV_CYCLES = 40,
    DRAW_OTAG_CYCLES = 483,
    CLEAR_OTAG_R_CYCLES = 330,
    LOAD_IMAGE_CYCLES = 830,
    DRAW_SYNC_CYCLES = 246, // an idle GPU queue
    /* DrawOTag's list DMA over the table's empty head, which the CPU waits
     * for: 43.4k for an empty 704-entry table (a movie frame). [call]
     * 50d481f; the title after the opening movie then starts on
     * DuckStation's field [field]. */
    DRAW_OTAG_NODE_CYCLES = 61,
    /* AddPrim/AddPrims, SetSemiTrans/SetShadeTex, and the primitive setters
     * (SetPolyF3 .. SetLineG4, the same few stores as SetSprt). [call]
     * 686859c */
    PRIM_ADD_CYCLES = 38,
    PRIM_SET_ATTR_CYCLES = 13,
    PRIM_SETTER_CYCLES = 5,
};

/* ---- libspu (decomp libspu/spu.c, s_crwa.c, s_itc.c) ---- */
enum {
    /* SPU RAM DMA: a halfword per 16 system clocks (DuckStation spu.cpp
     * TRANSFER_TICKS_PER_HALFWORD) [model] dd07ccc; the game's VAB uploads
     * of 54256 and 282848 bytes then wait 262 and 1096 lines, as on
     * DuckStation [field] 414793b. */
    SPU_DMA_CLOCKS_PER_BYTE = 8,
    /* SpuClearReverbWorkArea: the setup of each 1 KiB chunk (_spu_t's
     * waits, the DMA event) besides its DMA: 97 chunks took 1.27M clocks
     * in SsInit. [call] 686859c */
    SPU_CLEAR_CHUNK_CYCLES = 3070,
    /* SpuIsTransferCompleted(SPU_TRANSFER_PEEK) while the DMA runs, and
     * Psyz_SpuDmaWait's step: a poll's time in ns. [unrecorded] dd07ccc */
    SPU_TRANSFER_POLL_NS = 1000,
    SPU_DMA_WAIT_STEP_NS = 64000,
};

/* ---- libsnd (decomp libsnd) ---- */
enum {
    /* A sequencer tick (SsSeqCalledTbyT), mostly _SsVmFlush's pass over the
     * 24 voices, with no sequence playing. [call] 686859c */
    SS_TICK_CYCLES = 6335,
    /* SsUtAllKeyOff: a SpuSetVoiceAttr per voice, 22.8k for 24 voices.
     * [call] f8ddd9d; the menu sound bank loads after the title then end on
     * DuckStation's field [field]. */
    SS_UT_ALL_KEY_OFF_VOICE_CYCLES = 951,
    /* SsVabOpenHeadWithMode's header parse (a menu bank) and
     * SsVabTransBody before its DMA. [call] 7ffeed0 */
    SS_VAB_OPEN_HEAD_CYCLES = 4514,
    SS_VAB_TRANS_BODY_CYCLES = 991,
};

/* ---- Memory card (libcard.c) ---- */
enum {
    /* A 128-byte frame ends when the card has answered its last byte, this
     * long after the vblank that started it: a FileRead's last frame raises
     * its event 266.3k clocks after that vblank. [call] 8a79d5e */
    CARD_SECTOR_CLOCKS = 266300,
    /* _bu_init clears the directory cache of a port without a card (a ROM
     * bzero of its 15 entries and the code around it), after the timeout.
     * [call] 8a79d5e */
    CARD_BU_INIT_EMPTY_PORT_CLOCKS = 74200,
    /* The handler's bookkeeping when a request ends (its events undelivered
     * and raised), added to that interrupt. [call] 8a79d5e */
    CARD_FINISH_ISR_CLOCKS = 5400,
    /* A PS1 program spinning on the card driver: a scanline a spin.
     * [unrecorded] dd07ccc */
    CARD_SPIN_NS = 64000,
};

/* ---- CD drive and MDEC (libcd.c, libpress.c) ---- */
enum {
    /* One StGetNext poll of the stream ring in a movie player's busy loop,
     * CPU time only (the interrupts that come meanwhile lengthen the
     * wait), in millicycles: the game spins 4,000,000 polls on an empty
     * ring before its first read, 290 fields with one pad, 292 with two.
     * [field] f0e3704 (no breakpoints) */
    ST_POLL_MILLICYCLES = 46800,
    /* Another drive query in a busy loop (CdSync, CdReady, CdReadSync),
     * interrupts included. [unrecorded] 8080ec2 */
    CD_POLL_NS = 2000,
    /* An MDEC macroblock (a block in the mono depths) leaves the decoder
     * about every 101 us with its DMA (2688 cycles per macroblock in
     * DuckStation's model and the DMA): the game's 320x176 movie frames,
     * 220 macroblocks, take 22.2 ms. [call] 8080ec2 (a frame's decode) */
    MDEC_UNIT_NS = 101000,
};

#endif
