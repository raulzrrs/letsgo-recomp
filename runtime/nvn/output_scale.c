// Output resolution, shared by the Metal and Vulkan backends (each calls output_scale() once per presented frame).
// Render targets the game sizes for the Switch's 1920x1080 screen are allocated at (sx, sy) times their size; the
// image is then drawn into (vx, vy, vw, vh) of the window. Video tab "Internal resolution" (g_settings.res_mode):
//   Match display  render at the window's own pixel size (default)
//   1x .. 4x       N times the Switch's docked image: 1080 lines x N (wide windows keep their shape: 32:9 at 2x =
//                  7680 x 2160); capped to the GPU's 16384-pixel texture limit
//   .75x / .50x / .25x  of Match display (scaled up to the window - with MetalFX on macOS when chosen)
//   FSR 3.1        (the upscaler) Match display / the preset's ratio: 1.0 Native AA ... 3.0 Ultra Performance
//   Dynamic        Match display, lowered in steps while the GPU can't hold 60 fps and raised again when it can
//                  (each change reallocates the render targets: a short hitch, so it changes rarely, with hysteresis)
// Test overrides: LARECOMP_SCALE=<f> (render height = 1080 x f), LARECOMP_WIDE_TEST=<f> (forced shape).
#include <math.h>
#include <stdlib.h>

#include "../hle.h"
#include "../settings.h"
#include "output_scale.h"

#define MAX_TEX 16384.0

// Dynamic resolution: a factor on the Match display scale, 0.5 .. 1 in 0.05 steps.
static double g_dyn = 1.0;
static double g_dyn_gpu_sum, g_dyn_dt_sum, g_last_t;
static int g_dyn_n, g_dyn_gpu_n, g_dyn_good;
static double g_dyn_cool_until, g_dyn_probe_wait = 5.0, g_dyn_probe_at;
static double g_dyn_last_raise = -100;
static double g_dyn_target_ms;

static void dyn_update(double now, double gpu_ms, double target_ms) {
    // Do not mix 30 Hz scene samples with 60 Hz world/menu samples.
    if (g_dyn_target_ms != target_ms) {
        g_dyn_target_ms = target_ms;
        g_last_t = now;
        g_dyn_dt_sum = g_dyn_gpu_sum = 0;
        g_dyn_n = g_dyn_gpu_n = g_dyn_good = 0;
        return;
    }
    double dt = g_last_t > 0 ? (now - g_last_t) * 1000.0 : target_ms;
    g_last_t = now;
    if (dt > 250) return;  // (loading, menus, a stall: not the GPU's steady state)
    g_dyn_dt_sum += dt;
    g_dyn_n++;
    if (gpu_ms >= 0) { g_dyn_gpu_sum += gpu_ms; g_dyn_gpu_n++; }
    if (g_dyn_n < 30) return;  // decide every ~half second
    double avg_dt = g_dyn_dt_sum / g_dyn_n, avg_gpu = g_dyn_gpu_n ? g_dyn_gpu_sum / g_dyn_gpu_n : -1;
    g_dyn_dt_sum = g_dyn_gpu_sum = 0;
    g_dyn_n = g_dyn_gpu_n = 0;
    if (now < g_dyn_cool_until) return;
    double prev = g_dyn;
    bool slow = avg_dt > target_ms * (17.4 / 16.666667) ||
                (avg_gpu >= 0 && avg_gpu > target_ms * (14.5 / 16.666667));
    if (slow) {
        g_dyn = fmax(0.5, g_dyn - (avg_dt > target_ms * (22.0 / 16.666667) ? 0.1 : 0.05));
        g_dyn_good = 0;
        if (now - g_dyn_last_raise < 3.0) g_dyn_probe_wait = fmin(g_dyn_probe_wait * 2, 60.0);  // raised too eagerly
    } else if (g_dyn < 1.0) {
        g_dyn_good++;
        // with GPU timing: raise when there is clear headroom; without (Vulkan): probe upwards now and then
        bool room = avg_gpu >= 0 ? avg_gpu < target_ms * (10.5 / 16.666667) && g_dyn_good >= 4 :
                                   now >= g_dyn_probe_at && g_dyn_good >= 2 * (int)g_dyn_probe_wait;
        if (room) { g_dyn = fmin(1.0, g_dyn + 0.05); g_dyn_good = 0; g_dyn_last_raise = now; g_dyn_probe_at = now + g_dyn_probe_wait; }
    }
    g_dyn = round(g_dyn * 20) / 20;
    if (g_dyn != prev) {
        g_dyn_cool_until = now + 1.5;
        LOG("[dynres] %.2f -> %.2f (frame %.1f ms, gpu %.1f ms)\n", prev, g_dyn, avg_dt, avg_gpu);
    }
}

