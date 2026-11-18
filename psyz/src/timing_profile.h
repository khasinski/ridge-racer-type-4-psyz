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
 *   [rec]    entry-to-return clocks of the call on DuckStation's
 *            recompiler, the CPU mode of the reference runs, measured
 *            without breakpoints by a timing stub patched into RAM (the
 *            game's tools/port/ds_calltime.py); the median of the calls in
 *            the context named, the stub's own ~10 clocks included.
 *   [rec ratio] a breakpoint value of a cost the call's context varies
 *            (or that leaves out part of the call), scaled by the recompiler
 *            / interpreter clocks of the whole call in one context (the
 *            tool's DUMMYBP=1 run).
 *   [call]   entry-to-return ticks measured with GDB breakpoints at the
 *            call and its return. A breakpoint (and single-stepping) runs
 *            DuckStation's interpreter, whose clocks differ from the
 *            recompiler's by -45% to +20% per routine; kept where the
 *            recompiler agrees (BIOS ROM loops) or cannot measure it
 *            (interrupt handlers, calls of a few clocks, card events).
 *   [field]  fitted so that the port's mode entries, FrameCounter and rand
 *            land on DuckStation's fields, in runs without breakpoints.
 *   [model]  taken from DuckStation's device model (its source), not
 *            measured.
 *   [exc]    exception-to-return clocks of an interrupt on DuckStation's
 *            recompiler: stubs at the exception vector and at the BIOS's
 *            returns (rfe) log the tick (the game's tools/port/ds_frames.py
 *            DSF_EXC=1), less the stubs' own clocks; the median.
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
     * memcpy(0x5C) = 19019, memset(8) = 1384. [call] 686859c; the
     * recompiler gives the same within 3 clocks (bzero 6, 48 and 480
     * bytes, memcpy 8 bytes) */
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
     * SystemInit. [rec] boot (interpreter: 66.1k, 50487e6) */
    ADD_SIO_DEVICE_CYCLES = 65704,
    /* One pass of libsio's busy waits for the serial port (the status read,
     * the wait callback's load and call, a three to six instruction compare
     * in the game, and the branch back): about 22 instructions from the
     * I-cache with two main RAM loads and the SIO1 status read. The game's
     * callbacks count these passes, so they are its link timeouts: 25000
     * (the race, ~24 ms) and 99999 (the menus, ~94 ms). Unlike the other
     * costs here it also applies on a real-time clock (libcomb.c paces the
     * passes). DuckStation's SIO1 has no link cable, so its loops never
     * wait there. [unrecorded] estimated from the instruction stream */
    SIO_POLL_CYCLES = 32,
};

/* ---- Interrupts (kernel.c, libapi.c) ---- */
enum {
    /* The BIOS exception handler and libetc's dispatcher around a callback,
     * before the BIOS card driver is installed: an RCnt interrupt with no
     * tick 2374 clocks from the exception to its return, a vblank 2442,
     * at boot (SoundSystemInit's bzero, DuckStation fields 426-435) [exc]
     * less the stubs' 24 clocks a dispatch (the bzero's span with and
     * without them). */
    KERNEL_IRQ_CYCLES = 2350,
    /* The card driver's chain (InitCARD) on every exception from then on:
     * a non-ticking RCnt interrupt takes 2748 in the menus [exc] */
    BIOS_CARD_CHAIN_CYCLES = 374,
    /* SetRCnt up to its last mode write, which starts the count (kernel.c
     * counts from where the CPU's work has got to, the handlers' time owed
     * included): the sequencer's timer then interrupts at DuckStation's
     * phase against the vblanks to within 40 clocks over the Grand Prix
     * menus (the timer's period from its RCNT2 mode write in
     * DUCKSTATION_CD_TRACE against the blank times) [field] */
    SET_RCNT_CYCLES = 84,
    /* The vblank handler besides the kernel's: libetc's VSync handler and
     * callbacks dispatch (68), and the BIOS card driver's turn once
     * StartCARD ran (1409): a vblank with libpad's exchange of a digital
     * pad takes 20309 [exc] in the menus. */
    VBLANK_HANDLER_CYCLES = 68,
    CARD_VBLANK_TURN_CYCLES = 1409,
    /* The vblank where a raw card access ends (Psyz_CardRawEndsAt): the
     * driver's handler there instead, and no libpad exchange: 4901 for the
     * whole exception [exc] the boot's card polling (DuckStation fields
     * 699-798) */
    CARD_RAW_END_CYCLES = 2109,
    /* libpad's controller exchange at each vblank once PadStartCom ran: its
     * SIO busy waits for a digital pad in port 1, port 2 empty [call]
     * 686859c; 4.8k more with a pad in port 2 (26.2k against 21.4k for the
     * whole handler) [call] f0e3704. */
    LIBPAD_VBLANK_CYCLES = 16084,
    LIBPAD_VBLANK_PORT2_CYCLES = 4790,
    /* Each halfword of a controller's packet past the first: libpad's
     * handler takes 22.0k clocks with a JogCon in port 1 (three halfwords)
     * against 15.6k with a digital pad. [rec] ds_irqlog, menus */
    LIBPAD_HALFWORD_CYCLES = 3200,
    /* A vblank that arrives while another interrupt's exception runs
     * (libapi.c Psyz_VSyncBlank): the BIOS priority chains test for it
     * this long after that interrupt, libpad's verifier (_padIsVsync) and
     * then the card driver's, and libetc's dispatcher last reads the
     * interrupt status this long before the end of the exception as
     * charged (KERNEL_IRQ_CYCLES and the callbacks'). On DuckStation
     * (stubs logging root counter 2 at trapIntr, _padIntPad and
     * SsSeqCalledTbyT, and a breakpoint for the 1118 clocks from a vblank
     * to _padIntPad) libpad misses a blank 1056 to 2607 clocks after a
     * sequencer interrupt that does not tick, or from 1056 clocks after one
     * that does to 62 clocks after the tick returns; the card driver's
     * handler misses those from 1197 clocks on, through a ticking one's
     * tick too (blanks 3.2k and 7.4k into one, the opening's port vsyncs
     * 397 and 431: no vblank exception follows and the driver's port
     * alternation skips that turn). [field] VS menus, 20000 attract
     * fields */
    BIOS_CHAIN_PAD_TEST_CLOCKS = 1056,
    BIOS_CHAIN_CARD_TEST_CLOCKS = 1197,
    LIBETC_DISPATCH_EXIT_CLOCKS = 243,
};

/* ---- libpad (libetc.c) ---- */
enum {
    /* PadInitDirect clears its driver state (a 0x1E0-byte BIOS bzero) and
     * sets up the exchange; PadStartCom. [rec] boot (the breakpoint value
     * 73.3k of 50487e6 had an interrupt inside; the interpreter gives 64.1k
     * and 1.6k) */
    PAD_INIT_DIRECT_CYCLES = 63992,
    PAD_START_COM_CYCLES = 1317,
};

/* ---- libgpu (decomp libgpu/sys.c, prim.c) ---- */
enum {
    /* The library code of the calls, without the BIOS memcpy of the PS1
     * environment (BIOS_COPY_*: DRAWENV 0x5C, DISPENV 0x14 bytes) and the
     * DMA the CPU waits for (17 clocks per 16 words: the ordering table
     * clear and an image upload, which the next DrawSync(0) finishes).
     * [call] 686859c, but PutDrawEnv, PutDispEnv and DrawOTag [rec] in a
     * Time Attack run (DS fields 283-3700: 19816 and 4941 with the memcpy,
     * the display start changing at every call, and 468 for a table that
     * starts with a packet). The recompiler confirms DrawSync (249). */
    /* SystemInit's graphics setup [ret] (tools/port/ds_frames.py return
     * marks, the boot): ResetGraph(0) (the GPU reset and its waits) 28164,
     * ResetGraph(1) 2488, SetGraphDebug 41, SetDispMask 175, InitGeom (the
     * GTE registers) 48903 */
    RESET_GRAPH_CYCLES = 28164,
    RESET_GRAPH_QUEUE_CYCLES = 2488,
    SET_GRAPH_DEBUG_CYCLES = 41,
    SET_DISP_MASK_CYCLES = 175,
    INIT_GEOM_CYCLES = 48903,
    PUT_DRAW_ENV_CYCLES = 790,
    PUT_DISP_ENV_CYCLES = 603,
    GET_ENV_CYCLES = 40,
    DRAW_OTAG_CYCLES = 468,
    CLEAR_OTAG_R_CYCLES = 330,
    LOAD_IMAGE_CYCLES = 830,
    DRAW_SYNC_CYCLES = 246, // an idle GPU queue
    /* The GPU's list DMA (gpu_timing.c), which stalls the CPU while it
     * moves data: DuckStation's linked-list DMA takes 8 clocks to read a
     * node's header and 5 more to set up a packet's block, and the RAM's
     * 17 clocks per 16 words of it (dma.cpp, bus.h GetDMARAMTickCount), in
     * 1000-clock slices, and moves on as the GPU takes the commands. [model] */
    DRAW_OTAG_NODE_CYCLES = 8,
    DRAW_OTAG_BLOCK_CYCLES = 5,
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
     * waits, the DMA event) besides its DMA: its 97 chunks take 1.219M
     * clocks in SsInit on the recompiler [mark] (the entry of the next
     * function, as CD_CW_ISSUE_CYCLES); refit to SpuInit's 1,248,339 to
     * _SsInit, the two vblanks within it included (ds_frames.py). */
    SPU_CLEAR_CHUNK_CYCLES = 4410,
    /* SpuInit (cold): _spu_init's resets and waits and SpuStart, 29.2k up
     * to SsInit's SpuClearReverbWorkArea [mark] */
    SPU_INIT_CYCLES = 29230,
    /* SpuIsTransferCompleted(SPU_TRANSFER_PEEK) while the DMA runs, and
     * Psyz_SpuDmaWait's step: a poll's time in ns. [unrecorded] dd07ccc */
    SPU_TRANSFER_POLL_NS = 1000,
    SPU_DMA_WAIT_STEP_NS = 64000,
};

