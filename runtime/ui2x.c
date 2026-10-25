// The interface textures at 2x (runtime/ui2x.h). Made on the start screen from the player's own files, in steps:
//   1. the upscaler: waifu2x-ncnn-vulkan (nihui, MIT), a fixed release downloaded from GitHub with curl and checked
//      by its SHA-256, unpacked with Windows' tar into cache/waifu2x/ (elsewhere: put it there by hand);
//   2. extracted/romfs.bin: every SARC archive's BNTX textures (tools/ui_texkeys.py does the same in Python), one per
//      content key, decoded (bcdec: BC1-BC5, BC7; RGB5A1, RGBA8, R8, RG8) to PNG in cache/ui2x_work/in (transparent
//      texels take their neighbours' colour first: no dark fringes once filtered); an image with transparency as two
//      RGB images, its colour and its alpha as grey (waifu2x 20250915 loses the alpha of an RGBA image outside its
//      last tiles: 25/32 of a 512 x 288 one came out transparent);
//   3. waifu2x (CUnet, 2x, light denoising: the BC blocks) into cache/ui2x_work/out, the alpha as an image of its own;
//   4. each compressed again (stb_dxt: BC1 when opaque, BC3, BC4 / BC5 for one / two channels) into cache/ui2x.pak:
//      "UI2X", version, count, then per texture {key, original size, format, size, offset, bytes}, then the blocks.
// The key: CRC-32 and Adler-32 of level 0's block rows untiled - the same bytes in the BNTX file and, untiled again,
// in the texture the game uploads (textures.c).
#include "ui2x.h"

#include <SDL3/SDL_timer.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hle.h"
#include "platform.h"
#include "settings.h"

#define BCDEC_IMPLEMENTATION
#include "../third_party/bcdec/bcdec.h"
#define STB_DXT_IMPLEMENTATION
#include "../third_party/stb/stb_dxt.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WARNINGS
#include "../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

#define PAK "cache/ui2x.pak"
#define WORK "cache/ui2x_work"
#define TOOL_DIR "cache/waifu2x"
#define TOOL_NAME "waifu2x-ncnn-vulkan-20250915-windows"
#define TOOL_URL "https://github.com/nihui/waifu2x-ncnn-vulkan/releases/download/20250915/" TOOL_NAME ".zip"
#define TOOL_SIZE 35497352ull
#define TOOL_SHA256 "7425be94b94e4c8f37a1e433ac0e0100c43790e2c37418f4b65d8235adfbdc87"

static _Atomic int g_state;
static _Atomic float g_progress;
static _Atomic bool g_running;
int ui2x_state(void) { return g_state; }
float ui2x_progress(void) { return g_progress; }
bool ui2x_running(void) { return g_running; }

static bool is_file(const char *p) { struct stat s; return stat(p, &s) == 0 && S_ISREG(s.st_mode); }
static uint64_t file_size(const char *p) { struct stat s; return stat(p, &s) == 0 ? (uint64_t)s.st_size : 0; }

// ================================================================== the key
uint64_t ui2x_key(const uint8_t *p, uint64_t n) {
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
    uint32_t crc = ~0u, a = 1, b = 0;
    for (uint64_t i = 0; i < n;) {
        uint64_t end = i + 5552 < n ? i + 5552 : n;  // (Adler-32: sums reduced before they can overflow)
        for (; i < end; i++) {
            crc = tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
            a += p[i], b += a;
        }
        a %= 65521, b %= 65521;
    }
    return (uint64_t)~crc << 32 | (b << 16 | a);
}

// ================================================================== the pack, while the game runs
typedef struct { uint64_t key; uint32_t src_w, src_h, fmt, w, h, pad; uint64_t off, size; } PakEntry;  // (56 bytes on disk)
static pthread_mutex_t g_pak_lock = PTHREAD_MUTEX_INITIALIZER;
static PakEntry *g_ent;
static uint32_t g_nent;
static FILE *g_pak;
static int g_pak_loaded;  // 0 not tried, 1 open, -1 none

static void pak_close(void) {
    if (g_pak) fclose(g_pak);
    free(g_ent);
    g_pak = NULL, g_ent = NULL, g_nent = 0, g_pak_loaded = 0;
}

static int cmp_ent(const void *x, const void *y) {
    uint64_t a = ((const PakEntry *)x)->key, b = ((const PakEntry *)y)->key;
    return a < b ? -1 : a > b;
}

