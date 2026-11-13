// A non-Steam shortcut for the game, written into Steam's own shortcuts.vdf (runtime/steam.h). The launcher's "Add to
// Steam" button and `make steam` (tools/steam_shortcut.sh, the .desktop entry) are two roads to the same place: this one
// puts an entry in Steam's list so the game shows up there without the player browsing for the file.
//
// Steam keeps its shortcuts in <Steam>/userdata/<account>/config/shortcuts.vdf, a binary key-value file that it reads at
// start and writes when it changes: it must not be running, or it writes its own copy over ours when it exits. The file
// is edited surgically (the raw bytes of every other entry are kept as they are) and copied to shortcuts.vdf.steam-backup
// before the first change. The entry id follows Steam's rule for non-Steam games, the CRC-32 of the quoted exe path
// followed by the name with the top bit set, so Steam and its artwork lookups agree with it.
#include "steam.h"

#include "../third_party/stb/stb_image.h"  // (the implementations live in runtime/ui2x.c)
#include "../third_party/stb/stb_image_write.h"
#include "../paths.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SEP "/"

// ------------------------------------------------------------------ where Steam is
static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// The Steam directory: LARECOMP_STEAM_ROOT (tests), else the usual three installs, first one with a userdata/ in it.
static bool steam_root(char *out, size_t n) {
    const char *env = getenv("LARECOMP_STEAM_ROOT"), *home = getenv("HOME");
    if (env && *env) { snprintf(out, n, "%s", env); return true; }
    if (!home || !*home) return false;
    static const char *const rel[] = {".steam/steam", ".local/share/Steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam"};
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        char p[1024], u[1100];
        snprintf(p, sizeof p, "%s/%s", home, rel[i]);
        snprintf(u, sizeof u, "%s/userdata", p);
        if (is_dir(u)) { snprintf(out, n, "%s", p); return true; }
    }
    return false;
}

// The account Steam starts by itself: config/loginusers.vdf holds a block per account, and the one with AutoLogin "1"
// is that account. Its userdata directory is the account id: the SteamID64 less 76561197960265728.
static bool steam_autologin(const char *root, char *out, size_t n) {
    char path[1200];
    snprintf(path, sizeof path, "%s/config/loginusers.vdf", root);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512], id[32] = {0}, found[32] = {0};
    while (fgets(line, sizeof line, f)) {
        char a[128] = {0}, b[128] = {0};
        if (sscanf(line, " \"%127[0-9]\"", a) == 1) {  // (a block key: the SteamID64)
            snprintf(id, sizeof id, "%s", a);
            continue;
        }
        if (sscanf(line, " \"%127[^\"]\" \"%127[^\"]\"", a, b) == 2 && !strcmp(a, "AutoLogin") && !strcmp(b, "1"))
            snprintf(found, sizeof found, "%s", id);
    }
    fclose(f);
    if (!found[0]) return false;
    snprintf(out, n, "%llu", strtoull(found, NULL, 10) - 76561197960265728ull);
    return true;
}

// <root>/userdata/<account>/config: the account Steam logs in by itself (the library the player sees), else the one with
// the most recently written shortcuts.vdf, else the first one
static bool steam_config_dir(const char *root, char *out, size_t n) {
    char acct[32];
    if (steam_autologin(root, acct, sizeof acct)) {
        char cfg[1024];
        snprintf(cfg, sizeof cfg, "%s/userdata/%s/config", root, acct);
        if (is_dir(cfg)) { snprintf(out, n, "%s", cfg); return true; }
    }
    char ud[1024];
    snprintf(ud, sizeof ud, "%s/userdata", root);
    DIR *d = opendir(ud);
    if (!d) return false;
    char best[1024] = {0};
    time_t best_t = 0, first_t = 0;
    char first[1024] = {0};
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char cfg[1024], vdf[1100];
        snprintf(cfg, sizeof cfg, "%s/%s/config", ud, e->d_name);
        snprintf(vdf, sizeof vdf, "%s/shortcuts.vdf", cfg);
        struct stat st;
        time_t t = stat(vdf, &st) == 0 ? st.st_mtime : 0;
        if (!first[0] || t < first_t) snprintf(first, sizeof first, "%s", cfg), first_t = t;  // (no file: 0)
        if (t > best_t) snprintf(best, sizeof best, "%s", cfg), best_t = t;
    }
    closedir(d);
    const char *pick = best[0] ? best : first;
    if (!pick[0]) return false;
    snprintf(out, n, "%s", pick);
    return true;
}

