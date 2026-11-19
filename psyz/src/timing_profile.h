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
    /* libetc's DMA dispatcher (DICR) on top of it: a DMA interrupt's
     * exception takes 2804 besides its callback's work on DuckStation
     * (the movie's MDEC output callbacks: 1883 to the callback, 945 from
     * its LoadImage's return, less the stubs' 24) against a root counter
     * interrupt's 2724 [exc] */
    DMA_IRQ_DISPATCH_CYCLES = 80,
    /* ... of which this much comes before the callback starts (the
     * MDEC output callback 1883 after the exception, its DecDCTout 2086
     * after it) [exc] */
    DEVICE_IRQ_TO_HANDLER_CYCLES = 1883,
    /* A DMA interrupt raised during another exception waits for libetc's
     * dispatcher, which takes it after that exception's handlers, within
     * the same exception. On DuckStation (the attract's movie, DSF_EXC:
     * an MDEC output interrupt due inside the vblank's, a root counter's or
     * the CD drive's exception) its callback starts about 300 clocks after
     * a vblank's or a root counter's handlers would have ended, 1700-2300
     * after a CD sector's (3110-clock deferred exceptions with the callback
     * 4850-5490 in), and the exception ends 4470 after the callback starts
     * (4300 standalone). [exc] */
    DMA_AFTER_LINE_TO_HANDLER_CYCLES = 300,
    DMA_AFTER_DEVICE_TO_HANDLER_CYCLES = 1900,
    DMA_NESTED_RETURN_CYCLES = 1091,
    /* libetc's dispatcher reads the interrupt status for the last time
     * this long before a DMA exception ends: a vblank 400 or less before
     * the end of an MDEC output exception raises its own (libpad's
     * exchange and the card driver's turn in it), one 566 or more before
     * is taken there (attract movie, DSF_EXC, DSF_PADLOG) [exc] */
    DMA_DISPATCH_EXIT_CLOCKS = 480,
    /* The card driver's chain (InitCARD) on every exception from then on:
     * a non-ticking RCnt interrupt takes 2748 in the menus [exc] */
    BIOS_CARD_CHAIN_CYCLES = 374,
    /* SetRCnt up to its last mode write, which starts the count (kernel.c
     * counts from where the CPU's work has got to, the handlers' time owed
     * included): the sequencer's timer then interrupts at DuckStation's
     * phase against the vblanks to within 40 clocks over the Grand Prix
     * menus (the timer's period from its RCNT2 mode write in
     * DUCKSTATION_CD_TRACE against the blank times) [field]; refit with
     * the boot's CD timing measured (tools/port/ds_frames.py marks): the
     * sequencer's interrupts within 15 clocks of DuckStation's exception
     * marks (DSF_EXC, Save and Load fields 200-1600, 2752 interrupts;
     * SsStart's entry to its last RCNT2 mode write itself 50 clocks there)
     * [exc] */
    SET_RCNT_CYCLES = 7,
    /* The vblank handler besides the kernel's: libetc's VSync handler and
     * callbacks dispatch (68), and the BIOS card driver's turn once
     * StartCARD ran (1409): a vblank with libpad's exchange of a digital
     * pad takes 20309 [exc] in the menus. */
    VBLANK_HANDLER_CYCLES = 68,
    CARD_VBLANK_TURN_CYCLES = 1409,
    /* VSync(0) and VSync(n)'s return once its loop sees the blank (after
     * the vblank's exception) or the interlaced field change: the vblank
     * exception's return to the caller's next call 110-119 clocks on
     * DuckStation (1366 Save and Load menu frames; PutDispEnv 125830 after
     * the blank when it waits 58 lines) [mark] (DSF_EXC, DSF_MARKS
     * PutDispEnv) */
    VSYNC_RETURN_CYCLES = 110,
    /* The vblank where a raw card access ends (Psyz_CardRawEndsAt): the
     * driver's handler there instead, and no libpad exchange: 4901 for the
     * whole exception [exc] the boot's card polling (DuckStation fields
     * 699-798) */
    CARD_RAW_END_CYCLES = 2109,
    /* libpad's controller exchange at each vblank once PadStartCom ran: its
     * SIO busy waits for a digital pad in port 1, port 2 empty [call]
     * 686859c; 4333 more with a pad in port 2: the vblank's exception
     * takes 24633 clocks against 20300 with one pad (DSF_EXC, the recompiler:
     * the Versus scenario with two saved cards, fields 800-829, against the
     * boot's 158-160; the breakpoint value was 4.8k [call] f0e3704). */
    LIBPAD_VBLANK_CYCLES = 16084,
    LIBPAD_VBLANK_PORT2_CYCLES = 4333,
    /* A digital pad in port 1 does not acknowledge a command other than
     * 0x42 (libpad's 0x43 while it probes for an analog controller after
     * PadStartCom): the exchange stops at the ID byte and the vblank's
     * exception takes 18688 clocks against 20300 for a 0x42 exchange
     * (DSF_EXC, DuckStation fields 146-157 against 158-160 of the boot). */
    LIBPAD_REFUSED_COMMAND_SAVED_CYCLES = 1612,
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
     * fields; 58 clocks less since the sequencer's interrupts come at
     * DuckStation's exception marks (SET_RCNT_CYCLES), where they came 58
     * before: DuckStation's libpad misses a blank 1043 clocks after such
     * a mark, the card driver one 1170 after it but not 1087 (Save and
     * Load, DSF_EXC, DSF_PADLOG, DSF_CARDPORT) */
    BIOS_CHAIN_PAD_TEST_CLOCKS = 1056 - 58,
    BIOS_CHAIN_CARD_TEST_CLOCKS = 1197 - 58,
    /* A device's exception (the CD drive's) reaches those tests this much
     * sooner: the root counters' handlers, which deliver their event in a
     * counter's exception, only test theirs. On DuckStation a blank 845 to
     * 2899 clocks into a CD exception has neither libpad's exchange nor a
     * card driver turn, one 365 to 414 clocks into it both (the card
     * scenarios of tools/port/ds_compare.py, DSF_EXC, DSF_PADLOG,
     * DSF_CARDPORT): the tests come between, here 637 and 496 [exc] */
    BIOS_RCNT_CHAIN_CYCLES = 560 - 58,
    /* ... refit: on DuckStation a blank 337 clocks or less before the end
     * of a root counter's or the CD drive's exception raises its own
     * (libpad's exchange and the card driver's turn in it), one 595 or
     * more before it is taken in it (DSF_EXC: Save and Load, Load a saved
     * card, the Garage's save; 56 blanks); against the port's windows of
     * those exceptions (a ticking sequencer's about 30 clocks short of
     * DuckStation's) a blank 301 before the end raises its own and ones
     * 356 and 410 before it do not (the Garage's save vsyncs 4492 and
     * 4361, Save and Load's 4230) [exc] */
    LIBETC_DISPATCH_EXIT_CLOCKS = 330,
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
    /* PutDispEnv sets the display mode only when the display area or the
     * interlace flags change (a double-buffered screen, every call: the
     * Time Attack's 4941); the 480-line menus keep theirs and take 4620
     * [mark] (DSF_MARKS PutDispEnv, PutDrawEnv: Save and Load, 1278 of
     * 1391 frames) */
    PUT_DISP_ENV_CYCLES = 603 - 321,
    PUT_DISP_ENV_MODE_CYCLES = 321,
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
    /* _SsVmFlush with its two SpuSetKey calls, and 11 more for each voice
     * silent for the last 15 ticks (its envelope history), which it
     * releases: DuckStation's boot ticks take 5495 before that history
     * fills (port vsyncs 131-142) and 5759 with all 24 voices silent (from
     * 143), _SsVmFlush:ret marks [mark]; the menus' 5544 + 2 * 52 above
     * had about 14 of them silent. */
    SS_VM_FLUSH_SILENT_VOICE_CYCLES = 11,
    SS_VM_FLUSH_CYCLES = 5494,
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
     * (tools/port/ds_frames.py marks); on the CPU-position clock the
     * movie's first Setloc acknowledge comes 1.2k clocks before
     * DuckStation's (47026 put it 34.8k after). */
    ST_POLL_MILLICYCLES = 47017,
    /* Another drive query in a busy loop (CdReady, the wait for a read's
     * Pause), interrupts included. [unrecorded] 8080ec2 */
    CD_POLL_NS = 2000,
    /* A poll of CdSync(1) and of CdReadSync(1) (LIBCD's own code, its
     * VSync(-1) reads included), and a CdReadSync(0) loop's turn: 380 and
     * 686 clocks on DuckStation in the menus' CdFileStep and the boot's
     * reads (LoadR4Directory's CdReadSync(1) loop 709 a turn, the caller's 20) [mark]
     * (DSF_MARKS CdSync:ret, CdReadSync:ret: Save and Load) */
    CD_SYNC_POLL_CYCLES = 374,
    /* ... of which a turn of CdReadSync's loop (CdReadSync(0) repeats it;
     * its two VSync(-1) calls 124 and 135 clocks apart, marked), the
     * CdReady(1) after it, up to its last look at the read's Pause, and
     * the return: LoadR4Directory's CdReadSync(1) during which that Pause
     * completes 640 clocks into it returns 69 clocks after the
     * completion's exception, the return mark's stub included [mark] */
    CD_READ_SYNC_TURN_CYCLES = 241,
    CD_READ_SYNC_END_CYCLES = 680 - 241 - 45,
    CD_READ_SYNC_RETURN_CYCLES = 45,
    /* LIBCD's CD_cw around a command: from its call to the command
     * register write, and from the acknowledge (the interrupt, the result
     * read, the return) to the caller; and the completion interrupt's
     * handling before a wait for it returns (Init, Pause). CD_init's own
     * setup before its first command (the controller's registers and
     * ResetCallback). Entry-to-entry clocks of the boot's commands on
     * DuckStation's recompiler [mark] (DUCKSTATION_CD_TRACE, a stub at each
     * function entry writing root counter 2 before SsStart). */
    CD_CW_ISSUE_CYCLES = 480,
    /* ... and each parameter byte CD_cw writes before the command: on
     * DuckStation a Setloc's acknowledge comes 25622-25640 clocks after
     * CD_cw, a Setmode's 25533, a ReadN's or a Pause's 25479-25513 [mark]
     * (DSF_MARKS CD_cw, DSF_EXC: the boot's and the menus' reads) */
    CD_CW_PARAM_CYCLES = 55,
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
    /* The sector's DMA to RAM (CdGetSector, 512 words: 544 clocks on
     * DuckStation) ends 23 clocks before that exception does; a blank
     * during it reaches the CPU only after it and is not hidden
     * (DuckStation's vblank event at the unformatted scenario's port vsync
     * 192 comes 243 clocks late, at the DMA's end, and neither libpad nor
     * the card driver misses it). */
    CD_DATA_DMA_TAIL_CLOCKS = 544 + 23,
    /* A read's ready callback that issues a command (CdRead's ending
     * Pause): CD_cw comes this long after the sector's exception, before
     * the card driver's chain is installed and BIOS_CARD_CHAIN_CYCLES later
     * after it. On DuckStation the Pause's acknowledge comes 28848-28890
     * clocks after the sector's interrupt in the boot's reads and 29221-
     * 29269 in the opening and the menus, the acknowledge's 25000 included
     * [exc] (DSF_EXC; the boot against the port's sector times, which
     * match DuckStation's to 50 clocks in the menus). */
    CD_DATA_CALLBACK_CW_CLOCKS = 3864,
    /* A movie's stream sector (CdRead2, StCdInterrupt as its ready
     * callback): the exception 3110 clocks with the card driver's chain
     * when StCdInterrupt leaves the sector for the MDEC callback (the
     * output DMA under way), 5040 when it copies the sector into the ring,
     * 5436 when that completes a frame; the copy in the MDEC callback
     * lengthens its slice by the same (DuckStation's slices 32.5k apart,
     * 34.5k with it). [exc] attract movie fields 956-1853, 824, 842 and 245
     * exceptions (medians) */
    ST_SECTOR_IRQ_CLOCKS = 2736,
    ST_COPY_CYCLES = 1930,
    ST_FRAME_DONE_CYCLES = 396,
    CD_ACK_IRQ_CLOCKS = 2400,
    CD_DONE_IRQ_CLOCKS = 2400,
    CD_CW_ACK_CYCLES = 2620,
    /* A Setloc's acknowledge before the card driver's chain is installed:
     * its exception 2928 clocks on DuckStation, the others' 2471; the
     * boot's CdSearchFile and LoadR4Directory reach their CdRead 375 and
     * 413 clocks later than the others' charges [exc] [mark] (2798 for
     * all of them with the chain, BIOS_CARD_CHAIN_CYCLES) */
    CD_CW_SETLOC_ACK_CYCLES = 394,
    /* ... and once the chain is installed: the acknowledges' exceptions
     * 2798 clocks on DuckStation, 327 more than before it [exc] */
    CD_CW_ACK_CHAIN_CYCLES = 327,
    CD_DONE_IRQ_CYCLES = 2120,
    /* CdRead's ending Pause: its completion's sync callback (restoring the
     * caller's) and the wait loop's return, more than CD_DONE_IRQ_CYCLES;
     * the Pause done to the next read's Setloc acknowledge of the boot's
     * directory reads on DuckStation (31143 clocks; the commands and
     * CD_SEARCH_* between them as charged) [exc] */
    CD_READ_PAUSE_CALLBACK_CYCLES = 220,
    CD_INIT_CYCLES = 4680,
    /* CdReset(1)'s CD_initvol (the SPU volume writes and CdMix) after
     * CD_init's last acknowledge, to the caller's next call (SoundSystemInit:
     * DuckStation's Demute acknowledge to SsSetSerialAttr 3167 clocks, less
     * CD_CW_ACK_CYCLES) [mark] */
    CD_INITVOL_CYCLES = 547,
    /* CdControl, CdControlF, CdControlB around CD_cw: before it, and from
     * its return after an acknowledge to the caller [mark] */
    CD_CONTROL_CYCLES = 73,
    CD_CONTROL_RETURN_CYCLES = 150,
    /* CdControlF's: to CD_cw (CdControl's 73 and CdControlB's 75 on
     * DuckStation, CdControlF's 61), and from the command register write
     * to its caller (CdFileStep's Setloc: CD_cw to the caller's next call
     * 773, CD_cw's write 645 in, CdFileStep's return 51) [mark] */
    CD_CONTROL_F_CYCLES = 61,
    CD_CONTROL_F_RETURN_CYCLES = 77,
    /* CdRead2's own code from its Setmode's return to its ReadS's CdControl
     * (the stream mode, CdDataCallback, CdReadyCallback): DuckStation's
     * Setmode acknowledge to ReadS's CD_cw 3234 clocks in the movies, less
     * CD_CW_ACK_CYCLES and CD_CW_ACK_CHAIN_CYCLES,
     * CD_CONTROL_RETURN_CYCLES and CD_CONTROL_CYCLES [mark] (DSF_MARKS
     * CdRead2, CdDataCallback, CdReadyCallback, CdControl, CD_cw: the
     * attract's and Versus's movie) */
    CD_READ2_CYCLES = 3234 - 2620 - 327 - 150 - 73,
    /* CdRead's setup up to its first command, the call included: 397
     * clocks to its Setmode's CdControl on DuckStation, 492 to its ReadN's
     * CdControlF when the mode stays; and from that CdControlF's return to
     * the caller (CD_cw to CdRead's return 777, CD_cw's write 480 in)
     * [mark] (DSF_MARKS CdRead:ret, CdControl, CdControlF, CD_cw: the
     * boot's reads and Save and Load's) */
    CD_READ_CYCLES = 397,
    CD_READ_KEEP_MODE_CYCLES = 95,
    CD_READ_END_CYCLES = 777 - 480 - 77,
    /* CdSearchFile on a new disc, after each read (from CdReadSync's
     * return): the volume descriptor, the path table, the directory's file
     * records; then each name compared, and the found file's return to the
     * caller's next call. DuckStation's CdReadSync return to the next
     * Setloc's CdControl: 2359, 1653, and 18889 to LoadR4Directory's
     * [mark] (DSF_MARKS CdReadSync:ret, CdControl: the boot) */
    /* CdSearchFile on a new disc before its first read: the caller's
     * return from CdInit (CD_initvol after its Demute) to CD_newmedia's
     * Setloc's CdControl, 3383 clocks from that Demute's acknowledge
     * exception on DuckStation (CD_CW_ACK_CYCLES, CD_INITVOL_CYCLES as
     * charged) [mark] */
    CD_SEARCH_NEWMEDIA_CYCLES = 345 - 129,
    CD_SEARCH_PVD_CYCLES = 2359,
    CD_SEARCH_PATH_TABLE_CYCLES = 1653,
    CD_SEARCH_DIR_CYCLES = 14047 - 845,
    CD_SEARCH_CMP_CYCLES = 747,
    CD_SEARCH_FOUND_CYCLES = 2699,
    /* The MDEC decodes a block in 448 clocks (DuckStation's mdec.cpp: a
     * colour macroblock is 6 blocks) and does not run ahead of its output
     * DMA: on DuckStation each of the attract movie's 16x176 slices (11
     * 24-bit macroblocks of 192 words) raises its DMA interrupt 30245-30750
     * clocks after the callback's call to output it (DSF_MARKS
     * ovl668_mdec_out, DSF_EXC; 2001 slices, mean 30500, hardly depending
     * on the slice's input: 0.05 clocks a byte), 2772 a macroblock, so 84
     * for its 192 words [exc] (attract fields 956-1099) */
    MDEC_BLOCK_CLOCKS = 448,
    MDEC_OUT_192_WORDS_CLOCKS = 84,
    /* The first output transfer after a decode's input began (DecDCTin)
     * raises its interrupt this much later than the others: a frame's
     * first slice 32406-32420 clocks after the movie player's call to
     * output it, the callbacks' 30449, in every frame of the attract's
     * movie [exc] */
    MDEC_FIRST_OUT_CLOCKS = 1961,
};

#endif