static void pak_load(void) {  // (g_pak_lock held)
    if (g_pak_loaded) return;
    g_pak_loaded = -1;
    FILE *f = fopen(PAK, "rb");
    if (!f) return;
    char magic[4];
    uint32_t hdr[3];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "UI2X", 4) || fread(hdr, 4, 3, f) != 3 || hdr[0] != 1 || !hdr[1] || hdr[1] > 100000) {
        fclose(f);
        return;
    }
    PakEntry *e = malloc(sizeof *e * hdr[1]);
    if (!e || fread(e, sizeof *e, hdr[1], f) != hdr[1]) { free(e); fclose(f); return; }
    qsort(e, hdr[1], sizeof *e, cmp_ent);
    g_ent = e, g_nent = hdr[1], g_pak = f, g_pak_loaded = 1;
    LOG("[ui2x] %u interface textures at 2x (" PAK ")\n", g_nent);
}

int ui2x_count(void) {
    pthread_mutex_lock(&g_pak_lock);
    pak_load();
    int n = (int)g_nent;
    pthread_mutex_unlock(&g_pak_lock);
    return n;
}

bool ui2x_enabled(void) { return g_settings.ui2x && ui2x_count() > 0; }

bool ui2x_find(uint64_t key, uint32_t src_w, uint32_t src_h, Ui2xTex *out) {
    if (!g_settings.ui2x) return false;
    bool ok = false;
    pthread_mutex_lock(&g_pak_lock);
    pak_load();
    PakEntry k = {.key = key};
    const PakEntry *e = g_nent ? bsearch(&k, g_ent, g_nent, sizeof k, cmp_ent) : NULL;
    if (e && e->src_w == src_w && e->src_h == src_h && e->size < (256u << 20)) {
        uint8_t *d = malloc(e->size);
        if (d && fseeko(g_pak, (off_t)e->off, SEEK_SET) == 0 && fread(d, 1, e->size, g_pak) == e->size) {
            *out = (Ui2xTex){e->w, e->h, (int)e->fmt, d, e->size};
            ok = true;
        } else {
            free(d);
        }
    }
    pthread_mutex_unlock(&g_pak_lock);
    return ok;
}

// ================================================================== SHA-256 (the upscaler's download)
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; uint32_t used; } Sha256;
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}
static bool sha256_file_is(const char *path, const char *hex) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    Sha256 s = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
    static uint8_t buf[1 << 16];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < r; i++) {
            s.buf[s.used++] = buf[i];
            s.n++;
            if (s.used == 64) { sha_block(&s, s.buf); s.used = 0; }
        }
    fclose(f);
    uint64_t bits = s.n * 8;
    s.buf[s.used++] = 0x80;
    if (s.used > 56) { while (s.used < 64) s.buf[s.used++] = 0; sha_block(&s, s.buf); s.used = 0; }
    while (s.used < 56) s.buf[s.used++] = 0;
    for (int i = 7; i >= 0; i--) s.buf[s.used++] = (uint8_t)(bits >> (i * 8));
    sha_block(&s, s.buf);
    char got[65];
    for (int i = 0; i < 8; i++) snprintf(got + i * 8, 9, "%08x", s.h[i]);
    if (strcmp(got, hex)) LOG("[ui2x] %s: SHA-256 %s, expected %s\n", path, got, hex);
    return !strcmp(got, hex);
}

// ================================================================== step 1: the upscaler
static void tool_paths(char *exe, size_t n, char *dir, size_t dn) {
    snprintf(dir, dn, TOOL_DIR "/" TOOL_NAME);
    snprintf(exe, n, "%s/waifu2x-ncnn-vulkan" PLAT_EXE_SUFFIX, dir);
}