// A running Steam owns the file: its exit would write its own copy over ours
static bool steam_running(void) {
    if (getenv("LARECOMP_STEAM_RUNNING")) return false;  // (tests)
    DIR *d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    for (struct dirent *e; (e = readdir(d)) && !found;) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64], comm[64];
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f) && !strcmp(comm, "steam\n")) found = true;  // (the client itself: not steamos-*)
        fclose(f);
    }
    closedir(d);
    return found;
}

// ------------------------------------------------------------------ CRC-32 (the app id)
static uint32_t crc32(const void *data, size_t n) {
    static uint32_t table[256];
    static bool init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (const uint8_t *p = data; n--; p++) c = table[(c ^ *p) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------------ the binary VDF
// A map is a run of entries ended by 0x08. An entry is a type byte, the key (0-terminated), then the value: 0x00 a map,
// 0x01 a string (0-terminated), 0x02 a u32. The file is one map. Everything here works on byte offsets, so an entry
// this code does not understand - Steam adds fields - is kept byte for byte.
typedef struct { size_t start, end; } Span;

static bool read_cstr(const uint8_t *b, size_t n, size_t *p) {  // (advance past a 0-terminated string)
    while (*p < n && b[*p]) (*p)++;
    if (*p >= n) return false;
    (*p)++;
    return true;
}

// Walk one value at *p inside [0, n); returns its end offset, 0 on a malformed file.
static size_t skip_value(const uint8_t *b, size_t n, size_t p, uint8_t type) {
    if (type == 0) {
        for (;;) {
            if (p >= n) return 0;
            uint8_t t = b[p++];
            if (t == 0x08) return p;
            if (t > 0x02) return 0;
            if (!read_cstr(b, n, &p)) return 0;
            p = skip_value(b, n, p, t);
            if (!p) return 0;
        }
    }
    if (type == 1) return read_cstr(b, n, &p) ? p : 0;
    if (type == 2) return p + 4 <= n ? p + 4 : 0;
    return 0;
}

// Copy of a value's bytes out of the file (a string value, or the u32), for the fields we read.
static bool value_string(const uint8_t *b, size_t n, size_t p, char *out, size_t on) {
    if (p >= n) return false;
    snprintf(out, on, "%s", (const char *)b + p);  // (p is at the value: past the key)
    return true;
}

// The entries of the "shortcuts" map: their spans and AppName / Exe, so ours can be found, dropped and re-added.
typedef struct { Span span; char name[256], exe[1100]; } Entry;

typedef struct {
    size_t map_start;  // first byte of the first entry
    size_t map_end;    // the byte after the closing 0x08
    size_t children_at;  // where the children start (== map_start)
    Entry *e;
    int n;
} Shortcuts;

static bool parse_shortcuts(const uint8_t *b, size_t n, Shortcuts *s) {
    size_t p = 0;
    memset(s, 0, sizeof *s);
    for (;;) {  // the root map
        if (p >= n) return false;
        uint8_t t = b[p++];
        if (t == 0x08) return false;  // (an empty file: no "shortcuts" key)
        if (t != 0x00) { if (!read_cstr(b, n, &p)) return false; p = skip_value(b, n, p, t); if (!p) return false; continue; }
        size_t key = p;
        if (!read_cstr(b, n, &p)) return false;
        if (strcmp((const char *)b + key, "shortcuts")) {  // (another top-level key: skip its value whole)
            p = skip_value(b, n, p, 0);
            if (!p) return false;
            continue;
        }
        s->map_start = p;
        while (p < n) {
            uint8_t ct = b[p];
            if (ct == 0x08) { s->map_end = p + 1; return true; }
            if (ct != 0x00) return false;  // (an entry of the shortcuts map is a map: a numbered key)
            size_t start = p;
            p++;
            size_t idx = p;
            if (!read_cstr(b, n, &p)) return false;
            size_t end = skip_value(b, n, p, 0);
            if (!end) return false;
            Entry entry = {{start, end}, {0}, {0}};
            for (size_t q = p; q + 1 < end;) {  // (the entry's own fields)
                uint8_t ft = b[q++];
                if (ft == 0x08) break;
                if (ft > 0x02) break;
                size_t fk = q;
                if (!read_cstr(b, n, &q)) break;
                const char *fname = (const char *)b + fk;
                if (ft == 0x01 && !strcmp(fname, "AppName")) value_string(b, n, q, entry.name, sizeof entry.name);
                else if (ft == 0x01 && !strcmp(fname, "exe")) value_string(b, n, q, entry.exe, sizeof entry.exe);
                q = skip_value(b, n, q, ft);
                if (!q) break;
            }
            (void)idx;
            Entry *grown = realloc(s->e, (size_t)(s->n + 1) * sizeof *grown);
            if (!grown) return false;
            s->e = grown;
            s->e[s->n++] = entry;
            p = end;
        }
        return false;
    }
}

// ------------------------------------------------------------------ the entry we add
typedef struct { uint8_t *b; size_t n, cap; bool oom; } Buf;

static void put(Buf *b, const void *p, size_t n) {
    if (b->oom || b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n) cap *= 2;
        uint8_t *grown = realloc(b->b, cap);
        if (!grown) { b->oom = true; return; }
        b->b = grown, b->cap = cap;
    }
    memcpy(b->b + b->n, p, n);
    b->n += n;
}
static void put_s(Buf *b, const char *s) { put(b, s, strlen(s) + 1); }
static void put_u32(Buf *b, uint32_t v) { put(b, &v, 4); }
static void put_str(Buf *b, const char *k, const char *v) {
    uint8_t t = 0x01;
    put(b, &t, 1), put_s(b, k), put_s(b, v);
}
static void put_int(Buf *b, const char *k, uint32_t v) {
    uint8_t t = 0x02;
    put(b, &t, 1), put_s(b, k), put_u32(b, v);
}

// A shortcut as Steam writes one (its tags map holds "Game")
static void entry_bytes(Buf *b, const char *idx, const char *name, const char *exe, const char *dir, const char *opts,
                        uint32_t appid) {
    uint8_t map = 0x00;
    put(b, &map, 1), put_s(b, idx);
    char q[4096];
    snprintf(q, sizeof q, "\"%s\"", exe);
    put_str(b, "AppName", name);
    put_str(b, "Exe", q);
    snprintf(q, sizeof q, "\"%s\"", dir);
    put_str(b, "StartDir", q);
    put_str(b, "icon", "");
    put_str(b, "ShortcutPath", "");
    put_str(b, "LaunchOptions", opts ? opts : "");
    put_int(b, "IsHidden", 0);
    put_int(b, "AllowDesktopConfig", 1);
    put_int(b, "AllowOverlay", 1);
    put_int(b, "OpenVR", 0);
    put_int(b, "Devkit", 0);
    put_str(b, "DevkitGameID", "");
    put_int(b, "LastPlayTime", 0);
    {
        uint8_t tags = 0x00, end = 0x08;
        put(b, &tags, 1), put_s(b, "tags");
        put_str(b, "0", "Game");  // (the tag's index is the key, its name the string)
        put(b, &end, 1);
    }
    put_int(b, "appid", appid);
    {
        uint8_t end = 0x08;
        put(b, &end, 1);
    }
}

// ------------------------------------------------------------------ the artwork
// Steam looks a non-Steam game's pictures up in <config>/grid/ by the app id: <id>.png (920x430), <id>p.png (600x900),
// <id>_hero.png (1920x620), <id>_logo.png (600x600) and <id>_icon.png (256x256). Without them the entry is a grey box.
// The project's own icon, centred on a dark card.
typedef struct { int w, h; const char *suffix; } GridSize;

static void grid_image(const uint8_t *src, int sw, int sh, int w, int h, uint8_t *dst) {
    const uint8_t bg[3] = {24, 26, 22};  // (the start screen's dark green-grey)
    for (int i = 0; i < w * h; i++) dst[i * 4] = bg[0], dst[i * 4 + 1] = bg[1], dst[i * 4 + 2] = bg[2], dst[i * 4 + 3] = 255;
    int side = h * 82 / 100;  // (the icon fits the card's height, with a margin)
    if (side > w * 82 / 100) side = w * 82 / 100;
    int ox = (w - side) / 2, oy = (h - side) / 2;
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            int sx = x * sw / side, sy = y * sh / side;  // (a box sample of the source)
            const uint8_t *sp = src + ((size_t)sy * sw + sx) * 4;
            uint8_t *dp = dst + ((size_t)(oy + y) * w + (ox + x)) * 4;
            unsigned a = sp[3];
            for (int c = 0; c < 3; c++) dp[c] = (uint8_t)((sp[c] * a + dp[c] * (255 - a)) / 255);
            dp[3] = 255;
        }
}

