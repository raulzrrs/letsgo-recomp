// The battle menu with a fifth button, CATCH (wild battles: runtime/hle/wildbattle.c, docs/MODO-BATALHA.md stage 6),
// made at run time from the player's own game files - tools/catch_button.py is the same in Python (the reference, and
// what the art was checked with). runtime/services/fs.c serves the results while one of our wild battles runs and the
// game's own files otherwise (trainer and two-player battles keep their four buttons).
//
// - bin/app/battle/bin/battle_command_00{,_eng,...}.arc (SARC): the BACK button's textures (its pattern frame 4: only
//   two-player battles show it) redrawn as CATCH, same size and format (timg/__Combined.bntx, Tegra block linear, BC3 /
//   BC4 / BC5); blyt/battle_command_button_catch.bflyt, a copy of the button's part layout showing those textures in red
//   (no pattern animation needed); in blyt/battle_command_00.bflyt N_command_04 / L_command_04 at RUN's place and the
//   four buttons one step (136) to the left (the game lays them out again from B_bounding_00: five end where four did);
// - layout_setting_battle_command_00.bin: the new panes and L_command_04's animations (L_command_03's but the pattern);
// - uikit_setting_battle_command_00.bin: a fifth button (RUN's, its hashes mapped) and its cell in the navigation grid
//   (column 4, the grid 5 columns wide).
// FlatBuffers are only appended to: new tables and vectors at the end, fields pointed at them (offsets point forward,
// so a vector that grows is copied whole). New hashes: FNV-1a 64 of the old one as "%016x:catch" (the game only compares).
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../../third_party/stb/stb_dxt.h"
#include "../../third_party/stb/stb_image.h"
#include "catch_button_art.h"

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 4096;
        b->p = realloc(b->p, b->cap);
    }
    if (d) memcpy(b->p + b->n, d, n);
    else memset(b->p + b->n, 0, n);
    b->n += n;
}
static void pad(Buf *b, size_t a, size_t mod) { while (b->n % a != mod) put(b, NULL, 1); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wrf(uint8_t *p, float v) { memcpy(p, &v, 4); }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

static uint64_t derive(uint64_t h) {
    char s[32];
    snprintf(s, sizeof s, "%016llx:catch", (unsigned long long)h);
    uint64_t r = 0xcbf29ce484222645ull;
    for (const char *c = s; *c; c++) r = (r ^ (uint8_t)*c) * 0x100000001b3ull;
    return r;
}

// ---------------------------------------------------------------- SARC
typedef struct { char name[128]; uint8_t *data; size_t size; bool own; } SFile;

static int sarc_read(const uint8_t *d, size_t n, SFile *out, int max, uint32_t *key) {
    if (n < 0x20 || memcmp(d, "SARC", 4)) return -1;
    uint32_t dataoff = rd32(d + 0xc);
    int cnt = rd16(d + 0x1a);
    *key = rd32(d + 0x1c);
    const uint8_t *names = d + 0x20 + 16 * cnt + 8;
    if (cnt > max) return -1;
    for (int i = 0; i < cnt; i++) {
        const uint8_t *e = d + 0x20 + 16 * i;
        uint32_t attr = rd32(e + 4), s = rd32(e + 8), t = rd32(e + 12);
        snprintf(out[i].name, sizeof out[i].name, "%s", (const char *)names + (attr & 0xffffff) * 4);
        out[i].data = (uint8_t *)d + dataoff + s, out[i].size = t - s, out[i].own = false;
    }
    return cnt;
}

static uint32_t sarc_hash(const char *s, uint32_t key) {
    uint32_t h = 0;
    for (; *s; s++) h = h * key + (uint8_t)*s;
    return h;
}

static int ent_cmp(const void *a, const void *b) {
    uint32_t x = ((const uint32_t *)a)[0], y = ((const uint32_t *)b)[0];
    return x < y ? -1 : x > y;
}

static void sarc_write(Buf *o, SFile *f, int n, uint32_t key) {
    Buf names = {0}, blob = {0};
    uint32_t (*ent)[4] = malloc(sizeof *ent * n);  // hash, name offset / 4, data start, end
    for (int i = 0; i < n; i++) {
        ent[i][0] = sarc_hash(f[i].name, key), ent[i][1] = (uint32_t)(names.n / 4);
        put(&names, f[i].name, strlen(f[i].name) + 1);
        pad(&names, 4, 0);
        size_t a = strstr(f[i].name, ".bntx") || strstr(f[i].name, ".bnsh") ? 0x1000 : 0x10;
        pad(&blob, a, 0);
        ent[i][2] = (uint32_t)blob.n;
        put(&blob, f[i].data, f[i].size);
        ent[i][3] = (uint32_t)blob.n;
    }
    qsort(ent, n, sizeof *ent, ent_cmp);
    size_t hdr = 0x14 + 0xc + 16 * (size_t)n + 8 + names.n, dataoff = (hdr + 0xfff) & ~(size_t)0xfff;
    uint8_t h[0x20] = "SARC";
    uint16_t v16[4] = {0x14, 0xfeff, 0, 0};
    memcpy(h + 4, v16, 4);
    wr32(h + 0xc, (uint32_t)dataoff);
    v16[0] = 0x100;
    memcpy(h + 0x10, v16, 4);
    memcpy(h + 0x14, "SFAT", 4);
    v16[0] = 0xc, v16[1] = (uint16_t)n;
    memcpy(h + 0x18, v16, 4);
    wr32(h + 0x1c, key);
    put(o, h, 0x20);
    for (int i = 0; i < n; i++) {
        uint32_t e[4] = {ent[i][0], 0x01000000u | ent[i][1], ent[i][2], ent[i][3]};
        put(o, e, 16);
    }
    uint8_t sfnt[8] = "SFNT";
    v16[0] = 8, v16[1] = 0;
    memcpy(sfnt + 4, v16, 4);
    put(o, sfnt, 8);
    put(o, names.p, names.n);
    put(o, NULL, dataoff - o->n);
    put(o, blob.p, blob.n);
    wr32(o->p + 8, (uint32_t)o->n);
    free(names.p), free(blob.p), free(ent);
}

// ---------------------------------------------------------------- textures
static size_t gob(size_t x, size_t y, size_t wg, size_t bh) {
    size_t off = (y / (8 * bh)) * 512 * bh * wg + (x / 64) * 512 * bh + ((y % (8 * bh)) / 8) * 512;
    size_t xi = x % 64, yi = y % 8;
    return off + (xi / 32) * 256 + (yi / 2) * 64 + ((xi % 32) / 16) * 32 + (yi % 2) * 16 + (xi % 16);
}

// BC1 with the transparent pixels (alpha < 128) as its 1-bit alpha (3 colours + transparent black, colour0 <= colour1):
// stb_dxt makes 4-colour blocks only; the PT-BR mod's label text_01 is BC1 with its background transparent
static void bc1_block(uint8_t *dst, const uint8_t *rgba) {
    bool holes = false;
    for (int i = 0; i < 16; i++) holes |= rgba[4 * i + 3] < 128;
    if (!holes) { stb_compress_dxt_block(dst, rgba, 0, STB_DXT_HIGHQUAL); return; }
    int lo = 255 * 3, hi = 0, ilo = -1, ihi = -1;  // (endpoints: the darkest and brightest opaque pixels)
    for (int i = 0; i < 16; i++) {
        if (rgba[4 * i + 3] < 128) continue;
        int l = rgba[4 * i] + rgba[4 * i + 1] + rgba[4 * i + 2];
        if (l < lo) lo = l, ilo = i;
        if (l >= hi) hi = l, ihi = i;
    }
    uint16_t c[2] = {0, 0};
    float pal[3][3] = {{0}};
    for (int e = 0; e < 2 && ilo >= 0; e++) {
        const uint8_t *px = rgba + 4 * (e ? ihi : ilo);
        c[e] = (uint16_t)(((px[0] >> 3) << 11) | ((px[1] >> 2) << 5) | (px[2] >> 3));
        pal[e][0] = (c[e] >> 11) * 255.0f / 31, pal[e][1] = ((c[e] >> 5) & 63) * 255.0f / 63, pal[e][2] = (c[e] & 31) * 255.0f / 31;
    }
    for (int k = 0; k < 3; k++) pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
    uint32_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 3;
        if (rgba[4 * i + 3] >= 128) {
            float bd = 1e9f;
            for (int j = 0; j < 3; j++) {
                float d = 0;
                for (int k = 0; k < 3; k++) d += (pal[j][k] - rgba[4 * i + k]) * (pal[j][k] - rgba[4 * i + k]);
                if (d < bd) bd = d, best = j;
            }
        }
        bits |= (uint32_t)best << (2 * i);
    }
    memcpy(dst, &c[0], 2), memcpy(dst + 2, &c[1], 2), memcpy(dst + 4, &bits, 4);  // (c0 <= c1: the darkest first)
}

