// A small Horizon kernel: handles, threads, synchronization, memory, and the SVC dispatcher.
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hle.h"
#include "kernel.h"
#ifdef _WIN32
#include "platform/windows_threads.h"
#endif

// ------------------------------------------------------------------ results
#undef R_OK
#define R_OK 0
#define R_TIMEOUT 0xEA01
#define R_CANCELLED 0xEC01
#define R_INVALID_HANDLE 0xE401
#define R_INVALID_STATE 0xFA01
#define R_INVALID_ADDRESS 0xCC01
#define R_NOT_FOUND 0xF201
#define R_INVALID_ENUM 0xF001
#define R_OUT_OF_RESOURCE 0xD001

#define PSEUDO_THREAD 0xFFFF8000u
#define PSEUDO_PROCESS 0xFFFF8001u
#define WAITERS_BIT 0x40000000u

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static __thread KThread *t_cur;

// ------------------------------------------------------------------ handle table
#define MAX_HANDLES 4096
static KObject *handles[MAX_HANDLES];

static uint32_t handle_alloc(KObject *o) {
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (!handles[i]) {
            handles[i] = o;
            o->refs++;
            return 0x100 + i;
        }
    }
    FATAL("out of handles\n");
}

KObject *handle_get(uint32_t h) {
    if (h == PSEUDO_THREAD) return &t_cur->obj;
    if (h < 0x101 || h >= 0x100 + MAX_HANDLES) return NULL;
    return handles[h - 0x100];
}

uint32_t kernel_new_handle(KObject *o) {
    pthread_mutex_lock(&g_klock);
    uint32_t h = handle_alloc(o);
    pthread_mutex_unlock(&g_klock);
    return h;
}

static void handle_close(uint32_t h) {
    KObject *o = handle_get(h);
    if (!o || h == PSEUDO_THREAD) return;
    handles[h - 0x100] = NULL;
    o->refs--;
}

// Threads blocked in WaitSynchronization; object signals wake only these (caller holds g_klock).
static KThread *g_syncwaiters;

static int g_prof_cv = -1;  // LARECOMP_PROF_CONDVAR (svc_wait_process_wide_key below)
static void wake(KThread *t) {
    if (g_prof_cv > 0) t->woken_ns = plat_now_ns();
    pthread_cond_signal(&t->cond);
}

void kernel_signal(KObject *o) {
    // caller holds g_klock
    o->signaled = true;
    for (KThread *t = g_syncwaiters; t; t = t->snext) wake(t);
}

// ------------------------------------------------------------------ time

uint64_t rt_ticks(void) {
    // 19.2 MHz system tick
    uint64_t ns = plat_now_ns();
    static double scale = -1;
    if (scale < 0) scale = getenv("LARECOMP_TICK_SCALE") ? atof(getenv("LARECOMP_TICK_SCALE")) : 1.0;
    if (scale != 1.0) ns = (uint64_t)(ns * scale);
    return (uint64_t)((__uint128_t)ns * 192 / 10000);
}

static void deadline(struct timespec *ts, int64_t timeout_ns) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ns / 1000000000;
    ts->tv_nsec += timeout_ns % 1000000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}

// Sleep on the current thread's condition. Returns false on timeout. Caller holds g_klock.
static pthread_cond_t g_nothread_cond = PTHREAD_COND_INITIALIZER;
static bool kwait(int64_t timeout_ns, const struct timespec *ts) {
    pthread_cond_t *cv = t_cur ? &t_cur->cond : &g_nothread_cond;
    if (timeout_ns < 0) {
        pthread_cond_wait(cv, &g_klock);
        return true;
    }
    return pthread_cond_timedwait(cv, &g_klock, ts) == 0;
}

// ------------------------------------------------------------------ memory map
typedef struct { uint64_t addr, size; uint32_t type, perm; } Region;
static Region regions[1024];
static int nregions;
static uint64_t heap_size;
static uint64_t tls_next = TLS_BASE;
uint64_t g_program_id;  // from main.npdm's ACI0 (runtime/main.c)
static uint64_t g_user_exception_ctx;  // (HLE heap: wiped by a reboot, kernel_reset forgets it)
uint64_t kernel_heap_size(void) { return heap_size; }  // bytes from HEAP_BASE set by SetHeapSize
// Writable guest data regions (heap, mapped/physical memory, module .data): for debug/runtime memory scans.
int kernel_data_regions(uint64_t *addr, uint64_t *size, int max) {
    int n = 0;
    for (int i = 0; i < nregions && n < max; i++)
        if ((regions[i].perm & 2) && regions[i].type != 0xB && regions[i].type != 0xC) {
            addr[n] = regions[i].addr; size[n] = regions[i].size; n++;
        }
    return n;
}

static void region_add(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm) {
    if (nregions == 1024) FATAL("too many regions\n");
    plat_commit(addr, size);  // back it before the OS writes there (Windows: file reads into the heap)
    int i = nregions++;
    while (i > 0 && regions[i - 1].addr > addr) { regions[i] = regions[i - 1]; i--; }
    regions[i] = (Region){addr, size, type, perm};
}

static void region_remove(uint64_t addr) {
    for (int i = 0; i < nregions; i++) {
        if (regions[i].addr == addr) {
            memmove(&regions[i], &regions[i + 1], (nregions - i - 1) * sizeof(Region));
            nregions--;
            return;
        }
    }
}

static void query_memory(uint64_t addr, uint64_t out) {
    uint64_t prev_end = 0;
    uint64_t info[5] = {0};  // addr, size, type|attr<<32, perm|ipc<<32, device|pad<<32
    for (int i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        if (addr >= r->addr && addr < r->addr + r->size) {
            info[0] = r->addr;
            info[1] = r->size;
            info[2] = r->type;
            info[3] = r->perm;
            memcpy((void *)out, info, 0x28);
            return;
        }
        if (addr < r->addr) {
            info[0] = prev_end;
            info[1] = r->addr - prev_end;
            memcpy((void *)out, info, 0x28);
            return;
        }
        prev_end = r->addr + r->size;
    }
    info[0] = prev_end;
    info[1] = (1ull << 39) - prev_end;
    memcpy((void *)out, info, 0x28);
}

