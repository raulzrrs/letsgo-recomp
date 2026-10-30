// Research tools of the 60 fps work (docs/PESQUISA-60FPS.md, docs/DEPURACAO.md), kept out of runtime/hle/fps60.c: each
// one is off unless its LARECOMP_* variable is set.
#include <stdatomic.h>
#include <stdbool.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));  // runtime/fnhook.c
void rt_call_original(Cpu *c, uint64_t addr);
uint64_t renderer_game_frame(void);
uint64_t fps60_timers(void);  // runtime/hle/fps60.c: the frame-step timers (the accessor's x0)

// Research (LARECOMP_ELAPSED_LOG=1): the elapsed time the animation functions f_7000663660 / f_7000664cd0 divide by
// G[index]: the object their parameter holds, asked as they do (its first virtual method, argument 1), field +8.
// LARECOMP_ELAPSED_LOG="<addr>:<which>[,...]" names other readers and the argument they pass (f_7000029d30, the frame
// loop: 0; f_700067be50: 1); LARECOMP_TIME_PROBE_FRAMES limits it to some presents.
static uint64_t g_el_fn[16];
static int g_el_arg[16], g_nel;
static long g_tp_from, g_tp_to = 0x7fffffffL;  // (LARECOMP_TIME_PROBE_FRAMES: shared by the tools below)
static int elapsed_hook(Cpu *c, uint64_t addr) {
    static int n;
    long f = (long)renderer_game_frame();
    if (f < g_tp_from || f > g_tp_to) return 0;
    int which = 1;
    for (int i = 0; i < g_nel; i++) if (g_el_fn[i] == addr) which = g_el_arg[i];
    uint64_t saved[32], pc_arg = c->x[1];
    memcpy(saved, c->x, sizeof saved);
    uint64_t obj = pc_arg ? *(uint64_t *)(uintptr_t)pc_arg : 0;
    if (obj >= GUEST_LO && n < 400) {
        uint64_t vt = *(uint64_t *)(uintptr_t)obj, fn = vt >= GUEST_LO ? *(uint64_t *)(uintptr_t)vt : 0;
        if (fn >= GUEST_LO) {
            c->x[0] = obj;
            c->x[1] = (uint64_t)which;
            guest_call(c, fn);
            uint64_t r = c->x[0];
            if (r >= GUEST_LO)
                LOG("[elapsed] f_%llx present %llu: delta %lld ns (timer %d: object %llx, getter f_%llx, step %llx)\n", (unsigned long long)addr,
                    (unsigned long long)renderer_game_frame(), (long long)*(const int64_t *)(uintptr_t)(r + 8), which,
                    (unsigned long long)obj, (unsigned long long)fn, (unsigned long long)r);
            n++;
        }
    }
    memcpy(c->x, saved, sizeof saved);
    return 0;
}

__attribute__((constructor)) static void elapsed_init(void) {
    if (getenv("LARECOMP_ELAPSED_LOG")) {
        const char *fr = getenv("LARECOMP_TIME_PROBE_FRAMES");
        if (fr) { g_tp_from = atol(fr); g_tp_to = strchr(fr, '-') ? atol(strchr(fr, '-') + 1) : g_tp_from; }
        char lb[512];
        snprintf(lb, sizeof lb, "%s", getenv("LARECOMP_ELAPSED_LOG"));
        if (strchr(lb, ':'))
            for (char *t = strtok(lb, ","); t && g_nel < 16; t = strtok(NULL, ",")) {
                g_el_fn[g_nel] = strtoull(t, NULL, 16);
                g_el_arg[g_nel] = strchr(t, ':') ? atoi(strchr(t, ':') + 1) : 1;
                rt_register_fnhook(g_el_fn[g_nel++], elapsed_hook);
            }
        else {
            rt_register_fnhook(0x7000663660ull, elapsed_hook);
            rt_register_fnhook(0x7000664cd0ull, elapsed_hook);
        }
    }
}

