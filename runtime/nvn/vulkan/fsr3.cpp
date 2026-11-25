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
#ifdef _WIN32
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
#include "ffx_api/vk/ffx_api_vk.h"
#include "ffx_api/ffx_api_loader.h"
static ffxFunctions fx;
#else
typedef void *ffxContext;  // (no FSR 3.1 off Windows: the DLL is the only Vulkan backend of the SDK)
#endif
static int loaded = -1;  // -1 not tried, 0 missing, 1 loaded
static ffxContext ctx;
static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;

#ifdef _WIN32
static void fsr3_message(uint32_t type, const wchar_t *msg) {
    char m[512];
    snprintf(m, sizeof m, "%ls", msg);
    LOG("[fsr3] %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", m);
}
#endif

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
#else
    return false;
#endif
}

// The backend asks the device for its functions; one this device lacks (the debug-utils names and labels of an
// extension not enabled) came back null and was called: a crash in the DLL. Those get a function doing nothing.
static VkResult VKAPI_CALL fsr3_noop(void) { return VK_SUCCESS; }
static PFN_vkVoidFunction VKAPI_CALL fsr3_device_proc(VkDevice dev, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
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

#ifdef _WIN32
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
    LOG("[fsr3] context %ux%u -> %ux%u\n", rw, rh, ow, oh);
    return true;
}
#endif

extern "C" void fsr3_jitter(uint32_t rw, uint32_t ow, uint64_t frame, float *jx, float *jy) {
    *jx = *jy = 0;
#ifdef _WIN32
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

#ifdef _WIN32
static FfxApiResource res(VkImage img, uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage) {
    FfxApiResourceDescription d = {};
    d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D, d.format = fmt, d.width = w, d.height = h, d.depth = 1, d.mipCount = 1;
    d.flags = 0, d.usage = usage;
    return ffxApiGetResourceVK((void *)img, d, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);  // (every image here: GENERAL)
}
#endif

extern "C" bool fsr3_dispatch(VkCommandBuffer cb, const Fsr3Dispatch *in) {
#ifdef _WIN32
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
    ffxReturnCode_t r = fx.Dispatch(&ctx, &d.header);
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

extern "C" void fsr3_shutdown(void) {
#ifdef _WIN32
    if (ctx) { vkDeviceWaitIdle(g_vk.dev); fx.DestroyContext(&ctx, NULL); ctx = NULL; }
#endif
}
