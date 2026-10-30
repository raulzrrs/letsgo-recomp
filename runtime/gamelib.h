// The recompiled game code: a library built from the player's own game files (`make gen` + `make` in a source
// checkout, the start screen's setup otherwise; runtime/gamebuild.c) and loaded at startup (runtime/gamelib.c).
// Nothing from the game ships with the app.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#if defined(_WIN32)
#define GAMELIB_NAME "lasgame.dll"
#define GAMELIB_EXPORT __declspec(dllexport)
#elif defined(__APPLE__)
#define GAMELIB_NAME "lasgame.dylib"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#elif defined(__ANDROID__)
#define GAMELIB_NAME "liblasgame.so"  // (in the APK's native libraries, which must be named lib*.so)
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#else
#define GAMELIB_NAME "lasgame.so"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#endif

// GAMELIB_ID (-D, from the Makefile): a hash of the generator and the headers the game code is compiled against. A
// library's own id is that plus "-" and the CPU level it was compiled for (gamelib_id()); one built with another id is
// rebuilt, not loaded. (tools/gamelib_entry.c gets the full id as its GAMELIB_ID.)
#ifndef GAMELIB_ID
#define GAMELIB_ID "dev"
#endif
#define GAMELIB_ABI 3  // (3: FN_ENTER calls the function hooks, runtime/fnhook.c)
#define GAMELIB_MAX_MODULES 4

// One table per recompiled module, in load order (main, sdk, subsdk0, subsdk1: runtime/loader.c).
typedef struct {
    const char *name;
    const uint64_t *addrs;  // sorted guest addresses
    const GuestFn *fns;     // the functions at them
    const uint64_t *count;
} GameLibModule;

typedef struct {
    uint32_t abi;    // GAMELIB_ABI
    const char *id;  // GAMELIB_ID
    uint32_t nmods;
    GameLibModule mods[GAMELIB_MAX_MODULES];
} GameLibTables;

// The library's one exported function (tools/gamelib_entry.c).
typedef const GameLibTables *(*GameLibEntry)(void);
#define GAMELIB_ENTRY "lasgame_tables"

#ifdef __cplusplus
extern "C" {
#endif

// Loads the game code for the game files in game_dir (once; later calls return the first result). Looks at
// LARECOMP_GAME_LIB, next to the executable (a source checkout's build/), then game_dir. false: why says what's wrong.
bool gamelib_load(const char *game_dir, char *why, size_t why_size);
// The tables of the loaded library (NULL before gamelib_load succeeds).
const GameLibTables *gamelib_tables(void);
// True when a library usable with this build is in one of those places (without loading it).
bool gamelib_present(const char *game_dir);
// The CPU level the game code is built for on this machine: "v3" (x86-64-v3: AVX2, FMA, BMI2) when the CPU has it, else
// "v2" (x86-64-v2: SSE4.2, slower); LARECOMP_GAME_CPU=v2 asks for v2 on a v3 CPU. "base" off x86-64 (no level).
const char *gamelib_cpu_level(void);
// The id a library must carry to be loaded here: GAMELIB_ID "-" gamelib_cpu_level().
const char *gamelib_id(void);

#ifdef __cplusplus
}
#endif
