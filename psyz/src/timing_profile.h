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
 *   [marks]  entry-to-return clocks on DuckStation's exact timing
 *            (REGTEST_EXACT, the default reference), from ds_frames.py
 *            DSF_MARKS stubs at the call and its return (~15 clocks each).
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
    /* An interrupt taken in the ROM's code costs its uncached instruction
     * fetches again after the return (the pipeline's prefetch is lost):
     * on DuckStation's exact timing the AudioState bzero's 30 exceptions
     * (port vsyncs 130-139) make it 1239 clocks longer than on the
     * recompiler, which takes them between its blocks; with the same
     * exceptions' lengths (DSF_EXC) [trace] */
    BIOS_ROM_IRQ_REFETCH_CYCLES = 41,
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
     * less the 6 of the exception stubs' clocks that fall between those
     * marks (the vector's stub logs after 15 of its 19 instructions, the
     * return's after 1 of 4: 25.6 clocks a dispatch in all, the bzero's
     * span with and without them). With 2350 (all 25.6 taken out) the
     * bzero's span came 680 clocks short of DuckStation's unmarked one
     * (6018935 clocks from SsSetMVol to InitSceneOverlayState). */
    KERNEL_IRQ_CYCLES = 2368,
    /* libetc's DMA dispatcher (DICR) on top of it: a DMA interrupt's
     * exception takes 2804 besides its callback's work on DuckStation
     * (the movie's MDEC output callbacks: 1883 to the callback, 945 from
     * its LoadImage's return, less the stubs' 24) against a root counter
     * interrupt's 2724 [exc] */
    DMA_IRQ_DISPATCH_CYCLES = 80,
    /* An interrupt pending as another's exception begins, or a blank
     * that comes during one, is served in it: it adds 1123 clocks where
     * its own exception takes 2724 (a non-ticking root counter's with a
     * stream sector's, Versus movie vsyncs 845-869: 10831 against the
     * sector's 9708 alone; VSync(0) 1615 sooner after a blank inside a
     * root counter's exception, two-cards vsync 904) [exc] */
    SHARED_EXCEPTION_SAVED_CYCLES = 2724 - 1123,
    /* A CD interrupt raised during a DMA interrupt's exception after
     * libetc's dispatcher first looked at the status there
     * (CD_IN_DMA_FIRST_LOOK_CLOCKS) and before its last look is served by
     * the dispatcher in it, after the DMA's callback: a stream sector left
     * for the MDEC callback adds 803 clocks to an MDEC output exception,
     * 3110 alone (DuckStation: no exception of its own for a sector raised
     * up to 5606 clocks into the MDEC exception, one at its end from 5719
     * in) [exc] [trace] */
    CD_IN_DMA_EXCEPTION_SAVED_CYCLES = 3110 - 803,
    /* ... and a root counter's raised during the vblank's exception adds
     * 618 (a non-ticking one) or 6546 (a tick) to it, against 2741 and
     * 8660 alone (DSF_EXC, the unformatted card's opening movie poll,
     * vsyncs 455-743: 20 in 290 fields) [exc] */
    RCNT_IN_VBLANK_SAVED_CYCLES = 2115,
    /* A blank that comes in another interrupt's exception after the BIOS
     * chains' vblank tests (no libpad exchange, no card driver turn) adds
     * only libetc's dispatcher's vblank work to it: the exception ends
     * 172-225 clocks later than alone, the frame's PutDispEnv the same
     * (DSF_EXC, Versus with two cards: a ticking timer's exception 8845
     * and 8888 clocks with the blank 1388 and 5703 in, a stream sector's
     * 5608 with it 2144 in, vsyncs 1052, 1086, 1140), where the port
     * charged a shared exception's 1209 [exc] */
    HIDDEN_VBLANK_CYCLES = 190,
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
    /* In a root counter's exception libetc's dispatcher looks at the
     * interrupt status at points of its work, the output DMA's stalls
     * included: an MDEC output interrupt raised up to 1475 clocks in has
     * its DecDCTout 3520 in (before the timer's tick), one up to 2148 in
     * 5267 in, one up to 2460 in 5859 in, a later one after the exception
     * (300 after its end, as above). On DuckStation's exact timing
     * (DUCKSTATION_DMA_TRACE, all 25 scenarios' movies: 1600 slices raised
     * in a timer's exception, the output DMA's first block) the boundaries
     * move by up to 30 clocks with the output DMA's blocks before them and
     * the outputs come within 100 of these, ticking timer or not [trace] */
    DMA_RCNT_FIRST_LOOK_CLOCKS = 1475,
    DMA_RCNT_SECOND_LOOK_CLOCKS = 2148,
    DMA_RCNT_THIRD_LOOK_CLOCKS = 2460,
    DMA_RCNT_FIRST_OUT_CLOCKS = 3520,
    DMA_RCNT_SECOND_OUT_CLOCKS = 5267,
    DMA_RCNT_THIRD_OUT_CLOCKS = 5859,
    /* the movie player's MDEC callback to its DecDCTout (the game's
     * SLICE_CALLBACK_TO_DCTOUT_CYCLES) */
    DMA_CALLBACK_TO_OUT_CLOCKS = 203,
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
    /* A VSync(1) query in the game's frame loop (do {} while (VSync(1) <
     * threshold)): 99 clocks from one call to the next, and from the last
     * to its VSync(0) (DuckStation's exact timing, DSF_MARKS VSync: the
     * Garage's and Save and Load's frames). Each query spent a scanline
     * (2167 clocks) before, which put the VSync(0) of a frame that ends
     * 0.7k clocks before a blank (the Garage's history screen leaving for
     * the team car select, port vsync 3393) past it. [mark] */
    LIBETC_VSYNC_QUERY_CYCLES = 99,
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
    /* ... a pad in port 2 refusing it saves this much (both pads refusing:
     * 20823 against 24654, DSF_EXC, REGTEST_PAD2=1, boot fields 220-229) */
    LIBPAD_REFUSED_COMMAND_PORT2_SAVED_CYCLES = 2194,
    /* An empty port 1 (no controller acknowledges the address byte): the
     * vblank's exception takes 16548 clocks against 20330 with a digital
     * pad there (DSF_EXC, REGTEST_PAD1_TYPE=None, the boot's fields
     * 250-800; with no pad the libpad probe for analog modes sends nothing
     * either) */
    LIBPAD_EMPTY_PORT1_SAVED_CYCLES = 3782,
    /* Each halfword of a controller's packet past the first: libpad's
     * handler takes 22.0k clocks with a JogCon in port 1 (three halfwords)
     * against 15.6k with a digital pad. [rec] ds_irqlog, menus. On the
     * exact timing a DualShock's vblank exception takes 26779-26854 clocks
     * (25-clock SIO phase steps; DSF_EXC, the opening movie, its wait loop
     * and the menus), 3200 gave 26703 [exc] */
    LIBPAD_HALFWORD_CYCLES = 3253,
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
     * before. Refit to the unmarked references (exception marks delay the
     * chains' tests by their stubs: tools/port/DUCKSTATION.md), the 19
     * ds_compare scenarios' pad flag and card driver port against the
     * port's position of each blank in a root counter's exception: libpad
     * makes its exchange at every blank up to 1044 clocks in (1034: 10 of
     * 19 scenarios) and at none from 1067 (1100-1165: 6 of 54), the card
     * driver takes its turn up to 1143 (1165: 2 of 18, 1170: 2 of 5) and
     * none from 1175 [field]; 34 later since the exceptions take
     * KERNEL_IRQ_CYCLES' 18 more (the blanks' positions in the timer's
     * exceptions moved by 34). Refit to DuckStation's exact timing
     * (tools/port/DUCKSTATION.md; the recompiler entered the exceptions up
     * to 50 clocks late and the boot put the timer 1.9k clocks off): in
     * the 19 ds_compare scenarios libpad makes its exchange at every blank
     * up to 1179 clocks into a root counter's exception and at none from
     * 1207 (a ticking one's or not), the card driver takes its turn up to
     * 1278 and at none from 1311 [field]. Those were the port's positions,
     * with the boot putting the sequencer's timer 132 clocks early; on
     * DuckStation's own positions (DUCKSTATION_DMA_TRACE IRQX entries and
     * VBLANK ticks of the 25 scenarios, the timer now within 8 clocks of
     * DuckStation's phase) libpad exchanges up to 1056 clocks in and at
     * none from 1083, the card driver turns up to 1172 and at none from
     * 1179 [trace] */
    BIOS_CHAIN_PAD_TEST_CLOCKS = 1070,
    BIOS_CHAIN_CARD_TEST_CLOCKS = 1176,
    /* A device's exception (the CD drive's) reaches those tests this much
     * sooner: the root counters' handlers, which deliver their event in a
     * counter's exception, only test theirs. On DuckStation a blank 845 to
     * 2899 clocks into a CD exception has neither libpad's exchange nor a
     * card driver turn, one 365 to 414 clocks into it both (the card
     * scenarios of tools/port/ds_compare.py, DSF_EXC, DSF_PADLOG,
     * DSF_CARDPORT): the tests come between, here 637 and 496 [exc]. On
     * DuckStation's positions (IRQX of the 25 scenarios' CD exceptions)
     * libpad exchanges up to 523 clocks in and at none from 576, the card
     * driver turns up to 648 and at none from 711: 520 and 496 sooner than
     * in a root counter's [trace] */
    BIOS_RCNT_CHAIN_CYCLES = 510,
    /* ... refit: on DuckStation a blank 337 clocks or less before the end
     * of a root counter's or the CD drive's exception raises its own
     * (libpad's exchange and the card driver's turn in it), one 595 or
     * more before it is taken in it (DSF_EXC: Save and Load, Load a saved
     * card, the Garage's save; 56 blanks); against the port's windows of
     * those exceptions (a ticking sequencer's about 30 clocks short of
     * DuckStation's) a blank 301 before the end raises its own and ones
     * 356 and 410 before it do not (the Garage's save vsyncs 4492 and
     * 4361, Save and Load's 4230) [exc]. On DuckStation's exact timing
     * the blanks of the 19 ds_compare scenarios 200 clocks or less before
     * a root counter exception's end raise their own and those 220 or more
     * before it do not, but 210 here makes Save and Load and two cards
     * differ from DuckStation's fields 2603 and 2621 on (none with 330;
     * again with the card menus' CPU time charged: 13 scenarios match
     * against 17, decision differences 2997 against 968): kept for a
     * device's exception (a root counter's has its own, below). */
    LIBETC_DISPATCH_EXIT_CLOCKS = 330,
    /* ... in a root counter's exception: DuckStation's exact timing has
     * libpad's exchange (the blank's own exception) at every blank up to
     * 193 clocks before the end of one (the port's window: 2742 clocks
     * not ticking, 8663 ticking) and at none from 223 (ta-full, vs-full,
     * the 27 ds_compare scenarios; the timer's blank every 131 fields
     * moves 33 clocks through it) [field]; those were the port's
     * positions with the timer 134 clocks early (BIOS_CHAIN_*_TEST_CLOCKS).
     * With the timer at DuckStation's phase: DuckStation's vblank
     * exception follows the root counter's at once for blanks up to 339
     * clocks before its end (DUCKSTATION_DMA_TRACE IRQX, the 25 scenarios)
     * and 330 here has the fewest decision differences (610 against 670
     * with 210, 636 with 270, 612 with 350) [trace] */
    LIBETC_RCNT_DISPATCH_EXIT_CLOCKS = 330,
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
    /* The part of PutDrawEnv (and DrawOTag) before _addque2's exec (_cwc)
     * starts the packet's DMA: SetDrawEnv2, then _addque2 up to its
     * SetIntrMask(0) and one look at GPUSTAT (DSF_MARKS PutDrawEnv,
     * _addque2, SetIntrMask, _cwc: 586 + 142 + 87 + 12 for _cwc's stores,
     * every frame of the Versus movie, fields 841-869). Its BIOS memcpy
     * comes after the restore. [mark] */
    PUT_DRAW_ENV_TO_EXEC_CYCLES = 827,
    /* PutDispEnv sets the display mode only when the display area or the
     * interlace flags change (a double-buffered screen, every call: the
     * Time Attack's 4941); the 480-line menus keep theirs and take 4620
     * [mark] (DSF_MARKS PutDispEnv, PutDrawEnv: Save and Load, 1278 of
     * 1391 frames) */
    PUT_DISP_ENV_CYCLES = 603 - 321,
    PUT_DISP_ENV_MODE_CYCLES = 321,
    GET_ENV_CYCLES = 40,
    DRAW_OTAG_CYCLES = 468,
    DRAW_OTAG_TO_EXEC_CYCLES = 40 + 142 + 87 + 12, // as PutDrawEnv's [mark]
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
    /* _addque2 waits for an idle GPU with the interrupts masked and runs
     * its exec once the GPU is idle: the exec's DMA starts this much after
     * the drawing ends (the Versus movie: _cwc 61524 clocks after
     * PutDrawEnv's, its 480x256 fill 61448 and the packet's DMA 23), and
     * the mask is restored this much after the DMA's first slice (_cwc to
     * the restore 1059 with a 1000-clock slice; the exception of an
     * interrupt that came during the wait 93126 clocks after the blank,
     * DSF_EXC unmarked). [mark] */
    ADDQUE2_POLL_EXIT_CLOCKS = 42,
    ADDQUE2_RESTORE_CLOCKS = 45,
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
    /* SpuClearReverbWorkArea: each chunk's time besides its DMA, from
     * one chunk's DMA interrupt to the next's (_spu_t's waits, the DMA
     * event, the interrupt's exception); its 97 chunks (96 of 1 KiB, one
     * of 64 bytes) take 1.222M clocks in SsInit on DuckStation's exact
     * timing (DSF_MARKS SpuClearReverbWorkArea:ret: 1,221,696, the two
     * vblanks within it included; DUCKSTATION_DMA_TRACE) [exc] */
    SPU_CLEAR_CHUNK_CYCLES = 4528,
    /* ... the chunk's DMA interrupt's exception (kernel, libspu's DMA
     * handler and its event, WaitEvent's return), part of that, and its
     * DMA's clocks below SPU_DMA_CLOCKS_PER_BYTE: on DuckStation's exact
     * timing the 96 chunks of 1 KiB take 12640 clocks from interrupt to
     * interrupt and the last one of 64 bytes 5035, the exception 4.6k
     * (DSF_EXC, 4672 with the marks' own; DUCKSTATION_DMA_TRACE IRQX) [exc] */
    SPU_CLEAR_DMA_IRQ_CLOCKS = 4580,
    SPU_CLEAR_DMA_SAVED_PER_64_BYTES = 5,
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
     * own is the rest of a tick with no sequence playing: 5921 with the
     * flush's 5759 (all voices silent: SsSeqCalledTbyT:ret and
     * _SsVmFlush:ret, one stub each, the unformatted card scenario's
     * opening movie poll, vsyncs 494-520) [mark]. It was the attract
     * boot's 5895 less the menus' flush (5648), 85 clocks a tick long,
     * which made the movie's 290-field poll 30k clocks long. */
    SS_TICK_CYCLES = 5921 - 5759,
    /* ... and this much more for each open sequence (its tracks' flags
     * looked at). DuckStation's Save and Load ticks, a sequence open and
     * playing, take 359 of their own by the marks' accounting (6169 with
     * the flush's 5641 and _SsSndPlay's 132, 12.5 a stub); the menus'
     * parts (the flush, the key-ons) were fit with 247 in all, which this
     * keeps. [mark] */
    SS_TICK_OPEN_SEQUENCE_CYCLES = 247 - (5921 - 5759),
    /* _SsVmFlush with its two SpuSetKey calls, and 11 more for each voice
     * silent for the last 15 ticks (its envelope history), which it
     * releases: DuckStation's boot ticks take 5495 before that history
     * fills (port vsyncs 131-142) and 5759 with all 24 voices silent (from
     * 143), _SsVmFlush:ret marks [mark]; the menus' 5544 + 2 * 52 above
     * had about 14 of them silent. */
    SS_VM_FLUSH_SILENT_VOICE_CYCLES = 11,
    /* ... the flush's own part and its envelope read for each voice the
     * sequencer may use (_SsVmMaxVoice, SsSetReservedVoice): 3579 clocks
     * with the race's 4 and 5759 with 24, all silent (Time Attack, port
     * vsyncs 2210-3990, _SsVmFlush:ret marks against the port's dirty
     * masks there); each voice whose registers it rewrites takes 601 and
     * 37 for each register (the volume's two, the pitch, the address, the
     * ADSR's two): 675 a voice with its volume changed, 712 with its
     * pitch too, about 858 with all of them (the menus' 920 a voice was
     * an average; with it the race's ticks were 2k clocks long, which hid
     * blanks DuckStation's take) [mark] */
    SS_VM_FLUSH_CYCLES = 3143,
    /* ... of which this much comes after its key-on write (SpuSetKey(SPU_ON)'s
     * register write, its return, SpuSetReverbVoice, SpuSetNoiseVoice):
     * the write 729 clocks after DuckStation's SpuSetKey mark with the whole
     * flush charged before it (Time Attack, 7237 ticks, DSF_MARKS
     * SsSeqCalledTbyT,SpuSetKey against PSYZ_SPU_KEY_TIME), the write about
     * 25 clocks into SpuSetKey [mark] */
    SS_VM_FLUSH_TAIL_CYCLES = 704,
    SS_VM_FLUSH_ENVELOPE_CYCLES = 98,
    SS_VM_FLUSH_VOICE_CYCLES = 601,
    SS_VM_FLUSH_WRITE_CYCLES = 37,
    /* The utility calls the game's sound slots and player voices make for
     * each voice: SsUtChangePitch 365 when the voice plays the tone named
     * (the pitch worked out and stored) and 85 when it does not (a voice
     * the race has not keyed on: the menus', the attract demo's),
     * SsUtSetVVol 33, a voice's SsUtKeyOnV about 1620 (a slot's two key-ons
     * when its program changes: 7637 against 4398). DuckStation, a stub at
     * entry and return (12.5 clocks taken off each): Time Attack race fields
     * 3700-3800 and its menus 2620-2700 [mark] */
    SS_UT_CHANGE_PITCH_CYCLES = 365,
    SS_UT_CHANGE_PITCH_MISS_CYCLES = 85,
    SS_UT_SET_VVOL_CYCLES = 33,
    SS_UT_KEY_ON_V_CYCLES = 1620,
    /* SsUtKeyOn (the menus' sound effects, a voice allocated): 2726 and
     * 2812 clocks on DuckStation's exact timing, its two calls in the
     * Versus card screen's field 6800 (DSF_MARKS SsUtKeyOn:ret, DSF_EXC,
     * 12.5 a stub taken off), against the port's charges within it, 514
     * and 588 [mark] */
    SS_UT_KEY_ON_CYCLES = 2190,
    SS_SND_PLAY_CYCLES = 132,
    SS_GET_SEQ_DATA_CYCLES = 332,
    /* _SsVmKeyOn by its paths, fit to 1739 calls on DuckStation (Save
     * and Load, name:ret marks on it and each callee, less the marks'
     * stubs, 12.5 clocks each; the port's KeyOn state per call): its own
     * code with _SsVmVSetUp and the tone search's setup 829, each of the
     * program's tones the search looks at 51 and 8 more when the note is
     * not below the tone's range; for each tone keyed, _SsVmAlloc (a free
     * first voice), _SsVmDoAllocate, note2pitch and _SsVmKeyOnNow 1825, 74
     * for each voice the allocation scans past (13 more when it is
     * allocated but not keyed, 41 or 30 more when it ties the quietest
     * envelope and is older or not). Calls of 2.84k-3.86k (the 3313 flat
     * cost before) are then within 40 clocks in 80% of the calls, the
     * rest within 85 (the instruction cache, which this does not model),
     * besides the 3% an interrupt lengthens. [mark] */
    SS_VM_KEY_ON_CYCLES = 829,
    SS_VM_SELECT_TONE_CYCLES = 51,
    SS_VM_SELECT_TONE_IN_RANGE_CYCLES = 8,
    SS_VM_KEY_ON_TONE_CYCLES = 1825,
    SS_VM_ALLOC_SCAN_CYCLES = 74,
    SS_VM_ALLOC_SCAN_RESERVED_CYCLES = 13,
    SS_VM_ALLOC_SCAN_OLDER_CYCLES = 41,
    SS_VM_ALLOC_SCAN_NOT_OLDER_CYCLES = 30,
    SS_VM_KEY_OFF_CYCLES = 1234,
    /* SoundSystemInit's sequencer setup after CdReset, entry to entry of
     * the next call [mark] (tools/port/ds_frames.py, the boot):
     * SsSetSerialAttr 142, SsSetSerialVol 146, SsSetTableSize 911,
     * SsSetTickMode 81, and _SsStart's 1000-pass wait loop up to SetRCnt
     * 2633, the rest to the caller's next call 799: SetRCnt then lands on
     * DuckStation's clock. */
    SS_SET_SERIAL_ATTR_CYCLES = 142,
    SS_SET_MVOL_CYCLES = 120, // SsSetMVol:ret 132 with its stub [mark] the boot
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
    /* The BIOS file layer on a card file (B(32h) open, B(34h) read, B(35h)
     * write, B(36h) close: the kernel's 0x2958, 0x2b28, 0x2c94, 0x2e00),
     * CPU time besides the driver's turns. Opening an existing file for
     * reading 26163, for writing 21233; an FASYNC read's start 16957 and
     * write's 16575, each returning 0 to the game's retry loop, which calls
     * again (42 clocks) for another 1712 / 1611; a close after a create
     * 4102, after a read 1245, after a write 773. Creating a file returns
     * 12.5k clocks after the SIO interrupt of the BIOS's last probe of the
     * card, 18.1k after the vblank exception that starts it (Save and Load
     * 39.8k after its blank, the Garage 46.9k: its exception is 7.2k
     * longer). [marks] the save's CardWriteFile and Save and Load's
     * CardReadFile at port vsyncs 4412 and 5904, the Garage's at 5341 */
    BIOS_CARD_OPEN_READ_CYCLES = 26163,
    BIOS_CARD_OPEN_WRITE_CYCLES = 21233,
    BIOS_CARD_READ_START_CYCLES = 16957 + 42 + 1712,
    BIOS_CARD_WRITE_START_CYCLES = 16575 + 42 + 1611,
    BIOS_CARD_CLOSE_CREATED_CYCLES = 4102,
    BIOS_CARD_CLOSE_READ_CYCLES = 1245,
    BIOS_CARD_CLOSE_WRITE_CYCLES = 773,
    BIOS_CARD_CREATE_END_CYCLES = 18100,
};

