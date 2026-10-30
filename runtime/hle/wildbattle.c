// Wild battles (experimental, docs/MODO-BATALHA.md): touching a wild Pokémon starts a battle (the game's "static
// encounter" process: Snorlax, the legendary birds, Mewtwo) instead of the Pokémon GO-style capture.
//
// How the game does a static battle (docs/MODO-BATALHA.md, stage 3c): a script command builds a process of class A
// (factory f_700017f0a0, constructor f_7000289ac0, object of 0xA8 bytes) whose [+0x98] holds the battle's 64-bit hash;
// its state machine loads bin/script_event_data/event_encount.bin (0x40-byte records), looks the hash up
// (f_70002bad00(table, &key) -> parsed record of 0x59 bytes), fills the battle configuration from the record
// (f_70002f60c0) and starts the battle. Here:
//  - the capture's entry point (f_700028ef70: x0 = the process manager, x1 = the allocator) builds a class A process
//    through that factory instead of the capture scene;
//  - the constructor hook then writes our own hash in [+0x98];
//  - the lookup hook answers that hash with a record of our own: Snorlax's real record as the mould (so everything the
//    game keeps in it stays valid) with the species, level and moves replaced.
// Catching during the battle (stage 6, below): the menu's fifth button, CATCH (runtime/hle/catch_button.c), ends the
// battle into the game's own capture with a catch rate raised by the wild Pokémon's HP and status, one ball at a time
// (a throw that doesn't hold brings the battle back, HP and status kept); a wild Pokémon that faints is lost.
// LARECOMP_WILDBATTLE=1 turns it on. Test settings (until the touched Pokémon is read): LARECOMP_WILDBATTLE_SPECIES=<n>
// (default 19), _LEVEL=<n> (3), _MOVES=<a,b,c,d> (33,45,0,0), _MAX=<n> (battles per run, default 1), _A6=1 (the "mode 3"
// flag of the process), _STATE_LOG=1 (log the process' state changes); the catch's: docs/DEPURACAO.md.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../hle.h"
#include "../settings.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));  // runtime/fnhook.c
void rt_call_original(Cpu *c, uint64_t addr);
uint64_t rt_addr(uint64_t eevee);  // runtime/fnhook.c: an Eevee 1.0.0 address in the running build (0: unknown build)
uint64_t renderer_game_frame(void);

#define ENCOUNTER_TRIGGER 0x7000305ad0ull  // a touched wild Pokémon (x0 = it, x1 -> the world's object): starts the capture
#define GOT_ALLOCATOR 0x70015faab8ull      // GOT slot -> pointer to the object holding the allocator at +0x68
#define CLASS_A_DTOR0 0x7000289f60ull
#define CLASS_A_DTOR1 0x7000289fd0ull
#define STATIC_FACTORY 0x700017f0a0ull  // (x0 manager, x1 allocator, x8 -> out handle): creates a class A process
#define STATIC_CTOR 0x7000289ac0ull     // class A constructor (x0 = the object)
#define TABLE_LOOKUP 0x70002bad00ull    // (x0 table, x1 -> u64 key) -> parsed record
#define SNORLAX_HASH 0x343e35139d576e27ull
#define WILD_HASH 0x57494c4442415454ull  // "WILDBATT": ours

static bool g_pending;  // the next class A constructor is ours
static int g_count, g_max = 1;
static int g_species = 19, g_level = 3, g_moves[4] = {33, 45, 0, 0};
static int g_a6;

#define R32(p, o) (*(volatile uint32_t *)((uintptr_t)(p) + (o)))
#define R8(p, o) (*(volatile uint8_t *)((uintptr_t)(p) + (o)))
#define R64(p, o) (*(volatile uint64_t *)((uintptr_t)(p) + (o)))

// The encounter trigger replaced whole: the game's own one starts the capture scene (f_700028ef70) and then keeps
// configuring the handle it gets back, which a battle process can't take. x1 -> [x1] -> [+0x68] is the process manager,
// [[*GOT 0x70015faab8]+0x68] the allocator (both as in the game's call to f_700028ef70).
static bool g_active;  // a static battle process of ours is alive: the touch that keeps calling the trigger is swallowed
// Catching during the battle (stage 6, below): what the player asked for and the wild Pokémon's state when the battle ended
static struct {
    bool requested;  // the player asked to catch: the run away that ends the battle is a catch
    bool catching;   // the capture after our battle is running (its catch rate is ours)
    bool have;       // hp / max_hp / status are the wild Pokémon's, from the battle that just ended
    unsigned hp, max_hp, status;  // status: pml's 1 paralysis, 2 sleep, 3 freeze, 4 burn, 5 poison (0 none)
    int choices;     // menu choices in this battle (LARECOMP_WILDBATTLE_AUTOCATCH)
    bool chose;      // the menu was answered in this battle (the quick opening's end)
    bool ended;      // this battle's end (the battle event's state 5) was handled
    bool caught;     // a throw of this capture held (its shake checks: 4)
    int throws;      // throws judged in this capture
    bool rebattle;   // the capture ended without the Pokémon: the battle starts again with its HP and status
    bool their_turn; // ... after a ball that broke: the throw was the player's turn, the wild Pokémon moves first
    uint8_t status_raw[5][8];  // the battle's own status blocks (+0xa0.., below), put back in the next battle
} g_catch;
static uint64_t g_mgr, g_alloc;  // the process manager and allocator of the touch that started the battle (again: the same)

// The touched Pokémon's own battle/capture configuration: the object (0x2b8 bytes) that f_70003058f0 builds from the wild Pokémon (species, form, level, nature,
// IVs, shiny...); the battle's configuration is the same kind of object, filled by f_70002f60c0 from a table record. So
// instead of trusting a hand-made record, the trigger keeps a copy of the real configuration and the hook on f_70002f60c0
// lays it over the record-built one (keeping the word at +0x298, which that function sets to 1 for a static battle).
// LARECOMP_WILDBATTLE_USECFG=0 keeps the synthetic record (species / level from LARECOMP_WILDBATTLE_*).
#define CFG_SIZE 0x2b8
#define CONFIG_FROM_RECORD 0x70002f60c0ull  // (x0 = configuration, x1 = record)
static uint8_t g_cfg[CFG_SIZE] __attribute__((aligned(16)));
static bool g_cfg_valid, g_usecfg;  // (off: copying the builder's configuration crashed; see docs/MODO-BATALHA.md)
static uint8_t g_rec[0x60] __attribute__((aligned(16)));  // (the synthetic record, below)

#define CONFIG_BUILD 0x70002f65d0ull         // (x0 = configuration object, x1 = allocator, x2 = struct, x8 = out): fills it
#define WILD_BUILDER_RETURN 0x7000305a98ull  // the return address of its call inside f_70003058f0, the wild Pokémon's builder
// f_70003058f0 runs in the same update as the trigger, right before it, so the last configuration it built is the touched
// Pokémon's: keep a copy (the species is the word at +0x58, as in the record-built one).
// Research (LARECOMP_WILDBATTLE_LOGPOKE=1): the wild builder f_70003058f0 gets x1 -> holder -> P, the Pokémon's own object.
// Log what the game reads from it (species f_70002185d0, form f_70002186a0, and the raw fields the builder copies), to
// compare with what the capture screen shows (species name, level).
#define WILD_BUILDER 0x70003058f0ull
// What the builder reads from the touched Pokémon's object P (docs/MODO-BATALHA.md): species f_70002185d0(P), form
// f_70002186a0(P), level = byte P+0xf0c, IVs = six bytes P+0xf18, +0xf1c, ... +0xf2c, gender = word P+0x40c (0 male, 1 female,
// else none: the builder turns it into 1 / 2 / 0, the record's fixed-gender codes).
static bool g_wild_valid;
static uint32_t g_wild_species, g_wild_form, g_wild_level, g_wild_gender;
static int8_t g_wild_iv[6];
static bool g_explicit;  // LARECOMP_WILDBATTLE_SPECIES given: that one, whatever was touched
static bool g_moves_given;  // LARECOMP_WILDBATTLE_MOVES given: those moves, not the level-up ones
static int logpoke_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t holder = c->x[1], p = R64(holder, 0);
    uint64_t save[32], lr = c->x[30], sp = c->sp;
    memcpy(save, c->x, sizeof save);
    c->x[0] = p;
    rt_call_original(c, 0x70002185d0ull);
    uint64_t species = c->x[0] & 0xffff;
    c->x[0] = p;
    rt_call_original(c, 0x70002186a0ull);
    uint64_t form = c->x[0] & 0xff;
    memcpy(c->x, save, sizeof save);
    c->x[30] = lr;
    c->sp = sp;
    g_wild_species = (uint32_t)species;
    g_wild_form = (uint32_t)form;
    g_wild_level = R8(p, 0xf0c);
    g_wild_gender = R32(p, 0x40c) == 0 ? 1 : R32(p, 0x40c) == 1 ? 2 : 0;
    for (int i = 0; i < 6; i++) g_wild_iv[i] = (int8_t)R8(p, 0xf18 + 4 * i);
    g_wild_valid = true;
    if (!getenv("LARECOMP_WILDBATTLE_LOGPOKE")) return 0;
    LOG("[wild] f%llu builder: P %llx species %llu form %llu | +f0c %u +f10 %u +f12 %u | +f18..f2c %d %d %d %d %d %d | +40c %u +410 %u\n",
        (unsigned long long)renderer_game_frame(), (unsigned long long)p, (unsigned long long)species, (unsigned long long)form,
        R8(p, 0xf0c), (unsigned)(*(volatile uint16_t *)(uintptr_t)(p + 0xf10)), (unsigned)(*(volatile uint16_t *)(uintptr_t)(p + 0xf12)),
        (int8_t)R8(p, 0xf18), (int8_t)R8(p, 0xf1c), (int8_t)R8(p, 0xf20), (int8_t)R8(p, 0xf24), (int8_t)R8(p, 0xf28), (int8_t)R8(p, 0xf2c),
        R32(p, 0x40c), R8(p, 0x410));
    if (atoi(getenv("LARECOMP_WILDBATTLE_LOGPOKE")) >= 2) {   // the whole object, to find where the level lives by comparing with the capture screen (build/poke_<present>.bin)
        char path[96];
        snprintf(path, sizeof path, "build/poke_%llu.bin", (unsigned long long)renderer_game_frame());
        FILE *f = fopen(path, "wb");
        if (f) { fwrite((const void *)(uintptr_t)p, 1, 0x1100, f); fclose(f); }
    }
    return 0;  // (the builder runs)
}

