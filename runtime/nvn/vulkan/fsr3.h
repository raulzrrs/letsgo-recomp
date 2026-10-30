// FSR 3.1 upscaling (runtime/nvn/vulkan/fsr3.c): the AMD FidelityFX DLL, loaded at run time.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkImage color, depth, mv, mask, output;  // color RGBA8 (gamma-encoded), depth R32F, mv RGBA16F, mask RGBA8 (or none)
    uint32_t rw, rh, ow, oh, mvw, mvh;       // render size, output size, the motion vectors' size
    float jx, jy;                            // this frame's jitter, in render pixels (fsr3_jitter)
    float mv_scale_x, mv_scale_y;            // the motion vectors to pixels, previous - current
    float near_, far_, fov_y, meters_per_unit, frame_ms, sharpness;
    bool reset;                              // no history (a cut, the first frame)
    bool exp;                                // (always false: DittoSR Experiment's variant, removed - ditto_sr.h reads it)
} Fsr3Dispatch;

bool fsr3_available(void);
// the jitter sequence for render width rw upscaled to ow: frame's offset in pixels
void fsr3_jitter(uint32_t rw, uint32_t ow, uint64_t frame, float *jx, float *jy);
// records the upscale into cb (outside any rendering); false: not done (use FSR 1)
bool fsr3_dispatch(VkCommandBuffer cb, const Fsr3Dispatch *d);
void fsr3_shutdown(void);
// OptiScaler is injected (winmm.dll next to the exe, or LARECOMP_FSR3_OPTISCALER=1) and takes the FFX calls - fsr3.cpp
bool fsr3_optiscaler(void);
// DittoSR (FSR4) (upscaler = dittosr_fsr4), a local test upscaler with FSR 3.1's inputs: its library and data are not part
// of the repository.
// present: its data is there (no GPU work); available: ready on the GPU; dispatch: as fsr3_dispatch
bool dittosr_present(void);
bool dittosr_available(void);
bool dittosr_dispatch(VkCommandBuffer cb, const Fsr3Dispatch *d);

