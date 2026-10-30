// 60 fps (experimental): the world and the menus at 60, battles and captures as the game made them (30). 120 (more
// experimental): the world at 120 the same way (G[1] = 8.33 ms), menus still 30 with the in-between image (60).
// The F12 menu switches the rate while the game runs (fps60_present, on the game thread: hooks added then).
//
// The game keeps a table of frame durations G (main+0xf7eeb8: 16.67 ms, 33.33 ms, 50 ms, ...; read through the pointer
// at main+0x15fb438). The world's update step comes from G[1] through the accessor f_7000039600; menus, battles and
// captures count their own frames, so at 60 fps they run twice as fast (docs/PESQUISA-60FPS.md). A community "60 FPS"
// patch changes G[1] for every reader; here:
//  - the animation systems take "elapsed time" as a fixed 33.33 ms per update and divide it by G[1] to get frames: the
//    time the accessor hands out is that elapsed time, so with 16.67 ms there they advance half a frame per update at
//    60 fps - right, by time (menus, the world's animations);
//  - battles and captures also count integer frames of their own: they keep the game's 33.33 ms and 30 fps (the entry
//    points of both scenes, mapped in docs/MODO-BATALHA.md, switch to it until the world's update runs again).
// Scenes move from the 30 fps side to the 60 fps side one at a time, once their frame counters are fixed.
//
// LARECOMP_FPS60=1 turns it on (settings: fps = 60). LARECOMP_G1_READERS="<addr>[,<addr>...]" (research): G[1] is
// 16.67 ms only while those functions run, to find which readers a speed comes from.
#include <stdatomic.h>
#include <stdbool.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));  // runtime/fnhook.c
void rt_call_original(Cpu *c, uint64_t addr);
uint64_t renderer_game_frame(void);

#define G1_ADDR 0x7000f7eec0ull  // G[1]: 33333334 ns
#define G1_ORIGINAL 33333334u
#define G1_60HZ 16666667u
static uint32_t g_g1 = G1_60HZ;  // the world's step: 16.67 ms (60 fps) or 8.33 ms (120)
// The step for a rate, rounded up as the game's own 33333334 is: the animation timers add rate x step in float, and a
// step a hair short (1e9 / 60 = 16666666 ns) left them just below a whole frame - 39.99999 instead of 40 - so a looping
// one turned over one update late, and on that update the walking animation added nothing to the player's position: a
// step lost, then a double one, once per stride (the world's "little jumps" walking at 60 fps; 30 fps lands on 40).
static uint32_t g1_for(int rate) { return (uint32_t)((1000000000u + (uint32_t)rate - 1) / (uint32_t)rate); }
static int g_rate = 60;
#define FIELD_UPDATE 0x70002f8bb0ull  // the world's update, every frame while it runs
#define G_ACCESSOR 0x7000039600ull    // G[index]: the world's step comes through here

static bool g_on;
static bool g_world = true;             // 60 fps now (not inside a battle or a capture)
static bool g_all60;                    // LARECOMP_FPS60=2
static bool g_special;                  // a battle or a capture started and the world hasn't updated since
#define ENCOUNTER_SCENE 0x700028ef70ull  // creates the capture scene (x0 = the encounter object)
#define BATTLE_START 0x700028bb60ull     // copies the battle's configuration

static long g_scene_start = -1;  // research: the present a capture began in (LARECOMP_SHOT_REL)
long fps60_scene_start(void) { return g_scene_start; }
static int scene_start_hook(Cpu *c, uint64_t addr) {
    if (g_scene_start < 0) {
        g_scene_start = (long)renderer_game_frame();
        LOG("[fps60] scene start %ld (f_%llx)\n", g_scene_start, (unsigned long long)addr);
    }
    (void)c, (void)addr;
    return 0;
}

static bool g_scene;  // inside a battle or a capture (until the world updates again)

// The frame counters a scene's timeline runs on (tools/patches.py) step on every other update at 60 fps: 1 = step now.
// Research (LARECOMP_FPS60_STATS=1): once a second, what the game did: presents, world updates (FIELD_UPDATE), reads of G through
// the accessor, and how many times the scene counters (tools/patches.py) were offered a step / allowed to take it.
static bool g_stats;
static unsigned long g_n_world, g_n_acc, g_n_tick_ask, g_n_tick_do;
int rt_scene_tick(void) {
    int r = !(g_all60 && g_scene && (renderer_game_frame() & 1));
    g_n_tick_ask++, g_n_tick_do += r;
    return r;
}