static int build_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != rt_addr(WILD_BUILDER_RETURN)) return 0;  // (another builder: the game's own)
    uint64_t cfg = c->x[0], lr = c->x[30];
    rt_call_original(c, addr);
    c->x[30] = lr;
    memcpy(g_cfg, (const void *)(uintptr_t)cfg, CFG_SIZE);
    g_cfg_valid = true;
    LOG("[wild] f%llu wild Pokémon's configuration %llx kept: species word +0x58 = %u, +0x54 = %u\n",
        (unsigned long long)renderer_game_frame(), (unsigned long long)cfg, *(const uint32_t *)(g_cfg + 0x58), *(const uint32_t *)(g_cfg + 0x54));
    return 1;
}

static int config_hook(Cpu *c, uint64_t addr) {
    uint64_t cfg = c->x[0], rec = c->x[1];
    if (rec != (uint64_t)(uintptr_t)g_rec) return 0;  // (a real record: the game's own)
    uint64_t lr = c->x[30];
    rt_call_original(c, addr);  // the configuration from our synthetic record
    c->x[30] = lr;
    if (g_usecfg && g_cfg_valid) {
        uint8_t from_rec[CFG_SIZE];
        memcpy(from_rec, (const void *)(uintptr_t)cfg, CFG_SIZE);
        int n = 0;
        for (int o = 0; o < CFG_SIZE; o += 4) {
            if (memcmp(from_rec + o, g_cfg + o, 4) && n++ < 90)
                LOG("[wild]   cfg +%03x: record-built %08x, real %08x\n", o, *(const uint32_t *)(from_rec + o), *(const uint32_t *)(g_cfg + o));
        }
        LOG("[wild] real configuration laid over the record's (%d words differ)\n", n);
        uint32_t keep = *(const uint32_t *)(from_rec + 0x298);
        memcpy((void *)(uintptr_t)cfg, g_cfg, CFG_SIZE);
        *(uint32_t *)(uintptr_t)(cfg + 0x298) = keep;
    }
    return 1;
}

// Research: the game's own species / form accessors (f_70003058f0 reads them from [x22] to build the capture's
// configuration), tried on the objects the trigger gets, to find which one is the touched Pokémon's data.
static void probe_identity(Cpu *c) {
    uint64_t cand[3] = {c->x[0], c->x[0] - 0x48, R64(c->x[2], 0)};
    const char *name[3] = {"x0", "x0-0x48", "*x2"};
    uint64_t lr = c->x[30], sp = c->sp, save[32];
    memcpy(save, c->x, sizeof save);
    for (int i = 0; i < 3; i++) {
        c->x[0] = cand[i];
        rt_call_original(c, 0x70002185d0ull);
        uint64_t species = c->x[0] & 0xffff;
        c->x[0] = cand[i];
        rt_call_original(c, 0x70002186a0ull);
        uint64_t form = c->x[0] & 0xff;
        LOG("[wild]   probe %-8s %llx -> species %llu form %llu\n", name[i], (unsigned long long)cand[i], (unsigned long long)species,
            (unsigned long long)form);
    }
    memcpy(c->x, save, sizeof save);
    c->x[30] = lr;
    c->sp = sp;
}

// The game's own answers about the party, from inside the trigger (its arguments are kept). In the game's own words the
// "partner" is the Pokémon walking with the player and the "buddy" is the starter (Pikachu form 8 / Eevee form 1):
// f_70002462e0(0, &slot) is how the script native GetPartnerIndex_ (f_700019a040) finds the walking one's party slot
// (-1: none); the script natives PokePartyGetCount (f_7000197e90) and PokePartyIsBuddy (f_7000198b20) are called with a
// parameter block of their own ({size, arg}, 64-bit cells).
#define WALKING_SLOT 0x70002462e0ull
#define NATIVE_PARTY_COUNT 0x7000197e90ull
#define NATIVE_IS_BUDDY 0x7000198b20ull
static uint64_t party_call(Cpu *c, uint64_t fn, uint32_t arg, bool native, int *slot) {
    uint64_t keep[19], lr = c->x[30], sp = c->sp;
    memcpy(keep, c->x, sizeof keep);
    c->sp -= 0x20;
    if (native) R64(c->sp, 0x10) = 8, R64(c->sp, 0x18) = arg, c->x[0] = 0, c->x[1] = c->sp + 0x10;
    else R32(c->sp, 0x10) = 6, c->x[0] = arg, c->x[1] = c->sp + 0x10;
    rt_call_original(c, fn);
    uint64_t r = c->x[0];
    if (slot) *slot = (r & 1) ? (int)R32(c->sp, 0x10) : -1;
    memcpy(c->x, keep, sizeof keep);
    c->x[30] = lr, c->sp = sp;
    return r;
}

// Whether the starter itself is the one walking with the player (early in the game no other Pokémon does: issue #6).
// The battle sends the starter in with ee422.bseq (f_7000a88780 for the opening, f_7000a8ced0 for a switch), which moves
// the walking Pokémon's field model into the battle; with the starter walking alone, the static battle - the kind this
// mode starts - has none for it and the game stops (f_7000a9ddb0 reading a null pointer), whether the starter leads or
// is switched in. In ours it is then sent out from a Poké Ball, with its species' own model (the two hooks below).
static bool starter_walks_alone(Cpu *c) {
    int walking;
    party_call(c, WALKING_SLOT, 0, false, &walking);
    int n = (int)(uint32_t)party_call(c, NATIVE_PARTY_COUNT, 0, true, NULL), starter = -1;
    for (int i = 0; i < n && i < 6; i++)
        if (party_call(c, NATIVE_IS_BUDDY, (uint32_t)i, true, NULL) & 1) { starter = i; break; }
    LOG("[wild] f%llu party: %d, walking with the player: slot %d, starter: slot %d\n", (unsigned long long)renderer_game_frame(),
        n, walking, starter);
    return starter >= 0 && (walking == starter || walking < 0);
}

// Is this battle Pokémon the starter (f_7000a7e090, the battle Pokémon's virtual: f_70001760d0, Pikachu form 8 / Eevee
// form 1)? The starter is sent out from the shoulder (ee422.bseq), which brings in the field model of the Pokémon walking
// with the player - the static battle has none when the starter walks alone, and f_7000a9ddb0 reads a null. In ours,
// while the starter walks alone: no, so it's sent out from a Poké Ball like the others.
#define BATTLE_IS_STARTER 0x7000a7e090ull
static bool g_starter_alone;  // this battle's player walks with the starter only
static int battle_starter_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!g_active || !g_starter_alone) return 0;
    c->x[0] = 0;
    return 1;
}

// A battle model's joint (f_7000a9c700: x0 = the model, x1 = the joint's name; the joint, or null when the model has no
// such joint): it looks the model's resource up by its key ([+0x1e0], or [+0x1e8]) and reads the result even when there
// is none. The Pokémon's fainting in our battles (the trainer battles' sequence ee620: setup_limit_hook) asks it of a model
// that a wild battle doesn't have (its key is 0: an opponent trainer's, presumably); in ours that answers "no joint",
// the game's own answer for a missing joint.
#define MODEL_JOINT 0x7000a9c700ull
static int model_joint_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t m = c->x[0];
    if (!g_active || !m || (R8(m, 0x192) && R64(m, 0x1e8)) || R64(m, 0x1e0)) return 0;
    c->x[0] = 0;
    return 1;
}

// A battle model's Pokémon (f_7000a99b40: x0 = the model, x2 -> {u32 species, u8 form, ...}, 0x14 bytes): for the starter
// (f_7000176140: Pikachu form 8, Eevee form 1) the model is the field one of the Pokémon on the player's shoulder or
// walking along, which the static battle doesn't load when the starter walks alone - its key stays 0 and the starter is
// never seen (and its moves crash: f_7000a9bf20 looks the model's resource up by that key). In ours, then, the starter's
// model is loaded as the species' own (form 0: there is no pm0133_01, the starter forms are field models), in a static
// copy that outlives the call.
#define BATTLE_MODEL_POKE 0x7000a99b40ull
static int battle_model_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!g_active || !g_starter_alone || !c->x[2]) return 0;
    uint32_t sp = R32(c->x[2], 0);
    uint8_t form = R8(c->x[2], 4);
    if (!((sp == 25 && form == 8) || (sp == 133 && form == 1))) return 0;
    static uint8_t desc[0x20] __attribute__((aligned(16)));
    memcpy(desc, (const void *)(uintptr_t)c->x[2], 0x14);
    desc[4] = 0;
    c->x[2] = (uint64_t)(uintptr_t)desc;
    LOG("[wild] f%llu the starter (species %u) walks alone: its battle model is the species' own\n", (unsigned long long)renderer_game_frame(), sp);
    return 0;  // (the game's function runs, with the copy)
}

// The static battle's process, built through its factory with the touch's manager and allocator (the constructor's hook
// then writes our hash in it). The registers are the caller's to restore.
static bool g_menu_seen;  // this battle's command menu has been shown (battle_event_hook)
static void start_battle(Cpu *c) {
    uint64_t out[2] = {0, 0}, sp = c->sp;
    g_pending = true;
    g_menu_seen = false;
    g_active = true;
    c->x[0] = g_mgr;
    c->x[1] = g_alloc;
    c->x[8] = (uint64_t)(uintptr_t)out;
    extern void fps60_scene_begin(void);  // (runtime/hle/fps60.c: a battle, 30 fps without in-between images)
    fps60_scene_begin();
    extern void input_mark(void);  // (runtime/hle/input.c: test scripts time the battle from here)
    input_mark();
    rt_call_original(c, STATIC_FACTORY);
    c->sp = sp;
    g_pending = false;
    LOG("[wild] factory returned, handle %llx\n", (unsigned long long)out[0]);
}

static int trigger_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (g_active) return 1;  // (swallowed: our battle is running)
    if (g_count >= g_max) return 0;  // (the game's capture runs)
    g_starter_alone = starter_walks_alone(c);  // (issue #6: the starter's battle model, above)
    g_count++;
    memset(&g_catch, 0, sizeof g_catch);
    if (getenv("LARECOMP_WILDBATTLE_PROBE")) probe_identity(c);
    uint64_t lr = c->x[30];
    g_mgr = R64(R64(c->x[1], 0), 0x68);
    g_alloc = R64(R64(R64(GA(GOT_ALLOCATOR), 0), 0), 0x68);
    LOG("[wild] f%llu encounter intercepted (pokemon obj %llx, manager %llx, allocator %llx): building the static battle process\n",
        (unsigned long long)renderer_game_frame(), (unsigned long long)c->x[0], (unsigned long long)g_mgr, (unsigned long long)g_alloc);
    start_battle(c);
    c->x[30] = lr;
    c->x[0] = 0;
    return 1;
}

// Research (LARECOMP_WILDBATTLE_DEBUG=1): 240 presents after the battle's process is gone, dump the frame's render passes
// (build/pass_NNN.png) and every draw (the F9 request of the renderer): what a black screen after the battle is made of.
extern volatile int g_debug_frame_request;
static volatile long g_end_frame;  // the present the battle's process was destroyed in (fade-in timing)
static volatile long g_dbg_frame;  // the same, for the debug dump (which clears it)