// a texture of the BNTX at d replaced by an image (w x h RGBA), same size, in the texture's format: BC1 / BC3 (RGB(A)),
// BC4 (R), BC5 (R, G) - the game's are BC3 / BC4 / BC5; a mod may have made them again in others (the PT-BR
// translation: the labels in BC3 with the alpha the shadow's, and BC1)
static bool bntx_replace(uint8_t *d, size_t n, const char *name, const uint8_t *rgba, int w, int h) {
    if (n < 0x40 || memcmp(d, "BNTX", 4)) return false;
    uint32_t count = rd32(d + 0x24);
    uint64_t info = rd64(d + 0x28);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t p = rd64(d + info + 8 * i);
        uint64_t name_off = rd64(d + p + 0x60);
        uint16_t nl = rd16(d + name_off);
        size_t len = strlen(name);
        if (nl < len || memcmp(d + name_off + 2, name, len) || (nl > len && d[name_off + 2 + len] != '^' && d[name_off + 2 + len] != '+'))
            continue;
        uint32_t fmt = rd32(d + p + 0x1c) >> 8, tw = rd32(d + p + 0x24), th = rd32(d + p + 0x28), blh = rd32(d + p + 0x34);
        uint32_t size = rd32(d + p + 0x50);
        uint64_t data = rd64(d + rd64(d + p + 0x70));
        if ((int)tw != w || (int)th != h || data + size > n || (fmt != 0x1a && (fmt < 0x1c || fmt > 0x1e))) return false;
        size_t bpe = fmt == 0x1a || fmt == 0x1d ? 8 : 16;
        uint32_t mips = rd16(d + p + 0x16);
        uint64_t ptrs = rd64(d + p + 0x70);
        memset(d + data, 0, size);
        // every mip level (a mod's labels have 8: drawn small, the game reads the lower ones), each a box filter of the
        // one above, its own block height (Tegra: halved while the level's height in blocks fits in half of it)
        uint8_t *img = malloc((size_t)w * h * 4);
        memcpy(img, rgba, (size_t)w * h * 4);
        int mw = w, mh = h;
        for (uint32_t m = 0; m < (mips ? mips : 1); m++) {
            if (m) {
                int nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
                uint8_t *half = malloc((size_t)nw * nh * 4);
                for (int y = 0; y < nh; y++)
                    for (int x = 0; x < nw; x++)
                        for (int k = 0; k < 4; k++) {
                            int x1 = x * 2 + 1 < mw ? x * 2 + 1 : x * 2, y1 = y * 2 + 1 < mh ? y * 2 + 1 : y * 2;
                            unsigned v = img[((y * 2) * mw + x * 2) * 4 + k] + img[((y * 2) * mw + x1) * 4 + k] +
                                         img[(y1 * mw + x * 2) * 4 + k] + img[(y1 * mw + x1) * 4 + k];
                            half[(y * nw + x) * 4 + k] = (uint8_t)((v + 2) / 4);
                        }
                free(img);
                img = half, mw = nw, mh = nh;
            }
            uint64_t at = m ? rd64(d + ptrs + 8 * m) : data, end = m + 1 < mips ? rd64(d + ptrs + 8 * (m + 1)) : data + size;
            if (at < data || end > data + size || at >= end) break;
            size_t bw = (mw + 3) / 4, bhh = (mh + 3) / 4;
            uint8_t *lin = malloc(bw * bhh * bpe);
            for (size_t by = 0; by < bhh; by++)
                for (size_t bx = 0; bx < bw; bx++) {
                    uint8_t blk[64], r[16], rg[32];
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) {
                            size_t sx = bx * 4 + x < (size_t)mw ? bx * 4 + x : (size_t)mw - 1, sy = by * 4 + y < (size_t)mh ? by * 4 + y : (size_t)mh - 1;
                            const uint8_t *px = img + (sy * mw + sx) * 4;
                            memcpy(blk + (y * 4 + x) * 4, px, 4);
                            r[y * 4 + x] = px[0], rg[(y * 4 + x) * 2] = px[0], rg[(y * 4 + x) * 2 + 1] = px[1];
                        }
                    uint8_t *dst = lin + (by * bw + bx) * bpe;
                    if (fmt == 0x1d) stb_compress_bc4_block(dst, r);
                    else if (fmt == 0x1e) stb_compress_bc5_block(dst, rg);
                    else if (fmt == 0x1a) bc1_block(dst, blk);
                    else stb_compress_dxt_block(dst, blk, 1, STB_DXT_HIGHQUAL);
                }
            size_t wg = (bw * bpe + 63) / 64, bh = (size_t)1 << blh;
            while (bh > 1 && bhh <= bh * 8 / 2) bh /= 2;
            for (size_t y = 0; y < bhh; y++)
                for (size_t x = 0; x < bw * bpe; x++) {
                    size_t o = gob(x, y, wg, bh);
                    if (at + o < end) d[at + o] = lin[y * bw * bpe + x];
                }
            free(lin);
        }
        free(img);
        return true;
    }
    return false;
}