// Every battle / capture and the world's return after it, in the log ("[scene]"; tools/capture_loop.py reads them) and for
// the scripted input (LARECOMP_INPUT_LOOP holds still for a while after the world comes back: input.c)
static bool g_mark_scene;
static long g_world_back = -1;
long fps60_world_back(void) { return g_world_back; } bool fps60_in_scene(void) { return g_mark_scene; }
static int scene_hook(Cpu *c, uint64_t addr) {
    scene_start_hook(c, addr);  // (one hook per address: this one also marks the start for LARECOMP_SHOT_REL)
    if (!g_mark_scene) { g_mark_scene = true; LOG("[scene] start %llu (f_%llx)\n", (unsigned long long)renderer_game_frame(), (unsigned long long)addr); }
    g_scene = true;
    if (g_all60) return 0;  // LARECOMP_FPS60=2: battles and captures at 60 too (their counters halved)
    g_special = true;
    (void)c, (void)addr;
    return 0;
}

// A battle the game starts without BATTLE_START: the wild battles mode builds it through the static battles' process
// (runtime/hle/wildbattle.c), which never calls it. Without this it counted as a menu and got in-between images.
void fps60_scene_begin(void) {
    g_scene = true;
    if (!g_all60) g_special = true;
}

static bool g_field_ran;  // the world's update ran since the last present
static bool g_played;     // the world has updated at least once (before: the controller screen, the title, loading)
static int field_hook(Cpu *c, uint64_t addr) {
    g_n_world++;
    if (g_mark_scene) { g_mark_scene = false; g_world_back = (long)renderer_game_frame(); LOG("[scene] world again %ld\n", g_world_back); }
    g_field_ran = true;
    if (!g_played) LOG("[scene] the world's first update at present %llu (the title screen is over)\n", (unsigned long long)renderer_game_frame());
    g_played = true;
    g_special = false;
    g_scene = false;
    (void)c, (void)addr;
    return 0;  // (the game's function runs as usual)
}

// The frame-step timers (the accessor's x0): entries of 16 bytes, step i at +0x10 + 16 i (setter f_7000039620)
static uint64_t g_timers;
uint64_t fps60_timers(void) { return g_timers; }  // (runtime/hle/fps60_research.c)

// G[1] at 60 Hz for the duration of f: the world's step; any other scene keeps the game's value.
static int accessor_hook(Cpu *c, uint64_t addr) {
    volatile uint32_t *g1 = (volatile uint32_t *)(uintptr_t)GA(G1_ADDR);
    g_n_acc++;
    g_timers = c->x[0];
    if (!g_on || !g_world) return 0;
    *g1 = g_g1;
    rt_call_original(c, addr);
    *g1 = G1_ORIGINAL;
    return 1;
}

static int g1_reader_hook(Cpu *c, uint64_t addr) {
    volatile uint32_t *g1 = (volatile uint32_t *)(uintptr_t)GA(G1_ADDR);
    *g1 = G1_60HZ;
    rt_call_original(c, addr);
    *g1 = G1_ORIGINAL;
    return 1;
}

// Research (LARECOMP_FPS60_LOG=1): the frame-rate setting object the animation systems index G with (main+0x15fab08
// holds a pointer to a pointer to it; +0x88 is the index), once a second.
static void log_index(void) {
    static uint64_t last;
    uint64_t f = renderer_game_frame();
    if (f - last < 60) return;
    last = f;
    const uint64_t *slot = (const uint64_t *)(uintptr_t)GA(0x7000000000ull + 0x15fab08);
    const uint64_t *obj = (const uint64_t *)(uintptr_t)*slot;
    if (!obj || (uint64_t)(uintptr_t)obj < GUEST_LO) return;
    const uint64_t *real = (const uint64_t *)(uintptr_t)*obj;
    if (!real || (uint64_t)(uintptr_t)real < GUEST_LO) return;
    LOG("[fps60] present %llu: frame-rate index %d (G[idx] = %u)\n", (unsigned long long)f, *(const int32_t *)((const uint8_t *)real + 0x88),
        *(const uint32_t *)(uintptr_t)(GA(0x7000f7eeb8ull) + 8ull * (uint32_t)*(const int32_t *)((const uint8_t *)real + 0x88)));
}

