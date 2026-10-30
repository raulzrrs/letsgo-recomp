// Research aid (docs/DEPURACAO.md): who writes a word of the game's memory. The page holding it is made read-only; the first
// write to it raises an access violation, a vectored handler logs the writer's backtrace (the guest functions show as
// f_<address>), makes the page writable, single-steps the store and protects the page again. Windows only.
//  LARECOMP_WATCH=<addr>[,<addr>...]        watch these 32-bit words (hex guest addresses) from the start
//  LARECOMP_WATCH_AT=<present>              ... from that present on instead (a heap object not there at the start)
//  LARECOMP_WATCH_MAX=<n>                   log only the first n writes (default: all)
//  LARECOMP_WATCH_READ=1                    reads too (the page is made inaccessible): who reads a table's field
//  LARECOMP_WATCH_PARTY=<present>           at that present, look for the party's Pokémon (the 0x104-byte PB7 records: species
//                                           u16 at +8, experience u32 at +0x10, level byte at +0xEC) and watch every
//                                           experience word found: catching a Pokémon then shows who awards the experience
//  LARECOMP_SNAP=<present A>,<present B>   copy the heaps' memory at A and at B list the 32-bit words that grew by the same small
//                                           amount (the most common deltas with sample addresses): a Pokémon's experience gain is one
//                                           delta shared by every party member, so its words stand out
#ifdef _WIN32
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../hle.h"
#include "../platform.h"

uint64_t renderer_game_frame(void);

#define MAXW 48
static uint64_t g_addr[MAXW];
static int g_n;
static __thread uint64_t t_rearm;  // the page to protect again after the single step

static bool watched_page(uint64_t page) {
    for (int i = 0; i < g_n; i++) if ((g_addr[i] & ~0xfffull) == page) return true;
    return false;
}

static int g_read = -1;
static DWORD guard(void) {
    if (g_read < 0) g_read = getenv("LARECOMP_WATCH_READ") ? atoi(getenv("LARECOMP_WATCH_READ")) : 0;
    return g_read ? PAGE_NOACCESS : PAGE_READONLY;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 &&
        (r->ExceptionInformation[0] == 1 || (g_read > 0 && r->ExceptionInformation[0] == 0))) {
        uint64_t a = (uint64_t)r->ExceptionInformation[1], page = a & ~0xfffull;
        if (!watched_page(page)) return EXCEPTION_CONTINUE_SEARCH;
        static long max = -2;
        static volatile long logged, seen;
        if (seen++ < 8) LOG("[watch] f%llu access %llx (%s) on a watched page\n", (unsigned long long)renderer_game_frame(), (unsigned long long)a,
                             r->ExceptionInformation[0] ? "write" : "read");
        if (max == -2) max = getenv("LARECOMP_WATCH_MAX") ? atol(getenv("LARECOMP_WATCH_MAX")) : -1;
        for (int i = 0; i < g_n && (max < 0 || logged < max); i++) {
            // (a store of up to 16 bytes starting before the word: SIMD vectors; a read, up to 64: a block copy)
            if (a + (r->ExceptionInformation[0] ? 16 : 64) > g_addr[i] && a < g_addr[i] + 4) {
                logged++;
                LOG("[watch] f%llu write to %llx (was %08x)\n", (unsigned long long)renderer_game_frame(), (unsigned long long)g_addr[i],
                    *(volatile uint32_t *)(uintptr_t)g_addr[i]);
                plat_print_backtrace();
                break;
            }
        }
        DWORD old;
        VirtualProtect((void *)(uintptr_t)page, 4096, PAGE_READWRITE, &old);
        t_rearm = page;
        ep->ContextRecord->EFlags |= 0x100;  // single step
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_rearm) {
        DWORD old;
        VirtualProtect((void *)(uintptr_t)t_rearm, 4096, guard(), &old);
        t_rearm = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void watch(uint64_t addr) {
    if (g_n >= MAXW) return;
    for (int i = 0; i < g_n; i++) if (g_addr[i] == addr) return;
    static bool installed;
    if (!installed) { installed = true; AddVectoredExceptionHandler(1, veh); }
    g_addr[g_n++] = addr;
    DWORD old;
    VirtualProtect((void *)(uintptr_t)(addr & ~0xfffull), 4096, PAGE_READONLY, &old);
    LOG("[watch] watching %llx (now %08x)\n", (unsigned long long)addr, *(volatile uint32_t *)(uintptr_t)addr);
}

// LARECOMP_WATCH_SPECIES="133:10,16:9,...": only records with one of these species:level pairs (the party you know)
static bool known_pair(uint16_t sp, uint8_t lv) {
    const char *e = getenv("LARECOMP_WATCH_SPECIES");
    if (!e) return true;
    for (const char *p = e; *p;) {
        char *end;
        long s = strtol(p, &end, 10);
        if (*end != ':') break;
        long l = strtol(end + 1, &end, 10);
        if (s == sp && l == lv) return true;
        p = *end == ',' ? end + 1 : end;
    }
    return false;
}

static void find_party(void) {
    uint64_t a = GUEST_LO;
    int found = 0;
    while (a < GUEST_HI) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) break;
        uint64_t base = (uint64_t)(uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_READWRITE) && size >= 0x104 && base >= GA(0x7200000000ull)) {  // (the heaps: the modules' data tables only look like Pokémon)
            const uint8_t *b = (const uint8_t *)(uintptr_t)base;
            for (size_t o = 0; o + 0x104 <= size; o += 4) {
                uint16_t sp = *(const uint16_t *)(b + o + 8);
                uint8_t lv = b[o + 0xEC];
                uint32_t exp = *(const uint32_t *)(b + o + 0x10);
                if (sp < 1 || sp > 151 || lv < 2 || lv > 100 || exp < 8 || exp > 1700000) continue;
                if (!known_pair(sp, lv)) continue;
                // sanity: the level matches the experience on the medium curves within a wide band (n^3 .. 1.3 n^3)
                double n3 = (double)lv * lv * lv;
                if (!getenv("LARECOMP_WATCH_SPECIES") && (exp < n3 * 0.8 || exp > n3 * 1.45)) continue;
                if (*(const uint32_t *)(b + o) == 0) continue;  // (an encryption constant is never 0)
                LOG("[watch] party candidate at %llx: species %u level %u exp %u\n", (unsigned long long)(base + o), sp, lv, exp);
                if (found < 24) watch(base + o + 0x10);
                found++;
            }
        }
        a = base + size;
    }
    LOG("[watch] %d party candidate(s)\n", found);
}

