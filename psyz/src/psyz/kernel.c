// PS1 kernel events, critical sections, root counters and the interrupt
// controller behind libetc's callbacks.
#ifdef __PSP__
#include <pspkernel.h>
#endif
#include <psyz.h>
#include <psyz/log.h>
#include <kernel.h>
#include <libapi.h>
#include <libetc.h>
#include <string.h>
#include <stdlib.h>
#include "os.h"
#include "../internal.h"
#include "../timing_profile.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
typedef volatile long AtomicWord;
typedef volatile __int64 AtomicTime;
static inline uint64_t AtomicTimeLoad(AtomicTime* p) {
    return (uint64_t)_InterlockedCompareExchange64(p, 0, 0);
}
static inline void AtomicTimeStore(AtomicTime* p, uint64_t v) {
    /* Exchange64 is not available on 32-bit MSVC; cmpxchg8b is. */
    uint64_t previous = AtomicTimeLoad(p);
    for (;;) {
        uint64_t observed = (uint64_t)_InterlockedCompareExchange64(
            p, (__int64)v, (__int64)previous);
        if (observed == previous)
            break;
        previous = observed;
    }
}
static inline long AtomicLoad(AtomicWord* p) { return _InterlockedOr(p, 0); }
static inline void AtomicStore(AtomicWord* p, long v) {
    _InterlockedExchange(p, v);
}
static inline int AtomicCas(AtomicWord* p, long expected, long desired) {
    return _InterlockedCompareExchange(p, desired, expected) == expected;
}
static inline void AtomicAdd(AtomicWord* p, long v) {
    _InterlockedExchangeAdd(p, v);
}
#else
typedef long AtomicWord;
typedef uint64_t AtomicTime;
static inline uint64_t AtomicTimeLoad(AtomicTime* p) {
#ifdef __PSP__
    /* Single-core Allegrex cannot emit 64-bit atomics. Keep the short
     * two-word access indivisible against interrupt/thread switches. */
    unsigned state = sceKernelCpuSuspendIntr();
    uint64_t value = *(volatile uint64_t*)p;
    sceKernelCpuResumeIntr(state);
    return value;
#else
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
#endif
}
static inline void AtomicTimeStore(AtomicTime* p, uint64_t v) {
#ifdef __PSP__
    unsigned state = sceKernelCpuSuspendIntr();
    *(volatile uint64_t*)p = v;
    sceKernelCpuResumeIntr(state);
#else
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
#endif
}
static inline long AtomicLoad(AtomicWord* p) {
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void AtomicStore(AtomicWord* p, long v) {
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
static inline int AtomicCas(AtomicWord* p, long expected, long desired) {
    return __atomic_compare_exchange_n(
        p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static inline void AtomicAdd(AtomicWord* p, long v) {
    __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}
#endif

#define EVENT_COUNT 16 // retail kernel default, see SetConf
#define EVENT_SLOT_MASK 0xFFFF

#define SYSTEM_CLOCK_HZ 33868800
#define DOT_CLOCK_HZ 6711647 // GPU clock / 8, the 320 pixel wide modes
#define HBLANK_NTSC_HZ 15732 // GPU clock / 3413 (NTSC) or / 3406 (PAL)
#define HBLANK_PAL_HZ 15620
#define MIN_PERIOD_NS 32000      // faster timers would only spin the host
#define MAX_CATCHUP_NS 200000000 // ticks owed past a longer host stall are lost

enum {
    IRQ_VBLANK,
    IRQ_GPU,
    IRQ_CDROM,
    IRQ_DMA,
    IRQ_RCNT0,
    IRQ_RCNT1,
    IRQ_RCNT2,
    IRQ_SIO0,
    IRQ_SIO,
    IRQ_SPU,
    IRQ_PIO,
    IRQ_COUNT,
};
#define IRQ_ALL ((1u << IRQ_COUNT) - 1)

typedef struct {
    AtomicWord status; // EvStUNUSED, EvStWAIT, EvStACTIVE or EvStALREADY
    unsigned int desc;
    unsigned int spec;
    long mode;
    long (*handler)();
} Event;

/* Times in ns; the exact time is ns + frac_acc / clock_hz ns (a period is
 * target / clock_hz s, period_ns and period_frac / clock_hz ns). */
typedef struct {
    uint64_t last_ns;   // when the count last restarted from 0
    uint64_t next_ns;   // when the count reaches the target
    uint64_t period_ns; // 0 until SetRCnt
    uint32_t period_frac;
    uint32_t frac_acc;
    uint32_t last_frac;
    uint32_t clock_hz;
    uint32_t target;
    int irq;
} RootCounter;

typedef struct {
    unsigned int desc;
    unsigned int spec;
} PendingEvent;

static Event events[EVENT_COUNT];
static AtomicWord event_waiters;

static void* kernel_lock;
/* Read before taking kernel_lock to recognize recursive entry. Volatile
 * does not synchronize this shared owner with the IRQ thread. */
static AtomicTime lock_owner;
static int lock_depth;
static int in_critical_section;
static int in_interrupt;

static void* bell_events;
static void* bell_irq;
static AtomicWord irq_thread_state; // 0: not started, 2: starting, 1: running, -1: polled

static unsigned irq_mask = 1u << IRQ_VBLANK;
static unsigned irq_latched;
static unsigned irq_saved_mask;
static int callbacks_running = 1;
static long rcnt_auto_ack[4] = {1, 1, 1, 1};
static RootCounter counters[3];
static AtomicTime poll_deadline_ns;

static PendingEvent pending[32];
static int pending_len;

static void* post_lock;
static void (*posted[16])(void);
static int posted_len;

static void (*vsync_callbacks[8])(void);
static void VSyncDispatch(void);
static void (*irq_callbacks[IRQ_COUNT])(void) = {VSyncDispatch};

/* Offline playback owns emulated time. Host scheduling must not change AKAO
 * timer counts or positions within the mixed PCM stream. Fixed at startup.
 *
 * The offline clock is the CPU's position, cpu_ns: every reading of the time
 * (Psyz_KernelNowUs/NowNs, Psyz_EmulatedClocks), every deadline (root
 * counters, timed completions, CPU stalls) and the blanks are on it. Time
 * passes only in the main thread's waits and spends (Psyz_KernelAdvanceToNs),
 * which stop at each deadline and blank on the way and run it there, at its
 * exact time; the audio is mixed up to the CPU's position before each
 * (whole samples, the SPU's own rate), so it follows the clock and never
 * sets it. */
static AtomicWord audio_clock = -1;
static AtomicTime cpu_ns;
static int advancing; // inside Psyz_KernelAdvanceToNs: a nested wait spends nothing
static AtomicWord host_clock_paused;
static AtomicWord host_clock_speed = 1;
/* Kernel time = wall time * speed - offset (modulo 2^64). */
static AtomicTime host_clock_frozen_us, host_clock_offset_us;

static int UsesAudioClock(void) {
    long mode = AtomicLoad(&audio_clock);
    if (mode < 0) {
        const char* clock = getenv("PSYZ_AUDIO_CLOCK");
        AtomicCas(&audio_clock, -1, clock && !strcmp(clock, "video"));
        mode = AtomicLoad(&audio_clock);
    }
    return (int)mode;
}

static uint64_t KernelNowUs(void) {
    if (UsesAudioClock())
        return AtomicTimeLoad(&cpu_ns) / 1000;
    if (AtomicLoad(&host_clock_paused))
        return AtomicTimeLoad(&host_clock_frozen_us);
    return Psyz_OsNowUs() * (uint64_t)AtomicLoad(&host_clock_speed) -
           AtomicTimeLoad(&host_clock_offset_us);
}

// The kernel's time in ns: the CPU's position, or the host clock's us.
static uint64_t KernelNowNs(void) {
    if (UsesAudioClock())
        return AtomicTimeLoad(&cpu_ns);
    return KernelNowUs() * 1000;
}

/* Emulated time for devices that run on their own clock (the CD drive): the
 * offline sample clock, or the host clock without pauses and at game speed,
 * like the root counters. */
uint64_t Psyz_KernelNowUs(void) { return KernelNowUs(); }

int Psyz_KernelOfflineClock(void) { return UsesAudioClock(); }

/* PS1 time of interrupt handlers (system clocks) not yet taken from the
 * main thread (Psyz_KernelInterruptCycles). */
static AtomicWord interrupt_debt;

static unsigned TakeInterruptDebt(void) {
    long debt = AtomicLoad(&interrupt_debt);
    while (debt && !AtomicCas(&interrupt_debt, debt, 0)) {
        debt = AtomicLoad(&interrupt_debt);
    }
    return (unsigned)debt;
}

void Psyz_KernelInterruptCycles(unsigned cycles) {
    if (!UsesAudioClock()) {
        return;
    }
    AtomicAdd(&interrupt_debt, (long)cycles);
}

unsigned Psyz_KernelTakeInterruptCycles(void) { return TakeInterruptDebt(); }

static int OwnsKernel(void);
static void SpendNs(uint64_t ns);

void Psyz_SpendTime(unsigned ns) {
    // a span measured on the PS1 includes the interrupts within it
    unsigned before = TakeInterruptDebt();
    SpendNs(ns);
    TakeInterruptDebt();
    if (before) {
        Psyz_KernelInterruptCycles(before);
    }
}

static uint64_t spend_deadline_ns;
static uint64_t spend_cycle_carry; // clocks * 1e9 not yet spent, below one ns
static void SpendNs(uint64_t ns) {
    if (UsesAudioClock()) {
        Psyz_AudioWaitNs(ns);
        return;
    }
    // A deadline already passed (other work ran since) restarts from now.
    uint64_t now_ns = KernelNowUs() * 1000;
    if (spend_deadline_ns < now_ns || spend_deadline_ns > now_ns + 1000000000) {
        spend_deadline_ns = now_ns;
    }
    spend_deadline_ns += ns;
    while (KernelNowUs() * 1000 < spend_deadline_ns) {
        Psyz_KernelPoll();
    }
}

static long handler_debt_base; // below
unsigned long long Psyz_EmulatedClocks(void) {
    if (!UsesAudioClock()) {
        return 0;
    }
    if (OwnsKernel() && handler_debt_base >= 0) {
        /* in a device's handler: where it has got to, not the main
         * thread's time owed (a movie slice callback's LoadImage DMA came
         * up to 30k clocks late, after its exception) */
        return Psyz_KernelCpuClocks();
    }
    uint64_t ns = AtomicTimeLoad(&cpu_ns);
    return ns * SYSTEM_CLOCK_HZ / 1000000000u +
           (uint64_t)AtomicLoad(&interrupt_debt);
}

uint64_t Psyz_KernelNowNs(void) { return KernelNowNs(); }

void Psyz_KernelWaitUntilNs(uint64_t at_ns) {
    if (!UsesAudioClock()) {
        return;
    }
    uint64_t now = AtomicTimeLoad(&cpu_ns) +
                   (uint64_t)AtomicLoad(&interrupt_debt) * 1000000000u / SYSTEM_CLOCK_HZ;
    if (at_ns <= now) {
        return;
    }
    if (OwnsKernel()) { // in a handler: it lengthens the handler
        Psyz_KernelInterruptCycles(
            (unsigned)((at_ns - now) * SYSTEM_CLOCK_HZ / 1000000000u));
        return;
    }
    Psyz_SpendTime((unsigned)(at_ns - now));
}

/* A wait the PS1 spins with the interrupts masked (libgpu's _addque2 polls
 * GPUSTAT's ready bit between SetIntrMask(0) and the restore): the
 * interrupts that come meanwhile are taken at its end. Inside a handler or
 * a critical section it is Psyz_KernelWaitUntilNs. */
static uint64_t masked_from_ns, masked_to_ns; // the latest masked wait

uint64_t Psyz_KernelUnmaskedNs(uint64_t at_ns) {
    return at_ns >= masked_from_ns && at_ns < masked_to_ns ? masked_to_ns : at_ns;
}

void Psyz_KernelWaitMaskedUntilNs(uint64_t at_ns) {
    if (!UsesAudioClock() || OwnsKernel()) {
        Psyz_KernelWaitUntilNs(at_ns);
        return;
    }
    uint64_t now = AtomicTimeLoad(&cpu_ns) +
                   (uint64_t)AtomicLoad(&interrupt_debt) * 1000000000u / SYSTEM_CLOCK_HZ;
    if (at_ns <= now) {
        return;
    }
    PS1_EnterCriticalSection();
    masked_from_ns = now;
    masked_to_ns = at_ns;
    Psyz_SpendCycles((unsigned)((at_ns - now) * SYSTEM_CLOCK_HZ / 1000000000u));
    PS1_ExitCriticalSection();
}

/* CPU stalls at their times (Psyz_KernelStallAt): a small queue in time
 * order, its head one of the kernel's deadlines (ServiceTimers). */
enum { STALLS_MAX = 8192 };
static struct {
    uint64_t at_ns;
    unsigned clocks;
} stalls[STALLS_MAX];
static unsigned stall_head, stall_len; // a ring, in time order
static void LowerPollDeadline(uint64_t at_ns);
static long handler_debt_base = -1; // the debt where a device handler began

/* The stalls already charged, for a device interrupt's exception recorded
 * after they came (StallsInWindow). */
enum { STALL_HISTORY = 4096 };
static struct {
    uint64_t at_ns;
    unsigned clocks;
} stall_history[STALL_HISTORY];
static unsigned stall_history_next;

static void NoteStall(uint64_t at_ns, unsigned clocks) {
    stall_history[stall_history_next].at_ns = at_ns;
    stall_history[stall_history_next].clocks = clocks;
    stall_history_next = (stall_history_next + 1) % STALL_HISTORY;
}

/* A device interrupt's exception from at_ns: the DMA stalls that come
 * while its handler runs lengthen it (as a root counter's, ServiceLine),
 * those charged already and those still due; it does not charge them. On
 * DuckStation the GPU's list DMA of a Save and Load menu frame holds a CD
 * acknowledge's exception 5040-5640 clocks before LIBCD's handler starts
 * (DSF_MARKS CdCommandCallback, DSF_EXC: 1800 without it). */
static unsigned StallsInWindow(uint64_t at_ns, unsigned cycles) {
    unsigned counted_queue = 0;
    uint64_t counted_end = at_ns;
    for (;;) {
        uint64_t end = at_ns + (uint64_t)cycles * 1000000000u / SYSTEM_CLOCK_HZ;
        unsigned more = 0;
        for (unsigned k = 0; k < STALL_HISTORY; k++) {
            if (stall_history[k].clocks && stall_history[k].at_ns >= counted_end &&
                stall_history[k].at_ns < end && stall_history[k].at_ns >= at_ns) {
                more += stall_history[k].clocks;
            }
        }
        while (counted_queue < stall_len) {
            unsigned i = (stall_head + counted_queue) % STALLS_MAX;
            if (stalls[i].at_ns >= end) {
                break;
            }
            if (stalls[i].at_ns >= at_ns) {
                more += stalls[i].clocks;
            }
            counted_queue++;
        }
        counted_end = end;
        if (!more) {
            return cycles;
        }
        cycles += more;
    }
}

// The stalls due by now_ns become handler time owed.
static void ServiceStalls(uint64_t now_ns) {
    unsigned owed = 0;
    while (stall_len && stalls[stall_head].at_ns <= now_ns) {
        NoteStall(stalls[stall_head].at_ns, stalls[stall_head].clocks);
        owed += stalls[stall_head].clocks;
        stall_head = (stall_head + 1) % STALLS_MAX;
        stall_len--;
    }
    if (owed) {
        AtomicAdd(&interrupt_debt, (long)owed);
    }
}

void Psyz_KernelStallAt(uint64_t at_ns, unsigned clocks) {
    if (!UsesAudioClock() || !clocks) {
        return;
    }
    {
        static int trace = -1;
        if (trace < 0) {
            const char* v = getenv("PSYZ_STALL_TRACE");
            trace = v && *v == '1';
        }
        if (trace) {
            fprintf(stderr, "stall at %llu clocks %u\n",
                    (unsigned long long)(at_ns * SYSTEM_CLOCK_HZ / 1000000000u), clocks);
        }
    }
    // where the CPU has got to: in a handler, after the handler time so far;
    // a transfer it starts stalls it slice by slice, its code running in the
    // DMA's 100-clock halts
    uint64_t now = AtomicTimeLoad(&cpu_ns);
    if (OwnsKernel() && handler_debt_base >= 0) {
        long in = AtomicLoad(&interrupt_debt) - handler_debt_base;
        now += ((uint64_t)(in > 0 ? in : 0) + 200) * 1000000000u /
               SYSTEM_CLOCK_HZ;
    }
    if (at_ns <= now) {
        NoteStall(at_ns, clocks);
        AtomicAdd(&interrupt_debt, (long)clocks);
        return;
    }
    if (stall_len) {
        unsigned last = (stall_head + stall_len - 1) % STALLS_MAX;
        if (stalls[last].at_ns >= at_ns || stall_len == STALLS_MAX) {
            // out of order or full: fold into the latest (it comes later)
            stalls[last].clocks += clocks;
            return;
        }
    }
    unsigned slot = (stall_head + stall_len) % STALLS_MAX;
    stalls[slot].at_ns = at_ns;
    stalls[slot].clocks = clocks;
    stall_len++;
    if (stall_len == 1) {
        LowerPollDeadline(at_ns);
    }
}

/* CPU work: the interrupts that come meanwhile (and those owed by native
 * work before it) make it last longer. Inside a handler it lengthens the
 * handler. Offline clock only: on a real-time clock the native work takes
 * host time of its own and a busy wait would only lower the frame rate. */
void Psyz_SpendCycles(unsigned cycles) {
    if (!UsesAudioClock()) {
        return; // the host's own time stands for it on a real-time clock
    }
    if (OwnsKernel() && in_interrupt) {
        Psyz_KernelInterruptCycles(cycles);
        return;
    }
    /* The main thread's work, also while it holds the kernel lock (a
     * library's section the interrupts must not enter, a critical
     * section): the interrupts that come wait for its end. */
    do {
        uint64_t scaled = (uint64_t)cycles * 1000000000u + spend_cycle_carry;
        spend_cycle_carry = scaled % SYSTEM_CLOCK_HZ;
        // 64-bit: UINT32_MAX clocks are 126.8 s (BiosByteLoopTime)
        SpendNs(scaled / SYSTEM_CLOCK_HZ);
    } while ((cycles = TakeInterruptDebt()) != 0);
}

static void KernelInit(void) {
    if (kernel_lock) {
        return;
    }
    post_lock = Psyz_OsLockCreate();
    bell_events = Psyz_OsBellCreate();
    bell_irq = Psyz_OsBellCreate();
    kernel_lock = Psyz_OsLockCreate();
    if (!kernel_lock || !post_lock || !bell_events || !bell_irq) {
        ERRORF("cannot create the kernel synchronisation objects");
    }
}

static int OwnsKernel(void) { return AtomicTimeLoad(&lock_owner) == Psyz_OsThreadSelf(); }

static void KernelLock(void) {
    uintptr_t self = Psyz_OsThreadSelf();
    KernelInit();
    if (AtomicTimeLoad(&lock_owner) == self) {
        lock_depth++;
        return;
    }
    Psyz_OsLock(kernel_lock);
    AtomicTimeStore(&lock_owner, self);
    lock_depth = 1;
}

static void KernelUnlock(void) {
    if (--lock_depth > 0) {
        return;
    }
    AtomicTimeStore(&lock_owner, 0);
    Psyz_OsUnlock(kernel_lock);
}

static Event* EventAt(long event) {
    unsigned long slot = (unsigned long)event & EVENT_SLOT_MASK;
    return slot < EVENT_COUNT ? &events[slot] : NULL;
}

static void DeliverLocked(unsigned int desc, unsigned int spec) {
    int became_pending = 0;
    for (int i = 0; i < EVENT_COUNT; i++) {
        Event* e = &events[i];
        if (AtomicLoad(&e->status) != EvStACTIVE || e->desc != desc ||
            e->spec != spec) {
            continue;
        }
        if (e->mode == EvMdNOINTR) {
            became_pending |= AtomicCas(&e->status, EvStACTIVE, EvStALREADY);
        } else if (e->mode == EvMdINTR && e->handler) {
            e->handler();
        }
    }
    if (became_pending && AtomicLoad(&event_waiters)) {
        Psyz_OsBellRing(bell_events);
    }
}

/* The BIOS exception handler and libetc's dispatcher around a callback
 * take KERNEL_IRQ_CYCLES (timing_profile.h). */

/* The latest exception for an interrupt other than the vblank: when it
 * began on the emulated clock and the PS1 clocks of its handlers (offline
 * clock only, where handlers are charged). libetc's dispatcher (trapIntr)
 * runs after the BIOS's priority chains and rereads the interrupt status
 * until nothing is pending, so a vblank that arrives after the chains
 * tested for it and before that last read is handled and acknowledged
 * there: the chain handlers (libpad's exchange, the BIOS card driver's
 * turn) never see that blank (Psyz_KernelBlankInDispatch). */
enum { DISPATCH_RING = 8 };
static struct {
    uint64_t ns;
    unsigned cycles;
    int device;         // a device model's interrupt (Psyz_KernelDispatch)
    unsigned open_tail; // its last clocks, where a blank is not hidden
    unsigned exit; // libetc's last status read this long before its end (0:
                   // the caller's)
} dispatches[DISPATCH_RING];
static unsigned dispatch_next;
static int dispatch_seen;

/* The latest exception's end on the offline clock, and whether it was a
 * device's: the CPU runs that exception's handlers until then, so a DMA
 * interrupt raised meanwhile waits for libetc's dispatcher (libetc.c). */
static uint64_t exception_end_ns, exception_start_ns;
static int exception_end_device, exception_root_counter;

static void NoteException(uint64_t at_ns, unsigned cycles, int device) {
    uint64_t end = at_ns + (uint64_t)cycles * 1000000000u / SYSTEM_CLOCK_HZ;
    if (end >= exception_end_ns) {
        exception_end_ns = end;
        exception_end_device = device;
        exception_start_ns = at_ns;
        exception_root_counter = !device;
    }
}

uint64_t Psyz_KernelExceptionStartNs(int* root_counter) {
    if (root_counter) {
        *root_counter = exception_root_counter;
    }
    return UsesAudioClock() ? exception_start_ns : 0;
}

static uint64_t vblank_exception_ns, vblank_exception_end_ns;

void Psyz_KernelNoteException(uint64_t at_ns, unsigned cycles) {
    if (UsesAudioClock()) {
        NoteException(at_ns, cycles, 0);
        if (exception_start_ns == at_ns) {
            exception_root_counter = 0; // the vblank's
        }
        vblank_exception_ns = at_ns;
        vblank_exception_end_ns =
            at_ns + (uint64_t)cycles * 1000000000u / SYSTEM_CLOCK_HZ;
    }
}

void Psyz_KernelSettleWait(long debt_before, uint64_t start_ns,
                           uint64_t until_ns, uint64_t own_ns) {
    if (!UsesAudioClock()) {
        return;
    }
    long during = AtomicLoad(&interrupt_debt) - debt_before;
    if (during <= 0) {
        return;
    }
    // the latest end of the exceptions that began during the wait
    uint64_t end = until_ns;
    for (int i = 0; i < DISPATCH_RING; i++) {
        uint64_t at = dispatches[i].ns;
        if (dispatches[i].cycles && at >= start_ns && at < until_ns &&
            at != own_ns) {
            uint64_t e = at + (uint64_t)dispatches[i].cycles * 1000000000u /
                                  SYSTEM_CLOCK_HZ;
            if (e > end) {
                end = e;
            }
        }
    }
    if (vblank_exception_ns >= start_ns && vblank_exception_ns < until_ns &&
        vblank_exception_end_ns > end) {
        end = vblank_exception_end_ns;
    }
    long left = (long)((end - until_ns) * SYSTEM_CLOCK_HZ / 1000000000u);
    if (left > during) {
        left = during;
    }
    AtomicAdd(&interrupt_debt, left - during);
}

uint64_t Psyz_KernelExceptionEndNs(int* device) {
    if (device) {
        *device = exception_end_device;
    }
    return UsesAudioClock() ? exception_end_ns : 0;
}

long Psyz_KernelDebtClocks(void) { return AtomicLoad(&interrupt_debt); }

/* An interrupt raised while another's exception runs, before libetc's
 * dispatcher looks at the interrupt status for the last time, is served in
 * that exception (InOpenException): it does not take the kernel's entry and
 * return again (SHARED_EXCEPTION_SAVED_CYCLES). */
static int InOpenException(uint64_t at_ns) {
    for (int i = 0; i < DISPATCH_RING; i++) {
        unsigned exit = dispatches[i].exit ? dispatches[i].exit : LIBETC_DISPATCH_EXIT_CLOCKS;
        if (!dispatches[i].cycles || dispatches[i].cycles <= exit ||
            dispatches[i].ns > at_ns) {
            continue;
        }
        uint64_t last = dispatches[i].ns +
            (uint64_t)(dispatches[i].cycles - exit) * 1000000000u / SYSTEM_CLOCK_HZ;
        if (at_ns < last) {
            return 1;
        }
    }
    return 0;
}

static void RecordDispatchExit(uint64_t at_ns, unsigned cycles, int device,
                               unsigned exit) {
    at_ns = Psyz_KernelUnmaskedNs(at_ns); // its exception begins at the unmask
    if (!UsesAudioClock()) {
        return;
    }
    /* the CD drive's (Psyz_KernelDispatchTail); a DMA interrupt's handlers
     * (Psyz_KernelDeviceIrq) have their own measured time, which includes
     * the transfers they start */
    if (device && !exit) {
        /* raised while another exception runs, it is served at that
         * exception's end, in the same exception (the kernel takes every
         * pending interrupt before it returns): on DuckStation a blank
         * 6017 clocks after a stream sector's interrupt that came during
         * an MDEC output exception is hidden (Versus with two cards, vsync
         * 1142) */
        uint64_t start = at_ns;
        int shared = 0;
        for (int i = 0; i < DISPATCH_RING; i++) {
            if (!dispatches[i].cycles || dispatches[i].ns > at_ns) {
                continue;
            }
            uint64_t end = dispatches[i].ns + (uint64_t)dispatches[i].cycles *
                                                  1000000000u / SYSTEM_CLOCK_HZ;
            if (end > start && end - at_ns < 100000000u) {
                start = end;
                shared = dispatches[i].ns == at_ns && !dispatches[i].device;
            }
        }
        unsigned saved = shared ? SHARED_EXCEPTION_SAVED_CYCLES : 0;
        /* pending together when the exception began (a root counter's and
         * a CD sector's, both held by a masked wait): one exception, whose
         * entry and return the second does not repeat (DuckStation: the
         * Versus movie's stream sector exception 9708 clocks, 10831 with
         * the timer's interrupt in it, where alone it takes 2724) [exc] */
        if (saved && cycles > saved) {
            cycles -= saved;
            AtomicAdd(&interrupt_debt, -(long)saved);
        }
        cycles = StallsInWindow(start, cycles) +
                 (unsigned)((start - at_ns) * SYSTEM_CLOCK_HZ / 1000000000u);
    }
    static int trace = -1;
    if (trace < 0) {
        const char* v = getenv("PSYZ_IRQ_TRACE");
        trace = v && *v == '1';
    }
    if (trace) {
        fprintf(stderr, "irq %s at %llu clocks for %u\n",
                device ? "device" : "rcnt",
                (unsigned long long)(at_ns * SYSTEM_CLOCK_HZ / 1000000000u),
                cycles);
    }
    dispatches[dispatch_next].ns = at_ns;
    dispatches[dispatch_next].cycles = cycles;
    dispatches[dispatch_next].device = device;
    dispatches[dispatch_next].open_tail = 0;
    dispatches[dispatch_next].exit = exit;
    dispatch_next = (dispatch_next + 1) % DISPATCH_RING;
    dispatch_seen = 1;
    NoteException(at_ns, cycles, device);
}

static void RecordDispatch(uint64_t at_ns, unsigned cycles, int device) {
    RecordDispatchExit(at_ns, cycles, device, 0);
}

static void ServiceLine(int line, uint64_t at_ns) {
    long debt = AtomicLoad(&interrupt_debt);
    irq_latched &= ~(1u << line);
    unsigned entry = KERNEL_IRQ_CYCLES + (Psyz_CardChainInstalled() ? BIOS_CARD_CHAIN_CYCLES : 0);
    /* a blank during another interrupt's exception, before libetc's last
     * look at the status: served in it (DuckStation: VSync(0) returns 4209
     * clocks after a blank 1100 clocks into a root counter's exception,
     * two-cards vsync 904, where a separate exception after it would end
     * at 5824) [exc] */
    if (line == IRQ_VBLANK && InOpenException(at_ns)) {
        entry -= SHARED_EXCEPTION_SAVED_CYCLES;
    }
    Psyz_KernelInterruptCycles(entry);
    switch (line) {
    case IRQ_VBLANK:
        DeliverLocked(RCntCNT3, EvSpINT);
        DeliverLocked(HwVBLANK, EvSpTRAP);
        break;
    case IRQ_RCNT0:
    case IRQ_RCNT1:
    case IRQ_RCNT2:
        DeliverLocked(RCntCNT0 + (line - IRQ_RCNT0), EvSpINT);
        break;
    }
    if (irq_callbacks[line]) {
        irq_callbacks[line]();
    }
    if (line != IRQ_VBLANK) {
        unsigned window = (unsigned)(AtomicLoad(&interrupt_debt) - debt);
        // DMA stalls that come while the handler runs lengthen it
        while (stall_len &&
               stalls[stall_head].at_ns <=
                   at_ns + (uint64_t)window * 1000000000u / SYSTEM_CLOCK_HZ) {
            NoteStall(stalls[stall_head].at_ns, stalls[stall_head].clocks);
            AtomicAdd(&interrupt_debt, (long)stalls[stall_head].clocks);
            window += stalls[stall_head].clocks;
            stall_head = (stall_head + 1) % STALLS_MAX;
            stall_len--;
        }
        RecordDispatch(at_ns, window, 0);
    }
}

void Psyz_KernelDeviceIrq(void (*handler)(void), unsigned before,
                          unsigned after, unsigned exit_clocks) {
    if (!UsesAudioClock()) {
        handler();
        return;
    }
    uint64_t at_ns = AtomicTimeLoad(&cpu_ns);
    long outer = handler_debt_base;
    if (outer >= 0) { // inside another device handler: from where it got to
        long in = AtomicLoad(&interrupt_debt) - outer;
        at_ns += (uint64_t)(in > 0 ? in : 0) * 1000000000u / SYSTEM_CLOCK_HZ;
    }
    long debt = AtomicLoad(&interrupt_debt);
    handler_debt_base = debt;
    Psyz_KernelInterruptCycles(before);
    handler();
    Psyz_KernelInterruptCycles(after); // the return
    handler_debt_base = outer;
    unsigned window = (unsigned)(AtomicLoad(&interrupt_debt) - debt);
    RecordDispatchExit(at_ns, window, 1, exit_clocks);
}

uint64_t Psyz_KernelCpuClocks(void) {
    if (!UsesAudioClock()) {
        return 0;
    }
    uint64_t ns = AtomicTimeLoad(&cpu_ns);
    uint64_t clocks = ns * SYSTEM_CLOCK_HZ / 1000000000u;
    if (OwnsKernel() && handler_debt_base >= 0) {
        long in = AtomicLoad(&interrupt_debt) - handler_debt_base;
        if (in > 0) {
            clocks += (uint64_t)in;
        }
    }
    return clocks;
}

void Psyz_KernelExtendDispatch(uint64_t at_ns, int clocks) {
    if (!UsesAudioClock() || !dispatch_seen) {
        return;
    }
    at_ns = Psyz_KernelUnmaskedNs(at_ns); // as RecordDispatchExit
    for (unsigned k = 1; k <= DISPATCH_RING; k++) {
        unsigned i = (dispatch_next + DISPATCH_RING - k) % DISPATCH_RING;
        if (dispatches[i].ns != at_ns || !dispatches[i].device) {
            continue;
        }
        uint64_t old_end = at_ns + (uint64_t)dispatches[i].cycles *
                                       1000000000u / SYSTEM_CLOCK_HZ;
        long cycles = (long)dispatches[i].cycles + clocks;
        dispatches[i].cycles = cycles > 0 ? (unsigned)cycles : 0;
        if (clocks > 0) {
            // work after the sector's DMA (a stream sector's copy into the
            // ring): the DMA no longer ends the exception
            dispatches[i].open_tail = 0;
        }
        AtomicAdd(&interrupt_debt, (long)clocks);
        if (exception_end_ns == old_end) { // the latest: it ends sooner too
            exception_end_ns = 0;
        }
        NoteException(at_ns, dispatches[i].cycles, 1);
        return;
    }
}

void Psyz_KernelDispatch(uint64_t at_ns, unsigned cycles, unsigned charged) {
    Psyz_KernelDispatchTail(at_ns, cycles, charged, 0);
}

void Psyz_KernelDispatchTail(uint64_t at_ns, unsigned cycles, unsigned charged,
                             unsigned open_tail) {
    if (!UsesAudioClock()) {
        return;
    }
    Psyz_KernelInterruptCycles(charged);
    RecordDispatch(at_ns, cycles, 1);
    dispatches[(dispatch_next + DISPATCH_RING - 1) % DISPATCH_RING].open_tail = open_tail;
}

// The latest recorded dispatch that began by at_ns.
static int LatestDispatch(uint64_t at_ns) {
    int best = -1;
    for (int i = 0; i < DISPATCH_RING; i++) {
        if (dispatches[i].cycles && dispatches[i].ns <= at_ns &&
            (best < 0 || dispatches[i].ns > dispatches[best].ns)) {
            best = i;
        }
    }
    return best;
}

int Psyz_KernelLastDispatch(uint64_t at_ns, unsigned* since_clocks,
                            unsigned* cycles) {
    int i = dispatch_seen ? LatestDispatch(at_ns) : -1;
    if (i < 0) {
        return 0;
    }
    uint64_t clocks =
        (at_ns - dispatches[i].ns) * SYSTEM_CLOCK_HZ / 1000000000u;
    *since_clocks = clocks > UINT32_MAX ? UINT32_MAX : (unsigned)clocks;
    *cycles = dispatches[i].cycles;
    return 1;
}

int Psyz_KernelBlankInDispatch(uint64_t blank_ns, unsigned test_clocks,
                               unsigned exit_clocks) {
    if (!dispatch_seen || !UsesAudioClock()) {
        return 0;
    }
    for (int i = 0; i < DISPATCH_RING; i++) {
        unsigned exit = dispatches[i].exit ? dispatches[i].exit : exit_clocks;
        if (dispatches[i].ns > blank_ns || dispatches[i].cycles <= exit) {
            continue;
        }
        uint64_t clocks =
            (blank_ns - dispatches[i].ns) * SYSTEM_CLOCK_HZ / 1000000000u;
        /* the BIOS's root counter handlers come before the chains' vblank
         * tests in a root counter's exception, not in a device's */
        unsigned test = dispatches[i].device && test_clocks > BIOS_RCNT_CHAIN_CYCLES
                            ? test_clocks - BIOS_RCNT_CHAIN_CYCLES
                            : test_clocks;
        if (dispatches[i].open_tail > exit) {
            exit = dispatches[i].open_tail;
        }
        if (clocks > test && exit < dispatches[i].cycles &&
            clocks <= dispatches[i].cycles - exit) {
            return 1;
        }
    }
    return 0;
}

/* How far, in clocks, the blank is from flipping Psyz_KernelBlankInDispatch's
 * answer: its distance to the nearest test point or exit point of a dispatch
 * it falls in or near (diagnostics: PSYZ_PAD_TRACE), -1 when none is near. */
int Psyz_KernelBlankMargin(uint64_t blank_ns, unsigned test_clocks,
                           unsigned exit_clocks) {
    int best = -1;
    if (!dispatch_seen || !UsesAudioClock()) {
        return -1;
    }
    for (int i = 0; i < DISPATCH_RING; i++) {
        unsigned exit = dispatches[i].exit ? dispatches[i].exit : exit_clocks;
        if (dispatches[i].ns > blank_ns || dispatches[i].cycles <= exit) {
            continue;
        }
        uint64_t clocks =
            (blank_ns - dispatches[i].ns) * SYSTEM_CLOCK_HZ / 1000000000u;
        if (clocks > (uint64_t)dispatches[i].cycles + 1000) {
            continue;
        }
        unsigned test = dispatches[i].device && test_clocks > BIOS_RCNT_CHAIN_CYCLES
                            ? test_clocks - BIOS_RCNT_CHAIN_CYCLES
                            : test_clocks;
        if (dispatches[i].open_tail > exit) {
            exit = dispatches[i].open_tail;
        }
        long c = (long)clocks;
        long d1 = labs(c - (long)test);
        long d2 = labs(c - (long)(dispatches[i].cycles - exit));
        long d = d1 < d2 ? d1 : d2;
        if (best < 0 || d < best) {
            best = (int)d;
        }
    }
    return best;
}

static void DrainPosted(void) {
    for (;;) {
        void (*fn)(void) = NULL;
        Psyz_OsLock(post_lock);
        if (posted_len > 0) {
            fn = posted[0];
            posted_len--;
            memmove(posted, posted + 1, posted_len * sizeof(*posted));
        }
        Psyz_OsUnlock(post_lock);
        if (!fn) {
            return;
        }
        fn();
    }
}

// Runs every unmasked latched line, queued event and posted callback.
// Needs the kernel lock outside of a critical section.
static void ServicePending(void) {
    if (in_interrupt || in_critical_section) {
        return;
    }
    in_interrupt = 1;
    for (;;) {
        unsigned ready = irq_latched & irq_mask;
        if (ready) {
            int line = 0;
            while (!(ready & (1u << line))) {
                line++;
            }
            ServiceLine(line, KernelNowNs());
        } else if (pending_len > 0) {
            PendingEvent ev = pending[0];
            pending_len--;
            memmove(pending, pending + 1, pending_len * sizeof(*pending));
            DeliverLocked(ev.desc, ev.spec);
        } else if (posted_len > 0) {
            DrainPosted();
        } else {
            break;
        }
    }
    in_interrupt = 0;
}

static void LatchLine(int line) {
    KernelLock();
    irq_latched |= 1u << line;
    ServicePending();
    KernelUnlock();
}

void Psyz_KernelRaise(unsigned int desc, unsigned int spec) {
    KernelLock();
    if (pending_len < LEN(pending)) {
        pending[pending_len].desc = desc;
        pending[pending_len].spec = spec;
        pending_len++;
    } else {
        WARNF("dropped event %08X/%04X", desc, spec);
    }
    ServicePending();
    KernelUnlock();
}

static void StartIrqThread(void);
void Psyz_KernelPost(void (*fn)(void)) {
    KernelInit();
    Psyz_OsLock(post_lock);
    if (posted_len < LEN(posted)) {
        posted[posted_len++] = fn;
    } else {
        WARNF("dropped a posted interrupt");
    }
    Psyz_OsUnlock(post_lock);
    StartIrqThread();
    Psyz_OsBellRing(bell_irq);
}

void Psyz_KernelVBlank(void) {
    Psyz_KernelPoll();
    LatchLine(IRQ_VBLANK);
}

/* Library code that the PS1 ran atomically with respect to an interrupt
 * handler (libsnd's sound API against its sequencer tick, which only checks
 * _snd_ev_flag) holds the kernel lock: the interrupt callbacks, which run
 * under it on the timer thread, wait instead of running concurrently. A PS1
 * interrupt could not run in parallel, nor see the state half updated. */
void Psyz_KernelLockInterrupts(void) { KernelLock(); }

void Psyz_KernelUnlockInterrupts(void) { KernelUnlock(); }

void PS1_EnterCriticalSection(void) {
    if (OwnsKernel()) {
        return; // already in one, or inside an interrupt handler
    }
    KernelLock();
    in_critical_section = 1;
}

void PS1_ExitCriticalSection(void) {
    if (!OwnsKernel() || !in_critical_section) {
        return;
    }
    in_critical_section = 0;
    ServicePending();
    KernelUnlock();
}

long OpenEvent(unsigned long desc, long spec, long mode, long (*func)()) {
    long id = -1;
    KernelLock();
    for (int i = 0; i < EVENT_COUNT; i++) {
        Event* e = &events[i];
        if (AtomicLoad(&e->status) == EvStUNUSED) {
            e->desc = (unsigned int)desc;
            e->spec = (unsigned int)spec;
            e->mode = mode;
            e->handler = func;
            AtomicStore(&e->status, EvStWAIT);
            id = (long)(DescEV | (unsigned)i);
            break;
        }
    }
    KernelUnlock();
    return id;
}

long CloseEvent(long event) {
    Event* e = EventAt(event);
    if (!e) {
        return 0;
    }
    KernelLock();
    AtomicStore(&e->status, EvStUNUSED);
    KernelUnlock();
    return 1;
}

static long SetEnabled(long event, long status) {
    Event* e = EventAt(event);
    if (!e) {
        return 0;
    }
    KernelLock();
    if (AtomicLoad(&e->status) != EvStUNUSED) {
        AtomicStore(&e->status, status);
    }
    KernelUnlock();
    return 1;
}

long EnableEvent(long event) { return SetEnabled(event, EvStACTIVE); }

long DisableEvent(long event) { return SetEnabled(event, EvStWAIT); }

long TestEvent(long event) {
    Event* e = EventAt(event);
    if (!e) {
        return 0;
    }
    if (AtomicCas(&e->status, EvStALREADY, EvStACTIVE)) {
        return 1;
    }
    if (AtomicLoad(&irq_thread_state) < 0) {
        Psyz_KernelPoll();
    }
    return 0;
}

long WaitEvent(long event) {
    Event* e = EventAt(event);
    long ret;
    int resume_critical_section;
    if (!e) {
        return 0;
    }
    if (AtomicCas(&e->status, EvStALREADY, EvStACTIVE)) {
        return 1;
    }
    if (AtomicLoad(&e->status) != EvStACTIVE) {
        return 0;
    }
    // interrupts cannot be delivered to a thread sleeping in a critical section
    resume_critical_section = OwnsKernel() && in_critical_section;
    if (resume_critical_section) {
        PS1_ExitCriticalSection();
    }
    KernelInit();
    AtomicAdd(&event_waiters, 1);
    for (;;) {
        if (AtomicCas(&e->status, EvStALREADY, EvStACTIVE)) {
            ret = 1;
            break;
        }
        if (AtomicLoad(&e->status) != EvStACTIVE) {
            ret = 0;
            break;
        }
        if (AtomicLoad(&irq_thread_state) < 0) {
            Psyz_KernelPoll();
            Psyz_OsBellWait(bell_events, 1000);
        } else {
            Psyz_OsBellWait(bell_events, 100000);
        }
    }
    AtomicAdd(&event_waiters, -1);
    if (resume_critical_section) {
        PS1_EnterCriticalSection();
    }
    return ret;
}

void DeliverEvent(unsigned ev1, unsigned ev2) {
    KernelLock();
    DeliverLocked(ev1, ev2);
    KernelUnlock();
}

void UnDeliverEvent(unsigned ev1, unsigned ev2) {
    KernelLock();
    for (int i = 0; i < EVENT_COUNT; i++) {
        Event* e = &events[i];
        if (e->desc == ev1 && e->spec == ev2 && e->mode == EvMdNOINTR) {
            AtomicCas(&e->status, EvStALREADY, EvStACTIVE);
        }
    }
    KernelUnlock();
}

static int CounterLine(long spec) {
    unsigned i = (unsigned)spec & 0xFFFF;
    return i < 3 ? IRQ_RCNT0 + (int)i : i == 3 ? IRQ_VBLANK : -1;
}

static unsigned CounterClock(int i, long mode) {
    if (mode & RCntMdSC) {
        return SYSTEM_CLOCK_HZ;
    }
    switch (i) {
    case 0:
        return DOT_CLOCK_HZ;
    case 1:
        return GetVideoMode() == MODE_PAL ? HBLANK_PAL_HZ : HBLANK_NTSC_HZ;
    default:
        return SYSTEM_CLOCK_HZ / 8;
    }
}

static int IsDue(uint64_t deadline, uint64_t now) { return now >= deadline; }

static void CounterStep(RootCounter* c) {
    c->last_ns = c->next_ns;
    c->last_frac = c->frac_acc;
    c->next_ns += c->period_ns;
    c->frac_acc += c->period_frac;
    if (c->frac_acc >= c->clock_hz) {
        c->frac_acc -= c->clock_hz;
        c->next_ns++;
    }
}

// The count restarts from 0 at now_ns plus frac / clock_hz ns.
static void CounterRestart(RootCounter* c, uint64_t now_ns, uint32_t frac) {
    c->last_ns = now_ns;
    c->last_frac = frac;
    c->next_ns = now_ns + c->period_ns;
    c->frac_acc = frac + c->period_frac;
    if (c->frac_acc >= c->clock_hz) {
        c->frac_acc -= c->clock_hz;
        c->next_ns++;
    }
}

/* One-shot device completions on the emulated clock (an MDEC transfer, an
 * SPU DMA, a card frame, the CD drive's next interrupt), fired as interrupts
 * once their time comes, like the root counters. Each callback has at most
 * one pending deadline: posting a callback that is already queued moves its
 * deadline (a device's new transfer replaces the old one's end), so a stale
 * deadline can never end a later transfer early. The queue is sized for
 * every completion callback PsyZ has; overflowing it is a bug that would
 * hang the waiter, so it stops the program. */
#define TIMED_MAX 16
static struct {
    uint64_t at_ns;
    void (*fn)(void);
} timed[TIMED_MAX];
static int timed_len;

static void StartIrqThread(void);

// A deadline at at_ns: the next service comes by then.
static void LowerPollDeadline(uint64_t at_ns) {
    StartIrqThread();
    KernelLock();
    if (at_ns < AtomicTimeLoad(&poll_deadline_ns)) {
        AtomicTimeStore(&poll_deadline_ns, at_ns);
    }
    KernelUnlock();
    Psyz_OsBellRing(bell_irq);
}

void Psyz_KernelPostAt(uint64_t at_us, void (*fn)(void)) {
    Psyz_KernelPostAtNs(at_us * 1000, fn);
}

void Psyz_KernelPostAtNs(uint64_t at_ns, void (*fn)(void)) {
    StartIrqThread();
    KernelLock();
    int slot = 0;
    while (slot < timed_len && timed[slot].fn != fn) {
        slot++;
    }
    if (slot == TIMED_MAX) {
        ERRORF("timed interrupt queue full (%d callbacks)", TIMED_MAX);
        abort();
    }
    if (slot == timed_len) {
        timed_len++;
    }
    timed[slot].at_ns = at_ns;
    timed[slot].fn = fn;
    uint64_t deadline = AtomicTimeLoad(&poll_deadline_ns);
    if (at_ns < deadline) {
        AtomicTimeStore(&poll_deadline_ns, at_ns);
    }
    KernelUnlock();
    Psyz_OsBellRing(bell_irq);
}

void Psyz_KernelCancel(void (*fn)(void)) {
    KernelLock();
    for (int i = 0; i < timed_len; i++) {
        if (timed[i].fn == fn) {
            timed[i] = timed[--timed_len];
            break; // one slot per callback
        }
    }
    KernelUnlock();
}

// Fires the due one-shot completions, earliest first; returns 1 with the
// next one's time in *next_ns when some remain.
static int ServiceTimed(uint64_t now, uint64_t* next_ns) {
    for (;;) {
        int first = -1;
        for (int i = 0; i < timed_len; i++) {
            if (first < 0 || timed[i].at_ns < timed[first].at_ns) {
                first = i;
            }
        }
        if (first < 0) {
            return 0;
        }
        if (!IsDue(timed[first].at_ns, now) || in_critical_section) {
            *next_ns = timed[first].at_ns;
            return 1;
        }
        void (*fn)(void) = timed[first].fn;
        timed[first] = timed[--timed_len];
        fn();
    }
}

// Fires every timer interrupt that came due. Returns 0 when no timer is
// triggered, otherwise 1 with the earliest deadline in *deadline_ns.
static int ServiceTimers(uint64_t* deadline_ns) {
    int triggered = 0;
    KernelLock();
    uint64_t now = KernelNowNs();
    in_interrupt = 1;
    for (int i = 0; i < LEN(counters); i++) {
        RootCounter* c = &counters[i];
        unsigned bit = 1u << (IRQ_RCNT0 + i);
        if (!c->period_ns || !c->irq) {
            continue;
        }
        if (now > c->next_ns && now - c->next_ns > MAX_CATCHUP_NS) {
            CounterRestart(c, now, 0);
        }
        while (IsDue(c->next_ns, now)) {
            uint64_t due_ns = c->next_ns; // its exact time, to the ns
            CounterStep(c);
            if (irq_mask & bit) {
                ServiceLine(IRQ_RCNT0 + i, due_ns);
            } else {
                irq_latched |= bit;
            }
        }
        if (!triggered || c->next_ns < *deadline_ns) {
            *deadline_ns = c->next_ns;
            triggered = 1;
        }
    }
    if (UsesAudioClock()) {
        ServiceStalls(now);
        if (stall_len && (!triggered || stalls[stall_head].at_ns < *deadline_ns)) {
            *deadline_ns = stalls[stall_head].at_ns;
            triggered = 1;
        }
    }
    uint64_t timed_next;
    if (ServiceTimed(now, &timed_next) &&
        (!triggered || timed_next < *deadline_ns)) {
        *deadline_ns = timed_next;
        triggered = 1;
    }
    in_interrupt = 0;
    AtomicTimeStore(&poll_deadline_ns, triggered ? *deadline_ns : UINT64_MAX);
    ServicePending();
    KernelUnlock();
    return triggered;
}

static void IrqThread(void) {
    for (;;) {
        uint64_t deadline;
        if (!ServiceTimers(&deadline)) {
            Psyz_OsBellWait(bell_irq, PSYZ_OS_FOREVER);
            continue;
        }
        uint64_t now = KernelNowNs();
        if (deadline > now) {
            uint64_t left = (deadline - now + 999) / 1000;
            Psyz_OsBellWait(
                bell_irq, (uint32_t)(left < INT32_MAX ? left : INT32_MAX));
        }
    }
}

static void StartIrqThread(void) {
    KernelInit();
    /* Publish "starting" before the new thread can post another callback. */
    if (!AtomicCas(&irq_thread_state, 0, 2))
        return;
    if (UsesAudioClock()) {
        AtomicStore(&irq_thread_state, -1);
        return;
    }
    AtomicStore(&irq_thread_state,
                Psyz_OsThreadStart(IrqThread, "psyz_irq") ? -1 : 1);
}

/* Desktop modal/focus pauses exclude wall time from root counters too.
 * Freezing only video/audio leaves AKAO sequencing ahead on return. */
void Psyz_KernelPauseHostClock(int paused) {
    if (UsesAudioClock())
        return;
    KernelLock();
    if (paused && !AtomicLoad(&host_clock_paused)) {
        AtomicTimeStore(&host_clock_frozen_us, KernelNowUs());
        AtomicStore(&host_clock_paused, 1);
    } else if (!paused && AtomicLoad(&host_clock_paused)) {
        AtomicTimeStore(
            &host_clock_offset_us,
            Psyz_OsNowUs() * (uint64_t)AtomicLoad(&host_clock_speed) -
                AtomicTimeLoad(&host_clock_frozen_us));
        AtomicStore(&host_clock_paused, 0);
    }
    KernelUnlock();
    Psyz_OsBellRing(bell_irq);
}

/* Root counters run `speed` times faster from now on, without a jump.
 * Offline playback takes its time from the mixed audio instead. */
void Psyz_KernelSetHostClockSpeed(unsigned speed) {
    if (UsesAudioClock())
        return;
    KernelLock();
    if (AtomicLoad(&host_clock_paused)) {
        AtomicStore(&host_clock_speed, (long)speed);
    } else {
        uint64_t now = KernelNowUs();
        AtomicStore(&host_clock_speed, (long)speed);
        AtomicTimeStore(
            &host_clock_offset_us, Psyz_OsNowUs() * (uint64_t)speed - now);
    }
    KernelUnlock();
    Psyz_OsBellRing(bell_irq);
}

void Psyz_KernelPoll(void) {
    uint64_t deadline;
    if (!AtomicLoad(&irq_thread_state) || OwnsKernel() ||
        !IsDue(AtomicTimeLoad(&poll_deadline_ns), KernelNowNs())) {
        return;
    }
    ServiceTimers(&deadline);
}

/* The offline clock's only advance: from the CPU's position to at_ns, the
 * kernel's deadlines (the main thread's, outside a handler or critical
 * section, where they can run) and the platform's blanks on the way each
 * run at their own time, a deadline before a blank at the same time. The
 * audio is mixed up to each. A wait inside one of them (a handler's) spends
 * no time: the handler's time is owed instead. */
void Psyz_KernelAdvanceToNs(uint64_t at_ns) {
    if (!UsesAudioClock() || advancing) {
        return;
    }
    advancing = 1;
    for (;;) {
        uint64_t now = AtomicTimeLoad(&cpu_ns);
        uint64_t next = UINT64_MAX;
        if (AtomicLoad(&irq_thread_state) && !OwnsKernel()) {
            next = AtomicTimeLoad(&poll_deadline_ns);
        }
        uint64_t blank = Psyz_AudioNextBlankNs();
        uint64_t at = next < blank ? next : blank;
        if (at > at_ns) {
            break;
        }
        if (at > now) {
            Psyz_AudioMixTo(at);
            AtomicTimeStore(&cpu_ns, at);
        }
        if (next <= blank) {
            uint64_t deadline;
            ServiceTimers(&deadline);
        } else {
            Psyz_AudioBlankDue();
        }
    }
    if (at_ns > AtomicTimeLoad(&cpu_ns)) {
        Psyz_AudioMixTo(at_ns);
        AtomicTimeStore(&cpu_ns, at_ns);
    }
    advancing = 0;
}

/* The next kernel deadline (offline clock), for a wait that runs until a
 * timer fires; UINT64_MAX when there is none. It services the kernel's
 * queues first, which also drops a stale earlier deadline (a counter set
 * again later). */
uint64_t Psyz_KernelNextDeadlineNs(void) {
    if (!UsesAudioClock() || !AtomicLoad(&irq_thread_state) || OwnsKernel()) {
        return UINT64_MAX;
    }
    uint64_t deadline;
    return ServiceTimers(&deadline) ? deadline : UINT64_MAX;
}

long SetRCnt(long spec, unsigned short target, long mode) {
    int i = (int)(spec & 0xFFFF);
    if (i >= 3) {
        return 0;
    }
    Psyz_SpendCycles(SET_RCNT_CYCLES);
    StartIrqThread();
    KernelLock();
    RootCounter* c = &counters[i];
    uint32_t clock = CounterClock(i, mode);
    uint64_t scaled = (uint64_t)(target ? target : 0x10000) * 1000000000u;
    c->clock_hz = clock;
    c->target = target ? target : 0x10000;
    c->period_ns = scaled / clock;
    c->period_frac = (uint32_t)(scaled % clock);
    if (c->period_ns < MIN_PERIOD_NS) {
        c->period_ns = MIN_PERIOD_NS;
        c->period_frac = 0;
    }
    c->irq = (mode & RCntMdINTR) != 0;
    /* The count starts at the mode write: the first target comes a period
     * after it, to the clock (SetRCnt's own code up to that write is
     * SET_RCNT_CYCLES, spent before). On the offline clock that is the main
     * thread's position with the handlers' time owed (inside a critical
     * section it is owed). */
    uint64_t start_ns = KernelNowNs();
    if (UsesAudioClock()) {
        start_ns += (uint64_t)AtomicLoad(&interrupt_debt) * 1000000000u /
                    SYSTEM_CLOCK_HZ;
    }
    CounterRestart(c, start_ns, 0);
    KernelUnlock();
    LowerPollDeadline(c->next_ns);
    return 1;
}

long GetRCnt(long spec) {
    int i = (int)(spec & 0xFFFF);
    if (i >= 3) {
        return 0;
    }
    KernelLock();
    RootCounter* c = &counters[i];
    long value = 0;
    if (c->period_ns) {
        // the counts since the last restart, which came last_frac /
        // clock_hz ns after last_ns, modulo the target
        uint64_t now = KernelNowNs();
        uint64_t since = now > c->last_ns ? now - c->last_ns : 0;
        uint64_t sub = since % 1000000000u * c->clock_hz;
        int64_t ticks = (int64_t)(since / 1000000000u * c->clock_hz % c->target);
        ticks += sub >= c->last_frac
                     ? (int64_t)((sub - c->last_frac) / 1000000000u)
                     : -1;
        value = (long)((ticks % (int64_t)c->target + c->target) % c->target);
    }
    KernelUnlock();
    return value;
}

long ResetRCnt(long spec) {
    int i = (int)(spec & 0xFFFF);
    if (i >= 3) {
        return 0;
    }
    KernelLock();
    CounterRestart(&counters[i], KernelNowNs(), 0);
    KernelUnlock();
    LowerPollDeadline(counters[i].next_ns);
    return 1;
}

long StartRCnt(long spec) {
    int line = CounterLine(spec);
    if (line < 0) {
        return 0;
    }
    KernelLock();
    irq_mask |= 1u << line;
    ServicePending();
    KernelUnlock();
    Psyz_OsBellRing(bell_irq);
    return line != IRQ_VBLANK;
}

long StopRCnt(long spec) {
    int line = CounterLine(spec);
    if (line < 0) {
        return 0;
    }
    KernelLock();
    irq_mask &= ~(1u << line);
    KernelUnlock();
    return 1;
}

long ChangeClearRCnt(long t, long flag) {
    long prev;
    if (t < 0 || t >= LEN(rcnt_auto_ack)) {
        return 0;
    }
    prev = rcnt_auto_ack[t];
    rcnt_auto_ack[t] = flag;
    return prev;
}

static void VSyncDispatch(void) {
    for (int i = 0; i < LEN(vsync_callbacks); i++) {
        if (vsync_callbacks[i]) {
            vsync_callbacks[i]();
        }
    }
}

int VSyncCallbacks(int ch, void (*f)()) {
    void (*prev)(void);
    if (ch < 0 || ch >= LEN(vsync_callbacks)) {
        return 0;
    }
    KernelLock();
    prev = vsync_callbacks[ch];
    vsync_callbacks[ch] = f;
    KernelUnlock();
    return (int)(intptr_t)prev;
}

int VSyncCallback(void (*f)()) { return VSyncCallbacks(4, f); }

void* InterruptCallback(int irq, void (*cb)()) {
    void (*prev)(void);
    if (irq < 0 || irq >= IRQ_COUNT) {
        return NULL;
    }
    if (cb && irq >= IRQ_RCNT0 && irq <= IRQ_RCNT2) {
        StartIrqThread();
    }
    KernelLock();
    prev = irq_callbacks[irq];
    irq_callbacks[irq] = cb;
    if (cb) {
        irq_mask |= 1u << irq;
    } else {
        irq_mask &= ~(1u << irq);
    }
    ServicePending();
    KernelUnlock();
    Psyz_OsBellRing(bell_irq);
    return (void*)prev;
}

int SetIntrMask(int mask) {
    int prev;
    KernelLock();
    prev = (int)irq_mask;
    irq_mask = (unsigned)mask & IRQ_ALL;
    ServicePending();
    KernelUnlock();
    return prev;
}

int GetIntrMask(void) { return (int)irq_mask; }

int ResetCallback(void) {
    KernelLock();
    if (callbacks_running) {
        KernelUnlock();
        return 0;
    }
    memset(irq_callbacks, 0, sizeof(irq_callbacks));
    memset(vsync_callbacks, 0, sizeof(vsync_callbacks));
    // the fractions of time carried between spends start again (D10 list in
    // internal.h)
    spend_deadline_ns = 0;
    spend_cycle_carry = 0;
    irq_callbacks[IRQ_VBLANK] = VSyncDispatch;
    irq_mask = 1u << IRQ_VBLANK;
    irq_latched = 0;
    callbacks_running = 1;
    KernelUnlock();
    return 1;
}

int StopCallback(void) {
    KernelLock();
    if (!callbacks_running) {
        KernelUnlock();
        return 0;
    }
    irq_saved_mask = irq_mask;
    irq_mask = 0;
    irq_latched = 0;
    callbacks_running = 0;
    KernelUnlock();
    return 1;
}

int RestartCallback(void) {
    KernelLock();
    if (callbacks_running) {
        KernelUnlock();
        return 0;
    }
    irq_mask = irq_saved_mask;
    callbacks_running = 1;
    ServicePending();
    KernelUnlock();
    return 1;
}

int CheckCallback(void) { return OwnsKernel() && in_interrupt; }