#ifdef _WIN32
#include <windows.h>
static DWORD WINAPI debug_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(50);
        if (g_dbg_frame && (long)renderer_game_frame() >= g_dbg_frame + 240) {
            LOG("[wild] f%llu debug dump requested\n", (unsigned long long)renderer_game_frame());
            g_debug_frame_request = 1;
            g_dbg_frame = 0;
        }
    }
    return 0;
}
#endif

// The game's screen fade (the script natives FadeIn_/FadeOut_, f_70001a7680 / f_70001a7860, end in these): the manager is
// [[*GOT 0x70015facf8]]. f_700011ece0(manager, 0, type, frames) fades in; f_700011ee50(manager, 0, type, ...) is the
// fade-out the object factories and FadeOut_ use. A script fades out before the battle and in after it; ours has no script,
// so (LARECOMP_WILDBATTLE_FADEIN=1) the last fade-out's type is repeated as a fade-in some presents after the battle.
#define FADE_IN_FN 0x700011ece0ull
#define FADE_OUT_FN 0x700011ee50ull
#define GOT_FADE 0x70015facf8ull
#define WORLD_TICK 0x70002f78d0ull  // the world's update, every frame (the parent of f_70002f8bb0, which fps60.c hooks: one hook per address)
static int g_fade_type = 1, g_fade_seen;
static bool g_fadein_on = true;
static int g_fadein_delay = 30, g_fadein_frames = 10;
static bool g_fadein_done = true;

// The battle again after a capture, straight to its menu: its opening (the player's Pokémon sent out - a ball, or the
// partner off the shoulder - and "Go on, X!") runs through behind the dark screen the capture left, at full speed and
// silent; the battle's fade-in waits until its menu is made (views_hook), or 30 s of frames at most.
extern volatile int g_present_fast, g_audio_muted;  // runtime/nvn/objects.c, runtime/services/audio.c
static bool g_hidden;          // the opening is running hidden
static bool g_menu_up;         // ... and its menu is up, waiting for the answer the player doesn't see
static uint64_t g_hidden_since;
// (a safety: shown anyway after this many frames. ~540-770 measured, up to 860 with the turn answered at 60 fps)
#define HIDDEN_MAX 1500
static int g_hidden_fade = 0xa, g_hidden_fade_frames = 2;  // the battle's own fade-in (its type and length), held back
static void hidden_begin(void) {
    g_hidden = true, g_hidden_since = renderer_game_frame();
    g_present_fast = 1, g_audio_muted = 1;
    LOG("[wild] f%llu the battle again: its opening runs hidden\n", (unsigned long long)g_hidden_since);
}
static void hidden_end(Cpu *c, const char *why) {
    if (!g_hidden) return;
    g_hidden = false, g_menu_up = false;
    g_present_fast = 0, g_audio_muted = 0;
    uint64_t save[32], lr = c->x[30], sp = c->sp;
    memcpy(save, c->x, sizeof save);
    c->x[0] = R64(R64(GA(GOT_FADE), 0), 0);
    c->x[1] = 0;
    c->x[2] = (uint64_t)g_hidden_fade;
    c->x[3] = (uint64_t)g_hidden_fade_frames;
    rt_call_original(c, FADE_IN_FN);
    memcpy(c->x, save, sizeof save);
    c->x[30] = lr, c->sp = sp;
    LOG("[wild] f%llu the battle again: shown (%s, %llu frames hidden)\n", (unsigned long long)renderer_game_frame(), why,
        (unsigned long long)(renderer_game_frame() - g_hidden_since));
}

static int fade_log_hook(Cpu *c, uint64_t addr) {
    if (g_hidden && addr == rt_addr(FADE_IN_FN)) {  // (the battle's fade-in: held back, shown with its menu)
        g_hidden_fade = (int)c->x[2], g_hidden_fade_frames = (int)c->x[3];
        return 1;
    }
    if (g_hidden && renderer_game_frame() > g_hidden_since + HIDDEN_MAX) hidden_end(c, "too long");
    LOG("[wild] f%llu fade %s: x1 %llx x2(type) %llx x3 %llx x4 %llx x5 %llx\n", (unsigned long long)renderer_game_frame(),
        addr == rt_addr(FADE_IN_FN) ? "in " : "out", (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
        (unsigned long long)c->x[4], (unsigned long long)c->x[5]);
    if (addr == rt_addr(FADE_OUT_FN)) { g_fade_type = (int)c->x[2]; g_fade_seen = 1; }
    return 0;
}

static int field_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (g_catch.rebattle && !g_active && g_end_frame && (long)renderer_game_frame() >= g_end_frame + 2) {
        uint64_t save[32], lr = c->x[30], sp = c->sp;
        memcpy(save, c->x, sizeof save);
        LOG("[wild] f%llu catch: the battle again (HP %u/%u, status %u)\n", (unsigned long long)renderer_game_frame(), g_catch.hp,
            g_catch.max_hp, g_catch.status);
        g_catch.requested = g_catch.ended = g_catch.caught = false;
        g_catch.choices = g_catch.throws = 0;
        g_catch.chose = false;
        hidden_begin();
        start_battle(c);
        memcpy(c->x, save, sizeof save);
        c->x[30] = lr, c->sp = sp;
    }
    if (!g_fadein_done && g_end_frame && (long)renderer_game_frame() >= g_end_frame + g_fadein_delay) {
        g_fadein_done = true;
        uint64_t save[32], lr = c->x[30], sp = c->sp;
        memcpy(save, c->x, sizeof save);
        c->x[0] = R64(R64(GA(GOT_FADE), 0), 0);
        c->x[1] = 0;
        c->x[2] = (uint64_t)g_fade_type;
        c->x[3] = (uint64_t)g_fadein_frames;
        LOG("[wild] f%llu fading in by hand: manager %llx type %d frames %d (last fade-out %s)\n",
            (unsigned long long)renderer_game_frame(), (unsigned long long)c->x[0], g_fade_type, g_fadein_frames, g_fade_seen ? "seen" : "NOT seen");
        rt_call_original(c, FADE_IN_FN);
        memcpy(c->x, save, sizeof save);
        c->x[30] = lr;
        c->sp = sp;
    }
    return 0;  // (the world's update runs)
}

// The background music's events (f_700016d060: x0 = the sound player, x2 -> {u64 hash, const char *name, u64 length}):
// a static battle pushes the field's music to the battle's when it starts (f_700028c770's process, after asking
// f_700016cee0 whether that is already done) and pops it back when its process ends - for us, when the capture after
// it ends. A capture that goes back to the battle popped it too, and the next battle pushed it only every other time
// (f_700016cee0 still saw the last push), so the field's music played over the battle's. In ours the battle's music
// goes on through the capture and the battle again: that pop and the next push are skipped.
#define BGM_EVENT 0x700016d060ull
static bool g_bgm_kept;  // the battle's music kept for the battle again
static bool bgm_is(Cpu *c, const char *name) {
    uint64_t e = c->x[2], s = e ? R64(e, 8) : 0;
    size_t n = strlen(name);
    return s && R64(e, 0x10) == n && !memcmp((const void *)(uintptr_t)s, name, n);
}
static int bgm_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    bool pop = bgm_is(c, "Pop_BGM_battle_to_field");
    if (pop && g_catch.catching && !g_catch.caught && g_catch.have)
        g_bgm_kept = true;
    else if (g_bgm_kept && g_active && bgm_is(c, "Push_BGM_field_to_battle"))
        g_bgm_kept = false;
    else {
        if (pop) g_bgm_kept = false;
        return 0;
    }
    LOG("[wild] f%llu the battle's music goes on: %s skipped\n", (unsigned long long)renderer_game_frame(), pop ? "its pop" : "the push");
    return 1;
}

static int dtor_hook(Cpu *c, uint64_t addr) {
    LOG("[wild] f%llu class A object %llx destroyed\n", (unsigned long long)renderer_game_frame(), (unsigned long long)c->x[0]);
    g_active = false;
    // a capture after our battle that ended without the Pokémon (run away from, or it fled): the battle again, from the
    // world's next update (still faded out: the battle's own fade-out follows)
    bool again = g_catch.catching && !g_catch.caught && g_catch.have;
    if (g_catch.catching)
        LOG("[wild] f%llu catch: the capture ended after %d throw(s), %s\n", (unsigned long long)renderer_game_frame(), g_catch.throws,
            g_catch.caught ? "caught" : again ? "not caught: the battle again" : "not caught");
    g_catch.requested = g_catch.catching = false;
    g_catch.rebattle = again;
    g_catch.their_turn = again && (g_catch.throws > 0 || getenv("LARECOMP_WILDBATTLE_THEIRTURN"));  // (test runs: any)
    g_end_frame = g_dbg_frame = (long)renderer_game_frame();
    if (g_fadein_on && !again) g_fadein_done = false;
    (void)addr;
    return 0;
}

static int ctor_hook(Cpu *c, uint64_t addr) {
    uint64_t lr = c->x[30], self = c->x[0];
    rt_call_original(c, addr);
    if (g_pending) {
        g_pending = false;
        R64(self, 0x98) = WILD_HASH;
        // as the script native CallWildBattle (f_700017e440) sets them after the factory: hash, a0 = a1 = 0, a2 = 1,
        // a3..a5 = !(flag bits 3, 1, 2) of the script's flags argument (0 here: all 1), a6 = 0 unless a special case
        // (the constructor's defaults are a0 = a1 = 1, a2 = 0 and would be wrong). LARECOMP_WILDBATTLE_RAWCTOR=1 keeps them.
        if (!getenv("LARECOMP_WILDBATTLE_RAWCTOR")) {
            R8(self, 0xa0) = 0;
            R8(self, 0xa1) = 0;
            R8(self, 0xa2) = 1;
            R8(self, 0xa3) = 1;
            R8(self, 0xa4) = 1;
            R8(self, 0xa5) = 1;
            R8(self, 0xa6) = 0;
        }
        if (g_a6) R8(self, 0xa6) = 1;
        LOG("[wild] class A object %llx: hash %llx, flags a0..a5 %02x%02x%02x%02x%02x%02x a6 %02x, state %u\n", (unsigned long long)self,
            (unsigned long long)R64(self, 0x98), R8(self, 0xa0), R8(self, 0xa1), R8(self, 0xa2), R8(self, 0xa3), R8(self, 0xa4),
            R8(self, 0xa5), R8(self, 0xa6), R32(self, 0x68));
    }
    c->x[30] = lr;
    return 1;
}

