// GLSL -> SPIR-V (shaderc), descriptor layouts from the SPIR-V itself, translated NVN programs, built-in shaders.
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "shaders.h"

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
}

// SPIR-V disk cache: cache/spirv/<hash of stage + source>.spv, so a known shader costs a file read instead of a
// GLSL compile (the driver's pipeline cache only covers the step after this one).
static uint64_t src_hash(const char *src, int stage) {
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 3;  // + version of the post-processing (no_contraction)
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

static uint32_t *cache_load(uint64_t h, size_t *nwords) {
    char path[64];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *d = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d && d[0] != 0x07230203) { free(d); d = NULL; }
    if (d) *nwords = (size_t)n / 4;
    return d;
}

static void cache_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    static int made;
    if (!made) { made = 1; mkdir("cache", 0755); mkdir("cache/spirv", 0755); }
    char path[64], tmp[80];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&made ^ (unsigned long long)(uintptr_t)spv);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(spv, 4, nwords, f) == nwords;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another thread may have stored it first)
}

// Vertex shaders: every float add/sub/mul/div/fma gets NoContraction, so no compiler fuses or splits them. Depth
// pre-pass and colour pass are different shaders computing the same position, and the colour pass tests EQUAL:
// any difference in fusing leaves holes (foliage flickered black). GLSL's `precise gl_Position` only covers what
// glslang traces back from the position, which the translated register code defeats.
static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

// SPIR-V words of src, or NULL (error logged with the source). *nwords set on success. Thread-safe: one shaderc
// compiler per thread (a shared one behind a lock serialised all the pipeline workers).
// No spirv-opt by default: it rewrote the depth pre-pass and colour-pass vertex shaders differently enough that
// their depths stopped matching (EQUAL test: black holes in foliage), and it was most of the compile time; the
// driver optimises anyway. LARECOMP_VK_OPT=1 turns it back on (experiments).
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords) {
    uint64_t h = src_hash(src, stage);
    static int nocache = -1;
    if (nocache < 0) nocache = getenv("LARECOMP_VK_NO_SPIRV_CACHE") != NULL;
    uint32_t *out = nocache ? NULL : cache_load(h, nwords);
    if (out) return out;
    static __thread shaderc_compiler_t comp;
    if (!comp) comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(o, getenv("LARECOMP_VK_OPT") ? shaderc_optimization_level_performance
                                                                                  : shaderc_optimization_level_zero);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, strlen(src), kind_of(stage), name, "main", o);
    shaderc_compile_options_release(o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        static int nlog;
        if (nlog++ < 20) LOG("[shader] %s: GLSL compile failed:\n%s\n%s\n", name, shaderc_result_get_error_message(r), src);
    } else {
        size_t n = shaderc_result_get_length(r);
        out = malloc(n);
        memcpy(out, shaderc_result_get_bytes(r), n);
        *nwords = n / 4;
        if (stage == STAGE_VERTEX) out = no_contraction(out, nwords);
        if (!nocache) cache_store(h, out, *nwords);
    }
    shaderc_result_release(r);
    return out;
}

VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4, .pCode = spv};
    VkShaderModule m;
    return vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name) {
    size_t n;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

// ================================================================== reflection
// Just enough SPIR-V parsing to list the descriptor bindings a module declares (set, binding, type).
enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *out, int max) {
    if (n < 5 || spv[0] != 0x07230203) return 0;
    uint32_t bound = spv[3];
    int32_t *binding = malloc(bound * sizeof *binding), *set = malloc(bound * sizeof *set);
    uint8_t *flags = calloc(bound, 1);    // 1 Block, 2 BufferBlock
    uint32_t *op = calloc(bound, sizeof *op), *ref = calloc(bound, sizeof *ref), *sc = calloc(bound, sizeof *sc);
    for (uint32_t i = 0; i < bound; i++) binding[i] = set[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, o = spv[i] & 0xFFFF;
        if (!wc) break;
        const uint32_t *w = spv + i;
        if (o == OP_DECORATE && wc >= 3 && w[1] < bound) {
            if (w[2] == DEC_BINDING && wc >= 4) binding[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_SET && wc >= 4) set[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_BLOCK) flags[w[1]] |= 1;
            else if (w[2] == DEC_BUFFER_BLOCK) flags[w[1]] |= 2;
        } else if ((o == OP_TYPE_IMAGE || o == OP_TYPE_SAMPLER || o == OP_TYPE_SAMPLED_IMAGE || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) {
            op[w[1]] = o;
        } else if ((o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[1] < bound) {
            op[w[1]] = o;
            ref[w[1]] = w[2];
        } else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) {
            op[w[1]] = o;
            sc[w[1]] = w[2];
            ref[w[1]] = w[3];
        } else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound && w[1] < bound) {
            op[w[2]] = o;
            ref[w[2]] = w[1];  // pointer type
            sc[w[2]] = w[3];
        }
        i += wc;
    }
    int cnt = 0;
    for (uint32_t id = 0; id < bound && cnt < max; id++) {
        if (op[id] != OP_VARIABLE || binding[id] < 0) continue;
        uint32_t t = ref[ref[id]];  // pointee
        while (t < bound && (op[t] == OP_TYPE_ARRAY || op[t] == OP_TYPE_RUNTIME_ARRAY)) t = ref[t];
        VkDescriptorType dt;
        if (t >= bound) continue;
        if (op[t] == OP_TYPE_SAMPLED_IMAGE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op[t] == OP_TYPE_IMAGE) dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (op[t] == OP_TYPE_SAMPLER) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (op[t] == OP_TYPE_STRUCT && (sc[id] == SC_STORAGE_BUFFER || (flags[t] & 2))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (op[t] == OP_TYPE_STRUCT && sc[id] == SC_UNIFORM) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else continue;
        out[cnt++] = (VkBindingInfo){(uint32_t)(set[id] < 0 ? 0 : set[id]), (uint32_t)binding[id], dt};
    }
    free(binding); free(set); free(flags); free(op); free(ref); free(sc);
    return cnt;
}

VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t k = 0;
    for (int i = 0; i < n && k < 96; i++)
        if (b[i].set == set) lb[k++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].type, 1, stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb};
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// ================================================================== built-in shaders
// A pixel's view-space position (the SSAO's, the contact shadows', the GI's normals), and with surf its position c and
// normal n (the SSAO and the contact shadows), rebuilt from the depth. The depth at binding 0; lin() and pc.a, the
// projection, declared by each group below.
static const char *vpos_src =
    "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
    "ivec2 sz;\n"
    "vec3 vpos(ivec2 p) {\n"
    "  p = clamp(p, ivec2(0), sz - 1);\n"
    "  float l = lin(texelFetch(dep, p, 0).r);\n"
    "  vec2 ndc = (vec2(p) + 0.5) / vec2(sz) * 2.0 - 1.0;\n"
    "  return vec3(ndc.x * l / pc.a.x, ndc.y * l / pc.a.y, l);\n"
    "}\n";