/* ---- CD drive and MDEC (libcd.c, libpress.c) ---- */
enum {
    /* One StGetNext poll of the stream ring in a movie player's busy loop,
     * CPU time only (the interrupts that come meanwhile lengthen the
     * wait), in millicycles: the game spins 4,000,000 polls on an empty
     * ring before its first read, 290 fields with one pad, 292 with two.
     * On DuckStation (the unformatted card's opening movie, two marks
     * only: 197,847,069 clocks from ovl668_start_stream_playback to
     * ovl668_seek_and_read_stream; DSF_EXC: its 965 exceptions 9.76M) the
     * polls take 188.08M [mark] [exc]. (47017 had been refit against the
     * idle ticks' and the timer's interrupts in the vblank's exceptions'
     * excess, SS_TICK_CYCLES, RCNT_IN_VBLANK_SAVED_CYCLES.) */
    ST_POLL_MILLICYCLES = 47021,
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
    /* ... of which CdRead's ready callback (cb_read, its two CdGetSector
     * calls: the sector's header and 512 words) takes this much: a sector
     * whose ready callback is the program's own takes the exception without
     * it and the callback's time (the XA player's in a race, which fetches
     * 32 words: 230 clocks on the marks, CdAudioReadyCallback in the game;
     * the exception 3029 clocks against 4177 for a CdRead sector on
     * DuckStation, Time Attack race fields 3300-8100) [exc] [mark] */
    CD_READ_CALLBACK_CLOCKS = 1355,
    /* The sector's DMA to RAM (CdGetSector, 512 words: 544 clocks on
     * DuckStation) ends 23 clocks before that exception does; a blank
     * during it reaches the CPU only after it and is not hidden
     * (DuckStation's vblank event at the unformatted scenario's port vsync
     * 192 comes 243 clocks late, at the DMA's end, and neither libpad nor
     * the card driver misses it). */
    CD_DATA_DMA_TAIL_CLOCKS = 544 + 23,
    /* ... 17/16 clocks a word (16 x): a ready callback of the program's own
     * fetches less (the XA player's 32 words), so its exception's DMA tail
     * is that much shorter: on DuckStation's exact timing a blank 352 to
     * 496 clocks before the end of such a sector's exception (3029 clocks,
     * the race's XA stream) is taken in it (vs-full vsync 17367, ta-full,
     * the course scenarios), not in its own exception as within CdRead's
     * tail; so are ones 186 to 318 before it, which the port's exit
     * (LIBETC_DISPATCH_EXIT_CLOCKS) still leaves out [field] */
    CD_GETSECTOR_DMA_WORD_X16 = 17,
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
    /* ... and 455 more for a sector left for the MDEC callback: there its
     * DecDCTout comes 4471 clocks after the slice's exception (median of
     * 459 such slices, Versus with two cards, DUCKSTATION_DMA_TRACE: the
     * output DMA's first slice), 2086 without the copy [trace] */
    ST_DEFERRED_COPY_EXTRA_CYCLES = 455,
    ST_FRAME_DONE_CYCLES = 396,
    /* A stream sector whose interrupt comes in an MDEC output exception
     * before libetc's dispatcher first reads the status there (1750
     * clocks in: DuckStation copies a sector raised 1692 in, leaves one
     * raised 1805 in for the next callback) is served first, within the
     * exception: the callback's DecDCTout 3130 clocks later than alone
     * with the copy, 528 more when it came before the BIOS chains looked
     * (up to 734 in; from 790 not) [trace] (DUCKSTATION_DMA_TRACE,
     * DUCKSTATION_CD_SECTORS: the Versus movie with two cards, 30 such
     * sectors, clocks from the exception's entry) */
    CD_IN_DMA_FIRST_LOOK_CLOCKS = 1750,
    CD_IN_DMA_CHAIN_LOOK_CLOCKS = 762,
    CD_IN_DMA_FIRST_CYCLES = 3130 - ST_COPY_CYCLES,
    CD_IN_DMA_CHAIN_CYCLES = 528,
    CD_ACK_IRQ_CLOCKS = 2400,
    /* ... and for each byte of the first response past the status that
     * LIBCD's handler reads: a GetlocP's acknowledge (8 bytes) takes 3087
     * clocks on DuckStation against a ReadS's 2868 and the port's 2814
     * (Time Attack race, its GetlocP every frame) [exc] */
    CD_RESULT_BYTE_CLOCKS = 39,
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
    /* A command's completion (its second response) to the CD_sync(0)
     * loop's return: on DuckStation's exact timing an Init's completion to
     * the Demute's acknowledge 27695 clocks in SoundSystemInit's CdReset,
     * 27660 in the boot's CdInit, before SpuInit (CD_cw's issue and its
     * 25000 included; DSF_MARKS CD_sync:ret: the wait's return 2650 and
     * 2608 clocks after the completion) [exc] [mark] */
    CD_DONE_IRQ_CYCLES = 2222,
    CD_DONE_IRQ_BOOT_SAVED_CYCLES = 42,
    /* CdRead's ending Pause: its completion's sync callback (restoring the
     * caller's) and the wait loop's return, more than CD_DONE_IRQ_CYCLES;
     * the Pause done to the next read's Setloc acknowledge of the boot's
     * directory reads on DuckStation (31143 clocks; the commands and
     * CD_SEARCH_* between them as charged) [exc] */
    CD_READ_PAUSE_CALLBACK_CYCLES = 160,
    /* CD_init to its first CD_cw: 4681 clocks the boot's first call, 4617
     * a later one's (SoundSystemInit's CdReset: its ResetCallback finds
     * the callbacks set up) on DuckStation's exact timing [mark] */
    CD_INIT_CYCLES = 4680,
    CD_INIT_AGAIN_CYCLES = 4617,
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
     * Setmode acknowledge to ReadS's CD_cw 3234 clocks in the movies with
     * the marks (DSF_MARKS CdRead2, CdDataCallback, CdReadyCallback,
     * CdControl, CD_cw: the attract's and Versus's movie), 77 less unmarked
     * (DUCKSTATION_CD_TRACE: the Setmode's acknowledge to the ReadS's 28637
     * clocks in the Versus movie), less CD_CW_ACK_CYCLES and
     * CD_CW_ACK_CHAIN_CYCLES, CD_CONTROL_RETURN_CYCLES and
     * CD_CONTROL_CYCLES [trace]: less than nothing, so it is charged with
     * the Setmode's return (Psyz_CdControlThen; cdread2.c asserts the sum
     * stays a charge) */
    CD_READ2_CYCLES = 3234 - 77 - 2620 - 327 - 150 - 73,
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
    /* ... and a search in the directory already cached (CdAudioFindStreamFile's
     * R4.STR at the boot): on DuckStation's exact timing its CdControlB's
     * return to CdAudioInit's Setmode acknowledge 37342 clocks [trace]
     * (DSF_MARKS: CdSearchFile:ret 8493 with its marks' stubs, its
     * DS_searchdir 580, DS_cachefile 87 and the found name's return 3437
     * after its compare), 2156 more than the compares' charges [mark] */
    CD_SEARCH_CACHED_CYCLES = 2156,
    /* The MDEC decodes a block in 448 clocks (DuckStation's mdec.cpp: a
     * colour macroblock is 6 blocks, 2688) and starts a unit only once its
     * output FIFO is empty; the output DMA moves a 192-word macroblock in
     * 32-word blocks of 34 clocks (the RAM's 17 per 16 words), at most
     * 100 clocks a slice while the MDEC decodes, the DMA halted 100 clocks
     * after each (dma.cpp's SLICE_SIZE_WHEN_DECODING_MDEC, DMAHaltTicks):
     * 3 blocks, the halt, 3 blocks, and the next macroblock's decode from
     * the read that empties the FIFO, 2958 clocks a macroblock. On
     * DuckStation (DUCKSTATION_DMA_TRACE: MDEC copy-outs, the DMA's slices
     * and halts, two-cards and Versus with two cards, 2700 slices of 11
     * macroblocks) 2960 a macroblock, a slice's first 2650 after the
     * previous one's exception, its interrupt 304 after its last [trace] */
    MDEC_BLOCK_CLOCKS = 448,
    MDEC_DMA_BLOCK_CLOCKS = 34,
    MDEC_DMA_SLICE_CLOCKS = 100,
    MDEC_DMA_HALT_CLOCKS = 100,
    MDEC_UNIT_DMA_CLOCKS = 270, // a macroblock's DMA without a PS1 clock
    /* The decode's first macroblock reaches the FIFO this long after
     * DecDCTin: 2688 after the input DMA's start, which comes 135 clocks
     * after the movie player's call (DUCKSTATION_DMA_TRACE with DSF_MARKS on
     * its step and DecDCTin's caller: 181 from the step's entry, 7 of them
     * the second mark's stub, the step's own 46 before the call; the
     * opening movie's frames, 834-880) [trace] */
    MDEC_IN_TO_FIRST_UNIT_CLOCKS = 135 + 2688,
};

#endif