static bool fetch_tool(void) {
    char exe[512], dir[512];
    tool_paths(exe, sizeof exe, dir, sizeof dir);
    if (is_file(exe)) return true;
#ifdef _WIN32
    g_state = UI2X_DOWNLOADING, g_progress = 0;
    mkdir("cache", 0755);
    mkdir(TOOL_DIR, 0755);
    const char *zip = TOOL_DIR "/download.zip";
    remove(zip);
    char curl[600], tar[600];
    const char *sys = getenv("SystemRoot");
    snprintf(curl, sizeof curl, "%s\\System32\\curl.exe", sys ? sys : "C:\\Windows");
    snprintf(tar, sizeof tar, "%s\\System32\\tar.exe", sys ? sys : "C:\\Windows");
    const char *dl[] = {curl, "-fsSL", "--retry", "2", "-o", zip, TOOL_URL, NULL};
    PlatProc p = plat_spawn(dl, TOOL_DIR "/download.log", false);
    int r = -1;
    if (p)
        while ((r = plat_proc_wait(p, false)) == -1) {
            g_progress = (float)((double)file_size(zip) / (double)TOOL_SIZE);
            SDL_Delay(100);
        }
    if (r != 0 || !sha256_file_is(zip, TOOL_SHA256)) {
        LOG("[ui2x] download of %s failed (curl %d, %llu bytes)\n", TOOL_URL, r, (unsigned long long)file_size(zip));
        remove(zip);
        return false;
    }
    const char *un[] = {tar, "-xf", zip, "-C", TOOL_DIR, NULL};
    p = plat_spawn(un, TOOL_DIR "/unpack.log", false);
    r = p ? plat_proc_wait(p, true) : -1;
    remove(zip);
    if (r != 0 || !is_file(exe)) { LOG("[ui2x] couldn't unpack the upscaler (tar %d)\n", r); return false; }
    remove(TOOL_DIR "/download.log");
    remove(TOOL_DIR "/unpack.log");
    return true;
#else
    LOG("[ui2x] no upscaler: put waifu2x-ncnn-vulkan (nihui) in %s\n", dir);
    return false;
#endif
}

// ================================================================== step 2: the textures, from the game files
typedef struct { uint64_t key; uint32_t w, h; uint8_t ch; bool alpha; } Item;  // ch: 1 (BC4, R8), 2 (BC5, RG8), 4 (colour [+ alpha])
static Item *g_items;
static uint32_t g_nitems, g_cap_items;


// Block-linear -> block rows (runtime/nvn/tegra.c; one slice, the BNTX block height shrunk as level_block_height)
static void untile(const uint8_t *src, uint64_t src_size, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t bpb, uint32_t bhl) {
    while (bhl > 0 && (8u << (bhl - 1)) >= hb) bhl--;
    uint32_t row = wb * bpb, width_gobs = (row + 63) / 64;
    uint64_t block = 512ull << bhl;
    for (uint32_t y = 0; y < hb; y++) {
        uint32_t gy = y >> 3;
        uint64_t base = (uint64_t)(gy >> bhl) * width_gobs * block + (uint64_t)(gy & ((1u << bhl) - 1)) * 512;
        uint32_t yi = y & 7;
        for (uint32_t x = 0; x < row; x += 16) {
            uint32_t n = row - x < 16 ? row - x : 16, xi = x & 63;
            uint64_t o = base + (uint64_t)(x >> 6) * block + (((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4));
            if (o + n <= src_size) memcpy(dst + (uint64_t)y * row + x, src + o, n);
            else memset(dst + (uint64_t)y * row + x, 0, n);
        }
    }
}

// Transparent texels take the average colour of their visible neighbours, a few rings out (filtering a 2x image
// then blends with those colours instead of black)
static void bleed(uint8_t *px, uint32_t w, uint32_t h) {
    uint8_t *known = malloc((size_t)w * h), *next = malloc((size_t)w * h);
    if (!known || !next) { free(known); free(next); return; }
    for (size_t i = 0; i < (size_t)w * h; i++) known[i] = px[i * 4 + 3] != 0;
    for (int pass = 0; pass < 8; pass++) {
        bool any = false;
        memcpy(next, known, (size_t)w * h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                if (known[i]) continue;
                uint32_t s[3] = {0}, n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = (int)x + dx, yy = (int)y + dy;
                        if (xx < 0 || yy < 0 || xx >= (int)w || yy >= (int)h || !known[(size_t)yy * w + xx]) continue;
                        const uint8_t *q = px + ((size_t)yy * w + xx) * 4;
                        s[0] += q[0], s[1] += q[1], s[2] += q[2], n++;
                    }
                if (!n) continue;
                for (int c = 0; c < 3; c++) px[i * 4 + c] = (uint8_t)(s[c] / n);
                next[i] = 1, any = true;
            }
        memcpy(known, next, (size_t)w * h);
        if (!any) break;
    }
    free(known), free(next);
}

// One BNTX texture decoded: RGBA8 padded to whole blocks (pw x ph), its key, size and channels. NULL: a format
// left out (*nskip counted) or broken.
typedef struct { uint64_t key; uint32_t w, h, pw, ph, ch; } Decoded;
static uint8_t *decode_texture(const uint8_t *b, uint64_t bsize, uint64_t o, Decoded *dd, uint32_t *nskip) {
#define RD(T, off) (*(const T *)(b + (off)))
    if (o + 0x78 > bsize) return NULL;
    uint32_t fmt = RD(uint32_t, o + 0x1C), type = fmt >> 8;
    int32_t w = RD(int32_t, o + 0x24), h = RD(int32_t, o + 0x28), layout = RD(int32_t, o + 0x34);
    uint64_t ptrs = RD(uint64_t, o + 0x70);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || ptrs + 8 > bsize) return NULL;
    uint64_t data0 = RD(uint64_t, ptrs);
