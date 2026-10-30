// Vulkan textures: NVN formats, images, change-detected uploads from guest memory, views and samplers
// (counterpart of the texture half of metal.m and the view helpers of renderer.m).
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vk.h"
#include "../astc.h"
#include "../renderer.h"
#include "../../settings.h"
#include "../../ui2x.h"

// ================================================================== formats
// expand_rgb: converted to RGBA8 on upload - 1 RGB8, 2 RGBA4 and 3 RGB5A1 (16-bit, red in the low bits: the
// Vulkan PACK16 formats put it in the high bits, and the alpha bit at the other end)
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;

static VkFmt fmt_of(uint32_t f) {
    switch (f) {
    case 0x01: return (VkFmt){VK_FORMAT_R8_UNORM};
    case 0x02: return (VkFmt){VK_FORMAT_R8_SNORM};
    case 0x03: return (VkFmt){VK_FORMAT_R8_UINT};
    case 0x04: return (VkFmt){VK_FORMAT_R8_SINT};
    case 0x05: return (VkFmt){VK_FORMAT_R16_SFLOAT};
    case 0x06: return (VkFmt){VK_FORMAT_R16_UNORM};
    case 0x07: return (VkFmt){VK_FORMAT_R16_SNORM};
    case 0x08: return (VkFmt){VK_FORMAT_R16_UINT};
    case 0x09: return (VkFmt){VK_FORMAT_R16_SINT};
    case 0x0A: return (VkFmt){VK_FORMAT_R32_SFLOAT};
    case 0x0B: return (VkFmt){VK_FORMAT_R32_UINT};
    case 0x0C: return (VkFmt){VK_FORMAT_R32_SINT};
    case 0x0D: return (VkFmt){VK_FORMAT_R8G8_UNORM};
    case 0x0E: return (VkFmt){VK_FORMAT_R8G8_SNORM};
    case 0x0F: return (VkFmt){VK_FORMAT_R8G8_UINT};
    case 0x10: return (VkFmt){VK_FORMAT_R8G8_SINT};
    case 0x11: return (VkFmt){VK_FORMAT_R16G16_SFLOAT};
    case 0x12: return (VkFmt){VK_FORMAT_R16G16_UNORM};
    case 0x13: return (VkFmt){VK_FORMAT_R16G16_SNORM};
    case 0x14: return (VkFmt){VK_FORMAT_R16G16_UINT};
    case 0x15: return (VkFmt){VK_FORMAT_R16G16_SINT};
    case 0x16: return (VkFmt){VK_FORMAT_R32G32_SFLOAT};
    case 0x17: return (VkFmt){VK_FORMAT_R32G32_UINT};
    case 0x18: return (VkFmt){VK_FORMAT_R32G32_SINT};
    case 0x19: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 1, 1};  // RGB8: expanded on upload
    case 0x25: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    case 0x26: return (VkFmt){VK_FORMAT_R8G8B8A8_SNORM};
    case 0x27: return (VkFmt){VK_FORMAT_R8G8B8A8_UINT};
    case 0x28: return (VkFmt){VK_FORMAT_R8G8B8A8_SINT};
    case 0x29: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT};
    case 0x2A: return (VkFmt){VK_FORMAT_R16G16B16A16_UNORM};
    case 0x2B: return (VkFmt){VK_FORMAT_R16G16B16A16_SNORM};
    case 0x2C: return (VkFmt){VK_FORMAT_R16G16B16A16_UINT};
    case 0x2D: return (VkFmt){VK_FORMAT_R16G16B16A16_SINT};
    case 0x2E: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT};
    case 0x2F: return (VkFmt){VK_FORMAT_R32G32B32A32_UINT};
    case 0x30: return (VkFmt){VK_FORMAT_R32G32B32A32_SINT};
    case 0x31: return (VkFmt){VK_FORMAT_S8_UINT};
    case 0x32: return (VkFmt){VK_FORMAT_D16_UNORM};
    case 0x33: return (VkFmt){VK_FORMAT_D32_SFLOAT};  // DEPTH24 (as on Metal: no 24-bit depth there)
    case 0x34: return (VkFmt){VK_FORMAT_D32_SFLOAT};
    case 0x35: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x36: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x37: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB, 0, 1};
    case 0x38: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB};
    case 0x39: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 2};      // RGBA4 (Metal ABGR4Unorm): expanded on upload
    case 0x3A: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};     // Metal B5G6R5Unorm
    case 0x3B: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 3};      // RGB5A1 (Metal A1BGR5Unorm): expanded on upload
    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};
    case 0x3D: return (VkFmt){VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    case 0x3E: return (VkFmt){VK_FORMAT_A2B10G10R10_UINT_PACK32};
    case 0x3F: return (VkFmt){VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    case 0x40: return (VkFmt){VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    case 0x41: case 0x42: return (VkFmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    case 0x43: return (VkFmt){VK_FORMAT_BC2_UNORM_BLOCK};
    case 0x44: return (VkFmt){VK_FORMAT_BC3_UNORM_BLOCK};
    case 0x45: case 0x46: return (VkFmt){VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
    case 0x47: return (VkFmt){VK_FORMAT_BC2_SRGB_BLOCK};
    case 0x48: return (VkFmt){VK_FORMAT_BC3_SRGB_BLOCK};
    case 0x49: return (VkFmt){VK_FORMAT_BC4_UNORM_BLOCK};
    case 0x4A: return (VkFmt){VK_FORMAT_BC4_SNORM_BLOCK};
    case 0x4B: return (VkFmt){VK_FORMAT_BC5_UNORM_BLOCK};
    // 0x4C = signed RGTC2; LARECOMP_BC5_UNORM=1 decodes it unsigned (see metal.m / notes/PLAN.md)
    case 0x4C: return (VkFmt){getenv("LARECOMP_BC5_UNORM") ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK};
    case 0x4D: return (VkFmt){VK_FORMAT_BC7_UNORM_BLOCK};
    case 0x4E: return (VkFmt){VK_FORMAT_BC7_SRGB_BLOCK};
    case 0x4F: return (VkFmt){VK_FORMAT_BC6H_SFLOAT_BLOCK};
    case 0x50: return (VkFmt){VK_FORMAT_BC6H_UFLOAT_BLOCK};
    case 0x6D: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 1};
    case 0x71: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1};
    case 0x76: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT, 0, 1};
    case 0x79: return (VkFmt){VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    case 0x7A: return (VkFmt){VK_FORMAT_ASTC_5x4_UNORM_BLOCK};
    case 0x7B: return (VkFmt){VK_FORMAT_ASTC_5x5_UNORM_BLOCK};
    case 0x7C: return (VkFmt){VK_FORMAT_ASTC_6x5_UNORM_BLOCK};
    case 0x7D: return (VkFmt){VK_FORMAT_ASTC_6x6_UNORM_BLOCK};
    case 0x7E: return (VkFmt){VK_FORMAT_ASTC_8x5_UNORM_BLOCK};
    case 0x7F: return (VkFmt){VK_FORMAT_ASTC_8x6_UNORM_BLOCK};
    case 0x80: return (VkFmt){VK_FORMAT_ASTC_8x8_UNORM_BLOCK};
    case 0x81: return (VkFmt){VK_FORMAT_ASTC_10x5_UNORM_BLOCK};
    case 0x82: return (VkFmt){VK_FORMAT_ASTC_10x6_UNORM_BLOCK};
    case 0x83: return (VkFmt){VK_FORMAT_ASTC_10x8_UNORM_BLOCK};
    case 0x84: return (VkFmt){VK_FORMAT_ASTC_10x10_UNORM_BLOCK};
    case 0x85: return (VkFmt){VK_FORMAT_ASTC_12x10_UNORM_BLOCK};
    case 0x86: return (VkFmt){VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
    case 0x87: return (VkFmt){VK_FORMAT_ASTC_4x4_SRGB_BLOCK};
    case 0x88: return (VkFmt){VK_FORMAT_ASTC_5x4_SRGB_BLOCK};
    case 0x89: return (VkFmt){VK_FORMAT_ASTC_5x5_SRGB_BLOCK};
    case 0x8A: return (VkFmt){VK_FORMAT_ASTC_6x5_SRGB_BLOCK};
    case 0x8B: return (VkFmt){VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    case 0x8C: return (VkFmt){VK_FORMAT_ASTC_8x5_SRGB_BLOCK};
    case 0x8D: return (VkFmt){VK_FORMAT_ASTC_8x6_SRGB_BLOCK};
    case 0x8E: return (VkFmt){VK_FORMAT_ASTC_8x8_SRGB_BLOCK};
    case 0x8F: return (VkFmt){VK_FORMAT_ASTC_10x5_SRGB_BLOCK};
    case 0x90: return (VkFmt){VK_FORMAT_ASTC_10x6_SRGB_BLOCK};
    case 0x91: return (VkFmt){VK_FORMAT_ASTC_10x8_SRGB_BLOCK};
    case 0x92: return (VkFmt){VK_FORMAT_ASTC_10x10_SRGB_BLOCK};
    case 0x93: return (VkFmt){VK_FORMAT_ASTC_12x10_SRGB_BLOCK};
    case 0x94: return (VkFmt){VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
    case 0x95: case 0x96: case 0x97: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    case 0x98: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM, 0, 1};
    case 0x99: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM};
    case 0x9A: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB, 0, 1};
    case 0x9B: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB};
    default: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    }
}

