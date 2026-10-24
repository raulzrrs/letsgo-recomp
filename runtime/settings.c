#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_bind.h"
#include "paths.h"

#define SETTINGS_FILE settings_file()

// LARECOMP_SETTINGS=<path>: another settings file (test runs never touch the player's larecomp.ini)
static const char *settings_file(void) {
    const char *e = getenv("LARECOMP_SETTINGS");
    return e && *e ? e : "larecomp.ini";
}

Settings g_settings = {
    .fullscreen = false,
    .fps = 30,
    .volume = 1.0f,
    .brightness = 1.0f,
    .contrast = 1.0f,
    .saturation = 1.0f,
    .ssao = false,
    .ssao_intensity = 2.0f,
    .ssao_radius = 100.0f,
    .ssao_temporal = true,
    .ssao_samples = 16,
    .shadow_filter = SHADOW_SMOOTH,
    .ui2x = true,
    .blur_quality = true,
    .mv_objects = true,
    .taa_sharpness = 0.5f,
    .fsr3_sharpness = 0.5f,
    .fsr3_preset = FSR3_QUALITY,
    .gi = false,
    .gi_intensity = 2.0f,
    .gi_radius = 300.0f,
    .gi_thickness = 25.0f,
    .gi_rays = 4,
    .gi_steps = 8,
    .gi_div = 2,
    .gi_temporal = true,
    .mouse_sens = 0.1f,
    .stick_deadzone = 0.15f,
    .reverb = true,
    .movie_audio = true,
    .show_fps = false,
    .render_thread = true,
    .vertex_read = VREAD_AUTO,
    .upscaler = UPSCALE_SHARP,
    .upscaler_pick = UPSCALE_SHARP,
    .antialias = AA_SMAA,  // (the game has no anti-aliasing of its own)
};