#undef RD
    uint32_t bpb, bs, ch;  // bytes per block, block size (1 or 4), channels
    switch (type) {
    case 0x1a: case 0x1d: bpb = 8, bs = 4, ch = type == 0x1d ? 1 : 4; break;
    case 0x1b: case 0x1c: case 0x20: bpb = 16, bs = 4, ch = 4; break;
    case 0x1e: bpb = 16, bs = 4, ch = 2; break;
    case 0x05: bpb = 2, bs = 1, ch = 4; break;
    case 0x0b: bpb = 4, bs = 1, ch = 4; break;
    case 0x02: bpb = 1, bs = 1, ch = 1; break;
    case 0x09: bpb = 2, bs = 1, ch = 2; break;
    default: (*nskip)++; return NULL;  // (ASTC, BC6: none in the interface that matters)
    }
    uint32_t wb = ((uint32_t)w + bs - 1) / bs, hb = ((uint32_t)h + bs - 1) / bs;
    uint64_t lin_size = (uint64_t)wb * hb * bpb;
    if (data0 >= bsize) return NULL;
    uint8_t *lin = malloc(lin_size);
    if (!lin) return NULL;
    untile(b + data0, bsize - data0, lin, wb, hb, bpb, (uint32_t)layout & 7);
    uint32_t pw = wb * bs, ph = hb * bs;
    *dd = (Decoded){ui2x_key(lin, lin_size), (uint32_t)w, (uint32_t)h, pw, ph, ch};
    uint8_t *px = calloc((size_t)pw * ph, 4);
    if (!px) { free(lin); return NULL; }
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            const uint8_t *blk = lin + ((uint64_t)by * wb + bx) * bpb;
            uint8_t *d = px + ((size_t)by * bs * pw + bx * bs) * 4;
            uint8_t t[16 * 2];
            switch (type) {
            case 0x1a: bcdec_bc1(blk, d, (int)pw * 4); break;
            case 0x1b: bcdec_bc2(blk, d, (int)pw * 4); break;
            case 0x1c: bcdec_bc3(blk, d, (int)pw * 4); break;
            case 0x20: bcdec_bc7(blk, d, (int)pw * 4); break;
            case 0x1d:
                bcdec_bc4(blk, t, 4);
                for (int i = 0; i < 16; i++) { uint8_t *q = d + ((size_t)(i / 4) * pw + i % 4) * 4; q[0] = q[1] = q[2] = t[i]; q[3] = 255; }
                break;
            case 0x1e:
                bcdec_bc5(blk, t, 8);
                for (int i = 0; i < 16; i++) { uint8_t *q = d + ((size_t)(i / 4) * pw + i % 4) * 4; q[0] = t[i * 2]; q[1] = t[i * 2 + 1]; q[2] = 0; q[3] = 255; }
                break;
            case 0x05: {  // R 0-4, G 5-9, B 10-14, A 15 (textures.c's expansion)
                uint32_t v = blk[0] | (uint32_t)blk[1] << 8;
                for (int c = 0; c < 3; c++) { uint32_t x = (v >> (5 * c)) & 31; d[c] = (uint8_t)(x << 3 | x >> 2); }
                d[3] = v >> 15 ? 255 : 0;
                break;
            }
            case 0x0b: memcpy(d, blk, 4); break;
            case 0x02: d[0] = d[1] = d[2] = blk[0]; d[3] = 255; break;
            case 0x09: d[0] = blk[0]; d[1] = blk[1]; d[2] = 0; d[3] = 255; break;
            }
        }
    free(lin);
    return px;
}