// ------------------------------------------------------------------ threads
static uint64_t next_thread_id = 0x50;

// ------------------------------------------------------------------ in-place reboot
// Recompiled guest code can't be killed from outside, so a reboot (runtime/main.c) sets g_rt_unwind, wakes every
// waiting thread, and each guest thread longjmps back to thread_main at its next safe point: rt_svc (entry and
// exit), an HLE call (runtime/dispatch.c), or a kernel wait (which returns early while g_rt_unwind is set). The
// frames dropped are recompiled code (its state lives in guest memory, which is wiped next) and HLE functions;
// none of those hold a host lock across a call into the guest, so nothing stays locked. svcExitThread ends a
// thread the same way.
volatile int g_rt_unwind;
static KThread *g_threads;  // every thread of this boot (g_klock)

void rt_unwind_point(void) {
    if (__builtin_expect(g_rt_unwind, 0) && t_cur) longjmp(t_cur->unwind, 1);
}

static void *thread_main(void *arg) {
    KThread *t = arg;
    t_cur = t;
    crash_handler_install();
    rt_set_fpcr(0);
    if (!setjmp(t->unwind)) guest_call(&t->cpu, t->entry);
    pthread_mutex_lock(&g_klock);
    t->finished = true;
    kernel_signal(&t->obj);
    pthread_mutex_unlock(&g_klock);
    t_cur = NULL;
    return NULL;
}

static KThread *thread_new(uint64_t entry, uint64_t arg, uint64_t stack_top, int prio, int core) {
    KThread *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cond, NULL);
    t->cur_svc = -1;
    t->obj.type = KOBJ_THREAD;
    t->entry = entry;
    t->prio = prio;
    t->core = core < 0 ? 0 : core;
    t->id = next_thread_id++;
    t->cpu.x[0] = arg;
    t->cpu.sp = stack_top & ~15ull;
    t->cpu.excl_addr = ~0ull;
    t->tlr = tls_next;
    tls_next += 0x200;
    t->cpu.tpidrro = t->tlr;
    t->cpu.thread = t;
    pthread_mutex_lock(&g_klock);
    t->anext = g_threads;
    g_threads = t;
    pthread_mutex_unlock(&g_klock);
    return t;
}

static void thread_start(KThread *t) {
#ifdef _WIN32
    t->host = guest_thread_create(thread_main, t, 256u << 20);
    if (!t->host) FATAL("Could not create a game thread.\n");
    t->started = true;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256ull << 20);  // guest recursion runs on the host stack
    t->started = true;
    pthread_create(&t->host, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
#endif
}

KThread *kernel_create_main_thread(uint64_t entry, uint64_t stack_size, int prio) {
    uint64_t stack = STACK_BASE + STACK_SIZE - ((stack_size + 0xFFFF) & ~0xFFFFull);
    region_add(stack, stack_size, 0xB, 3);
    KThread *t = thread_new(entry, 0, stack + stack_size, prio, 0);
    t->handle = kernel_new_handle(&t->obj);
    return t;
}

void kernel_run_main(KThread *t, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3) {
    t->cpu.x[0] = x0; t->cpu.x[1] = x1; t->cpu.x[2] = x2; t->cpu.x[3] = x3;
    thread_start(t);  // the host main thread goes on to run the window event loop
}

uint32_t kernel_current_thread_handle(void) { return t_cur ? t_cur->handle : 0; }
Cpu *kernel_current_cpu(void) { return t_cur ? &t_cur->cpu : NULL; }
KThread *kernel_current_thread(void) { return t_cur; }

// ------------------------------------------------------------------ mutex / condvar (Horizon semantics)
// Mutex word: owner handle | WAITERS_BIT. Waiting threads sleep on the global cond and are handed ownership.

static uint32_t *u32p(uint64_t a) { return (uint32_t *)(uintptr_t)a; }

