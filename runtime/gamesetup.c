// Game file setup (runtime/gamesetup.h). The picked folder is searched a few levels deep (dump tools nest the files):
// exefs/ is the folder with main and main.npdm, the RomFS either a romfs.bin image or the folder with bin/message.
// exefs/ is copied into <data root>/extracted (hard links where the file system allows), the RomFS image is copied
// or built (runtime/romfs_build.c), then the game code is recompiled from exefs/ (runtime/gamebuild.c).
#include "gamesetup.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gamebuild.h"
#include "gamelib.h"
#include "hle.h"
#include "paths.h"
#include "platform.h"
#include "romfs_build.h"

#define ROOT "extracted"  // (in the data root, the working directory)
// The two games (program ids, main.npdm's ACI0), any version. The 60 fps and wild battles options and the generator's
// instruction patches are addresses in the main: only the builds with address tables get them (game_build).
#define TITLE_EEVEE 0x0100187003A36000ull
#define TITLE_PIKACHU 0x010003F003A34000ull
static const uint8_t EEVEE_100[8] = {0x0a, 0x3f, 0x0d, 0x21, 0xe6, 0x7d, 0xad, 0x80};    // Let's Go, Eevee! v1.0.0 main
static const uint8_t PIKACHU_100[8] = {0x1c, 0x37, 0x56, 0x92, 0xde, 0x4f, 0x4b, 0x87};  // Let's Go, Pikachu! v1.0.0
static const uint8_t EEVEE_102[8] = {0x58, 0x31, 0xec, 0x64, 0xd6, 0xb6, 0x96, 0xfd};    // Let's Go, Eevee! v1.0.2 main

static _Atomic int g_state;
static _Atomic float g_progress;
static _Atomic bool g_running;
static char g_folder[1024];

static bool image_ok(void);
static bool is_file(const char *p);
int setup_state(void) {  // (idle with a romfs.bin set up before that can't be read: say so, the setup is to be done again)
    if (g_state == SETUP_IDLE && !g_running && is_file(ROOT "/romfs.bin") && !image_ok()) return SETUP_FAIL_IMAGE;
    return g_state;
}
float setup_progress(void) { return g_progress; }
bool setup_running(void) { return g_running; }

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static uint64_t program_id(const char *exefs) {
    char p[1100];
    snprintf(p, sizeof p, "%s/main.npdm", exefs);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    uint64_t id = 0;
    uint8_t meta[0x80], aci[0x18];  // ACI0 (offset at META+0x70): the program id at +0x10
    if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4) &&
        !fseek(f, *(uint32_t *)(meta + 0x70), SEEK_SET) && fread(aci, 1, sizeof aci, f) == sizeof aci &&
        !memcmp(aci, "ACI0", 4))
        id = *(uint64_t *)(aci + 0x10);
    fclose(f);
    return id;
}

static bool game_ok(const char *exefs) {
    uint64_t id = program_id(exefs);
    return id == TITLE_EEVEE || id == TITLE_PIKACHU;
}

int game_build(const char *exefs) {
    char p[1100];
    snprintf(p, sizeof p, "%s/main", exefs);
    FILE *f = fopen(p, "rb");
    if (!f) return GAME_BUILD_OTHER;
    uint8_t id[8] = {0};
    fseek(f, 0x40, SEEK_SET);  // (NSO header: the module id)
    size_t n = fread(id, 1, 8, f);
    fclose(f);
    uint64_t pid = program_id(exefs);
    if (n == 8 && !memcmp(id, EEVEE_100, 8) && pid == TITLE_EEVEE) return GAME_BUILD_EEVEE_100;
    if (n == 8 && !memcmp(id, PIKACHU_100, 8) && pid == TITLE_PIKACHU) return GAME_BUILD_PIKACHU_100;
    if (n == 8 && !memcmp(id, EEVEE_102, 8) && pid == TITLE_EEVEE) return GAME_BUILD_EEVEE_102;
    return GAME_BUILD_OTHER;
}