// One BNTX texture -> PNG for waifu2x (true: written)
static bool texture_to_png(const uint8_t *b, uint64_t bsize, uint64_t o, uint32_t *nskip) {
    Decoded d;
    uint8_t *px = decode_texture(b, bsize, o, &d, nskip);
    if (!px) return false;
    for (uint32_t i = 0; i < g_nitems; i++)  // (one per content: the language variants are mostly the same images)
        if (g_items[i].key == d.key && g_items[i].w == d.w && g_items[i].h == d.h) { free(px); return false; }
    bool alpha = false;
    if (d.ch == 4)
        for (uint32_t y = 0; y < d.h && !alpha; y++)
            for (uint32_t x = 0; x < d.w && !alpha; x++) alpha = px[((size_t)y * d.pw + x) * 4 + 3] != 255;
    char path[256];
    bool ok = true;
    if (alpha) {  // the alpha as a grey image of its own (waifu2x mishandles RGBA: see above)
        uint8_t *a = malloc((size_t)d.pw * d.ph * 3);
        if (!a) { free(px); return false; }
        for (size_t i = 0; i < (size_t)d.pw * d.ph; i++) a[i * 3] = a[i * 3 + 1] = a[i * 3 + 2] = px[i * 4 + 3];
        snprintf(path, sizeof path, WORK "/in/%016llx_a.png", (unsigned long long)d.key);
        ok = stbi_write_png(path, (int)d.w, (int)d.h, 3, a, (int)d.pw * 3) != 0;
        free(a);
        bleed(px, d.pw, d.ph);
    }
    // the colour (or the channels in R / G) as RGB
    for (size_t i = 0; i < (size_t)d.pw * d.ph; i++) px[i * 3] = px[i * 4], px[i * 3 + 1] = px[i * 4 + 1], px[i * 3 + 2] = px[i * 4 + 2];
    snprintf(path, sizeof path, WORK "/in/%016llx.png", (unsigned long long)d.key);
    ok = ok && stbi_write_png(path, (int)d.w, (int)d.h, 3, px, (int)d.pw * 3) != 0;
    free(px);
    if (!ok) return false;
    if (g_nitems == g_cap_items) {
        g_cap_items = g_cap_items ? g_cap_items * 2 : 1024;
        g_items = realloc(g_items, g_cap_items * sizeof *g_items);
    }
    g_items[g_nitems++] = (Item){d.key, d.w, d.h, (uint8_t)d.ch, alpha};
    return true;
}

// The RomFS image's files: calls fn for each .arc (path, offset, size)
static bool romfs_each_arc(FILE *f, void (*fn)(FILE *f, const char *path, uint64_t off, uint64_t size, void *u), void *u,
                           uint64_t *total) {
    uint64_t h[10];
    if (fseeko(f, 0, SEEK_SET) || fread(h, 8, 10, f) != 10 || h[4] > (64u << 20) || h[8] > (64u << 20)) return false;
    uint8_t *dm = malloc(h[4]), *fm = malloc(h[8]);
    bool ok = dm && fm && !fseeko(f, (off_t)h[3], SEEK_SET) && fread(dm, 1, h[4], f) == h[4] && !fseeko(f, (off_t)h[7], SEEK_SET) &&
              fread(fm, 1, h[8], f) == h[8];
    if (ok) {
        // directories depth first: (offset in dm, path)
        typedef struct { uint32_t o; char path[256]; } Dir;
        Dir *st = malloc(sizeof *st * 4096);
        int n = 0;
        if (st) st[n++] = (Dir){0, ""};
        while (st && n) {
            Dir d = st[--n];
            if (d.o + 0x18 > h[4]) continue;
            const uint32_t *de = (const uint32_t *)(dm + d.o);
            char here[256];
            uint32_t nl = de[5];
            if (nl && strlen(d.path) + nl + 2 < sizeof here) snprintf(here, sizeof here, "%s%s%.*s", d.path, d.path[0] ? "/" : "", (int)nl, (const char *)(dm + d.o + 0x18));
            else snprintf(here, sizeof here, "%s", d.path);
            for (uint32_t fo = de[3]; fo != 0xFFFFFFFF && fo + 0x20 <= h[8];) {
                const uint8_t *fe = fm + fo;
                uint32_t sib = *(const uint32_t *)(fe + 4), fl = *(const uint32_t *)(fe + 0x1C);
                uint64_t doff = *(const uint64_t *)(fe + 8), dsize = *(const uint64_t *)(fe + 0x10);
                const char *name = (const char *)(fe + 0x20);
                if (fl > 4 && !memcmp(name + fl - 4, ".arc", 4)) {
                    char path[512];
                    snprintf(path, sizeof path, "%s/%.*s", here, (int)fl, name);
                    if (total) *total += dsize;
                    else fn(f, path, h[9] + doff, dsize, u);
                }
                fo = sib;
            }
            for (uint32_t c = de[2]; c != 0xFFFFFFFF && c + 0x18 <= h[4] && n < 4096;) {
                st[n].o = c;
                snprintf(st[n].path, sizeof st[n].path, "%s", here);
                n++;
                c = ((const uint32_t *)(dm + c))[1];
            }
        }
        free(st);
    }
    free(dm), free(fm);
    return ok;
}