bool renderer_format_is_depth(uint32_t f) { return f >= 0x31 && f <= 0x36; }
bool renderer_format_has_stencil(uint32_t f) { return f == 0x31 || f == 0x35 || f == 0x36; }
VkFormat vk_format(uint32_t f) { return fmt_of(f).vf; }
bool vk_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }
bool renderer_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }

static VkImageAspectFlags aspect_of(uint32_t f) {
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (renderer_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool format_supports(VkFormat f, VkFormatFeatureFlags want) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

// ================================================================== output resolution / per-frame clears
// Same rules as metal.m (see the comments there).
static float g_rs_x = 1, g_rs_y = 1;
void renderer_get_scale(float *sx, float *sy) { *sx = g_rs_x; *sy = g_rs_y; }
void renderer_set_scale(float sx, float sy) { g_rs_x = sx; g_rs_y = sy; }
uint64_t renderer_present_count(void);

static bool tex_scalable(const NvnTexture *t) {
    if (getenv("LARECOMP_NO_SCALE")) return false;
    if (t->target != 1 || t->samples > 1 || !(t->flags & 8) || t->width < 32 || t->height < 16) return false;
    float a = (float)t->width / (float)t->height;
    return a > 1.74f && a < 1.81f;
}

bool renderer_texture_first_write(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!b || !b->presented || (b->sx == b->sy) || b->touched_frame == total_frames + 1) return false;
    b->touched_frame = total_frames + 1;
    return true;
}

static uint32_t g_level_gen = 1;

bool renderer_texture_frame_fresh(NvnTexture *t) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_NO_FRAME_CLEAR") == NULL;
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!on || !b || !b->scalable || b->frame_cleared == total_frames + 1) return false;
    b->frame_cleared = total_frames + 1;
    return true;
}

bool renderer_texture_level_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    static int keep = -1;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_TARGETS") != NULL;
    if (!b || keep || b->level_gen == g_level_gen) return false;
    if (!b->scalable) { b->level_gen = g_level_gen; return false; }
    bool fresh = b->level_gen != 0;
    b->level_gen = g_level_gen;
    return fresh;
}

void renderer_texture_mark_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b) b->presented = true;
}

bool renderer_texture_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && b->presented;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && b->scalable) { *sx = b->sx; *sy = b->sy; }
    else if (b && b->font_k > 1) { *sx = *sy = b->font_k; }
    else if (b && b->ui2x) { *sx = (float)b->ui2x_w / (float)t->width; *sy = (float)b->ui2x_h / (float)t->height; }
    else { *sx = 1; *sy = 1; }
}

// ================================================================== images
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// Images destroyed by the guest are kept until the submissions that may use them have completed.
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; uint64_t seq; } Dead;
static Dead *dead;
static int ndead, cap_dead;
static void views_forget(TexBackend *b);

void vk_collect_dead(void) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].seq > done) { dead[k++] = dead[i]; continue; }
        if (dead[i].view) vkDestroyImageView(g_vk.dev, dead[i].view, NULL);
        else vmaDestroyImage(g_vk.vma, dead[i].img, dead[i].mem);
    }
    ndead = k;
}

// The game's glyph caches (8-bit linear 2D textures the text is drawn from) are drawn again at K x on the host
// (runtime/hle/font_hires.c): such a texture gets an image K times as large, filled from that K x copy instead of
// guest memory. K, or 1 for every other texture.
int font_hires_scale(uint64_t addr, uint32_t w, uint32_t h);
bool font_hires_begin(uint64_t addr, const uint8_t **px, uint32_t *row, uint32_t *x0, uint32_t *y0, uint32_t *x1,
                      uint32_t *y1);
void font_hires_end(void);
static int font_cache_k(const NvnTexture *t) {
    if (t->target != 1 || !(t->flags & 0x10) || t->levels > 1 || !t->layout.fmt || t->layout.fmt->bpb != 1 || !t->addr) return 1;
    return font_hires_scale(t->addr, t->width, t->height);
}

// The glyph caches' scale (runtime/hle/font_hires.c): one step per 720 lines of the window, up to 4 (1: off).
// Android: always 1.
int renderer_font_scale(void) {
#ifdef __ANDROID__
    return 1;  // (phones: the game's own glyphs - no second rasterization, no 4x larger cache textures)
#else
    uint32_t h = g_vk.sc_extent.height;
    int k = (int)((h + 719) / 720);
    return k < 1 ? 1 : k > 4 ? 4 : k;
#endif
}