static uint8_t *art(const char *name, int *w, int *h, int want) {
    for (size_t i = 0; i < sizeof k_catch_art / sizeof *k_catch_art; i++)
        if (!strcmp(k_catch_art[i].name, name)) {
            int c;
            return stbi_load_from_memory(k_catch_art[i].png, (int)k_catch_art[i].size, w, h, &c, want);
        }
    return NULL;
}

// a grey image (w x h) as RGBA: v, v, v and v (the text's alpha: a mod's BC1 / BC3 keep it there) - or, for the label's
// shadow (text_00), 255 - v (the PT-BR mod's BC3 has it so); the game's own BC4 / BC5 take only the grey
static uint8_t *grey_rgba(const uint8_t *g, int w, int h, bool shadow) {
    uint8_t *o = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) o[4 * i] = o[4 * i + 1] = o[4 * i + 2] = g[i], o[4 * i + 3] = shadow ? 255 - g[i] : g[i];
    return o;
}

static bool redraw(uint8_t *bntx, size_t n, const char *lang) {
    static const struct { const char *art, *texture; int kind; } parts[] = {  // kind: 0 RGBA, 1 grey, 2 the shadow
        {"off", "battle_command_back_off_00", 0},
        {"icon", "battle_button_command_icon_08", 1},
        {"text_00_%s", "grpfont_battle_backButton_text_00", 2},
        {"text_01_%s", "grpfont_battle_backButton_text_01", 1},
    };
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, parts[i].art, lang);
        int w, h;
        uint8_t *img = art(nm, &w, &h, parts[i].kind ? 1 : 4);
        if (!img) return false;
        uint8_t *rgba = parts[i].kind ? grey_rgba(img, w, h, parts[i].kind == 2) : img;
        bool ok = bntx_replace(bntx, n, parts[i].texture, rgba, w, h);
        if (rgba != img) free(rgba);
        free(img);
        if (!ok) return false;
    }
    return true;
}