// the parsed record (f_70002bab70): +0 hash, +8 species, +0xc form, +0xd level, +0x10 shiny, +0x14 gender, +0x18 nature,
// +0x20/+0x28 text references, +0x30 ability, +0x34..+0x40 moves (u32), +0x44/+0x46 V1/V2, +0x48 IVs, +0x4e EVs,
// +0x54..+0x58 stat boosts
static void log_record(const char *what, const uint8_t *r) {
    uint32_t sp = *(const uint32_t *)(r + 8);
    LOG("[wild] %s: hash %llx species %u form %u level %u shiny %u gender %u nature %u ability %u moves %u,%u,%u,%u\n", what,
        (unsigned long long)*(const uint64_t *)r, sp, r[0xc], r[0xd], *(const uint32_t *)(r + 0x10), *(const uint32_t *)(r + 0x14),
        *(const uint32_t *)(r + 0x18), *(const uint32_t *)(r + 0x30), *(const uint32_t *)(r + 0x34), *(const uint32_t *)(r + 0x38),
        *(const uint32_t *)(r + 0x3c), *(const uint32_t *)(r + 0x40));
}

// Level-up moves from the game's own table: bin/archive/waza_oboe.gfpak (a GFLXPACK, docs/MODO-BATALHA.md) holds one file per
// species (the file's index is the species number): (move u16, level u16) pairs in level order, ended by 0xFFFFFFFF. A wild
// Pokémon knows the last four it learns up to its level. Read from the romfs image, the pack parsed once (header: u32 files at
// +0x10, i64 file-table offset at +0x18; 0x18-byte entries: type u8 at +2 (2 = LZ4 block), u32 sizes at +4 / +8, u32 offset at +0x10).
bool romfs_image_file(const char *path, int *fd, uint64_t *off, uint64_t *size);  // runtime/services/fs.c

static bool lz4_block(const uint8_t *in, size_t n, uint8_t *out, size_t outsz) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t t = in[i++];
        size_t lit = t >> 4;
        if (lit == 15) { uint8_t b; do { if (i >= n) return false; b = in[i++]; lit += b; } while (b == 255); }
        if (i + lit > n || o + lit > outsz) return false;
        memcpy(out + o, in + i, lit);
        i += lit, o += lit;
        if (i >= n) break;
        if (i + 2 > n) return false;
        size_t off = in[i] | ((size_t)in[i + 1] << 8), ml = t & 15;
        i += 2;
        if (ml == 15) { uint8_t b; do { if (i >= n) return false; b = in[i++]; ml += b; } while (b == 255); }
        ml += 4;
        if (!off || off > o || o + ml > outsz) return false;
        for (size_t k = 0; k < ml; k++, o++) out[o] = out[o - off];
    }
    return o == outsz;
}

static uint8_t *g_pack;
static size_t g_pack_size;
static bool g_pack_tried;

static bool learnset_pack(void) {
    if (g_pack_tried) return g_pack != NULL;
    g_pack_tried = true;
    int fd;
    uint64_t off, size;
    if (!romfs_image_file("bin/archive/waza_oboe.gfpak", &fd, &off, &size) || size < 0x40 || size > (64u << 20)) return false;
    uint8_t *b = malloc((size_t)size);
    if (!b || pread(fd, b, (size_t)size, (off_t)off) != (ssize_t)size) { free(b); return false; }
    g_pack = b, g_pack_size = (size_t)size;
    return true;
}

// The moves a Pokémon of this species and level knows (moves[4], 0 = empty). False: the table wasn't readable.
static bool learnset_moves(uint32_t species, uint32_t level, int moves[4]) {
    if (!learnset_pack()) return false;
    uint32_t nfiles = *(const uint32_t *)(g_pack + 0x10);
    uint64_t tab = *(const uint64_t *)(g_pack + 0x18);
    if (species >= nfiles || tab + (uint64_t)0x18 * nfiles > g_pack_size) return false;
    const uint8_t *e = g_pack + tab + (uint64_t)0x18 * species;
    uint32_t szd = *(const uint32_t *)(e + 4), szc = *(const uint32_t *)(e + 8), poff = *(const uint32_t *)(e + 0x10);
    if (e[2] != 2 || szd > 4096 || (uint64_t)poff + szc > g_pack_size) return false;
    uint8_t buf[4096];
    if (!lz4_block(g_pack + poff, szc, buf, szd)) return false;
    int n = 0, learned[64];
    for (uint32_t i = 0; i + 4 <= szd && n < 64; i += 4) {
        uint16_t mv = *(const uint16_t *)(buf + i), lv = *(const uint16_t *)(buf + i + 2);
        if (mv == 0xffff && lv == 0xffff) break;
        if (lv > level) continue;
        for (int k = 0; k < n; k++) if (learned[k] == mv) goto have;  // (already known: it keeps its place)
        learned[n++] = mv;
    have:;
    }
    int first = n > 4 ? n - 4 : 0;
    for (int k = 0; k < 4; k++) moves[k] = first + k < n ? learned[first + k] : 0;
    return n > 0;
}

static int lookup_hook(Cpu *c, uint64_t addr) {
    uint64_t key = R64(c->x[1], 0);
    if (key != WILD_HASH) return 0;  // (a real lookup)
    uint64_t lr = c->x[30], sk = SNORLAX_HASH;
    c->x[1] = (uint64_t)(uintptr_t)&sk;
    rt_call_original(c, addr);  // the real Snorlax record: the mould
    uint64_t rec = c->x[0];
    c->x[30] = lr;
    memcpy(g_rec, (const void *)(uintptr_t)rec, sizeof g_rec);
    log_record("mould (Snorlax)", g_rec);
    *(uint64_t *)g_rec = WILD_HASH;
    if (g_wild_valid && !g_explicit) {  // the touched Pokémon's own data
        *(uint32_t *)(g_rec + 8) = g_wild_species;
        g_rec[0xc] = (uint8_t)g_wild_form;
        g_rec[0xd] = (uint8_t)g_wild_level;
        *(uint32_t *)(g_rec + 0x14) = g_wild_gender;
        for (int i = 0; i < 6; i++) g_rec[0x48 + i] = (uint8_t)g_wild_iv[i];
        int mv[4];
        if (!g_moves_given && learnset_moves(g_wild_species, g_wild_level, mv)) memcpy(g_moves, mv, sizeof mv);
    } else {
        *(uint32_t *)(g_rec + 8) = (uint32_t)g_species;
        g_rec[0xc] = 0;
        g_rec[0xd] = (uint8_t)g_level;
    }
    for (int i = 0; i < 4; i++) *(uint32_t *)(g_rec + 0x34 + 4 * i) = (uint32_t)g_moves[i];
    memset(g_rec + 0x54, 0, 5);  // no stat boosts
    log_record("answering with", g_rec);
    c->x[0] = (uint64_t)(uintptr_t)g_rec;
    return 1;
}

// ---- Catching during the battle (docs/MODO-BATALHA.md, stage 6) ----
// The player asks to catch; the battle ends as a run away (result 3), which the battle event turns into a win (1) when it
// gets to its state 5: the after-battle process then starts the game's own capture (f_7000319bc0, state 0: a win of a
// battle of kind 2 goes to f_700031a0e0). The wild Pokémon's HP and status come from the battle's last write-back of its
// Pokémon (f_70009e4280, below), and the capture's catch value (f_70007de080, below) is raised by them:
// x (3 max HP - 2 HP) / max HP (full HP 1, nearly fainted 3), x2.5 asleep or frozen, x1.5 paralysed, burnt or poisoned.
// A wild Pokémon that fainted is lost: its win is turned into a run away (no capture).

static double catch_factor(void) {
    double f = 1;
    if (g_catch.have && g_catch.max_hp) f = (3.0 * g_catch.max_hp - 2.0 * g_catch.hp) / g_catch.max_hp;
    if (g_catch.status == 2 || g_catch.status == 3) f *= 2.5;
    else if (g_catch.status) f *= 1.5;
    return f;
}

// The capture's catch value (f_70007de080, x0 = its inputs, a float 0..255 back in s0; the ring's colour and the shake
// checks of f_70007dcb00 come from it): the species' rate enters it as rate^0.54 (x the level, the ball, the throw...),
// so the HP / status factor goes on the result, as the classic formula's (a rate raised before that power gave +17% at
// most for a Pikachu: 190 -> 255).
#define CATCH_VALUE 0x70007de080ull
static int catch_value_hook(Cpu *c, uint64_t addr) {
    if (!g_catch.catching) return 0;
    uint64_t lr = c->x[30], sp = c->sp;
    rt_call_original(c, addr);
    c->x[30] = lr, c->sp = sp;
    float v;
    uint32_t bits = (uint32_t)c->v[0];
    memcpy(&v, &bits, 4);
    double f = catch_factor(), r = v * f;
    if (r > 255) r = 255;
    float out = (float)r;
    memcpy(&bits, &out, 4);
    c->v[0] = bits;
    static int last = -1;
    if ((int)r != last) {
        last = (int)r;
        LOG("[wild] f%llu catch value %.1f x %.2f -> %.1f\n", (unsigned long long)renderer_game_frame(), v, f, r);
    }
    return 1;
}

// After the battle (the process f_7000319bc0, state word [+0x68]): its state 0 writes the battle's party back and marks
// who went up a level ([+0xd8 + slot]); a trainer battle (kind 1) then goes to state 1, the loop that starts the
// evolution event for each of them (f_7000728300 checks, f_700083cef0 starts it: the evolution scene), a static one
// (kind 2) to the capture (3) or the end (4). Ours, when the wild Pokémon was knocked out: from state 0 to the loop too,
// so a party Pokémon that reached its level evolves after the battle, as after a trainer's.
#define AFTER_BATTLE 0x7000319bc0ull
static bool g_evolve_check;  // the battle that just ended was ours, knocked out: its after-battle goes by the evolutions
static int after_battle_hook(Cpu *c, uint64_t addr) {
    uint64_t self = c->x[0], lr = c->x[30], sp = c->sp;
    uint32_t before = R32(self, 0x68);
    rt_call_original(c, addr);
    c->x[30] = lr, c->sp = sp;
    if (g_evolve_check && before == 0 && R32(self, 0x68) == 4) {
        g_evolve_check = false;
        R32(self, 0x68) = 1;
        LOG("[wild] f%llu after the battle: the evolutions' check\n", (unsigned long long)renderer_game_frame());
    } else if (before == 0)
        g_evolve_check = false;
    return 1;
}