static const char *surf =
    "void surface(ivec2 p, out vec3 c, out vec3 n) {\n"
    "  c = vpos(p);\n"
    "  vec3 l = vpos(p - ivec2(1, 0)), r = vpos(p + ivec2(1, 0)), u = vpos(p - ivec2(0, 1)), d = vpos(p + ivec2(0, 1));\n"
    // A pixel whose two neighbours of a direction are both nearer or both farther is no plane: a crack or a sliver one
    // pixel wide (the seams between the ground's pieces and the path patches drawn ~2 units over it). At 4K a pixel
    // covers ~0.5 units of ground, so its normal came out nearly level, the ground around it occluded it, and every
    // seam was a thin dark line (at 720p hardly). The surface is the neighbours'.
    "  float tc = 1e-4 * c.z;\n"
    "  if ((l.z - c.z) * (r.z - c.z) > 0.0 && min(abs(l.z - c.z), abs(r.z - c.z)) > tc) c = (l + r) * 0.5;\n"
    "  else if ((u.z - c.z) * (d.z - c.z) > 0.0 && min(abs(u.z - c.z), abs(d.z - c.z)) > tc) c = (u + d) * 0.5;\n"
    // (two pixels wide: the same with the neighbours two away)
    "  else {\n"
    "    vec3 l2 = vpos(p - ivec2(2, 0)), r2 = vpos(p + ivec2(2, 0)), u2 = vpos(p - ivec2(0, 2)), d2 = vpos(p + ivec2(0, 2));\n"
    "    if ((l2.z - c.z) * (r2.z - c.z) > 0.0 && min(abs(l2.z - c.z), abs(r2.z - c.z)) > tc) { c = (l2 + r2) * 0.5; l = l2; r = r2; }\n"
    "    else if ((u2.z - c.z) * (d2.z - c.z) > 0.0 && min(abs(u2.z - c.z), abs(d2.z - c.z)) > tc) { c = (u2 + d2) * 0.5; u = u2; d = d2; }\n"
    "  }\n"
    "  vec3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;\n"
    "  vec3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;\n"
    "  n = normalize(cross(dx, dy));\n"
    "  if (dot(n, c) > 0.0) n = -n;\n"
    "}\n";