// ---- research: LARECOMP_STEP_SCALE="<fn addr of the running build>:<factor>[,...]": timer 1's step times that factor
// while the function runs (which reader of the step a speed comes from: the world's walking at 60 / 120 fps)
#define SS_MAX 8
static uint64_t g_ss_fn[SS_MAX];
static double g_ss_k[SS_MAX];
static int g_nss;
static int step_scale_hook(Cpu *c, uint64_t addr) {
    double k = 1;
    for (int i = 0; i < g_nss; i++) if (g_ss_fn[i] == addr) k = g_ss_k[i];
    volatile int64_t *step = fps60_timers() ? (volatile int64_t *)(uintptr_t)(fps60_timers() + 0x20) : NULL;
    int64_t was = step ? *step : 0;
    static int log = -1, n;  // LARECOMP_STEP_SCALE_LOG=<first present>: the object's +0x494..+0x49c, 40 calls from then
    if (log == -1) log = getenv("LARECOMP_STEP_SCALE_LOG") ? atoi(getenv("LARECOMP_STEP_SCALE_LOG")) : -2;
    if (log >= 0 && (long)renderer_game_frame() >= log && n < 40 && c->x[0] >= GUEST_LO) {
        const uint8_t *o = (const uint8_t *)(uintptr_t)c->x[0];
        n++;
        LOG("[stepscale] present %llu f_%llx obj %llx: +494 %u +498 %u +49c %g +493 %u step %lld\n",
            (unsigned long long)renderer_game_frame(), (unsigned long long)addr, (unsigned long long)c->x[0],
            *(const uint32_t *)(o + 0x494), *(const uint32_t *)(o + 0x498), *(const float *)(o + 0x49c), o[0x493], (long long)was);
    }
    if (step) *step = (int64_t)((double)was * k);
    rt_call_original(c, addr);
    if (step) *step = was;
    {   // LARECOMP_FIND_MOVER=<first present>: in each object (its first 0x1000 bytes) handed to the function, the float
        // triples whose ground-plane step (x, z) between two of its updates is 1-20 cm - a walking character's position -
        // for 6 updates; logged with their offsets
        static long from = -2;
        if (from == -2) from = getenv("LARECOMP_FIND_MOVER") ? atol(getenv("LARECOMP_FIND_MOVER")) : -1;
        static struct { uint64_t obj; float prev[0x400]; int hits[0x400]; int n; } fm[32];
        long f = (long)renderer_game_frame();
        uint64_t o = c->x[0];
        if (from >= 0 && f >= from && f < from + 6 && o >= GUEST_LO) {
            int i = 0;
            while (i < 32 && fm[i].obj && fm[i].obj != o) i++;
            if (i < 32) {
                const float *now = (const float *)(uintptr_t)o;
                if (fm[i].obj == o && fm[i].n)
                    for (int w = 0; w + 2 < 0x400; w++) {
                        float dx = now[w] - fm[i].prev[w], dz = now[w + 2] - fm[i].prev[w + 2];
                        float d = sqrtf(dx * dx + dz * dz);
                        if (d > 1 && d < 20 && isfinite(d) && fabsf(now[w]) < 1e6f) fm[i].hits[w]++;
                    }
                fm[i].obj = o;
                memcpy(fm[i].prev, now, sizeof fm[i].prev);
                fm[i].n++;
                static FILE *dump;  // LARECOMP_FIND_MOVER_DUMP=<file>: every object's 0x1000 bytes at each of those updates
                static int dump_init;
                if (!dump_init) { dump_init = 1; const char *p = getenv("LARECOMP_FIND_MOVER_DUMP"); if (p) dump = fopen(p, "w"); }
                if (dump) {  // (one write per line, under a lock: several game threads update characters)
                    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
                    static char line[0x400 * 9 + 64];
                    pthread_mutex_lock(&lock);
                    int len = snprintf(line, sizeof line, "%ld %llx", f, (unsigned long long)o);
                    for (int w = 0; w < 0x400; w++) len += snprintf(line + len, sizeof line - len, " %08x", ((const uint32_t *)now)[w]);
                    line[len++] = '\n';
                    fwrite(line, 1, (size_t)len, dump);
                    fflush(dump);
                    pthread_mutex_unlock(&lock);
                }
            }
        }
        if (from >= 0 && f == from + 6) {
            for (int i = 0; i < 32 && fm[i].obj; i++)
                for (int w = 0; w + 2 < 0x400; w++)
                    if (fm[i].hits[w] >= 4) {
                        const float *now = (const float *)(uintptr_t)fm[i].obj;
                        LOG("[mover] obj %llx +0x%x: (%g, %g, %g) moved in %d updates\n", (unsigned long long)fm[i].obj, w * 4,
                            now[w], now[w + 1], now[w + 2], fm[i].hits[w]);
                    }
            from = -1;
        }
    }
    return 1;
}
__attribute__((constructor)) static void step_scale_init(void) {
    const char *e = getenv("LARECOMP_STEP_SCALE");
    if (!e) return;
    extern void rt_register_fnhook_raw(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t && g_nss < SS_MAX; t = strtok(NULL, ",")) {
        g_ss_fn[g_nss] = strtoull(t, NULL, 16);
        g_ss_k[g_nss] = strchr(t, ':') ? atof(strchr(t, ':') + 1) : 1;
        rt_register_fnhook_raw(g_ss_fn[g_nss++], step_scale_hook);
    }
}