// The battle event (f_700028be80: state word [+0x68], the battle [+0x720], its result [[+0x720]+0x68]+0x508: 1 a win,
// 3 a run away). LARECOMP_WILDBATTLE_STATE_LOG=1 also logs its states and those of the static battle's process
// (f_7000289b40, same state word).
#define BATTLE_EVENT_UPDATE 0x700028be80ull
#define STATIC_UPDATE 0x7000289b40ull
static bool g_state_log;
static int battle_event_hook(Cpu *c, uint64_t addr) {
    int k = addr == rt_addr(BATTLE_EVENT_UPDATE);
    // (60 fps: a battle, 30 fps without in-between images. start_battle says so, but the battle again starts from inside
    // a world update - field_hook - whose end took it back as the world's: that battle ran as a menu, at 60 with
    // in-between images, and its menus flickered. Said again on each of its updates.)
    if (k && g_active) {
        extern void fps60_scene_begin(void);
        fps60_scene_begin();
    }
    if (g_hidden && g_menu_up && g_catch.their_turn) {  // (A every 8 frames: the menu may not take it while it opens)
        extern void input_press_a_at(uint64_t frame);
        static uint64_t last;
        uint64_t f = renderer_game_frame();
        if (f >= last + 8 || f < last) last = f, input_press_a_at(f + 1);
    }
    if (g_hidden && renderer_game_frame() > g_hidden_since + HIDDEN_MAX) hidden_end(c, "too long");
    uint32_t st = R32(c->x[0], 0x68);
    if (k && g_active && st == 4 && g_menu_seen && (R32(c->x[0], 0x574) & 0x80)) {  // (the legendaries' rule off: setup_limit_hook)
        R32(c->x[0], 0x574) &= ~0x80u;
        LOG("[wild] f%llu the battle's menu is up: a knocked out Pokémon faints\n", (unsigned long long)renderer_game_frame());
    }
    uint64_t bt = k && st >= 4 ? R64(c->x[0], 0x720) : 0, bd = bt ? R64(bt, 0x68) : 0;
    if (k && st == 5 && g_active && bd) {
        if (!g_catch.ended) {  // (state 5 runs once, but the hook may see it twice)
            g_catch.ended = true;
            uint32_t res = R32(bd, 0x508);
            if (res == 3 && g_catch.requested) {
                R32(bd, 0x508) = 1;
                g_catch.catching = true;
                LOG("[wild] f%llu catch: the battle ended for the capture (HP %u/%u, status %u)\n", (unsigned long long)renderer_game_frame(),
                    g_catch.hp, g_catch.max_hp, g_catch.status);
            } else if (res == 1 && g_catch.have && g_catch.hp == 0) {
                R32(bd, 0x508) = 3;
                g_evolve_check = true;
                LOG("[wild] f%llu catch: the wild Pokémon fainted, no capture\n", (unsigned long long)renderer_game_frame());
            } else if (res == 1)
                g_catch.catching = true;
        }
    }
    if (g_state_log) {
        static uint32_t last[2] = {0xffffffff, 0xffffffff};
        if (st != last[k]) {
            last[k] = st;
            LOG("[wild] f%llu %s %llx state %u", (unsigned long long)renderer_game_frame(), k ? "battle event" : "process",
                (unsigned long long)c->x[0], st);
            if (bd) LOG(" battle %llx data %llx result %d", (unsigned long long)bt, (unsigned long long)bd, (int)R32(bd, 0x508));
            LOG("\n");
        }
    }
    return 0;
}

// The static battle's setup (f_700028c770: x0 = the battle event, x1 -> {mode, configuration}) makes every static
// battle the legendaries' kind: a time limit of 300 seconds at [+0x562] (the "MATCH TIME" at the top left), and in the
// battle setup at [+0xe0] the flag 0x80 of [+0x574] and the byte [+0x584] - a knocked out Pokémon ends the battle on the
// spot (no fainting: the capture follows) and no Exp. Points. Ours: no time limit (0 hides it). The flag stays for the
// opening (cleared from the start, the game stopped: docs/HISTORICO.md), and goes once the command menu
// is up (battle_event_hook): a knocked out wild Pokémon then faints as in the trainer battles ("Rattata fainted!", it
// falls and is called back; model_joint_hook).
#define STATIC_SETUP 0x700028c770ull
static int setup_limit_hook(Cpu *c, uint64_t addr) {
    if (!g_active) return 0;
    uint64_t ev = c->x[0], lr = c->x[30];
    rt_call_original(c, addr);
    c->x[30] = lr;
    *(volatile uint16_t *)(uintptr_t)(ev + 0x562) = 0;
    return 1;
}

// Exp. Points (the battle main module's "exp on", f_70009d8ae0: competitor <= 1, setup byte +0x4a4 == 0 and flags +0x494
// without 0x80 / 0x800): in ours, on, as the trainer battles - the knock out gives the party its points, levels and moves
// ("<Pokémon> got N Exp. Points!", the party in waiting too) before the battle ends.
#define EXP_ENABLED 0x70009d8ae0ull
static int exp_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!g_active) return 0;
    c->x[0] = 1;
    return 1;
}

// The server's run away roll (f_70009f7300, from the run away of f_70009f3ed0: true = got away; false = "You couldn't
// get away!" and the turn goes on): the player's turn that was the throw (decide_hook) doesn't get away.
#define ESCAPE_ROLL 0x70009f7300ull
static bool g_pass;  // the player's next run away is the throw's turn: it fails, its line unseen
static bool g_pass_line;  // its "You couldn't get away!": not shown (msg_hook, msgwin_hook)
static int escape_roll_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!g_pass || !g_active) return 0;
    g_pass = false, g_pass_line = true;
    c->x[0] = 0;
    return 1;
}

// The client's "can it run away?" (f_7000a5c350: 5 yes; else "You can't escape!" - a trapping move such as Wrap, or
// an ability): CATCH goes out as a run away, but a ball can be thrown at a Pokémon that holds yours, so it answers yes.
#define CAN_ESCAPE 0x7000a5c350ull
static int escape_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!g_active || (!g_catch.requested && !g_pass)) return 0;
    c->x[0] = 5;
    return 1;
}

// The species data's accessor (pml's GetPersonalParam: x0 = the personal data, [x0+0x10] its personal_total.bin record,
// x1 = the parameter; 8 = the catch rate, the record's byte +8). During our capture the only one asking for the catch rate
// is the capture's formula (f_70007de080, every frame: the ring's colour follows it too). Test runs:
// LARECOMP_WILDBATTLE_FORCE_RATE=<n> answers n; LARECOMP_WILDBATTLE_CATCHLOG=1 logs who asks for it (each caller once).
#define PERSONAL_PARAM 0x700071dfd0ull
static bool g_catch_log;
static int personal_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[1] != 8) return 0;
    unsigned base = R8(R64(c->x[0], 0x10), 8);
    if (g_catch_log) {
        static uint64_t seen[16];
        static int n;
        bool known = false;
        for (int i = 0; i < n; i++) known |= seen[i] == c->x[30];
        if (!known && n < 16) {
            seen[n++] = c->x[30];
            LOG("[wild] f%llu catch rate asked: %u, lr %llx\n", (unsigned long long)renderer_game_frame(), base, (unsigned long long)c->x[30]);
        }
    }
    if (!g_catch.catching) return 0;
    static int forced = -2;  // (test runs: LARECOMP_WILDBATTLE_FORCE_RATE=<n> answers n, to make throws fail or succeed)
    if (forced == -2) forced = getenv("LARECOMP_WILDBATTLE_FORCE_RATE") ? atoi(getenv("LARECOMP_WILDBATTLE_FORCE_RATE")) : -1;
    if (forced < 0) return 0;
    unsigned r = (unsigned)forced;
    static unsigned last = 0xffff;
    if (r != last) {
        last = r;
        LOG("[wild] f%llu catch rate %u -> %u\n", (unsigned long long)renderer_game_frame(), base, r);
    }
    c->x[0] = r;
    return 1;
}

// A Pokémon's battle state from the game's own object (pml's CoreParam): [param+0x98] is its accessor, [acc+0x10] the
// core block (u32 encryption key, 4 bytes, 0xE0 encrypted), [acc+8] the party block (0x1C bytes: status u32 +0, level +4,
// HP +8, max HP +0xA, as PB7 +0xE8..), encrypted with the same key while [acc+0x18] is set (f_7000739640: the LCG
// 0x41C64E6D / 0x6073, each u16 xored with the high half).
typedef struct { uint32_t status; unsigned level, hp, max_hp; } PokeState;
static bool poke_state(uint64_t param, PokeState *st) {
    uint64_t acc = param ? R64(param, 0x98) : 0, calc = acc ? R64(acc, 8) : 0, core = acc ? R64(acc, 0x10) : 0;
    if (!calc || !core) return false;
    uint8_t d[0x1c];
    memcpy(d, (const void *)(uintptr_t)calc, sizeof d);
    if (R8(acc, 0x18)) {
        uint32_t seed = R32(core, 0);
        for (int i = 0; i < 0x1c / 2; i++) {
            seed = seed * 0x41C64E6Du + 0x6073u;
            ((uint16_t *)d)[i] ^= (uint16_t)(seed >> 16);
        }
    }
    st->status = *(const uint32_t *)d;
    st->level = d[4];
    st->hp = *(const uint16_t *)(d + 8);
    st->max_hp = *(const uint16_t *)(d + 0xa);
    return true;
}

// Research (LARECOMP_WILDBATTLE_HPSCAN=<A>:<B>, frames from the battle's start): where the battle keeps the wild Pokémon's
// HP. At A (full HP) every u16 of the heaps equal to its max HP is kept; at B (after a hit) those that dropped are logged.
static unsigned g_scan_max_hp;
#ifdef _WIN32
static DWORD WINAPI hpscan_thread(LPVOID unused) {
    (void)unused;
    long fa = 0, fb = 0, fc = 0;
    sscanf(getenv("LARECOMP_WILDBATTLE_HPSCAN"), "%ld:%ld:%ld", &fa, &fb, &fc);
    extern long input_mark_frame(void);
    while (input_mark_frame() < 0 || (long)renderer_game_frame() < input_mark_frame() + fa) Sleep(20);
    // (a bitmap per committed region: the heap is one region of gigabytes, and the value is common)
    struct { uint64_t base, size; uint8_t *bits; } reg[64];
    int nreg = 0;
    size_t n = 0;
    for (uint64_t a = 0x7200000000ull; a < 0x7800000000ull && nreg < 64;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) break;
        uint64_t base = (uint64_t)(uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE) {
            uint8_t *bits = calloc(size / 16 + 1, 1);
            if (bits) {
                const uint16_t *w = (const uint16_t *)(uintptr_t)base;
                for (uint64_t i = 0; i < size / 2; i++)
                    if (w[i] == g_scan_max_hp) bits[i >> 3] |= (uint8_t)(1 << (i & 7)), n++;
                reg[nreg].base = base, reg[nreg].size = size, reg[nreg++].bits = bits;
            }
        }
        a = base + size;
    }
    LOG("[wild] f%llu HP scan: %zu words hold %u\n", (unsigned long long)renderer_game_frame(), n, g_scan_max_hp);
    while ((long)renderer_game_frame() < input_mark_frame() + fb) Sleep(20);
    // (B: those that dropped; with a C, only those that dropped again)
    size_t nc = 0, capc = 1 << 16;
    uint64_t *cand = malloc(capc * sizeof *cand);
    uint16_t *was = malloc(capc * sizeof *was);
    for (int r = 0; r < nreg; r++) {
        const uint16_t *w0 = (const uint16_t *)(uintptr_t)reg[r].base;
        for (uint64_t i = 0; i < reg[r].size / 2 && nc < capc; i++)
            if ((reg[r].bits[i >> 3] & (1 << (i & 7))) && w0[i] < g_scan_max_hp && w0[i]) cand[nc] = reg[r].base + 2 * i, was[nc++] = w0[i];
        free(reg[r].bits);
    }
    LOG("[wild] f%llu HP scan: %zu dropped\n", (unsigned long long)renderer_game_frame(), nc);
    if (fc) while ((long)renderer_game_frame() < input_mark_frame() + fc) Sleep(20);
    int shown = 0;
    for (size_t i = 0; i < nc && shown < 80; i++) {
        uint16_t v = *(const uint16_t *)(uintptr_t)cand[i];
        if (fc && v >= was[i]) continue;
        shown++;
        const uint16_t *w = (const uint16_t *)(uintptr_t)(cand[i] - 8);
        LOG("[wild]   %llx %u -> %u: around %04x %04x %04x %04x [%04x] %04x %04x %04x %04x\n", (unsigned long long)cand[i], was[i], v, w[0],
            w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]);
    }
    free(cand), free(was);
    LOG("[wild] f%llu HP scan done (%d shown)\n", (unsigned long long)renderer_game_frame(), shown);
    return 0;
}
#endif