// ---------------------------------------------------------------- BFLYT
typedef struct { const uint8_t *p; uint32_t size; } Sec;

static int lyt_sections(const uint8_t *d, size_t n, Sec *s, int max) {
    if (n < 0x14 || memcmp(d, "FLYT", 4)) return -1;
    uint16_t hsz = rd16(d + 6), cnt = rd16(d + 0x10);
    size_t o = hsz;
    if (cnt > max) return -1;
    for (int i = 0; i < cnt; i++) {
        if (o + 8 > n) return -1;
        s[i].p = d + o, s[i].size = rd32(d + o + 4);
        o += s[i].size;
    }
    return cnt;
}

static void lyt_join(Buf *o, const uint8_t *head, Sec *s, int n) {
    uint16_t hsz = rd16(head + 6);
    put(o, head, hsz);
    for (int i = 0; i < n; i++) put(o, s[i].p, s[i].size);
    wr32(o->p + 0xc, (uint32_t)o->n);
    uint16_t c = (uint16_t)n;
    memcpy(o->p + 0x10, &c, 2);
}

static bool is_pane(const Sec *s, const char *name) {
    static const char *const kinds[] = {"pan1", "prt1", "pic1", "bnd1"};
    for (int k = 0; k < 4; k++)
        if (!memcmp(s->p, kinds[k], 4)) return !strncmp((const char *)s->p + 0xc, name, 0x18);
    return false;
}

// the button's part layout -> the CATCH copy: the BACK textures by default, red colours
static bool part_layout(const uint8_t *d, size_t n, Buf *o) {
    Sec s[96];
    int cnt = lyt_sections(d, n, s, 96);
    if (cnt < 0) return false;
    static const char *const swap[][2] = {
        {"battle_command_battle_off_00", "battle_command_back_off_00"},
        {"battle_button_command_icon_00", "battle_button_command_icon_08"},
        {"grpfont_battle_battleButton_text_00", "grpfont_battle_backButton_text_00"},
        {"grpfont_battle_battleButton_text_01", "grpfont_battle_backButton_text_01"},
    };
    static const struct { const char *mat; int black[4], white[4]; } colours[] = {  // (-1: as it is; FIGHT's are pink)
    // (the label's black keeps alpha 0, as the game's: a mod's BC1 / BC3 labels have alpha, and their background takes it)
        {"N_glare_00", {-1}, {255, 64, 64, 255}},       {"P_buttonBG_00", {-1}, {232, 24, 40, 255}},
        {"P_buttonBG_01", {-1}, {255, 255, 255, 255}},  {"P_buttonBG_02", {-1}, {255, 112, 112, 255}},
        {"P_buttonBG_03", {-1}, {200, 0, 24, 255}},     {"P_grptext_00", {150, 0, 16, 0}, {-1}},
    };
    Buf txl = {0}, mat = {0};
    for (int i = 0; i < cnt; i++) {
        if (!memcmp(s[i].p, "txl1", 4)) {
            uint16_t k = rd16(s[i].p + 8);
            Buf body = {0};
            uint8_t head[12] = "txl1";
            memcpy(head + 8, &k, 2);
            put(&txl, head, 12);
            put(&txl, NULL, 4 * (size_t)k);
            for (int j = 0; j < k; j++) {
                const char *nm = (const char *)s[i].p + 0xc + rd32(s[i].p + 0xc + 4 * j);
                char out[96];
                snprintf(out, sizeof out, "%s", nm);
                for (size_t q = 0; q < sizeof swap / sizeof *swap; q++) {
                    size_t l = strlen(swap[q][0]);
                    if (!strncmp(nm, swap[q][0], l) && (nm[l] == '^' || !nm[l])) snprintf(out, sizeof out, "%s%s", swap[q][1], nm + l);
                }
                wr32(txl.p + 0xc + 4 * j, (uint32_t)(4 * k + body.n));
                put(&body, out, strlen(out) + 1);
            }
            put(&txl, body.p, body.n);
            pad(&txl, 4, 0);
            wr32(txl.p + 4, (uint32_t)txl.n);
            free(body.p);
            s[i].p = txl.p, s[i].size = (uint32_t)txl.n;
        } else if (!memcmp(s[i].p, "mat1", 4)) {
            put(&mat, s[i].p, s[i].size);
            for (size_t q = 0; q < sizeof colours / sizeof *colours; q++) {
                size_t l = strlen(colours[q].mat) + 1;
                uint8_t *at = NULL;
                for (size_t o2 = 0; o2 + l <= mat.n && !at; o2++)
                    if (!memcmp(mat.p + o2, colours[q].mat, l)) at = mat.p + o2;
                if (!at) { free(txl.p), free(mat.p); return false; }
                for (int c = 0; c < 4; c++) {
                    if (colours[q].black[0] >= 0) at[0x24 + c] = (uint8_t)colours[q].black[c];
                    if (colours[q].white[0] >= 0) at[0x28 + c] = (uint8_t)colours[q].white[c];
                }
            }
            s[i].p = mat.p, s[i].size = (uint32_t)mat.n;
        }
    }
    lyt_join(o, d, s, cnt);
    free(txl.p), free(mat.p);
    return true;
}