bool fps60_enabled(void) { return g_on; }
int fps60_rate(void) { return g_rate; }  // the world's frame rate while on: 60 or 120
// The duration of a frame now (runtime/nvn/objects.c pacing, output_scale.c dynamic resolution): the world's at its
// rate, the rest at the game's 30
uint64_t fps60_period_ns(void) { return g_on && g_world ? 1000000000ull / (uint64_t)g_rate : 33333333ull; }
bool fps60_world(void) { return g_world; }
// A menu (the Pokédex, the bag...): the world stopped and no battle or capture runs. Logic at 30, shown at 60 with the
// in-between image (runtime/nvn/vulkan/exec.c); battles and captures get none (tried, left out: docs/HISTORICO.md 46).
bool fps60_menu(void) { return g_on && g_played && !g_world && !g_special; }
// Before the world's first update this boot: the logos, the title screen (the logo with Pikachu or Eevee), loading
// (runtime/nvn/vulkan/exec.c: SSAO at half its intensity, as in cutscenes)
bool game_title(void) { return !g_played; }

// At each present (before its pacing): was this a world frame?
void fps60_probe_present(void);
int rt_present_interval(void);
// ---- the player's walking at 60 / 120 fps
// The player moves by the walking animation's root motion (time-based: right at any rate) plus, once per world update,
// an offset of its own (MOVE_FN: position += facing x a vector x one of the movement component's parameters +0x70 /
// +0x74 / +0x78, by the stick's state - -2 cm at a run, sized for a 33.3 ms update). Per update, not per second: at 30
// fps it takes 60 cm/s off the animation's 450.8 (391 cm/s), at 60 fps 120 (331), at 120 fps 240 (211) - the player
// walked at 85 % and 53 % of the speed (measured with tools/walk_speed.sh; found with LARECOMP_WATCH_LIKE / memwatch.c:
// the writes to the player's position, in order). The same function turns the player toward the stick with a slerp
// (SLERP_FN) of t = 0.5 per update: twice / four times as fast at 60 / 120. While it runs in the world at 60 / 120 fps,
// the parameters are scaled by the step's share of 33.3 ms and the slerp's t becomes 1 - (1 - t)^share: per second,
// what the game does at 30. Eevee 1.0.0's addresses (Pikachu's: runtime/fnhook.c, tools/addrmap.py).
#define MOVE_FN 0x700021b720ull
#define SLERP_FN 0x700020b810ull
static __thread bool t_in_move;
static __thread float t_move_k;
static int move_hook(Cpu *c, uint64_t addr) {
    float k = g_on && g_world ? (float)g_g1 / (float)G1_ORIGINAL : 1.0f;
    if (k >= 0.999f || c->x[0] < GUEST_LO) return 0;  // (the game's own step: as it is)
    float *p = (float *)(uintptr_t)(c->x[0] + 0x70), was[3] = {p[0], p[1], p[2]};
    for (int i = 0; i < 3; i++) p[i] = was[i] * k;
    t_in_move = true, t_move_k = k;
    rt_call_original(c, addr);
    t_in_move = false;
    for (int i = 0; i < 3; i++) p[i] = was[i];
    return 1;
}
static int slerp_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!t_in_move) return 0;
    float t;
    memcpy(&t, &c->v[0], 4);
    if (t > 0 && t < 1) {
        t = 1.0f - powf(1.0f - t, t_move_k);
        memcpy(&c->v[0], &t, 4);
    }
    return 0;  // (the slerp runs with that t)
}
static bool hooked;
static void hook_all(void) {
    if (hooked) return;
    hooked = true;
    rt_register_fnhook(FIELD_UPDATE, field_hook);
    rt_register_fnhook(ENCOUNTER_SCENE, scene_hook);
    rt_register_fnhook(BATTLE_START, scene_hook);
    rt_register_fnhook(G_ACCESSOR, accessor_hook);
    if (!getenv("LARECOMP_FPS60_NOWALKFIX")) {
        extern uint64_t rt_addr(uint64_t eevee);
        rt_register_fnhook(MOVE_FN, move_hook), rt_register_fnhook(SLERP_FN, slerp_hook);
        LOG("[fps60] walking fix: movement f_%llx, turning f_%llx\n", (unsigned long long)rt_addr(MOVE_FN), (unsigned long long)rt_addr(SLERP_FN));
    }
}

// The F12 menu's frame rate (g_settings.fps), followed at each present (LARECOMP_FPS60 overrides it)
static void follow_setting(void) {
    if (getenv("LARECOMP_FPS60")) return;
    extern uint64_t rt_addr(uint64_t eevee);
    bool on = g_settings.fps >= 60 && rt_addr(FIELD_UPDATE);
    int rate = g_settings.fps == 120 ? 120 : 60;
    if (on == g_on && rate == g_rate) return;
    if (on) hook_all();
    if (on && !g_on) g_world = true, g_special = g_scene = false;  // (g_played: kept - the world has run already or not)
    g_on = on, g_rate = rate, g_g1 = g1_for(rate);
    LOG("[fps60] %s (from present %llu)\n", on ? (rate == 120 ? "120 fps" : "60 fps") : "off: 30 fps",
        (unsigned long long)renderer_game_frame());
}