// The battle's own Pokémon (BTL_POKEPARAM): f_70009e4280 (x0 = it) writes it back to its pml object ([+0x60]) when the
// battle ends, won or run from: species u16 at +0x78, max HP +0x7c, HP +0x7e, the status conditions in the low 3 bits of
// +0xa0 (paralysis), +0xa8 (sleep), +0xb0 (freeze), +0xb8 (burn), +0xc0 (poison) (pml's order 1..5), the Pokémon's id
// in the battle at +0x87 (0..5 the player's, the wild one 12). LARECOMP_WILDBATTLE_HPLOG=1 logs each.
#define BPP_REFLECT 0x70009e4280ull
#define R16(p, o) (*(volatile uint16_t *)((uintptr_t)(p) + (o)))
static bool g_hp_log;
static int reflect_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t b = c->x[0];
    unsigned status = 0;
    for (int i = 0; i < 5 && !status; i++) if (R8(b, 0xa0 + 8 * i) & 7) status = (unsigned)i + 1;
    if (g_hp_log)
        LOG("[wild] f%llu battle Pokémon %llx: species %u HP %u/%u status %u\n", (unsigned long long)renderer_game_frame(), (unsigned long long)b,
            R16(b, 0x78), R16(b, 0x7e), R16(b, 0x7c), status);
    if (g_active && R8(b, 0x87) >= 6) {  // (+0x87: the Pokémon's id in the battle, 0..5 the player's)
        g_catch.have = true;
        g_catch.hp = R16(b, 0x7e), g_catch.max_hp = R16(b, 0x7c), g_catch.status = status;
        memcpy(g_catch.status_raw, (const void *)(uintptr_t)(b + 0xa0), sizeof g_catch.status_raw);
    }
    return 0;
}

// One ball at a time: the capture's "no Poké Ball left" check (f_7000919970: the bag has none of items 4, 12, 5, 3, 2, 1)
// answers yes once a throw of our capture didn't hold. The scene then withdraws by itself (its own way out, poke_get
// message 7) and the battle starts again (dtor_hook).
#define NO_BALLS_LEFT 0x7000919970ull
static int noballs_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!g_catch.catching || g_catch.throws < 1 || g_catch.caught) return 0;
    c->x[0] = 1;
    return 1;
}

// The lines the catch changes, in the game's language (runtime/mods.c: game_language). A message file's line is decrypted
// by f_70006786b0 (x0 = its data: [+0x90] -> u16 line count at +2, [+0x98] the lines' table, 8 bytes each from +4: u32
// offset, u16 length; x1 = the line, w2 = its length, w3 = its index, x4 = the string buffer: u16 *text at +0x58,
// capacity +0x78, length +0x7a). The files are told apart by their line count (the same in every language), only while
// one of our battles or captures runs: btl_std.dat (392 lines) line 82 "You got away safely!" when the battle ends for
// the capture (line 4 "A wild X appeared!", when the battle comes back after one, isn't shown: msgwin_hook);
// poke_get.dat (70 lines) line 1 "Oh! You encountered a wild X!" -> its own line 2 "Now's your chance! Catch the wild X!"
// (the legendaries' one), line 7 "You don't have a single Poké Ball left!..." when one ball didn't hold (noballs_hook), and the menu's way out - it brings the battle back - line 24 "Run away", line 4 "Do you want to
// give up catching this Pokémon?" and line 14 "You got away safely!".
#define MSG_DECRYPT 0x70006786b0ull
#define BTL_STD_LINES 392
#define POKE_GET_LINES 70
#include <uchar.h>
enum { TXT_CATCHING, TXT_AGAIN, TXT_RUN, TXT_RUN_ASK, TXT_RUN_DONE, TXT_N };
static const struct { const char *lang; const char16_t *t[TXT_N]; } k_catch_text[] = {
    {"en", {u"Time to catch it!", u"It’s still raring to go!\nBack to the battle!", u"Back to battle",
            u"Do you want to go back to the battle?", u"Back to the battle!"}},
    {"es", {u"¡Es hora de atraparlo!", u"¡Todavía tiene ganas de pelear!\n¡De vuelta al combate!", u"Volver al combate",
            u"¿Quieres volver al combate?", u"¡De vuelta al combate!"}},
    {"fr", {u"C’est le moment de le capturer !", u"Il est encore en forme !\nRetour au combat !", u"Retour au combat",
            u"Voulez-vous retourner au combat ?", u"Retour au combat !"}},
    {"de", {u"Zeit, es zu fangen!", u"Es ist noch kampfbereit!\nZurück in den Kampf!", u"Zurück zum Kampf",
            u"Möchtest du zum Kampf zurückkehren?", u"Zurück in den Kampf!"}},
    {"it", {u"È il momento di catturarlo!", u"È ancora pronto a lottare!\nSi torna alla lotta!", u"Torna alla lotta",
            u"Vuoi tornare alla lotta?", u"Si torna alla lotta!"}},
    {"ja", {u"いまだ！ つかまえよう！", u"まだ たたかう きだ！\nバトルに もどろう！", u"バトルに もどる", u"バトルに もどりますか？",
            u"バトルに もどった！"}},
    {"ko", {u"지금이다! 잡아 보자!", u"아직 싸울 생각인 것 같다!\n다시 배틀이다!", u"배틀로 돌아간다", u"배틀로 돌아가겠습니까?",
            u"배틀로 돌아왔다!"}},
    {"zh-CN", {u"现在就来捕捉吧！", u"它还想继续战斗！\n回到对战吧！", u"回到对战", u"要回到对战吗？", u"回到对战了！"}},
    {"zh-TW", {u"現在就來捕捉吧！", u"牠還想繼續戰鬥！\n回到對戰吧！", u"回到對戰", u"要回到對戰嗎？", u"回到對戰了！"}},
};
static const char16_t *catch_text(int which) {
    extern const char *game_language(void);
    const char *l = game_language();
    size_t best = 0;
    for (size_t i = 0; i < sizeof k_catch_text / sizeof *k_catch_text; i++)
        if (!strncmp(l, k_catch_text[i].lang, strlen(k_catch_text[i].lang))) best = i;
    return k_catch_text[best].t[which];
}
static void put_text(uint64_t sb, const char16_t *t) {
    uint16_t *buf = (uint16_t *)(uintptr_t)R64(sb, 0x58);
    unsigned cap = R16(sb, 0x78), n = 0;
    if (!buf || !cap) return;
    for (; t[n] && n + 1 < cap; n++) buf[n] = t[n];
    buf[n] = 0;
    R16(sb, 0x7a) = (uint16_t)n;
}
static bool g_msg_log;
bool wildbattle_quick_intro(void);
static bool g_skip_window;  // the next message window isn't opened (msgwin_hook)
static int msg_hook(Cpu *c, uint64_t addr) {
    if (!g_active && !g_catch.catching) return 0;
    uint64_t md = c->x[0], sb = c->x[4], lr = c->x[30], sp = c->sp;
    unsigned lines = R16(R64(md, 0x90), 2), idx = (unsigned)c->x[3] & 0xffff;
    if (g_msg_log) LOG("[wild] f%llu line %u of %llx (%u lines)\n", (unsigned long long)renderer_game_frame(), idx, (unsigned long long)md, lines);
    const char16_t *mine = NULL;
    // (the line after "Back to battle", "Back to the battle!", doesn't wait for A: runtime/hle/input.c presses it)
    if (lines == POKE_GET_LINES && g_catch.catching && idx == 14 && R32(md, 0x54)) {
        extern void input_press_a_at(uint64_t frame);
        static uint64_t last;
        uint64_t f = renderer_game_frame();
        if (f > last + 30) last = f, input_press_a_at(f + 6), input_press_a_at(f + 16);
    }
    if (lines == BTL_STD_LINES && idx == 82 && g_catch.requested) mine = catch_text(TXT_CATCHING);
    else if (lines == BTL_STD_LINES && idx == 4 && wildbattle_quick_intro()) g_skip_window = true;
    else if (lines == BTL_STD_LINES && idx == 83 && g_pass_line) g_pass_line = false, g_skip_window = true;
    else if (lines == POKE_GET_LINES && idx == 7 && g_catch.catching && g_catch.throws >= 1 && !g_catch.caught) mine = catch_text(TXT_AGAIN);
    else if (lines == POKE_GET_LINES && g_catch.catching && (idx == 24 || idx == 4 || idx == 14))  // (its way out: the battle again)
        mine = catch_text(idx == 24 ? TXT_RUN : idx == 4 ? TXT_RUN_ASK : TXT_RUN_DONE);
    else if (lines == POKE_GET_LINES && idx == 1 && g_catch.catching && R32(md, 0x54) && R64(md, 0x98)) {  // (+0x54: in memory)
        uint64_t info = R64(md, 0x98) + 4 + 2 * 8;  // (line 2, decrypted with its own index)
        c->x[1] = R64(md, 0x98) + R32(info, 0), c->x[2] = R16(info, 4), c->x[3] = 2;
        return 0;
    }
    if (!mine) return 0;
    rt_call_original(c, addr);
    c->x[30] = lr, c->sp = sp;
    put_text(sb, mine);
    c->x[0] = sb;
    return 1;
}