const char *game_version(const char *exefs) {
    static const struct { uint8_t id[8]; const char *v; } k[] = {
        {{0x0a, 0x3f, 0x0d, 0x21, 0xe6, 0x7d, 0xad, 0x80}, "1.0.0"},  // Eevee
        {{0x1c, 0x37, 0x56, 0x92, 0xde, 0x4f, 0x4b, 0x87}, "1.0.0"},  // Pikachu
        {{0x58, 0x31, 0xec, 0x64, 0xd6, 0xb6, 0x96, 0xfd}, "1.0.2"},  // Eevee
        {{0xc2, 0x08, 0xdb, 0x6a, 0x4e, 0xf4, 0x36, 0x1f}, "1.0.2"},  // Pikachu (issue #1's log)
    };
    char p[1100];
    snprintf(p, sizeof p, "%s/main", exefs);
    uint8_t id[8] = {0};
    FILE *f = fopen(p, "rb");
    if (f) {
        fseek(f, 0x40, SEEK_SET);
        if (fread(id, 1, 8, f) != 8) memset(id, 0, 8);
        fclose(f);
    }
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (!memcmp(id, k[i].id, 8)) return k[i].v;
    return "1.0.0";  // (a build not listed here)
}

const char *game_name(const char *exefs) {
    uint64_t id = program_id(exefs);
    return id == TITLE_EEVEE ? "Eevee" : id == TITLE_PIKACHU ? "Pikachu" : NULL;
}

// (a romfs.bin that isn't a readable RomFS - cut short, or another dump tool's format - stopped the game at its first
// read with 2002-6063, issue #31: set up again instead)
static bool image_ok(void) {
    char why[128];
    return romfs_image_ok(ROOT "/romfs.bin", why, sizeof why);
}

bool setup_ready(void) {
    return game_ok(ROOT "/exefs") && image_ok() && gamelib_present(ROOT);
}

bool setup_needs_build(void) {
    return game_ok(ROOT "/exefs") && image_ok() && !gamelib_present(ROOT);
}

typedef struct { char exefs[1024], romfs[1024], romfs_bin[1024]; bool package; } Found;

static void find_game(const char *dir, int depth, Found *f) {
    if (depth > 5 || (f->exefs[0] && (f->romfs[0] || f->romfs_bin[0]))) return;
    char p[1100];
    snprintf(p, sizeof p, "%s/main.npdm", dir);
    if (!f->exefs[0] && is_file(p)) {
        snprintf(p, sizeof p, "%s/main", dir);
        if (is_file(p)) snprintf(f->exefs, sizeof f->exefs, "%s", dir);
    }
    snprintf(p, sizeof p, "%s/bin/message", dir);
    if (!f->romfs[0] && is_dir(p)) snprintf(f->romfs, sizeof f->romfs, "%s", dir);
    snprintf(p, sizeof p, "%s/romfs.bin", dir);
    if (!f->romfs_bin[0] && is_file(p)) snprintf(f->romfs_bin, sizeof f->romfs_bin, "%s", p);
    DIR *d = opendir(dir);
    if (!d) return;
    char sub[64][256];
    int nsub = 0;
    for (struct dirent *e; (e = readdir(d));) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;
        size_t len = strlen(n);
        if (len > 4) {
            char ext[5];
            for (int i = 0; i < 4; i++) ext[i] = (char)tolower((unsigned char)n[len - 4 + i]);
            ext[4] = 0;
            if (!strcmp(ext, ".xci") || !strcmp(ext, ".nsp") || !strcmp(ext, ".nca") || !strcmp(ext, ".nsz") ||
                !strcmp(ext, ".xcz"))
                f->package = true;
        }
        snprintf(p, sizeof p, "%s/%s", dir, n);
        if (nsub < 64 && strcmp(n, "bin") && is_dir(p)) snprintf(sub[nsub++], sizeof sub[0], "%s", n);
    }
    closedir(d);
    for (int i = 0; i < nsub; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, sub[i]);
        find_game(p, depth + 1, f);
    }
}

// progress: copying + the image are the first 15%, the game code the rest
static float g_phase0, g_phase1;
static void copy_progress(uint64_t done, uint64_t total) {
    if (total) g_progress = g_phase0 + (g_phase1 - g_phase0) * (float)((double)done / (double)total);
}
static void build_progress(double f, const char *step) {
    g_progress = 0.15f + 0.85f * (float)f;
    g_state = !strncmp(step, "Reading", 7) ? SETUP_READING : !strncmp(step, "Finishing", 9) ? SETUP_FINISHING
                                                                                           : SETUP_COMPILING;
}

static bool build_code(void) {
    if (!gamebuild_available()) { g_state = SETUP_FAIL_TOOLS; return false; }
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    GameBuildOpts o = {build_progress, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1};
    char why[256] = "";
    g_state = SETUP_READING;
    if (!gamebuild_run(ROOT, &o, why, sizeof why)) {
        LOG("[setup] game code: %s\n", why);
        g_state = SETUP_FAIL_BUILD;
        return false;
    }
    return true;
}