static DWORD WINAPI party_thread(LPVOID arg) {
    long at = (long)(intptr_t)arg;
    while ((long)renderer_game_frame() < at) Sleep(100);
    find_party();
    return 0;
}


// ---- LARECOMP_SNAP
// Only the 4 KB pages that are not all zero are copied (the heap region is gigabytes of mostly untouched memory).
typedef struct { uint64_t addr; uint8_t *copy; } SnapPage;
static SnapPage *g_pages;
static size_t g_npages, g_cappages;

static void snap_take(void) {
    static const uint8_t zero[4096];
    uint64_t a = GA(0x7200000000ull), bytes = 0;
    while (a < GUEST_HI) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) break;
        uint64_t base = (uint64_t)(uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY)) {
            for (uint64_t o = 0; o + 4096 <= size; o += 4096) {
                const uint8_t *pg = (const uint8_t *)(uintptr_t)(base + o);
                if (!memcmp(pg, zero, 4096)) continue;
                if (g_npages == g_cappages) {
                    g_cappages = g_cappages ? g_cappages * 2 : 4096;
                    g_pages = realloc(g_pages, g_cappages * sizeof *g_pages);
                }
                uint8_t *c = malloc(4096);
                if (!c) goto done;
                memcpy(c, pg, 4096);
                g_pages[g_npages++] = (SnapPage){base + o, c};
                bytes += 4096;
            }
        }
        a = base + size;
    }
done:
    LOG("[snap] %zu non-zero page(s), %llu MB copied\n", g_npages, (unsigned long long)(bytes >> 20));
}