// The battle's message window opening a text (f_7000aaf230: x0 = the window, x1 = the string): in the battle again the
// opening's "A wild X appeared!" isn't shown.
#define MSG_WINDOW_SHOW 0x7000aaf230ull
static int msgwin_hook(Cpu *c, uint64_t addr) {
    (void)c, (void)addr;
    if (!g_skip_window) return 0;
    g_skip_window = false;
    LOG("[wild] f%llu the battle again: the opening's message not shown\n", (unsigned long long)renderer_game_frame());
    return 1;
}

// The capture's way out (its menu's "Run away", ours "Back to battle": f_70007d2380 makes the dialog at [scene+0x760],
// a state machine at +0x10: [+0x10] state, [+0x14] the next, [+0x1c] a flag): state 0 (f_70007de440) asks "Do you want
// to give up catching this Pokémon?" and its answer "yes" leads to state 1 (f_70007de7d0, the way out). In our capture
// there's no question: state 0 goes straight to 1.
#define RUN_ASK 0x70007de440ull
static int run_ask_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!g_catch.catching) return 0;
    R32(c->x[0], 0x14) = 1;
    R8(c->x[0], 0x1c) = 0;
    return 1;
}

// Research (LARECOMP_WILDBATTLE_MSGLOG=1): message files as they are loaded (f_70001afc00: x0 = the object, x1 -> {u64
// hash, char *path, ...}); msg_hook logs the lines decrypted during our battles and captures
#define MSG_LOAD 0x70001afc00ull
static int msgload_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t desc = c->x[1], path = desc ? R64(desc, 8) : 0;
    LOG("[wild] f%llu message file %s -> %llx\n", (unsigned long long)renderer_game_frame(), path ? (const char *)(uintptr_t)path : "?",
        (unsigned long long)c->x[0]);
    return 0;
}
// The capture's judgement of a throw (f_70007dcb00, x0 = the judge): the four shake checks of the classic formula
// (b = 65536 / (255 / a)^(3/16), the scene's xorshift at [[+8]+0x720]), the number that passed at +0x128: 4 holds.
#define CAPTURE_JUDGE 0x70007dcb00ull
static int judge_hook(Cpu *c, uint64_t addr) {
    uint64_t judge = c->x[0], lr = c->x[30], sp = c->sp;
    rt_call_original(c, addr);
    c->x[30] = lr, c->sp = sp;
    if (g_catch.catching) {
        unsigned shakes = R32(judge, 0x128);
        g_catch.throws++;
        if (shakes >= 4) g_catch.caught = true;
        LOG("[wild] f%llu catch: throw %d, %u shake check(s) passed%s\n", (unsigned long long)renderer_game_frame(), g_catch.throws, shakes,
            shakes >= 4 ? ": caught" : "");
    }
    return 1;
}

// BTL_POKEPARAM::Setup (f_70009e2490: x0 = the battle's Pokémon, x1 -> its pml object): species +0x78, HP +0x7e, max HP
// +0x7c from the pml object. LARECOMP_WILDBATTLE_HPLOG=1 logs each one set up.
#define BPP_SETUP 0x70009e2490ull
static int setup_hook(Cpu *c, uint64_t addr) {
    uint64_t b = c->x[0], src = c->x[1], lr = c->x[30], sp = c->sp;
    uint8_t id = R8(src, 0x30);
    rt_call_original(c, addr);
    c->x[30] = lr, c->sp = sp;
    // the battle again after a capture that didn't hold: the wild Pokémon (id >= 6) as it was when the last one ended
    if (g_active && g_catch.rebattle && g_catch.have && id >= 6) {
        R16(b, 0x7e) = (uint16_t)(g_catch.hp < R16(b, 0x7c) ? g_catch.hp : R16(b, 0x7c));
        memcpy((void *)(uintptr_t)(b + 0xa0), g_catch.status_raw, sizeof g_catch.status_raw);
    }
    // (test runs: LARECOMP_WILDBATTLE_HP=<n> starts the wild Pokémon with n HP - a knock out in one move)
    static int hp = -2;
    if (hp == -2) hp = getenv("LARECOMP_WILDBATTLE_HP") ? atoi(getenv("LARECOMP_WILDBATTLE_HP")) : -1;
    if (g_active && hp > 0 && id >= 6 && !g_catch.rebattle) R16(b, 0x7e) = (uint16_t)(hp < R16(b, 0x7c) ? hp : R16(b, 0x7c));
    if (g_hp_log)
        LOG("[wild] f%llu battle Pokémon %llx set up: species %u HP %u/%u, id %u\n", (unsigned long long)renderer_game_frame(),
            (unsigned long long)b, R16(b, 0x78), R16(b, 0x7e), R16(b, 0x7c), id);
    return 1;
}

// The battle's command menu (docs/MODO-BATALHA.md, stage 6). With the menu's files of runtime/hle/catch_button.c it has a
// fifth button, CATCH (uikit hash g_catch_button_hash, after RUN), that the game doesn't know: f_7000ab2730 sets up the
// four buttons' views (each f_7000ab2e60: the button found by hash, its pattern frame, and its decide callback written at
// [button+0x150]: f_7000ab49f0 / ab4a20 / ab4a30 / ab4a50 with {view, event} stored at +0x170; on a decision it writes
// {1, id} to the event, id = the table at 0x7000f98258 indexed by the view's pattern frame [view+8]: 1..4 fight, Pokémon,
// bag, run, 7 back). Our hook gives CATCH the same callback with a view of frame 5: id 0, which the game's own handling
// (f_7000ab1e80, ids 1..7) leaves alone, and decide_hook turns into the catch: the action RUN (4, the battle ends as a run
// away: battle_event_hook) with the catch asked for.
#define CMD_VIEWS 0x7000ab2730ull
#define CMD_DECIDE 0x7000ab1e80ull
#define CMD_OWNER 0x7000ab1670ull
#define BUTTON_FIND 0x7000368330ull     // (layout, -> u64 hash) -> the uikit button
#define BUTTON_INVOKE 0x7000ab49f0ull   // the decide callback's four functions
#define BUTTON_DESTROY 0x7000ab4a20ull
#define BUTTON_CLONE 0x7000ab4a30ull
#define BUTTON_MOVE 0x7000ab4a50ull
static uint64_t g_menu_event;           // the menu's event: {u32 type (1 decided), u32 id}, the callback writes it
static int32_t g_catch_view[4] __attribute__((aligned(16))) = {0, 0, 5, 0};  // (a view of pattern frame 5: id 0)
extern uint64_t g_catch_button_hash;    // (runtime/hle/catch_button.c, when it made the menu's files)
static int views_hook(Cpu *c, uint64_t addr) {
    uint64_t views = c->x[0], lr = c->x[30], sp = c->sp;
    rt_call_original(c, addr);
    c->x[30] = lr, c->sp = sp;
    g_menu_event = views + 0x270;
    g_menu_seen = true;
    if (g_hidden && g_catch.their_turn)  // (the menu answered unseen, decide_hook: A pressed until it takes - battle_event_hook)
        g_menu_up = true;
    else
        hidden_end(c, "its menu");
    uint64_t list = R64(views, 0x158), holder = list ? R64(list, 0) : 0, layout = holder ? R64(holder, 0x58) : 0;
    if (!layout) return 1;
    uint64_t hash = g_catch_button_hash, keep[32];
    if (!hash) return 1;
    memcpy(keep, c->x, sizeof keep);
    c->x[0] = layout, c->x[1] = (uint64_t)(uintptr_t)&hash;
    rt_call_original(c, BUTTON_FIND);
    uint64_t button = c->x[0];
    memcpy(c->x, keep, sizeof keep);
    c->x[30] = lr, c->sp = sp;
    if (!button) return 1;  // (the game's own menu: four buttons)
    R64(button, 0x170) = (uint64_t)(uintptr_t)g_catch_view, R64(button, 0x178) = g_menu_event;
    R64(button, 0x168) = rt_addr(BUTTON_MOVE), R64(button, 0x160) = rt_addr(BUTTON_CLONE);
    R64(button, 0x158) = rt_addr(BUTTON_DESTROY), R64(button, 0x150) = rt_addr(BUTTON_INVOKE);
    if (getenv("LARECOMP_WILDBATTLE_MENULOG"))
        LOG("[wild] f%llu CATCH button %llx wired (event %llx)\n", (unsigned long long)renderer_game_frame(), (unsigned long long)button,
            (unsigned long long)g_menu_event);
    return 1;
}

// the decide callback (x0 -> its stored {view, event}): CATCH's is ours (the event is read by the menu before
// f_7000ab1e80 runs, so the press is noted here)
static bool g_catch_pressed;
static int invoke_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (R64(c->x[0], 0) == (uint64_t)(uintptr_t)g_catch_view) g_catch_pressed = true;
    return 0;
}

static int decide_hook(Cpu *c, uint64_t addr) {
    uint64_t frame = c->x[0], lr = c->x[30], sp = c->sp;
    bool pressed = g_catch_pressed;
    g_catch_pressed = false;
    rt_call_original(c, addr);
    c->x[30] = lr, c->sp = sp;
    uint64_t keep[32];
    memcpy(keep, c->x, sizeof keep);
    c->x[0] = frame;
    rt_call_original(c, CMD_OWNER);
    uint64_t owner = c->x[0];
    memcpy(c->x, keep, sizeof keep);
    c->x[30] = lr, c->sp = sp;
    uint64_t act = owner ? R64(owner, 0x78) : 0;
    if (getenv("LARECOMP_WILDBATTLE_MENULOG"))
        LOG("[wild] f%llu menu choice: %s, action %u\n", (unsigned long long)renderer_game_frame(), pressed ? "CATCH" : "the game's",
            act ? R32(act, 0x10) : 0xffff);
    if (!g_active || !act) return 1;
    g_catch.chose = true;
    // the battle again after a ball that broke: the player's first turn was the throw. The menu is answered unseen with
    // a run away that doesn't get away (escape_roll_hook) and whose line isn't shown, so the wild Pokémon moves alone,
    // then the menu comes back. (A "skip" action, 7 / 5 / 0, left the turn waiting for a move.)
    if (g_catch.their_turn) {
        g_catch.their_turn = false;
        g_pass = true;
        LOG("[wild] f%llu the battle again: the player's turn was the throw\n", (unsigned long long)renderer_game_frame());
        R32(act, 0x10) = 4;
        R32(frame, 0x184) = 0;
        hidden_end(c, "their turn");
        return 1;
    }
    // (test runs: LARECOMP_WILDBATTLE_AUTOCATCH=<n> turns the n-th choice in each battle into a catch)
    static int autocatch = -1;
    if (autocatch < 0) autocatch = getenv("LARECOMP_WILDBATTLE_AUTOCATCH") ? atoi(getenv("LARECOMP_WILDBATTLE_AUTOCATCH")) : 0;
    bool test = autocatch > 0 && ++g_catch.choices == autocatch;
    if (pressed || test) {
        R32(act, 0x10) = 4;
        R32(frame, 0x184) = 0;
        g_catch.requested = true;
        LOG("[wild] f%llu catch asked for\n", (unsigned long long)renderer_game_frame());
    }
    return 1;
}