static void *setup_thread(void *arg) {
    (void)arg;
    g_progress = 0;
    if (!g_folder[0]) {  // only the game code (files set up already, e.g. a new version of the app)
        LOG("[setup] the game files are set up, the game code is from another version: rebuilding it\n");
        if (build_code()) g_state = setup_ready() ? SETUP_DONE : SETUP_FAIL_BUILD;
        g_running = false;
        return NULL;
    }
    g_state = SETUP_LOOKING;
    Found f = {0};
    find_game(g_folder, 0, &f);
    LOG("[setup] %s: exefs %s, romfs %s, romfs.bin %s\n", g_folder, f.exefs[0] ? f.exefs : "-", f.romfs[0] ? f.romfs : "-",
        f.romfs_bin[0] ? f.romfs_bin : "-");
    if (!f.exefs[0] || (!f.romfs[0] && !f.romfs_bin[0])) g_state = f.package ? SETUP_FAIL_PACKAGE : SETUP_FAIL_NOT_FOUND;
    else if (!game_ok(f.exefs)) g_state = SETUP_FAIL_VERSION;
    else {
        // (the picked folder can't be our own copy: setting up again deletes that first)
        char a[4096], b[4096];
        mkdir(ROOT, 0755);
        if (realpath(f.exefs, a) && realpath(ROOT, b) && !strncmp(a, b, strlen(b)))
            snprintf(f.exefs, sizeof f.exefs, "%s", ""), g_state = SETUP_FAIL_NOT_FOUND;
    }
    if (g_state >= SETUP_FAIL_NOT_FOUND) { g_running = false; return NULL; }

    rt_remove_game_files(ROOT);  // (an earlier or interrupted setup's)
    mkdir(ROOT, 0755);
    g_state = SETUP_COPYING;
    g_phase0 = 0, g_phase1 = 0.01f;
    bool ok = true;
    if (!strcmp(f.exefs, f.romfs)) {  // (a flat dump, main next to bin/: only the six code files, not the whole RomFS)
        static const char *const code[] = {"main", "main.npdm", "rtld", "sdk", "subsdk0", "subsdk1"};
        mkdir(ROOT "/exefs", 0755);
        for (size_t i = 0; ok && i < sizeof code / sizeof *code; i++) {
            char src[1100], dst[1100];
            snprintf(src, sizeof src, "%s/%s", f.exefs, code[i]);
            snprintf(dst, sizeof dst, ROOT "/exefs/%s", code[i]);
            ok = is_file(src) && romfs_copy_file(src, dst, copy_progress);
        }
    } else
        ok = romfs_copy_tree(f.exefs, ROOT "/exefs", copy_progress);
    if (ok && f.romfs_bin[0]) {
        g_phase0 = 0.01f, g_phase1 = 0.15f;
        ok = romfs_copy_file(f.romfs_bin, ROOT "/romfs.bin", copy_progress);
    } else if (ok) {
        g_state = SETUP_IMAGE;
        g_phase0 = 0.01f, g_phase1 = 0.15f;
        char why[256] = "";
        ok = romfs_build(f.romfs, ROOT "/romfs.bin", copy_progress, why, sizeof why);
        if (!ok) LOG("[setup] romfs image: %s\n", why);
    }
    char why[128];
    if (!ok) g_state = SETUP_FAIL_COPY;
    else if (!romfs_image_ok(ROOT "/romfs.bin", why, sizeof why)) {
        LOG("[setup] the game data image %s: %s\n", f.romfs_bin[0] ? f.romfs_bin : "(built)", why);
        g_state = SETUP_FAIL_IMAGE;
    } else if (build_code()) g_state = setup_ready() ? SETUP_DONE : SETUP_FAIL_BUILD;
    LOG("[setup] %s\n", g_state == SETUP_DONE ? "ready" : "failed");
    g_running = false;
    return NULL;
}

void setup_start(const char *folder) {
    if (g_running) return;
    snprintf(g_folder, sizeof g_folder, "%s", folder ? folder : "");
    for (char *c = g_folder; *c; c++) if (*c == '\\') *c = '/';
    g_running = true;
    g_state = SETUP_LOOKING;
    pthread_t t;
    pthread_create(&t, NULL, setup_thread, NULL);
    pthread_detach(t);
}