typedef struct { uint64_t done, total; uint32_t written, skipped; } ExtractCtx;
static void extract_arc(FILE *f, const char *path, uint64_t off, uint64_t size, void *u) {
    ExtractCtx *x = u;
    x->done += size;
    g_progress = x->total ? (float)((double)x->done / (double)x->total) : 0;
    if (size < 16 || size > (256u << 20)) return;
    uint8_t *d = malloc(size);
    if (!d || fseeko(f, (off_t)off, SEEK_SET) || fread(d, 1, size, f) != size || memcmp(d, "SARC", 4)) { free(d); return; }
    uint16_t hlen = *(uint16_t *)(d + 4);
    uint32_t data_off = *(uint32_t *)(d + 0x0C);
    if (hlen + 12u > size) { free(d); return; }
    uint16_t n = *(uint16_t *)(d + hlen + 6);
    for (uint32_t i = 0; i < n && hlen + 12 + i * 16 + 16 <= size; i++) {
        const uint32_t *node = (const uint32_t *)(d + hlen + 12 + i * 16);
        uint64_t s = (uint64_t)data_off + node[2], e = (uint64_t)data_off + node[3];
        if (e > size || s + 4 > e || memcmp(d + s, "BNTX", 4)) continue;
        const uint8_t *b = d + s;
        uint64_t bs = e - s;
        for (uint64_t o = 0; o + 4 <= bs; o++)  // (each texture's BRTI block)
            if (b[o] == 'B' && !memcmp(b + o, "BRTI", 4) && texture_to_png(b, bs, o, &x->skipped)) x->written++;
    }
    free(d);
    (void)path;
}

// ================================================================== step 4: compressed into the pack
// How far a 2x image (stride bytes a texel, its first nc channels compared), taken back down to 1x (2x2 averages), is
// from waifu2x's input: the mean absolute difference, 0..255 (-1: the input can't be read). waifu2x sometimes exits 0
// with garbage for every image (once in seven runs, RTX 3080: in half the time) - a pack of it drew the menus as noise.
// Measured: good images up to 7.2, that garbage 66 to 188.
static double back_down_error(const char *in_path, const uint8_t *out, int w, int h, int stride, int nc) {
    int iw, ih, ic;
    uint8_t *in = stbi_load(in_path, &iw, &ih, &ic, 3);
    if (!in || iw * 2 != w || ih * 2 != h) { stbi_image_free(in); return -1; }
    uint64_t sum = 0;
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < iw; x++)
            for (int c = 0; c < nc; c++) {
                const uint8_t *o = out + ((size_t)(y * 2) * w + x * 2) * stride + c;
                int avg = (o[0] + o[stride] + o[(size_t)w * stride] + o[(size_t)w * stride + stride] + 2) / 4;
                int d = avg - in[((size_t)y * iw + x) * 3 + c];
                sum += (uint64_t)(d < 0 ? -d : d);
            }
    stbi_image_free(in);
    return (double)sum / ((double)iw * ih * nc);
}
#define BACK_DOWN_MAX 16.0

static uint32_t g_rejected;  // images whose 2x version didn't match (left out of the pack)
static double g_err_max;     // the largest difference of an image kept