void fps60_present(void) {
#ifdef _WIN32
    { extern void memwatch_present(void); memwatch_present(); }  // (research: LARECOMP_WATCH_LIKE)
#endif
    follow_setting();
    fps60_probe_present();
    if (g_stats) {
        static uint64_t t0, p0;
        extern uint64_t plat_now_ns(void);
        uint64_t now = plat_now_ns(), p = renderer_game_frame();
        if (!t0) t0 = now, p0 = p;
        if (now - t0 >= 1000000000ull) {
            LOG("[stats] t+%.2fs presents %llu world %lu acc %lu tick_ask %lu tick_do %lu scene %d interval %d G1 %u\n", (now - t0) / 1e9,
                (unsigned long long)(p - p0), g_n_world, g_n_acc, g_n_tick_ask, g_n_tick_do, g_scene, rt_present_interval(), *(volatile uint32_t *)(uintptr_t)GA(G1_ADDR));
            t0 = now, p0 = p, g_n_world = g_n_acc = g_n_tick_ask = g_n_tick_do = 0;
        }
    }
    if (getenv("LARECOMP_FPS60_LOG")) log_index();
    if (!g_on) return;
    // 60 only while the world itself updates: menus, the Pokédex and other screens that stop it count their own frames
    // and ran twice as fast (measured: the menu opened in 4 game frames instead of 8)
    static int menu_native = -1;  // (research, LARECOMP_FPS60_MENU_NATIVE=1: menus at 60 natively, as before HISTORICO 47)
    if (menu_native < 0) menu_native = getenv("LARECOMP_FPS60_MENU_NATIVE") != NULL;
    bool w = !g_special && (g_field_ran || menu_native);
    g_field_ran = false;
    g_world = w;
    static int last = -1;  // 0 world, 1 menu (in-between images), 2 battle / capture
    int state = w ? 0 : fps60_menu() ? 1 : 2;
    static const char *const name[] = {"world: %d fps", "menu: 30 fps with in-between images", "battle / capture: 30 fps"};
    if (state != last) {
        char what[64];
        snprintf(what, sizeof what, name[state], g_rate);
        LOG("[fps60] %s from present %llu\n", what, (unsigned long long)renderer_game_frame());
    }
    last = state;
}

// Before each boot (runtime/main.c): the launcher's "60 fps (experimental)" (larecomp.ini fps = 60) turns the mode on
// when LARECOMP_FPS60 doesn't say otherwise.
void fps60_configure(void) {
    extern void settings_load(void);
    settings_load();  // (read once; without the launcher it would come later, in boot)
    if (!getenv("LARECOMP_FPS60")) g_on = g_settings.fps >= 60, g_all60 = false;
    g_rate = g_settings.fps == 120 ? 120 : 60, g_g1 = g1_for(g_rate);
    extern uint64_t rt_addr(uint64_t eevee);  // (runtime/fnhook.c: 0 in a build it doesn't know)
    if (!rt_addr(FIELD_UPDATE)) g_on = g_all60 = false;
    if (g_on || g_stats || getenv("LARECOMP_INPUT_LOOP")) hook_all();
    // the world's update always: g_played tells the title screen (game_title) whatever the frame rate
    else if (rt_addr(FIELD_UPDATE)) rt_register_fnhook(FIELD_UPDATE, field_hook);
    g_world = true, g_special = g_scene = g_played = false;
    if (g_on) LOG("[fps60] on: the world at %d, menus at 30 with in-between images, battles and captures at 30\n", g_rate);
}

__attribute__((constructor)) static void fps60_init(void) {
    const char *e = getenv("LARECOMP_FPS60");
    g_on = e && atoi(e);
    g_all60 = e && atoi(e) == 2;
    g_stats = getenv("LARECOMP_FPS60_STATS") != NULL;  // (the hooks: fps60_configure)
    if (!g_on && (getenv("LARECOMP_SHOT_REL") || getenv("LARECOMP_WINDOW_SHOTS_REL"))) { rt_register_fnhook(ENCOUNTER_SCENE, scene_start_hook); rt_register_fnhook(BATTLE_START, scene_start_hook); }
    e = getenv("LARECOMP_G1_READERS");
    char buf[512];
    if (!e) return;
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) rt_register_fnhook(strtoull(t, NULL, 16), g1_reader_hook);
}