// the menu's layout: N_command_04 / L_command_04 after N_command_03, the buttons and their area one step left
static bool menu_layout(const uint8_t *d, size_t n, Buf *o) {
    Sec s[128];
    int cnt = lyt_sections(d, n, s, 124);
    if (cnt < 0) return false;
    int i3 = -1, ib = -1, ic[4] = {-1, -1, -1, -1};
    for (int i = 0; i < cnt; i++) {
        char nm[16];
        for (int k = 0; k < 4; k++) {
            snprintf(nm, sizeof nm, "N_command_%02d", k);
            if (is_pane(&s[i], nm)) ic[k] = i;
        }
        if (is_pane(&s[i], "B_bounding_00") && !memcmp(s[i].p, "bnd1", 4)) ib = i;
    }
    i3 = ic[3];
    if (i3 < 0 || ib < 0 || ic[0] < 0 || i3 + 4 > cnt || memcmp(s[i3 + 1].p, "pas1", 4) || memcmp(s[i3 + 2].p, "prt1", 4) ||
        memcmp(s[i3 + 3].p, "pae1", 4) || s[i3 + 2].size < 0x7c || strcmp((const char *)s[i3 + 2].p + 0x60, "battle_command_button_00"))
        return false;
    const float step = 136.0f;
    uint8_t *mod[4];  // the four N_command
    for (int k = 0; k < 4; k++) {
        mod[k] = malloc(s[ic[k]].size);
        memcpy(mod[k], s[ic[k]].p, s[ic[k]].size);
    }
    uint8_t *n4 = malloc(s[i3].size), *l4 = malloc(s[i3 + 2].size);
    memcpy(n4, s[i3].p, s[i3].size);  // (at N_command_03's place: RUN's)
    memcpy(l4, s[i3 + 2].p, s[i3 + 2].size);
    memset(n4 + 0xc, 0, 0x18), memcpy(n4 + 0xc, "N_command_04", 12);
    memset(l4 + 0xc, 0, 0x18), memcpy(l4 + 0xc, "L_command_04", 12);
    memset(l4 + 0x60, 0, 0x1c), memcpy(l4 + 0x60, "battle_command_button_catch", 27);
    for (int k = 0; k < 4; k++) {
        wrf(mod[k] + 0x2c, rdf(mod[k] + 0x2c) - step);
        s[ic[k]].p = mod[k];
    }
    // (the game lays the buttons out from B_bounding_00's right edge - RUN at the edge - 66, the others every 136 - and
    // with five it starts one step further left by itself: the area stays, so CATCH ends where RUN did)
    memmove(s + i3 + 8, s + i3 + 4, sizeof *s * (cnt - i3 - 4));
    s[i3 + 4] = (Sec){n4, s[i3].size};
    s[i3 + 5] = s[i3 + 1];
    s[i3 + 6] = (Sec){l4, s[i3 + 2].size};
    s[i3 + 7] = s[i3 + 3];
    lyt_join(o, d, s, cnt + 4);
    for (int k = 0; k < 4; k++) free(mod[k]);
    free(n4), free(l4);
    return true;
}

// ---------------------------------------------------------------- FlatBuffers (append only)
typedef struct { Buf b; } FB;
enum { RAW, STR, STRS };
typedef struct { int field; int kind; uint8_t raw[8]; int width; char *str; char **strs; int nstrs; } FVal;
typedef struct { uint32_t like; FVal v[16]; int n; } FTable;

static uint32_t fb_u32(FB *f, size_t o) { return rd32(f->b.p + o); }
static size_t fb_ref(FB *f, size_t a) { return a + fb_u32(f, a); }
static size_t fb_vt(FB *f, size_t t) { int32_t s; memcpy(&s, f->b.p + t, 4); return (size_t)((int64_t)t - s); }
static int fb_fields(FB *f, size_t t, size_t *at, int max) {  // at[i] = address of field i, 0 if absent
    size_t vt = fb_vt(f, t);
    int n = (rd16(f->b.p + vt) - 4) / 2;
    for (int i = 0; i < max; i++) at[i] = i < n && rd16(f->b.p + vt + 4 + 2 * i) ? t + rd16(f->b.p + vt + 4 + 2 * i) : 0;
    return n;
}
static int fb_vec(FB *f, size_t a, size_t *out, int max) {
    size_t t = fb_ref(f, a);
    int n = (int)fb_u32(f, t);
    for (int i = 0; i < n && i < max; i++) out[i] = t + 4 + 4 * i + fb_u32(f, t + 4 + 4 * i);
    return n;
}
static char *fb_string(FB *f, size_t a) {
    size_t t = fb_ref(f, a);
    uint32_t n = fb_u32(f, t);
    char *s = malloc(n + 1);
    memcpy(s, f->b.p + t + 4, n);
    s[n] = 0;
    return s;
}

