// Sharper in-game text: the game's glyph cache drawn again at K x the resolution on the host.
//
// The game's text goes through nn::font's scalable font (in main, no symbols): TextureCache::UpdateTextureCache
// (Pikachu 1.0.0 f_70004654d0) rasterizes each new glyph with the SDK's nn::fontll engine at the glyph's font size
// and copies it into its cache texture - an 8-bit linear texture whose rows are stored bottom-up - at a cell of a
// line it allocated (one column of zeros on each side). Outline fonts get the outline glyph there and then the fill
// glyph subtracted at half strength (one channel: outline 255, fill ~128). Text is drawn from that texture with
// normalized coordinates, so an image K times as large in each axis drops in unchanged (runtime/nvn/vulkan/textures.c
// font_k): this file keeps that K x image in host memory, rasterizing every glyph the game caches a second time,
// at K x its font size, with the same engine and the same composition, into the K x cell.
//
// Where: the game's ReleasesGlyph call right after a glyph was written (recognised by the instructions around the
// return address, so it works in every build), with the cache in x19, the glyph's node in x26, the texture data in
// x23 and the outline glyph's top bearing on the caller's stack (+0x30). Node: +0x30 code, +0x34 font index, +0x36
// font size, +0x3a line kind, +0x40 / +0x42 cell x / line y, +0x44 width. Cache: +8 the engine, +0x28 / +0x2c the
// texture's size, +0x1be8 line kind -> line height, +0x1bf0 per font {+8 ascent / size}, +0x1c00 per font: outline.
// Glyph maps (nn::fontll, iType's FS_GLYPHMAP): +0xc left bearing, +0xe top bearing, +0x1c width (= bytes per row),
// +0x1e rows, +0x28 the 8-bit pixels.
//
// Always on: K from the window's height at the first glyph (one step per 720 lines, up to 4; 720p and below, and Android: off).
// LARECOMP_FONT_SCALE=<1..4> forces K (1: off, for comparisons); LARECOMP_FONT_LOG=1 logs the glyphs drawn (the
// first few hundred).
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

uint64_t loader_find_symbol(const char *name);
int renderer_font_scale(void);  // textures.c: K from the window

#define SYM_SET_SCALE "_ZN2nn6fontll18ScalableFontEngine8SetScaleEiiii"
#define SYM_ACQUIRE "_ZN2nn6fontll18ScalableFontEngine15AcquireGlyphmapEjt"
#define SYM_RELEASE "_ZN2nn6fontll18ScalableFontEngine13ReleasesGlyphEPv"
#define SYM_FLAGS "_ZN2nn6fontll18ScalableFontEngine8SetFlagsENS1_5FlagsE"
#define SYM_OUTLINE "_ZN2nn6fontll18ScalableFontEngine15SetOutlineWidthEt"

static uint64_t orig(const char *name, uint64_t *cache) {
    if (!*cache) *cache = loader_find_symbol(name);
    if (!*cache) FATAL("font: no SDK implementation of %s\n", name);
    return *cache;
}

// The SDK's own function, from inside a wrapper (x30 kept: the wrapper returns to the game).
static void call_sdk(Cpu *c, const char *sym, uint64_t *cache) {
    uint64_t lr = c->x[30];
    guest_call(c, orig(sym, cache));
    c->x[30] = lr;
}
static uint64_t a_scale, a_acquire, a_release, a_flags, a_outline;

// ---------------------------------------------------------------- engine state the game set (restored after ours)
#define NENG 8
static struct { uint64_t eng; uint32_t flags; uint32_t outline; } g_eng[NENG];
static pthread_mutex_t g_eng_lock = PTHREAD_MUTEX_INITIALIZER;
static int eng_slot(uint64_t e) {
    for (int i = 0; i < NENG; i++) if (g_eng[i].eng == e) return i;
    for (int i = 0; i < NENG; i++) if (!g_eng[i].eng) { g_eng[i].eng = e; g_eng[i].flags = ~0u; g_eng[i].outline = ~0u; return i; }
    return -1;
}