static void create_image(NvnTexture *t, TexBackend *b) {
    VkFmt mf = fmt_of(t->format);
    b->sx = b->scalable ? g_rs_x : 1;
    b->sy = b->scalable ? g_rs_y : 1;
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (b->scalable) {
        tw = (uint32_t)lroundf(tw * b->sx); th = (uint32_t)lroundf(th * b->sy);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    b->font_k = font_cache_k(t);
    if (b->font_k > 1) { tw *= b->font_k; th *= b->font_k; }
    if (b->ui2x) tw = b->ui2x_w, th = b->ui2x_h;
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t layers = 1, depth = 1;
    switch (t->target) {
    case 0: type = VK_IMAGE_TYPE_1D; th = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D; depth = t->depth ? t->depth : 1; break;
    case 3: type = VK_IMAGE_TYPE_1D; layers = t->height ? t->height : 1; th = 1; break;
    case 4: case 6: layers = t->depth ? t->depth : 1; break;
    case 8: layers = 6; break;
    case 9: layers = t->depth ? t->depth : 6; break;
    default: break;
    }
    uint32_t levels = t->levels ? t->levels : 1;
    {   // never more levels than the size allows (scaled targets, odd guest values)
        uint32_t m = 1, sz = tw > th ? tw : th;
        if (depth > sz) sz = depth;
        while (sz > 1) { sz >>= 1; m++; }
        if (levels > m) levels = m;
    }
    uint32_t samples = t->samples > 1 ? t->samples : 1;
    if (samples > 1) levels = 1;
    VkFormat vf = mf.vf;
    if (b->ui2x) vf = b->ui2x_fmt, levels = 1;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    b->astc_bw = b->astc_bh = 0;
    if (!format_supports(vf, need)) {
        bool astc = vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        const FormatInfo *fi = t->layout.fmt;
        if (astc && fi && fi->bpb == 16) {  // desktop GPUs: decoded on the CPU into an RGBA8 image of the same colour space
            b->astc_bw = fi->bw;
            b->astc_bh = fi->bh;
            b->astc_srgb = t->format >= 0x87;
            vf = b->astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            static int nlog;
            if (nlog++ < 20) LOG("[vk] format %d (nvn %#x) not supported: RGBA8 instead\n", vf, t->format);
            vf = VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
    bool depthfmt = renderer_format_is_depth(t->format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depthfmt) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format_supports(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateFlags flags = 0;
    if (!depthfmt) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // format views (NVN texture views, raw sRGB views)
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && tw == th && samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = vf,
                            .extent = {tw, th, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkResult r = vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL);
    if (r != VK_SUCCESS) {
        LOG("[vk] image creation failed (%d): nvn fmt %#x %ux%ux%u target %u\n", r, t->format, t->width, t->height, t->depth, t->target);
        ci.format = vf = VK_FORMAT_R8G8B8A8_UNORM;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
        depthfmt = false;
    }
    b->format = vf;
    b->width = tw; b->height = th; b->depth = depth; b->layers = layers; b->levels = levels; b->samples = samples;
    b->aspect = depthfmt ? aspect_of(t->format) : VK_IMAGE_ASPECT_COLOR_BIT;
    b->base_type = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : t->target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || t->target == 4 || t->target == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vk_image_init_layout(vk_upload_cmd(), b->image, b->aspect);
    if (depthfmt) {
        // New depth / stencil images start zeroed (as fresh memory reads on Apple GPUs). The game builds its depth
        // pyramid with depth-only copies, so the stencil of those images was never written: whatever the memory
        // held, different per image and per run, then failed or passed the depth-of-field passes' stencil test in
        // patches (flicker that came and went between runs, every other frame, below 1x internal resolution).
        VkCommandBuffer cb = vk_upload_cmd();
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
    }
}

static void defer(Dead d) {
    if (ndead == cap_dead) { cap_dead = cap_dead ? cap_dead * 2 : 256; dead = realloc(dead, cap_dead * sizeof *dead); }
    d.seq = vk_pending_seq();
    dead[ndead++] = d;
}

void vk_defer_image(VkImage img, VmaAllocation mem) { defer((Dead){.img = img, .mem = mem}); }
void vk_defer_view(VkImageView v) { defer((Dead){.view = v}); }

void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv);  // (exec.c: LARECOMP_VK_DEVLOST_TRACE)

static uint32_t level_w(const TexBackend *b, uint32_t lv) { uint32_t v = b->width >> lv; return v ? v : 1; }
static uint32_t level_h(const TexBackend *b, uint32_t lv) { uint32_t v = b->height >> lv; return v ? v : 1; }

static void destroy_image(TexBackend *b) {
    extern void vk_pclear_forget(TexBackend *b);  // (exec.c: a clear still pending on it)
    vk_pclear_forget(b);
    views_forget(b);
    if (b->image || b->mem) defer((Dead){.img = b->image, .mem = b->mem});  // (never the same allocation twice)
    b->image = VK_NULL_HANDLE;
    b->mem = VK_NULL_HANDLE;
}

// Live game textures: an in-place reboot (vk_textures_reset) destroys them all.
static NvnTexture **g_live;
static int g_nlive, g_cap_live;
static void live_add(NvnTexture *t, TexBackend *b) {
    if (g_nlive == g_cap_live) { g_cap_live = g_cap_live ? g_cap_live * 2 : 1024; g_live = realloc(g_live, g_cap_live * sizeof *g_live); }
    g_live[g_nlive++] = t;
    b->reg = g_nlive;
}
static void live_remove(TexBackend *b) {
    if (!b->reg) return;
    NvnTexture *last = g_live[--g_nlive];
    g_live[b->reg - 1] = last;
    if (last->backend) ((TexBackend *)last->backend)->reg = b->reg;
    b->reg = 0;
}

static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        if (b->scalable && (b->sx != g_rs_x || b->sy != g_rs_y)) {  // output resolution changed: reallocate
            destroy_image(b);
            create_image(t, b);
        }
        return b;
    }
    vk_collect_dead();
    b = calloc(1, sizeof *b);
    b->scalable = tex_scalable(t);
    create_image(t, b);
    t->backend = b;
    live_add(t, b);
    return b;
}

// A texture the game fills from memory (CopyBufferToTexture) holds full-size data, so it can't live at the scaled
// output resolution: Let's Go's movie planes (R8 1920x1080 / RG8 960x540, 16:9 render-target flags) were scaled to
// the window and the full-size copies overran them (device lost). Such a texture stays at its own size from then on.
void vk_tex_unscale(NvnTexture *t) {
    TexBackend *b = tex_backend(t);
    if (!b->scalable) return;
    b->scalable = false;
    if (b->sx != 1 || b->sy != 1) {
        LOG("[vk] texture %ux%u fmt %#x at %llx flags %#x stride %u filled from memory: kept at full size\n", t->width,
            t->height, t->format, (unsigned long long)t->addr, t->flags, (unsigned)t->stride);
        destroy_image(b);
        create_image(t, b);
    }
}

void backend_texture_destroyed(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b && b->full) b = t->backend = b->full;  // (its twin standing in: vk_tex_lo_begin)
    if (b && b->lo) {
        destroy_image(b->lo);
        free(b->lo);
        b->lo = NULL;
    }
    if (b) {
        live_remove(b);
        destroy_image(b);
        free(b->pending_rgba);
        free(b);
    }
    t->backend = NULL;
}

