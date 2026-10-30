// AMD FidelityFX Super Resolution 3.1 upscaling (C++: the SDK's Vulkan header is) (FidelityFX SDK 1.1.4: the newest FSR with a Vulkan backend - the FSR
// SDK 2.x, FSR 4, is DirectX 12 only), through its C API (third_party/fidelityfx, MIT) and the signed prebuilt
// amd_fidelityfx_vk.dll, loaded at run time next to the exe: without it the upscaler stays FSR 1 (exec.c).
// exec.c post_process hands it the game image at the render resolution with the scene's depth, the motion vectors
// (camera and per object) and a mask of the interface; the scene was jittered by fsr3_jitter's sequence.
#ifdef _WIN32
#include <windows.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "vk.h"
#include "fsr3.h"
}
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
#include "ffx_api/vk/ffx_api_vk.h"

// The FFX API: on Windows AMD's signed amd_fidelityfx_vk.dll, loaded at run time; on Android the SDK's own sources built
// into build/deps-android/lib/libffx_fsr3.a (tools/build_ffx_android.sh) and linked in when it was made
// (LARECOMP_FFX_STATIC, Makefile)
#if defined(_WIN32) || defined(LARECOMP_FFX_STATIC)
#define FSR3_FX 1
#endif
#ifdef _WIN32
#include "ffx_api/ffx_api_loader.h"
static ffxFunctions fx;
#elif defined(LARECOMP_FFX_STATIC)
static struct {
    PfnFfxCreateContext CreateContext;
    PfnFfxDestroyContext DestroyContext;
    PfnFfxConfigure Configure;
    PfnFfxQuery Query;
    PfnFfxDispatch Dispatch;
} fx = {ffxCreateContext, ffxDestroyContext, ffxConfigure, ffxQuery, ffxDispatch};
#endif
// (a local research hook - its header is not part of the repository: it wraps the Vulkan functions the provider asks for)
#if __has_include("ditto_capture.h")
#include <stdarg.h>
#include "ditto_capture.h"
#endif
// (DittoSR, a local test upscaler - its library is not part of the repository either: dittosr_* below)
#if __has_include("ditto/ditto_sr.h")
#ifdef __ANDROID__
extern "C" const char *rt_data_root(void);
#endif
#include "ditto/ditto_sr.h"
#define DITTOSR 1
#endif
static int loaded = -1;  // -1 not tried, 0 missing, 1 loaded
static ffxContext ctx;
static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;

static void fsr3_message(uint32_t type, const wchar_t *msg) {
    char m[512];
    snprintf(m, sizeof m, "%ls", msg);
    LOG("[fsr3] %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", m);
}

extern "C" bool fsr3_available(void) {
#ifdef _WIN32
    if (loaded < 0) {
        HMODULE m = LoadLibraryA("amd_fidelityfx_vk.dll");
        loaded = m != NULL;
        if (m) ffxLoadFunctions(&fx, m);
        if (loaded && (!fx.CreateContext || !fx.Dispatch || !fx.Query || !fx.DestroyContext)) loaded = 0;
        LOG("[fsr3] amd_fidelityfx_vk.dll %s\n", loaded ? "loaded" : "not found: FSR 1 instead");
    }
    return loaded == 1;
#elif defined(LARECOMP_FFX_STATIC)
    if (loaded < 0) loaded = 1, LOG("[fsr3] FidelityFX SDK 1.1.4 built in (FSR 3.1 upscaler, Vulkan)\n");
    return loaded == 1;
#else
    return false;
#endif
}

// The backend asks the device for its functions; one this device lacks (the debug-utils names and labels of an
// extension not enabled) came back null and was called: a crash in the DLL. Those get a function doing nothing.
static VkResult VKAPI_CALL fsr3_noop(void) { return VK_SUCCESS; }
// In a profiled frame (LARECOMP_GPUPROF_AT) each of its passes a row of its own: "FSR 3.1 pass <n> (<groups>)"
extern "C" void vk_gp_mark_str(VkCommandBuffer cb, const char *s);
extern "C" bool g_gpuprof_frame;
static PFN_vkCmdDispatch real_dispatch;
static int fsr3_pass;
static void VKAPI_CALL fsr3_dispatch_marked(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (g_gpuprof_frame) {
        char m[64];
        snprintf(m, sizeof m, "FSR 3.1 pass %d (%ux%ux%u groups)", fsr3_pass++, x, y, z);
        vk_gp_mark_str(cb, m);
    }
    real_dispatch(cb, x, y, z);
}
static PFN_vkVoidFunction fsr3_device_proc_(VkDevice dev, const char *name);
static PFN_vkVoidFunction VKAPI_CALL fsr3_device_proc(VkDevice dev, const char *name) {
#ifdef DITTO_CAPTURE
    return ditto::wrap(name, fsr3_device_proc_(dev, name));
#else
    return fsr3_device_proc_(dev, name);
#endif
}
static PFN_vkVoidFunction fsr3_device_proc_(VkDevice dev, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
    if (!strcmp(name, "vkCmdDispatch") && f) {
        real_dispatch = (PFN_vkCmdDispatch)f;
        return (PFN_vkVoidFunction)fsr3_dispatch_marked;
    }
    size_t n = strlen(name);
    if (!f && n > 3 && !strcmp(name + n - 3, "KHR") && n < 128) {  // (an extension's name for what Vulkan 1.1+ has in its core)
        char core[128];
        snprintf(core, sizeof core, "%.*s", (int)(n - 3), name);
        f = vkGetDeviceProcAddr(dev, core);
    }
    if (!f) {
        LOG("[fsr3] the backend asked for %s: not on this device, a no-op\n", name);
        f = (PFN_vkVoidFunction)fsr3_noop;
    }
    return f;
}