static bool pack(void) {
    char tmp[] = PAK ".tmp";
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    PakEntry *e = calloc(g_nitems ? g_nitems : 1, sizeof *e);
    uint64_t off = 16 + (uint64_t)sizeof *e * g_nitems;
    fwrite("UI2X", 1, 4, f);
    uint32_t hdr[3] = {1, 0, 0};
    fwrite(hdr, 4, 3, f);
    fwrite(e, sizeof *e, g_nitems, f);  // (the table, again at the end)
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_nitems; i++) {
        g_progress = (float)i / (float)g_nitems;
        const Item *it = &g_items[i];
        char path[256];
        snprintf(path, sizeof path, WORK "/out/%016llx.png", (unsigned long long)it->key);
        int w, h, c;
        uint8_t *px = stbi_load(path, &w, &h, &c, 4);
        if (!px) continue;
        if ((uint32_t)w != it->w * 2 || (uint32_t)h != it->h * 2) { stbi_image_free(px); continue; }
        snprintf(path, sizeof path, WORK "/in/%016llx.png", (unsigned long long)it->key);
        double err = back_down_error(path, px, w, h, 4, 3);
        bool opaque = true;
        if (it->alpha) {  // (its alpha, upscaled as an image of its own)
            snprintf(path, sizeof path, WORK "/out/%016llx_a.png", (unsigned long long)it->key);
            int aw, ah, ac;
            uint8_t *a = stbi_load(path, &aw, &ah, &ac, 1);
            if (!a || aw != w || ah != h) { stbi_image_free(a); stbi_image_free(px); continue; }
            snprintf(path, sizeof path, WORK "/in/%016llx_a.png", (unsigned long long)it->key);
            double ea = back_down_error(path, a, w, h, 1, 1);  // (the input is grey: its R is the alpha)
            err = ea < 0 ? -1 : err > ea ? err : ea;
            for (size_t k = 0; k < (size_t)w * h; k++) px[k * 4 + 3] = a[k], opaque &= a[k] == 255;
            stbi_image_free(a);
        }
        if (err < 0 || err > BACK_DOWN_MAX) { g_rejected++; stbi_image_free(px); continue; }
        if (err > g_err_max) g_err_max = err;
        int fmt = it->ch == 1 ? UI2X_BC4 : it->ch == 2 ? UI2X_BC5 : opaque ? UI2X_BC1 : UI2X_BC3;
        uint32_t bpb = fmt == UI2X_BC1 || fmt == UI2X_BC4 ? 8 : 16, wb = ((uint32_t)w + 3) / 4, hb = ((uint32_t)h + 3) / 4;
        uint64_t size = (uint64_t)wb * hb * bpb;
        uint8_t *out = malloc(size);
        if (!out) { stbi_image_free(px); continue; }
        for (uint32_t by = 0; by < hb; by++)
            for (uint32_t bx = 0; bx < wb; bx++) {
                uint8_t blk[64], r[16], rg[32];
                for (int k = 0; k < 16; k++) {  // (edges: the last row / column repeated)
                    uint32_t x = bx * 4 + k % 4, y = by * 4 + k / 4;
                    if (x >= (uint32_t)w) x = (uint32_t)w - 1;
                    if (y >= (uint32_t)h) y = (uint32_t)h - 1;
                    memcpy(blk + k * 4, px + ((size_t)y * w + x) * 4, 4);
                    r[k] = blk[k * 4], rg[k * 2] = blk[k * 4], rg[k * 2 + 1] = blk[k * 4 + 1];
                }
                uint8_t *d = out + ((uint64_t)by * wb + bx) * bpb;
                if (fmt == UI2X_BC4) stb_compress_bc4_block(d, r);
                else if (fmt == UI2X_BC5) stb_compress_bc5_block(d, rg);
                else stb_compress_dxt_block(d, blk, fmt == UI2X_BC3, STB_DXT_HIGHQUAL);
            }
        stbi_image_free(px);
        fwrite(out, 1, size, f);
        free(out);
        e[n++] = (PakEntry){it->key, it->w, it->h, (uint32_t)fmt, (uint32_t)w, (uint32_t)h, 0, off, size};
        off += size;
    }
    hdr[1] = n;
    LOG("[ui2x] back down to 1x: %u of %u images off by more than %.0f (left out), the largest kept %.2f\n",
        g_rejected, g_nitems, BACK_DOWN_MAX, g_err_max);
    // (many off: waifu2x's output as a whole is broken - no pack, the start screen says to try again)
    bool ok = n > 0 && g_rejected <= g_nitems / 10 && !fseeko(f, 4, SEEK_SET) && fwrite(hdr, 4, 3, f) == 3 && fwrite(e, sizeof *e, n, f) == n;
    ok = !fclose(f) && ok;
    free(e);
    if (!ok) { remove(tmp); return false; }
    pthread_mutex_lock(&g_pak_lock);
    pak_close();  // (the old one, if the game had it open)
    remove(PAK);
    ok = rename(tmp, PAK) == 0;
    pthread_mutex_unlock(&g_pak_lock);
    LOG("[ui2x] %u of %u textures packed into " PAK " (%.0f MB)\n", n, g_nitems, (double)off / (1 << 20));
    return ok;
}