// Reboot (device.c backend_reset; GPU idle): every game texture and its views go.
void vk_textures_reset(void) {
    int n = g_nlive;
    while (g_nlive > 0) backend_texture_destroyed(g_live[g_nlive - 1]);
    vk_collect_dead();
    LOG("[vk] reboot: %d textures released\n", n);
}

// ================================================================== the in-between images at a reduced size
// Frame generation renders its in-between images by replaying frame N's command lists (exec.c vk_interp_replay); at
// 4K the scene alone is 1.3 ms of an image's ~3.7. With LARECOMP_FG_SCALE (research) they are
// rendered smaller: every scaled render target (the game's 1080p-sized ones: scene, depth, bloom, composite) gives way
// to a twin at f x its size for the replay, and the result is resolved against frame N's full-size image (exec.c
// post_process_ex, side_resolve.frag). The game's own code, its viewports and our passes see only smaller targets
// (their scale, sx / sy, is the twin's), so nothing else changes. A twin the replay reads before rendering to it (a
// target the game draws once, not every frame) is filled from its full-size image from then on.
extern uint32_t vk_view_gen;
static bool g_lo_on;
static float g_lo_saved_x, g_lo_saved_y;
static NvnTexture **g_lo_swapped;
static int g_nlo_swapped, g_cap_lo_swapped;
bool vk_tex_lo_active(void) { return g_lo_on; }
TexBackend *vk_tex_lo(TexBackend *b) { return g_lo_on && b && b->lo && b->lo->full == b ? b->lo : b; }
void vk_full_barrier(VkCommandBuffer cb);
void vk_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da);
static void lo_seed(TexBackend *b, TexBackend *lo) {
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    bool depth = (b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
    if (!depth) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (!format_supports(b->format, need) || b->format != lo->format) return;
    VkCommandBuffer cb = vk_cmd();
    vk_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (uint32_t lv = 0; lv < b->levels && lv < lo->levels; lv++) {
        VkImageBlit bl = {.srcSubresource = {b->aspect, lv, 0, b->layers}, .dstSubresource = {lo->aspect, lv, 0, lo->layers},
                          .srcOffsets = {{0, 0, 0}, {(int32_t)level_w(b, lv), (int32_t)level_h(b, lv), 1}},
                          .dstOffsets = {{0, 0, 0}, {(int32_t)level_w(lo, lv), (int32_t)level_h(lo, lv), 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, lo->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                       depth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
    vk_barrier(cb, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}
void vk_tex_lo_begin(float f) {
    if (g_lo_on) return;
    g_lo_saved_x = g_rs_x, g_lo_saved_y = g_rs_y;
    g_rs_x *= f, g_rs_y *= f;  // (the twins' scale: tex_backend keeps them at it, create_image makes them at it)
    g_nlo_swapped = 0;
    for (int i = 0; i < g_nlive; i++) {
        NvnTexture *t = g_live[i];
        TexBackend *b = t->backend;
        if (!b || !b->scalable || !b->image || b->full) continue;
        TexBackend *lo = b->lo;
        if (!lo) lo = b->lo = calloc(1, sizeof *lo), lo->full = b;
        if (!lo->image || lo->sx != g_rs_x || lo->sy != g_rs_y) {
            if (lo->image) destroy_image(lo);
            lo->scalable = true;
            create_image(t, lo);
        }
        // (as its target is now: N rendered it, so the replay neither clears it nor counts a first write)
        lo->presented = b->presented, lo->gpu_owned = true, lo->touched_frame = b->touched_frame;
        lo->frame_cleared = b->frame_cleared, lo->level_gen = b->level_gen;
        lo->lo_use = 0;
        if (b->lo_seed) lo_seed(b, lo);
        if (g_nlo_swapped == g_cap_lo_swapped)
            g_cap_lo_swapped = g_cap_lo_swapped ? g_cap_lo_swapped * 2 : 256,
            g_lo_swapped = realloc(g_lo_swapped, g_cap_lo_swapped * sizeof *g_lo_swapped);
        g_lo_swapped[g_nlo_swapped++] = t;
        t->backend = lo;
    }
    g_lo_on = true;
    vk_view_gen++;  // (exec.c's per-frame view memo: the views are the twins' now)
}
void vk_tex_lo_end(void) {
    if (!g_lo_on) return;
    for (int i = 0; i < g_nlo_swapped; i++) {
        NvnTexture *t = g_lo_swapped[i];
        TexBackend *lo = t->backend;
        if (lo && lo->full) {
            lo->full->presented |= lo->presented;
            t->backend = lo->full;
        }
    }
    g_nlo_swapped = 0;
    g_rs_x = g_lo_saved_x, g_rs_y = g_lo_saved_y;
    g_lo_on = false;
    vk_view_gen++;
}

// ================================================================== uploads (same change detection as metal.m)
static uint64_t full_hash_budget, full_hash_budget_frame;

// Four independent lanes (one serial multiply chain ran at ~1.5 bytes/cycle; dynamic textures are hashed every
// frame). Values are only compared within a run.
static uint64_t hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

// A texture's content key (LARECOMP_TEXKEY_LOG=1: logged at each upload): runtime/ui2x.c's, of level 0's block rows
// untiled - tools/ui_texkeys.py computes the same from the game files.
static void texkey_log(const NvnTexture *t, const uint8_t *lin, uint64_t n, uint32_t bpb) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_TEXKEY_LOG") != NULL;
    if (on) LOG("[texkey] %016llx %ux%u nvn %#x bpb %u levels %u\n", (unsigned long long)ui2x_key(lin, n), t->width, t->height, t->format, bpb, t->levels);
}

static uint64_t fnv_sample(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    uint64_t step = n > 4096 ? n / 4096 : 1;
    for (uint64_t i = 0; i < n; i += step) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

static void upload_pending(TexBackend *b) {
    pthread_mutex_lock(&pending_lock);
    void *px = b->pending_rgba;
    uint32_t w = b->pending_w, h = b->pending_h, stride = b->pending_stride;
    b->pending_rgba = NULL;
    pthread_mutex_unlock(&pending_lock);
    if (!px) return;
    if (w > b->width) w = b->width;
    if (h > b->height) h = b->height;
    VkSpan s = vk_transient_copy(px, (uint64_t)stride * h, 16);
    free(px);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = stride / 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_cmd();
    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
// A glyph cache drawn at K x: the part of its K x copy that changed (all of it the first time).
static void upload_font_cache(NvnTexture *t, TexBackend *b) {
    const uint8_t *px;
    uint32_t row, x0, y0, x1, y1;
    if (!font_hires_begin(t->addr, &px, &row, &x0, &y0, &x1, &y1)) return;
    if (!b->uploaded) { x0 = 0; y0 = 0; x1 = b->width; y1 = b->height; b->uploaded = true; }
    if (x1 > b->width) x1 = b->width;
    if (y1 > b->height) y1 = b->height;
    if (x1 > x0 && y1 > y0 && row >= b->width) {
        uint32_t w = x1 - x0, h = y1 - y0;
        VkSpan s = vk_transient((uint64_t)w * h, 16);
        for (uint32_t y = 0; y < h; y++) memcpy((uint8_t *)s.ptr + (uint64_t)y * w, px + (uint64_t)(y0 + y) * row + x0, w);
        VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = w, .bufferImageHeight = h,
                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {(int32_t)x0, (int32_t)y0, 0},
                                .imageExtent = {w, h, 1}};
        VkCommandBuffer cb = vk_upload_cmd();
        vk_dl_note_upload((void *)b->image, w, h, 0);
        vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    }
    font_hires_end();
}

// A glyph cache drawn at K x (made so if it became one after its image was made): its K x copy is uploaded, whoever
// owns the texture (the game's GPU writes into it are dropped or redone from that copy). False: any other texture.
static bool font_cache_use(NvnTexture *t, TexBackend *b) {
    {
        int fk = font_cache_k(t);
        if (fk != (b->font_k ? b->font_k : 1)) {  // (became a glyph cache after its image was made)
            LOG("[vk] glyph cache %ux%u at %llx: drawn at %dx\n", t->width, t->height, (unsigned long long)t->addr, fk);
            destroy_image(b);
            create_image(t, b);
            b->uploaded = false;
        }
        if (b->font_k > 1) { upload_font_cache(t, b); return true; }
    }
    return false;
}

// An interface texture with a 2x version (runtime/ui2x.c): a plain 2D texture of one level (not a render target or
// linear: flags 8, 0x10), of the formats the
// interface uses, whose level 0 untiled has a key in cache/ui2x.pak. True: b now holds the 2x image (made again at that
// size and format when needed). False: not one (b back to the game's own size if it was one before).
static bool ui2x_candidate(const NvnTexture *t) {
    if (t->target != 1 || t->levels > 1 || (t->flags & 0x18) || !t->layout.fmt || t->layout.layers > 1) return false;
    uint32_t f = t->format;  // BC1-BC5, BC7 (unorm / sRGB), RGBA8, RGB5A1 (the Pokemon icons), R8, RG8
    return (f >= 0x42 && f <= 0x4e) || f == 0x25 || f == 0x01 || f == 0x0d || fmt_of(f).expand_rgb >= 2;
}
static bool ui2x_apply(NvnTexture *t, TexBackend *b, uint64_t data_hash) {
    b->ui2x_hash = data_hash;
    const FormatInfo *fi = t->layout.fmt;
    LevelLayout l = tegra_level(&t->layout, 0);
    uint64_t n = (uint64_t)l.wb * l.hb * fi->bpb;
    uint8_t *lin = malloc(n);
    Ui2xTex x = {0};
    bool found = false;
    if (lin) {
        tegra_deswizzle((const uint8_t *)(uintptr_t)t->addr, lin, l.wb, l.hb, 1, fi->bpb, l.block_height_log2, l.block_depth_log2);
        found = ui2x_find(ui2x_key(lin, n), t->width, t->height, &x);
        free(lin);
    }
    static const VkFormat vkf[] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK,
                                   VK_FORMAT_BC5_UNORM_BLOCK};
    if (!found) {
        if (b->ui2x) { b->ui2x = 0; destroy_image(b); create_image(t, b); }
        return false;
    }
    VkFormat f = vkf[x.fmt & 3];
    if (fmt_of(t->format).vf == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || fmt_of(t->format).vf == VK_FORMAT_BC3_SRGB_BLOCK ||
        fmt_of(t->format).vf == VK_FORMAT_BC7_SRGB_BLOCK || fmt_of(t->format).vf == VK_FORMAT_BC2_SRGB_BLOCK)
        f = f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : f == VK_FORMAT_BC3_UNORM_BLOCK ? VK_FORMAT_BC3_SRGB_BLOCK : f;
    if (!b->ui2x || b->ui2x_fmt != f || b->ui2x_w != x.w || b->ui2x_h != x.h || !b->image) {
        b->ui2x = 1, b->ui2x_fmt = f, b->ui2x_w = x.w, b->ui2x_h = x.h;
        destroy_image(b);
        create_image(t, b);
    }
    if (b->format != f) {  // (the format isn't there: the game's own)
        b->ui2x = 0;
        destroy_image(b);
        create_image(t, b);
        free(x.data);
        return false;
    }
    VkSpan s = vk_transient(x.size, 16);
    memcpy(s.ptr, x.data, x.size);
    free(x.data);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = (x.w + 3) / 4 * 4, .bufferImageHeight = (x.h + 3) / 4 * 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {x.w, x.h, 1}};
    VkCommandBuffer cb = vk_upload_cmd();
    vk_dl_note_upload((void *)b->image, x.w, x.h, 0);
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    static int nlog;
    if (nlog++ < 8) LOG("[ui2x] texture %ux%u fmt %#x at %llx -> %ux%u\n", t->width, t->height, t->format, (unsigned long long)t->addr, x.w, x.h);
    return true;
}

static void upload_texture(NvnTexture *t, TexBackend *b) {
    if (!t->addr || t->target == 10) return;
    uint64_t frame = vk_frame_counter();
    if (b->uploaded && b->checked_frame == frame) return;
    if (b->uploaded && !b->dynamic && ((frame + ((uintptr_t)b >> 6)) & 7)) return;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb) return;
    VkFmt mf = fmt_of(t->format);
    uint64_t total = tegra_total_size(&t->layout);
    b->checked_frame = frame;
    prof_tex_checks++;
    if (t->flags & 0x10) b->dynamic = true;
    if (full_hash_budget_frame != frame) { full_hash_budget_frame = frame; full_hash_budget = 4 << 20; }
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    static int always_full = -1;
    if (always_full < 0) always_full = getenv("LARECOMP_TEX_FULLHASH") ? 1 : 0;
    bool full = b->dynamic || !b->uploaded || always_full;
    if (!full && frame - b->full_frame >= 120 && full_hash_budget >= total) { full = true; full_hash_budget -= total; }
    if (!full) {
        prof_tex_samples++;
        uint64_t sh = fnv_sample(mem, total);
        if (sh == b->sample_hash) return;
    }
    prof_tex_full++; prof_tex_full_bytes += total; if (b->dynamic) prof_tex_dyn++;
    uint64_t h = hash_full(mem, total);
    b->full_frame = frame;
    b->sample_hash = fnv_sample(mem, total);
    if (b->uploaded && h == b->data_hash) return;
    if (b->uploaded && h != b->data_hash) b->dynamic = true;
    b->data_hash = h;
    b->uploaded = true;
    // the interface at 2x (runtime/ui2x.c): checked whenever the data is new
    if ((b->ui2x || (ui2x_candidate(t) && ui2x_enabled())) && ui2x_apply(t, b, h)) return;
    if (b->format != mf.vf && !b->astc_bw) return;  // unsupported format: nothing sensible to copy
    VkCommandBuffer cb = vk_upload_cmd();
    uint32_t layers = t->layout.layers ? t->layout.layers : 1;
    if (layers > b->layers && b->depth == 1) layers = b->layers;
    uint64_t layer_size = tegra_layer_size(&t->layout);
    bool linear = (t->flags & 0x10) != 0;
    uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            if (b->astc_bw) {  // blocks -> linear block rows -> RGBA8 texels
                const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
                uint8_t *blocks = malloc((uint64_t)l.wb * l.hb * 16);
                if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * 16, src + (uint64_t)y * t->stride, l.wb * 16);
                else tegra_deswizzle(src, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
                if (lv == 0 && layer == 0) texkey_log(t, blocks, (uint64_t)l.wb * l.hb * 16, 16);
                VkSpan s = vk_transient((uint64_t)l.w * l.h * 4, 16);
                astc_decode_ldr(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                free(blocks);
                VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                        .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv), l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
                vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                continue;
            }
            uint32_t row = l.wb * fi->bpb;
            uint64_t sz = (uint64_t)row * l.hb * l.d;
            bool expand = (mf.expand_rgb == 1 && fi->bpb == 3) || (mf.expand_rgb >= 2 && fi->bpb == 2);
            uint32_t out_row = expand ? l.wb * 4 : row;
            VkSpan s = vk_transient((uint64_t)out_row * l.hb * l.d, 16);
            uint8_t *lin = expand ? malloc(sz) : s.ptr;
            const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
            if (linear) {
                for (uint32_t y = 0; y < l.hb; y++) memcpy(lin + (uint64_t)y * row, src + (uint64_t)y * t->stride, row);
            } else {
                tegra_deswizzle(src, lin, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2);
            }
            if (lv == 0 && layer == 0 && l.d == 1) texkey_log(t, lin, (uint64_t)row * l.hb, fi->bpb);
            static int dump_fmt = -2;  // LARECOMP_DUMP_TEX_FMT=<nvn format>: level 0 of those textures, untiled -> build/tex/
            if (dump_fmt == -2) dump_fmt = getenv("LARECOMP_DUMP_TEX_FMT") ? (int)strtol(getenv("LARECOMP_DUMP_TEX_FMT"), NULL, 0) : -1;
            if ((int)t->format == dump_fmt && lv == 0 && layer == 0) {
                char path[160];
                mkdir("build/tex", 0755);
                snprintf(path, sizeof path, "build/tex/%llx_%ux%u_f%x_row%u.bin", (unsigned long long)t->addr, l.w, l.h, t->format, row);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(lin, 1, (size_t)row * l.hb, f); fclose(f); }
            }
            if (expand) {  // RGB8 / RGBA4 / RGB5A1 -> RGBA8
                uint8_t *o = s.ptr;
                uint64_t n = (uint64_t)l.wb * l.hb * l.d;
                if (mf.expand_rgb == 1)
                    for (uint64_t i = 0; i < n; i++) { o[i * 4] = lin[i * 3]; o[i * 4 + 1] = lin[i * 3 + 1]; o[i * 4 + 2] = lin[i * 3 + 2]; o[i * 4 + 3] = 255; }
                else
                    for (uint64_t i = 0; i < n; i++) {
                        uint32_t v = lin[i * 2] | (uint32_t)lin[i * 2 + 1] << 8;
                        if (mf.expand_rgb == 2) {  // R 0-3, G 4-7, B 8-11, A 12-15
                            for (int c = 0; c < 4; c++) o[i * 4 + c] = (uint8_t)(((v >> (4 * c)) & 15) * 17);
                        } else {                   // R 0-4, G 5-9, B 10-14, A 15 (the Pokémon icons in Let's Go)
                            for (int c = 0; c < 3; c++) { uint32_t x = (v >> (5 * c)) & 31; o[i * 4 + c] = (uint8_t)(x << 3 | x >> 2); }
                            o[i * 4 + 3] = v >> 15 ? 255 : 0;
                        }
                    }
                free(lin);
            }
            uint32_t w = l.w, hh = l.h, d = l.d;
            // the copy stays inside the image's level: a texture allocated at the internal resolution (16:9, render
            // target sized) can still be filled from the CPU - the save slots' screenshots on the title screen. Its
            // full-size data overran the scaled image (only the width was clamped): GPU writes past the allocation,
            // a device loss below 1x after a seed switch.
            if (w > level_w(b, lv)) w = level_w(b, lv);
            if (hh > level_h(b, lv)) hh = level_h(b, lv);
            VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.wb * fi->bw, .bufferImageHeight = l.hb * fi->bh,
                                    .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect, lv,
                                                         b->depth > 1 ? 0 : layer, 1},
                                    .imageExtent = {w, b->base_type == VK_IMAGE_VIEW_TYPE_1D || b->base_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 1 : hh,
                                                    b->depth > 1 ? d : 1}};
            if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) continue;  // packed D/S: no CPU layout
            vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        }
    }
}