// Try to take mutex `m` for `tag`, or mark it contended. Returns true if acquired. Uses CAS so a
// concurrent user-space unlock (owner storing 0) is never overwritten.
static bool mutex_acquire_or_mark(uint64_t m, uint32_t tag) {
    for (;;) {
        uint32_t v = __atomic_load_n(u32p(m), __ATOMIC_SEQ_CST);
        if (v == 0) {
            if (__atomic_compare_exchange_n(u32p(m), &v, tag, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true;
        } else {
            if ((v & WAITERS_BIT) ||
                __atomic_compare_exchange_n(u32p(m), &v, v | WAITERS_BIT, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                return false;
        }
    }
}

// Pick a thread waiting on mutex `addr` and give it ownership. Caller holds g_klock.
static void mutex_release(uint64_t addr) {
    KThread *next = NULL;
    int count = 0;
    for (KThread *t = g_waitlist; t; t = t->wnext) {
        if (t->wait_kind == WAIT_MUTEX && t->wait_addr == addr) {
            if (!next || t->prio < next->prio) next = t;
            count++;
        }
    }
    if (!next) {
        __atomic_store_n(u32p(addr), 0, __ATOMIC_SEQ_CST);
        return;
    }
    __atomic_store_n(u32p(addr), next->handle | (count > 1 ? WAITERS_BIT : 0), __ATOMIC_SEQ_CST);
    next->wait_kind = WAIT_NONE;
    next->wait_result = R_OK;
    wake(next);
}

KThread *g_waitlist;

static void waitlist_add(KThread *t) { t->wnext = g_waitlist; g_waitlist = t; }
static void waitlist_remove(KThread *t) {
    for (KThread **p = &g_waitlist; *p; p = &(*p)->wnext)
        if (*p == t) { *p = t->wnext; return; }
}

// Block until wait_kind is cleared by a waker, timeout, or cancel. Caller holds g_klock.
static uint32_t block_current(int64_t timeout_ns) {
    KThread *t = t_cur;
    struct timespec ts;
    if (timeout_ns >= 0) deadline(&ts, timeout_ns);
    waitlist_add(t);
    while (t->wait_kind != WAIT_NONE) {
        if (g_rt_unwind) {  // reboot: give up the wait (the thread unwinds when its svc returns)
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_CANCELLED;
            break;
        }
        if (t->cancel) {
            t->cancel = false;
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_CANCELLED;
            break;
        }
        if (!kwait(timeout_ns, &ts)) {
            // A waker may have run while this thread's timed wait was expiring (it holds g_klock, so it got there
            // first): it already cleared wait_kind, set the result and - for a condition variable - handed this
            // thread the mutex. Reporting a timeout then made the SDK lock that mutex again, owned by itself: a
            // deadlock (Let's Go's movie thread froze the title movie at a random frame).
            if (t->wait_kind == WAIT_NONE) break;
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_TIMEOUT;
            break;
        }
    }
    waitlist_remove(t);
    return t->wait_result;
}

// LARECOMP_PROF_LOCKS=1: the contended mutexes (every svcArbitrateLock that has to wait), every 2 s: per mutex address
// the number of waits, the time spent in them and the guest call chain of the first one (resolve with tools/addr2sym.py).
typedef struct { uint64_t addr, n, ns, chain[3]; } LockStat;
static LockStat lock_stats[64];

static void lock_prof_note(uint64_t addr, uint64_t ns) {  // (caller holds g_klock)
    static uint64_t last;
    LockStat *s = NULL, *free_slot = NULL;
    for (int i = 0; i < 64; i++) {
        if (lock_stats[i].addr == addr) { s = &lock_stats[i]; break; }
        if (!lock_stats[i].addr && !free_slot) free_slot = &lock_stats[i];
    }
    if (!s && free_slot) {
        s = free_slot;
        s->addr = addr;
        uint64_t fp = t_cur->cpu.x[29];
        s->chain[0] = t_cur->cpu.x[30];
        for (int k = 1; k < 3; k++) {  // the AArch64 frame record chain {fp, lr}
            if (fp < GUEST_LO || fp >= GUEST_HI || (fp & 7)) break;
            s->chain[k] = ((uint64_t *)(uintptr_t)fp)[1];
            fp = ((uint64_t *)(uintptr_t)fp)[0];
        }
    }
    if (s) { s->n++; s->ns += ns; }
    uint64_t now = plat_now_ns();
    if (!last) last = now;
    if (now - last < 2000000000ull) return;
    last = now;
    for (int shown = 0; shown < 6; shown++) {  // the six that waited longest
        LockStat *best = NULL;
        for (int i = 0; i < 64; i++)
            if (lock_stats[i].addr && (!best || lock_stats[i].ns > best->ns)) best = &lock_stats[i];
        if (!best || !best->n) break;
        LOG("[locks] %llx: %llu waits, %llu ms (%.1f us each), first from %llx <- %llx <- %llx\n", (unsigned long long)best->addr,
            (unsigned long long)best->n, (unsigned long long)(best->ns / 1000000), best->ns / 1000.0 / best->n,
            (unsigned long long)best->chain[0], (unsigned long long)best->chain[1], (unsigned long long)best->chain[2]);
        best->addr = 0;  // (counted: out of the next pick, and the slot is free again below)
        best->n = best->ns = 0;
    }
    memset(lock_stats, 0, sizeof lock_stats);
}

// A contended mutex is handed to the next waiter by the owner's unlock (mutex_release). Sleeping on a condition
// variable for that costs a wake-up (~12 us on Windows, measured with LARECOMP_PROF_LOCKS) and the world's frame has
// ~1000 of them per thread (a hot nn::os::Mutex of the game, three threads), so a waiter first spins on its own wait
// state for a few microseconds. LARECOMP_LOCK_SPIN_US=<n> (default 40; 0: always sleep, as before); off on a machine
// with fewer than 4 logical processors, where the spinning thread would take the owner's core.
#if defined(__x86_64__)
#define CPU_RELAX() __builtin_ia32_pause()
#else
#define CPU_RELAX() __asm__ volatile("yield")
#endif
static uint64_t lock_spin_ns(void) {
    static int64_t ns = -1;
    if (ns < 0) {
        const char *e = getenv("LARECOMP_LOCK_SPIN_US");
        long us = e ? atol(e) : 40;
        if (!e && SDL_GetNumLogicalCPUCores() < 4) us = 0;
        ns = us > 0 ? us * 1000 : 0;
    }
    return (uint64_t)ns;
}

static uint32_t svc_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag) {
    pthread_mutex_lock(&g_klock);
    uint32_t v = __atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    if (v != (owner | WAITERS_BIT)) {
        pthread_mutex_unlock(&g_klock);
        return R_OK;
    }
    KThread *t = t_cur;
    t->wait_kind = WAIT_MUTEX;
    t->wait_addr = addr;
    t->wait_tag = tag;
    static int prof_locks = -1;
    if (prof_locks < 0) prof_locks = getenv("LARECOMP_PROF_LOCKS") != NULL;
    uint64_t t0 = prof_locks ? plat_now_ns() : 0;
    uint32_t r;
    uint64_t spin = lock_spin_ns();
    bool handed = false;
    if (spin) {  // wait on the thread's own flag first: the owner's unlock hands the mutex over without a sleep + wake-up
        waitlist_add(t);
        pthread_mutex_unlock(&g_klock);
        uint64_t end = plat_now_ns() + spin;
        while (__atomic_load_n((int *)&t->wait_kind, __ATOMIC_ACQUIRE) != WAIT_NONE && !g_rt_unwind && plat_now_ns() < end)
            CPU_RELAX();
        pthread_mutex_lock(&g_klock);
        waitlist_remove(t);
        handed = t->wait_kind == WAIT_NONE;  // (the waker wrote the result under g_klock, which we hold again)
        r = t->wait_result;
    }
    if (!handed) r = block_current(-1);
    if (prof_locks) lock_prof_note(addr, plat_now_ns() - t0);
    pthread_mutex_unlock(&g_klock);
    return r == R_CANCELLED ? R_OK : r;
}

static uint32_t svc_arbitrate_unlock(uint64_t addr) {
    pthread_mutex_lock(&g_klock);
    mutex_release(addr);
    pthread_mutex_unlock(&g_klock);
    return R_OK;
}

// LARECOMP_PROF_CONDVAR=1: every 2 s, the condition-variable waits by how long they took (until the thread runs again), and how
// long the woken threads took to run after their waker released them (the wake-up latency spinning would save)
static uint64_t cv_hist[8], cv_lat_ns, cv_lat_n, cv_wait_ns;
static void cv_prof_note(uint64_t t0, uint64_t woken) {  // (caller holds g_klock)
    static const uint64_t edges[7] = {10000, 25000, 50000, 100000, 250000, 1000000, 5000000};
    static uint64_t last;
    uint64_t now = plat_now_ns(), d = now - t0;
    int b = 0;
    while (b < 7 && d >= edges[b]) b++;
    cv_hist[b]++;
    cv_wait_ns += d;
    if (woken > t0 && woken <= now) cv_lat_ns += now - woken, cv_lat_n++;
    if (!last) last = now;
    if (now - last < 2000000000ull) return;
    last = now;
    LOG("[condvar] waits <10us %llu, <25 %llu, <50 %llu, <100 %llu, <250 %llu, <1ms %llu, <5ms %llu, more %llu; %.0f ms waited; "
        "wake-up latency %.1f us average over %llu (%.0f ms)\n",
        (unsigned long long)cv_hist[0], (unsigned long long)cv_hist[1], (unsigned long long)cv_hist[2], (unsigned long long)cv_hist[3],
        (unsigned long long)cv_hist[4], (unsigned long long)cv_hist[5], (unsigned long long)cv_hist[6], (unsigned long long)cv_hist[7],
        cv_wait_ns / 1e6, cv_lat_n ? cv_lat_ns / 1e3 / cv_lat_n : 0.0, (unsigned long long)cv_lat_n, cv_lat_ns / 1e6);
    memset(cv_hist, 0, sizeof cv_hist);
    cv_lat_ns = cv_lat_n = cv_wait_ns = 0;
}

static uint32_t svc_wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout) {
    pthread_mutex_lock(&g_klock);
    KThread *t = t_cur;
    if (g_prof_cv < 0) g_prof_cv = getenv("LARECOMP_PROF_CONDVAR") != NULL;
    uint64_t cv_t0 = g_prof_cv ? plat_now_ns() : 0;
    t->woken_ns = 0;
    mutex_release(mutex);
    t->wait_kind = WAIT_CONDVAR;
    t->wait_addr = key;
    t->wait_mutex = mutex;
    t->wait_tag = tag;
    __atomic_store_n(u32p(key), 1, __ATOMIC_SEQ_CST);
    uint32_t r = block_current(timeout);
    if (g_prof_cv) cv_prof_note(cv_t0, t->woken_ns);
    // Timed out / cancelled: the mutex is NOT reacquired here. As on the console, the caller does it: the SDK's
    // InternalConditionVariableImplByHorizon::TimedWait calls InternalCriticalSection::Enter on 0xea01 (TimedOut).
    // Reacquiring it here too left the thread owning the mutex and then waiting on itself (Let's Go boot deadlock).
    pthread_mutex_unlock(&g_klock);
    return r;
}

static void svc_signal_process_wide_key(uint64_t key, int32_t count) {
    pthread_mutex_lock(&g_klock);
    int woken = 0;
    bool any_left = false;
    for (;;) {
        if (count > 0 && woken >= count) break;
        KThread *best = NULL;
        for (KThread *t = g_waitlist; t; t = t->wnext)
            if (t->wait_kind == WAIT_CONDVAR && t->wait_addr == key && (!best || t->prio < best->prio)) best = t;
        if (!best) break;
        woken++;
        // the woken thread must reacquire its mutex
        uint64_t m = best->wait_mutex;
        best->wait_result = R_OK;
        if (mutex_acquire_or_mark(m, best->wait_tag)) {
            best->wait_kind = WAIT_NONE;
            wake(best);
        } else {
            best->wait_kind = WAIT_MUTEX;
            best->wait_addr = m;
        }
    }
    for (KThread *t = g_waitlist; t; t = t->wnext)
        if (t->wait_kind == WAIT_CONDVAR && t->wait_addr == key) any_left = true;
    if (!any_left) __atomic_store_n(u32p(key), 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_klock);
}

// ------------------------------------------------------------------ address arbiter
enum { ARB_WAIT_LESS = 0, ARB_DEC_WAIT_LESS = 1, ARB_WAIT_EQ = 2 };
enum { ARB_SIGNAL = 0, ARB_SIGNAL_INC_EQ = 1, ARB_SIGNAL_MOD_EQ = 2 };

static uint32_t svc_wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout) {
    pthread_mutex_lock(&g_klock);
    int32_t cur = (int32_t)__atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    bool wait;
    if (type == ARB_WAIT_LESS) wait = cur < value;
    else if (type == ARB_DEC_WAIT_LESS) {
        wait = cur < value;
        if (wait) __atomic_store_n(u32p(addr), (uint32_t)(cur - 1), __ATOMIC_SEQ_CST);
    } else if (type == ARB_WAIT_EQ) wait = cur == value;
    else { pthread_mutex_unlock(&g_klock); return R_INVALID_ENUM; }
    if (!wait) { pthread_mutex_unlock(&g_klock); return R_INVALID_STATE; }
    if (timeout == 0) { pthread_mutex_unlock(&g_klock); return R_TIMEOUT; }
    t_cur->wait_kind = WAIT_ARBITER;
    t_cur->wait_addr = addr;
    uint32_t r = block_current(timeout);
    pthread_mutex_unlock(&g_klock);
    return r;
}

static int arbiter_wake(uint64_t addr, int32_t count) {
    int woken = 0;
    for (KThread *t = g_waitlist; t; t = t->wnext) {
        if (count > 0 && woken >= count) break;
        if (t->wait_kind == WAIT_ARBITER && t->wait_addr == addr) {
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_OK;
            wake(t);
            woken++;
        }
    }
    return woken;
}

static uint32_t svc_signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    pthread_mutex_lock(&g_klock);
    uint32_t r = R_OK;
    int32_t cur = (int32_t)__atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    if (type == ARB_SIGNAL) {
        arbiter_wake(addr, count);
    } else if (type == ARB_SIGNAL_INC_EQ) {
        if (cur != value) r = R_INVALID_STATE;
        else { __atomic_store_n(u32p(addr), (uint32_t)(cur + 1), __ATOMIC_SEQ_CST); arbiter_wake(addr, count); }
    } else if (type == ARB_SIGNAL_MOD_EQ) {
        int waiting = 0;
        for (KThread *t = g_waitlist; t; t = t->wnext)
            if (t->wait_kind == WAIT_ARBITER && t->wait_addr == addr) waiting++;
        int32_t nv;
        if (count <= 0) nv = waiting > 0 ? value - 2 : value + 1;
        else nv = waiting == 0 ? value + 1 : (waiting <= count ? value - 1 : value);
        if (cur != value) r = R_INVALID_STATE;
        else { __atomic_store_n(u32p(addr), (uint32_t)nv, __ATOMIC_SEQ_CST); arbiter_wake(addr, count); }
    } else {
        r = R_INVALID_ENUM;
    }
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ wait synchronization
static uint32_t svc_wait_synchronization(uint64_t handles_ptr, int32_t count, int64_t timeout, int32_t *index) {
    pthread_mutex_lock(&g_klock);
    struct timespec ts;
    if (timeout > 0) deadline(&ts, timeout);
    KThread *t = t_cur;
    uint32_t *hs = (uint32_t *)(uintptr_t)handles_ptr;
    uint32_t r;
    if (ENV("LARECOMP_VSYNC_BT")) {  // (debug) who waits for the display's vsync event: the game's frame loop
        extern uint32_t vi_vsync_handle(void);
        static int shown;
        for (int i = 0; i < count && shown < 2; i++)
            if (hs[i] && hs[i] == vi_vsync_handle()) {
                shown++;
                LOG("[vsync-bt] wait on vsync (%d handles, timeout %lld) lr %llx:\n", count, (long long)timeout,
                    (unsigned long long)(t ? t->cpu.x[30] : 0));
                plat_print_backtrace();
            }
    }
    for (;;) {
        for (int i = 0; i < count; i++) {
            // (an empty slot, e.g. a Bluetooth event the game never got: wait on the others, as issue #1 found in the
            // GO Park settings; a wait on nothing but empty slots still ends in the timeout below)
            if (!hs[i]) continue;
            KObject *o = handle_get(hs[i]);
            if (!o) {
                LOG("[kernel] WaitSynchronization: invalid handle %#x at [%d] of %d (lr %llx)\n", hs[i], i, count,
                    (unsigned long long)(t ? t->cpu.x[30] : 0));
                r = R_INVALID_HANDLE;
                goto out;
            }
            if (o->signaled) { *index = i; r = R_OK; goto out; }
        }
        if (t->cancel) { t->cancel = false; r = R_CANCELLED; goto out; }
        if (g_rt_unwind) { r = R_CANCELLED; goto out; }
        if (timeout == 0) { r = R_TIMEOUT; goto out; }
        t->in_sync_wait = true;
        t->snext = g_syncwaiters;
        g_syncwaiters = t;
        bool ok = kwait(timeout < 0 ? -1 : timeout, &ts);
        for (KThread **p = &g_syncwaiters; *p; p = &(*p)->snext)
            if (*p == t) { *p = t->snext; break; }
        t->in_sync_wait = false;
        if (!ok) { r = R_TIMEOUT; goto out; }
    }
out:
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ debug output
static void debug_string(uint64_t p, uint64_t len) {
    fprintf(stderr, "[guest] %.*s", (int)len, (const char *)(uintptr_t)p);
    if (len && ((const char *)(uintptr_t)p)[len - 1] != '\n') fputc('\n', stderr);
}

// ------------------------------------------------------------------ SVC dispatch
static int trace_svc = -1;

static void svc_impl(Cpu *c, uint32_t imm, uint64_t pc);

static _Atomic uint64_t svc_wait_ns[32][0x80], svc_calls[32][0x80];
static int prof_svc = -1;

static void svc_entry(Cpu *c, uint32_t imm, uint64_t pc);
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    rt_unwind_point();
    KThread *self = t_cur;
    if (self) {
        self->cur_svc_pc = pc;
        for (int i = 0; i < 4; i++) self->cur_svc_args[i] = c->x[i];
        self->cur_svc = (int)imm;
    }
    svc_entry(c, imm, pc);
    if (self) self->cur_svc = -1;
    rt_unwind_point();
}

// LARECOMP_THREAD_DUMP=<seconds>[,<repeat seconds>]: list every guest thread, the svc it is blocked in and where
// from (guest pc / lr), to find what a hung boot waits on. (Read without g_klock: a debug snapshot.)
static void *thread_dump_main(void *arg) {
    const char *e = (const char *)arg;
    double first = atof(e), every = strchr(e, ',') ? atof(strchr(e, ',') + 1) : 0;
    plat_sleep_until_ns(plat_now_ns() + (uint64_t)(first * 1e9));
    for (;;) {
        LOG("[threads] ---- dump\n");
        for (KThread *t = g_threads; t; t = t->anext) {
            if (t->finished) continue;
            LOG("[threads] id %llu h %x prio %d entry %llx wait %d tag %x addr %llx | svc %02x(%llx, %llx, %llx, %llx) "
                "@%llx lr %llx\n", (unsigned long long)t->id, t->handle, t->prio, (unsigned long long)t->entry,
                (int)t->wait_kind, t->wait_tag, (unsigned long long)t->wait_addr, t->cur_svc < 0 ? 0xff : t->cur_svc,
                (unsigned long long)t->cur_svc_args[0], (unsigned long long)t->cur_svc_args[1],
                (unsigned long long)t->cur_svc_args[2], (unsigned long long)t->cur_svc_args[3],
                (unsigned long long)(t->cur_svc < 0 ? t->cpu.pc : t->cur_svc_pc), (unsigned long long)t->cpu.x[30]);
            // guest call stack: the AArch64 frame record chain {fp, lr} from x29
            uint64_t fp = t->cpu.x[29];
            LOG("[threads]     stack:");
            for (int i = 0; i < 16 && fp >= GUEST_LO && fp < GUEST_HI && !(fp & 7); i++) {
                uint64_t lr = ((uint64_t *)(uintptr_t)fp)[1], up = ((uint64_t *)(uintptr_t)fp)[0];
                LOG(" %llx", (unsigned long long)lr);
                if (up <= fp) break;
                fp = up;
            }
            LOG("\n");
        }
        if (every <= 0) break;
        plat_sleep_until_ns(plat_now_ns() + (uint64_t)(every * 1e9));
    }
    return NULL;
}

static void svc_entry(Cpu *c, uint32_t imm, uint64_t pc) {
    if (prof_svc < 0) prof_svc = ENV("LARECOMP_PROF_SVC") != NULL;
    if (prof_svc && imm < 0x80) {
        uint64_t a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3], lr = c->x[30];
        uint32_t h0 = imm == 0x18 && a2 ? *(uint32_t *)(uintptr_t)a1 : 0;
        uint64_t t0 = plat_now_ns();
        svc_impl(c, imm, pc);
        uint64_t dt = plat_now_ns() - t0;
        static _Atomic int nslow;
        if (((t_cur && t_cur->id == 80 && (imm == 0x18 || imm == 0x34)) || (dt > 10000000 && imm != 0x0b && imm != 0x1c)) && nslow < 20000) {
            nslow++;
            LOG("[slow] t%llu svc%02x %llums a=%llx,%llx,%llx,%llx h0=%x lr=%llx\n", (unsigned long long)(t_cur ? t_cur->id : 0), imm,
                (unsigned long long)(dt / 1000000), (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, h0, (unsigned long long)lr);
        }
        unsigned tid = t_cur ? (unsigned)(t_cur->id & 31) : 31;
        svc_wait_ns[tid][imm] += dt;
        svc_calls[tid][imm]++;
        static _Atomic uint64_t last;
        uint64_t now = plat_now_ns();
        if (now - last > 2000000000ull) {
            last = now;
            for (int t = 0; t < 32; t++) {
                int any = 0;
                for (int i = 0; i < 0x80; i++)
                    if (svc_calls[t][i]) {
                        if (!any) { LOG("[svcprof] t%d", t); any = 1; }
                        LOG(" %02x:%llu/%llums", i, (unsigned long long)svc_calls[t][i], (unsigned long long)(svc_wait_ns[t][i] / 1000000));
                        svc_calls[t][i] = 0; svc_wait_ns[t][i] = 0;
                    }
                if (any) LOG("\n");
            }
        }
        return;
    }
    if (trace_svc < 0) trace_svc = getenv("LARECOMP_TRACE_SVC") != NULL;
    if (!trace_svc) {
        uint64_t a0 = c->x[0], a1 = c->x[1], lr = c->x[30];
        svc_impl(c, imm, pc);
        static int nbad;  // (the first ones: which call got a handle that doesn't exist, for crash reports)
        if (c->x[0] == R_INVALID_HANDLE && nbad < 16) {
            nbad++;
            LOG("[kernel] svc %02x(%llx, %llx) -> invalid handle (lr %llx)\n", imm, (unsigned long long)a0,
                (unsigned long long)a1, (unsigned long long)lr);
        }
        return;
    }
    uint64_t in[4] = {c->x[0], c->x[1], c->x[2], c->x[3]};
    svc_impl(c, imm, pc);
    LOG("[svc] %02x(%llx, %llx, %llx, %llx) -> %llx %llx %llx  @%llx\n", imm, in[0], in[1], in[2], in[3], c->x[0],
        c->x[1], c->x[2], pc);
}

static void svc_impl(Cpu *c, uint32_t imm, uint64_t pc) {
    uint64_t *x = c->x;
    switch (imm) {
    case 0x01: {  // SetHeapSize
        uint64_t size = x[1];
        if (size > HEAP_MAX) { x[0] = R_OUT_OF_RESOURCE; break; }
        if (size && !heap_size) { extern uint64_t renderer_share_heap(uint64_t, uint64_t); renderer_share_heap(HEAP_BASE, size); }  // (Android: the GPU's view of it, before use)
        heap_size = size;
        region_remove(HEAP_BASE);
        if (size) region_add(HEAP_BASE, size, 5, 3);
        x[0] = R_OK;
        x[1] = HEAP_BASE;
        break;
    }
    case 0x02: case 0x03: x[0] = R_OK; break;  // SetMemoryPermission / SetMemoryAttribute
    case 0x04:  // MapMemory(dst, src, size): src is inaccessible while mapped, so copying is equivalent
        memcpy((void *)x[0], (void *)x[1], x[2]);
        region_add(x[0], x[2], 0xB, 3);
        x[0] = R_OK;
        break;
    case 0x05:  // UnmapMemory(dst, src, size): copy back
        memcpy((void *)x[1], (void *)x[0], x[2]);
        region_remove(x[0]);
        x[0] = R_OK;
        break;
    case 0x06:  // QueryMemory(out_info, _, addr)
        query_memory(x[2], x[0]);
        x[0] = R_OK;
        x[1] = 0;
        break;
    case 0x07: LOG("ExitProcess\n"); exit(0);
    case 0x08: {  // CreateThread(_, entry, arg, stack_top, prio, core)
        KThread *t = thread_new(x[1], x[2], x[3], (int)x[4], (int)(int32_t)x[5]);
        t->handle = kernel_new_handle(&t->obj);
        LOG("[kernel] CreateThread entry=%llx stack=%llx prio=%d -> %x\n", x[1], x[3], (int)x[4], t->handle);
        x[0] = R_OK;
        x[1] = t->handle;
        break;
    }
    case 0x09: {  // StartThread
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KOBJ_THREAD) { x[0] = R_INVALID_HANDLE; break; }
        thread_start((KThread *)o);
        x[0] = R_OK;
        break;
    }
    case 0x0A: longjmp(t_cur->unwind, 1);  // ExitThread: back to thread_main, which signals the thread object
    case 0x0B: {  // SleepThread (in slices of at most 50 ms: a reboot doesn't wait out a long sleep)
        int64_t ns = (int64_t)x[0];
        if (ns <= 0) { sched_yield(); break; }
        for (uint64_t end = plat_now_ns() + (uint64_t)ns, now; !g_rt_unwind && (now = plat_now_ns()) < end;) {
            uint64_t d = end - now < 50000000 ? end - now : 50000000;
            struct timespec ts = {(time_t)(d / 1000000000), (long)(d % 1000000000)};
            nanosleep(&ts, NULL);
        }
        break;
    }
    case 0x0C: {  // GetThreadPriority(_, handle)
        KObject *o = handle_get((uint32_t)x[1]);
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        x[1] = o ? (uint64_t)((KThread *)o)->prio : 0;
        break;
    }
    case 0x0D: {  // SetThreadPriority
        KObject *o = handle_get((uint32_t)x[0]);
        if (o) ((KThread *)o)->prio = (int)x[1];
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        break;
    }
    case 0x0E: {  // GetThreadCoreMask(_, _, handle)
        KObject *o = handle_get((uint32_t)x[2]);
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        x[1] = o ? (uint64_t)((KThread *)o)->core : 0;
        x[2] = 0xF;
        break;
    }
    case 0x0F: x[0] = R_OK; break;  // SetThreadCoreMask
    case 0x10: x[0] = t_cur->core; break;  // GetCurrentProcessorNumber
    case 0x11: case 0x12: case 0x17: {  // SignalEvent / ClearEvent / ResetSignal
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o) x[0] = R_INVALID_HANDLE;
        else {
            KObject *ev = o->type == KOBJ_EVENT ? o : NULL;
            if (!ev) x[0] = R_INVALID_HANDLE;
            else {
                if (imm == 0x11) kernel_signal(ev);
                else if (imm == 0x17 && !ev->signaled) { x[0] = R_INVALID_STATE; pthread_mutex_unlock(&g_klock); break; }
                else ev->signaled = false;
                x[0] = R_OK;
            }
        }
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x13: {  // MapSharedMemory(handle, addr, size, perm)
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KOBJ_SHMEM) { x[0] = R_INVALID_HANDLE; break; }
        KSharedMem *s = (KSharedMem *)o;
        memset((void *)x[1], 0, x[2]);
        s->guest_addr = x[1];  // HLE services write straight into the guest's mapping
        region_add(x[1], x[2], 6, 3);
        if (s->on_map) s->on_map(s);
        x[0] = R_OK;
        break;
    }
    case 0x14: region_remove(x[1]); x[0] = R_OK; break;  // UnmapSharedMemory
    case 0x15: {  // CreateTransferMemory(_, addr, size, perm)
        KTransferMem *t = calloc(1, sizeof *t);
        t->obj.type = KOBJ_TMEM;
        t->addr = x[1];
        t->size = x[2];
        x[0] = R_OK;
        x[1] = kernel_new_handle(&t->obj);
        break;
    }
    case 0x16:  // CloseHandle
        pthread_mutex_lock(&g_klock);
        handle_close((uint32_t)x[0]);
        pthread_mutex_unlock(&g_klock);
        x[0] = R_OK;
        break;
    case 0x18: {  // WaitSynchronization(_, handles, count, timeout)
        int32_t idx = -1;
        x[0] = svc_wait_synchronization(x[1], (int32_t)x[2], (int64_t)x[3], &idx);
        x[1] = (uint32_t)idx;
        break;
    }
    case 0x19: {  // CancelSynchronization
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get((uint32_t)x[0]);
        if (o && o->type == KOBJ_THREAD) {
            ((KThread *)o)->cancel = true;
            wake((KThread *)o);
            x[0] = R_OK;
        } else x[0] = R_INVALID_HANDLE;
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x1A: x[0] = svc_arbitrate_lock((uint32_t)x[0], x[1], (uint32_t)x[2]); break;
    case 0x1B: x[0] = svc_arbitrate_unlock(x[0]); break;
    case 0x1C: x[0] = svc_wait_process_wide_key(x[0], x[1], (uint32_t)x[2], (int64_t)x[3]); break;
    case 0x1D: svc_signal_process_wide_key(x[0], (int32_t)x[1]); x[0] = R_OK; break;
    case 0x1E: x[0] = rt_ticks(); break;
    case 0x1F: {  // ConnectToNamedPort(_, name)
        uint32_t h = ipc_connect_named_port((const char *)(uintptr_t)x[1]);
        x[0] = h ? R_OK : R_NOT_FOUND;
        x[1] = h;
        break;
    }
    case 0x21: x[0] = ipc_handle_request(c, (uint32_t)x[0]); break;  // SendSyncRequest
    case 0x22: {  // SendSyncRequestWithUserBuffer(buf, size, handle)
        uint64_t saved = c->tpidrro;
        c->tpidrro = x[0];
        x[0] = ipc_handle_request(c, (uint32_t)x[2]);
        c->tpidrro = saved;
        break;
    }
    case 0x24: x[0] = R_OK; x[1] = 0x81; break;  // GetProcessId
    case 0x25: {  // GetThreadId(_, handle)
        KObject *o = handle_get((uint32_t)x[1]);
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        x[1] = o ? ((KThread *)o)->id : 0;
        break;
    }
    case 0x26:  // Break
        if (x[0] & 0x80000000u) { LOG("[kernel] Break notification %llx\n", x[0]); x[0] = R_OK; break; }
        if (x[2] == 4 && x[1] >= GUEST_LO && x[1] < GUEST_HI) {  // the info is the failing nn::Result
            uint32_t res = *(uint32_t *)(uintptr_t)x[1];
            LOG("[kernel] svcBreak result %#x = %04u-%04u\n", res, 2000 + (res & 0x1FF), (res >> 9) & 0x1FFF);
        }
        FATAL("svcBreak(reason=%llx, info=%llx, size=%llx) at %llx\n", x[0], x[1], x[2], pc);
    case 0x27: debug_string(x[0], x[1]); x[0] = R_OK; break;
    case 0x29: {  // GetInfo(_, id0, handle, id1)
        uint32_t id0 = (uint32_t)x[1];
        uint64_t v = 0;
        uint32_t r = R_OK;
        switch (id0) {
        case 0: v = 0xF; break;
        case 1: v = 0xFFFFFFFFFFFFFFFFull; break;
        case 2: v = ALIAS_BASE; break;
        case 3: v = ALIAS_SIZE; break;
        case 4: v = HEAP_BASE; break;
        case 5: v = HEAP_MAX; break;
        case 6: case 21: v = 0xCD500000ull; break;  // application memory (3.2 GB, as on hardware)
        case 7: case 22: v = heap_size + 0x4000000; break;
        case 8: v = 0; break;
        case 9: v = 0; break;
        case 11: v = ((uint64_t)arc4random() << 32) | arc4random(); break;
        case 12: v = GUEST_LO; break;
        case 13: v = GUEST_HI - GUEST_LO; break;
        case 14: v = STACK_BASE; break;
        case 15: v = STACK_SIZE; break;
        case 16: case 17: v = 0; break;
        case 18: v = g_program_id; break;
        case 20:  // UserExceptionContextAddress: nn::os::SetDyingMessageRegion stores its region at +0x1c0
            if (!g_user_exception_ctx) g_user_exception_ctx = (uint64_t)(uintptr_t)hle_alloc(0x1000);
            v = g_user_exception_ctx;
            break;
        default: r = R_INVALID_ENUM; LOG("[kernel] GetInfo unknown id %u/%llu\n", id0, x[3]);
        }
        x[0] = r;
        x[1] = v;
        break;
    }
    case 0x2C:  // MapPhysicalMemory(addr, size)
        memset((void *)x[0], 0, x[1]);
        region_add(x[0], x[1], 0x7, 3);
        x[0] = R_OK;
        break;
    case 0x2D:  // UnmapPhysicalMemory
        region_remove(x[0]);
        x[0] = R_OK;
        break;
    case 0x34: x[0] = svc_wait_for_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int64_t)x[3]); break;
    case 0x35: x[0] = svc_signal_to_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int32_t)x[3]); break;
    case 0x45: {  // CreateEvent -> w1 writable, w2 readable
        KObject *ev = calloc(1, sizeof *ev);
        ev->type = KOBJ_EVENT;
        x[0] = R_OK;
        x[1] = kernel_new_handle(ev);
        x[2] = kernel_new_handle(ev);
        if (ENV("LARECOMP_PROF_SVC")) LOG("[evt] guest w=%llx r=%llx lr=%llx\n", (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[30]);
        break;
    }
    default:
        FATAL("unimplemented svc 0x%x at %llx (x0=%llx x1=%llx x2=%llx)\n", imm, pc, x[0], x[1], x[2]);
    }
}

