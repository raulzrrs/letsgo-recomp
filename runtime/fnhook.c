// Guest function hooks. Every generated function starts with FN_ENTER (runtime/cpu.h); while g_fnhook is set it
// calls rt_fnhook first, and a hook that returns 1 has handled the call - the guest function returns at once (the
// hook sets x0 etc. as the function would). rt_call_original runs the game's own function from inside a hook
// (wrapping). Cost when no hook is registered: one predictable branch per call.
//
// Investigation (docs/MODO-BATALHA.md): LARECOMP_HOOK_DUMP="<addr>:<arg>,<arg>...[;<addr>:...]" logs, the first
// LARECOMP_HOOK_DUMP_MAX (default 4) times each function is entered, the registers x0..x7 and the memory the args
// name: "x2:30" = 0x30 bytes at x2, "*x0:400" = 0x400 bytes at the pointer stored at x0. LARECOMP_HOOK_DUMP_FRAMES=
// <first>-<last> limits it to those presents (the count then starts there).
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "gamesetup.h"
#include "hle.h"

volatile int g_fnhook;

typedef int (*FnHook)(Cpu *c, uint64_t addr);
#define NHOOK 1024
static struct { uint64_t addr; FnHook fn; } hooks[NHOOK];
static __thread uint64_t bypass;  // rt_call_original: the next entry of this function is the original running

// The runtime's hooks are written with addresses of Let's Go Eevee 1.0.0's main (the build the research was done on,
// docs/). The same functions in the other known builds, found by tools/addrmap.py (the code matches instruction by
// instruction, struct offsets included): each registration is kept as written and the hook table is rebuilt for the
// running build (rt_fnhook_set_build, before each boot; an unknown build gets none). Hooks at addresses the user gave
// (LARECOMP_HOOK_DUMP) are taken as they are.
static const uint64_t k_pikachu_100[][2] = {
    {0x700011ece0, 0x700011ecb0}, {0x700011ee50, 0x700011ee20}, {0x700017f0a0, 0x700017f070},
    {0x70002185d0, 0x70002185a0}, {0x70002186a0, 0x7000218670}, {0x7000289ac0, 0x7000289a90},
    {0x7000289b40, 0x7000289b10}, {0x7000289f60, 0x7000289f30}, {0x7000289fd0, 0x7000289fa0},
    {0x700028bb60, 0x700028bb30}, {0x700028ef70, 0x700028ef40}, {0x70002bad00, 0x70002bacd0},
    {0x70002f60c0, 0x70002f6090}, {0x70002f65d0, 0x70002f65a0}, {0x70002f78d0, 0x70002f78a0},
    {0x70002f8bb0, 0x70002f8b80}, {0x70003058f0, 0x70003058c0}, {0x7000305a98, 0x7000305a68},
    {0x7000305ad0, 0x7000305aa0}, {0x70006237b0, 0x7000623750}, {0x7000641190, 0x7000641130},
    {0x7000663660, 0x7000663600}, {0x7000664cd0, 0x7000664c70},
    {0x70002462e0, 0x70002462b0}, {0x7000197e90, 0x7000197e60}, {0x7000198b20, 0x7000198af0},
    {0x700021b720, 0x700021b6f0}, {0x700020b810, 0x700020b7e0},  // (fps60.c: the walking at 60 / 120 fps)
    {0x70000395f0, 0x70000395f0}, {0x7000039600, 0x7000039600}, {0x700004eb50, 0x700004eb50},  // (same place)
    {0x7000180a20, 0x70001809f0}, {0x7000180a70, 0x7000180a40},  // (cutscene.c: the natives EvCameraStart / End)
    // (wildbattle.c: catching during the battle)
    {0x700028be80, 0x700028be50}, {0x70001afc00, 0x70001afbd0}, {0x700031a0e0, 0x700031a0b0}, {0x7000368330, 0x70003682d0},
    {0x70006786b0, 0x7000678650}, {0x700071dfd0, 0x700071df70}, {0x70007dcb00, 0x70007dcb30}, {0x7000919970, 0x70009199a0},
    {0x70009e2490, 0x70009e24a0}, {0x70009e4280, 0x70009e4290}, {0x7000ab1670, 0x7000ab1680}, {0x7000ab1af0, 0x7000ab1b00},
    {0x7000ab1e80, 0x7000ab1e90}, {0x7000ab2730, 0x7000ab2740}, {0x7000ab49f0, 0x7000ab4a00}, {0x7000ab4a20, 0x7000ab4a30},
    {0x7000ab4a30, 0x7000ab4a40}, {0x7000ab4a50, 0x7000ab4a60}, {0x700028c770, 0x700028c740}, {0x70009d8ae0, 0x70009d8af0},
    {0x70007de080, 0x70007de0b0}, {0x7000a5c350, 0x7000a5c360}, {0x7000aaf230, 0x7000aaf240}, {0x7000319bc0, 0x7000319b90}, {0x70007de440, 0x70007de470}, {0x70009f7300, 0x70009f7310},
    {0x7000a7e090, 0x7000a7e0a0}, {0x7000a99b40, 0x7000a99b50}, {0x7000a9c700, 0x7000a9c710}, {0x700016d060, 0x700016d030},
};
static int g_build = -1;  // GAME_BUILD_* (runtime/gamesetup.h); -1 before the first boot