// ---- research: LARECOMP_FARG_SCALE="<fn addr of the running build>:<factor>[,...]": the function's first float argument
// (s0: a time step handed down) times that factor (which part of an update a speed comes from)
static uint64_t g_fa_fn[SS_MAX];
static float g_fa_k[SS_MAX];
static int g_nfa;
static int farg_scale_hook(Cpu *c, uint64_t addr) {
    float s0;
    memcpy(&s0, &c->v[0], 4);
    static int log = -1, n;  // LARECOMP_FARG_LOG=<first present>: x0's floats +0x120..+0x138 (an animation player's), 300 calls
    if (log == -1) log = getenv("LARECOMP_FARG_LOG") ? atoi(getenv("LARECOMP_FARG_LOG")) : -2;
    if (log >= 0 && (long)renderer_game_frame() >= log && n < 300 && c->x[0] >= GUEST_LO) {
        const uint8_t *o = (const uint8_t *)(uintptr_t)c->x[0];
        n++;
        LOG("[farg] present %llu obj %llx dt %g: mode %u fps %u speed %g time %g start %g end %g\n", (unsigned long long)renderer_game_frame(),
            (unsigned long long)c->x[0], s0, *(const uint32_t *)(o + 0x120), *(const uint32_t *)(o + 0x124), *(const float *)(o + 0x128),
            *(const float *)(o + 0x12c), *(const float *)(o + 0x130), *(const float *)(o + 0x134));
    }
    for (int i = 0; i < g_nfa; i++) if (g_fa_fn[i] == addr) s0 *= g_fa_k[i];
    memcpy(&c->v[0], &s0, 4);
    rt_call_original(c, addr);
    return 1;
}
__attribute__((constructor)) static void farg_scale_init(void) {
    const char *e = getenv("LARECOMP_FARG_SCALE");
    if (!e) return;
    extern void rt_register_fnhook_raw(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t && g_nfa < SS_MAX; t = strtok(NULL, ",")) {
        g_fa_fn[g_nfa] = strtoull(t, NULL, 16);
        g_fa_k[g_nfa] = strchr(t, ':') ? (float)atof(strchr(t, ':') + 1) : 1;
        rt_register_fnhook_raw(g_fa_fn[g_nfa++], farg_scale_hook);
    }
}

// ---- research: LARECOMP_FNWATCH=<guest addr of a float> + LARECOMP_FNWATCH_FNS="<fn addr of the running build>[,...]": logs
// (the first 200 times) when the float changes during one of those functions - which one writes it, and by how much
static uint64_t g_watch;
static int watch_hook(Cpu *c, uint64_t addr) {
    static int n;
    volatile float *w = (volatile float *)(uintptr_t)g_watch;
    float before = *w;
    uint64_t x0 = c->x[0];
    float s0;
    memcpy(&s0, &c->v[0], 4);
    rt_call_original(c, addr);
    float after = *w;
    if (after != before && n < 200) {
        n++;
        LOG("[fnwatch] present %llu f_%llx (x0 %llx s0 %g, x0 +0x78 %g): %g -> %g (d %g)\n", (unsigned long long)renderer_game_frame(),
            (unsigned long long)addr, (unsigned long long)x0, s0, x0 >= GUEST_LO ? *(const float *)(uintptr_t)(x0 + 0x78) : 0.0f,
            before, after, after - before);
    }
    return 1;
}
__attribute__((constructor)) static void watch_init(void) {
    const char *e = getenv("LARECOMP_FNWATCH"), *fns = getenv("LARECOMP_FNWATCH_FNS");  // (LARECOMP_WATCH: memwatch.c's)
    if (!e || !fns) return;
    g_watch = strtoull(e, NULL, 16);
    extern void rt_register_fnhook_raw(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", fns);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) rt_register_fnhook_raw(strtoull(t, NULL, 16), watch_hook);
}

// ---- research: LARECOMP_TIME_PROBE="<fn addr>[,...]" (+ LARECOMP_TIME_PROBE_FRAMES=<first>-<last>) ------------------
// The readers of the frame-duration table G take an elapsed-time object in x1 (its +8: nanoseconds): log, per call, the
// present, x1's elapsed time and what the function returned (x0, and d0 as a double), up to 600 lines.
static int time_probe_hook(Cpu *c, uint64_t addr) {
    static int lines;
    uint64_t x1 = c->x[1];
    long f = (long)renderer_game_frame();
    int64_t el = x1 >= GUEST_LO && x1 < GUEST_HI ? *(const int64_t *)(uintptr_t)(x1 + 8) : -1;
    rt_call_original(c, addr);
    if (f >= g_tp_from && f <= g_tp_to && lines < 600) {
        double d0;
        memcpy(&d0, &c->v[0], 8);
        LOG("[time] present %ld f_%llx elapsed %lld ns -> x0 %llx d0 %g\n", f, (unsigned long long)addr, (long long)el,
            (unsigned long long)c->x[0], d0);
        lines++;
    }
    return 1;
}
__attribute__((constructor)) static void time_probe_init(void) {
    const char *e = getenv("LARECOMP_TIME_PROBE");
    if (!e) return;
    const char *fr = getenv("LARECOMP_TIME_PROBE_FRAMES");
    if (fr) { g_tp_from = atol(fr); g_tp_to = strchr(fr, '-') ? atol(strchr(fr, '-') + 1) : g_tp_from; }
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) rt_register_fnhook(strtoull(t, NULL, 16), time_probe_hook);
}