// ---------------------------------------------------------------- the K x images (one per glyph cache texture)
#define NCACHE 4
typedef struct {
    uint64_t addr;           // the game's texture data (texture address)
    uint32_t w, h, k;        // its size, the scale
    uint8_t *px;             // (w*k) x (h*k), rows bottom-up as the game's
    uint32_t x0, y0, x1, y1; // changed since the last upload (x1 <= x0: nothing)
} FontCache;
static FontCache g_fc[NCACHE];
static pthread_mutex_t g_fc_lock = PTHREAD_MUTEX_INITIALIZER;

RT_ON_RESET(font_hires_reset) {
    pthread_mutex_lock(&g_fc_lock);
    for (int i = 0; i < NCACHE; i++) free(g_fc[i].px);
    memset(g_fc, 0, sizeof g_fc);
    pthread_mutex_unlock(&g_fc_lock);
    pthread_mutex_lock(&g_eng_lock);
    memset(g_eng, 0, sizeof g_eng);
    pthread_mutex_unlock(&g_eng_lock);
}

static int log_left(void) {
    static int n = -1;
    if (n < 0) n = ENV("LARECOMP_FONT_LOG") ? 400 : 0;
    return n > 0 ? n-- : 0;
}

// The K for this game (fixed at the first glyph: the cache textures keep their size).
static int font_k(void) {
    static int k;
    if (!k) {
        const char *e = ENV("LARECOMP_FONT_SCALE");
        k = e ? atoi(e) : renderer_font_scale();
        if (k < 1) k = 1;
        if (k > 4) k = 4;
        LOG("[font] glyph cache at %dx\n", k);
    }
    return k;
}

static FontCache *cache_for(uint64_t addr, uint32_t w, uint32_t h, int k) {
    for (int i = 0; i < NCACHE; i++)
        if (g_fc[i].addr == addr) return g_fc[i].w == w && g_fc[i].h == h ? &g_fc[i] : NULL;
    for (int i = 0; i < NCACHE; i++)
        if (!g_fc[i].addr) {
            uint8_t *px = calloc((size_t)w * k * h * k, 1);
            if (!px) return NULL;
            g_fc[i] = (FontCache){addr, w, h, (uint32_t)k, px, 0, 0, w * k, h * k};
            LOG("[font] glyph cache texture %llx %ux%u -> %ux%u\n", (unsigned long long)addr, w, h, w * k, h * k);
            return &g_fc[i];
        }
    return NULL;
}

// For the renderer: K if the texture at addr (w x h) is a glyph cache drawn at K x, else 1.
int font_hires_scale(uint64_t addr, uint32_t w, uint32_t h) {
    int k = 1;
    pthread_mutex_lock(&g_fc_lock);
    for (int i = 0; i < NCACHE; i++)
        if (g_fc[i].addr == addr && g_fc[i].w == w && g_fc[i].h == h) k = (int)g_fc[i].k;
    pthread_mutex_unlock(&g_fc_lock);
    return k;
}

// The changed part of the K x image of that texture: true (and locked until font_hires_end) with *px, the image's
// row length and the rectangle (rows counted as stored); false: not a glyph cache.
bool font_hires_begin(uint64_t addr, const uint8_t **px, uint32_t *row, uint32_t *x0, uint32_t *y0, uint32_t *x1,
                      uint32_t *y1) {
    pthread_mutex_lock(&g_fc_lock);
    for (int i = 0; i < NCACHE; i++)
        if (g_fc[i].addr == addr) {
            FontCache *f = &g_fc[i];
            *px = f->px; *row = f->w * f->k;
            *x0 = f->x0; *y0 = f->y0; *x1 = f->x1; *y1 = f->y1;
            f->x0 = f->y0 = f->x1 = f->y1 = 0;
            return true;
        }
    pthread_mutex_unlock(&g_fc_lock);
    return false;
}
void font_hires_end(void) { pthread_mutex_unlock(&g_fc_lock); }

