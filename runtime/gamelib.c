// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "paths.h"
#include "rt.h"

static const GameLibTables *g_tables;
static char g_error[256];

const char *gamelib_cpu_level(void) {
#if defined(__x86_64__)
    static const char *level;
    if (!level) {
        __builtin_cpu_init();
        level = __builtin_cpu_supports("x86-64-v3") ? "v3" : "v2";
        const char *env = getenv("LARECOMP_GAME_CPU");
        if (env && !strcmp(env, "v2")) level = "v2";
    }
    return level;
#else
    return "base";
#endif
}

const char *gamelib_id(void) {
    static char id[64];
    if (!id[0]) snprintf(id, sizeof id, "%s-%s", GAMELIB_ID, gamelib_cpu_level());
    return id;
}

// The library's id file (<library>.id, written next to it by the build): its gamelib_id(), read without loading it.
static bool id_matches(const char *lib) {
    char path[1100], id[128] = {0};
    snprintf(path, sizeof path, "%s.id", lib);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0;
    return !strcmp(id, gamelib_id());
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The places to look, in order (gamelib.h). Returns how many were filled.
static int candidates(const char *game_dir, char out[3][1100]) {
    int n = 0;
    const char *env = getenv("LARECOMP_GAME_LIB");
    if (env && *env) snprintf(out[n++], 1100, "%s", env);
    char exe_side[1024];
    if (rt_resource(GAMELIB_NAME, NULL, exe_side, sizeof exe_side) && exists(exe_side))
        snprintf(out[n++], 1100, "%s", exe_side);
    if (game_dir) snprintf(out[n++], 1100, "%s/%s", game_dir, GAMELIB_NAME);
    return n;
}

bool gamelib_present(const char *game_dir) {
    if (g_tables) return true;
    char c[3][1100];
    int n = candidates(game_dir, c);
    const char *env = getenv("LARECOMP_GAME_LIB");
    for (int i = 0; i < n; i++)  // (LARECOMP_GAME_LIB: taken as given, as gamelib_load does; its own id is checked there)
        if (exists(c[i]) && ((i == 0 && env && *env) || id_matches(c[i]))) return true;
    return false;
}

static const GameLibTables *open_lib(const char *path) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(g_error, sizeof g_error, "can't load %s (error %lu)", path, GetLastError()); return NULL; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(g_error, sizeof g_error, "%s", dlerror()); return NULL; }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(g_error, sizeof g_error, "%s: not game code", path); return NULL; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || strcmp(t->id, gamelib_id())) {
        snprintf(g_error, sizeof g_error, "%s was built for another version (library: abi %u id %s; this runtime: abi %u id %s)", path,
                 t ? t->abi : 0, t && t->id ? t->id : "-", (unsigned)GAMELIB_ABI, gamelib_id());
        return NULL;  // (left loaded: nothing of it runs)
    }
    return t;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    static bool tried;
    if (!tried) {
        tried = true;
        snprintf(g_error, sizeof g_error, "the game code hasn't been built: set up the game files on the start screen");
        char c[3][1100];
        int n = candidates(game_dir, c);
        const char *env = getenv("LARECOMP_GAME_LIB");
        for (int i = 0; i < n && !g_tables; i++) {
            bool given = i == 0 && env && *env;  // LARECOMP_GAME_LIB: loaded as given, the library's own id decides
            if (!exists(c[i])) {
                if (given) { snprintf(g_error, sizeof g_error, "LARECOMP_GAME_LIB: no file %s", c[i]); break; }
                continue;
            }
            if (!given && !id_matches(c[i])) {
                snprintf(g_error, sizeof g_error, "%s was built for another version", c[i]);
                continue;
            }
            g_tables = open_lib(c[i]);
            if (!g_tables) LOG("[gamelib] %s\n", g_error);
            // (the library asked for by name doesn't load: stop there. Falling back to the next candidate ran another
            // game's code - Eevee's build/lasgame.dll with Pikachu's files - and crashed far from the cause)
            if (!g_tables && given) break;
            if (g_tables)
                for (uint32_t m = 0; m < g_tables->nmods; m++)
                    LOG("[gamelib] %s: %s, %llu functions\n", c[i], g_tables->mods[m].name,
                        (unsigned long long)*g_tables->mods[m].count);
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