// table t's values: kinds[field] STR / STRS, others raw (the bytes up to the next field, 8 / 4 / less)
static void fb_read(FB *f, size_t t, const int *kinds, FTable *out) {
    size_t at[16];
    fb_fields(f, t, at, 16);
    size_t size = rd16(f->b.p + fb_vt(f, t) + 2);
    out->like = (uint32_t)t, out->n = 0;
    for (int i = 0; i < 16; i++) {
        if (!at[i]) continue;
        FVal *v = &out->v[out->n++];
        memset(v, 0, sizeof *v);
        v->field = i, v->kind = kinds ? kinds[i] : RAW;
        if (v->kind == STR) v->str = fb_string(f, at[i]);
        else if (v->kind == STRS) {
            size_t e[16];
            v->nstrs = fb_vec(f, at[i], e, 16);
            v->strs = malloc(sizeof *v->strs * (v->nstrs ? v->nstrs : 1));
            for (int k = 0; k < v->nstrs; k++) {
                uint32_t n = fb_u32(f, e[k]);
                v->strs[k] = malloc(n + 1);
                memcpy(v->strs[k], f->b.p + e[k] + 4, n);
                v->strs[k][n] = 0;
            }
        } else {
            size_t end = t + size;
            for (int j = 0; j < 16; j++) if (at[j] > at[i] && at[j] < end) end = at[j];
            v->width = end - at[i] >= 8 ? 8 : end - at[i] >= 4 ? 4 : (int)(end - at[i]);
            memcpy(v->raw, f->b.p + at[i], v->width);
        }
    }
}
static void fb_free(FTable *t) {
    for (int i = 0; i < t->n; i++) {
        free(t->v[i].str);
        for (int k = 0; k < t->v[i].nstrs; k++) free(t->v[i].strs[k]);
        free(t->v[i].strs);
    }
}
static FVal *fb_val(FTable *t, int field) {
    for (int i = 0; i < t->n; i++) if (t->v[i].field == field) return &t->v[i];
    return NULL;
}

static void fb_set_off(FB *f, size_t slot, size_t target) { wr32(f->b.p + slot, (uint32_t)(target - slot)); }
static size_t fb_put_string(FB *f, const char *s) {
    pad(&f->b, 4, 0);
    size_t at = f->b.n;
    uint32_t n = (uint32_t)strlen(s);
    put(&f->b, &n, 4);
    put(&f->b, s, n + 1);
    return at;
}
static size_t fb_put_vector(FB *f, int n) {
    pad(&f->b, 4, 0);
    size_t at = f->b.n;
    uint32_t c = (uint32_t)n;
    put(&f->b, &c, 4);
    put(&f->b, NULL, 4 * (size_t)n);
    return at;
}
static size_t fb_put_table(FB *f, FTable *t) {
    size_t vt = fb_vt(f, t->like), size = rd16(f->b.p + vt + 2);
    pad(&f->b, 8, t->like % 8);
    size_t at = f->b.n;
    put(&f->b, NULL, size);
    int32_t so = (int32_t)((int64_t)at - (int64_t)vt);
    memcpy(f->b.p + at, &so, 4);
    size_t fa[16];
    fb_fields(f, at, fa, 16);
    for (int i = 0; i < t->n; i++)
        if (t->v[i].kind == RAW && fa[t->v[i].field]) memcpy(f->b.p + fa[t->v[i].field], t->v[i].raw, t->v[i].width);
    for (int i = 0; i < t->n; i++) {
        FVal *v = &t->v[i];
        if (v->kind == STR) {
            size_t s = fb_put_string(f, v->str);
            fb_set_off(f, fa[v->field], s);
        } else if (v->kind == STRS) {
            size_t vec = fb_put_vector(f, v->nstrs);
            for (int k = 0; k < v->nstrs; k++) {
                size_t s = fb_put_string(f, v->strs[k]);
                fb_set_off(f, vec + 4 + 4 * k, s);
            }
            fb_set_off(f, fa[v->field], vec);
        }
    }
    return at;
}
static void fb_regrow(FB *f, size_t slot, FTable *t, int n) {
    size_t vec = fb_put_vector(f, n);
    for (int i = 0; i < n; i++) {
        size_t at = fb_put_table(f, &t[i]);
        fb_set_off(f, vec + 4 + 4 * i, at);
    }
    fb_set_off(f, slot, vec);
}

static void raw64(FVal *v, uint64_t x) { memcpy(v->raw, &x, 8), v->width = 8; }
static uint64_t get64(FVal *v) { uint64_t x = 0; memcpy(&x, v->raw, v->width < 8 ? v->width : 8); return x; }
static void set_strs(FVal *v, const char *s) {
    for (int k = 0; k < v->nstrs; k++) free(v->strs[k]);
    free(v->strs);
    v->strs = malloc(sizeof *v->strs), v->strs[0] = strdup(s), v->nstrs = 1;
}
// glibc declares clone() (sched.h, _GNU_SOURCE): this file's own static one has another name
#define clone catch_button_clone
static FTable clone(FTable *t) {  // (deep: the strings are its own)
    FTable c = *t;
    for (int i = 0; i < c.n; i++) {
        if (c.v[i].str) c.v[i].str = strdup(c.v[i].str);
        if (c.v[i].strs) {
            char **s = malloc(sizeof *s * (c.v[i].nstrs ? c.v[i].nstrs : 1));
            for (int k = 0; k < c.v[i].nstrs; k++) s[k] = strdup(c.v[i].strs[k]);
            c.v[i].strs = s;
        }
    }
    return c;
}