static void mark(FontCache *f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    if (f->x1 <= f->x0) { f->x0 = x0; f->y0 = y0; f->x1 = x1; f->y1 = y1; return; }
    if (x0 < f->x0) f->x0 = x0;
    if (y0 < f->y0) f->y0 = y0;
    if (x1 > f->x1) f->x1 = x1;
    if (y1 > f->y1) f->y1 = y1;
}

// ---------------------------------------------------------------- where the game is
static bool words_at(uint64_t lr, int from, const uint32_t *w, const uint32_t *mask, int n) {
    if (lr < MAIN_BASE + 64 || lr >= SDK_BASE) return false;
    for (int i = 0; i < n; i++)
        if ((((const u32u *)(uintptr_t)(lr + (int64_t)(from + i) * 4))[0] & mask[i]) != w[i]) return false;
    return true;
}
#define BL 0x94000000u, 0xFC000000u
// ReleasesGlyph right after a glyph went into the cache (UpdateTextureCache, Pikachu 1.0.0 +0x7dc):
//   ldr x0, [x19, #0x1e80] / bl (unmap) / ldr x0, [x19, #8] / mov x1, x20 / bl ReleasesGlyph / ldr x0, [x19, #8] / bl / mov w23, #0x1c
static bool site_written(uint64_t lr) {
    static const uint32_t w[] = {0xf94f4260, 0x94000000, 0xf9400660, 0xaa1403e1, 0x94000000, 0xf9400660, 0x94000000, 0x321e0bf7};
    static const uint32_t m[] = {~0u, 0xFC000000u, ~0u, ~0u, 0xFC000000u, ~0u, 0xFC000000u, ~0u};
    static uint64_t yes, no;
    if (lr == yes) return true;
    if (lr == no) return false;
    bool r = words_at(lr, -5, w, m, 8);
    if (r) yes = lr; else no = lr;
    return r;
}
// AcquireGlyphmap of a glyph's first (or only) map: fcvtzs w27, s0 / bl / mov x20, x0 / ldr x0, [x19, #8] / bl / ldrh w9, [x20, #0x1e]
static bool site_first_map(uint64_t lr) {
    static const uint32_t w[] = {0x1e38001b, 0x94000000, 0xaa0003f4, 0xf9400660, 0x94000000, 0x79403e89};
    static const uint32_t m[] = {~0u, 0xFC000000u, ~0u, ~0u, 0xFC000000u, ~0u};
    static uint64_t yes, no;
    if (lr == yes) return true;
    if (lr == no) return false;
    bool r = words_at(lr, -2, w, m, 6);
    if (r) yes = lr; else no = lr;
    return r;
}

// The glyph's first map as the game got it (an outline font's outline glyph is released before the cell is done).
static __thread struct { uint64_t node; int16_t lo_x, hi_y; uint16_t w, rows; } t_first;

// ---------------------------------------------------------------- the engine, called from here
static uint64_t sdk(Cpu *c, const char *sym, uint64_t *cache, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4;
    call_sdk(c, sym, cache);
    return c->x[0];
}

typedef struct { int16_t lo_x, hi_y; int w, rows; } MapInfo;
static MapInfo map_info(uint64_t g) {
    return (MapInfo){*(int16_t *)(uintptr_t)(g + 0xc), *(int16_t *)(uintptr_t)(g + 0xe), *(int16_t *)(uintptr_t)(g + 0x1c),
                     *(int16_t *)(uintptr_t)(g + 0x1e)};
}