// Reboot, step 1: stop every guest thread (see rt_unwind_point) and join it. `idle` keeps the window responsive.
void kernel_stop_threads(void (*idle)(void)) {
    g_rt_unwind = 1;
    uint64_t t0 = plat_now_ns(), logged = t0;
    for (;;) {
        int live = 0;
        pthread_mutex_lock(&g_klock);
        for (KThread *t = g_threads; t; t = t->anext)
            if (t->started && !t->finished) { live++; wake(t); }
        pthread_cond_broadcast(&g_nothread_cond);
        pthread_mutex_unlock(&g_klock);
        if (!live) break;
        uint64_t now = plat_now_ns();
        if (now - logged > 3000000000ull) {  // (a thread that never reaches a safe point: who and where)
            logged = now;
            LOG("[reboot] still waiting for %d guest threads after %llums:\n", live, (unsigned long long)((now - t0) / 1000000));
            for (KThread *t = g_threads; t; t = t->anext)
                if (t->started && !t->finished)
                    LOG("[reboot]   thread %llu (entry %llx) pc %llx lr %llx wait %d sync %d\n", (unsigned long long)t->id,
                        (unsigned long long)t->entry, (unsigned long long)t->cpu.pc, (unsigned long long)t->cpu.x[30],
                        t->wait_kind, t->in_sync_wait);
        }
        if (idle) idle();
        usleep(2000);
    }
    int n = 0;
    for (KThread *t = g_threads; t; t = t->anext)
        if (t->started) {
#ifdef _WIN32
            if (!guest_thread_join(t->host)) FATAL("Could not join a game thread.\n");
#else
            pthread_join(t->host, NULL);
#endif
            n++;
        }
    LOG("[reboot] %d guest threads stopped in %llums\n", n, (unsigned long long)((plat_now_ns() - t0) / 1000000));
}