// ---- research: LARECOMP_TIMER_USERS=<first>-<last> -------------------------------------------------------------
// Who reads the frame-step timers: calls to their getter f_70000395f0 (x1 = the timer, the caller's lr), counted over
// those presents and listed at the last one. Timer 1 is the step the accessor f_7000039600 rewrites every frame with
// G[1]; timer 0 keeps 33.3 ms.
#define TIMER_GETTER 0x70000395f0ull
static struct { uint64_t lr; int which; uint64_t n; } g_tu[256];
static int g_ntu;
static long g_tu_from, g_tu_to;
static int timer_users_hook(Cpu *c, uint64_t addr) {
    long f = (long)renderer_game_frame();
    if (f >= g_tu_from && f <= g_tu_to) {
        uint64_t lr = c->x[30];
        int which = (int)c->x[1], i;
        for (i = 0; i < g_ntu && !(g_tu[i].lr == lr && g_tu[i].which == which); i++) {}
        if (i == g_ntu && g_ntu < 256) g_tu[g_ntu++] = (__typeof__(g_tu[0])){lr, which, 0};
        if (i < 256) g_tu[i].n++;
    } else if (f > g_tu_to && g_ntu) {
        for (int i = 0; i < g_ntu; i++)
            LOG("[timers] timer %d read from lr %llx: %llu times in %ld presents\n", g_tu[i].which, (unsigned long long)g_tu[i].lr,
                (unsigned long long)g_tu[i].n, g_tu_to - g_tu_from + 1);
        g_ntu = 0;
        g_tu_to = -1;
    }
    (void)addr;
    return 0;
}
__attribute__((constructor)) static void timer_users_init(void) {
    const char *e = getenv("LARECOMP_TIMER_USERS");
    if (!e) return;
    g_tu_from = atol(e);
    g_tu_to = strchr(e, '-') ? atol(strchr(e, '-') + 1) : g_tu_from;
    rt_register_fnhook(TIMER_GETTER, timer_users_hook);
}

// ---- research: LARECOMP_CALLCOUNT="<fn>[,...]" (+ LARECOMP_TIME_PROBE_FRAMES): calls per present of each function
#define CC_MAX 8
static uint64_t g_cc_fn[CC_MAX];
static unsigned g_cc_n[CC_MAX];
static int g_ncc;
static long g_cc_frame = -1;
static int callcount_hook(Cpu *c, uint64_t addr) {
    long f = (long)renderer_game_frame();
    if (f != g_cc_frame) {
        if (g_cc_frame >= g_tp_from && g_cc_frame <= g_tp_to) {
            char line[256];
            int o = snprintf(line, sizeof line, "[calls] present %ld:", g_cc_frame);
            for (int i = 0; i < g_ncc; i++) o += snprintf(line + o, sizeof line - o, " f_%llx %u", (unsigned long long)g_cc_fn[i], g_cc_n[i]);
            LOG("%s\n", line);
        }
        memset(g_cc_n, 0, sizeof g_cc_n);
        g_cc_frame = f;
    }
    for (int i = 0; i < g_ncc; i++) if (g_cc_fn[i] == addr) g_cc_n[i]++;
    (void)c;
    return 0;
}
__attribute__((constructor)) static void callcount_init(void) {
    const char *e = getenv("LARECOMP_CALLCOUNT");
    if (!e) return;
    const char *fr = getenv("LARECOMP_TIME_PROBE_FRAMES");
    if (fr) { g_tp_from = atol(fr); g_tp_to = strchr(fr, '-') ? atol(strchr(fr, '-') + 1) : g_tp_from; }
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t && g_ncc < CC_MAX; t = strtok(NULL, ",")) {
        g_cc_fn[g_ncc++] = strtoull(t, NULL, 16);
        rt_register_fnhook(g_cc_fn[g_ncc - 1], callcount_hook);
    }
}