// The glyph of that node again at K x into the K x image.
static void draw_hires(Cpu *c, uint64_t cache, uint64_t node, uint64_t tex, int k) {
    uint64_t eng = *(u64u *)(uintptr_t)(cache + 8);
    uint32_t W = *(u32u *)(uintptr_t)(cache + 0x28), H = *(u32u *)(uintptr_t)(cache + 0x2c);
    uint32_t code = *(u32u *)(uintptr_t)(node + 0x30);
    uint16_t fi = *(u16u *)(uintptr_t)(node + 0x34), size = *(u16u *)(uintptr_t)(node + 0x36);
    uint8_t kind = *(u8a *)(uintptr_t)(node + 0x3a);
    uint32_t cx = *(u16u *)(uintptr_t)(node + 0x40), ly = *(u16u *)(uintptr_t)(node + 0x42), cw = *(u16u *)(uintptr_t)(node + 0x44);
    uint64_t tbl = *(u64u *)(uintptr_t)(cache + 0x1bf0) + (uint64_t)fi * 0x1c;
    int asc = (int)(*(float *)(uintptr_t)(tbl + 8) * (float)size + 0.5f);
    bool outline = *(u8a *)(uintptr_t)(*(u64u *)(uintptr_t)(cache + 0x1c00) + fi) != 0;
    if (!W || !H || W > 8192 || H > 8192 || !size || t_first.node != node) return;
    // line height (the game's own function: cells reach that far down)
    c->x[0] = kind;
    {
        uint64_t lr = c->x[30];
        guest_call(c, *(u64u *)(uintptr_t)(cache + 0x1be8));
        c->x[30] = lr;
    }
    uint32_t lh = (uint32_t)c->x[0] & 0xffff;
    if (!lh || ly + lh > H) lh = H - ly;

    pthread_mutex_lock(&g_eng_lock);
    int es = eng_slot(eng);
    uint32_t flags = es >= 0 ? g_eng[es].flags : ~0u, ow = es >= 0 ? g_eng[es].outline : ~0u;
    pthread_mutex_unlock(&g_eng_lock);

    // the K x cell: the low one's columns cx..cx+cw+1 (glyph from cx+1), rows ly..ly+lh (top-down) - all of them, its
    // empty edge columns too: left as they were, a glyph that had used them before showed as thin vertical lines
    uint32_t KW = W * k, KH = H * k;
    int cl = (int)(cx * k), cr = (int)((cx + cw + 2) * k), ct = (int)(ly * k), cb = (int)((ly + lh) * k);
    if (cr > (int)KW) cr = (int)KW;
    if (cb > (int)KH) cb = (int)KH;
    int pen_x = (int)cx + 1 - t_first.lo_x, base_y = (int)ly + asc;  // the low glyph's pen origin, baseline (top-down)

    uint8_t *cell = calloc((size_t)(cr - cl > 0 ? cr - cl : 1) * (cb - ct > 0 ? cb - ct : 1), 1);
    if (!cell) return;
    int cellw = cr - cl;
    sdk(c, SYM_SET_SCALE, &a_scale, eng, (uint64_t)(size * k) << 16, 0, 0, (uint64_t)(size * k) << 16);
    if (outline && ow != ~0u) sdk(c, SYM_OUTLINE, &a_outline, eng, ow * k, 0, 0, 0);
    uint64_t g = sdk(c, SYM_ACQUIRE, &a_acquire, eng, code, 8, 0, 0);
    int ox = 0, oy = 0;  // where the first map's top-left lands in the cell
    bool ok = g != 0;
    if (ok) {
        MapInfo m = map_info(g);
        ox = pen_x * k + m.lo_x - cl;
        oy = base_y * k - m.hi_y - ct;
        const uint8_t *bits = (const uint8_t *)(uintptr_t)(g + 0x28);
        for (int i = 0; i < m.rows; i++) {
            int y = oy + i;
            if (y < 0 || y >= cb - ct) continue;
            for (int j = 0; j < m.w; j++) {
                int x = ox + j;
                if (x >= 0 && x < cellw) cell[(size_t)y * cellw + x] = bits[(size_t)i * m.w + j];
            }
        }
        if (log_left())
            LOG("[font] U+%04x size %u (x%d) font %u%s: low %dx%d lo_x %d hi_y %d at %u,%u (line %u, asc %d) | high %dx%d lo_x %d hi_y %d\n",
                code, size, k, fi, outline ? " outline" : "", cw, t_first.rows, t_first.lo_x, t_first.hi_y, cx, ly, lh, asc,
                m.w, m.rows, m.lo_x, m.hi_y);
        sdk(c, SYM_RELEASE, &a_release, eng, g, 0, 0, 0);
    }
    if (ok && outline) {  // the fill, subtracted at half strength where the outline glyph is (as the game does)
        sdk(c, SYM_FLAGS, &a_flags, eng, 1, 0, 0, 0);
        uint64_t f = sdk(c, SYM_ACQUIRE, &a_acquire, eng, code, 8, 0, 0);
        if (f) {
            MapInfo m = map_info(f);
            const uint8_t *bits = (const uint8_t *)(uintptr_t)(f + 0x28);
            for (int i = 0; i < m.rows; i++) {
                int y = oy + i;
                if (y < 0 || y >= cb - ct) continue;
                for (int j = 0; j < m.w; j++) {
                    int x = ox + j;
                    if (x >= 0 && x < cellw) cell[(size_t)y * cellw + x] -= bits[(size_t)i * m.w + j] >> 1;
                }
            }
            sdk(c, SYM_RELEASE, &a_release, eng, f, 0, 0, 0);
        }
        sdk(c, SYM_FLAGS, &a_flags, eng, flags != ~0u ? flags : 2, 0, 0, 0);
    }
    // the game's state back
    if (outline && ow != ~0u) sdk(c, SYM_OUTLINE, &a_outline, eng, ow, 0, 0, 0);
    sdk(c, SYM_SET_SCALE, &a_scale, eng, (uint64_t)size << 16, 0, 0, (uint64_t)size << 16);

    if (ok && cellw > 0 && cb > ct) {
        pthread_mutex_lock(&g_fc_lock);
        FontCache *fc = cache_for(tex, W, H, k);
        if (fc && fc->k == (uint32_t)k)
            for (int y = ct; y < cb; y++)  // (stored bottom-up)
                memcpy(fc->px + (size_t)(KH - 1 - y) * KW + cl, cell + (size_t)(y - ct) * cellw, (size_t)cellw);
        if (fc) mark(fc, (uint32_t)cl, KH - (uint32_t)cb, (uint32_t)cr, KH - (uint32_t)ct);
        pthread_mutex_unlock(&g_fc_lock);
    }
    free(cell);
}