// Host writes into guest memory that textures may sit on (Let's Go's movie decoder fills NV12 buffers the game
// wraps in R8 / RG8 textures): any thread records the range; a texture over it is CPU-owned again at its next use,
// so upload_texture's hash check picks the new data up (a GPU clear had made it GPU-owned: a green movie).
#define DIRTY_RING 4096  // (with the render thread a list can run a few frames after the writes made before it were stamped)
static struct { uint64_t lo, hi, seq; } g_dirty[DIRTY_RING];
static _Atomic uint64_t g_dirty_seq;
static pthread_mutex_t dirty_lock = PTHREAD_MUTEX_INITIALIZER;

void backend_pool_written(uint64_t addr, uint64_t size) {
    if (!size) return;
    pthread_mutex_lock(&dirty_lock);
    uint64_t s = g_dirty_seq + 1;
    g_dirty[s % DIRTY_RING].lo = addr;
    g_dirty[s % DIRTY_RING].hi = addr + size;
    g_dirty[s % DIRTY_RING].seq = s;
    g_dirty_seq = s;
    pthread_mutex_unlock(&dirty_lock);
}

// The render thread (renderthread.c) stamps each list with the write count when the game submitted it: the list sees the
// writes made before that and not the later ones (0: no list, everything so far).
static uint64_t g_dirty_limit;
uint64_t backend_dirty_seq(void) { return g_dirty_seq; }
void backend_set_dirty_limit(uint64_t seq) { g_dirty_limit = seq; }