/* ---- libsnd (decomp libsnd) ---- */
enum {
    /* A sequencer tick (SsSeqCalledTbyT) by its parts, on DuckStation
     * (ds_frames.py name:ret marks, each call's time less its marked
     * callees', the Save and Load scenario's menu sound effects, port
     * vsyncs 2905-3000) [mark]: the tick's own 396; _SsVmFlush 5544 with
     * no voice to update (24 envelope reads, the reverb and noise masks)
     * plus its two SpuSetKey calls (52 each), and about 920 a voice whose
     * registers it rewrites (DuckStation's flush calls SpuSetVoiceAttr,
     * 779, for it); _SsSndPlay 132 a playing track; _SsGetSeqData 332 an
     * event; _SsVmKeyOn 3313 (median of 22, 2692-3556) and _SsVmKeyOff
     * 1234. A tick with sound effects starting takes 10k-20k. The marks'
     * stubs add about 15 clocks each to the calls around them: the tick's
     * own is the rest of a tick with no sequence playing, 5895 in all
     * (the attract boot's ticks, SsSeqCalledTbyT:ret alone) [rec ratio]. */
    SS_TICK_CYCLES = 5895 - (5544 + 2 * 52),
    SS_VM_FLUSH_CYCLES = 5544 + 2 * 52,
    SS_VM_FLUSH_VOICE_CYCLES = 920,
    SS_SND_PLAY_CYCLES = 132,
    SS_GET_SEQ_DATA_CYCLES = 332,
    SS_VM_KEY_ON_CYCLES = 3313,
    SS_VM_KEY_OFF_CYCLES = 1234,
    /* SoundSystemInit's sequencer setup after CdReset, entry to entry of
     * the next call [mark] (tools/port/ds_frames.py, the boot):
     * SsSetSerialAttr 142, SsSetSerialVol 146, SsSetTableSize 911,
     * SsSetTickMode 81, and _SsStart's 1000-pass wait loop up to SetRCnt
     * 2633, the rest to the caller's next call 799: SetRCnt then lands on
     * DuckStation's clock. */
    SS_SET_SERIAL_ATTR_CYCLES = 142,
    SS_SET_SERIAL_VOL_CYCLES = 146,
    SS_SET_TABLE_SIZE_CYCLES = 911,
    SS_SET_TICK_MODE_CYCLES = 81,
    SS_START_CYCLES = 2633,
    SS_START_RETURN_CYCLES = 799,
    /* _SsInit: the voice and state registers and _SsVmInit, 35.2k up to
     * the caller's next call (SoundSystemInit) [mark] */
    SS_INIT_CYCLES = 35204,
    /* SsUtAllKeyOff: a SpuSetVoiceAttr per voice, 21.8k for 24 voices [rec]
     * in a Time Attack run's menus (interpreter: 22.8k, f8ddd9d); the menu
     * sound bank loads after the title end on DuckStation's field [field]. */
    SS_UT_ALL_KEY_OFF_VOICE_CYCLES = 909,
    /* SsVabOpenHeadWithMode's header parse (the menu banks: 4.3k-5.4k,
     * the breakpoint value of 7ffeed0 scaled by 4668 / 4919 [rec ratio]) and
     * SsVabTransBody before its DMA [rec] (interpreter: 1.0k). */
    SS_VAB_OPEN_HEAD_CYCLES = 4284,
    SS_VAB_TRANS_BODY_CYCLES = 675,
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
    /* InitCARD (the BIOS card driver's setup and its handlers in the
     * chains) 155157 [ret] the boot, with a vblank and a sequencer tick in
     * it; StartCARD 2893 */
    INIT_CARD_CYCLES = 146643,
    START_CARD_CYCLES = 2893,
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
     * [field] f0e3704; refit with the interrupt costs above to DuckStation's
     * 5,839,811 us from the opening's ring setup to its first read
     * (tools/port/ds_frames.py marks). */
    ST_POLL_MILLICYCLES = 47026,
    /* Another drive query in a busy loop (CdSync, CdReady, CdReadSync),
     * interrupts included. [unrecorded] 8080ec2 */
    CD_POLL_NS = 2000,
    /* LIBCD's CD_cw around a command: from its call to the command
     * register write, and from the acknowledge (the interrupt, the result
     * read, the return) to the caller; and the completion interrupt's
     * handling before a wait for it returns (Init, Pause). CD_init's own
     * setup before its first command (the controller's registers and
     * ResetCallback). Entry-to-entry clocks of the boot's commands on
     * DuckStation's recompiler [mark] (DUCKSTATION_CD_TRACE, a stub at each
     * function entry writing root counter 2 before SsStart). */
    CD_CW_ISSUE_CYCLES = 480,
    /* The drive's interrupts as exceptions, from the exception to its
     * return: a data sector's INT1 (LIBCD's handler, CdRead's callback and
     * its CdGetSector), a command's acknowledge and a second response,
     * before the card driver's chain is installed (BIOS_CARD_CHAIN_CYCLES
     * more after it). They hide a vblank that comes meanwhile from the BIOS
     * chains (libapi.c Psyz_VSyncBlank). A sector's is handler time; an
     * acknowledge's and a completion's are spent by CD_cw and the waits
     * (CD_CW_ACK_CYCLES, CD_DONE_IRQ_CYCLES). [exc] the boot's reads
     * (3800, 2420) and the Grand Prix menus' loads (4130, 2790) */
    CD_DATA_IRQ_CLOCKS = 3780,
    /* A read's ready callback that issues a command (CdRead's ending
     * Pause): CD_cw comes this long after the sector's exception, before
     * the card driver's chain is installed (the boot's reads, 3336 [exc])
     * and BIOS_CARD_CHAIN_CYCLES later after it. */
    CD_DATA_CALLBACK_CW_CLOCKS = 3336,
    CD_ACK_IRQ_CLOCKS = 2400,
    CD_DONE_IRQ_CLOCKS = 2400,
    CD_CW_ACK_CYCLES = 2620,
    CD_DONE_IRQ_CYCLES = 2120,
    CD_INIT_CYCLES = 4680,
    /* CdReset(1)'s CD_initvol (the SPU volume writes and CdMix) after
     * CD_init's last acknowledge, to the caller's next call (SoundSystemInit:
     * DuckStation's Demute acknowledge to SsSetSerialAttr 3167 clocks, less
     * CD_CW_ACK_CYCLES) [mark] */
    CD_INITVOL_CYCLES = 547,
    /* CdControl, CdControlF, CdControlB around CD_cw: before it, and from
     * its return after an acknowledge to the caller [mark] */
    CD_CONTROL_CYCLES = 80,
    CD_CONTROL_RETURN_CYCLES = 150,
    /* CdRead's setup up to its first command, the call included [mark] */
    CD_READ_CYCLES = 770,
    /* CdSearchFile on a new disc, after each read (the read's return
     * included): the volume descriptor, the path table, the directory's
     * file records; then each name compared, and the found file's return
     * to the caller's next call [mark] */
    CD_SEARCH_PVD_CYCLES = 3215,
    CD_SEARCH_PATH_TABLE_CYCLES = 2561,
    CD_SEARCH_DIR_CYCLES = 14047,
    CD_SEARCH_CMP_CYCLES = 747,
    CD_SEARCH_FOUND_CYCLES = 2699,
    /* An MDEC macroblock (a block in the mono depths) leaves the decoder
     * about every 101 us with its DMA (2688 cycles per macroblock in
     * DuckStation's model and the DMA): the game's 320x176 movie frames,
     * 220 macroblocks, take 22.2 ms. [call] 8080ec2 (a frame's decode) */
    MDEC_UNIT_NS = 101000,
};

#endif
