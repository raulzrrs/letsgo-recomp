// User settings (larecomp.ini in the data folder), set on the start screen (runtime/ui/launcher_lg.mm).
#pragma once
// frame_gen = auto ("auto" in larecomp.ini): the most images per world frame that the display's refresh shows evenly
// and the GPU keeps up with (runtime/nvn/vulkan/exec.c ip_images)
#define FG_AUTO 1
// How the in-between images are made (fg_method): the frame's draws run again with their constants between N-1's and
// N's (exec.c vk_interp_replay, the default), or N-1's and N's images moved along a motion field toward the image's
// time (exec.c flow_*: optical flow)
enum { FG_RERENDER = 0, FG_FLOW = 1 };
#include <stdbool.h>

enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12 };
// (11 was 67% of the display: larecomp.ini "67%" now loads as RES_Q75)
// FSR 3.1: temporal (AMD FidelityFX, runtime/nvn/vulkan/fsr3.c), FSR 1 when its DLL is missing
enum { UPSCALE_BILINEAR = 0, UPSCALE_FSR1 = 2 /* AMD FSR 1 */, UPSCALE_FSR3 = 3 };
// FSR 3.1's presets (AMD's ratios, each axis): the render resolution is the window's divided by it
enum { FSR3_NATIVE_AA = 0, FSR3_QUALITY = 1, FSR3_BALANCED = 2, FSR3_PERFORMANCE = 3, FSR3_ULTRA_PERFORMANCE = 4 };
float settings_fsr3_ratio(void);  // 1.0, 1.5, 1.7, 2.0, 3.0
#define UPSCALE_SHARP UPSCALE_FSR1  // the non-bilinear upscaler (the default)
// TAA: temporal, on the game's HDR scene before its bloom (runtime/nvn/vulkan/exec.c taa_run), jittered draws
// FSR 3.1 is the fourth choice: it anti-aliases and upscales; with the others a lower internal resolution reaches the
// window bilinearly (the upscaler follows: settings_apply_levels)
enum { AA_OFF = 0, AA_SMAA = 1, AA_TAA = 2, AA_FSR3 = 3 };
// SSAO and GI quality (F12 menu): their parameters follow (settings_apply_levels); HIGH = the former defaults
enum { FX_OFF = 0, FX_LOW = 1, FX_MEDIUM = 2, FX_HIGH = 3, FX_ULTRA = 4 };  // (ultra: SSAO only - high plus light contact shadows)
enum { VREAD_AUTO = 0, VREAD_COPY = 1, VREAD_DIRECT = 2 };
// the game's (each of its taps a hard in / out), each tap bilinear (2x2 PCF), each tap a 3x3 tent (4 bilinear lookups)
enum { SHADOW_GAME = 0, SHADOW_BILINEAR = 1, SHADOW_SMOOTH = 2 };