static void write_grid(const char *cfg, uint32_t appid) {
    char icon[1100];
    if (!rt_resource("icon.png", "docs/icon/icon_1024.png", icon, sizeof icon)) return;
    int sw = 0, sh = 0, sc = 0;
    uint8_t *src = stbi_load(icon, &sw, &sh, &sc, 4);
    if (!src) return;
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/grid", cfg);
    mkdir(dir, 0755);
    static const GridSize sizes[] = {{920, 430, ".png"}, {600, 900, "p.png"}, {1920, 620, "_hero.png"},
                                     {600, 600, "_logo.png"}, {256, 256, "_icon.png"}};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        uint8_t *px = malloc((size_t)sizes[i].w * (size_t)sizes[i].h * 4);
        if (!px) continue;
        grid_image(src, sw, sh, sizes[i].w, sizes[i].h, px);
        char path[1400];
        snprintf(path, sizeof path, "%s/%08x%s", dir, appid, sizes[i].suffix);
        stbi_write_png(path, sizes[i].w, sizes[i].h, 4, px, sizes[i].w * 4);
        free(px);
    }
    stbi_image_free(src);
}

// ------------------------------------------------------------------ the operation
static bool write_file(const char *path, const void *data, size_t n) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp-lasrecomp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = !fclose(f) && ok;
    if (!ok) { remove(tmp); return false; }
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}