static bool layout_setting(const uint8_t *d, size_t n, Buf *o) {
    FB f = {{0}};
    put(&f.b, d, n);
    size_t root = fb_u32(&f, 0), at[8];
    fb_fields(&f, root, at, 8);
    if (!at[2] || !at[3]) return free(f.b.p), false;
    static const int ak[16] = {[1] = STRS, [2] = STR}, pk[16] = {[1] = STR, [2] = STRS};
    size_t e[256];
    int na = fb_vec(&f, at[2], e, 256), extra = 0;
    FTable *anims = calloc(na + 16, sizeof *anims);
    for (int i = 0; i < na && na <= 240; i++) fb_read(&f, e[i], ak, &anims[i]);
    for (int i = 0; i < na; i++) {
        FVal *p = fb_val(&anims[i], 1), *file = fb_val(&anims[i], 2), *h = fb_val(&anims[i], 3);
        if (!p || !file || !h || p->nstrs != 1 || strcmp(p->strs[0], "L_command_03") || strstr(file->str, "patern")) continue;
        FTable c = clone(&anims[i]);
        set_strs(fb_val(&c, 1), "L_command_04");
        raw64(fb_val(&c, 3), derive(get64(h)));
        anims[na + extra++] = c;
    }
    int np = fb_vec(&f, at[3], e, 256), extra_p = 0;
    FTable *panes = calloc(np + 32, sizeof *panes);
    for (int i = 0; i < np && np <= 240; i++) fb_read(&f, e[i], pk, &panes[i]);
    for (int i = 0; i < np; i++) {
        FVal *name = fb_val(&panes[i], 1), *path = fb_val(&panes[i], 2), *h = fb_val(&panes[i], 3);
        if (!name || !h) continue;
        bool own = !strcmp(name->str, "N_command_03") || !strcmp(name->str, "L_command_03");
        bool inside = path && path->nstrs == 1 && !strcmp(path->strs[0], "L_command_03");
        if (!own && !inside) continue;
        FTable c = clone(&panes[i]);
        if (own) {
            FVal *nm = fb_val(&c, 1);
            nm->str[strlen(nm->str) - 1] = '4';
        } else set_strs(fb_val(&c, 2), "L_command_04");
        raw64(fb_val(&c, 3), derive(get64(h)));
        panes[np + extra_p++] = c;
    }
    bool ok = extra == 6 && extra_p == 20 && na <= 240 && np <= 240;
    if (ok) {
        fb_regrow(&f, at[2], anims, na + extra);
        fb_regrow(&f, at[3], panes, np + extra_p);
        put(o, f.b.p, f.b.n);
    }
    for (int i = 0; i < na + extra; i++) fb_free(&anims[i]);
    for (int i = 0; i < np + extra_p; i++) fb_free(&panes[i]);
    free(anims), free(panes), free(f.b.p);
    return ok;
}

uint64_t g_catch_button_hash;  // (the fifth button's, for runtime/hle/wildbattle.c to find it)
static bool g_archive_failed;  // an archive couldn't be made: the settings (which name the new panes) stay the game's
bool catch_button_failed(void) { return g_archive_failed; }  // (runtime/services/fs.c then serves none of them)
void catch_button_begin(void) { g_archive_failed = false; }  // (each RomFS index: a new boot, maybe other mods)

static bool uikit_setting(const uint8_t *d, size_t n, Buf *o) {
    FB f = {{0}};
    put(&f.b, d, n);
    size_t root = fb_u32(&f, 0), at[16];
    fb_fields(&f, root, at, 16);
    if (!at[8] || !at[9]) return free(f.b.p), false;
    size_t e[16], nav[2], navf[8], cells[16];
    FTable buttons[6], cell[6];
    int nb = fb_vec(&f, at[8], e, 16);
    if (nb != 4 || fb_vec(&f, at[9], nav, 2) < 1) return free(f.b.p), false;
    for (int i = 0; i < 4; i++) fb_read(&f, e[i], NULL, &buttons[i]);
    fb_fields(&f, nav[0], navf, 8);
    int nc = navf[4] && navf[1] ? fb_vec(&f, navf[4], cells, 16) : 0;
    if (nc != 4 || fb_u32(&f, navf[1]) != 4) return free(f.b.p), false;
    for (int i = 0; i < 4; i++) fb_read(&f, cells[i], NULL, &cell[i]);
    buttons[4] = buttons[3];  // CATCH: RUN's with every hash mapped
    for (int i = 0; i < buttons[4].n; i++) raw64(&buttons[4].v[i], derive(get64(&buttons[4].v[i])));
    cell[4] = cell[3];
    FVal *c0 = fb_val(&cell[4], 0), *c1 = fb_val(&cell[4], 1);
    if (!c0 || !c1 || get64(c0) != get64(fb_val(&buttons[3], 0))) return free(f.b.p), false;
    raw64(c0, get64(fb_val(&buttons[4], 0)));
    g_catch_button_hash = get64(fb_val(&buttons[4], 0));
    uint32_t col = 4;
    memcpy(c1->raw, &col, 4);
    wr32(f.b.p + navf[1], 5);  // (the grid's columns)
    fb_regrow(&f, at[8], buttons, 5);
    fb_regrow(&f, navf[4], cell, 5);
    put(o, f.b.p, f.b.n);
    free(f.b.p);
    return true;
}