static void dirty_check(NvnTexture *t, TexBackend *b) {
    uint64_t now = g_dirty_limit ? g_dirty_limit : g_dirty_seq;
    if (b->dirty_seq == now) return;
    if (!b->dirty_seq) { b->dirty_seq = now; return; }  // (new texture: uploaded from memory anyway)
    uint64_t lo = t->addr, hi = t->addr + tegra_total_size(&t->layout);
    pthread_mutex_lock(&dirty_lock);
    for (uint64_t s = now; s > b->dirty_seq && s + DIRTY_RING > now; s--) {
        int i = (int)(s % DIRTY_RING);
        if (g_dirty[i].seq == s && g_dirty[i].lo < hi && lo < g_dirty[i].hi) {
            if (b->scalable) {  // host data is full size (see vk_tex_unscale)
                extern void vk_tex_unscale(NvnTexture *t);
                pthread_mutex_unlock(&dirty_lock);
                vk_tex_unscale(t);
                pthread_mutex_lock(&dirty_lock);
            }
            b->gpu_owned = false;
            b->dynamic = true;
            b->checked_frame = ~0ull;
            break;
        }
    }
    pthread_mutex_unlock(&dirty_lock);
    b->dirty_seq = now;
}

TexBackend *vk_tex(NvnTexture *t, bool for_render) {
    TexBackend *b = tex_backend(t);
    if (b->full && !b->lo_use) {  // (a twin's first use in this in-between image: read first, it is filled from now on)
        b->lo_use = for_render ? 1 : 2;
        if (!for_render && !b->full->lo_seed) {
            b->full->lo_seed = true;
            LOG("[vk] reduced in-between images: %ux%u fmt %#x is read before it is drawn: filled from the full image\n",
                t->width, t->height, t->format);
        }
    }
    if (t->addr && !for_render) dirty_check(t, b);
    if (b->pending_rgba) upload_pending(b);
    if (for_render) b->gpu_owned = true;
    else if ((b->font_k > 1 || t->flags & 0x10) && font_cache_use(t, b)) return b;
    else if (!b->gpu_owned) {
        extern uint64_t prof_up_ns;
        uint64_t t0 = prof_up_ns != ~0ull ? SDL_GetTicksNS() : 0;
        upload_texture(t, b);
        if (prof_up_ns != ~0ull) prof_up_ns += SDL_GetTicksNS() - t0;
    }
    return b;
}