// Unknown keys (an older larecomp.ini: Link's Awakening's options from LAS Recomp) are ignored, and dropped at the
// next save.
static void parse_line(const char *line) {
    char key[64];
    char val[1100];  // (the mods list can be long)
    if (sscanf(line, " %63[^= ] = %1099[^\n]", key, val) < 1) return;
    if (!strcmp(key, "game_lang") || !strcmp(key, "mod")) {  // (may be empty)
        if (sscanf(line, " %63[^= ] = %1099[^\n]", key, val) != 2) val[0] = 0;
        if (key[0] == 'g') strlcpy(g_settings.game_lang, val, sizeof g_settings.game_lang);
        else strlcpy(g_settings.mod, val, sizeof g_settings.mod);
        return;
    }
    if (!strcmp(key, "fullscreen")) g_settings.fullscreen = atoi(val) != 0;
    else if (!strcmp(key, "fps")) g_settings.fps = atoi(val) == 120 ? 120 : atoi(val) == 60 ? 60 : 30;
    else if (!strcmp(key, "render_height")) g_settings.res_height = atoi(val) >= 360 && atoi(val) <= 4320 ? atoi(val) : 0;
    else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
    else if (!strcmp(key, "brightness")) g_settings.brightness = (float)atof(val);
    else if (!strcmp(key, "contrast")) g_settings.contrast = (float)atof(val);
    else if (!strcmp(key, "saturation")) g_settings.saturation = (float)atof(val);
    // (an older file's "ssao = 1" / "gi = 1" were today's high; the levels are saved after them)
    else if (!strcmp(key, "ssao")) g_settings.ssao_level = atoi(val) != 0 ? FX_HIGH : FX_OFF;
    else if (!strcmp(key, "ssao_level")) g_settings.ssao_level = atoi(val);
    else if (!strcmp(key, "gi_level")) g_settings.gi_level = atoi(val);
    else if (!strcmp(key, "mv_objects")) g_settings.mv_objects = atoi(val) != 0;
    else if (!strcmp(key, "gi")) g_settings.gi_level = atoi(val) != 0 ? FX_HIGH : FX_OFF;
    else if (!strcmp(key, "gi_debug")) g_settings.gi_debug = atoi(val) != 0;  // (by hand / tests: never saved)
    else if (!strcmp(key, "ui2x")) g_settings.ui2x = atoi(val) != 0;
    else if (!strcmp(key, "blur_quality")) g_settings.blur_quality = atoi(val) != 0;
    else if (!strcmp(key, "shadow_filter"))
        g_settings.shadow_filter = !strcmp(val, "game") ? SHADOW_GAME : !strcmp(val, "bilinear") ? SHADOW_BILINEAR : SHADOW_SMOOTH;
    else if (!strcmp(key, "ssao_debug")) g_settings.ssao_debug = atoi(val) != 0;  // (by hand / tests: never saved)
    else if (!strcmp(key, "mv_debug")) g_settings.mv_debug = atoi(val) != 0;      // (the same)
    else if (!strcmp(key, "mouse_sens")) g_settings.mouse_sens = (float)atof(val);
    else if (!strcmp(key, "stick_deadzone")) g_settings.stick_deadzone = (float)atof(val);
    else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
    else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
    else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
    else if (!strcmp(key, "resolution")) {
        int n = atoi(val);
        g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : (!strcmp(val, "75%") || !strcmp(val, "67%")) ? RES_Q75 :
                              !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 :
                              (n >= 1 && n <= 4) ? n : n > 4 ? 4 : RES_MATCH;
    }
    // (after "aa" in the file. An older file's "fsr3": FSR 3.1 is an anti-aliasing choice now)
    else if (!strcmp(key, "upscaler")) {
        if (!strcmp(val, "fsr3")) g_settings.antialias = AA_FSR3;
        else g_settings.upscaler_pick = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : UPSCALE_SHARP;
    }
    // (an older "antialias = off" was only LAS Recomp's default being saved: a new key, so SMAA starts on)
    else if (!strcmp(key, "smaa")) g_settings.antialias = atoi(val) != 0 ? AA_SMAA : AA_OFF;
    // (after "smaa" in the file: the choice among the three; "smaa" stays for older versions)
    else if (!strcmp(key, "aa"))
        g_settings.antialias = !strcmp(val, "fsr3") ? AA_FSR3 : !strcmp(val, "taa") ? AA_TAA : !strcmp(val, "smaa") ? AA_SMAA : AA_OFF;
    else if (!strcmp(key, "taa_sharpness")) g_settings.taa_sharpness = (float)atof(val);
    else if (!strcmp(key, "fsr3_sharpness")) g_settings.fsr3_sharpness = (float)atof(val);
    else if (!strcmp(key, "fsr3_preset"))
        g_settings.fsr3_preset = !strcmp(val, "native") ? FSR3_NATIVE_AA : !strcmp(val, "balanced") ? FSR3_BALANCED :
                                 !strcmp(val, "performance") ? FSR3_PERFORMANCE : !strcmp(val, "ultra") ? FSR3_ULTRA_PERFORMANCE : FSR3_QUALITY;
    else if (!strcmp(key, "frame_gen"))  // (1: an older on / off)
        g_settings.frame_gen = !strcmp(val, "auto") ? FG_AUTO : atoi(val) == 1 ? 2 : atoi(val);
    else if (!strcmp(key, "fg_method")) g_settings.fg_method = !strcmp(val, "flow") ? FG_FLOW : FG_RERENDER;
    else if (!strcmp(key, "fg_quality")) {}  // (a setting of a test version: the in-between images are always full size)
    else if (!strcmp(key, "render_thread")) g_settings.render_thread = atoi(val) != 0;
    else if (!strcmp(key, "vertex_read")) g_settings.vertex_read = !strcmp(val, "direct") ? VREAD_DIRECT : !strcmp(val, "copy") ? VREAD_COPY : VREAD_AUTO;
    else if (!strcmp(key, "dpad_move")) g_settings.dpad_move = atoi(val) != 0;
    else if (!strcmp(key, "wild_battle")) g_settings.wild_battle = atoi(val) != 0;
    else if (!strcmp(key, "skip_controller")) g_settings.skip_controller = atoi(val) != 0;
    else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c
}

float settings_fsr3_ratio(void) {
    static const float r[] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return r[g_settings.fsr3_preset >= 0 && g_settings.fsr3_preset <= 4 ? g_settings.fsr3_preset : 1];
}