// Reboot, step 2 (threads stopped): a fresh kernel. Kernel objects are leaked rather than freed: host statics
// can still point at some (vi.c's vsync event, signalled by its host timer thread).
void kernel_reset(void) {
    pthread_mutex_lock(&g_klock);
    g_user_exception_ctx = 0;
    for (KThread *t = g_threads, *next; t; t = next) {
        next = t->anext;
        pthread_cond_destroy(&t->cond);
        free(t);
    }
    g_threads = NULL;
    g_waitlist = NULL;
    g_syncwaiters = NULL;
    memset(handles, 0, sizeof handles);
    nregions = 0;
    heap_size = 0;
    tls_next = TLS_BASE;
    next_thread_id = 0x50;
    g_rt_unwind = 0;
    pthread_mutex_unlock(&g_klock);
}

void kernel_init(void) {
    static bool dump_started;
    const char *dump = getenv("LARECOMP_THREAD_DUMP");
    if (dump && !dump_started) {
        dump_started = true;
        pthread_t th;
        pthread_create(&th, NULL, thread_dump_main, (void *)dump);
        pthread_detach(th);
    }
    // Reserve the heap, alias, stack, and TLS regions.
    region_add(TLS_BASE, TLS_SIZE, 0xC, 3);
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        region_add(m->base + m->text_off, m->ro_off - m->text_off, 3, 5);
        region_add(m->base + m->ro_off, m->data_off - m->ro_off, 3, 1);
        region_add(m->base + m->data_off, m->size - m->data_off, 4, 3);
    }
}

KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) {
    KObject *ev = calloc(1, sizeof *ev);
    ev->type = KOBJ_EVENT;
    *w = kernel_new_handle(ev);
    *r = kernel_new_handle(ev);
    if (ENV("LARECOMP_PROF_SVC")) { const char *s = plat_symbol((uint64_t)(uintptr_t)__builtin_return_address(0), NULL); LOG("[evt] hle w=%x r=%x from %s\n", *w, *r, s ? s : "?"); }
    return ev;
}

KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle) {
    KSharedMem *s = calloc(1, sizeof *s);
    s->obj.type = KOBJ_SHMEM;
    s->size = size;
    s->on_map = on_map;
    *handle = kernel_new_handle(&s->obj);
    return s;
}