void renderer_mark_gpu_owned(NvnTexture *t) {
    TexBackend *b = tex_backend(t);
    b->gpu_owned = true;
    if (b->font_k > 1) b->uploaded = false;  // (a glyph cache the GPU wrote: all of its K x copy again)
}
bool renderer_is_gpu_owned(NvnTexture *t) { return tex_backend(t)->gpu_owned; }
// Host-produced texel data (decoded video, any thread): applied on the render thread at the texture's next use.
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    TexBackend *b = t->backend;
    if (!b) return;  // the game binds the texture before decoding into it; first frames may arrive earlier
    void *copy = malloc((size_t)stride * h);
    memcpy(copy, rgba, (size_t)stride * h);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->gpu_owned = true;
    pthread_mutex_unlock(&pending_lock);
}

// ================================================================== views
#define VIEW_TAB 16384
typedef struct { TexBackend *b; uint64_t k1, k2; VkImageView view; } ViewEnt;
static ViewEnt view_tab[VIEW_TAB];

// Views may still be referenced by submitted or pending work: destroyed once that has completed.
void vk_views_forget(TexBackend *b) { views_forget(b); }
uint32_t vk_view_gen;  // bumped whenever a cached view may be destroyed (exec.c's per-frame texture memo)
static void views_forget(TexBackend *b) {
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
}

VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) return e->view;
        if (!e->b) break;
    }
    extern uint64_t prof_views_created;
    prof_views_created++;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    VkImageView v;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    ViewEnt *e = &view_tab[slot];
    if (e->b) { defer((Dead){.view = e->view}); vk_view_gen++; }  // evict (16 probes taken)
    *e = (ViewEnt){b, k1, k2, v};
    return v;
}