static int count_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static void *generate_thread(void *unused) {
    (void)unused;
    uint64_t t0 = plat_now_ns();
    char exe[512], dir[512];
    tool_paths(exe, sizeof exe, dir, sizeof dir);
    if (!fetch_tool()) { g_state = UI2X_FAIL_DOWNLOAD; goto done; }
    // 2: the textures
    g_state = UI2X_EXTRACTING, g_progress = 0;
    plat_remove_tree(WORK);
    mkdir("cache", 0755);
    mkdir(WORK, 0755);
    mkdir(WORK "/in", 0755);
    mkdir(WORK "/out", 0755);
    FILE *f = fopen("extracted/romfs.bin", "rb");
    ExtractCtx x = {0};
    if (!f || !romfs_each_arc(f, NULL, NULL, &x.total) || !romfs_each_arc(f, extract_arc, &x, NULL) || !g_nitems) {
        if (f) fclose(f);
        LOG("[ui2x] no interface textures read from extracted/romfs.bin\n");
        g_state = UI2X_FAIL_FILES;
        goto done;
    }
    fclose(f);
    LOG("[ui2x] %u interface textures (%u in formats left out), %.1f s\n", x.written, x.skipped, (plat_now_ns() - t0) / 1e9);
    // 3: waifu2x (its models are found next to it: -m relative to its own folder), then 4: the pack. On an AMD integrated
    // GPU its output came back mostly wrong (issue #30: 2981 of 3081 images off) while the GPU didn't finish our own work
    // within 10 s: once more, gentler - small tiles, one thread each (-t 32 -j 1:1:1) - then the start screen says to try
    // again. (Its CPU mode, -g -1, crashes: waifu2x-ncnn-vulkan 20250915.)
    for (int gentle = getenv("LARECOMP_UI2X_GENTLE") != NULL;; gentle = 1) {  // (test runs: LARECOMP_UI2X_GENTLE=1 starts there)
        g_state = UI2X_UPSCALING, g_progress = 0;
        plat_remove_tree(WORK "/out");
        mkdir(WORK "/out", 0755);
        const char *argv[] = {exe, "-i", WORK "/in", "-o", WORK "/out", "-n", "1", "-s", "2", "-f", "png", "-m", "models-cunet",
                              gentle ? "-t" : NULL, "32", "-j", "1:1:1", NULL};
        int n_in = count_files(WORK "/in");  // (the images with transparency: two each)
        PlatProc p = plat_spawn(argv, WORK "/waifu2x.log", false);
        int r = -1;
        // (no new image for 3 minutes: stopped - the gentle pass on an AMD integrated GPU sat there for good, issue #30)
        int last_made = -1;
        uint64_t last_new = plat_now_ns();
        if (p)
            while ((r = plat_proc_wait(p, false)) == -1) {
                int now_made = count_files(WORK "/out");
                g_progress = (float)now_made / (float)(n_in ? n_in : 1);
                if (now_made != last_made) last_made = now_made, last_new = plat_now_ns();
                else if (plat_now_ns() - last_new > 180000000000ull) {
                    LOG("[ui2x] waifu2x: no new image in 3 minutes, stopped\n");
                    plat_proc_kill(p);
                    r = -2;
                    break;
                }
                SDL_Delay(200);
            }
        int made = count_files(WORK "/out");
        LOG("[ui2x] waifu2x%s: exit %d, %d of %d images, %.1f s\n", gentle ? " (small tiles)" : "", r, made, n_in,
            (plat_now_ns() - t0) / 1e9);
        bool ran = r == 0 && made >= n_in / 2;
        if (!ran) {
            FILE *lg = fopen(WORK "/waifu2x.log", "rb");  // (its last lines into ours)
            if (lg) {
                char buf[2048];
                fseek(lg, 0, SEEK_END);
                long sz = ftell(lg);
                fseek(lg, sz > (long)sizeof buf - 1 ? sz - (long)sizeof buf + 1 : 0, SEEK_SET);
                size_t k = fread(buf, 1, sizeof buf - 1, lg);
                buf[k] = 0;
                LOG("[ui2x] waifu2x said: ...%s\n", buf);
                fclose(lg);
            }
        }
        if (ran) {
            g_state = UI2X_PACKING, g_progress = 0;
            g_rejected = 0, g_err_max = 0;
            if (pack()) break;
            if (g_rejected <= g_nitems / 10) { g_state = UI2X_FAIL_FILES; goto done; }
        }
        if (gentle) { g_state = UI2X_FAIL_UPSCALER; goto done; }
        LOG("[ui2x] the upscaled images didn't work out: once more with small tiles\n");
    }
    plat_remove_tree(WORK);
    LOG("[ui2x] done in %.1f s\n", (plat_now_ns() - t0) / 1e9);
    g_state = UI2X_DONE, g_progress = 1;
done:
    free(g_items);
    g_items = NULL, g_nitems = g_cap_items = 0;
    g_running = false;
    return NULL;
}

void ui2x_generate_start(void) {
    if (g_running) return;
    g_running = true;
    g_state = UI2X_EXTRACTING, g_progress = 0;
    pthread_t t;
    pthread_create(&t, NULL, generate_thread, NULL);
    pthread_detach(t);
}