static float clampf_(float v, float lo, float hi) { return v != v ? 1 : v < lo ? lo : v > hi ? hi : v; }
void settings_clamp_color(void) {
    g_settings.brightness = clampf_(g_settings.brightness, 0.5f, 1.5f);
    g_settings.contrast = clampf_(g_settings.contrast, 0.5f, 1.5f);
    g_settings.saturation = clampf_(g_settings.saturation, 0.0f, 2.0f);
    if (g_settings.shadow_filter < SHADOW_GAME || g_settings.shadow_filter > SHADOW_SMOOTH) g_settings.shadow_filter = SHADOW_SMOOTH;
    g_settings.taa_sharpness = g_settings.taa_sharpness != g_settings.taa_sharpness ? 0.5f : clampf_(g_settings.taa_sharpness, 0.0f, 1.0f);
    g_settings.fsr3_sharpness = g_settings.fsr3_sharpness != g_settings.fsr3_sharpness ? 0.5f : clampf_(g_settings.fsr3_sharpness, 0.0f, 1.0f);
    settings_apply_levels();
}

// SSAO and GI by level (F12 menu). High: the parameters they had before the levels (SSAO 16 samples; GI 4 rays x 8
// steps at half the scene's size). Medium and low keep the look (strength, reach, accumulation over frames) with less
// work: SSAO 12 / 8 samples (the accumulation evens out the noise); GI 3 rays x 6 steps at half the size, 2 x 6 at a
// third (about 1/2 and 1/6 of high's). The upscaler: FSR 3.1 when it is the anti-aliasing, else bilinear (FSR 1 stays
// only as FSR 3.1's fallback without its DLL: exec.c post_process).
void settings_apply_levels(void) {
    int s = g_settings.ssao_level < FX_OFF || g_settings.ssao_level > FX_ULTRA ? FX_OFF : g_settings.ssao_level;
    int g = g_settings.gi_level < FX_OFF || g_settings.gi_level > FX_HIGH ? FX_OFF : g_settings.gi_level;
    g_settings.ssao_level = s, g_settings.gi_level = g;
    static const int ssao_samples[] = {16, 8, 12, 16, 16};
    g_settings.ssao = s != FX_OFF;
    // Ultra: high plus the contact shadows (8 steps, strength 0.375: ~half the GPU time of 16 steps at 0.5)
    g_settings.contact_on = s == FX_ULTRA;
    g_settings.contact_steps = 8;
    g_settings.contact_strength = 0.375f;
    g_settings.ssao_samples = ssao_samples[s];
    g_settings.ssao_intensity = 2.0f, g_settings.ssao_radius = 100.0f, g_settings.ssao_temporal = true;
    static const int gi_rays[] = {4, 2, 3, 4}, gi_steps[] = {8, 6, 6, 8}, gi_div[] = {2, 3, 2, 2};
    g_settings.gi = g != FX_OFF;
    g_settings.gi_rays = gi_rays[g], g_settings.gi_steps = gi_steps[g], g_settings.gi_div = gi_div[g];
    g_settings.gi_intensity = 2.0f, g_settings.gi_radius = 300.0f, g_settings.gi_thickness = 25.0f, g_settings.gi_temporal = true;
    if (g_settings.antialias < AA_OFF || g_settings.antialias > AA_FSR3) g_settings.antialias = AA_SMAA;
    g_settings.upscaler_pick = UPSCALE_BILINEAR;
    g_settings.upscaler = g_settings.antialias == AA_FSR3 ? UPSCALE_FSR3 : UPSCALE_BILINEAR;
    if (g_settings.frame_gen != FG_AUTO && (g_settings.frame_gen < 2 || g_settings.frame_gen > 4)) g_settings.frame_gen = 0;
    if (g_settings.fg_method != FG_FLOW) g_settings.fg_method = FG_RERENDER;
}

