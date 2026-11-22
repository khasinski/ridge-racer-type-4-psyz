// PS1 kernel events, critical sections, root counters and the interrupt
// controller behind libetc's callbacks, on the host's clock.
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
static void* bell_wait; // Psyz_KernelWaitUntilNs
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

/* The kernel's time. On a real-time clock it is the host's monotonic clock
 * without the pauses and at the game's speed; an interrupt thread runs the
 * root counters and the timed completions when their time comes.
 *
 * The offline clock (PSYZ_AUDIO_CLOCK=video, headless test runs) is a
 * deterministic field clock: it stands still while the game works and
 * moves only in the waits (VSync's for its blank, a poll of the line
 * counter, a wait for a stream's next sector: Psyz_KernelAdvanceToNs), which
 * run the root counters, the timed completions and the platform's blanks on
 * the way at their times, with the audio mixed up to each. */
static AtomicWord audio_clock = -1;
static AtomicTime clock_ns; // the offline clock
static int advancing; // inside Psyz_KernelAdvanceToNs
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
        return AtomicTimeLoad(&clock_ns) / 1000;
    if (AtomicLoad(&host_clock_paused))
        return AtomicTimeLoad(&host_clock_frozen_us);
    return Psyz_OsNowUs() * (uint64_t)AtomicLoad(&host_clock_speed) -
           AtomicTimeLoad(&host_clock_offset_us);
}

static uint64_t KernelNowNs(void) {
    if (UsesAudioClock())
        return AtomicTimeLoad(&clock_ns);
    return KernelNowUs() * 1000;
}

uint64_t Psyz_KernelNowUs(void) { return KernelNowUs(); }

uint64_t Psyz_KernelNowNs(void) { return KernelNowNs(); }

int Psyz_KernelOfflineClock(void) { return UsesAudioClock(); }

static void KernelInit(void) {
    if (kernel_lock) {
        return;
    }
    post_lock = Psyz_OsLockCreate();
    bell_events = Psyz_OsBellCreate();
    bell_irq = Psyz_OsBellCreate();
    bell_wait = Psyz_OsBellCreate();
    kernel_lock = Psyz_OsLockCreate();
    if (!kernel_lock || !post_lock || !bell_events || !bell_irq || !bell_wait) {
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

static void ServiceLine(int line) {
    irq_latched &= ~(1u << line);
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
}

/* Runs the callbacks posted so far; those they post wait for the next
 * service (a device whose completion starts another one cannot hold the
 * kernel in a loop). */
static void DrainPosted(void) {
    Psyz_OsLock(post_lock);
    int n = posted_len;
    Psyz_OsUnlock(post_lock);
    while (n-- > 0) {
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

// Runs every unmasked latched line and queued event, then the callbacks
// posted so far. Needs the kernel lock outside of a critical section.
static void ServicePending(void) {
    if (in_interrupt || in_critical_section) {
        return;
    }
    in_interrupt = 1;
    for (int drained = 0;;) {
        unsigned ready = irq_latched & irq_mask;
        if (ready) {
            int line = 0;
            while (!(ready & (1u << line))) {
                line++;
            }
            ServiceLine(line);
        } else if (pending_len > 0) {
            PendingEvent ev = pending[0];
            pending_len--;
            memmove(pending, pending + 1, pending_len * sizeof(*pending));
            DeliverLocked(ev.desc, ev.spec);
        } else if (!drained && posted_len > 0) {
            drained = 1;
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

/* At the outermost unlock of the main thread the interrupts held up meanwhile
 * (posted callbacks, events) run. */
void Psyz_KernelUnlockInterrupts(void) {
    if (lock_depth == 1 && !in_interrupt && !in_critical_section &&
        AtomicLoad(&irq_thread_state) < 0) {
        ServicePending();
    }
    KernelUnlock();
}

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


/* One-shot device completions on the kernel's clock (a paced CD read's
 * next sector, an SPU DMA's end), fired as interrupts once their time
 * comes, like the root counters. Each callback has at most one pending
 * deadline: posting a callback that is already queued moves its deadline,
 * so a stale deadline can never end a later transfer early. The queue is
 * sized for every completion callback PsyZ has; overflowing it is a bug
 * that would hang the waiter, so it stops the program. */
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
            CounterStep(c);
            if (irq_mask & bit) {
                ServiceLine(IRQ_RCNT0 + i);
            } else {
                irq_latched |= bit;
            }
        }
        if (!triggered || c->next_ns < *deadline_ns) {
            *deadline_ns = c->next_ns;
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

/* The interrupt thread runs on a real-time clock; the offline clock runs
 * the deadlines in its waits. */
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
 * Freezing only video/audio leaves the sequencer ahead on return. */
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

/* The offline clock's only advance: to at_ns, the kernel's deadlines (the
 * main thread's, outside a handler or critical section, where they can
 * run) and the platform's blanks on the way each run at their own time, a
 * deadline before a blank at the same time. The audio is mixed up to
 * each. */
void Psyz_KernelAdvanceToNs(uint64_t at_ns) {
    if (!UsesAudioClock() || advancing) {
        return;
    }
    advancing = 1;
    for (;;) {
        uint64_t now = AtomicTimeLoad(&clock_ns);
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
            AtomicTimeStore(&clock_ns, at);
        }
        if (next <= blank) {
            uint64_t deadline;
            ServiceTimers(&deadline);
        } else {
            Psyz_AudioBlankDue();
        }
    }
    if (at_ns > AtomicTimeLoad(&clock_ns)) {
        Psyz_AudioMixTo(at_ns);
        AtomicTimeStore(&clock_ns, at_ns);
    }
    advancing = 0;
}

/* A wait until at_ns on the kernel's clock: the offline clock moves there
 * (Psyz_AudioWaitNs, which starts it with the platform's audio); on a
 * real-time clock the host waits while the interrupt thread runs what
 * comes due. */
void Psyz_KernelWaitUntilNs(uint64_t at_ns) {
    if (UsesAudioClock()) {
        uint64_t now = AtomicTimeLoad(&clock_ns);
        if (at_ns > now) {
            Psyz_AudioWaitNs(at_ns - now);
        }
        return;
    }
    KernelInit();
    for (;;) {
        Psyz_KernelPoll();
        uint64_t now = KernelNowNs();
        if (now >= at_ns) {
            return;
        }
        /* The host's sleeps overshoot by up to a millisecond or two: sleep
         * to that margin before the time and spin the rest, so a wait ends
         * on time (a movie frame's due time against the field's blank). */
        if (at_ns - now > 2000000) {
            Psyz_OsBellWait(bell_wait, (uint32_t)((at_ns - now - 2000000) / 1000));
        }
    }
}

long SetRCnt(long spec, unsigned short target, long mode) {
    int i = (int)(spec & 0xFFFF);
    if (i >= 3) {
        return 0;
    }
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
    CounterRestart(c, KernelNowNs(), 0);
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