#define MAXDELTA 4096
static void snap_diff(void) {
    static uint32_t count[MAXDELTA];
    static uint64_t sample[MAXDELTA][6];
    memset(count, 0, sizeof count);
    LOG("[snap] f%llu: growth since the copy\n", (unsigned long long)renderer_game_frame());
    for (size_t p = 0; p < g_npages; p++) {
        const uint32_t *now = (const uint32_t *)(uintptr_t)g_pages[p].addr, *old = (const uint32_t *)g_pages[p].copy;
        for (int i = 0; i < 1024; i++) {
            uint32_t d = now[i] - old[i];
            if (d >= 1 && d < MAXDELTA && old[i] != 0) {
                if (count[d] < 6) sample[d][count[d]] = g_pages[p].addr + 4 * i;
                count[d]++;
            }
        }
    }
    for (int rank = 0; rank < 14; rank++) {
        uint32_t best = 0, bd = 0;
        for (uint32_t d = 3; d < MAXDELTA; d++) if (count[d] > best && count[d] <= 20 && count[d] >= 3) best = count[d], bd = d;  // (a party's words: the noisy counters have thousands)
        if (!best) break;
        LOG("[snap] delta +%u: %u word(s), e.g.", bd, best);
        for (uint32_t k = 0; k < 6 && k < best; k++) LOG(" %llx", (unsigned long long)sample[bd][k]);
        LOG("\n");
        count[bd] = 0;
    }
}

static DWORD WINAPI snap_thread(LPVOID arg) {
    long a = (long)(intptr_t)arg & 0xffffff, b = (long)((intptr_t)arg >> 24);
    while ((long)renderer_game_frame() < a) Sleep(100);
    snap_take();
    for (int k = 0; k < 30; k++) {  // (every 250 presents from B on)
        while ((long)renderer_game_frame() < b + 250L * k) Sleep(100);
        snap_diff();
    }
    return 0;
}

static DWORD WINAPI watch_at_thread(LPVOID arg) {
    long at = (long)(intptr_t)arg;
    while ((long)renderer_game_frame() < at) Sleep(1);
    const char *w = getenv("LARECOMP_WATCH");
    for (const char *p = w; *p;) {
        watch(strtoull(p, (char **)&p, 16));
        while (*p == ',' || *p == ' ') p++;
    }
    return 0;
}

// LARECOMP_WATCH_LIKE=<present>:<addr>: at that present, every word of the heaps holding the value at addr now (another
// copy of a position: which one the game computes, and where) is watched (up to MAXW)
// (called at each present on the game thread, runtime/hle/fps60.c: the values hold still while it scans)
void memwatch_present(void) {
    const char *e = getenv("LARECOMP_WATCH_LIKE");
    if (!e || (long)renderer_game_frame() != atol(e)) return;
    uint64_t src = strchr(e, ':') ? strtoull(strchr(e, ':') + 1, NULL, 16) : 0;
    uint32_t v = *(volatile uint32_t *)(uintptr_t)src;
    int found = 0;
    for (uint64_t a = GA(0x7200000000ull); a < GUEST_HI;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) break;
        uint64_t base = (uint64_t)(uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE)
            for (uint64_t o = 0; o + 4 <= size; o += 4)
                if (*(const uint32_t *)(uintptr_t)(base + o) == v) {
                    LOG("[watch] %08x also at %llx\n", v, (unsigned long long)(base + o));
                    if (found++ < MAXW) watch(base + o);
                }
        a = base + size;
    }
    LOG("[watch] %d word(s) like %llx\n", found, (unsigned long long)src);
}

__attribute__((constructor)) static void memwatch_init(void) {
    const char *w = getenv("LARECOMP_WATCH");
    if (w && getenv("LARECOMP_WATCH_AT")) CreateThread(NULL, 0, watch_at_thread, (LPVOID)(intptr_t)atol(getenv("LARECOMP_WATCH_AT")), 0, NULL);
    else if (w) {
        for (const char *p = w; *p;) {
            watch(strtoull(p, (char **)&p, 16));
            while (*p == ',' || *p == ' ') p++;
        }
    }
    if (getenv("LARECOMP_SNAP")) {
        long a = 0, b = 0;
        sscanf(getenv("LARECOMP_SNAP"), "%ld,%ld", &a, &b);
        if (a > 0 && b > a) CreateThread(NULL, 0, snap_thread, (LPVOID)(((intptr_t)b << 24) | (intptr_t)a), 0, NULL);
    }
    if (getenv("LARECOMP_WATCH_PARTY")) CreateThread(NULL, 0, party_thread, (LPVOID)(intptr_t)atol(getenv("LARECOMP_WATCH_PARTY")), 0, NULL);
}
#endif