// ---------------------------------------------------------------- the archives
// a mod's folder that translates the game into Portuguese (its labels: LUTAR, MOCHILA, CORRER...): CAPTURAR, the Spanish
// archive's word too
static bool portuguese_mod(const char *host) {
    char l[1200];
    size_t i = 0;
    for (; host && host[i] && i + 1 < sizeof l; i++) l[i] = (char)(host[i] >= 'A' && host[i] <= 'Z' ? host[i] + 32 : host[i]);
    l[i] = 0;
    return strstr(l, "pt-br") || strstr(l, "ptbr") || strstr(l, "pt_br") || strstr(l, "traduc") || strstr(l, "portug");
}

static bool archive(const char *path, const char *mod, const uint8_t *d, size_t n, Buf *o) {
    static const struct { const char *suffix, *lang; } langs[] = {
        {"_eng", "eng"}, {"_fre", "fre"}, {"_ger", "ger"}, {"_ita", "ita"}, {"_spa", "spa"},
        {"_kor", "kor"}, {"_sch", "sch"}, {"_tch", "tch"},
    };
    const char *lang = "jpn";
    for (size_t i = 0; i < sizeof langs / sizeof *langs; i++)
        if (strstr(path, langs[i].suffix)) lang = langs[i].lang;
    if (mod && portuguese_mod(mod)) lang = "spa";
    SFile f[96];
    uint32_t key;
    int cnt = sarc_read(d, n, f, 95, &key);
    if (cnt < 0) return false;
    int ib = -1, im = -1, ip = -1;
    for (int i = 0; i < cnt; i++) {
        if (!strcmp(f[i].name, "timg/__Combined.bntx")) ib = i;
        if (!strcmp(f[i].name, "blyt/battle_command_00.bflyt")) im = i;
        if (!strcmp(f[i].name, "blyt/battle_command_button_00.bflyt")) ip = i;
    }
    if (ib < 0 || im < 0 || ip < 0) return false;
    uint8_t *bntx = malloc(f[ib].size);
    memcpy(bntx, f[ib].data, f[ib].size);
    Buf menu = {0}, part = {0};
    bool ok = redraw(bntx, f[ib].size, lang) && menu_layout(f[im].data, f[im].size, &menu) && part_layout(f[ip].data, f[ip].size, &part);
    if (ok) {
        f[ib].data = bntx;
        f[im].data = menu.p, f[im].size = menu.n;
        snprintf(f[cnt].name, sizeof f[cnt].name, "blyt/battle_command_button_catch.bflyt");
        f[cnt].data = part.p, f[cnt].size = part.n;
        sarc_write(o, f, cnt + 1, key);
    }
    free(bntx), free(menu.p), free(part.p);
    return ok;
}

// path: a RomFS path; d / n: the file as the game has it (mod: the mod's file it came from, or NULL). True with *out / *outn (malloc'd) when it's one of the
// menu's files and the CATCH version could be made.
bool catch_button_file(const char *path, const char *mod, const uint8_t *d, size_t n, uint8_t **out, size_t *outn) {
    static const char dir[] = "bin/app/battle/bin/";
    if (strncmp(path, dir, sizeof dir - 1)) return false;
    const char *name = path + sizeof dir - 1;
    Buf o = {0};
    bool ok = false;
    // (the archives come first in the RomFS' order - "battle_command" < "layout" < "uikit"; after a failure
    // runtime/services/fs.c drops them all: catch_button_failed)
    if (!strncmp(name, "battle_command_00", 17) && strstr(name, ".arc")) g_archive_failed |= !(ok = archive(path, mod, d, n, &o));
    else if (!strcmp(name, "layout_setting_battle_command_00.bin")) ok = !g_archive_failed && layout_setting(d, n, &o);
    else if (!strcmp(name, "uikit_setting_battle_command_00.bin")) ok = !g_archive_failed && uikit_setting(d, n, &o);
    else return false;
    if (!ok) {
        LOG("[wild] %s: not the expected layout, the CATCH button left out\n", path);
        free(o.p);
        return false;
    }
    *out = o.p, *outn = o.n;
    const char *dump = getenv("LARECOMP_WILDBATTLE_CATCH_DUMP");  // (checks: the files as made, in that folder)
    if (dump) {
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dump, name);
        FILE *f = fopen(p, "wb");
        if (f) fwrite(o.p, 1, o.n, f), fclose(f);
    }
    return true;
}
