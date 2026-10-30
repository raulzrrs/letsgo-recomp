// Research aid (docs/MODO-BATALHA.md): LARECOMP_SCAN="<hex bytes>[@<frame>]" scans the guest's mapped memory once the game
// has presented <frame> frames (default 1300: in the field) for those bytes and logs every hit with its region and the
// 0x40 bytes that follow; LARECOMP_SCAN_BACK=<n> also logs n records of 0x40 bytes before the hit (a table's first
// records) and searches memory for 8-byte pointers to hit - n*0x40 (whoever holds the table). Windows only (VirtualQuery).
#ifdef _WIN32
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "hle.h"

uint64_t renderer_game_frame(void);

static uint8_t g_pat[64];
static size_t g_patlen;
static long g_frame = 1300;
static int g_back;

static void dump(uint64_t p, size_t len) {
    for (size_t o = 0; o < len; o += 16) {
        char line[160];
        int n = snprintf(line, sizeof line, "[scan]     +%03zx:", o);
        for (size_t k = 0; k < 16 && o + k < len; k += 4) n += snprintf(line + n, sizeof line - n, " %08x", *(const uint32_t *)(uintptr_t)(p + o + k));
        LOG("%s\n", line);
    }
}

// calls fn(base, size) for each committed, readable, non-guard region of the guest range
static void each_region(void (*fn)(const uint8_t *, size_t, void *), void *arg) {
    uint64_t a = GUEST_LO;
    while (a < GUEST_HI) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) break;
        uint64_t base = (uint64_t)(uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        int ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                 (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
        if (ok) fn((const uint8_t *)(uintptr_t)base, (size_t)size, arg);
        a = base + size;
    }
}

typedef struct { const uint8_t *pat; size_t len; uint64_t hits[64]; int nhits; } Scan;

static void scan_region(const uint8_t *b, size_t size, void *arg) {
    Scan *s = arg;
    for (size_t o = 0; o + s->len <= size; o += (s->len % 8 == 0 ? 8 : 1)) {
        if (b[o] == s->pat[0] && !memcmp(b + o, s->pat, s->len)) {
            LOG("[scan] hit at %llx (region %llx size %zx)\n", (unsigned long long)(uintptr_t)(b + o), (unsigned long long)(uintptr_t)b, size);
            if (s->nhits < 64) s->hits[s->nhits++] = (uint64_t)(uintptr_t)(b + o);
        }
    }
}

static size_t g_len = 0x40;
static bool g_noptr;

static DWORD WINAPI scan_thread(LPVOID unused) {
    (void)unused;
    while ((long)renderer_game_frame() < g_frame) Sleep(50);
    Scan s = {g_pat, g_patlen, {0}, 0};
    LOG("[scan] scanning at present %llu for %zu bytes\n", (unsigned long long)renderer_game_frame(), g_patlen);
    each_region(scan_region, &s);
    LOG("[scan] %d hit(s)\n", s.nhits);
    for (int i = 0; i < s.nhits; i++) {
        uint64_t h = s.hits[i];
        if (g_back > 0) {
            LOG("[scan]   %d records before %llx:\n", g_back, (unsigned long long)h);
            dump(h - (uint64_t)g_back * 0x40, (size_t)g_back * 0x40);
        }
        LOG("[scan]   at %llx:\n", (unsigned long long)h);
        dump(h, g_len);
        if (g_noptr) continue;
        uint64_t base = h - (uint64_t)g_back * 0x40;
        uint8_t ptr[8];
        memcpy(ptr, &base, 8);
        Scan p = {ptr, 8, {0}, 0};
        LOG("[scan]   pointers to table base %llx:\n", (unsigned long long)base);
        each_region(scan_region, &p);
    }
    LOG("[scan] done\n");
    return 0;
}

__attribute__((constructor)) static void memscan_init(void) {
    const char *e = getenv("LARECOMP_SCAN");
    if (!e) return;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", e);
    char *at = strchr(buf, '@');
    if (at) { *at = 0; g_frame = atol(at + 1); }
    for (const char *c = buf; c[0] && c[1] && g_patlen < sizeof g_pat; c += 2) {
        char hex[3] = {c[0], c[1], 0};
        g_pat[g_patlen++] = (uint8_t)strtoul(hex, NULL, 16);
    }
    if (getenv("LARECOMP_SCAN_BACK")) g_back = atoi(getenv("LARECOMP_SCAN_BACK"));
    // LARECOMP_SCAN_LEN=<bytes> logged at each hit (default 0x40); LARECOMP_SCAN_NOPTR=1 skips the search for pointers to it
    if (getenv("LARECOMP_SCAN_LEN")) g_len = (size_t)strtoul(getenv("LARECOMP_SCAN_LEN"), NULL, 0);
    g_noptr = getenv("LARECOMP_SCAN_NOPTR") != NULL;
    if (g_patlen) CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
}
#endif