double output_scale_dynamic_factor(void) { return g_dyn; }

void output_scale(double dw, double dh, bool reshape_allowed, double now, double gpu_ms, OutScale *o) {
    double fit = fmin(dw / 1920.0, dh / 1080.0);
    static double forced = -1, wide_test = -1;
    if (forced < 0) forced = getenv("LARECOMP_SCALE") ? atof(getenv("LARECOMP_SCALE")) : 0;
    if (wide_test < 0) wide_test = getenv("LARECOMP_WIDE_TEST") ? atof(getenv("LARECOMP_WIDE_TEST")) : 0;
    double shape = wide_test > 0 ? wide_test : (dw / dh) / (16.0 / 9.0);
    bool reshape = reshape_allowed && fabs(shape - 1.0) > 0.005;
    if (!reshape) shape = 1.0;
    // render height in units of the Switch's 1080 lines
    double h;
    int mode = g_settings.res_mode;
    if (forced > 0) h = forced;
    // FSR 3.1, DittoSR, DittoSR (FSR4): the window's resolution divided by its preset's ratio (the launcher's choice does
    // not apply)
    else if (g_settings.upscaler == UPSCALE_FSR3 || UPSCALE_IS_DITTOSR(g_settings.upscaler) || UPSCALE_IS_FSR4(g_settings.upscaler))
        h = (reshape ? dh / 1080.0 : fit) / settings_fsr3_ratio();
    else if (g_settings.res_height > 0) h = g_settings.res_height / 1080.0;  // (the launcher's choice)
    else if (mode >= 1 && mode <= 8) h = mode;
    else {
        h = reshape ? dh / 1080.0 : fit;  // Match display
        if (mode == RES_DYNAMIC) {
            extern bool fps60_enabled(void), fps60_world(void), fps60_menu(void);
            extern int fps60_rate(void);
            double budget = !fps60_enabled() ? 33.333333 : fps60_world() ? 1000.0 / fps60_rate() : fps60_menu() ? 16.666667 : 33.333333;
            dyn_update(now, gpu_ms, budget);
            h *= g_dyn;
        }
        else if (mode == RES_Q75) h *= 0.75;
        else if (mode == RES_Q25) h *= 0.25;
        else if (mode == RES_Q50) h *= 0.5;
    }
    h = fmax(h, 0.1);  // (.25x of a small window)
    // The temporal upscalers: a scale that makes the Switch's 1920 x 1080 whole pixels, a multiple of 4 on each side.
    // The render targets are rounded to whole pixels (textures.c) but the game draws at the exact scale: Balanced's
    // 1 / 1.7 at 4K drew 2258.8 x 1270.6 into 2259 x 1271, a stretch the upscaler does not know of - its history blurred
    // (FSR 3.1, GSR 2 and DittoSR (FSR4) alike). 4K Balanced: 2240 x 1260, 1.71x; the other presets were whole already.
    bool snap = forced <= 0 && (g_settings.upscaler == UPSCALE_FSR3 || UPSCALE_IS_DITTOSR(g_settings.upscaler) ||
                                UPSCALE_IS_FSR4(g_settings.upscaler));
    if (snap && !reshape) h = fmax(round(h * 30) / 30, 1.0 / 30);
    double sx = h * shape, sy = h;
    if (snap && reshape) sx = fmax(round(sx * 480) / 480, 1.0 / 480), sy = fmax(round(sy * 270) / 270, 1.0 / 270);
    double cap = fmin(MAX_TEX / (1920.0 * sx), MAX_TEX / (1080.0 * sy));
    if (cap < 1.0) { sx *= cap; sy *= cap; }
    o->sx = sx; o->sy = sy; o->shape = shape;
    // where the image goes in the window: fills it when reshaped, else a fitted 16:9 box
    if (reshape) { o->vw = dw; o->vh = dh; }
    else { o->vw = 1920.0 * fit; o->vh = 1080.0 * fit; }
    if (wide_test > 0) {
        double f2 = fmin(dw / (1920.0 * wide_test), dh / 1080.0);
        o->vw = 1920.0 * wide_test * f2; o->vh = 1080.0 * f2;
    }
    o->vx = (dw - o->vw) / 2; o->vy = (dh - o->vh) / 2;
}