// (in this build's guest window: GA, rt.h)
uint64_t rt_addr(uint64_t eevee) {
    if (g_build == GAME_BUILD_EEVEE_100 || g_build < 0) return GA(eevee);
    if (g_build == GAME_BUILD_PIKACHU_100)
        for (size_t i = 0; i < sizeof k_pikachu_100 / sizeof *k_pikachu_100; i++)
            if (k_pikachu_100[i][0] == eevee) return GA(k_pikachu_100[i][1]);
    return 0;
}

#define NREG 256
static struct { uint64_t addr; FnHook fn; bool user; } regs[NREG];
static int nregs;

static void add_hook(uint64_t addr, FnHook fn) {
    if (!addr) return;
    uint32_t s = (uint32_t)((addr >> 2) * 0x9E3779B1u) & (NHOOK - 1);
    while (hooks[s].addr && hooks[s].addr != addr) s = (s + 1) & (NHOOK - 1);
    // (the function before the address: hooks are also added while the game runs - the F12 menu's frame rate - and a
    // thread looking the slot up must not find the address without its function)
    hooks[s].fn = fn;
    atomic_thread_fence(memory_order_release);
    hooks[s].addr = addr;
    g_fnhook = 1;
}

static void reg(uint64_t addr, FnHook fn, bool user) {
    for (int i = 0; i < nregs; i++)
        if (regs[i].addr == addr && regs[i].user == user) { regs[i].fn = fn; goto add; }
    if (nregs == NREG) { LOG("[fnhook] too many hooks\n"); return; }
    regs[nregs].addr = addr, regs[nregs].fn = fn, regs[nregs].user = user, nregs++;
add:
    if (user) add_hook(addr, fn);
    else if (g_build >= 0) {
        uint64_t a = rt_addr(addr);
        if (!a) LOG("[fnhook] %llx: no such function in this build\n", (unsigned long long)addr);
        add_hook(a, fn);
    }
}

void rt_register_fnhook(uint64_t addr, FnHook fn) { reg(addr, fn, false); }
// A hook at an address of the running build as it is (research: LARECOMP_STEP_SCALE, as LARECOMP_HOOK_DUMP's)
void rt_register_fnhook_raw(uint64_t addr, FnHook fn) { reg(addr, fn, true); }

void rt_fnhook_set_build(int build) {
    if (build == g_build) return;
    g_build = build;
    memset(hooks, 0, sizeof hooks);
    g_fnhook = 0;
    int n = nregs;
    nregs = 0;
    for (int i = 0; i < n; i++) reg(regs[i].addr, regs[i].fn, regs[i].user);
}

// (Every game function's entry passes here: the table first - bypass only ever names a hooked function, and the
// thread-local read cost every call)
int rt_fnhook(Cpu *c, uint64_t addr) {
    uint32_t s = (uint32_t)((addr >> 2) * 0x9E3779B1u) & (NHOOK - 1);
    while (hooks[s].addr) {
        if (hooks[s].addr == addr) {
            if (bypass == addr) { bypass = 0; return 0; }
            return hooks[s].fn(c, addr);
        }
        s = (s + 1) & (NHOOK - 1);
    }
    return 0;
}

// The game's function at addr (gamelib tables), run as if called: x0.. as set by the caller of the hook.
// addr: the hooked function (as the hook received it) or an Eevee 1.0.0 address from the runtime's code (translated).
void rt_call_original(Cpu *c, uint64_t addr) {
    if (g_build == GAME_BUILD_PIKACHU_100)
        for (size_t i = 0; i < sizeof k_pikachu_100 / sizeof *k_pikachu_100; i++)
            if (k_pikachu_100[i][0] == addr) { addr = k_pikachu_100[i][1]; break; }
    // (a canonical address from the runtime's code, when the window has moved: a hooked one is already in the window)
    if (GUEST_LO != 0x7000000000ull && addr >= 0x7000000000ull && addr < 0x7320000000ull) addr = GA(addr);
    const GameLibTables *t = gamelib_tables();
    for (uint32_t m = 0; t && m < t->nmods; m++) {
        const GameLibModule *md = &t->mods[m];
        uint64_t lo = 0, hi = *md->count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (md->addrs[mid] < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < *md->count && md->addrs[lo] == addr) {
            bypass = addr;
            md->fns[lo](c);
            bypass = 0;  // (a function with no hook never took it: rt_fnhook reads it only on a hooked address)
            return;
        }
    }
    LOG("[fnhook] no game function at %llx\n", (unsigned long long)addr);
}

