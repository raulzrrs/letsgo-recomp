// Big files (issue #55). The game reads a whole file into a buffer it allocates from one of its own heaps (gflib's
// TLSF pools: a class whose Allocate is f_700001b910 and Free f_700001b970), at f_7000032220: heap->Allocate(file
// size, 0x1000), then nn::fs::ReadFile into it. Those heaps are sized for the game's files (the one used while booting
// has 8 MB, each child heap fits its parent exactly), so a mod's bigger file got no buffer and the SDK stopped the game
// with 2002-6063 (a null buffer), on the emulator too.
// When that Allocate fails, the buffer comes from a reserve of ours instead, and the heap's Free of it goes back there.
// The reserve has to be guest memory: the buffer goes to the fs service in an IPC descriptor, whose address fields hold
// 39 bits (a host allocation above that came back cut). It is the end of the guest window, past the most the kernel
// heap can grow to (rt.h: HEAP_BASE + HEAP_MAX = 0x7310000000, GUEST_HI = 0x7320000000): 256 MB, committed as used.
// LARECOMP_FILE_RESERVE=0 turns it off (the game's behaviour). Same addresses in Pikachu 1.0.0 (tools/addrmap.py);
// not mapped for Eevee 1.0.2.
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../hle.h"
#include "../platform.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));  // runtime/fnhook.c
void rt_call_original(Cpu *c, uint64_t addr);
uint64_t rt_addr(uint64_t eevee);  // runtime/fnhook.c: an Eevee 1.0.0 address in the running build (0: not mapped)

#define HEAP_ALLOCATE  0x700001b910ull  // the heap class's Allocate(this, size, alignment)
#define HEAP_FREE      0x700001b970ull  // Free(this, pointer)
#define LR_FILE_BUFFER 0x7000032260ull  // f_7000032220: the file buffer's Allocate returns here
#define RESERVE_BASE   0x7310000000ull  // (rt.h: HEAP_BASE + HEAP_MAX)
#define RESERVE_SIZE   0x0010000000ull  // (up to GUEST_HI)

// First fit over a list of blocks kept in address order (few at a time: the files over a heap's room)
typedef struct { uint64_t addr, size; } Block;
static Block g_used[256];
static int g_nused;
static uint64_t g_committed;  // bytes from RESERVE_BASE committed at this boot
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_on = -1;

static uint64_t reserve_alloc(uint64_t size, uint64_t align) {
    if (align < 0x1000) align = 0x1000;
    size = (size + 0xFFF) & ~0xFFFull;
    pthread_mutex_lock(&g_lock);
    uint64_t at = RESERVE_BASE, got = 0;
    int i = 0;
    for (;; i++) {
        at = (at + align - 1) & ~(align - 1);
        uint64_t end = i < g_nused ? g_used[i].addr : RESERVE_BASE + RESERVE_SIZE;
        if (at + size <= end) { got = at; break; }
        if (i >= g_nused) break;
        at = g_used[i].addr + g_used[i].size;
    }
    if (got && g_nused < (int)(sizeof g_used / sizeof *g_used)) {
        for (int k = g_nused; k > i; k--) g_used[k] = g_used[k - 1];
        g_used[i] = (Block){got, size};
        g_nused++;
        if (got + size - RESERVE_BASE > g_committed) {
            plat_commit(RESERVE_BASE + g_committed, got + size - RESERVE_BASE - g_committed);
            g_committed = got + size - RESERVE_BASE;
        }
    } else {
        got = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

static bool reserve_free(uint64_t p) {
    if (p < RESERVE_BASE || p >= RESERVE_BASE + RESERVE_SIZE) return false;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nused; i++)
        if (g_used[i].addr == p) {
            for (int k = i; k + 1 < g_nused; k++) g_used[k] = g_used[k + 1];
            g_nused--;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return true;
}

static int alloc_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != rt_addr(LR_FILE_BUFFER)) return 0;
    uint64_t size = c->x[1], align = c->x[2];
    rt_call_original(c, addr);
    if (c->x[0]) return 1;
    uint64_t p = reserve_alloc(size, align);
    static int logged;
    if (logged++ < 16)
        LOG("[heaps] a %llu KB file has no room in the game's heap: %s\n", (unsigned long long)(size >> 10),
            p ? "read into the reserve for big files" : "the reserve is full too");
    c->x[0] = p;
    return 1;
}

static int free_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    return reserve_free(c->x[1]);  // (ours: not the heap's to take back)
}

// Before each boot (runtime/main.c)
void file_heap_configure(void) {
    static bool hooked;
    pthread_mutex_lock(&g_lock);
    g_nused = 0;
    g_committed = 0;  // (a reboot wipes the window: committed again as used)
    pthread_mutex_unlock(&g_lock);
    if (g_on < 0) {
        const char *e = getenv("LARECOMP_FILE_RESERVE");
        g_on = !(e && *e == '0');
    }
    if (!g_on || hooked || !rt_addr(HEAP_ALLOCATE) || !rt_addr(HEAP_FREE) || !rt_addr(LR_FILE_BUFFER)) return;
    hooked = true;
    rt_register_fnhook(HEAP_ALLOCATE, alloc_hook);
    rt_register_fnhook(HEAP_FREE, free_hook);
}