#ifdef FSR3_FX
static bool ensure_context(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    if (ctx && ctx_rw == rw && ctx_rh == rh && ctx_ow == ow && ctx_oh == oh) return true;
    if (ctx) {
        vkDeviceWaitIdle(g_vk.dev);
        fx.DestroyContext(&ctx, NULL);
        ctx = NULL;
    }
    ffxCreateBackendVKDesc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.vkDevice = g_vk.dev, be.vkPhysicalDevice = g_vk.phys, be.vkDeviceProcAddr = fsr3_device_proc;
    // the game image is gamma-encoded (perceptual): FSR linearises it and writes the output the same way
    ffxCreateContextDescUpscale cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, cd.header.pNext = &be.header;
    cd.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    cd.maxRenderSize.width = rw, cd.maxRenderSize.height = rh, cd.maxUpscaleSize.width = ow, cd.maxUpscaleSize.height = oh;
    cd.fpMessage = fsr3_message;
    ffxReturnCode_t r = fx.CreateContext(&ctx, &cd.header, NULL);
    if (r != FFX_API_RETURN_OK) {
        LOG("[fsr3] context %ux%u -> %ux%u failed (%u): FSR 1 instead\n", rw, rh, ow, oh, r);
        ctx = NULL;
        loaded = 0;
        return false;
    }
    ctx_rw = rw, ctx_rh = rh, ctx_ow = ow, ctx_oh = oh;
    // (what picks its shaders' variants: 16-bit floats, 64-wide waves - ffx_fsr3upscaler.cpp's permutations)
    VkPhysicalDeviceSubgroupSizeControlProperties sz = {};
    sz.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
    VkPhysicalDeviceProperties2 p2 = {};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, p2.pNext = &sz;
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    VkPhysicalDeviceVulkan12Features f12 = {};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 f2 = {};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
    LOG("[fsr3] context %ux%u -> %ux%u (waves %u..%u, fp16 %d)\n", rw, rh, ow, oh, sz.minSubgroupSize, sz.maxSubgroupSize,
        (int)f12.shaderFloat16);
    return true;
}
#endif

extern "C" void fsr3_jitter(uint32_t rw, uint32_t ow, uint64_t frame, float *jx, float *jy) {
    *jx = *jy = 0;
#ifdef FSR3_FX
    if (!fsr3_available() || !rw || !ow) return;
    int32_t phases = 8;
    ffxQueryDescUpscaleGetJitterPhaseCount pq = {};
    pq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    pq.renderWidth = rw, pq.displayWidth = ow, pq.pOutPhaseCount = &phases;
    fx.Query(NULL, &pq.header);  // (a query without a context: the API's static queries)
    if (phases < 1) phases = 8;
    ffxQueryDescUpscaleGetJitterOffset jq = {};
    jq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    jq.index = (int32_t)(frame % (uint64_t)phases), jq.phaseCount = phases, jq.pOutX = jx, jq.pOutY = jy;
    fx.Query(NULL, &jq.header);
#else
    (void)frame;
#endif
}

static FfxApiResource res(VkImage img, uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage) {
    FfxApiResourceDescription d = {};
    d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D, d.format = fmt, d.width = w, d.height = h, d.depth = 1, d.mipCount = 1;
    d.flags = 0, d.usage = usage;
    return ffxApiGetResourceVK((void *)img, d, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);  // (every image here: GENERAL)
}