typedef struct {
    bool fullscreen;
    int fps;  // 30 (the game's own), 60 or 120 (experimental): the world's rate (runtime/hle/fps60.c; F12 menu, live)
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int res_height;      // the launcher's render height in lines (720 / 1080 / 1440 / 2160); 0: res_mode decides
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (FSR 3.1 with its anti-aliasing,
                         // else upscaler_pick: settings_apply_levels)
    int upscaler_pick;   // (older files' upscaler choice: read, no longer used - bilinear unless FSR 3.1)
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    float volume;        // master volume, 1 = 100%
    float brightness;    // colour of the game image (F12 menu, runtime/ui/menu.mm; post pass in runtime/nvn/vulkan/exec.c):
    float contrast;      // 1 = unchanged for all three; brightness / contrast 0.5..1.5, saturation 0..2
    float saturation;
    int ssao_level;       // FX_*: SSAO quality (default off); ssao and its parameters follow it
    int gi_level;         // FX_*: GI quality (default off); gi and its parameters follow it
    bool ssao;            // screen-space ambient occlusion on the game's HDR scene (F12 menu; runtime/nvn/vulkan/exec.c)
    int ssao_samples;     // per pixel: 8 / 12 / 16
    float ssao_intensity; // 0..3 (default 2)
    float ssao_radius;    // in the game's world units (~cm): 10..400 (default 100)
    bool ssao_temporal;   // accumulated over frames along the camera motion vectors (exec.c ssao_run)
    bool contact_on;      // (settings_apply_levels: SSAO ultra) short rays toward the sun in the depth: small objects'
                          // shadows the shadow map misses (exec.c contact_run)
    int contact_steps;    // ... their steps along a ray (8)
    float contact_strength;  // ... and how dark (0.375)
    bool ssao_debug;      // (not saved) the scene shows the occlusion alone
    bool mv_debug;        // (not saved) the scene shows its camera motion vectors (runtime/nvn/vulkan/exec.c scene_hook)
    bool mv_objects;      // per-object motion vectors (characters' own motion) when motion vectors are in use (default on)
    float taa_sharpness;  // TAA: RCAS sharpening at the present, 0..1 (default 0.5)
    float fsr3_sharpness; // FSR 3.1: its RCAS sharpening (AMD's robust CAS), 0..1 (default 0.5; graphics page, under its mode)
    int fsr3_preset;      // FSR3_*: with the FSR 3.1 upscaler, the render resolution (default FSR3_QUALITY)
    int frame_gen;        // frame generation: 0 off, FG_AUTO, 2 / 3 / 4 images per world frame (exec.c frame interpolation, any GPU)
    int fg_method;        // FG_RERENDER / FG_FLOW: how its in-between images are made (F12 advanced tab; live)
    bool blur_quality;    // the game's downsample / blur / depth-of-field composite read better (runtime/nvn/vulkan/exec.c fx_patch_fs)
    bool ui2x;            // the interface textures at 2x when cache/ui2x.pak was made (start screen; runtime/ui2x.c)
    int shadow_filter;    // SHADOW_*: how the shadow map is filtered (F12 menu; textures.c vk_sampler, translate.c shpcf)
    // screen-space indirect light, one bounce (F12 menu / advanced; runtime/nvn/vulkan/exec.c gi_run). World units ~cm
    bool gi;
    float gi_intensity;   // 0..4 (default 2): x the diffuse reflectance (0.5)
    float gi_radius;      // the rays' reach: 50..1000 (default 300)
    float gi_thickness;   // depth tolerance of a hit: 5..100 (default 25)
    int gi_rays;          // per GI pixel: 1..8 (default 4)
    int gi_steps;         // depth checks per ray: 4..32 (default 8)
    int gi_div;           // GI resolution: 2 half, 3 third, 4 quarter of each axis (default 2)
    bool gi_temporal;     // accumulated over frames along the camera motion vectors (default on)
    bool gi_debug;        // (not saved) the indirect light alone, shown at the present
    float mouse_sens;    // mouse aim (the virtual gyro, runtime/hle/input.c): degrees per pixel
    float stick_deadzone;  // a controller's sticks: the share of their travel read as rest (runtime/hle/input.c)
    bool reverb;         // room reverb effect
    bool movie_audio;
    bool show_fps;
    bool dpad_move;      // the d-pad (and arrow keys) move the player like the stick
    bool render_thread;  // the backend runs on its own thread (runtime/nvn/vulkan/renderthread.c); default on
    int vertex_read;     // VREAD_*: where the GPU reads vertex and index data (runtime/nvn/vulkan/exec.c)
    bool wild_battle;    // touching a wild Pokémon starts a battle, then the capture (runtime/hle/wildbattle.c)
    bool skip_controller;  // no controller screen at start: straight to the Game Freak logo (runtime/hle/controller_skip.c)
    char game_lang[8];   // the game's language code ("en-US", "es"...: runtime/mods.c game_language)
    char mod[1024];      // the active mods (mods/<name>/), '|' between them, the first one's files win; empty = none (runtime/mods.h)
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
void settings_clamp_color(void);  // brightness / contrast / saturation / SSAO into their ranges
void settings_apply_levels(void);  // the SSAO / GI parameters from their levels, the upscaler from the anti-aliasing and pick