// ---- research: LARECOMP_PROBE="<fn addr>[:<bytes>]" ----------------------------------------------------------------
// At each entry of the function, remember its x0 (object) and, after the original returns, x0 (result): for the next
// LARECOMP_PROBE_FRAMES (120) presents, compare `bytes` (0x400) of each of them with the previous present and log, at
// the end, every 32-bit word that changed: how many presents, and its usual step as an integer and as a float. A
// frame counter shows as an integer stepping by 1 (or a float by a constant) every present of the scene.
#define PROBE_MAX 4
static struct { uint64_t base; const char *what; uint32_t prev[0x400 / 4]; uint32_t changed[0x400 / 4]; int32_t step[0x400 / 4]; uint32_t step_n[0x400 / 4]; bool have; } g_probe[PROBE_MAX];
static int g_nprobe;
static uint32_t g_probe_bytes = 0x400;
static uint64_t g_probe_from, g_probe_to;
static int probe_hook(Cpu *c, uint64_t addr) {
    uint64_t obj = c->x[0];
    rt_call_original(c, addr);
    uint64_t ret = c->x[0];
    if (!g_probe_from) g_probe_from = renderer_game_frame() + 1, g_probe_to = g_probe_from + (getenv("LARECOMP_PROBE_FRAMES") ? atoi(getenv("LARECOMP_PROBE_FRAMES")) : 120);
    for (int pass = 0; pass < 2 && g_nprobe < PROBE_MAX; pass++) {
        uint64_t p = pass ? ret : obj;
        if (p < GUEST_LO || p >= GUEST_HI) continue;
        g_probe[g_nprobe].base = p, g_probe[g_nprobe].what = pass ? "returned object" : "x0 object";
        LOG("[probe] f_%llx %s %llx\n", (unsigned long long)addr, g_probe[g_nprobe].what, (unsigned long long)p);
        g_nprobe++;
    }
    return 1;
}
void fps60_probe_present(void) {
    uint64_t f = renderer_game_frame();
    if (!g_probe_from || f < g_probe_from || f > g_probe_to) return;
    for (int i = 0; i < g_nprobe; i++) {
        const uint32_t *now = (const uint32_t *)(uintptr_t)g_probe[i].base;
        for (uint32_t w = 0; w < g_probe_bytes / 4; w++) {
            if (g_probe[i].have && now[w] != g_probe[i].prev[w]) {
                g_probe[i].changed[w]++;
                int32_t d = (int32_t)(now[w] - g_probe[i].prev[w]);
                if (d == g_probe[i].step[w]) g_probe[i].step_n[w]++;
                else if (g_probe[i].step_n[w] == 0) g_probe[i].step[w] = d;
            }
            g_probe[i].prev[w] = now[w];
        }
        g_probe[i].have = true;
    }
    if (f == g_probe_to) {
        for (int i = 0; i < g_nprobe; i++)
            for (uint32_t w = 0; w < g_probe_bytes / 4; w++) {
                if (g_probe[i].changed[w] < 5) continue;
                float fv;
                memcpy(&fv, &g_probe[i].prev[w], 4);
                LOG("[probe] %s +0x%03x: changed in %u of %llu presents; usual step %d (%u times) value %u / %g\n", g_probe[i].what, w * 4,
                    g_probe[i].changed[w], (unsigned long long)(g_probe_to - g_probe_from), g_probe[i].step[w], g_probe[i].step_n[w],
                    g_probe[i].prev[w], fv);
            }
    }
}
__attribute__((constructor)) static void probe_init(void) {
    const char *e = getenv("LARECOMP_PROBE");
    if (!e) return;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", e);
    char *colon = strchr(buf, ':');
    if (colon) *colon = 0, g_probe_bytes = (uint32_t)strtoul(colon + 1, NULL, 16);
    if (g_probe_bytes > 0x400) g_probe_bytes = 0x400;
    rt_register_fnhook(strtoull(buf, NULL, 16), probe_hook);
}