// ---------------------------------------------------------------- the wrappers
HLE_FUNC(SYM_SET_SCALE, font_set_scale) { call_sdk(c, SYM_SET_SCALE, &a_scale); }

HLE_FUNC(SYM_FLAGS, font_set_flags) {
    pthread_mutex_lock(&g_eng_lock);
    int s = eng_slot(c->x[0]);
    if (s >= 0) g_eng[s].flags = (uint32_t)c->x[1];
    pthread_mutex_unlock(&g_eng_lock);
    call_sdk(c, SYM_FLAGS, &a_flags);
}

HLE_FUNC(SYM_OUTLINE, font_set_outline) {
    pthread_mutex_lock(&g_eng_lock);
    int s = eng_slot(c->x[0]);
    if (s >= 0) g_eng[s].outline = (uint32_t)c->x[1] & 0xffff;
    pthread_mutex_unlock(&g_eng_lock);
    call_sdk(c, SYM_OUTLINE, &a_outline);
}

HLE_FUNC(SYM_ACQUIRE, font_acquire) {
    uint64_t lr = c->x[30], node = c->x[26];
    call_sdk(c, SYM_ACQUIRE, &a_acquire);
    if (c->x[0] && site_first_map(lr)) {
        MapInfo m = map_info(c->x[0]);
        t_first.node = node; t_first.lo_x = m.lo_x; t_first.hi_y = m.hi_y; t_first.w = (uint16_t)m.w; t_first.rows = (uint16_t)m.rows;
    }
}

HLE_FUNC(SYM_RELEASE, font_release) {
    uint64_t lr = c->x[30];
    bool written = site_written(lr);
    uint64_t cache = c->x[19], node = c->x[26], tex = c->x[23];
    call_sdk(c, SYM_RELEASE, &a_release);
    if (!written) return;
    int k = font_k();
    if (k < 2) return;
    uint64_t keep[2] = {c->x[0], c->x[1]};
    draw_hires(c, cache, node, tex, k);
    c->x[0] = keep[0]; c->x[1] = keep[1];
    t_first.node = 0;
}