// Research (LARECOMP_WILDBATTLE_MENULOG=1): the menu's events (f_7000ab1af0: w1 = 1 a button decided, 2..4 others)
#define CMD_EVENT 0x7000ab1af0ull
static int cmdevent_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    LOG("[wild] f%llu menu event %u\n", (unsigned long long)renderer_game_frame(), (unsigned)c->x[1]);
    return 0;
}

// Research (LARECOMP_WILDBATTLE_MENULOG=1): the battle's command menu frame (f_7000aaac10 builds it: x8 -> the handle) and
// its methods, read from the relocated vtable
#define CMD_FRAME_NEW 0x7000aaac10ull
static int cmdframe_hook(Cpu *c, uint64_t addr) {
    uint64_t out = c->x[8], lr = c->x[30];
    rt_call_original(c, addr);
    c->x[30] = lr;
    uint64_t obj = R64(out, 0), vt = R64(GA(0x7001608ed8ull), 0) + 0x10;  // (the class' vtable, from the constructor f_7000aab290)
    LOG("[wild] f%llu command frame %llx vtable %llx:", (unsigned long long)renderer_game_frame(), (unsigned long long)obj, (unsigned long long)vt);
    for (int i = 0; vt && i < 64; i++) LOG(" %llx", (unsigned long long)R64(vt, 8 * i));
    LOG("\n");
    return 1;
}

// The capture after a battle (f_700031a0e0, x0 = the after-battle process: [+0xc0] the wild Pokémon's object)
#define CAPTURE_AFTER_BATTLE 0x700031a0e0ull
static int capture_hook(Cpu *c, uint64_t addr) {
    extern void input_mark_capture(void);  // (runtime/hle/input.c: test scripts time the capture from here)
    input_mark_capture();
    if (!g_catch_log) return 0;
    PokeState st;
    if (poke_state(R64(c->x[0], 0xc0), &st))
        LOG("[wild] f%llu capture after the battle: status %x level %u HP %u/%u\n", (unsigned long long)renderer_game_frame(),
            st.status, st.level, st.hp, st.max_hp);
    (void)addr;
    return 0;
}

// Before each boot (runtime/main.c): the launcher's "Wild battles (experimental)" (larecomp.ini wild_battle = 1) turns the
// mode on when LARECOMP_WILDBATTLE doesn't say otherwise. The research variables (docs/DEPURACAO.md) tune it.
static bool g_enabled;
// runtime/services/fs.c: whether the battle menu's files are made with the CATCH button (the mode is on), and whether
// one of our battles runs now (its files are served then; the game's own otherwise)
bool wildbattle_enabled(void) { return g_enabled; }
// The battle again after a capture: runtime/services/fs.c serves the wild Pokémon's entrance (bin/battle/waza/sequence/
// ee330.bseq, the camera on it) one frame long, and its "A wild X appeared!" isn't shown (msgwin_hook): the battle comes
// back with the send-out alone, which runs hidden (hidden_begin). (The send-out itself, ee420.bseq, stays: cut or sped
// up, the player's Pokémon wasn't put on the field until its first move.)
bool wildbattle_quick_intro(void) { return g_active && g_catch.rebattle && !g_catch.chose; }
// (LARECOMP_WILDBATTLE_CATCH_MENU=0: the game's menu in our battles too - the padded original files, as other battles get)
bool wildbattle_menu_active(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_WILDBATTLE_CATCH_MENU") ? atoi(getenv("LARECOMP_WILDBATTLE_CATCH_MENU")) != 0 : 1;
    return g_active && on;
}

void wildbattle_configure(void) {
    static bool hooked;
    extern void settings_load(void);
    settings_load();  // (read once; without the launcher it would come later, in boot)
    const char *e = getenv("LARECOMP_WILDBATTLE");
    bool on = e && *e ? atoi(e) != 0 : g_settings.wild_battle;
    if (!rt_addr(ENCOUNTER_TRIGGER)) on = false;  // (a build runtime/fnhook.c doesn't know: these functions are elsewhere)
    g_enabled = on;
    if (!on || hooked) return;
    hooked = true;
    g_max = 1 << 30;  // every touch (the test runs set LARECOMP_WILDBATTLE_MAX)
    if (getenv("LARECOMP_WILDBATTLE_SPECIES")) { g_species = atoi(getenv("LARECOMP_WILDBATTLE_SPECIES")); g_explicit = true; }
    if (getenv("LARECOMP_WILDBATTLE_LEVEL")) g_level = atoi(getenv("LARECOMP_WILDBATTLE_LEVEL"));
    if (getenv("LARECOMP_WILDBATTLE_MAX")) g_max = atoi(getenv("LARECOMP_WILDBATTLE_MAX"));
    if (getenv("LARECOMP_WILDBATTLE_A6")) g_a6 = atoi(getenv("LARECOMP_WILDBATTLE_A6"));
    if (getenv("LARECOMP_WILDBATTLE_MOVES")) {
        g_moves_given = true;
        char buf[64];
        snprintf(buf, sizeof buf, "%s", getenv("LARECOMP_WILDBATTLE_MOVES"));
        int n = 0;
        for (char *t = strtok(buf, ","); t && n < 4; t = strtok(NULL, ",")) g_moves[n++] = atoi(t);
        for (; n < 4; n++) g_moves[n] = 0;
    }
    if (getenv("LARECOMP_WILDBATTLE_FADEIN")) g_fadein_on = atoi(getenv("LARECOMP_WILDBATTLE_FADEIN")) != 0;
    if (getenv("LARECOMP_WILDBATTLE_FADE_TYPE")) { g_fade_type = atoi(getenv("LARECOMP_WILDBATTLE_FADE_TYPE")); g_fade_seen = 1; }
    if (getenv("LARECOMP_WILDBATTLE_FADE_DELAY")) g_fadein_delay = atoi(getenv("LARECOMP_WILDBATTLE_FADE_DELAY"));
    if (getenv("LARECOMP_WILDBATTLE_FADE_FRAMES")) g_fadein_frames = atoi(getenv("LARECOMP_WILDBATTLE_FADE_FRAMES"));
    if (getenv("LARECOMP_WILDBATTLE_USECFG")) g_usecfg = atoi(getenv("LARECOMP_WILDBATTLE_USECFG")) != 0;
    rt_register_fnhook(ENCOUNTER_TRIGGER, trigger_hook);
    rt_register_fnhook(CLASS_A_DTOR0, dtor_hook);
    rt_register_fnhook(CLASS_A_DTOR1, dtor_hook);
    rt_register_fnhook(FADE_IN_FN, fade_log_hook);
    rt_register_fnhook(FADE_OUT_FN, fade_log_hook);
    rt_register_fnhook(WORLD_TICK, field_hook);  // (the fade-in after a battle, and the battle again after a capture)
    rt_register_fnhook(STATIC_CTOR, ctor_hook);
    rt_register_fnhook(TABLE_LOOKUP, lookup_hook);
    if (g_usecfg) { rt_register_fnhook(CONFIG_FROM_RECORD, config_hook); rt_register_fnhook(CONFIG_BUILD, build_hook); }
    rt_register_fnhook(WILD_BUILDER, logpoke_hook);
#ifdef _WIN32
    if (getenv("LARECOMP_WILDBATTLE_HPSCAN")) {
        g_scan_max_hp = getenv("LARECOMP_WILDBATTLE_HPSCAN_HP") ? (unsigned)atoi(getenv("LARECOMP_WILDBATTLE_HPSCAN_HP")) : 15;
        CreateThread(NULL, 0, hpscan_thread, NULL, 0, NULL);
    }
#endif
    g_hp_log = getenv("LARECOMP_WILDBATTLE_HPLOG") != NULL;
    g_catch_log = getenv("LARECOMP_WILDBATTLE_CATCHLOG") != NULL;
    g_state_log = getenv("LARECOMP_WILDBATTLE_STATE_LOG") != NULL;
    rt_register_fnhook(BPP_REFLECT, reflect_hook);
    rt_register_fnhook(BPP_SETUP, setup_hook);
    rt_register_fnhook(PERSONAL_PARAM, personal_hook);
    rt_register_fnhook(BATTLE_EVENT_UPDATE, battle_event_hook);
    rt_register_fnhook(STATIC_SETUP, setup_limit_hook);
    rt_register_fnhook(EXP_ENABLED, exp_hook);
    rt_register_fnhook(CATCH_VALUE, catch_value_hook);
    rt_register_fnhook(CAN_ESCAPE, escape_hook);
    rt_register_fnhook(ESCAPE_ROLL, escape_roll_hook);
    rt_register_fnhook(MSG_WINDOW_SHOW, msgwin_hook);
    rt_register_fnhook(AFTER_BATTLE, after_battle_hook);
    rt_register_fnhook(BATTLE_IS_STARTER, battle_starter_hook);
    rt_register_fnhook(BATTLE_MODEL_POKE, battle_model_hook);
    rt_register_fnhook(MODEL_JOINT, model_joint_hook);
    rt_register_fnhook(BGM_EVENT, bgm_hook);
    rt_register_fnhook(RUN_ASK, run_ask_hook);
    rt_register_fnhook(CMD_DECIDE, decide_hook);
    rt_register_fnhook(CMD_VIEWS, views_hook);
    rt_register_fnhook(BUTTON_INVOKE, invoke_hook);
    rt_register_fnhook(CAPTURE_JUDGE, judge_hook);
    rt_register_fnhook(NO_BALLS_LEFT, noballs_hook);
    g_msg_log = getenv("LARECOMP_WILDBATTLE_MSGLOG") != NULL;
    rt_register_fnhook(MSG_DECRYPT, msg_hook);
    if (g_msg_log) rt_register_fnhook(MSG_LOAD, msgload_hook);
    if (g_state_log) rt_register_fnhook(STATIC_UPDATE, battle_event_hook);
    rt_register_fnhook(CAPTURE_AFTER_BATTLE, capture_hook);
    if (getenv("LARECOMP_WILDBATTLE_MENULOG")) { rt_register_fnhook(CMD_FRAME_NEW, cmdframe_hook); rt_register_fnhook(CMD_EVENT, cmdevent_hook); }
#ifdef _WIN32
    if (getenv("LARECOMP_WILDBATTLE_DEBUG")) CreateThread(NULL, 0, debug_thread, NULL, 0, NULL);
#endif
    LOG("[wild] on: the touched Pokémon's own species and level%s (max %d battle(s))\n", g_explicit ? " (a fixed one: LARECOMP_WILDBATTLE_SPECIES)" : "", g_max);
}