static VkImageViewType view_type_for(int textype) {
    switch (textype) {
    case TEXTYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TEXTYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TEXTYPE_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TEXTYPE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TEXTYPE_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case TEXTYPE_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static const VkComponentMapping IDENTITY = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

// render: attachment view (one level, one layer). Otherwise a sampled view of type want_textype (TEXTYPE_*),
// with the NVN component swizzle, converting between compatible types the way renderer.m does.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype) {
    TexBackend *b = vk_tex(t, render);
    if (!has_view) v = NULL;
    VkFormat fmt = b->format;
    if (v && (v->setmask & 4) && !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == b->format) fmt = vf;  // (fallback images keep their own format)
    }
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : b->levels - (lv0 < b->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    if (lv0 + nlv > b->levels) nlv = b->levels - lv0;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    if (render) {
        VkImageViewType type = b->base_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (b->base_type == VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;  // (3D render targets: not used by the game)
        return vk_image_view(b, type, fmt, lv0, 1, ly0, 1, IDENTITY, b->aspect);
    }
    VkImageAspectFlags aspect = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    VkComponentMapping sw = IDENTITY;
    if (!(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && b->samples == 1) {
        const uint8_t *s = v && (v->setmask & 8) ? v->swizzle : t->swizzle;
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
        if (c[0] || c[1] || c[2] || c[3]) {  // never set: identity
            if (fmt_of(t->format).alpha_one)
                for (int i = 0; i < 4; i++) if (c[i] == 5) c[i] = 1;
            // NVN swizzle 0 zero, 1 one, 2..5 r,g,b,a == VkComponentSwizzle - 1
            sw = (VkComponentMapping){(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1), (VkComponentSwizzle)(c[2] + 1),
                                      (VkComponentSwizzle)(c[3] + 1)};
        } else if (fmt_of(t->format).alpha_one) {
            sw.a = VK_COMPONENT_SWIZZLE_ONE;
        }
    }
    VkImageViewType want = view_type_for(want_textype);
    VkImageViewType have = b->base_type;
    if (b->samples > 1) return vk_dummy_view(want_textype);  // multisampled images can't be sampled as 2D
    if (want == have) {
        if (have == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_2D && (have == VK_IMAGE_VIEW_TYPE_2D_ARRAY || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, have == VK_IMAGE_VIEW_TYPE_CUBE ? nly : 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_CUBE && have == VK_IMAGE_VIEW_TYPE_2D_ARRAY && b->layers >= 6 && b->width == b->height)
        return vk_image_view(b, want, fmt, lv0, nlv, ly0 + 6 <= b->layers ? ly0 : 0, 6, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_1D_ARRAY && have == VK_IMAGE_VIEW_TYPE_1D)
        return vk_image_view(b, want, fmt, lv0, nlv, 0, 1, sw, aspect);
    static int nlog;
    if (nlog++ < 30) LOG("[tex] type mismatch: want %d have %d (%ux%ux%u target %u fmt %#x layers %u)\n", want, have, t->width,
                         t->height, t->depth, t->target, t->format, b->layers);
    return vk_dummy_view(want_textype);
}

// 1x1 black textures for unbound slots, per type
VkImageView vk_dummy_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    TexBackend *b = &dummies[textype];
    VkImageViewType vt = view_type_for(textype);
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_R8G8B8A8_UNORM;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = vk_image_view(b, vt, b->format, 0, 1, 0, cube ? 6 : 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    return views[textype];
}

// A 1x1 depth image at 1.0 for depth-compare lookups that have no depth texture bound (exec.c texture_info): the
// game binds a small colour texture there when it has no shadow map (a battle: 16x16, format 0x25). The Switch compares
// against its red channel; Vulkan leaves a compare on a colour format undefined (the validation layer's
// VUID-vkCmdDrawIndexed-None-06479), so it is this image instead - every lookup lit, as against that white texture.
VkImageView vk_dummy_depth_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    TexBackend *b = &dummies[textype];
    VkImageViewType vt = view_type_for(textype);
    if (vt == VK_IMAGE_VIEW_TYPE_3D) return vk_dummy_view(textype);  // (no 3D depth images)
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkFormat fmt = VK_FORMAT_D32_SFLOAT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, fmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) fmt = VK_FORMAT_D16_UNORM;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = fmt, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkClearDepthStencilValue one = {1.0f, 0};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, cube ? 6 : 1};
    vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &all);
    b->format = fmt;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = vk_image_view(b, vt, b->format, 0, 1, 0, cube ? 6 : 1, IDENTITY, VK_IMAGE_ASPECT_DEPTH_BIT);
    return views[textype];
}

// ================================================================== samplers
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

// shadow_linear: a depth-compare sampler filters bilinearly whatever the game asked (the shadow filter, F12 menu)
static VkSampler sampler_create(const NvnSamplerDesc *d, bool shadow_linear) {
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->min_filter;
    si.minFilter = (mf == 1 || mf == 3 || mf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    if (d->compare_mode && shadow_linear) si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    bool border = false;
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; border = true; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0]; si.addressModeV = modes[1]; si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= 1) si.maxLod = si.minLod + 0.25f;  // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias;
    if (d->max_aniso > 1) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = fminf(d->max_aniso, 16); }
    // Anisotropic filtering raised to 16x (textures stay sharp at grazing angles: paths and grass towards the top of
    // the screen): the game asks for little or none, sized for the Switch. Mipmapped, linearly filtered colour samplers
    // only. LARECOMP_ANISO=<n> chooses (0 or 1: the game's own).
    static int aniso = -1;
    if (aniso < 0) aniso = getenv("LARECOMP_ANISO") ? atoi(getenv("LARECOMP_ANISO")) : 16;
    if (aniso > 1 && mf >= 2 && si.minFilter == VK_FILTER_LINEAR && !d->compare_mode) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = fmaxf(si.maxAnisotropy, fminf((float)aniso, 16));
    }
    static float lodmin = -1;
    if (lodmin < 0) lodmin = getenv("LARECOMP_LOD_MIN") ? (float)atof(getenv("LARECOMP_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) si.minLod = fmaxf(si.minLod, lodmin);
    static int stest = -1;
    if (stest < 0) stest = getenv("LARECOMP_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { si.minLod = 0; si.maxLod = VK_LOD_CLAMP_NONE; si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 16; }
    if (d->compare_mode) {
        si.compareEnable = VK_TRUE;
        si.compareOp = getenv("LARECOMP_NO_SHADOW") ? VK_COMPARE_OP_ALWAYS : compare_op(d->compare_func);
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border) {
        const float *c = d->border;
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (g_vk.custom_border) {
            memcpy(cb.customBorderColor.float32, c, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            si.pNext = &cb;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    VkSampler s;
    VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s));
    return s;
}

// Let's Go's shadow map is sampled with nearest filtering (each of its 11 taps a hard in / out: stair-stepped edges);
// SHADOW_BILINEAR / SHADOW_SMOOTH make each tap a bilinear compare (2x2 PCF), the depth formats permitting. Depth-
// compare samplers live in their own table, by description and filter, so a change in the menu takes at once.
VkSampler vk_sampler(NvnSamplerDesc *d) {
    if (!d->compare_mode) {
        if (!d->backend) d->backend = (void *)sampler_create(d, false);
        return (VkSampler)d->backend;
    }
    static int linear_ok = -1;
    if (linear_ok < 0) {
        linear_ok = format_supports(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) &&
                    format_supports(VK_FORMAT_D32_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        LOG("[vk] shadow map: %s\n", linear_ok ? "bilinear compare available" : "no linear filtering on depth formats, the game's filter");
    }
    bool lin = linear_ok && g_settings.shadow_filter != SHADOW_GAME;
    static struct { NvnSamplerDesc d; bool lin; VkSampler s; } tab[64];
    static int n;
    NvnSamplerDesc k = *d;
    k.backend = NULL;
    for (int i = 0; i < n; i++)
        if (tab[i].lin == lin && !memcmp(&tab[i].d, &k, sizeof k)) return tab[i].s;
    VkSampler s = sampler_create(&k, lin);
    if (n < 64) tab[n].d = k, tab[n].lin = lin, tab[n].s = s, n++;  // (past 64: a new one each time - not seen in the game)
    return s;
}

// The sampler of d with `bias` more mip bias (exec.c texture_info: TAA / FSR 3.1), mipmapped and linearly filtered
// ones only; the others, d's own. Kept by description and bias (the bias takes few values: steps of 0.25), never freed -
// d is copied by value into the sampler pools.
VkSampler vk_sampler_biased(NvnSamplerDesc *d, float bias) {
    uint32_t mf = d->min_filter;
    if (bias == 0 || !(mf == 3 || mf == 5) || d->compare_mode) return vk_sampler(d);
    typedef struct { NvnSamplerDesc d; float bias; VkSampler s; } Biased;
    static Biased *tab;
    static uint32_t cap, n;
    NvnSamplerDesc k = *d;
    k.backend = NULL;
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *kb = (const uint8_t *)&k;
    for (size_t i = 0; i < sizeof k; i++) h = (h ^ kb[i]) * 0x100000001B3ull;
    uint32_t bq = (uint32_t)(int32_t)lroundf(bias * 4);
    h = (h ^ bq) * 0x100000001B3ull;
    if (n * 2 >= cap) {  // (open addressing, grown at half full)
        uint32_t ncap = cap ? cap * 2 : 512;
        Biased *nt = calloc(ncap, sizeof *nt);
        for (uint32_t i = 0; i < cap; i++) {
            if (!tab[i].s) continue;
            uint64_t hh = 0xcbf29ce484222325ull;
            const uint8_t *b = (const uint8_t *)&tab[i].d;
            for (size_t j = 0; j < sizeof tab[i].d; j++) hh = (hh ^ b[j]) * 0x100000001B3ull;
            hh = (hh ^ (uint32_t)(int32_t)lroundf(tab[i].bias * 4)) * 0x100000001B3ull;
            uint32_t sl = (uint32_t)hh & (ncap - 1);
            while (nt[sl].s) sl = (sl + 1) & (ncap - 1);
            nt[sl] = tab[i];
        }
        free(tab);
        tab = nt, cap = ncap;
    }
    uint32_t sl = (uint32_t)h & (cap - 1);
    while (tab[sl].s) {
        if (tab[sl].bias == bias && !memcmp(&tab[sl].d, &k, sizeof k)) return tab[sl].s;
        sl = (sl + 1) & (cap - 1);
    }
    NvnSamplerDesc b = k;
    b.lod_bias += bias;
    tab[sl] = (Biased){k, bias, sampler_create(&b, false)};
    n++;
    return tab[sl].s;
}

VkSampler vk_default_sampler(bool linear) {
    static VkSampler s[2];
    if (!s[linear]) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                  .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s[linear]));
    }
    return s[linear];
}