// One larecomp.ini line applied now, then the settings that follow from it (LARECOMP_SET_AT: runtime/nvn/vulkan/device.c)
void settings_set_line(const char *line) {
    parse_line(line);
    settings_apply_levels();
}

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) { settings_apply_levels(); return; }
    char line[1200];  // (the mods list can be long)
    while (fgets(line, sizeof line, f)) {
        // (a file edited by hand may have CRLF: "resolution = dynamic\r" did not match and fell back silently)
        for (size_t n = strlen(line); n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '); n--) line[n - 1] = 0;
        // (0.3.1 saved some settings without their line break - "fullscreen = 0fps = 60render_height = 1440": a key
        // glued to the previous value is split off again, so a file written then keeps its settings)
        const char *p = line;
        for (;;) {
            const char *eq = strstr(p, " = "), *next = eq ? strstr(eq + 3, " = ") : NULL, *k = next;
            while (k && k > eq + 3 && ((k[-1] >= 'a' && k[-1] <= 'z') || k[-1] == '_')) k--;
            if (!next || k == next || k <= eq + 3) { parse_line(p); break; }
            char part[256];
            snprintf(part, sizeof part, "%.*s", (int)(k - p), p);
            parse_line(part);
            p = k;
        }
    }
    fclose(f);
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    settings_clamp_color();
    LOG("[settings] loaded %s\n", SETTINGS_FILE);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# " RT_APP_NAME " settings (set on the start screen)\n");
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    fprintf(f, "fps = %d\n", g_settings.fps);
    fprintf(f, "render_height = %d\n", g_settings.res_height);
    fprintf(f, "smaa = %d\n", g_settings.antialias != AA_OFF);
    fprintf(f, "aa = %s\n", g_settings.antialias == AA_FSR3 ? "fsr3" : g_settings.antialias == AA_TAA ? "taa" :
                            g_settings.antialias == AA_SMAA ? "smaa" : "off");
    fprintf(f, "upscaler = bilinear\n");  // (FSR 3.1: "aa"; for older versions)
    fprintf(f, "taa_sharpness = %.2f\n", g_settings.taa_sharpness);
    fprintf(f, "fsr3_sharpness = %.2f\n", g_settings.fsr3_sharpness);
    static const char *const presets[] = {"native", "quality", "balanced", "performance", "ultra"};
    fprintf(f, "fsr3_preset = %s\n", presets[g_settings.fsr3_preset >= 0 && g_settings.fsr3_preset <= 4 ? g_settings.fsr3_preset : 1]);
    if (g_settings.frame_gen == FG_AUTO) fprintf(f, "frame_gen = auto\n");
    else fprintf(f, "frame_gen = %d\n", g_settings.frame_gen);
    fprintf(f, "fg_method = %s\n", g_settings.fg_method == FG_FLOW ? "flow" : "rerender");
    fprintf(f, "mod = %s\n", g_settings.mod);
    fprintf(f, "mouse_sens = %.3f\n", g_settings.mouse_sens);
    fprintf(f, "stick_deadzone = %.2f\n", g_settings.stick_deadzone);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "brightness = %.2f\n", g_settings.brightness);
    fprintf(f, "contrast = %.2f\n", g_settings.contrast);
    fprintf(f, "saturation = %.2f\n", g_settings.saturation);
    fprintf(f, "ssao_level = %d\n", g_settings.ssao_level);
    fprintf(f, "gi_level = %d\n", g_settings.gi_level);
    fprintf(f, "mv_objects = %d\n", g_settings.mv_objects);
    fprintf(f, "ui2x = %d\n", g_settings.ui2x);
    fprintf(f, "blur_quality = %d\n", g_settings.blur_quality);
    fprintf(f, "shadow_filter = %s\n", g_settings.shadow_filter == SHADOW_GAME ? "game" :
                                       g_settings.shadow_filter == SHADOW_BILINEAR ? "bilinear" : "smooth");
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "render_thread = %d\n", g_settings.render_thread);
    fprintf(f, "vertex_read = %s\n", g_settings.vertex_read == VREAD_DIRECT ? "direct" : g_settings.vertex_read == VREAD_COPY ? "copy" : "auto");
    fprintf(f, "dpad_move = %d\n", g_settings.dpad_move);
    fprintf(f, "wild_battle = %d\n", g_settings.wild_battle);
    fprintf(f, "skip_controller = %d\n", g_settings.skip_controller);
    fprintf(f, "game_lang = %s\n", g_settings.game_lang);
    bind_settings_save(f);
    fclose(f);
}