static const char *builtin_src(const char *name, int *stage) {
    if (!strcmp(name, "present.vert")) {
        *stage = STAGE_VERTEX;
        // pv[0].x: flip (lower-left origin), pv[1]: uv crop (origin, size). Fullscreen triangle; Vulkan NDC y points down.
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 crop; } pc;\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  vec2 u = vec2(p.x, pc.a.x > 0.5 ? 1.0 - p.y : p.y);\n"
               "  uv = pc.crop.xy + u * pc.crop.zw;\n"
               "}\n";
    }
    if (!strcmp(name, "present.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               // A render resolution above the window's (the launcher's 1440p / 4K on a smaller screen: supersampling)
               // is reduced with four bilinear taps spread over each window pixel's footprint - one bilinear tap
               // skips texels and keeps the jaggies (exactly 2x: the 2x2 box).
               "void main() {\n"
               "  vec2 d = vec2(dFdx(uv.x), dFdy(uv.y)), r = abs(d) * vec2(textureSize(t, 0));\n"
               "  if (max(r.x, r.y) < 1.05) { o = vec4(texture(t, uv).rgb, 1.0); return; }\n"
               "  vec2 q = d * 0.25;\n"
               "  vec3 c = texture(t, uv + vec2(-q.x, -q.y)).rgb + texture(t, uv + vec2(q.x, -q.y)).rgb +\n"
               "           texture(t, uv + vec2(-q.x, q.y)).rgb + texture(t, uv + vec2(q.x, q.y)).rgb;\n"
               "  o = vec4(c * 0.25, 1.0);\n"
               "}\n";
    }
    // ---- post passes before the present (Video tab "Anti-aliasing" / "Upscaler"): exec.c vk_present_draw.
    // Each draws a fullscreen triangle into an image the size of its output; push constants: two vec4.
    if (!strcmp(name, "post.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "}\n";
    }
    // SMAA 1x (third_party/smaa, runtime/nvn/smaa_data.c): the reference SMAA.hlsl in its GLSL mode, High preset,
    // with an entry point per pass. a = SMAA_RT_METRICS (1 / size, size). The vertex-stage offset helpers run per
    // pixel here. Luma edges (t0 = colour) -> blending weights (t0 = edges, t1 = area, t2 = search) -> neighbourhood
    // blending (t0 = colour, t1 = weights). Vulkan's texture origin is top-left, as SMAA expects (no flips).
    if (!strncmp(name, "smaa_", 5)) {
        *stage = STAGE_FRAGMENT;
        static char *src[3];
        int k = !strcmp(name, "smaa_edge.frag") ? 0 : !strcmp(name, "smaa_weight.frag") ? 1 : !strcmp(name, "smaa_blend.frag") ? 2 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            extern const char smaa_hlsl_src[];
            static const char *head =
                "#version 450\n"
                "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
                "#define SMAA_RT_METRICS pc.a\n"
                "#define SMAA_GLSL_4\n"
                "#define SMAA_PRESET_HIGH\n";
            static const char *mains[3] = {
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off[3];\n"
                "  SMAAEdgeDetectionVS(uv, off);\n"
                "  o = vec4(SMAALumaEdgeDetectionPS(uv, off, t0), 0.0, 0.0);\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy, pix;\n"
                "  vec4 off[3];\n"
                "  SMAABlendingWeightCalculationVS(uv, pix, off);\n"
                "  o = SMAABlendingWeightCalculationPS(uv, pix, off, t0, t1, t2, vec4(0.0));\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off;\n"
                "  SMAANeighborhoodBlendingVS(uv, off);\n"
                "  o = vec4(SMAANeighborhoodBlendingPS(uv, off, t0, t1).rgb, 1.0);\n"
                "}\n"};
            size_t n = strlen(head) + strlen(smaa_hlsl_src) + strlen(mains[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s", head, smaa_hlsl_src, mains[k]);
        }
        return src[k];
    }
    // AMD FidelityFX Super Resolution 1 (MIT), EASU: edge-adaptive upscale. a = (in / out scale xy, offset zw),
    // b.xy = input size. 12 taps around the source pixel, direction / edge length from luma, Lanczos-2-like
    // weights stretched along the edge, clamped to the nearest 4 (no ringing).
    if (!strcmp(name, "easu.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
               "  float dx = lD - lB, dy = lE - lA;\n"
               "  float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  dir += vec2(dx, dy) * w;\n"
               "  len += (lx * lx + ly * ly) * w;\n"
               "}\n"
               "void tapf(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
               "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;\n"
               "  float d2 = min(dot(v, v), clp);\n"
               "  float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
               "  wb *= wb; wa *= wa;\n"
               "  float w = (1.5625 * wb - 0.5625) * wa;\n"
               "  ac += c * w; aw += w;\n"
               "}\n"
               "void main() {\n"
               "  vec2 pp = floor(gl_FragCoord.xy) * pc.a.xy + pc.a.zw;\n"
               "  vec2 fpf = floor(pp); pp -= fpf;\n"
               "  ivec2 fp = ivec2(fpf);\n"
               "  //    b c\n"
               "  //  e f g h\n"
               "  //  i j k l\n"
               "  //    n o\n"
               "  vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));\n"
               "  vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));\n"
               "  vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));\n"
               "  vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));\n"
               "  float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);\n"
               "  float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);\n"
               "  vec2 dir = vec2(0.0);\n"
               "  float len = 0.0;\n"
               "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
               "  setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
               "  setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
               "  setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
               "  float dr = dot(dir, dir);\n"
               "  bool zro = dr < 1.0 / 32768.0;\n"
               "  dir = zro ? vec2(1.0, 0.0) : dir * inversesqrt(dr);\n"
               "  len = len * 0.5; len *= len;\n"
               "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
               "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
               "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
               "  vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
               "  vec3 ac = vec3(0.0);\n"
               "  float aw = 0.0;\n"
               "  tapf(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);\n"
               "  tapf(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
               "  tapf(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
               "  tapf(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
               "  tapf(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
               "  tapf(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
               "  tapf(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
               "  tapf(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
               "  tapf(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
               "  tapf(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
               "  tapf(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
               "  tapf(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
               "  o = vec4(min(mx, max(mn, ac / aw)), 1.0);\n"
               "}\n";
    }
    // FSR 1 RCAS: contrast-adaptive sharpening of the EASU output (same size). a.x = sharpness (exp2(-stops)),
    // b.xy = image size. The lobe is limited so the result stays inside the neighbours' range; noise is damped.
    // Debanding, the last post pass (exec.c post_process_ex): the game's 8-bit targets show steps in smooth gradients
    // (sky, fog, the menus' blurred backdrop). As mpv's / libplacebo's deband: around each pixel, at a random angle and
    // distance within a radius, four taps; when their mean is within the threshold of the pixel (a step of a band, not
    // an edge or a texture's detail) it takes the mean - twice, the second time twice as far and half the threshold.
    // Then a triangular dither of one 8-bit step, fixed per pixel (not per frame: TAA, FSR and the in-between images
    // see the same pattern). a = (1 / size, threshold), b = (size, radius in pixels).
    if (!strcmp(name, "deband.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float h12(vec2 p) { vec3 q = fract(vec3(p.xyx) * 0.1031); q += dot(q, q.yzx + 33.33); return fract((q.x + q.y) * q.z); }\n"
               "void main() {\n"
               "  vec2 px = gl_FragCoord.xy, uv = px * pc.a.xy;\n"
               "  vec3 c = texelFetch(t, ivec2(px), 0).rgb;\n"
               "  for (int i = 1; i <= 2; i++) {\n"
               "    float ang = h12(px + float(i) * 17.0) * 6.2831853;\n"
               "    float rad = pc.b.z * float(i) * h12(px * 1.7 + float(i) * 31.0);\n"
               "    vec2 d = vec2(cos(ang), sin(ang)) * rad * pc.a.xy, e = vec2(-d.y, d.x);\n"
               "    vec3 m = 0.25 * (texture(t, uv + d).rgb + texture(t, uv - d).rgb + texture(t, uv + e).rgb + texture(t, uv - e).rgb);\n"
               "    c = mix(c, m, step(abs(c - m), vec3(pc.a.z / float(i))));\n"
               "  }\n"
               "  float n = h12(px) + h12(px * 1.31 + 7.0) - 1.0;\n"
               "  o = vec4(clamp(c + n / 255.0, 0.0, 1.0), 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "rcas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void main() {\n"
               "  ivec2 ip = ivec2(gl_FragCoord.xy);\n"
               "  //    b\n"
               "  //  d e f\n"
               "  //    h\n"
               "  vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));\n"
               "  float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);\n"
               "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
               "  float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
               "  nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);\n"
               "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
               "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
               "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));\n"
               "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
               "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.a.x * nz;\n"
               "  o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);\n"
               "}\n";
    }
    // SSAO (F12 menu; exec.c ssao_run): on the game's depth, between its opaque and its blended draws. a = the game's
    // projection (P00, P11, A, B: GL style, z_ndc = (A z + B) / -z, so the distance along the view axis is
    // B / (z_ndc + A)); b.z = 1 when depth was stored as (z_ndc + 1) / 2 (NVN's -1..1 mode). Positions are rebuilt
    // in view space (x right, y as the image, z the distance: one handedness throughout), the normal from the nearer
    // neighbours (no smearing across silhouettes).
    if (!strcmp(name, "ssao.frag") || !strcmp(name, "ssao_blur.frag") || !strcmp(name, "ssao_temporal.frag") ||
        !strcmp(name, "contact.frag")) {
        *stage = STAGE_FRAGMENT;
        static const char *common =
            "#version 450\n"
            "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;\n"
            "layout(location = 0) out vec4 o;\n"
            "float lin(float d) { return pc.a.w / ((pc.b.z > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.z); }\n";
        // b.x = radius (world units), b.y = intensity, b.w = the largest screen radius in pixels. c.y samples (8-16) on a
        // spiral turned per pixel (interleaved gradient noise; the blur evens it out), denser near the centre;
        // c.x turns it per frame too (temporal accumulation: other directions every frame).
        static const char *ao =
            "void main() {\n"
            "  sz = textureSize(dep, 0);\n"
            "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "  if (texelFetch(dep, p, 0).r >= 0.999999) { o = vec4(1.0); return; }\n"
            "  vec3 c, n;\n"
            "  surface(p, c, n);\n"
            "  float rad = pc.b.x;\n"
            "  float rpx = min(rad * pc.a.x / c.z * 0.5 * float(sz.x), pc.b.w);\n"
            "  if (rpx < 1.0) { o = vec4(1.0); return; }\n"
            "  float ang = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + pc.c.x);\n"
            "  float occ = 0.0, bias = 0.02 * c.z;\n"
            "  int ns = int(pc.c.y);\n"
            "  for (int i = 0; i < ns; i++) {\n"
            "    float t = (float(i) + 0.5) / float(ns), a = ang + float(i) * 2.39996323;\n"
            "    vec3 v = vpos(p + ivec2(vec2(cos(a), sin(a)) * (t * rpx + 1.0))) - c;\n"
            "    float vv = dot(v, v);\n"
            "    float cosv = (dot(v, n) - bias) * inversesqrt(vv + 1e-3);\n"
            "    occ += max(cosv, 0.0) * clamp(1.0 - vv / (rad * rad), 0.0, 1.0);\n"
            "  }\n"
            "  o = vec4(clamp(1.0 - pc.b.y * occ * (2.0 / float(ns)), 0.0, 1.0), 0.0, 0.0, 1.0);\n"
            "}\n";
        // Depth-aware blur along b.xy (one texel steps), 9 taps: neighbours more than a few % farther or nearer than
        // the centre don't count (no halo around silhouettes).
        static const char *blur =
            "layout(set = 0, binding = 0) uniform sampler2D t;\n"
            "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
            "void main() {\n"
            "  ivec2 sz = textureSize(t, 0), p = ivec2(gl_FragCoord.xy);\n"
            "  float l0 = lin(texelFetch(dep, p, 0).r), s = 0.0, w = 0.0;\n"
            "  for (int i = -4; i <= 4; i++) {\n"
            "    ivec2 q = clamp(p + ivec2(pc.b.xy * float(i)), ivec2(0), sz - 1);\n"
            "    float dl = abs(lin(texelFetch(dep, q, 0).r) - l0) / (0.04 * l0);\n"
            "    float k = exp(-float(i * i) / 10.0) * max(1.0 - dl, 0.0);\n"
            "    s += texelFetch(t, q, 0).r * k; w += k;\n"
            "  }\n"
            "  o = vec4(w > 0.0 ? s / w : texelFetch(t, p, 0).r, 0.0, 0.0, 1.0);\n"
            "}\n";
        // Temporal accumulation (exec.c ssao_run): t = this frame's blurred occlusion, dep = depth, mv = the camera
        // motion vectors (xy velocity, z previous / current distance), hist = the previous result (r occlusion,
        // g its distance). The history is fetched where the pixel was; it counts only if the distance stored there is
        // the one the pixel had then (else: uncovered, or a character moving on its own - camera vectors don't know
        // it), and clamped to this frame's 3x3 range - exactly: with any slack the history stayed that much darker for
        // good where a character's contact shadow had passed (a fixed point of the blend: tools/ssao_temporal_check.py).
        // b.y = the history's weight, b.w = 1: no history (a cut, the
        // first frame). Out: r = the occlusion, g = this distance (the next frame's history).
        static const char *temporal =
            "layout(set = 0, binding = 0) uniform sampler2D t;\n"
            "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
            "layout(set = 0, binding = 2) uniform sampler2D mv;\n"
            "layout(set = 0, binding = 3) uniform sampler2D hist;\n"
            "void main() {\n"
            "  ivec2 sz = textureSize(t, 0), p = ivec2(gl_FragCoord.xy);\n"
            "  float d = texelFetch(dep, p, 0).r, l = lin(d), a = texelFetch(t, p, 0).r;\n"
            "  if (d >= 0.999999 || pc.b.w > 0.5) { o = vec4(a, l, 0.0, 1.0); return; }\n"
            "  float mn = a, mx = a;\n"
            "  for (int y = -1; y <= 1; y++)\n"
            "    for (int x = -1; x <= 1; x++) {\n"
            "      float c = texelFetch(t, clamp(p + ivec2(x, y), ivec2(0), sz - 1), 0).r;\n"
            "      mn = min(mn, c); mx = max(mx, c);\n"
            "    }\n"
            "  vec4 m = texelFetch(mv, p, 0);\n"
            "  vec2 puv = (vec2(p) + 0.5) / vec2(sz) - m.xy;\n"
            "  float w = 0.0, h = a;\n"
            "  if (all(greaterThanEqual(puv, vec2(0.0))) && all(lessThan(puv, vec2(1.0)))) {\n"
            "    float lh = texelFetch(hist, ivec2(puv * vec2(sz)), 0).g, le = m.z * l;\n"
            "    if (abs(lh - le) < 0.02 * le + 1.0) {\n"
            "      w = pc.b.y;\n"
            "      h = clamp(texture(hist, puv).r, mn, mx);\n"
            "    }\n"
            "  }\n"
            "  o = vec4(mix(a, h, w), l, 0.0, 1.0);\n"
            "}\n";
        // Contact shadows (exec.c contact_run; the graphics page): from each pixel a short ray toward the sun (c.xyz, view
        // space as above; the light found in the shadow map's pass), b.w steps along b.x world units, jittered per
        // pixel; a step behind the depth there by less than c.w (the thickness: not the far side of a wall) is in
        // shadow, fainter the farther along the ray the hit. Surfaces facing away from the sun are left to the game's
        // lighting. Out: 1 - b.y (the strength) in shadow, faded out with distance (the shadow map covers far away).
        static const char *contact =
            "void main() {\n"
            "  sz = textureSize(dep, 0);\n"
            "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
            "  if (texelFetch(dep, p, 0).r >= 0.999999) { o = vec4(1.0); return; }\n"
            "  vec3 c, n;\n"
            "  surface(p, c, n);\n"
            "  vec3 L = normalize(pc.c.xyz);\n"
            "  if (dot(n, L) < 0.05) { o = vec4(1.0); return; }\n"
            "  float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
            "  vec3 st = c + n * (0.003 * c.z);\n"
            "  int N = int(pc.b.w);\n"
            "  float sh = 0.0;\n"
            "  for (int i = 0; i < N; i++) {\n"
            "    float t = (float(i) + jit) / float(N);\n"
            "    vec3 q = st + L * (pc.b.x * t);\n"
            "    if (q.z <= 1.0) break;\n"
            "    vec2 uv = vec2(q.x * pc.a.x, q.y * pc.a.y) / q.z * 0.5 + 0.5;\n"
            "    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) break;\n"
            "    float dz = q.z - lin(texelFetch(dep, ivec2(uv * vec2(sz)), 0).r);\n"
            "    if (dz > 0.002 * q.z && dz < pc.c.w) { sh = 1.0 - t * t; break; }\n"
            "  }\n"
            "  float fade = clamp(2.0 - c.z / 2000.0, 0.0, 1.0);\n"
            "  o = vec4(1.0 - pc.b.y * sh * fade, 0.0, 0.0, 1.0);\n"
            "}\n";
        static char *src[4];
        int k = !strcmp(name, "ssao_blur.frag") ? 1 : !strcmp(name, "ssao_temporal.frag") ? 2 : !strcmp(name, "contact.frag") ? 3 : 0;
        if (!src[k]) {
            const char *body = k == 3 ? contact : k == 2 ? temporal : k ? blur : ao;
            const char *pre = k == 0 || k == 3 ? vpos_src : "", *pre2 = k == 0 || k == 3 ? surf : "";
            size_t n = strlen(common) + strlen(pre) + strlen(pre2) + strlen(body) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s%s", common, pre, pre2, body);
        }
        return src[k];
    }
    // ---- Screen-space indirect light, one bounce (F12 GI; exec.c gi_run), after FastGI's design: rays traced at a
    // reduced resolution against the depth, the scene's own colour where they hit, two bilateral blurs, an optional
    // temporal accumulation along the camera motion vectors, then an edge-aware upsample added onto the scene.
    // View space as in the SSAO (x right, y as the image, z the distance), the normals rebuilt from the depth (the
    // game has no normal buffer). a = the projection (P00, P11, A, B), d.x = 1: depth stored as (z_ndc + 1) / 2,
    // d.zw = the GI resolution. World units: the game's (~cm).
    // The trace, the blurs and the composite's taps read a depth and normals (RGBA8) at the GI's own size, each GI
    // pixel the full-size pixel under its centre (gi_depth / gi_normal: what the passes fetched before, from the
    // full-size images). The trace's 4 x 8 steps + 2 bisections per ray are scattered reads, bound by the cache: over
    // the 4K depth (D32, 33 MB) 0.87 ms at 4K, GI high; over the GI-size distance as R32F 0.72, as 16 bits 0.6. So
    // the distance is R16_UNORM of log2(distance) / 16 (0 on the sky): 1.7e-4 of it (0.3 units at 20 m; fp16 would be
    // 5e-4), up to 65536 units - 650 m, past the 60 m the GI lights.
    if (!strncmp(name, "gi_", 3)) {
        *stage = STAGE_FRAGMENT;
        static const char *common =
            "#version 450\n"
            "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } pc;\n"
            "layout(location = 0) out vec4 o;\n"
            "float lin(float d) { return pc.a.w / ((pc.d.x > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.z); }\n"
            "vec3 vpos(vec2 uv, float l) { vec2 n = uv * 2.0 - 1.0; return vec3(n.x * l / pc.a.x, n.y * l / pc.a.y, l); }\n"
            "vec2 puv(vec3 p) { return vec2(p.x * pc.a.x, p.y * pc.a.y) / p.z * 0.5 + 0.5; }\n"
            // the GI pixel's centre in uv, and the full-size pixel under it
            "vec2 low_uv() { return (floor(gl_FragCoord.xy) + 0.5) / pc.d.zw; }\n"
            // bilateral weight of a tap: depth within ~1.5% (2 units at least), normals alike
            "float geo(float l0, float l1, vec3 n0, vec3 n1) {\n"
            "  float w = clamp(1.0 - abs(l0 - l1) / max(2.0, l0 * 0.015), 0.0, 1.0) * step(0.001, l1);\n"
            "  float nw = clamp(dot(n0, n1), 0.0, 1.0);\n"
            "  nw *= nw;\n"
            "  return w * nw * nw;\n"
            "}\n"
            "vec3 dec_n(vec4 e) { return normalize(e.xyz * 2.0 - 1.0); }\n";
        // The normal from the nearer neighbour on each axis (no smearing across silhouettes), facing the camera. Not
        // the SSAO's surface(), tried (one pass shared by both, or the same function): its seam rule takes the edges of
        // the alpha-tested leaves for seams, and the GI's light broke into dark blocks and speckles over the foliage.
        // A full-size target shared with the SSAO also cost 0.125 ms at 4K and saved it nothing (its own rebuild hides
        // under its samples' reads: 1.94 ms either way).
        static const char *qnormal =
            "vec3 quick_normal(ivec2 p) {\n"
            "  vec3 c = vpos(p), l = vpos(p - ivec2(1, 0)), r = vpos(p + ivec2(1, 0)), u = vpos(p - ivec2(0, 1)), d = vpos(p + ivec2(0, 1));\n"
            "  vec3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;\n"
            "  vec3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;\n"
            "  vec3 n = normalize(cross(dx, dy));\n"
            "  return dot(n, c) > 0.0 ? -n : n;\n"
            "}\n";
        // GI size: the distance (R16_UNORM, log2 / 16 as above) and the normal (RGBA8: n * 0.5 + 0.5, a = 0 on the sky)
        // of the full-size pixel under each GI pixel's centre (the normals were a full-size pass: 0.08 ms at 4K)
        static const char *depth =
            "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
            "void main() {\n"
            "  float d = texelFetch(dep, ivec2(low_uv() * vec2(textureSize(dep, 0))), 0).r;\n"
            "  o = vec4(d >= 0.999999 ? 0.0 : max(log2(max(lin(d), 1.0)) / 16.0, 1.0 / 65535.0), 0.0, 0.0, 1.0);\n"
            "}\n";
        static const char *normal =
            "void main() {\n"
            "  sz = textureSize(dep, 0);\n"
            "  ivec2 p = ivec2(low_uv() * vec2(sz));\n"
            "  if (texelFetch(dep, p, 0).r >= 0.999999) { o = vec4(0.5, 0.5, 0.5, 0.0); return; }\n"
            "  o = vec4(quick_normal(p) * 0.5 + 0.5, 1.0);\n"
            "}\n";
        // Trace (GI size, RGBA16F: rgb the gathered light, a the distance). b = (radius, thickness, origin bias,
        // max receiver distance), c = (rays, steps, frame turn, luminance clamp), d.y = hit rejection strength.
        // Per ray: cosine-distributed around the normal, steps placed quadratically (dense near the origin); the first
        // depth crossing is refined twice by bisection and is the end of the ray, hit or not (no leaking through
        // walls); a hit counts if within the thickness, faces the ray, lies on the ray (not just inside a depth slab)
        // and faces the camera - FastGI's validation. Its light: the scene pixel there, point sampled, luminance clamped.
        // ldep / nrm: the GI-size distance and normals; dep (the full-size depth) only gives the full size: the position
        // of a GI pixel is its full-size pixel's.
        static const char *trace =
            "layout(set = 0, binding = 0) uniform sampler2D ldep;\n"
            "layout(set = 0, binding = 1) uniform sampler2D nrm;\n"
            "layout(set = 0, binding = 2) uniform sampler2D scene;\n"
            "layout(set = 0, binding = 3) uniform sampler2D dep;\n"
            "vec2 fs, ls;\n"
            "vec2 at_uv(ivec2 q) { return (vec2(ivec2((vec2(q) + 0.5) / ls * fs)) + 0.5) / fs; }\n"
            "float dist_at(ivec2 q) { float v = texelFetch(ldep, q, 0).r; return v > 0.0 ? exp2(v * 16.0) : 0.0; }\n"
            "void main() {\n"
            "  fs = vec2(textureSize(dep, 0)), ls = pc.d.zw;\n"
            "  vec2 cs = vec2(textureSize(scene, 0));\n"
            "  ivec2 lp = ivec2(gl_FragCoord.xy);\n"
            "  float l0 = dist_at(lp);\n"
            "  if (l0 <= 0.0) { o = vec4(0.0); return; }\n"
            "  vec4 ne = texelFetch(nrm, lp, 0);\n"
            "  if (l0 >= pc.b.w || ne.a < 0.5) { o = vec4(0.0, 0.0, 0.0, l0); return; }\n"
            "  vec3 n = dec_n(ne);\n"
            "  vec3 org = vpos(at_uv(lp), l0) + n * pc.b.z;\n"
            "  vec3 ax = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);\n"
            "  vec3 tg = normalize(cross(ax, n)), bt = cross(n, tg);\n"
            "  float noise = fract(52.9829189 * fract(dot(floor(gl_FragCoord.xy), vec2(0.06711056, 0.00583715))) + pc.c.z);\n"
            "  int rays = int(pc.c.x), steps = int(pc.c.y);\n"
            "  float R = pc.b.x, th = pc.b.y, bias = pc.b.z;\n"
            "  vec3 rad = vec3(0.0);\n"
            "  for (int ray = 0; ray < rays; ray++) {\n"
            "    float u = (float(ray) + 0.5) / float(rays), an = 6.2831853 * fract(noise + float(ray) * 0.618034);\n"
            "    vec3 dir = tg * (sqrt(u) * cos(an)) + bt * (sqrt(u) * sin(an)) + n * sqrt(1.0 - u);\n"
            "    float prev = 0.0;\n"
            "    for (int i = 0; i < steps; i++) {\n"
            "      float t = (float(i) + 0.75 + noise * 0.25) / float(steps), dist = R * t * t;\n"
            "      vec3 p = org + dir * dist;\n"
            "      if (p.z <= 1.0) break;\n"
            "      vec2 h = puv(p);\n"
            "      if (any(lessThanEqual(h, vec2(0.0))) || any(greaterThanEqual(h, vec2(1.0)))) break;\n"
            "      float sd = dist_at(ivec2(h * ls));\n"
            "      if (sd <= 0.0) { prev = dist; continue; }\n"
            "      float delta = p.z - sd;\n"
            "      if (delta < 0.0) { prev = dist; continue; }\n"
            "      float lo = prev, hi = dist;\n"
            "      for (int k = 0; k < 2; k++) {\n"
            "        float mid = (lo + hi) * 0.5;\n"
            "        vec3 mp = org + dir * mid;\n"
            "        vec2 mh = puv(mp);\n"
            "        if (any(lessThanEqual(mh, vec2(0.0))) || any(greaterThanEqual(mh, vec2(1.0)))) break;\n"
            "        float md = dist_at(ivec2(mh * ls)), mdel = mp.z - md;\n"
            "        if (md > 0.0 && mdel >= 0.0) { hi = mid; h = mh; sd = md; delta = mdel; } else lo = mid;\n"
            "      }\n"
            "      dist = hi;\n"
            "      t = sqrt(hi / R);\n"
            "      if (delta <= th && dist > bias * 2.0) {\n"
            "        ivec2 hp = ivec2(h * ls);\n"
            "        vec3 hn = dec_n(texelFetch(nrm, hp, 0));\n"
            "        float facing = clamp(dot(hn, -dir) * 4.0, 0.0, 1.0), conf = 1.0;\n"
            "        if (pc.d.y > 0.0) {\n"
            "          float hl = sd;\n"
            "          vec3 hit = vpos(at_uv(hp), hl), to = hit - org;\n"
            "          float along = dot(to, dir), off = length(to - dir * along);\n"
            // (a GI pixel's footprint: the hit is found to within one)
            "          float foot = 2.0 * hl / (ls.y * pc.a.y);\n"
            "          float g = clamp(1.0 - off / max(1.0, min(th * 0.25, 10.0) + foot * 1.5), 0.0, 1.0);\n"
            "          vec3 ad = to * inversesqrt(max(dot(to, to), 1e-6));\n"
            "          g *= smoothstep(0.0, 0.15, dot(n, ad)) * smoothstep(0.0, 0.15, dot(hn, -ad));\n"
            "          g *= smoothstep(0.05, 0.25, dot(hn, normalize(-hit)));\n"
            "          g *= step(bias, along) * step(along, R);\n"
            "          conf = mix(1.0, g, pc.d.y);\n"
            "        }\n"
            "        vec2 e = min(h, 1.0 - h);\n"
            "        float fade = clamp(min(e.x, e.y) * 20.0, 0.0, 1.0) * (1.0 - t * t);\n"
            "        vec3 col = max(texelFetch(scene, ivec2(h * cs), 0).rgb, vec3(0.0));\n"
            "        col *= min(1.0, pc.c.w / max(dot(col, vec3(0.2126, 0.7152, 0.0722)), 1e-4));\n"
            "        rad += col * facing * fade * conf;\n"
            "      }\n"
            "      break;\n"
            "    }\n"
            "  }\n"
            "  float df = clamp((pc.b.w - l0) / max(1.0, pc.b.w * 0.2), 0.0, 1.0);\n"
            "  o = vec4(rad * (df / float(rays)), l0);\n"
            "}\n";
        // Bilateral blur along b.xy (GI pixels), 5 taps at b.z spacing
        static const char *blur =
            "layout(set = 0, binding = 0) uniform sampler2D gi;\n"
            "layout(set = 0, binding = 1) uniform sampler2D nrm;\n"
            "void main() {\n"
            "  ivec2 ls = ivec2(pc.d.zw), p = ivec2(gl_FragCoord.xy);\n"
            "  vec4 c = texelFetch(gi, p, 0);\n"
            "  if (c.a <= 0.0) { o = vec4(0.0); return; }\n"
            "  vec3 n = dec_n(texelFetch(nrm, p, 0));\n"
            "  vec3 s = c.rgb;\n"
            "  float ws = 1.0;\n"
            "  for (int k = -2; k <= 2; k++) {\n"
            "    if (k == 0) continue;\n"
            "    ivec2 q = clamp(p + ivec2(pc.b.xy * (sign(float(k)) * floor(abs(float(k)) * pc.b.z + 0.5))), ivec2(0), ls - 1);\n"
            "    vec4 tp = texelFetch(gi, q, 0);\n"
            "    vec3 tn = dec_n(texelFetch(nrm, q, 0));\n"
            "    float w = (abs(k) == 1 ? 0.75 : 0.375) * geo(c.a, tp.a, n, tn);\n"
            "    s += tp.rgb * w; ws += w;\n"
            "  }\n"
            "  o = vec4(s / ws, c.a);\n"
            "}\n";
        // Temporal (GI size): the history where the pixel was (camera motion vectors at full size: xy velocity, z
        // previous / current distance), each of its 4 bilinear taps kept only at the expected distance, clamped to
        // this frame's neighbourhood (+15% of its range), weight b.y lowered with screen motion; b.w = 1: no history.
        static const char *temporal =
            "layout(set = 0, binding = 0) uniform sampler2D cur;\n"
            "layout(set = 0, binding = 1) uniform sampler2D mv;\n"
            "layout(set = 0, binding = 2) uniform sampler2D hist;\n"
            "void main() {\n"
            "  ivec2 ls = ivec2(pc.d.zw), p = ivec2(gl_FragCoord.xy);\n"
            "  vec4 c = texelFetch(cur, p, 0);\n"
            "  if (c.a <= 0.0 || pc.b.w > 0.5) { o = c; return; }\n"
            "  vec2 uv = low_uv();\n"
            "  vec4 m = texelFetch(mv, ivec2(uv * vec2(textureSize(mv, 0))), 0);\n"
            "  vec2 pv = uv - m.xy;\n"
            "  if (any(lessThanEqual(pv, vec2(0.0))) || any(greaterThanEqual(pv, vec2(1.0)))) { o = c; return; }\n"
            "  float le = m.z * c.a;\n"
            "  vec2 px = pv * pc.d.zw - 0.5, b0 = floor(px), f = px - b0;\n"
            "  vec3 h = vec3(0.0);\n"
            "  float ws = 0.0;\n"
            "  for (int y = 0; y < 2; y++)\n"
            "    for (int x = 0; x < 2; x++) {\n"
            "      vec4 tp = texelFetch(hist, clamp(ivec2(b0) + ivec2(x, y), ivec2(0), ls - 1), 0);\n"
            "      vec2 bl = mix(1.0 - f, f, vec2(x, y));\n"
            "      float w = bl.x * bl.y * step(0.001, tp.a) * clamp(1.0 - abs(tp.a - le) / max(2.5, le * 0.01), 0.0, 1.0);\n"
            "      h += tp.rgb * w; ws += w;\n"
            "    }\n"
            "  if (ws < 0.05) { o = c; return; }\n"
            "  h /= ws;\n"
            "  vec3 lo = c.rgb, hi = c.rgb;\n"
            "  const ivec2 nb[4] = ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));\n"
            "  for (int i = 0; i < 4; i++) {\n"
            "    vec4 tp = texelFetch(cur, clamp(p + nb[i], ivec2(0), ls - 1), 0);\n"
            "    if (tp.a > 0.0 && abs(tp.a - c.a) < max(2.5, c.a * 0.01)) { lo = min(lo, tp.rgb); hi = max(hi, tp.rgb); }\n"
            "  }\n"
            "  vec3 mg = (hi - lo) * 0.15 + 0.001;\n"
            "  h = clamp(h, max(vec3(0.0), lo - mg), hi + mg);\n"
            "  float w = pc.b.y * clamp(ws, 0.0, 1.0) / (1.0 + length(m.xy * pc.d.zw) * 0.1);\n"
            "  o = vec4(mix(c.rgb, h, w), c.a);\n"
            "}\n";
        // Composite (full size): the GI's 4 nearest pixels, bilinear x bilateral; b.x = strength (intensity x
        // reflectance). Added onto the scene (exec.c: additive blending, colour only); b.y = 1 the debug view: the
        // light alone, b.z x it, tone mapped (x / (1 + x)) into RGBA8.
        // dep (binding 0) the full-size depth, nrm the GI-size normals; the pixel's own normal rebuilt here (5 reads:
        // 0.14 ms at 4K, as with the full-size normals read)
        static const char *composite =
            "layout(set = 0, binding = 1) uniform sampler2D gi;\n"
            "layout(set = 0, binding = 2) uniform sampler2D nrm;\n"
            "void main() {\n"
            "  ivec2 fsz = textureSize(dep, 0), ls = ivec2(pc.d.zw);\n"
            "  sz = fsz;\n"
            "  vec2 uv = gl_FragCoord.xy * pc.c.xy;\n"
            "  ivec2 fp = clamp(ivec2(uv * vec2(fsz)), ivec2(0), fsz - 1);\n"
            "  float d0 = texelFetch(dep, fp, 0).r;\n"
            "  vec3 g = vec3(0.0);\n"
            "  if (d0 < 0.999999) {\n"
            "    float l0 = lin(d0);\n"
            "    vec3 n = quick_normal(fp);\n"
            "    vec2 px = uv * pc.d.zw - 0.5, b0 = floor(px), f = px - b0;\n"
            "    float ws = 0.0;\n"
            "    for (int y = 0; y < 2; y++)\n"
            "      for (int x = 0; x < 2; x++) {\n"
            "        ivec2 q = clamp(ivec2(b0) + ivec2(x, y), ivec2(0), ls - 1);\n"
            "        vec4 tp = texelFetch(gi, q, 0);\n"
            "        vec3 tn = dec_n(texelFetch(nrm, q, 0));\n"
            "        vec2 bl = mix(1.0 - f, f, vec2(x, y));\n"
            "        float w = bl.x * bl.y * geo(l0, tp.a, n, tn);\n"
            "        g += tp.rgb * w; ws += w;\n"
            "      }\n"
            "    g = g / max(ws, 1e-4) * pc.b.x;\n"
            "  }\n"
            "  if (pc.b.y > 0.5) { g *= pc.b.z; o = vec4(g / (1.0 + g), 1.0); }\n"
            "  else o = vec4(g, 0.0);\n"
            "}\n";
        static char *src[6];
        int k = !strcmp(name, "gi_normal.frag") ? 0 : !strcmp(name, "gi_trace.frag") ? 1 : !strcmp(name, "gi_blur.frag") ? 2
              : !strcmp(name, "gi_temporal.frag") ? 3 : !strcmp(name, "gi_composite.frag") ? 4 : !strcmp(name, "gi_depth.frag") ? 5 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            const char *bodies[6] = {normal, trace, blur, temporal, composite, depth};
            const char *pre = k == 0 || k == 4 ? vpos_src : "", *pre2 = k == 0 || k == 4 ? qnormal : "";
            size_t n = strlen(common) + strlen(pre) + strlen(pre2) + strlen(bodies[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s%s", common, pre, pre2, bodies[k]);
        }
        return src[k];
    }
    // TAA (exec.c taa_run), on the game's HDR scene before its bloom: t = this frame (jittered), h = the history (rgb,
    // a = its distance), mv = the motion vectors (xy velocity, z previous / current distance), dep = depth.
    // a = the projection (P00, P11, A, B), b = (reset, history weight, depth mode, 0). Blended in a tone-mapped space
    // (c / (1 + max c): no fireflies from the bright highlights); the velocity of the nearest of the 3x3 (edges keep the
    // foreground's); the history by Catmull-Rom, clipped to the 3x3's YCoCg mean +- 1.25 sigma (variance clipping),
    // dropped where its distance is not the one expected (uncovered). Out: rgb, a = this distance.
    if (!strcmp(name, "taa.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D h;\n"
               "layout(set = 0, binding = 2) uniform sampler2D mv;\n"
               "layout(set = 0, binding = 3) uniform sampler2D dep;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float lin(float d) { return pc.a.w / ((pc.b.z > 0.5 ? d * 2.0 - 1.0 : d) + pc.a.z); }\n"
               "vec3 tm(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }\n"
               "vec3 itm(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }\n"
               "vec3 ycc(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }\n"
               "vec3 rgb(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }\n"
               "vec3 cat(vec2 uv, vec2 sz) {\n"  // Catmull-Rom, 5 bilinear taps (the corners dropped)
               "  vec2 sp = uv * sz, tp = floor(sp - 0.5) + 0.5, f = sp - tp;\n"
               "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
               "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
               "  vec2 w12 = w1 + w2, t0 = (tp - 1.0) / sz, t3 = (tp + 2.0) / sz, t12 = (tp + w2 / w12) / sz;\n"
               "  vec3 r = texture(h, vec2(t12.x, t0.y)).rgb * (w12.x * w0.y) + texture(h, vec2(t0.x, t12.y)).rgb * (w0.x * w12.y)\n"
               "         + texture(h, t12).rgb * (w12.x * w12.y) + texture(h, vec2(t3.x, t12.y)).rgb * (w3.x * w12.y)\n"
               "         + texture(h, vec2(t12.x, t3.y)).rgb * (w12.x * w3.y);\n"
               "  float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
               "  return max(r / ws, vec3(0.0));\n"
               "}\n"
               "void main() {\n"
               "  ivec2 sz = textureSize(t, 0), p = ivec2(gl_FragCoord.xy);\n"
               "  ivec2 dsz = textureSize(dep, 0);\n"
               "  vec3 c = max(texelFetch(t, p, 0).rgb, vec3(0.0));\n"
               "  vec2 dsc = vec2(dsz) / vec2(sz);\n"
               "  float l = lin(texelFetch(dep, min(ivec2(vec2(p) * dsc), dsz - 1), 0).r);\n"
               "  if (pc.b.x > 0.5) { o = vec4(c, l); return; }\n"
               "  vec3 m1 = vec3(0.0), m2 = vec3(0.0);\n"
               "  float dn = 1.0;\n"
               "  ivec2 np = p;\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      ivec2 q = clamp(p + ivec2(x, y), ivec2(0), sz - 1);\n"
               "      vec3 s = ycc(tm(max(texelFetch(t, q, 0).rgb, vec3(0.0))));\n"
               "      m1 += s; m2 += s * s;\n"
               "      float d = texelFetch(dep, min(ivec2(vec2(q) * dsc), dsz - 1), 0).r;\n"
               "      if (d < dn) { dn = d; np = q; }\n"
               "    }\n"
               "  m1 /= 9.0;\n"
               "  vec3 sg = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));\n"
               "  vec4 v = texelFetch(mv, min(ivec2(vec2(np) * vec2(textureSize(mv, 0)) / vec2(sz)), textureSize(mv, 0) - 1), 0);\n"
               "  vec2 uv = (vec2(p) + 0.5) / vec2(sz), pu = uv - v.xy;\n"
               "  if (any(lessThan(pu, vec2(0.0))) || any(greaterThan(pu, vec2(1.0)))) { o = vec4(c, l); return; }\n"
               "  ivec2 hs = textureSize(h, 0);\n"  // (an in-between image at a reduced size: N's history is larger)
               "  float lh = texelFetch(h, clamp(ivec2(pu * vec2(hs)), ivec2(0), hs - 1), 0).a, le = v.z * l;\n"
               "  float keep = clamp(1.0 - (abs(lh - le) / max(le * 0.02, 2.0) - 1.0), 0.0, 1.0);\n"
               "  vec3 hy = ycc(tm(cat(pu, vec2(hs))));\n"
               "  vec3 lo = m1 - 1.25 * sg, hi = m1 + 1.25 * sg, ce = 0.5 * (hi + lo), ex = 0.5 * (hi - lo) + 1e-4;\n"
               "  vec3 dv = (hy - ce) / ex;\n"
               "  float mx = max(abs(dv.x), max(abs(dv.y), abs(dv.z)));\n"
               "  if (mx > 1.0) hy = ce + (hy - ce) / mx;\n"
               "  vec3 cy = ycc(tm(c));\n"
               "  float w = pc.b.y * keep;\n"
               "  o = vec4(itm(rgb(mix(cy, hy, w))), l);\n"
               "}\n";
    }
    // FSR 3's mask of the interface (exec.c post_process): the pixels the interface drew over the composited scene
    // (t0 the final image, t1 its copy before the first interface draw) - 1 there: FSR takes them as they are
    if (!strcmp(name, "ui_mask.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
               "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec3 d = abs(texelFetch(t0, p, 0).rgb - texelFetch(t1, min(p, textureSize(t1, 0) - 1), 0).rgb);\n"
               "  o = vec4(max(d.r, max(d.g, d.b)) > 2.0 / 255.0 ? 1.0 : 0.0);\n"
               "}\n";
    }
    // The scene depth as R32F (FSR 3 reads a colour image: exec.c post_process)
    if (!strcmp(name, "depth_r32.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = vec4(texelFetch(dep, ivec2(gl_FragCoord.xy), 0).r); }\n";
    }
    // N's result moved to an in-between image (exec.c, frame interpolation: its SSAO / GI are N's, taken where its
    // motion vectors - relative to N - say each pixel was): src at uv - velocity (uv itself off the image).
    // b.xy = 1 / the output's size.
    if (!strcmp(name, "reproject.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D src;\n"
               "layout(set = 0, binding = 1) uniform sampler2D mv;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.b.xy;\n"
               "  ivec2 ms = textureSize(mv, 0);\n"
               "  vec2 puv = uv - texelFetch(mv, min(ivec2(uv * vec2(ms)), ms - 1), 0).xy;\n"
               "  if (any(lessThan(puv, vec2(0.0))) || any(greaterThanEqual(puv, vec2(1.0)))) puv = uv;\n"
               "  o = texture(src, puv);\n"
               "}\n";
    }
    // An in-between image with FSR 3.1 (exec.c post_process, frame interpolation): FSR's own history goes forward in time
    // with the real frames, so the image is resolved here instead, like TAA - t = the image (render size, after the
    // colour pass), h = N's FSR 3.1 output (the output's size), taken where the motion vectors (relative to N) say each
    // pixel was and clipped to t's 3x3 (YCoCg min / max), mixed with t by the history weight. Uncovered or off-image: t.
    // h is sampled Catmull-Rom (5 bilinear taps): bilinear, reprojecting softened it and the in-between images were 13 %
    // less detailed than the real frames - a sharpness that pulsed at the frame rate (exec.c then adds FSR's RCAS).
    // a.xy = 1 / the output's size, a.zw = 1 / t's size, b.x = the history's weight.
    if (!strcmp(name, "side_resolve.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D h;\n"
               "layout(set = 0, binding = 2) uniform sampler2D mv;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 ycc(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }\n"
               "vec3 rgb(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }\n"
               "vec3 cr(vec2 uv) {\n"  // (Catmull-Rom from 5 bilinear taps, the corners left out)
               "  vec2 sz = vec2(textureSize(h, 0)), p = uv * sz, c1 = floor(p - 0.5) + 0.5, f = p - c1;\n"
               "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
               "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
               "  vec2 w12 = w1 + w2, t0 = (c1 - 1.0) / sz, t3 = (c1 + 2.0) / sz, t12 = (c1 + w2 / w12) / sz;\n"
               "  vec3 r = texture(h, vec2(t12.x, t0.y)).rgb * w12.x * w0.y + texture(h, vec2(t0.x, t12.y)).rgb * w0.x * w12.y\n"
               "         + texture(h, t12).rgb * w12.x * w12.y + texture(h, vec2(t3.x, t12.y)).rgb * w3.x * w12.y\n"
               "         + texture(h, vec2(t12.x, t3.y)).rgb * w12.x * w3.y;\n"
               "  float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
               "  return max(r / ws, vec3(0.0));\n"
               "}\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.a.xy;\n"
               "  vec3 c = texture(t, uv).rgb;\n"
               "  vec3 mn = vec3(1e9), mx = vec3(-1e9);\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      vec3 s = ycc(texture(t, uv + vec2(x, y) * pc.a.zw).rgb);\n"
               "      mn = min(mn, s); mx = max(mx, s);\n"
               "    }\n"
               "  ivec2 ms = textureSize(mv, 0);\n"
               "  vec2 puv = uv - texelFetch(mv, min(ivec2(uv * vec2(ms)), ms - 1), 0).xy;\n"
               "  if (any(lessThan(puv, vec2(0.0))) || any(greaterThanEqual(puv, vec2(1.0)))) { o = vec4(c, 1.0); return; }\n"
               "  vec3 hc = rgb(clamp(ycc(cr(puv)), mn, mx));\n"
               "  o = vec4(mix(c, hc, pc.b.x), 1.0);\n"
               "}\n";
    }
    // FSR 3.1's output with the interface composited after it (exec.c post_process_ex): t0 = FSR's output, t1 = the image
    // it was given (the render size, the interface not jittered), m = the interface mask (render size). Where the mask
    // says interface (bilinear: its edges blend), t1 by Catmull-Rom (5 bilinear taps, as side_resolve.frag's history).
    // a.xy = 1 / the output's size.
    if (!strcmp(name, "ui_composite.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
               "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
               "layout(set = 0, binding = 2) uniform sampler2D m;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 cr(vec2 uv) {\n"
               "  vec2 sz = vec2(textureSize(t1, 0)), p = uv * sz, c1 = floor(p - 0.5) + 0.5, f = p - c1;\n"
               "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
               "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
               "  vec2 w12 = w1 + w2, q0 = (c1 - 1.0) / sz, q3 = (c1 + 2.0) / sz, q12 = (c1 + w2 / w12) / sz;\n"
               "  vec3 r = texture(t1, vec2(q12.x, q0.y)).rgb * w12.x * w0.y + texture(t1, vec2(q0.x, q12.y)).rgb * w0.x * w12.y\n"
               "         + texture(t1, q12).rgb * w12.x * w12.y + texture(t1, vec2(q3.x, q12.y)).rgb * w3.x * w12.y\n"
               "         + texture(t1, vec2(q12.x, q3.y)).rgb * w12.x * w3.y;\n"
               "  float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
               "  return clamp(r / ws, 0.0, 1.0);\n"
               "}\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.a.xy;\n"
               "  vec3 f = texelFetch(t0, ivec2(gl_FragCoord.xy), 0).rgb;\n"
               "  float k = texture(m, uv).r;\n"
               "  o = vec4(k > 0.0 ? mix(f, cr(uv), k) : f, 1.0);\n"
               "}\n";
    }
    // An in-between image with frame N's interface where N's mask says interface (exec.c ip_ui_keep): t = the image,
    // h = N's processed image of the same size, m = N's mask (the render size: sampled, any coverage counts - its edges'
    // antialiasing too). a.xy = 1 / the size.
    if (!strcmp(name, "ui_keep.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1) uniform sampler2D h;\n"
               "layout(set = 0, binding = 2) uniform sampler2D m;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.a.xy;\n"
               "  o = vec4(texture(m, uv).r > 0.0 ? texture(h, uv).rgb : texelFetch(t, ivec2(gl_FragCoord.xy), 0).rgb, 1.0);\n"
               "}\n";
    }
    // A post image back into a target (b.xy = 1 / the output's size; sampled, the sizes may differ): TAA's result into
    // the game's scene
    if (!strcmp(name, "copy.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = vec4(texture(t, gl_FragCoord.xy * pc.b.xy).rgb, 1.0); }\n";
    }
    // Camera motion vectors (exec.c scene_hook_run): each pixel's position, rebuilt from its depth, taken to the
    // previous frame by m = previous view-projection x inverse of this one (the game's matrices, GL style: NDC y up).
    // a.x = 1: depth stored as (z_ndc + 1) / 2; a.y = the sign of NDC y at the bottom of the image (+1: row 0 is NDC
    // y = -1). Out (RGBA16F): xy = velocity = this frame's uv - the previous frame's, in uv units; z = the pixel's
    // distance in the previous frame / its distance now (q.w: the input point is this frame's clip / its w), for the
    // temporal effects' disocclusion test.
    if (!strcmp(name, "mv.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 1) uniform sampler2D obj;\n"  // (exec.c: per-object velocities, w = 1 written)
               "layout(push_constant) uniform P { mat4 m; vec4 a; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 sz = textureSize(dep, 0), p = ivec2(gl_FragCoord.xy);\n"
               "  vec4 ov = texelFetch(obj, min(p, textureSize(obj, 0) - 1), 0);\n"
               "  if (ov.w > 0.5) { o = vec4(ov.xyz, 1.0); return; }\n"
               "  float d = texelFetch(dep, p, 0).r;\n"
               "  vec2 uv = (vec2(p) + 0.5) / vec2(sz);\n"
               "  vec2 n = uv * 2.0 - 1.0;\n"
               "  n.y *= pc.a.y;\n"
               "  vec4 q = pc.m * vec4(n, pc.a.x > 0.5 ? d * 2.0 - 1.0 : d, 1.0);\n"
               "  vec2 pn = q.xy / q.w;\n"
               "  pn.y *= pc.a.y;\n"
               "  o = vec4(uv - (pn * 0.5 + 0.5), q.w, 1.0);\n"
               "}\n";
    }
    // Motion vectors shown (b.xy = 1 / the output's size, b.z = the scale: velocity in uv x b.z around mid-grey) - the
    // F12 debug view onto the scene, and LARECOMP_MV_DUMP's RGBA8 encoding (b.z = 32: +-1/64 of the image, 8 bits).
    if (!strcmp(name, "mv_show.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 v = texture(t, gl_FragCoord.xy * pc.b.xy).rg * pc.b.z;\n"
               "  o = vec4(clamp(0.5 + v, 0.0, 1.0), 0.5, 1.0);\n"
               "}\n";
    }
    // The occlusion onto the scene: multiplied by the pipeline's blending (exec.c), or shown alone (the debug view and
    // LARECOMP_SSAO_DUMP). b.xy = 1 / the output's size (the occlusion is at the depth's size: sampled, not fetched);
    // b.z = a scale when shown alone (the scene is HDR: 1.0 is far past white after the game's tone mapping), 0: none.
    if (!strcmp(name, "ssao_apply.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  float a = texture(t, gl_FragCoord.xy * pc.b.xy).r;\n"
               "  o = vec4(vec3(a * (pc.b.z > 0.0 ? pc.b.z : 1.0)), 1.0);\n"
               "}\n";
    }
    // Colour adjustment (F12 menu: runtime/ui/menu.mm), on the gamma-encoded image. a = (brightness, contrast,
    // saturation): brightness scales, contrast pivots around mid-grey, saturation mixes with the Rec. 709 luma.
    if (!strcmp(name, "color.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec3 c = texelFetch(t, ivec2(gl_FragCoord.xy), 0).rgb * pc.a.x;\n"
               "  c = (c - 0.5) * pc.a.y + 0.5;\n"
               "  c = mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, pc.a.z);\n"
               "  o = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "clear.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, pc.d.x, 1.0);\n"
               "}\n";
    }
    // clear.frag.<f|i|u><index>: writes the push-constant colour to colour target <index>
    if (!strncmp(name, "clear.frag.", 11)) {
        *stage = STAGE_FRAGMENT;
        static __thread char buf[512];
        char k = name[11];
        int idx = atoi(name + 12);
        const char *ty = k == 'i' ? "ivec4" : k == 'u' ? "uvec4" : "vec4";
        const char *cv = k == 'i' ? "floatBitsToInt(pc.c)" : k == 'u' ? "floatBitsToUint(pc.c)" : "pc.c";
        snprintf(buf, sizeof buf,
                 "#version 450\n"
                 "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
                 "layout(location = %d) out %s o;\n"
                 "void main() { o = %s; }\n", idx, ty, cv);
        return buf;
    }
    if (!strcmp(name, "clear.frag.none")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nvoid main() {}\n";
    }
    return NULL;
}

VkShaderModule vk_builtin_shader(const char *name) {
    static struct { char name[32]; VkShaderModule m; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = builtin_src(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 64) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}