// ---- LARECOMP_HOOK_DUMP
typedef struct { uint64_t addr; int nargs; struct { int reg, deref; uint32_t len; } arg[8]; _Atomic int hits; } Dump;
static Dump dumps[32];
static int ndumps, dump_max = 4;

static bool guest_ptr(uint64_t p) { return p >= GUEST_LO && p < GUEST_HI; }

static void hexdump(const char *what, uint64_t p, uint32_t len) {
    LOG("[hook]   %s @%llx (%u bytes):\n", what, (unsigned long long)p, len);
    if (!guest_ptr(p)) { LOG("[hook]     (not guest memory)\n"); return; }
    const uint8_t *b = (const uint8_t *)(uintptr_t)p;
    for (uint32_t o = 0; o < len; o += 16) {
        char line[128];
        int n = snprintf(line, sizeof line, "[hook]     +%03x:", o);
        for (uint32_t k = 0; k < 16 && o + k < len; k += 4) n += snprintf(line + n, sizeof line - n, " %08x", *(const uint32_t *)(b + o + k));
        LOG("%s\n", line);
    }
}

static long dump_from = -1, dump_to = -1;  // LARECOMP_HOOK_DUMP_FRAMES=<first>-<last>: only during those presents

static int dump_hook(Cpu *c, uint64_t addr) {
    extern uint64_t renderer_present_count(void);
    uint64_t frame = renderer_present_count();
    if (dump_from >= 0 && ((long)frame < dump_from || (long)frame > dump_to)) return 0;
    for (int i = 0; i < ndumps; i++) {
        Dump *d = &dumps[i];
        if (d->addr != addr || atomic_fetch_add(&d->hits, 1) >= dump_max) continue;
        LOG("[hook] f%llu %llx entered (#%d) from lr %llx: x0 %llx x1 %llx x2 %llx x3 %llx x4 %llx x5 %llx x6 %llx x7 %llx\n",
            (unsigned long long)frame, (unsigned long long)addr, d->hits, (unsigned long long)c->x[30], (unsigned long long)c->x[0], (unsigned long long)c->x[1],
            (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[4], (unsigned long long)c->x[5],
            (unsigned long long)c->x[6], (unsigned long long)c->x[7]);
        for (int k = 0; k < d->nargs; k++) {
            uint64_t p = c->x[d->arg[k].reg];
            char what[16];
            snprintf(what, sizeof what, "%sx%d", d->arg[k].deref ? "*" : "", d->arg[k].reg);
            if (d->arg[k].deref) p = guest_ptr(p) ? *(const uint64_t *)(uintptr_t)p : 0;
            hexdump(what, p, d->arg[k].len);
        }
    }
    return 0;  // (the game's function runs as usual)
}

__attribute__((constructor)) static void hook_dump_init(void) {
    const char *e = getenv("LARECOMP_HOOK_DUMP");
    if (!e) return;
    if (getenv("LARECOMP_HOOK_DUMP_MAX")) dump_max = atoi(getenv("LARECOMP_HOOK_DUMP_MAX"));
    if (getenv("LARECOMP_HOOK_DUMP_FRAMES")) {
        const char *f = getenv("LARECOMP_HOOK_DUMP_FRAMES");
        dump_from = atol(f);
        dump_to = strchr(f, '-') ? atol(strchr(f, '-') + 1) : dump_from;
    }
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *fn = strtok(buf, ";"); fn && ndumps < 32; fn = strtok(NULL, ";")) {
        Dump *d = &dumps[ndumps];
        char *colon = strchr(fn, ':');
        d->addr = strtoull(fn, NULL, 16);
        for (char *a = colon ? colon + 1 : NULL; a && *a && d->nargs < 8;) {
            int deref = *a == '*';
            if (deref) a++;
            if (*a != 'x') break;
            int reg = (int)strtol(a + 1, &a, 10);
            uint32_t len = *a == ':' ? (uint32_t)strtoul(a + 1, &a, 16) : 0x40;
            d->arg[d->nargs].reg = reg;
            d->arg[d->nargs].deref = deref;
            d->arg[d->nargs].len = len;
            d->nargs++;
            while (*a == ',') a++;
        }
        ndumps++;
    }
    for (int i = 0; i < ndumps; i++) reg(dumps[i].addr, dump_hook, true);
}