extern "C" bool fsr3_dispatch(VkCommandBuffer cb, const Fsr3Dispatch *in) {
#ifdef FSR3_FX
    if (!fsr3_available() || !ensure_context(in->rw, in->rh, in->ow, in->oh)) return false;
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    d.color = res(in->color, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, in->rw, in->rh, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.depth = res(in->depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, in->rw, in->rh, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.motionVectors = res(in->mv, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, in->mvw, in->mvh, FFX_API_RESOURCE_USAGE_READ_ONLY);
    if (in->mask) d.transparencyAndComposition = res(in->mask, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, in->rw, in->rh, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.output = res(in->output, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, in->ow, in->oh, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset.x = in->jx, d.jitterOffset.y = in->jy;
    d.motionVectorScale.x = in->mv_scale_x, d.motionVectorScale.y = in->mv_scale_y;
    d.renderSize.width = in->rw, d.renderSize.height = in->rh;
    d.upscaleSize.width = in->ow, d.upscaleSize.height = in->oh;
    d.enableSharpening = in->sharpness > 0.01f;
    d.sharpness = in->sharpness;
    d.frameTimeDelta = in->frame_ms;
    d.preExposure = 1.0f;
    d.reset = in->reset;
    d.cameraNear = in->near_;
    d.cameraFar = in->far_;
    d.cameraFovAngleVertical = in->fov_y;
    d.viewSpaceToMetersFactor = in->meters_per_unit;
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    fsr3_pass = 0;
#ifdef DITTO_CAPTURE
    ditto::before_upscale();
#endif
    ffxReturnCode_t r = fx.Dispatch(&ctx, &d.header);
#ifdef DITTO_CAPTURE
    ditto::after_upscale(cb);
#endif
    if (r != FFX_API_RETURN_OK) {
        static int n;
        if (n++ < 4) LOG("[fsr3] dispatch failed (%u)\n", r);
        return false;
    }
    return true;
#else
    (void)cb, (void)in;
    return false;
#endif
}

// LARECOMP_FSR3_OPTISCALER=1 (research): OptiScaler 10 injected as winmm.dll takes the FFX calls and can hand them to
// the AMD Vulkan driver's FSR 4 (amdvkffx64.dll next to it: FSR 4.1.2's INT8 Lite model runs on other GPUs too).
// exec.c post_process_ex then hands it images of its own that live on, and a mask every frame (OptiScaler keeps a
// dispatch's image views until the next one)
// On by itself when a winmm.dll is loaded from the exe's folder (OptiScaler's setup puts it there; Windows' own is in
// System32): without the switch, OptiScaler crashed the NVIDIA driver after the first loading screen. =0 / =1 force it.
extern "C" bool fsr3_optiscaler(void) {
    static int on = -1;
    if (on >= 0) return on > 0;
    const char *e = getenv("LARECOMP_FSR3_OPTISCALER");
    on = e ? atoi(e) != 0 : 0;
#ifdef _WIN32
    char dll[MAX_PATH], exe[MAX_PATH];
    HMODULE m = GetModuleHandleA("winmm.dll");
    if (!e && m && GetModuleFileNameA(m, dll, sizeof dll) && GetModuleFileNameA(NULL, exe, sizeof exe)) {
        char *a = strrchr(dll, '\\'), *b = strrchr(exe, '\\');
        if (a && b && a - dll == b - exe && !_strnicmp(dll, exe, (size_t)(a - dll))) on = 1;
    }
#endif
    if (on) LOG("[fsr3] OptiScaler mode%s: inputs in images of their own, a mask every frame\n", e ? " (LARECOMP_FSR3_OPTISCALER)" : " (winmm.dll next to the exe)");
    return on > 0;
}

extern "C" void fsr3_shutdown(void) {
#ifdef FSR3_FX
    if (ctx) { vkDeviceWaitIdle(g_vk.dev); fx.DestroyContext(&ctx, NULL); ctx = NULL; }
#endif
#ifdef DITTOSR
    dittosr::shutdown();
#endif
}

// DittoSR (aa = dittosr): a local test upscaler with FSR 3.1's inputs, its library and data kept out of the repository
// (ditto/ditto_sr.h, <data root>/dittosr/). Without them it is not offered, and chosen it falls back to FSR 1.
extern "C" bool dittosr_present(void) {  // (its data there: no GPU work - the start screen asks)
#ifdef DITTOSR
    char path[1100];
#ifdef __ANDROID__
    snprintf(path, sizeof path, "%s/dittosr/sm_000.spv", rt_data_root());
#else
    snprintf(path, sizeof path, "dittosr/sm_000.spv");
#endif
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
#else
    return false;
#endif
}
extern "C" bool dittosr_available(void) {
#ifdef DITTOSR
    return dittosr::available();
#else
    return false;
#endif
}
extern "C" bool dittosr_dispatch(VkCommandBuffer cb, const Fsr3Dispatch *in) {
#ifdef DITTOSR
    return dittosr::dispatch(cb, in);
#else
    (void)cb, (void)in;
    return false;
#endif
}