int steam_add_shortcut(const char *exe, const char *name, const char *opts, char *out, size_t on) {
    char root[1024];
    if (!steam_root(root, sizeof root)) {
        snprintf(out, on, "No Steam install found (looked in ~/.steam/steam, ~/.local/share/Steam, the Flatpak).");
        return STEAM_ERR_NO_STEAM;
    }
    if (steam_running()) {
        snprintf(out, on, "Steam is running: quit Steam, then add the shortcut (Steam keeps the list in memory and "
                          "writes it back on exit).");
        return STEAM_ERR_STEAM_RUNNING;
    }
    char cfg[1024];
    if (!steam_config_dir(root, cfg, sizeof cfg)) {
        snprintf(out, on, "No Steam account folder in %s/userdata.", root);
        return STEAM_ERR_NO_STEAM;
    }
    char vdf[1200], bak[1300];
    snprintf(vdf, sizeof vdf, "%s/shortcuts.vdf", cfg);
    snprintf(bak, sizeof bak, "%s.steam-backup", vdf);

    uint8_t *b = NULL;
    size_t n = 0;
    {   // read what is there (an absent file is an empty one)
        FILE *f = fopen(vdf, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                b = malloc((size_t)sz);
                if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); b = NULL; sz = 0; }
                n = (size_t)(b ? sz : 0);
            }
            fclose(f);
        }
    }

    char exe_dir[1100];
    snprintf(exe_dir, sizeof exe_dir, "%s", exe);
    { char *slash = strrchr(exe_dir, '/'); if (slash) *slash = 0; else snprintf(exe_dir, sizeof exe_dir, "."); }

    char appid_key[4200];
    snprintf(appid_key, sizeof appid_key, "\"%s\"%s", exe, name);
    uint32_t appid = (crc32(appid_key, strlen(appid_key)) | 0x80000000u);

    Shortcuts sc;
    bool have = n && parse_shortcuts(b, n, &sc);
    if (n && !have) {  // (a file this code does not understand: never guess)
        free(b);
        snprintf(out, on, "%s has an unexpected layout: left alone.", vdf);
        return STEAM_ERR_VDF;
    }
    bool already = false;
    long max_idx = -1;
    for (int i = 0; i < sc.n; i++)
        if (!strcmp(sc.e[i].name, name)) already = true;
    if (have) {  // the highest numbered key, to add ours after it
        for (int i = 0; i < sc.n; i++) {
            size_t p = sc.e[i].span.start + 1;
            max_idx = (max_idx > atol((const char *)b + p)) ? max_idx : atol((const char *)b + p);
        }
    }
    if (already) {
        free(b), free(sc.e);
        snprintf(out, on, "Steam already has \"%s\".", name);
        return STEAM_OK_ALREADY;
    }

    Buf ob = {0};
    if (have) {
        put(&ob, b, sc.map_start);  // (the root map and the "shortcuts" key)
        for (int i = 0; i < sc.n; i++) put(&ob, b + sc.e[i].span.start, sc.e[i].span.end - sc.e[i].span.start);
        char idx[32];
        snprintf(idx, sizeof idx, "%ld", max_idx + 1);
        entry_bytes(&ob, idx, name, exe, exe_dir, opts, appid);  // (StartDir: harmless, Steam opens none)
        uint8_t end = 0x08;
        put(&ob, &end, 1);
        put(&ob, b + sc.map_end, n - sc.map_end);  // (anything after the map: kept)
    } else {
        uint8_t root_map = 0x00, shortcuts_map = 0x00, end = 0x08;
        put(&ob, &root_map, 1), put_s(&ob, "shortcuts");
        put(&ob, &shortcuts_map, 1);
        entry_bytes(&ob, "0", name, exe, exe_dir, opts, appid);
        put(&ob, &end, 1);  // the shortcuts map
        put(&ob, &end, 1);  // the root map
    }
    free(b), free(sc.e);
    if (ob.oom) { free(ob.b); snprintf(out, on, "Out of memory."); return STEAM_ERR_VDF; }

    // The backup is made once and then left alone: it stays the file the player had before this ever ran.
    struct stat sst;
    if (stat(vdf, &sst) == 0 && stat(bak, &sst) != 0) {
        FILE *src = fopen(vdf, "rb"), *dst = fopen(bak, "wb");
        if (src && dst) {
            char chunk[65536];
            size_t got;
            while ((got = fread(chunk, 1, sizeof chunk, src)) > 0) fwrite(chunk, 1, got, dst);
        }
        if (src) fclose(src);
        if (dst) fclose(dst);
    }
    bool ok = write_file(vdf, ob.b, ob.n);
    free(ob.b);
    if (!ok) {
        snprintf(out, on, "Couldn't write %s.", vdf);
        return STEAM_ERR_WRITE;
    }
    write_grid(cfg, appid);
    snprintf(out, on, "Added \"%s\" to Steam (restart Steam to see it).", name);
    return STEAM_OK_ADDED;
}
