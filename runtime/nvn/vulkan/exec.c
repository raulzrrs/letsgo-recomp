// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <SDL3/SDL_timer.h>
#include <float.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../kernel.h"
#include "../../platform.h"
#include "shaders.h"
#include "vk.h"
#include "../renderer.h"
#include "../../settings.h"
#include "fsr3.h"

static uint64_t g_draws_total;  // (never reset: LARECOMP_COUNTER_LOG)
static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
// Skipped draws by reason (logged with the stats): 0 no program bound, 1 program still compiling, 2 program failed,
// 3 no program object, 4 primitive mode, 5 pipeline still building, 6 pipeline failed.
static uint64_t stat_skip_why[8];
uint64_t g_skin_watch;  // (debug, LARECOMP_DUMP_SKIN: a vertex cbuf watched across frames)
// docs/PLANO-3D.md experiments, logged with the stats: [0] palettes replaced by identity, [1] skinned-draw cbufs
// read in place (UBO_DIRECT), [2] skinned-draw cbufs UBO_DIRECT could not place (copied), [3] one-bone shaders.
static uint64_t g_exp_count[4];
#define SKIP(why) (stat_skipped++, stat_skip_why[(why) & 7]++)
static uint64_t stat_upload_bytes;
// LARECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu, prof_ipc;
static VkQueryPool prof_qp;
// GPU time of the submissions (their first and last timestamps), with LARECOMP_VK_PROF or the performance overlay on:
// its share of the time is the overlay's GPU use (ui/menu.mm)
static _Atomic uint64_t gpu_busy_ns;
uint64_t renderer_gpu_busy_ns(void) { return gpu_busy_ns; }
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
static _Atomic uint64_t stat_translate_ns, stat_spirv_ns, stat_pipe_ns;  // CPU time translating / compiling programs
static uint64_t stat_shader_wait_ns;  // draws waiting for their program / pipeline (wait_built)
static _Atomic int new_pipes;  // pipelines created since the pipeline cache was last saved
static uint64_t op_counts[CMD_COUNT];
static uint64_t prof_op_ns[CMD_COUNT], prof_op_n[CMD_COUNT];
// (LARECOMP_VK_PROF: vertex / index data copied to the GPU ring - time and bytes - and the index scans; with LARECOMP_VCOPY_SAME=1
// also how many of the vertex bytes copied were the same as the last copy of that range, a hash per range)
static uint64_t prof_gcpy_ns, prof_gcpy_bytes, prof_scan_ns, prof_vsame_bytes, prof_vall_bytes, prof_gcpy_wait_ns;  // (LARECOMP_VK_PROF: the CPU time of each command type, nested lists apart)
static uint64_t stat_vtx_copied, stat_vtx_direct;  // vertex / index ranges of the draws: copied to VRAM / read in place
bool g_debug_frame;
static char stats_text[2048];
void vk_collect_dead(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 8
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    uint64_t seq;
    VkDescriptorPool pools[16];
    int npools, cur_pool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static uint64_t submit_seq;  // last submitted
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;
// Deferred clears (clear_defer, below): pending until the next pass takes them as its load op, or recorded on their own
// before anything else reaches the command buffer (vk_cmd)
static int pclear_n;
static bool pclear_hold;
static void pclear_flush(void);

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

// LARECOMP_VK_PROF: the GPU waits by caller in the stats line ("waits": offset in this library for addr2line: ms/count)
static struct { uintptr_t ra; uint64_t ns, n; } wsite[16];
void vk_wait_sites_log(char *out, size_t cap) {
    uintptr_t base = 0;  // (Windows: absolute addresses)
#ifndef _WIN32
    Dl_info di = {0};
    if (dladdr((void *)vk_wait_sites_log, &di)) base = (uintptr_t)di.dli_fbase;
#endif
    size_t o = 0;
    for (int i = 0; i < 16 && wsite[i].ra; i++) {
        o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, " %lx:%.0fms/%llu", (unsigned long)(wsite[i].ra - base),
                              wsite[i].ns / 1e6, (unsigned long long)wsite[i].n);
        wsite[i].ns = wsite[i].n = 0;
    }
}

__attribute__((noinline)) void vk_wait_seq(uint64_t seq) {
    if (!seq) return;
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    stat_wait_ns += SDL_GetTicksNS() - t0;
    cum_wait += SDL_GetTicksNS() - t0;
    for (int i = 0; i < 16; i++)
        if (wsite[i].ra == ra || !wsite[i].ra) { wsite[i].ra = ra; wsite[i].ns += SDL_GetTicksNS() - t0; wsite[i].n++; break; }
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r != VK_SUCCESS) {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        vk_wait_seq(s->seq);
        if (s->ts) {  // (profiling) GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) {
                uint64_t ns = (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
                prof_gpu += ns;
                gpu_busy_ns += ns;
            }
            s->ts = false;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        s->cur_pool = 0;
        pthread_mutex_lock(&seq_lock);
        s->main_begun = s->upload_begun = false;
        s->seq = submit_seq + 1;
        pthread_mutex_unlock(&seq_lock);
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    pthread_mutex_lock(&seq_lock);
    *begun = true;
    pthread_mutex_unlock(&seq_lock);
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
VkCommandBuffer vk_cmd(void) {
    if (pclear_n && !pclear_hold) pclear_flush();  // (whatever comes now may read or write a target cleared before it)
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if ((prof || g_settings.show_fps) && prof_qp) {
            vkCmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 2, 2);
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 2);
            s->ts = true;
        }
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    begin_cb(&s->upload, &s->upload_begun);
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
// A barrier from (src stages, their accesses) to (dst stages, theirs): what our own passes need, where the full one made
// the GPU drain everything before each of them
void vk_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                           .dstStageMask = ds, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
// Before one of our full-screen passes (post_pass_view): anything written before - a pass, a copy, a dispatch - read by
// its fragment shader or under its colour attachment, and anything read before written by it. What comes after one of
// them and is not another (the game's passes, copies, dispatches, FSR 3.1) has a full barrier of its own first.
#define POST_DST_STAGES (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | \
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)
#define POST_DST_ACCESS (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | \
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT)
static void vk_post_barrier(VkCommandBuffer cb) {
    vk_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, POST_DST_STAGES, POST_DST_ACCESS);
}

static void ring_retire(void);

// ---- LARECOMP_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
static VkQueryPool gp_qp;
static int gp_n;
#define GP_MAX 4096
static char gp_label[GP_MAX][112];
static uint32_t gp_draws[GP_MAX];
// LARECOMP_GPUPROF_STATS=1 (with LARECOMP_GPUPROF_DRAWS): the pipeline statistics of every draw of the profiled frames
// (input vertices, vertex shader invocations, clipped primitives, fragment shader invocations), printed on its row.
static VkQueryPool gps_qp;
static int gps_n;
static int gp_stat[GP_MAX];  // mark -> its index in gps_qp + 1 (0: none)
// LARECOMP_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with LARECOMP_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = getenv("LARECOMP_VK_DEVLOST_TRACE") ? atoi(getenv("LARECOMP_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
uint64_t vk_frame_counter(void);
static uint64_t gp_frame;  // the frame counter when the profiled frame began (parity: LARECOMP_EXP_ALT)
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= GP_MAX - 1) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_MAX};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) {
        gp_frame = vk_frame_counter();
        vkCmdResetQueryPool(cb, gp_qp, 0, GP_MAX);
        if (getenv("LARECOMP_GPUPROF_STATS")) {
            if (!gps_qp) {
                VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
                                            .queryCount = GP_MAX,
                                            .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                                                                  VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                                                  VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                                                  VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};
                if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gps_qp) != VK_SUCCESS) gps_qp = VK_NULL_HANDLE;
            }
            if (gps_qp) vkCmdResetQueryPool(cb, gps_qp, 0, GP_MAX);
            gps_n = 0;
        }
    }
    gp_stat[gp_n] = 0;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
// (fsr3.cpp: a row per FidelityFX dispatch in a profiled frame)
void vk_gp_mark_str(VkCommandBuffer cb, const char *s) { gp_mark(cb, "%s", s); }
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    static uint64_t t[GP_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (frame %llu; rows: the work after each mark, until the next)\n", gp_n, total,
            (unsigned long long)gp_frame);
        static uint64_t stats[GP_MAX * 4];
        bool have_stats = gps_qp && gps_n &&
            vkGetQueryPoolResults(g_vk.dev, gps_qp, 0, (uint32_t)gps_n, sizeof stats, stats, 32, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS;
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            bool st = have_stats && gp_stat[i];
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] || st ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
            if (st) {
                const uint64_t *q = &stats[(gp_stat[i] - 1) * 4];  // (the enabled bits, in order: IA vertices, VS invocations, clip prims, FS invocations)
                LOG(" | ia %llu vsinv %llu clipprim %llu fsinv %llu\n", (unsigned long long)q[0], (unsigned long long)q[1],
                    (unsigned long long)q[2], (unsigned long long)q[3]);
            }
        }
    }
    gp_n = 0;
}

static void ring_flush_gpu(void);
static void acopy_drain(void);
// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    acopy_drain();  // (the vertex / index copies this submission reads)
    ring_flush_gpu();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    // (closes the submission's last row: without it the frame's last pass - FSR 3.1, the present, the menus - had no
    // row of its own; this one's row is the gap until the next submission)
    gp_mark(s->main, "(submission end; until the next submission)");
    if (s->ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                     .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbs,
                        .signalSemaphoreInfoCount = signal ? 2 : 1, .pSignalSemaphoreInfos = sig,
                        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        if (sr == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
        VK_CHECK(sr);
        PROF_ADD(prof_submit);
    }
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    cur_sub = (cur_sub + 1) % SUBS;
    pthread_mutex_unlock(&seq_lock);
    stat_flushes++;
    cum_submits++;
    ring_retire();
    vk_collect_dead();
}

void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE); }

void renderer_flush(void) {
    renderer_submit();
    vk_wait_seq(submit_seq);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    Sub *s = &subs[cur_sub];
    // A reused slot keeps its old begun flags until sub() prepares it: an empty queue's fence must name the last
    // submission, not a future one that submit's empty-work fast path never signals (LAS Recomp 37b7ff6).
    bool pending = s->seq == submit_seq + 1 && (s->main_begun || s->upload_begun);
    uint64_t q = submit_seq + (pending ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 16) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = 5, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        s->cur_pool++;
    }
}

// ================================================================== transient memory ring
// The largest index of a draw (bounds the vertex data copied for it), ignoring the restart value: AVX2 where the CPU has it (the
// game code needs it too), NEON on AArch64, a plain loop elsewhere. A draw scans a few thousand indices: ~1.4 us scalar, ~0.15 us here.
#if defined(__x86_64__)
__attribute__((target("avx2"))) static uint32_t scan_max16_simd(const uint16_t *q, uint32_t n, uint16_t restart) {
    __m256i acc = _mm256_setzero_si256(), rs = _mm256_set1_epi16((short)restart);
    uint32_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(q + i));
        acc = _mm256_max_epu16(acc, _mm256_andnot_si256(_mm256_cmpeq_epi16(v, rs), v));
    }
    uint16_t t[16];
    _mm256_storeu_si256((__m256i *)t, acc);
    uint32_t m = 0;
    for (int k = 0; k < 16; k++) if (t[k] > m) m = t[k];
    for (; i < n; i++) if (q[i] != restart && q[i] > m) m = q[i];
    return m;
}
__attribute__((target("avx2"))) static uint32_t scan_max32_simd(const uint32_t *q, uint32_t n, uint32_t restart) {
    __m256i acc = _mm256_setzero_si256(), rs = _mm256_set1_epi32((int)restart);
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(q + i));
        acc = _mm256_max_epu32(acc, _mm256_andnot_si256(_mm256_cmpeq_epi32(v, rs), v));
    }
    uint32_t t[8];
    _mm256_storeu_si256((__m256i *)t, acc);
    uint32_t m = 0;
    for (int k = 0; k < 8; k++) if (t[k] > m) m = t[k];
    for (; i < n; i++) if (q[i] != restart && q[i] > m) m = q[i];
    return m;
}
#elif defined(__aarch64__)
static uint32_t scan_max16_simd(const uint16_t *q, uint32_t n, uint16_t restart) {
    uint16x8_t acc = vdupq_n_u16(0), rs = vdupq_n_u16(restart);
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint16x8_t v = vld1q_u16(q + i);
        acc = vmaxq_u16(acc, vbicq_u16(v, vceqq_u16(v, rs)));
    }
    uint32_t m = vmaxvq_u16(acc);
    for (; i < n; i++) if (q[i] != restart && q[i] > m) m = q[i];
    return m;
}
static uint32_t scan_max32_simd(const uint32_t *q, uint32_t n, uint32_t restart) {
    uint32x4_t acc = vdupq_n_u32(0), rs = vdupq_n_u32(restart);
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32x4_t v = vld1q_u32(q + i);
        acc = vmaxq_u32(acc, vbicq_u32(v, vceqq_u32(v, rs)));
    }
    uint32_t m = vmaxvq_u32(acc);
    for (; i < n; i++) if (q[i] != restart && q[i] > m) m = q[i];
    return m;
}
#endif
static uint32_t scan_max_index_(const void *src, uint32_t isz, uint32_t n, uint32_t restart) {
#if defined(__x86_64__)
    static int simd = -1;
    if (simd < 0) simd = __builtin_cpu_supports("avx2");
#elif defined(__aarch64__)
    const int simd = 1;
#else
    const int simd = 0;
#endif
    uint32_t m = 0;
    if (isz == 2) {
        if (simd) return scan_max16_simd(src, n, (uint16_t)restart);
        const uint16_t *q = src;
        for (uint32_t i = 0; i < n; i++) if (q[i] > m && q[i] != (uint16_t)restart) m = q[i];
    } else {
        if (simd) return scan_max32_simd(src, n, restart);
        const uint32_t *q = src;
        for (uint32_t i = 0; i < n; i++) if (q[i] > m && q[i] != restart) m = q[i];
    }
    return m;
}
static uint32_t scan_max_index(const void *src, uint32_t isz, uint32_t n, uint32_t restart) {
    PROF_T0();
    uint32_t m = scan_max_index_(src, isz, n, restart);
    PROF_ADD(prof_scan_ns);
    return m;
}

// Two pools of 64 MB chunks, a chunk reused once every submission that allocated from it has completed.
// Pool 0: host-visible chunks (VRAM through the BAR when the GPU has one) the GPU reads in place: constants and the sources of
// texture uploads. Pool 1 (ring_gpu_enabled, discrete GPUs): the vertex and index data of the draws, in VRAM. The CPU writes a
// staging chunk in system memory and each submission starts with a copy of what was written since the last one (ring_flush_gpu),
// so the GPU fetches vertices from VRAM instead of reading the game's memory over PCIe once per vertex invocation (an
// instanced draw re-read its per-instance streams for every vertex: ~0.8 ms for 71 thousand invocations, docs/PESQUISA-GPU.md).
#define CHUNK_SIZE (64ull << 20)
// (epoch: how many times the chunk was taken again - a span kept from before (vtx_memo) is valid while it is the same)
typedef struct { VkBuffer buf, dev; VmaAllocation mem, dmem; uint8_t *ptr; uint64_t size, used, seq, flushed; uint32_t epoch; } Chunk;
typedef struct { Chunk *c; int n, active; bool gpu; } Pool;
static Pool pools[2] = {{.active = -1}, {.active = -1, .gpu = true}};

// LARECOMP_VERTEX_READ=direct|copy|auto (default auto: copies on a discrete GPU, in place on an integrated one, whose memory
// is the system's). The draws' own ranges the GPU writes stay in place either way (gpuw below).
static bool ring_gpu_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_VERTEX_READ");
        if (!e) e = g_settings.vertex_read == VREAD_DIRECT ? "direct" : g_settings.vertex_read == VREAD_COPY ? "copy" : NULL;
        if (e && !strcmp(e, "direct")) on = 0;
        else if (e && (!strcmp(e, "copy") || !strcmp(e, "hybrid"))) on = 1;
        // (Android, the memory pools in GPU memory: uncached, slow for the GPU's vertex fetch - the world's frame took twice the
        // GPU time read in place - and every buffer copy of the game waited for the GPU to be idle, 15 a frame)
        else on = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || g_vk.heap_export;
        LOG("[vk] vertex and index data: %s\n", on ? "copied to VRAM (ranges the GPU writes are read in place)" : "read in place from the game's memory");
    }
    return on;
}

static int new_chunk(Pool *pl, uint64_t size) {
    pl->c = realloc(pl->c, (pl->n + 1) * sizeof *pl->c);
    Chunk *c = &pl->c[pl->n];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = pl->gpu ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                              : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = pl->gpu ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO,
                                  .flags = (pl->gpu ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    c->ptr = info.pMappedData;
    c->size = size;
    if (pl->gpu) {
        VkBufferCreateInfo di = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo da = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &di, &da, &c->dev, &c->dmem, NULL));
    }
    {   // (where the GPU reads the copies from: VRAM through the BAR, or system memory over PCIe)
        VkMemoryPropertyFlags mp = 0;
        vmaGetAllocationMemoryProperties(g_vk.vma, pl->gpu ? c->dmem : c->mem, &mp);
        LOG("[vk] ring %d chunk %d: %llu MB, %s%s\n", pl->gpu ? 1 : 0, pl->n, (unsigned long long)(size >> 20),
            mp & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local" : "system memory",
            mp & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host-visible" : "");
    }
    return pl->n++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in transient_from

static VkSpan transient_from(Pool *pl, uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (pl->active >= 0) {
        Chunk *c = &pl->c[pl->active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){pl->gpu ? c->dev : c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < pl->n; i++)
        if (i != pl->active && pl->c[i].seq <= done && pl->c[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(pl, size > CHUNK_SIZE ? size : CHUNK_SIZE);
    pl->active = pick;
    Chunk *c = &pl->c[pick];
    c->epoch++;
    c->used = size;
    c->flushed = 0;
    c->seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){pl->gpu ? c->dev : c->buf, 0, c->ptr};
}

VkSpan vk_transient(uint64_t size, uint64_t align) { return transient_from(&pools[0], size, align); }
// (the vertex data kept from frame to frame, keep_copy below: its areas are spans too)
static Pool keep_pool;
static uint64_t keep_gen[2], keep_seq[2];
static bool keep_retired[2];
// The ring chunk a buffer is (pool * 4096 + index; 2 * 4096 + area: a kept-vertex area; -1: not the ring's - the game's heap
// imported, valid as long as it is), and its epoch now (a kept area's: its generation)
static int span_chunk(VkBuffer b, uint32_t *epoch) {
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < pools[p].n; i++) {
            Chunk *c = &pools[p].c[i];
            if (c->buf == b || (c->dev && c->dev == b)) { *epoch = c->epoch; return p * 4096 + i; }
        }
    for (int i = 0; i < keep_pool.n && i < 2; i++)
        if (keep_pool.c[i].dev == b) { *epoch = (uint32_t)keep_gen[i]; return 2 * 4096 + i; }
    *epoch = 0;
    return -1;
}
// A span kept from earlier in the frame used again: still the chunk's (epoch), and kept from being taken again until this
// submission completes (a kept-vertex area: same generation, not retired, and its last reader is this submission now)
static bool span_still(int ck, uint32_t epoch) {
    if (ck < 0) return true;
    if (ck / 4096 == 2) {
        int a = ck % 4096;
        if ((uint32_t)keep_gen[a] != epoch || keep_retired[a]) return false;
        keep_seq[a] = vk_pending_seq();
        return true;
    }
    Chunk *c = &pools[ck / 4096].c[ck % 4096];
    if (c->epoch != epoch) return false;
    c->seq = vk_pending_seq();
    return true;
}
// Vertex / index data of a draw: in VRAM (valid once the submission's first copy has run), or as vk_transient where that is off.
static VkSpan transient_gpu(uint64_t size, uint64_t align) { return transient_from(&pools[ring_gpu_enabled() ? 1 : 0], size, align); }

// The start of a submission: the staging bytes written since the last one go to VRAM (the upload buffer runs first, then a
// barrier, vk_submit_extra).
static void ring_flush_gpu(void) {
    Pool *pls[2] = {&pools[1], &keep_pool};  // (the ring, and the vertex data kept from frame to frame)
    for (int k = 0; k < 2; k++) {
        Pool *pl = pls[k];
        for (int i = 0; i < pl->n; i++) {
            Chunk *c = &pl->c[i];
            if (c->used <= c->flushed) continue;
            VkBufferCopy rg = {c->flushed, c->flushed, c->used - c->flushed};
            vkCmdCopyBuffer(vk_upload_cmd(), c->buf, c->dev, 1, &rg);
            c->flushed = c->used;
        }
    }
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_SLOTS };
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
static struct { uint64_t addr, size, seq; VkSpan span; bool gpu; } gcopy[GCOPY_TAB];
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; }
// ---- vertex / index copies off the render thread
// The draws' vertex and index data copied to the GPU ring (~12 MB a frame in Viridian City, ~1.1 ms: the render thread's
// largest single cost after the buffer-copy waits) is filled in by two helper threads: the render thread reserves the ring
// space and records the draw at once, a helper copies, and every copy is done before the submission that reads it
// (vk_submit_extra) and before the render thread itself writes guest memory (acopy_drain in exec_list). Until the
// submission the game leaves that memory alone: it rewrites vertex data only after the frame's fence, which follows the
// submission. Copies under 32 KB stay in place; big ones go in 256 KB pieces. The render thread takes pieces too while it
// waits. LARECOMP_VK_ASYNC_COPY=0 (or fewer than 4 logical processors): all in place, as before.
#define ACOPY_MAX 4096
#define ACOPY_MIN (32u << 10)
#define ACOPY_PIECE (256u << 10)
static struct { void *dst; const void *src; uint64_t n; } acq[ACOPY_MAX];
static _Atomic uint64_t acq_pushed, acq_taken, acq_done;  // (counters only grow: a job is acq[i % ACOPY_MAX])
static pthread_mutex_t acq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t acq_cond = PTHREAD_COND_INITIALIZER;
static int acq_sleeping;

static bool acopy_take_one(void) {
    uint64_t t = atomic_load(&acq_taken);
    for (;;) {
        if (t >= atomic_load(&acq_pushed)) return false;
        if (atomic_compare_exchange_weak(&acq_taken, &t, t + 1)) break;
    }
    memcpy(acq[t % ACOPY_MAX].dst, acq[t % ACOPY_MAX].src, acq[t % ACOPY_MAX].n);
    atomic_fetch_add(&acq_done, 1);
    return true;
}
static void *acopy_main(void *a) {
    (void)a;
    for (;;) {
        if (acopy_take_one()) continue;
        pthread_mutex_lock(&acq_lock);
        acq_sleeping++;
        while (atomic_load(&acq_taken) >= atomic_load(&acq_pushed)) pthread_cond_wait(&acq_cond, &acq_lock);
        acq_sleeping--;
        pthread_mutex_unlock(&acq_lock);
    }
    return NULL;
}
static bool acopy_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_VK_ASYNC_COPY");
        on = (!e || atoi(e) != 0) && SDL_GetNumLogicalCPUCores() >= 4;
        if (on) for (int i = 0; i < 2; i++) { pthread_t t; pthread_create(&t, NULL, acopy_main, NULL); }
        LOG("[vk] vertex and index copies: %s\n", on ? "by two helper threads" : "on the render thread");
    }
    return on;
}
// Every copy queued so far is done (the render thread helps)
static void acopy_drain(void) {
    uint64_t want = atomic_load(&acq_pushed);
    if (atomic_load(&acq_done) >= want) return;
    PROF_T0();
    while (acopy_take_one()) {}
#if defined(__x86_64__)
    while (atomic_load(&acq_done) < want) __builtin_ia32_pause();
#else
    while (atomic_load(&acq_done) < want) __asm__ volatile("yield");
#endif
    PROF_ADD(prof_gcpy_wait_ns);
}
static void acopy_push(void *dst, const void *src, uint64_t n) {
    for (uint64_t o = 0; o < n; o += ACOPY_PIECE) {
        if (atomic_load(&acq_pushed) - atomic_load(&acq_done) >= ACOPY_MAX) acopy_drain();  // (full)
        uint64_t i = atomic_load(&acq_pushed);
        acq[i % ACOPY_MAX].dst = (uint8_t *)dst + o;
        acq[i % ACOPY_MAX].src = (const uint8_t *)src + o;
        acq[i % ACOPY_MAX].n = n - o < ACOPY_PIECE ? n - o : ACOPY_PIECE;
        atomic_store(&acq_pushed, i + 1);
    }
    pthread_mutex_lock(&acq_lock);
    if (acq_sleeping) pthread_cond_broadcast(&acq_cond);
    pthread_mutex_unlock(&acq_lock);
}

static VkSpan transient_gpu_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = transient_gpu(size ? size : 16, align);
    PROF_T0();
    if (size >= ACOPY_MIN && acopy_on()) acopy_push(s.ptr, data, size);
    else if (size) memcpy(s.ptr, data, size);
    PROF_ADD(prof_gcpy_ns);
    if (prof) prof_gcpy_bytes += size;
    return s;
}
static VkSpan guest_copy_pool(uint64_t addr, uint64_t size, bool gpu) {  // (gpu: vertex / index data, in VRAM when that ring is on)
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    if (gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size && gcopy[slot].gpu == gpu) return gcopy[slot].span;
    VkSpan s = gpu ? transient_gpu_copy((const void *)(uintptr_t)addr, size, 256) : vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s; gcopy[slot].gpu = gpu;
    return s;
}
static VkSpan guest_copy(uint64_t addr, uint64_t size) { return guest_copy_pool(addr, size, false); }

// ---- vertex data kept from one frame to the next
// 85% of the vertex bytes copied to VRAM in Viridian City were the same as the frame before (LARECOMP_VCOPY_SAME): static meshes,
// copied again every frame. A range copied here stays in VRAM, and a later frame reads it from there when the game has not
// written its pages since: the OS tracks the heap's written pages (plat_written, any writer: the game's code, ours, the OS's),
// and each check notes them in a per-page epoch, so a copy is good while all its pages are older than it. Two 128 MB areas
// (staging + VRAM, as pool 1) filled in turn; one is reused once the GPU is done with it. The GPU's own writes (compute) are
// never copied (gpuw: read in place). Dropped when the heap changes size (pages given back and taken again are not "written")
// and at a reboot. LARECOMP_VK_KEEP_VERTICES=0: off (also where the platform can't track writes).
#define KEEP_AREA (128ull << 20)
#define KEEP_TAB 65536
static Pool keep_pool = {.active = -1, .gpu = true};
static uint64_t keep_gen[2], keep_seq[2];  // (an area's generation: bumped when it is emptied; the last submission reading it)
static bool keep_retired[2];               // (its copies no longer used, waiting for the GPU to be done with them)
static int keep_area = -1;
static struct { uint64_t addr, len, epoch, gen, off; int area, dirty, skip; } keep[KEEP_TAB];
static uint64_t *page_epoch;  // (the heap's pages: the epoch of the check that last found them written)
static uint64_t keep_tick = 1, keep_heap;
static uint64_t stat_keep_hits, stat_keep_bytes;  // (LARECOMP_VK_PROF: ranges / bytes read from a kept copy)
static uint64_t stat_keep_new, stat_keep_volatile, stat_keep_busy;  // (... new copies kept; ranges the game rewrites, to the ring;
                                                                     // to the ring because the other area was still in use)
static int keep_state = -1;
static bool keep_on(void) {
    if (keep_state < 0) {
        const char *e = getenv("LARECOMP_VK_KEEP_VERTICES");
        uint64_t probe;
        keep_state = (!e || atoi(e) != 0) && ring_gpu_enabled() && plat_written(HEAP_BASE, 4096, &probe, 1) >= 0;
        if (keep_state) page_epoch = calloc((GUEST_HI - HEAP_BASE) >> 12, sizeof *page_epoch);
        if (!page_epoch) keep_state = 0;
        LOG("[vk] vertex data kept in VRAM from frame to frame: %s\n",
            keep_state ? "on (the heap's written pages are tracked)" : "off (pages not tracked: by default only on NVIDIA, LARECOMP_WRITE_WATCH=1)");
    }
    return keep_state;
}
static void keep_drop_all(void) {
    for (int i = 0; i < 2; i++) keep_gen[i]++;
    memset(keep, 0, sizeof keep);
}
// The pages of [a, a+n) written since the last look: noted with a new epoch. Returns that epoch.
static uint64_t keep_look(uint64_t a, uint64_t n) {
    static uint64_t pages[4096];
    uint64_t tick = ++keep_tick;
    int k = plat_written(a, n, pages, 4096);
    if (k < 0 || k == 4096) {  // (not tracked, or maybe more than the list: all of it)
        for (uint64_t pg = (a - HEAP_BASE) >> 12; pg <= (a + n - 1 - HEAP_BASE) >> 12; pg++) page_epoch[pg] = tick;
    } else {
        for (int i = 0; i < k; i++) page_epoch[(pages[i] - HEAP_BASE) >> 12] = tick;
    }
    return tick;
}
static bool keep_clean(uint64_t a, uint64_t n, uint64_t epoch) {
    for (uint64_t pg = (a - HEAP_BASE) >> 12; pg <= (a + n - 1 - HEAP_BASE) >> 12; pg++)
        if (page_epoch[pg] >= epoch) return false;
    return true;
}
static void acopy_push(void *dst, const void *src, uint64_t n);
static bool acopy_on(void);
// A copy of [addr, addr+len) in VRAM for this submission: the kept one when still good, else a new one kept from now on.
// False: not for this range - outside the heap, too big, off, rewritten by the game frame after frame (found written at two
// checks in a row: to the ring, tried again every 64 checks), or no room without waiting for the GPU - copy it to the ring.
static bool keep_copy(uint64_t addr, uint64_t len, VkSpan *out) {
    if (!keep_on() || addr < HEAP_BASE || addr + len > GUEST_HI || len > KEEP_AREA / 8) return false;
    if (kernel_heap_size() != keep_heap) { keep_heap = kernel_heap_size(); keep_drop_all(); }
    uint32_t h = (uint32_t)((addr >> 4) * 0x9E3779B1u) & (KEEP_TAB - 1), at = h;
    int found = -1;
    for (int i = 0; i < 16; i++) {
        uint32_t k = (h + i) & (KEEP_TAB - 1);
        if (keep[k].addr == addr) { found = (int)k; break; }
        if (!keep[k].addr || keep[k].gen != keep_gen[keep[k].area]) at = k;
    }
    if (found >= 0) at = (uint32_t)found;
    uint64_t span = found >= 0 && keep[at].len > len ? keep[at].len : len;
    keep_look(addr, span);
    uint64_t pending = vk_pending_seq();
    if (found >= 0 && keep[at].gen == keep_gen[keep[at].area] && keep[at].len >= len && keep_clean(addr, keep[at].len, keep[at].epoch)) {
        Chunk *c = &keep_pool.c[keep[at].area];
        keep_seq[keep[at].area] = pending;
        keep[at].dirty = 0;
        *out = (VkSpan){c->dev, keep[at].off, c->ptr + keep[at].off};
        stat_keep_hits++, stat_keep_bytes += len;
        return true;
    }
    int dirty = found >= 0 && keep[at].gen == keep_gen[keep[at].area] ? keep[at].dirty + 1 : 0;  // (still there, but written since its copy)
    if (dirty >= 2 && ++keep[at].skip % 64) { keep[at].dirty = dirty; stat_keep_volatile++; return false; }
    // a new copy, at the end of the current area (or the other one, emptied once the GPU is done with it)
    if (keep_pool.n < 2) { new_chunk(&keep_pool, KEEP_AREA); new_chunk(&keep_pool, KEEP_AREA); keep_area = 0; }
    Chunk *c = &keep_pool.c[keep_area];
    uint64_t off = (c->used + 255) & ~255ull;
    if (off + len > c->size) {
        // Full: on to the other area, emptied, once the GPU is done with it. Its copies are dropped first (they'd be read every
        // frame, and it would never be free), so the ranges in it go to the ring for the frames the GPU still needs, never a wait.
        int o = keep_area ^ 1;
        if (!keep_retired[o]) { keep_gen[o]++; keep_retired[o] = true; }
        if (keep_seq[o] > vk_completed_seq()) { stat_keep_busy++; return false; }
        keep_retired[o] = false;
        keep_area = o;
        c = &keep_pool.c[keep_area];
        c->used = c->flushed = 0;
        off = 0;
    }
    c->used = off + len;
    keep_seq[keep_area] = pending;
    if (len >= ACOPY_MIN && acopy_on()) acopy_push(c->ptr + off, (const void *)(uintptr_t)addr, len);
    else memcpy(c->ptr + off, (const void *)(uintptr_t)addr, len);
    keep[at].addr = addr, keep[at].len = len, keep[at].epoch = keep_tick + 1, keep[at].gen = keep_gen[keep_area], keep[at].off = off,
    keep[at].area = keep_area, keep[at].dirty = dirty, keep[at].skip = 0;
    stat_keep_new++;
    *out = (VkSpan){c->dev, off, c->ptr + off};
    return true;
}

// Vertex data copied to VRAM, once per start address and submission: every draw of a mesh starts at the same address and wants a
// prefix of it (up to its largest index), so one copy serves them all. A per-vertex stream is copied whole (its bound range, up to
// 8 MB) the first time, so that later draws of other parts of the buffer don't copy it again (37 MB a frame became 16).
#define VCOPY_TAB 1024
static struct { uint64_t addr, len, seq; VkSpan span; } vcopy[VCOPY_TAB];
static VkSpan vertex_copy(uint64_t addr, uint64_t need, uint64_t bound, bool per_vertex) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40);
    uint32_t h = (uint32_t)((addr >> 4) * 0x9E3779B1u) & (VCOPY_TAB - 1), at = h;
    for (int i = 0; i < 4; i++) {
        uint32_t k = (h + i) & (VCOPY_TAB - 1);
        if (vcopy[k].addr == addr && vcopy[k].seq == seq) {
            if (vcopy[k].len >= need) return vcopy[k].span;
            at = k;  // (a longer prefix is needed now: a new copy replaces it)
            goto copy;
        }
        if (vcopy[k].seq != seq) at = k;  // (a free or stale slot)
    }
copy:;
    uint64_t len = per_vertex && bound <= (8ull << 20) && bound > need ? bound : need;
    VkSpan sp;
    if (!keep_copy(addr, len, &sp)) sp = transient_gpu_copy((const void *)(uintptr_t)addr, len, 256);
    if (prof && ENV("LARECOMP_VCOPY_SAME")) {  // (research: would a copy kept from an earlier frame still be good?)
        static struct { uint64_t addr, len, hash; } seen[8192];
        uint64_t hsh = len;
        const uint64_t *w = (const uint64_t *)(uintptr_t)addr;
        for (uint64_t i = 0; i < len / 8; i++) hsh = (hsh ^ w[i]) * 0x9E3779B97F4A7C15ull;
        uint32_t slot = (uint32_t)((addr >> 4) * 0x9E3779B1u) & 8191;
        if (seen[slot].addr == addr && seen[slot].len == len && seen[slot].hash == hsh) prof_vsame_bytes += len;
        prof_vall_bytes += len;
        seen[slot].addr = addr, seen[slot].len = len, seen[slot].hash = hsh;
    }
    vcopy[at].addr = addr; vcopy[at].len = len; vcopy[at].seq = seq; vcopy[at].span = sp;
    return sp;
}

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// LARECOMP_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
static Job *jobs_head, *jobs_tail;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;

static void *worker(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&jobs_lock);
        while (!jobs_head) pthread_cond_wait(&jobs_cond, &jobs_lock);
        Job *j = jobs_head;
        jobs_head = j->next;
        if (!jobs_head) jobs_tail = NULL;
        pthread_mutex_unlock(&jobs_lock);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}

static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_SYNC_SHADERS") != NULL;
    return v;
}

// Shader translation runs on the workers too, then compiles on the same worker (translate_program). It had one
// dedicated thread (the translator used a static string ring, now per thread): on a first visit, ~380 programs took
// ~240 ms one after another while the workers idled. Off the game thread either way (up to ~90 ms per frame there).
static void run_job(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) {
        started = true;
        // first entry into an area builds dozens of pipelines at once: use most of the CPU (draws wait for them)
        int n = SDL_GetNumLogicalCPUCores() - 4;
        if (n < 4) n = 4;
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); }
    }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&jobs_lock);
    if (jobs_tail) jobs_tail->next = j; else jobs_head = j;
    jobs_tail = j;
    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_lock);
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
    uint64_t obj;       // frame interpolation: the game object the current draws belong to (CMD_OBJECT), 0 = none
    uint32_t obj_draw;  // draws of that object so far in this run of it
} st;

// frame interpolation (prototype, LARECOMP_INTERP=1): the hooks below; the rest at the end of this file
uint64_t renderer_present_count(void);
static int ip_on = -1;
static bool ip_replaying;     // running a recorded frame again for the in-between image
static uint32_t ip_ord, ip_draw_ord;  // draws so far in this frame / replay, and this draw's place among them (vtx_memo)
static uint64_t ip_stats_tagged, ip_stats_untagged;  // draws with / without a game object (CMD_OBJECT)
static uint64_t ip_draw_sig;  // the current draw's identity (draw_)
static long ip_sig_dbg;  // (LARECOMP_INTERP_SIG_LOG)
static uint64_t ip_stats_held;  // words switched half-way instead of blended (ip_cbuf: held, then stepped)
static uint64_t ip_stats_content;  // draws paired with another draw than their identity names (ip_pair_draw)
static uint32_t ip_draw_occ;  // which occurrence of its identity this draw is, this frame / replay (ip_occ_next)
static uint32_t ip_occ_next(uint64_t sig);
static uint64_t ip_draw_shape;  // the draw without its vertex buffer's address or object (ip_pair_draw)
static bool ip_part_set;  // (the replay: this draw's partner in N-1, ip_pair_draw)
static int32_t ip_sib = -1;  // (... none: a sibling of N with one - its words shared follow it, ip_cbuf)
static void ip_draw_record(void);
static void ip_pair_draw(void);
static uint64_t ip_part_sig;
static uint32_t ip_part_occ;
static int ip_kept_why;  // (the replay: why this 3D draw kept frame N's constants - 1 no match, 2 object rejected, 3 jump)
static char ip_sig_txt[256];
static int ip_sig_txt_n;
static bool ip_draw_obj;      // ... and whether it belongs to a game object (CMD_OBJECT): only those are blended
static bool ip_draw_2d;       // ... a 2D draw (no depth test): interface, menus
static bool ip_vtx_draw;      // ... and its small vertex streams recorded / blended (ip_vtx_stream)
static bool ip_vtx_stream(int s, uint64_t addr, uint64_t need, uint32_t stride, bool inst, VkSpan *out);
static bool ip_pick_set;      // ... its previous occurrence picked (ip_cbuf, at its first cbuf)
static bool ip_side_post;     // post-processing an in-between image (vk_interp_keep): resolved against N's FSR 3.1
static TexBackend *ip_n_post(double ow, double oh);  // (N's processed image, for that: below)
static bool ip_enabled(void);
static float ip_lo_factor(void);
// The scene images rendered so far: one per present, plus the in-between images of the 60 fps mode (vk_interp_replay
// replays a frame for them). The scene's temporal work (camera and motion vectors, SSAO, GI, TAA and its jitter, FSR 3's
// inputs) counts in these: an in-between image is a frame of its own, with its own jitter, history and effects - counted
// by presents it shared frame N's number, ran none of them, and went on screen without SSAO, GI or anti-aliasing every
// other image (flicker in cutscenes and menus at 60 fps). In the order they are rendered: N, then the image between N-1
// and N; each one's motion vectors are taken from the image rendered before it.
static uint64_t ip_replays;  // in-between images rendered
static float ip_t;           // (where the in-between image is, between N-1 and N: below)
static int ip_k = 2;         // images per present with interpolation: N and K - 1 in-between ones (vk_interp_mult)
static uint64_t fx_frame(void) { return renderer_present_count() + ip_replays + (ip_replaying ? 1 : 0); }
bool vk_interp_enabled(void) { return ip_enabled(); }  // (cmdbuf.c: hooks the game's object draw functions)
static bool ip_enabled(void) {
    // Frame generation (F12 menu, frame_gen = 2 / 3 / 4): everywhere, k images per frame (ip_images). Without it, the
    // 60 fps mode (runtime/hle/fps60.c): the world at 60 natively; menus and the Pokédex at the game's 30 with the
    // in-between image; battles and captures at 30 without it (tried, left out: docs/HISTORICO.md 46).
    // LARECOMP_INTERP=1 (research): always on.
    if (ip_on == -1) ip_on = getenv("LARECOMP_INTERP") ? atoi(getenv("LARECOMP_INTERP")) : 0;
    extern bool fps60_menu(void);
    return g_settings.frame_gen >= FG_AUTO || ip_on > 0 || fps60_menu();
}
static const void *ip_cbuf(int stage, int k, uint64_t addr, uint32_t size);
static bool ssao_proj_at(const float *m, float out[4]);  // (a perspective projection: below)
// Wide screens (runtime/hle/widescreen.c): a copy of a vertex constant block whose 16:9 perspective projections (in its
// first 512 bytes) take the window's aspect - their P00 scaled by 16/9 / aspect; NULL when off or none there. The
// camera the effects read (occlusion, indirect light, motion vectors), as the draws are widened (ShaderKey.widen).
static const uint8_t *wide_block(const uint8_t *src, uint32_t size) {
    extern float widescreen_aspect(void);
    float wa = widescreen_aspect();
    if (wa <= 1.0f || !src || size > 0x1000) return NULL;
    static uint8_t buf[0x1000];
    bool any = false;
    for (uint32_t o = 0; o + 64 <= size && o < 512; o += 16) {
        const float *m = (const float *)(src + o);
        float pj[4];
        if (!ssao_proj_at(m, pj) || fabsf(pj[1] / pj[0] - 16.0f / 9.0f) > 0.01f) continue;
        if (!any) memcpy(buf, src, size), any = true;
        ((float *)(buf + o))[0] = m[0] * (16.0f / 9.0f) / wa;
    }
    return any ? buf : NULL;
}
static void ip_note_list(uint64_t handle);
static bool ip_skip_op(uint32_t op) {  // what the replay must not do again: it writes guest memory or signals the game
    switch (op) {
    case CMD_DISPATCH: case CMD_DISPATCH_INDIRECT: case CMD_COPY_BUFFER_TO_BUFFER: case CMD_COPY_TEXTURE_TO_BUFFER:
    case CMD_UPDATE_UNIFORM_BUFFER: case CMD_FENCE_SYNC: case CMD_WAIT_SYNC: case CMD_SIGNAL_EVENT: case CMD_WAIT_EVENT:
    case CMD_REPORT_COUNTER: case CMD_CLEAR_BUFFER:
    case CMD_COPY_BUFFER_TO_TEXTURE:  // (uploads: done when the frame ran; again, they overran scaled images)
        return true;
    default:
        return false;
    }
}

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (LARECOMP_VK_PROF) called once per presented frame: frames over 50 ms are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs;
    uint64_t now = SDL_GetTicksNS();
    if (last_t && now - last_t > 50000000) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        LOG("[hitch] frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f) + present %.1f + other %.1f; %llu draws, "
            "%llu submits, compiles %d pending\n", (unsigned long long)frame, dt / 1e6, ex / 1e6, wt / 1e6, present_ns / 1e6,
            ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws), (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles));
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits;
}

static void save_pipeline_cache(void *a) {
    (void)a;
    uint64_t t0 = SDL_GetTicksNS();
    vk_pipeline_cache_save();
    LOG("[vk] pipeline cache saved (%.0f ms)\n", (SDL_GetTicksNS() - t0) / 1e6);
}

const char *renderer_stats(void) {
    // The pipeline cache was written only at a clean exit: a crash or a killed process lost every pipeline built in
    // the session, and the next one popped in again. Save it on a worker once a burst of new pipelines settles
    // (no new ones for ~6 s: entering an area builds them for a few seconds).
    static int quiet, last_new;
    int np = atomic_load(&new_pipes);
    if (np && np == last_new) {
        if (++quiet >= 3) { atomic_fetch_sub(&new_pipes, np); np = 0; quiet = 0; run_job(save_pipeline_cache, NULL); }
    } else {
        quiet = 0;
    }
    last_new = np;
    static int dumps;
    if (dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d (translate %.0fms, spirv %.0fms, pipelines %.0fms, draws waited %.0fms), fbcopy %llu, upload %.1fMB",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), atomic_exchange(&stat_translate_ns, 0) / 1e6, atomic_exchange(&stat_spirv_ns, 0) / 1e6, atomic_exchange(&stat_pipe_ns, 0) / 1e6,
             stat_shader_wait_ns / 1e6,
             (unsigned long long)stat_feedback_copies, (double)stat_upload_bytes / 1048576.0);
    {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | vertex ranges %llu copied, %llu in place", (unsigned long long)stat_vtx_copied, (unsigned long long)stat_vtx_direct);
        stat_vtx_copied = stat_vtx_direct = 0;
    }
    {   // (the translations the GLSL disk cache gave, and those it stored: maxwell/translate.c)
        extern _Atomic unsigned maxwell_glc_hits, maxwell_glc_stores;
        unsigned gh = atomic_exchange(&maxwell_glc_hits, 0), gs = atomic_exchange(&maxwell_glc_stores, 0);
        size_t n = strlen(stats_text);
        if (gh || gs) snprintf(stats_text + n, sizeof stats_text - n, " | glsl cache %u read, %u stored", gh, gs);
    }
    stat_feedback_copies = stat_upload_bytes = stat_shader_wait_ns = 0;
    if (g_exp_count[0] | g_exp_count[1] | g_exp_count[2] | g_exp_count[3]) {
        LOG("[exp] skin identity %llu, ubo direct %llu, ubo copied %llu, one-bone shaders %llu\n",
            (unsigned long long)g_exp_count[0], (unsigned long long)g_exp_count[1], (unsigned long long)g_exp_count[2],
            (unsigned long long)g_exp_count[3]);
        g_exp_count[0] = g_exp_count[1] = g_exp_count[2] = 0;  // ([3]: a running total)
    }
    if (g_skin_watch) {
        const uint32_t *w = (const uint32_t *)(uintptr_t)g_skin_watch;
        LOG("[skin] cb3 %llx now: row8 %08x %08x row9 %08x %08x\n", (unsigned long long)g_skin_watch, w[32], w[33], w[36], w[37]);
    }
    if (stat_skipped) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " [skip: noprog %llu compiling %llu failed %llu noobj %llu mode %llu pipe-building %llu pipe-failed %llu]",
                 (unsigned long long)stat_skip_why[0], (unsigned long long)stat_skip_why[1],
                 (unsigned long long)stat_skip_why[2], (unsigned long long)stat_skip_why[3],
                 (unsigned long long)stat_skip_why[4], (unsigned long long)stat_skip_why[5],
                 (unsigned long long)stat_skip_why[6]);
        memset(stat_skip_why, 0, sizeof stat_skip_why);
    }
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms ipcbuf %.0f",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6, prof_ipc / 1e6);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu", (unsigned long long)prof_views_created);
        prof_views_created = 0;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu ring copies %.0f ms %.0f MB (waited %.0f ms), index scans %.0f ms",
                 prof_gcpy_ns / 1e6, prof_gcpy_bytes / 1048576.0, prof_gcpy_wait_ns / 1e6, prof_scan_ns / 1e6);
        if (keep_state > 0) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, ", kept vertex copies read %llu (%.0f MB), new %llu, rewritten %llu, no room %llu",
                     (unsigned long long)stat_keep_hits, stat_keep_bytes / 1048576.0, (unsigned long long)stat_keep_new,
                     (unsigned long long)stat_keep_volatile, (unsigned long long)stat_keep_busy);
            stat_keep_hits = stat_keep_bytes = stat_keep_new = stat_keep_volatile = stat_keep_busy = 0;
        }
        if (prof_vall_bytes) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, ", vertex bytes as last copied %.1f%%", 100.0 * prof_vsame_bytes / prof_vall_bytes);
        }
        prof_gcpy_ns = prof_gcpy_bytes = prof_scan_ns = prof_vsame_bytes = prof_vall_bytes = prof_gcpy_wait_ns = 0;
        // the command types that took 10 ms or more in the window (nvn.h CMD_*): type:ms/count
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | ops:");
        for (int i = 0; i < CMD_COUNT; i++)
            if (prof_op_ns[i] >= 10000000) {
                n = strlen(stats_text);
                snprintf(stats_text + n, sizeof stats_text - n, " %d:%.0f/%llu", i, prof_op_ns[i] / 1e6, (unsigned long long)prof_op_n[i]);
            }
        memset(prof_op_ns, 0, sizeof prof_op_ns);
        memset(prof_op_n, 0, sizeof prof_op_n);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | waits:");
        n = strlen(stats_text);
        vk_wait_sites_log(stats_text + n, sizeof stats_text - n);
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = prof_ipc = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void vk_exec_init(void) {
    prof = getenv("LARECOMP_VK_PROF") != NULL;
    prof_up_ns = 0;
    {   // (the submissions' timestamps: LARECOMP_VK_PROF, the performance overlay)
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 2};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    st_defaults();
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static int g_wide_ui;  // wide screens: this draw's interface placement (ShaderKey.widen's wide_.y)
static float tex_sc[2][64][2];
static uint32_t tex_wh[2][64][2];

// ================================================================== SSAO hook (F12 menu)
// The game's 3D scene is one pass into a 16:9 float target with depth (Let's Go: R11G11B10F + D32S8, 1080p scaled):
// opaque draws first, then blended ones (foliage cards, effects) - some of them among the opaque ones - then bloom and
// the composite read it. The occlusion goes onto that target once per frame, when the pass ends (ssao_draw_hook). The
// projection comes from the draws' constant buffers (Let's Go: the scene block, vertex cbuf 5).
static struct {
    bool qual, busy;           // the pass running now is the scene's; ssao_run is recording
    NvnTexture *tex;           // the scene target of this frame's count of opaque draws
    TexBackend *color, *depth;
    VkImageView cview;
    VkFormat cfmt;
    uint32_t cw, ch, opaque;
    uint64_t frame, done_frame, proj_frame, scan_frame;
    float proj[4];             // P00, P11, A, B (GL style: z_ndc = (A z + B) / -z)
    int pbind;                 // where the projection was found: vertex cbuf, byte offset
    uint32_t poff;
    // the camera (motion vectors): view-projection of this frame and the one before (row-major, GL style)
    double vp[16], vp_prev[16];
    uint64_t vp_frame;
    uint64_t vp_real;          // the real frame (renderer_present_count) whose camera vp is: real frames chain by these
    // the whole scene block (vertex cbuf pbind) of this frame and the previous one: an object without its own previous
    // constants takes this frame's except this block - it then moves with the camera (mvo_prev_info)
    uint8_t scene[2][4096];
    uint32_t scene_size[2];
    int scene_cur;
    bool prev_ok;              // vp_prev is the frame just before vp's
    // per-object motion vectors: the velocity target added to the scene pass (colour attachment vel_rt)
    bool vel;
    uint32_t vel_rt;
    uint64_t vel_frame;
    TexBackend *vel_img;
    NvnTexture *dtex;          // the scene's depth texture (TAA: the draws on it are jittered)
    uint64_t taa_frame;        // TAA resolved this frame
} ao = {.pbind = -1};
static void taa_run(void);
static bool taa_on(void);
static bool jitter_on(void);
static const float *taa_jitter(void);
static bool mv_nodump;  // (mv_run for TAA: no LARECOMP_MV_DUMP files)
// TAA: the history's weight (0.9: about ten frames; less where it is clipped or uncovered)
#define TAA_HISTORY 0.9f
static TexBackend *post_image_fmt(int i, uint32_t w, uint32_t h, VkFormat fmt);
static VkImageView post_view(TexBackend *d);
// Per-object motion vectors (level 2) wanted: the camera's are, and the advanced menu's option is on
static bool mv_wanted(void);
static bool mvo_wanted(void) { return g_settings.mv_objects && mv_wanted(); }
// FSR 3's interface mask: a copy (post image 25) of the image the
// interface is drawn into, made before the frame's first interface draw - one reading none of the scene's (scaled)
// render targets; the present compares the two. Menus draw it into the presented image; while playing, the game draws
// it at the render size into the scaled image its last pass then takes (colour grading) into the presented one
// (ui_scene_rt: that pass's input, seen the frame before).
static uint64_t ui_snap_frame;
static TexBackend *ui_snap, *ui_snap_of;     // the copy, and the image it was made of
static TexBackend *ui_scene_rt;              // the last pass's scaled input
static TexBackend *ui_final_src;             // ... seen this frame (ui_final_frame)
static uint64_t ui_final_frame;
static VkImageView raw_view(TexBackend *b);
static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_);
static bool fsr3_on(void);
static bool gsr2_on(void);
// Mip bias of the game's own textures while TAA or FSR 3.1 resolves the image (post_process_ex, texture_info): the
// temporal accumulation brings back the detail of a finer mip level without its shimmer - AMD's guidance for FSR,
// log2(render / display width) - 1 (native: -1; Quality, 1.5x: -1.58); TAA at native -0.5 (its history is shorter).
// In steps of 0.25 (a sampler each), -2 .. 0. LARECOMP_MIP_BIAS=<offset> (research) replaces the -1 / -0.5; 0: none.
static float g_mip_bias;
static void ssao_run(void);
static void sun_probe(void);  // (contact shadows: the shadow pass's light)
// The debug views (F12: the occlusion alone, the motion vectors) replace the game image at the present, after the
// game's own passes (fog, bloom, tone mapping): written into the scene they came out white or washed out.
static TexBackend *debug_view;  // RGBA8 at the depth's size, written by ssao_run
static uint64_t debug_view_frame;
// GI: the luminance a sample of bounced light is clamped to (the scene's HDR units: no fireflies from the sun-lit
// highlights); the debug view's exposure
#define GI_LUMA_CLAMP 4.0f
#define GI_DEBUG_EXPOSURE 16.0f  // (the scene is dim before the game's exposure: x16 shows the bounce)
// Camera motion vectors wanted this frame (their debug view, LARECOMP_MV_DUMP; temporal effects later)
static bool mv_wanted(void) {
    static int dump = -1;
    if (dump < 0) dump = getenv("LARECOMP_MV_DUMP") != NULL;
    return g_settings.mv_debug || dump || (g_settings.ssao && g_settings.ssao_temporal) || (g_settings.gi && g_settings.gi_temporal) ||
           AA_ON(g_settings.antialias) == AA_TAA || UPSCALE_TEMPORAL(g_settings.upscaler) ||
           (g_settings.frame_gen && g_settings.fg_method == FG_FLOW);  // (the optical flow starts from them: flow_prepare)
}

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static int debug_pass_no;

static void vk_end_pass_at(int line);
static void debug_save_depth(TexBackend *b, const char *path);
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    vkCmdEndRendering(vk_cmd());
    in_pass = false;
    {   // LARECOMP_INTERP_RT_DUMP=<present>:<width> (research): each pass into a colour target of that width (the game's
        // size) at that present, the real frame's and its in-between images' -> build/frames/rt_<present>_<image>_<n>.png
        static long at = -2, wd;
        if (at == -2) {
            const char *e = getenv("LARECOMP_INTERP_RT_DUMP");
            at = e ? atol(e) : -1, wd = e && strchr(e, ':') ? atol(strchr(e, ':') + 1) : 0;
        }
        static int nth;
        if (at >= 0 && (long)renderer_present_count() == at && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend &&
            (long)st.rt.color[0]->width == wd) {
            char path[512];
            snprintf(path, sizeof path, "build/frames/rt_%ld_%llu_%d.png", at, (unsigned long long)fx_frame(), nth);
            renderer_save_texture_tex(st.rt.color[0]->backend, path);
            if (st.rt.depth && st.rt.depth->backend) {  // (its depth too, as a PGM stretched over its range: build/frames or _DIR)
                const char *dir = getenv("LARECOMP_INTERP_RT_DUMP_DIR");
                snprintf(path, sizeof path, "%s/rt_%ld_%llu_%d_depth.pgm", dir ? dir : "build/frames", at, (unsigned long long)fx_frame(), nth);
                debug_save_depth(st.rt.depth->backend, path);
                LOG("[interp-rt] %s: depth format %d\n", path, (int)((TexBackend *)st.rt.depth->backend)->format);
            }
            nth++;
        }
    }
    if (g_debug_frame && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("LARECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        renderer_save_texture_tex(st.rt.color[0]->backend, path);
    }
    // (the scene pass ends - a copy, a dispatch, another target - with its opaque draws done: the occlusion now)
    if (ao.qual && !ao.busy && ao.opaque >= 16 && ao.done_frame != fx_frame()) ssao_run();
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }

// ---- Deferred clears. The game clears a target in a pass of its own (a depth-only target, then the scene's colour
// and that depth: 6 - 8 such passes a frame in the world), each its own render pass: on a tiler the cleared image is
// written to memory, and the next pass loads it back. A whole-target clear with no pass open is kept here instead and
// becomes the load op (CLEAR) of the next pass that renders to the same target at the same size - nothing written or
// read for it. Anything else first (another pass without that target, a copy, a dispatch, our passes, a submission:
// all reach vk_cmd) records the pending clears as they were, in order. Measured on the phone (Adreno 830, world):
// ~5 % of the frame's GPU time in those passes. LARECOMP_DEFER_CLEARS=0: each its own pass (comparison).
typedef struct {
    NvnTexture *t;
    TexBackend *b;
    NvnTextureView view;
    bool has_view, color;
    VkImageAspectFlags aspects;  // (depth / stencil: which of them)
    VkClearValue v;
    uint32_t w, h;  // (the level's extent: the pass taking it must render all of it)
} PClear;
static PClear pclear[9];

static PClear *pclear_find(NvnTexture *t, const NvnTextureView *view, bool has_view, bool color) {
    for (int i = 0; i < pclear_n; i++) {
        PClear *p = &pclear[i];
        if (p->t == t && p->color == color && p->has_view == has_view && (!has_view || !memcmp(&p->view, view, sizeof *view))) return p;
    }
    return NULL;
}
static void pclear_drop(PClear *p) { *p = pclear[--pclear_n]; }

// (textures.c: a target's image destroyed - its pending clear goes with it)
void vk_pclear_forget(TexBackend *b) {
    for (int i = 0; i < pclear_n;)
        if (pclear[i].b == b) pclear_drop(&pclear[i]); else i++;
}

static void pclear_flush(void) {
    if (!pclear_n) return;
    pclear_hold = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    for (int i = 0; i < pclear_n; i++) {
        PClear *p = &pclear[i];
        VkImageView v = vk_view(p->t, &p->view, p->has_view, true, TEXTYPE_2D);
        if (!v) continue;
        VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                       .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = p->v};
        VkRenderingAttachmentInfo da = a, sa = a;
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {p->w, p->h}}, .layerCount = 1};
        if (p->color) ri.colorAttachmentCount = 1, ri.pColorAttachments = &a;
        else {
            if (!(p->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            if (!(p->aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) sa.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            if (p->b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &da;
            if (p->b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &sa;
        }
        vkCmdBeginRendering(cb, &ri);
        vkCmdEndRendering(cb);
    }
    vk_full_barrier(cb);
    pclear_n = 0;
    pclear_hold = false;
}

// The current target's attachment (colour index, or -1: depth) and its level's extent; false when it has none
static bool rt_attachment(int index, NvnTexture **t, uint32_t *w, uint32_t *h, uint32_t *gw, uint32_t *gh) {
    NvnTexture *x = index >= 0 ? st.rt.color[index] : st.rt.depth;
    if (!x || !x->backend) return false;
    bool hv = index >= 0 ? st.rt.has_cview[index] : st.rt.has_dview;
    const NvnTextureView *v = index >= 0 ? &st.rt.cview[index] : &st.rt.dview;
    uint32_t lv = hv && (v->setmask & 1) ? v->base_level : 0;
    TexBackend *b = x->backend;
    *t = x, *w = level_extent(b->width, lv), *h = level_extent(b->height, lv);
    *gw = level_extent(x->width, lv), *gh = level_extent(x->height, lv);
    return true;
}

// A clear (clear(), its arguments) with no pass open kept as pending: true when kept. Only what the pass would have
// done exactly: a whole clear (all channels, the scissor over the whole target), every attachment of the target the
// same size; the colour targets' first-use clears of begin_pass (renderer_texture_*_fresh: the alpha) kept with it.
static bool clear_defer(int index, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval, uint32_t smask) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_DEFER_CLEARS") ? atoi(getenv("LARECOMP_DEFER_CLEARS")) : 1;
    if (!on || (in_pass && !st.rt_dirty)) return false;
    if (!((index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF))) return false;
    if (index >= 0 ? index >= st.rt.ncolor || !st.rt.color[index] : !st.rt.depth || !(depth || stencil)) return false;
    if (st.rt.depth && !renderer_format_is_depth(st.rt.depth->format)) return false;
    uint32_t w = 0, h = 0, gw = 0, gh = 0;
    for (int i = -1; i < st.rt.ncolor; i++) {
        NvnTexture *t;
        uint32_t aw, ah, agw, agh;
        if (!rt_attachment(i, &t, &aw, &ah, &agw, &agh)) continue;
        if (!vk_view(t, i >= 0 ? &st.rt.cview[i] : &st.rt.dview, i >= 0 ? st.rt.has_cview[i] : st.rt.has_dview, true, TEXTYPE_2D))
            return false;
        if (w && (aw != w || ah != h)) return false;
        w = aw, h = ah, gw = agw, gh = agh;
    }
    if (!w) return false;
    int64_t x = st.scissor[0][0], y = st.scissor[0][1];
    if (x > 0 || y > 0 || x + st.scissor[0][2] < (int64_t)gw || y + st.scissor[0][3] < (int64_t)gh) return false;
    if (pclear_n + st.rt.ncolor + 1 > (int)(sizeof pclear / sizeof *pclear)) pclear_flush();
    vk_end_pass();  // (a pass of another target still open: ended, as begin_pass would)
    for (int i = -1; i < st.rt.ncolor; i++) {
        NvnTexture *t;
        uint32_t aw, ah, agw, agh;
        if (!rt_attachment(i, &t, &aw, &ah, &agw, &agh)) continue;
        renderer_mark_gpu_owned(t);
        const NvnTextureView *v = i >= 0 ? &st.rt.cview[i] : &st.rt.dview;
        bool hv = i >= 0 ? st.rt.has_cview[i] : st.rt.has_dview;
        VkClearValue cv = {0};
        VkImageAspectFlags as = 0;
        if (i >= 0) {  // (begin_pass's first-use clear: its alpha; the game's own clear over it)
            bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);
            bool first = renderer_texture_first_write(t) || fresh;
            if (i == index) memcpy(cv.color.float32, color, 16);
            else if (first) cv.color.float32[3] = fresh ? 0.0f : 1.0f;
            else continue;
        } else {
            TexBackend *b = t->backend;
            as = (depth && (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                 (stencil && (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            if (index >= 0 || !as) continue;
            cv.depthStencil = (VkClearDepthStencilValue){dval, sval};
        }
        PClear *p = pclear_find(t, v, hv, i >= 0);
        if (!p) {
            p = &pclear[pclear_n++];
            *p = (PClear){.t = t, .b = t->backend, .view = *v, .has_view = hv, .color = i >= 0, .w = aw, .h = ah};
        }
        if (i >= 0) p->v = cv;
        else {  // (a depth clear and a stencil clear one after the other: both)
            if (as & VK_IMAGE_ASPECT_DEPTH_BIT) p->v.depthStencil.depth = dval;
            if (as & VK_IMAGE_ASPECT_STENCIL_BIT) p->v.depthStencil.stencil = sval;
            p->aspects |= as;
        }
    }
    return true;
}

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    // TAA: the scene is complete when the game moves on to another colour target (its bloom): resolved there, once
    if ((jitter_on() || g_settings.upscaler == UPSCALE_FSR3 || UPSCALE_IS_DITTOSR(g_settings.upscaler) || UPSCALE_IS_FSR4(g_settings.upscaler)) && ao.tex && ao.frame == fx_frame() && ao.taa_frame != ao.frame && ao.opaque >= 16 &&
        st.rt.ncolor && st.rt.color[0] && st.rt.color[0] != ao.tex)
        taa_run();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each pass its own submission)
    st.rt_dirty = false;
    pclear_hold = true;  // (the pending clears: this pass's load ops, or recorded before it - below)
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        gp_mark(cb, "pass: %d color %ux%u fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    // Bound color slots without a texture / view went in as VK_NULL_HANDLE attachments (legal), but Intel's Windows
    // driver for UHD 630 crashes in vkCmdBeginRendering on them (LAS Recomp f4cb039, its issues #8 / #46). Empty slots
    // at the end are left out (pipelines and clears follow pass_ncolor); one in the middle is logged once.
    while (pass_ncolor && !ca[pass_ncolor - 1].imageView) pass_ncolor--;
    static bool logged_hole;
    for (uint32_t i = 0; i < pass_ncolor && !logged_hole; i++)
        if (!ca[i].imageView) {
            logged_hole = true;
            LOG("[vk] pass with an empty color slot %u of %u (texture %s)\n", i, pass_ncolor, st.rt.color[i] ? "without a view" : "none");
        }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    pass_w = minw; pass_h = minh;
    if (pclear_n) {  // (clear_defer: this pass's targets' pending clears are its load ops; the others recorded first)
        for (int i = -1; i < (int)pass_ncolor; i++) {
            if (i >= 0 ? !ca[i].imageView : !has_d && !has_s) continue;
            NvnTexture *t = i >= 0 ? st.rt.color[i] : st.rt.depth;
            PClear *p = pclear_find(t, i >= 0 ? &st.rt.cview[i] : &st.rt.dview, i >= 0 ? st.rt.has_cview[i] : st.rt.has_dview, i >= 0);
            if (!p || p->w != minw || p->h != minh) continue;
            if (i >= 0) ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, ca[i].clearValue = p->v;
            else {
                if (has_d && (p->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, da.clearValue = p->v;
                if (has_s && (p->aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, sa.clearValue = p->v;
                if ((p->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) && !has_d) continue;  // (an aspect this pass lacks: recorded)
                if ((p->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) && !has_s) continue;
            }
            pclear_drop(p);
        }
        pclear_hold = false;
        pclear_flush();
    }
    pclear_hold = false;
    ao.qual = false;
    if ((g_settings.ssao || g_settings.gi || g_settings.contact_on || mv_wanted()) && pass_ncolor && ca[0].imageView && has_d && pass_samples == 1 && st.rt.color[0]) {  // (TAA: mv_wanted)
        TexBackend *c0 = st.rt.color[0]->backend;
        VkFormat f = pass_color[0];
        if (c0 && c0->scalable && (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT ||
                                   f == VK_FORMAT_R32G32B32A32_SFLOAT)) {
            uint64_t fr = fx_frame();
            if (ao.tex != st.rt.color[0] || ao.frame != fr) { ao.tex = st.rt.color[0]; ao.frame = fr; ao.opaque = 0; }
            ao.qual = true;
            ao.color = c0;
            ao.depth = st.rt.depth->backend;
            ao.dtex = st.rt.depth;
            ao.cview = ca[0].imageView;
            ao.cfmt = f;
            ao.cw = minw; ao.ch = minh;
        }
    }
    // the velocity target (RGBA16F at the pass's size: xy velocity, z previous / current distance, w = 1 written):
    // cleared at the frame's first scene pass, kept by the next ones; written by the opaque draws only (draw_)
    ao.vel = false;
    if (ao.qual && mvo_wanted() && pass_ncolor < 8) {
        TexBackend *v = post_image_fmt(20, minw, minh, VK_FORMAT_R16G16B16A16_SFLOAT);
        uint64_t fr = fx_frame();
        ca[pass_ncolor] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(v),
                                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                      .loadOp = ao.vel_frame != fr ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        pass_color[pass_ncolor] = VK_FORMAT_R16G16B16A16_SFLOAT;
        ao.vel_rt = pass_ncolor++;
        ao.vel = true, ao.vel_frame = fr, ao.vel_img = v;
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    in_pass = true;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    // (wide screens, the interface placed by its vertex shader: a scissor over the whole screen is the whole image's -
    // what reaches the window's sides is drawn there; a smaller one, the box's)
    if (g_wide_ui && st.rt.ncolor && st.rt.color[0] && x <= 0 && y <= 0 && (int64_t)x + sw >= (int64_t)st.rt.color[0]->width &&
        (int64_t)y + sh >= (int64_t)st.rt.color[0]->height)
        return (VkRect2D){{0, 0}, {(uint32_t)(w < (int)pass_w ? w : (int)pass_w), (uint32_t)(h < (int)pass_h ? h : (int)pass_h)}};
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    vkCmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    vkCmdSetViewport(vk_cmd(), 0, 1, &vp);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
#define NDYN (sizeof DYN / sizeof DYN[0])

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    if (clear_defer(index, color, mask, depth, dval, stencil, sval, smask)) return;
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    dyn_defaults(cb);
    if (depth) { vkCmdSetDepthTestEnable(cb, VK_TRUE); vkCmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    bool vs_uses_vid;
    _Atomic int state;  // 0 building, 1 ready, 2 failed
} Prog;

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("LARECOMP_SAVE_MSL")) return;
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

static void compile_program(void *arg) {
    Prog *p = arg;
    size_t nv = 0, nf = 0;
    uint64_t t0 = SDL_GetTicksNS();
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv);
    uint32_t *sf = sv ? vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &nf) : NULL;
    if (sv && getenv("LARECOMP_RENDERDOC_AT")) {  // which guest shader each SPIR-V is: tools/rdc_skin.py (FNV-1a 64)
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < nv * 4; i++) { h ^= ((const uint8_t *)sv)[i]; h *= 0x100000001B3ull; }
        pthread_mutex_lock(&lock);
        FILE *f = fopen("build/rdc-shaders.txt", "a");
        if (f) { fprintf(f, "%016llx vs %llx\n", (unsigned long long)h, (unsigned long long)p->code[0]); fclose(f); }
        pthread_mutex_unlock(&lock);
    }
    if (!sv || !sf) { free(sv); free(sf); atomic_store(&p->state, 2); return; }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    free(sv);
    free(sf);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    atomic_fetch_add(&stat_spirv_ns, SDL_GetTicksNS() - t0);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&p->state, p->vs && p->fs ? 1 : 2);
}

// ---- the game's blur and depth of field, filtered better (blur_quality, on by default)
// The background blur of the menus and the depth of field of the cutscenes are one chain of the game's: the HDR scene
// taken down (1/2, 1/3, 1/6 of its size, one bilinear tap each: shimmers as anything in it moves), blurred there
// separably (5 and 9 taps), and the levels mixed back into the full-size scene by its blur size (bilinear: blocky,
// diamond-shaped steps at high resolutions). Those shaders, known by the hash of their code (the same one draw_ logs
// with LARECOMP_LOG_FSHASH), have their texture reads replaced in their GLSL:
//   down: the 13-tap filter of "Next generation post processing in Call of Duty" (Jimenez) over the pixel's footprint;
//   soft: each blur tap four rotated-grid taps half a texel out (the gaps between taps filled, the steps smoothed);
//   up:   a smaller texture than the output read with a cubic B-spline (four bilinear taps); others as they were.
// The bloom shares the downsample and one blur shader: it gets the same.
enum { FX_DOWN = 1, FX_SOFT, FX_UP };
static const struct { uint64_t hash; int mode; } fx_shaders[] = {
    {0xc5f8d9e5305c23c5ull, FX_DOWN},  // 1 tap x a scale: every level of the chain (and the bloom's)
    {0xbefcb4496f4421b8ull, FX_SOFT},  // 5 taps (offsets from the constants)
    {0xbb3a0401b87bf5ebull, FX_SOFT},  // 9 taps (the same)
    {0x1051fb0a1c062d98ull, FX_SOFT},  // 5 taps (offsets in the varyings; the bloom's too)
    {0x3779b28fd4ab87f7ull, FX_SOFT},  // 9 taps (the same)
    {0x616b00767ad5ed0bull, FX_UP},    // a level added onto the next larger one
    {0x8f037ec93d4ec782ull, FX_UP},    // the composite: six blurred levels and the scene
};
static const char fx_glsl[] =
    "vec2 fx_px(vec2 uv, vec2 ts) { return max(abs(dFdx(uv)), abs(dFdy(uv))) * ts; }  // source texels per output pixel\n"
    "vec4 fx_down(sampler2D t, vec2 uv) {\n"
    "  vec2 ts = vec2(textureSize(t, 0)), k = max(fx_px(uv, ts) * 0.5, vec2(0.5)) / ts;\n"
    "  vec4 a = texture(t, uv + k * vec2(-2, -2)), b = texture(t, uv + k * vec2(0, -2)), c = texture(t, uv + k * vec2(2, -2));\n"
    "  vec4 d = texture(t, uv + k * vec2(-1, -1)), e = texture(t, uv + k * vec2(1, -1));\n"
    "  vec4 f = texture(t, uv + k * vec2(-2, 0)), g = texture(t, uv), h = texture(t, uv + k * vec2(2, 0));\n"
    "  vec4 i = texture(t, uv + k * vec2(-1, 1)), j = texture(t, uv + k * vec2(1, 1));\n"
    "  vec4 l = texture(t, uv + k * vec2(-2, 2)), m = texture(t, uv + k * vec2(0, 2)), n = texture(t, uv + k * vec2(2, 2));\n"
    "  return (d + e + i + j) * 0.125 + (a + c + l + n) * 0.03125 + (b + f + h + m) * 0.0625 + g * 0.125;\n"
    "}\n"
    "vec4 fx_soft(sampler2D t, vec2 uv) {\n"
    "  vec2 o = 0.5 / vec2(textureSize(t, 0));\n"
    "  return 0.25 * (texture(t, uv + o * vec2(-0.5, -1)) + texture(t, uv + o * vec2(1, -0.5)) +\n"
    "                 texture(t, uv + o * vec2(0.5, 1)) + texture(t, uv + o * vec2(-1, 0.5)));\n"
    "}\n"
    "vec4 fx_up(sampler2D t, vec2 uv) {\n"
    "  vec2 ts = vec2(textureSize(t, 0));\n"
    "  if (max(fx_px(uv, ts).x, fx_px(uv, ts).y) > 0.75) return texture(t, uv);  // (not magnified: as the game read it)\n"
    "  vec2 p = uv * ts - 0.5, f = fract(p), q = floor(p) - 0.5;\n"
    "  vec2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0, w1 = (4.0 - 6.0 * f * f + 3.0 * f * f * f) / 6.0;\n"
    "  vec2 w3 = f * f * f / 6.0, w2 = 1.0 - w0 - w1 - w3;\n"
    "  vec2 s0 = w0 + w1, s1 = w2 + w3, c0 = (q + w1 / s0) / ts, c1 = (q + 2.0 + w3 / s1) / ts;\n"
    "  return s0.y * (s0.x * texture(t, vec2(c0.x, c0.y)) + s1.x * texture(t, vec2(c1.x, c0.y))) +\n"
    "         s1.y * (s0.x * texture(t, vec2(c0.x, c1.y)) + s1.x * texture(t, vec2(c1.x, c1.y)));\n"
    "}\n";

static void fx_patch_fs(Prog *p) {
    if (!g_settings.blur_quality || !p->fi || !p->fi->msl || !p->code[1]) return;
    uint64_t h = 1469598103934665603ull;
    const uint8_t *b = (const uint8_t *)(uintptr_t)(p->code[1] + 0x30);
    for (int i = 0; i < 0x200; i++) { h ^= b[i]; h *= 0x100000001B3ull; }  // (draw_'s fragment-shader hash)
    int mode = 0;
    for (size_t i = 0; i < sizeof fx_shaders / sizeof *fx_shaders; i++) if (fx_shaders[i].hash == h) mode = fx_shaders[i].mode;
    const char *src = p->fi->msl, *m = strstr(src, "void main() {");
    if (!mode || !m) return;
    const char *fn = mode == FX_DOWN ? "fx_down(t" : mode == FX_SOFT ? "fx_soft(t" : "fx_up(t";
    size_t n = 0;
    for (const char *q = m; (q = strstr(q, "texture(t")); q++) n++;
    size_t len = strlen(src), cap = len + sizeof fx_glsl + n * 8 + 1;
    char *s = malloc(cap), *o = s;
    if (!s) return;
    memcpy(o, src, (size_t)(m - src)), o += m - src;  // (declarations: the helpers come right before main)
    memcpy(o, fx_glsl, sizeof fx_glsl - 1), o += sizeof fx_glsl - 1;
    for (const char *q = m; *q;) {
        if (!strncmp(q, "texture(t", 9)) { memcpy(o, fn, strlen(fn)), o += strlen(fn), q += 9; }
        else *o++ = *q++;
    }
    *o = 0;
    p->fi->msl = s;  // (the translator's string is kept: it lives as long as the program)
    static int nlog;
    if (nlog++ < 16) LOG("[fx] fragment shader %llx (hash %016llx): %d texture reads %s\n", (unsigned long long)p->code[1],
                         (unsigned long long)h, (int)n, mode == FX_DOWN ? "13-tap downsample" : mode == FX_SOFT ? "rotated-grid" : "B-spline");
}

static uint64_t gmem_limit(void);
static int gmem_chunk_shift(void);
static void translate_program(void *arg) {
    Prog *p = arg;
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = gmem_limit();
    maxwell_gmem_chunk_shift = gmem_chunk_shift();
    uint64_t t0 = SDL_GetTicksNS();
    p->vi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], STAGE_VERTEX, &p->vkey);
    // Experiment (research): LARECOMP_EXP_VS_EARLY=<vertex program address, hex>: that program's vertex shader returns at
    // the start of main (every output at its default), to tell the cost of running the shader from the rest of its draw.
    // (LARECOMP_EXP_VS_EARLY=<address>:<pc, hex>: it returns just before the instruction at that pc instead)
    if (p->vi->msl && getenv("LARECOMP_EXP_VS_EARLY") && p->code[0] == strtoull(getenv("LARECOMP_EXP_VS_EARLY"), NULL, 16)) {
        const char *m = strstr(p->vi->msl, "void main() {"), *r = m ? strstr(m, "  uint r") : NULL;
        const char *colon = strchr(getenv("LARECOMP_EXP_VS_EARLY"), ':');
        if (m && colon) {
            char pat[32];
            snprintf(pat, sizeof pat, "// pc %s:", colon + 1);
            const char *at = strstr(m, pat);
            while (at && at > m && at[-1] != '\n') at--;  // (the start of that line)
            r = at ? at : NULL;
        }
        if (r) {
            size_t len = strlen(p->vi->msl), pr = (size_t)(r - p->vi->msl);
            char *s = malloc(len + 16);
            memcpy(s, p->vi->msl, pr);
            memcpy(s + pr, "  return;\n", 10);
            memcpy(s + pr + 10, p->vi->msl + pr, len - pr + 1);
            p->vi->msl = s;
            LOG("[exp] vertex program %llx returns at the start of main\n", (unsigned long long)p->code[0]);
        }
    }
    // Experiment (research): LARECOMP_GLSL_OVERRIDE_DIR=<dir>: <dir>/<code address, hex>_vs.glsl replaces the generated GLSL
    // of that vertex program (a hand-edited version of what LARECOMP_SAVE_MSL=1 wrote in build/glsl/).
    if (p->vi->msl && getenv("LARECOMP_GLSL_OVERRIDE_DIR")) {
        char path[512];
        snprintf(path, sizeof path, "%s/%llx_vs.glsl", getenv("LARECOMP_GLSL_OVERRIDE_DIR"), (unsigned long long)p->code[0]);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *s = malloc((size_t)n + 1);
            if (s && fread(s, 1, (size_t)n, f) == (size_t)n) { s[n] = 0; p->vi->msl = s; LOG("[exp] GLSL override: %s\n", path); }
            fclose(f);
        }
    }
    // Experiment X4 (docs/PLANO-3D.md), LARECOMP_VK_SKIN_ONEBONE=1: skinned vertex shaders see 100% weight on the
    // first bone (a12 = bone weights) - rigid, but whole, if the first bone's matrix reaches the GPU correctly.
    // Experiment X6, LARECOMP_VK_SKIN_IDX_X=1: every bone slot uses the first bone index (a11.x), weights unchanged.
    if (p->vi->msl && getenv("LARECOMP_VK_SKIN_IDX_X") && strstr(p->vi->msl, "in uvec4 a11;")) {
        const char *d = strstr(p->vi->msl, "in uvec4 a11;"), *m = strstr(p->vi->msl, "void main() {");
        if (d && m && d < m) {
            static const char def[] = "#define a11 uvec4(a11_in.x, a11_in.x, a11_in.x, a11_in.x)\n";
            size_t len = strlen(p->vi->msl), pd = (size_t)(d - p->vi->msl) + strlen("in uvec4 a11"), pm = (size_t)(m - p->vi->msl);
            char *s = malloc(len + 16 + sizeof def), *o = s;
            memcpy(o, p->vi->msl, pd); o += pd;
            memcpy(o, "_in", 3); o += 3;                       // in uvec4 a11_in;
            memcpy(o, p->vi->msl + pd, pm - pd); o += pm - pd;
            memcpy(o, def, sizeof def - 1); o += sizeof def - 1;
            memcpy(o, p->vi->msl + pm, len - pm + 1);
            p->vi->msl = s;
            g_exp_count[3]++;
        }
    }
    // Experiments X8+, LARECOMP_VK_SKIN_W="<GLSL vec4 expression of a12_in>": the bone weights a skinned vertex shader
    // sees, rewritten (a12_in = the weights as the GPU fetched them).
    if (p->vi->msl && getenv("LARECOMP_VK_SKIN_W") && strstr(p->vi->msl, "in vec4 a12;")) {
        const char *d = strstr(p->vi->msl, "in vec4 a12;"), *m = strstr(p->vi->msl, "void main() {");
        if (d && m && d < m) {
            char def[512];
            snprintf(def, sizeof def, "#define a12 (%s)\n", getenv("LARECOMP_VK_SKIN_W"));
            size_t dl = strlen(def), len = strlen(p->vi->msl), pd = (size_t)(d - p->vi->msl) + strlen("in vec4 a12"),
                   pm = (size_t)(m - p->vi->msl);
            char *s = malloc(len + 16 + dl), *o = s;
            memcpy(o, p->vi->msl, pd); o += pd;
            memcpy(o, "_in", 3); o += 3;                       // in vec4 a12_in;
            memcpy(o, p->vi->msl + pd, pm - pd); o += pm - pd;
            memcpy(o, def, dl); o += dl;
            memcpy(o, p->vi->msl + pm, len - pm + 1);
            p->vi->msl = s;
            g_exp_count[3]++;
        }
    }
    // (only skinned shaders - the ones with bone indices too: a12 is something else elsewhere.)
    if (p->vi->msl && getenv("LARECOMP_VK_SKIN_ONEBONE") && strstr(p->vi->msl, "in vec4 a12;") &&
        strstr(p->vi->msl, "in uvec4 a11;")) {
        const char *m = strstr(p->vi->msl, "void main() {");
        if (m) {
            static const char def[] = "#define a12 vec4(1.0, 0.0, 0.0, 0.0)\n";
            size_t pre = (size_t)(m - p->vi->msl), len = strlen(p->vi->msl);
            char *s = malloc(len + sizeof def);
            memcpy(s, p->vi->msl, pre);
            memcpy(s + pre, def, sizeof def - 1);
            memcpy(s + pre + sizeof def - 1, m, len - pre + 1);
            p->vi->msl = s;  // (the translator's string is kept: it lives as long as the program)
            g_exp_count[3]++;
        }
    }
    if (p->vi->msl && !p->code[1]) {  // vertex-only program (see get_program): an empty fragment shader
        p->fi = calloc(1, sizeof *p->fi);
        p->fi->stage = STAGE_FRAGMENT;
        p->fi->msl = strdup("#version 460\nvoid main() {}\n");
    } else if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        p->fkey.vel_vs = p->vi->vel;  // (the fragment side of the motion vectors only if the vertex side took)
        p->fi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], STAGE_FRAGMENT, &p->fkey);
    }
    atomic_fetch_add(&stat_translate_ns, SDL_GetTicksNS() - t0);
    if (!p->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        atomic_store(&p->state, 2);
        return;
    }
    if (!p->fi->msl) {
        LOG("[shader] FS translate failed: %s\n", p->fi->error);
        atomic_store(&p->state, 2);
        return;
    }
    fx_patch_fs(p);
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    compile_program(p);  // (this worker: queued again it would wait behind the other translations)
}

#define PROG_TAB 4096
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

// BindProgram(program, stages): the stages in the mask come from `program` (NULL: unbound), the others stay. A bind
// that leaves one program's stages and nothing else uses that program itself (as before); a mix of stages from
// several programs (Let's Go: vertex and fragment bound separately) uses a combined program. The shader caches are
// keyed by code address, so the combination needs no identity of its own.
static NvnProgram g_comb_prog;
static void bind_program_stages(NvnProgram *p, uint32_t mask) {
    if (!mask) mask = p ? p->stage_mask : 0x3F;  // (no mask recorded: the whole program)
    NvnProgram *cur = st.program;
    NvnProgram next = cur ? *cur : (NvnProgram){0};
    next.backend = NULL;
    for (int i = 0; i < 6; i++) {
        uint32_t bit = 1u << i;
        if (!(mask & bit)) continue;
        if (p && (p->stage_mask & bit)) next.stages[i] = p->stages[i], next.stage_mask |= bit;
        else memset(&next.stages[i], 0, sizeof next.stages[i]), next.stage_mask &= ~bit;
    }
    if (!next.stage_mask) { st.program = NULL; return; }
    if (p && next.stage_mask == p->stage_mask &&
        !memcmp(next.stages, p->stages, sizeof next.stages)) { st.program = p; return; }
    g_comb_prog = next;
    st.program = &g_comb_prog;
}

static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    // A vertex shader without a fragment shader (Let's Go's depth / shadow passes: stages 0x1) gets an empty fragment
    // shader in translate_program - no color output, only depth, as the hardware does. They were all skipped.
    if (!(np->stage_mask & 1)) {
        static uint32_t seen;
        if (np->stage_mask < 32 && !(seen & (1u << np->stage_mask))) {
            seen |= 1u << np->stage_mask;
            LOG("[shader] draw with stages %#x (needs vertex + fragment): skipped\n", np->stage_mask);
        }
        return NULL;
    }
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = np->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    // (and what is at the addresses: a shader archive reloaded elsewhere puts other shaders there, objects.c)
    for (int i = 0; i < 2; i++) { h ^= np->stages[i].content_hash; h *= 0x100000001B3ull; }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    h ^= fkey->velocity | (uint64_t)fkey->vel_rt << 8; h *= 0x100000001B3ull;
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    for (int probe = 0; prog_tab[slot].key && prog_tab[slot].key != h; probe++) {
        if (probe == 64) { prog_tab[slot].key = 0; break; }  // (full run of slots: the entry is replaced, its Prog kept)
        slot = (slot + 1) & (PROG_TAB - 1);
    }
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    {   // an address seen before with other code: the old address-only key would have handed back that shader
        static struct { uint64_t addr, content; } seen[8192];
        static uint64_t nother;
        for (int i = 0; i < 2; i++) {
            uint64_t a = np->stages[i].code_addr, c = np->stages[i].content_hash;
            if (!a) continue;
            uint32_t s = (uint32_t)((a >> 8) * 0x9E3779B1u) & 8191;
            for (int k = 0; k < 16; k++, s = (s + 1) & 8191) {
                if (!seen[s].addr) { seen[s].addr = a; seen[s].content = c; break; }
                if (seen[s].addr != a) continue;
                if (seen[s].content != c && (nother++, !(nother & (nother - 1))))
                    LOG("[shader] %s shader address %llx now holds another shader %llu (f%llu)\n", i ? "fragment" : "vertex",
                        (unsigned long long)a, (unsigned long long)nother, (unsigned long long)renderer_game_frame());
                seen[s].content = c;
                break;
            }
        }
    }
    Prog *p = calloc(1, sizeof *p);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    run_job(translate_program, p);
    return p;
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default: {
        static uint8_t told[256];
        if (f < 256 && !told[f]++) LOG("[vk] vertex format %#x unknown: read as RGBA32F\n", f);
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
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

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint8_t shadow_q;  // the shadow filter (programs whose fragment shader reads a shadow map: ShaderInfo.uses_shpcf)
    uint32_t write_mask[8];
} PipeKey;

typedef struct { PipeKey k; uint64_t hash; VkPipeline pipe; _Atomic int state; } Pipe;  // state: 0 building, 1 ready, 2 failed
#define PIPE_TAB 8192
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// LARECOMP_PIPE_STATS=<vertex program address, hex>|all: after each pipeline is created, what the driver reports about its
// compiled shaders (VK_KHR_pipeline_executable_properties: one executable per stage; registers, spills, instructions...).
static void pipe_exec_report(VkPipeline pipe, uint64_t vs, uint64_t fs) {
    const char *want = getenv("LARECOMP_PIPE_STATS");
    if (!want || (strcmp(want, "all") && strtoull(want, NULL, 16) != vs)) return;
    static PFN_vkGetPipelineExecutablePropertiesKHR get_props;
    static PFN_vkGetPipelineExecutableStatisticsKHR get_stats;
    if (!get_props) {
        get_props = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineExecutablePropertiesKHR");
        get_stats = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineExecutableStatisticsKHR");
        if (!get_props || !get_stats) { LOG("[pipestats] the driver does not export the functions\n"); return; }
    }
    uint32_t n = 0;
    VkPipelineInfoKHR pinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipe};
    if (get_props(g_vk.dev, &pinfo, &n, NULL) != VK_SUCCESS || !n) return;
    VkPipelineExecutablePropertiesKHR props[8] = {0};
    if (n > 8) n = 8;
    for (uint32_t i = 0; i < n; i++) props[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    if (get_props(g_vk.dev, &pinfo, &n, props) != VK_SUCCESS) return;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ns = 0;
        VkPipelineExecutableInfoKHR einfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = pipe, .executableIndex = i};
        if (get_stats(g_vk.dev, &einfo, &ns, NULL) != VK_SUCCESS) continue;
        VkPipelineExecutableStatisticKHR stats[64] = {0};
        if (ns > 64) ns = 64;
        for (uint32_t s = 0; s < ns; s++) stats[s].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        if (get_stats(g_vk.dev, &einfo, &ns, stats) != VK_SUCCESS) continue;
        char line[4096];
        int l = snprintf(line, sizeof line, "[pipestats] vs %llx fs %llx  %s (stages %#x):", (unsigned long long)vs, (unsigned long long)fs,
                         props[i].name, props[i].stages);
        for (uint32_t s = 0; s < ns && l < (int)sizeof line - 80; s++) {
            const VkPipelineExecutableStatisticValueKHR *v = &stats[s].value;
            switch (stats[s].format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: l += snprintf(line + l, sizeof line - l, " %s=%u;", stats[s].name, v->b32); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: l += snprintf(line + l, sizeof line - l, " %s=%lld;", stats[s].name, (long long)v->i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: l += snprintf(line + l, sizeof line - l, " %s=%llu;", stats[s].name, (unsigned long long)v->u64); break;
            default: l += snprintf(line + l, sizeof line - l, " %s=%g;", stats[s].name, v->f64); break;
            }
        }
        LOG("%s\n", line);
    }
}

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    PipeKey *k = &pp->k;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->prog->fs, .pName = "main"}};
    int32_t shadow_q = k->shadow_q;
    VkSpecializationMapEntry sme = {SHADOW_Q_SPEC_ID, 0, sizeof shadow_q};
    VkSpecializationInfo spi = {1, &sme, sizeof shadow_q, &shadow_q};
    if (k->prog->fi && k->prog->fi->uses_shpcf) stages[1].pSpecializationInfo = &spi;
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    {   // (LARECOMP_PIPE_STATS: the vertex fetch layout of the program asked for)
        const char *w = getenv("LARECOMP_PIPE_STATS");
        if (w && (!strcmp(w, "all") || strtoull(w, NULL, 16) == k->prog->code[0])) {
            LOG("[pipestats] vs %llx inputs:", (unsigned long long)k->prog->code[0]);
            for (uint32_t i = 0; i < na; i++) LOG(" a%u(bind %u fmt %d off %u)", va[i].location, va[i].binding, (int)va[i].format, va[i].offset);
            LOG(" | bindings:");
            for (uint32_t i = 0; i < nb; i++)
                LOG(" %u(stride %u %s)", vb[i].binding, vb[i].stride, vb[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? "instance" : "vertex");
            for (uint32_t i = 0; i < nd; i++) LOG(" div%u=%u", vd[i].binding, vd[i].divisor);
            LOG("\n");
        }
    }
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout};
    uint64_t tp = SDL_GetTicksNS();
    extern bool g_pipe_exec_stats;
    if (g_pipe_exec_stats) pi.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pp->pipe);
    atomic_fetch_add(&stat_pipe_ns, SDL_GetTicksNS() - tp);
    if (r == VK_SUCCESS && g_pipe_exec_stats) pipe_exec_report(pp->pipe, k->prog->code[0], k->prog->code[1]);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    atomic_fetch_add(&stat_compiles, 1);
    if (r == VK_SUCCESS) atomic_fetch_add(&new_pipes, 1);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static int nfeedback;
static uint64_t g_cur_fs_hash;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

typedef struct { NvnTexture *key; TexBackend copy; bool lo; } Snap;
static Snap snaps[16];
static int nsnaps;

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    bool lo = vk_tex_lo_active();  // (the reduced in-between images' targets: copies of their own)
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t && snaps[i].lo == lo) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t, s->lo = lo;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ||
        (b->format != VK_FORMAT_D32_SFLOAT_S8_UINT && b->format != VK_FORMAT_D32_SFLOAT)) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image && !snaps[k].lo) existing = &snaps[k];
            if (vk_tex_lo_active()) existing = NULL;  // (whether a copy is still good is known for the full-size targets only)
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing) {
                feedback_tex[nfeedback] = t;
                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            stat_feedback_copies++;
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                              .extent = {src->width, src->height, 1}};
            vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t;
            feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
            if (vk_tex_lo_active()) { st.rt_dirty = true; continue; }
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
// ================================================================== per-object motion vectors (level 2)
// The scene pass's opaque draws run a vertex shader variant that also computes the previous frame's position
// (ShaderKey.velocity: maxwell/translate.c) from the previous frame's copy of the same draw's constant buffers. A draw
// is the same draw in the next frame by its program, vertex buffer, draw parameters and game object, and the
// occurrence of that in the frame (a mesh drawn for several objects). Its vertex cbufs 3+ are kept per frame (two
// frames); one whose previous copy is missing, of another size, or jumps (a camera cut, another object paired with
// it: a word changing by more than half of its magnitude + MVO_JUMP) has its velocity unknown (mvo_check): the
// camera's is used there.
typedef struct { uint64_t key; uint32_t off, size; } MvoEntry;
typedef struct {
    MvoEntry *e;
    uint32_t n, cap;
    uint8_t *data;
    uint64_t used, dcap, frame;
    uint32_t *tab;
} MvoFrame;
#define MVO_TAB (1u << 15)
static MvoFrame mvo[2];
static int mvo_cur;
static uint64_t mvo_draw;  // this draw's key (0: not one of the velocity draws)
static bool mvo_lookup;    // (cbuf_info called for the fallback: no recording)
static uint64_t mvo_occ_key[MVO_TAB];
static uint32_t mvo_occ[MVO_TAB];
static uint64_t mvo_stats[3];  // cbufs: previous found, missing, rejected (a jump)
// A word changing by more than half of its magnitude + this between two frames is a jump (another object paired, a
// cut). In the game's units (~cm): 4 rejected the running player's feet (a bone moving 9 cm in a frame)
#define MVO_JUMP 40.0f
// this draw's previous copies (mvo_check): all of its vertex cbufs 3+ found, or none used - a draw missing one (Let's
// Go: its per-draw block with the camera, cb3, or its object block) gets zeros for all, which puts its previous
// position at w = 0: the fragment shader marks the velocity unknown there (w = 0) and the camera's is used instead.
// (Taking this frame's copy for the missing one stood the object still on the screen: the door, frame to frame.)
static const void *mvo_pd[18];
static uint32_t mvo_ps[18];
static bool mvo_all;

// A draw's own arguments in its CmdRec.x (x1..x7 of the call; runtime/nvn/cmdbuf.c fills the ones a shorter call
// leaves out): elements - mode, index type, count, indices, base vertex, base instance, instances; arrays - mode,
// first, count, base instance, instances. Past them are leftover registers (and past x[6] the float arguments): a
// key over those changed from frame to frame for some draws (the player's left eye never found its previous copy).
static int draw_nargs(bool indexed) { return indexed ? 7 : 5; }

static uint64_t mvo_mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0x100000001B3ull; }
// this draw without its occurrence (mvo_draw_key), and which occurrence of that it is in the frame
static uint64_t mvo_base;
static uint32_t mvo_nth;
static uint64_t mvo_occ_draw(uint64_t base, uint32_t n) { uint64_t k = mvo_mix(base, n); return k ? k : 1; }

// The record of this real frame (renderer_present_count: an in-between image records nothing - it is a side image whose
// previous is N, the frame just recorded - and starts counting occurrences again: mvo_replay_begin)
static void mvo_frame_check(void) {
    if (ip_replaying) return;
    uint64_t fr = renderer_present_count();
    MvoFrame *f = &mvo[mvo_cur];
    if (f->frame == fr && f->tab) return;
    mvo_cur ^= 1;
    f = &mvo[mvo_cur];
    if (!f->tab) f->tab = calloc(MVO_TAB, sizeof *f->tab);
    else memset(f->tab, 0, MVO_TAB * sizeof *f->tab);
    f->n = 0, f->used = 0, f->frame = fr;
    memset(mvo_occ_key, 0, sizeof mvo_occ_key);
}

static uint64_t mvo_draw_key(const CmdRec *r, bool indexed) {
    mvo_frame_check();
    uint64_t h = mvo_mix(0xcbf29ce484222325ull, (uint64_t)(uintptr_t)st.program);
    h = mvo_mix(h, indexed);
    h = mvo_mix(h, st.vbuf[0].addr);
    for (int i = 0; i < draw_nargs(indexed); i++) h = mvo_mix(h, r->x[i]);
    h = mvo_mix(h, st.obj);
    if (!h) h = 1;
    uint32_t s = (uint32_t)(h ^ (h >> 29)) & (MVO_TAB - 1);
    while (mvo_occ_key[s] && mvo_occ_key[s] != h) s = (s + 1) & (MVO_TAB - 1);
    if (!mvo_occ_key[s]) { mvo_occ_key[s] = h; mvo_occ[s] = 0; }
    mvo_base = h, mvo_nth = mvo_occ[s];
    return mvo_occ_draw(h, mvo_occ[s]++);
}

static void mvo_record(int k, const void *src, uint32_t size) {
    MvoFrame *f = &mvo[mvo_cur];
    if (ip_replaying || !f->tab || f->n >= MVO_TAB / 2) return;
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 4096; f->e = realloc(f->e, f->cap * sizeof *f->e); }
    if (f->used + size > f->dcap) { f->dcap = (f->used + size) * 2 + (1 << 20); f->data = realloc(f->data, f->dcap); }
    uint64_t key = mvo_mix(mvo_draw, (uint64_t)k);
    memcpy(f->data + f->used, src, size);
    f->e[f->n] = (MvoEntry){key, (uint32_t)f->used, size};
    f->used += size;
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (MVO_TAB - 1);
    while (f->tab[s]) s = (s + 1) & (MVO_TAB - 1);
    f->tab[s] = ++f->n;
}

// The previous frame's copy of draw `draw`'s cbuf k (NULL: none)
static const MvoEntry *mvo_find(const MvoFrame *f, uint64_t draw, int k) {
    uint64_t key = mvo_mix(draw, (uint64_t)k);
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (MVO_TAB - 1);
    while (f->tab[s]) {
        const MvoEntry *c = &f->e[f->tab[s] - 1];
        if (c->key == key) return c;
        s = (s + 1) & (MVO_TAB - 1);
    }
    return NULL;
}

// The record the current image's previous constants come from: the real frame before, or N for an in-between image
static const MvoFrame *mvo_prev_frame(void) { return &mvo[ip_replaying ? mvo_cur : mvo_cur ^ 1]; }
static bool mvo_prev_ok(void) {
    const MvoFrame *f = mvo_prev_frame();
    return f->tab && (ip_replaying || f->frame + 1 == mvo[mvo_cur].frame);
}
static void mvo_replay_begin(void) { memset(mvo_occ_key, 0, sizeof mvo_occ_key); }

// How far a previous copy is from this frame's data: the sum of the float words' differences, stopping past `limit`
static float mvo_dist(const uint32_t *a, const uint32_t *b, uint32_t nw, float limit) {
    float d = 0;
    for (uint32_t i = 0; i < nw && d < limit; i++) {
        if (a[i] == b[i] || ((a[i] >> 23) & 0xFF) == 0xFF || ((b[i] >> 23) & 0xFF) == 0xFF) continue;
        float fa, fb;
        memcpy(&fa, &a[i], 4), memcpy(&fb, &b[i], 4);
        d += fabsf(fb - fa);
    }
    return d;
}

// Which of the previous frame's draws this one is. A mesh drawn several times without a game object of its own (the
// flowers and the grass: obj 0) is told apart only by the order of its draws, and that order shifts when one of them
// is culled or comes into view - the camera moving: the n-th draw then took the previous frame's (n +- 1)-th
// constants, another bush's, and a vector towards it (FSR 3.1's frame generation, since removed, smeared them: pieces
// of flower beds floating beside them). Of the previous frame's occurrences of the same draw, the one whose vertex cbufs 3+ are closest to
// this draw's is taken (the same occurrence first, kept unless another is closer).
static uint64_t mvo_pick(const ShaderInfo *si) {
    if (!mvo_prev_ok()) return mvo_draw;
    const MvoFrame *f = mvo_prev_frame();
    int k0 = -1;
    for (int k = 3; k < 18 && k0 < 0; k++)
        if ((si->cbuf_mask & (1u << k)) && st.ubuf[0][k - 3].addr) k0 = k;
    if (k0 < 0 || !mvo_find(f, mvo_occ_draw(mvo_base, mvo_nth ? 0 : 1), k0)) return mvo_draw;  // (one occurrence: itself)
    uint64_t pick = mvo_draw;
    float best = INFINITY;
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t n = i == 0 ? mvo_nth : i - 1 < mvo_nth ? i - 1 : i;  // (this occurrence first, then all the others)
        uint64_t cand = mvo_occ_draw(mvo_base, n);
        if (i > 0 && !mvo_find(f, cand, k0)) break;  // (past the previous frame's last occurrence)
        float d = 0;
        bool whole = true;
        for (int k = k0; k < 18 && d < best; k++) {
            if (!(si->cbuf_mask & (1u << k))) continue;
            Range r = st.ubuf[0][k - 3];
            const MvoEntry *e = mvo_find(f, cand, k);
            if (!e || !r.addr) { whole = false; break; }
            d += mvo_dist((const uint32_t *)(f->data + e->off), (const uint32_t *)(uintptr_t)r.addr,
                          (uint32_t)((r.size < e->size ? r.size : e->size) / 4), best - d);
        }
        if (whole && d < best) best = d, pick = cand;
        if (best == 0) break;
    }
    return pick;
}

// The previous frame's copy of the vertex cbuf k of draw `draw` (mvo_pick), if usable (else NULL)
static const void *mvo_prev(uint64_t draw, int k, uint32_t *size) {
    const MvoFrame *f = mvo_prev_frame();
    if (!mvo_prev_ok()) { mvo_stats[1]++; return NULL; }
    const MvoEntry *e = mvo_find(f, draw, k);
    Range r = k - 3 >= 0 && k - 3 < 18 ? st.ubuf[0][k - 3] : (Range){0, 0};
    static long probe_at = -2;
    if (probe_at == -2) probe_at = getenv("LARECOMP_MVO_PROBE") ? atol(getenv("LARECOMP_MVO_PROBE")) : -1;
    bool probe = probe_at >= 0 && (long)renderer_present_count() == probe_at;
    if (!e || !r.addr) {
        if (probe) LOG("[mvoprobe] draw %016llx vs %llx k %d MISSING (entry %d addr %llx)\n", (unsigned long long)mvo_draw,
                       (unsigned long long)st.program->stages[0].code_addr, k, e != NULL, (unsigned long long)r.addr);
        mvo_stats[1]++;
        return NULL;
    }
    const uint32_t *a = (const uint32_t *)(f->data + e->off), *b = (const uint32_t *)(uintptr_t)r.addr;
    uint32_t nw = (uint32_t)((r.size < e->size ? r.size : e->size) / 4);
    for (uint32_t i = 0; i < nw; i++) {
        if (a[i] == b[i]) continue;
        uint32_t ea = (a[i] >> 23) & 0xFF, eb = (b[i] >> 23) & 0xFF;
        if (ea == 0xFF || eb == 0xFF) continue;
        float fa, fb;
        memcpy(&fa, &a[i], 4), memcpy(&fb, &b[i], 4);
        if (fabsf(fb - fa) > 0.5f * (fabsf(fa) + fabsf(fb)) + MVO_JUMP) {
            if (probe) LOG("[mvoprobe] draw %016llx vs %llx k %d REJECTED word %u: %g -> %g (size %u)\n", (unsigned long long)mvo_draw,
                           (unsigned long long)st.program->stages[0].code_addr, k, i, fa, fb, e->size);
            mvo_stats[2]++;
            return NULL;
        }
    }
    mvo_stats[0]++;
    {   // (research, LARECOMP_MVO_PROBE=<frame>: each velocity draw's cbufs - previous identical to this frame's?)
        static long at = -2;
        if (at == -2) at = getenv("LARECOMP_MVO_PROBE") ? atol(getenv("LARECOMP_MVO_PROBE")) : -1;
        if (at >= 0 && (long)renderer_present_count() == at) {
            uint32_t same = 0;
            for (uint32_t i = 0; i < nw; i++) same += a[i] == b[i];
            LOG("[mvoprobe] draw %016llx prog %p vs %llx k %d size %u words %u identical %u addr %llx\n", (unsigned long long)mvo_draw,
                (void *)st.program, (unsigned long long)st.program->stages[0].code_addr, k, e->size, nw, same, (unsigned long long)r.addr);
        }
    }
    *size = e->size;
    return f->data + e->off;
}

static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz);
// Before a velocity draw's descriptors: its vertex cbufs' previous copies, all or none
static void mvo_check(const ShaderInfo *si) {
    mvo_all = true;
    uint64_t prev = mvo_pick(si);
    for (int k = 3; k < 18; k++) {
        mvo_pd[k] = NULL;
        if (!(si->cbuf_mask & (1u << k))) continue;
        mvo_pd[k] = mvo_prev(prev, k, &mvo_ps[k]);
        if (!mvo_pd[k]) mvo_all = false;
    }
}

// stage_set, bindings 100 + k: the previous frame's cbuf k, or zeros (velocity unknown: mvo_check)
static VkDescriptorBufferInfo mvo_prev_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (mvo_draw && stage == 0 && mvo_all && k < 18 && mvo_pd[k])
        return ubo_span(vk_transient_copy(mvo_pd[k], mvo_ps[k], 256), mvo_ps[k]);
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if ((si->cbuf_dynamic & (1u << k)) && size < 0x10000) size = 0x10000;
    return ubo_span(vk_transient_copy(zeros, size, 256), size);
}

static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    // frame interpolation: recorded now, or (in the replay) the blended copy this draw takes - also what its motion
    // vectors are taken from there
    uint64_t prof_ti_ = prof ? SDL_GetTicksNS() : 0;
    const void *ipd = ip_enabled() ? ip_cbuf(stage, k, r.addr, (uint32_t)size) : NULL;
    if (prof) prof_ipc += SDL_GetTicksNS() - prof_ti_;
    if (mvo_draw && stage == 0 && !mvo_lookup && size <= r.size)
        mvo_record(k, ipd ? ipd : (const void *)(uintptr_t)r.addr, (uint32_t)size);  // (motion vectors)
    bool skinned_vs = stage == 0 && ((si->attr_mask >> 11) & 1);  // (Let's Go: a11 = bone indices)
    // Experiment X2 (docs/PLANO-3D.md), LARECOMP_VK_SKIN_IDENTITY=1: a skinned vertex shader's dynamically indexed
    // cbuf (its bone palette) reads identity matrices - models in their bind pose if the rest of the chain is right.
    static int skin_identity = -1;
    if (skin_identity < 0) skin_identity = getenv("LARECOMP_VK_SKIN_IDENTITY") != NULL;
    if (skin_identity && skinned_vs && (si->cbuf_dynamic & (1u << k))) {
        static float ident[0x10000 / 4];
        if (ident[0] != 1.0f)
            for (int m = 0; m < 0x10000 / 64; m++) ident[m * 16] = ident[m * 16 + 5] = ident[m * 16 + 10] = ident[m * 16 + 15] = 1.0f;
        g_exp_count[0]++;
        return ubo_span(vk_transient_copy(ident, size, 256), size);
    }
    // LARECOMP_VK_UBO_DIRECT=1 (experiment X3): bind the range in place in the imported guest heap, read when the GPU
    // runs - as on the console - in case compute shaders write it after this point. Counted: direct vs copied.
    static int direct = -1;
    if (direct < 0) direct = getenv("LARECOMP_VK_UBO_DIRECT") ? atoi(getenv("LARECOMP_VK_UBO_DIRECT")) : 0;
    if (direct) {
        extern bool vk_guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);
        VkBuffer gb;
        VkDeviceSize goff;
        uint64_t align = g_vk.props.limits.minUniformBufferOffsetAlignment;
        if (!(r.addr % (align ? align : 256)) && vk_guest_direct(r.addr, size, &gb, &goff)) {
            if (skinned_vs) g_exp_count[1]++;
            return ubo_span((VkSpan){.buf = gb, .off = goff}, size);
        }
        if (skinned_vs) g_exp_count[2]++;
    }
    if (ipd) return ubo_span(vk_transient_copy(ipd, size, 256), size);  // (the replay's blended copy)
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(r.addr, size), size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): the guest heap imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// LARECOMP_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;

static bool gmem_import(uint64_t size) {
    if (g_vk.heap_export) return false;  // (Android: the heap is GPU memory itself, renderer_share_heap)
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)HEAP_BASE, &mp) != VK_SUCCESS ||
        !mp.memoryTypeBits)
        return false;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)HEAP_BASE};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, &mem) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};  // (uniforms read in place: cbuf_info)
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (gmem_buf) {  // the heap grew: the old import may still be in use by submitted work
        vk_wait_seq(submit_seq);
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    LOG("[vk] guest heap imported for shader global memory: %llu MB\n", (unsigned long long)(size >> 20));
    return true;
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)

// ---- the game's memory pools in GPU memory (Android). Its drivers can't import process memory (no
// VK_EXT_external_memory_host), so the memory the GPU reads in place - what the game puts in its NVN memory pools, as on
// the Switch, where the GPU sees nothing else - becomes GPU memory itself: when the game makes a pool, its pages get an
// exportable allocation (pieces of at most maxMemoryAllocationSize) holding what they had, mapped over them from its fd
// (VK_KHR_external_memory_fd); when it finalizes one, its pages go back to plain memory. Not the whole heap: the driver
// commits a whole allocation at once (+2.2 GB of RAM for the 3.2 GB heap, measured). The game's code sees memory as
// before. Shaders reach a page through a table of device addresses, one per heap page (maxwell_gmem_chunk_shift 12,
// gmem_info binds it; pages outside pools: one zero page); in-place reads through the piece's buffer (guest_direct).
#ifdef __ANDROID__
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#define PAGE_SHIFT_GM 12
typedef struct { uint64_t at, n, free_after; VkBuffer buf; VkDeviceMemory mem; } HeapPiece;
static pthread_mutex_t pieces_lk = PTHREAD_MUTEX_INITIALIZER;
static HeapPiece *pieces, *dead;  // live pieces by address; finalized ones, freed once the GPU is past them
static int npieces, cap_pieces, ndead, cap_dead;
static VkBuffer ptab_buf, zero_buf;
static uint64_t *ptab;             // per heap page: its device address (the zero page outside pools)
static VkDeviceAddress zero_page;
static uint64_t piece_max;         // largest allocation
static uint64_t pool_bytes;        // in GPU memory now

static uint32_t heap_memory_type(uint32_t bits, bool host_only) {
    static int forced = -2;  // LARECOMP_VK_HEAP_TYPE=<index>: that memory type for the pools (measurements)
    if (forced == -2) forced = getenv("LARECOMP_VK_HEAP_TYPE") ? atoi(getenv("LARECOMP_VK_HEAP_TYPE")) : -1;
    if (!host_only && forced >= 0 && forced < 32 && (bits >> forced & 1)) return (uint32_t)forced;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    const VkMemoryPropertyFlags want[] = {  // (cached first; exportable memory on Adreno: uncached only)
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    for (int w = 0; w < 2; w++)
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want[w]) == want[w]) return i;
    return UINT32_MAX;
}

// A buffer over its own host-visible allocation, with a device address; exp: exportable (its fd in *fd).
static bool gm_alloc(uint64_t n, bool exp, VkBuffer *buf, VkDeviceMemory *mem, void **map, int *fd, uint32_t *type_out) {
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = exp ? &eb : NULL, .size = n,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, *buf, &mr);
    uint32_t type = heap_memory_type(mr.memoryTypeBits, !exp);
    VkMemoryAllocateFlagsInfo fl = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &fl, .buffer = *buf};
    VkExportMemoryAllocateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &ded,
                                     .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = exp ? (void *)&ex : (void *)&ded,
                               .allocationSize = mr.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(g_vk.dev, &ai, NULL, mem) != VK_SUCCESS) {
        vkDestroyBuffer(g_vk.dev, *buf, NULL);
        return false;
    }
    vkBindBufferMemory(g_vk.dev, *buf, *mem, 0);
    *map = NULL, *fd = -1;
    PFN_vkGetMemoryFdKHR get_fd = exp ? (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryFdKHR") : NULL;
    VkMemoryGetFdInfoKHR fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = *mem,
                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    if (vkMapMemory(g_vk.dev, *mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS || (exp && (!get_fd || get_fd(g_vk.dev, &fi, fd) != VK_SUCCESS))) {
        if (*map) vkUnmapMemory(g_vk.dev, *mem);
        vkDestroyBuffer(g_vk.dev, *buf, NULL);
        vkFreeMemory(g_vk.dev, *mem, NULL);
        return false;
    }
    if (type_out) *type_out = type;
    return true;
}
static VkDeviceAddress gm_address(VkBuffer buf) {
    VkBufferDeviceAddressInfo da = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf};
    return vkGetBufferDeviceAddress(g_vk.dev, &da);
}

// The page table and the zero page, when the game sets its heap up (kernel.c SetHeapSize; again after an in-place reboot).
uint64_t renderer_share_heap(uint64_t lo, uint64_t size) {
    (void)lo, (void)size;
    if (!g_vk.dev || !g_vk.heap_export || getenv("LARECOMP_VK_NO_GMEM") || ptab) return 0;
    VkDeviceMemory mem;
    void *zmap;
    int fd;
    uint64_t tn = (HEAP_MAX >> PAGE_SHIFT_GM) * 8;
    if (!gm_alloc(tn, false, &ptab_buf, &mem, (void **)&ptab, &fd, NULL)) { LOG("[vk] no page table for the pools: shader global memory reads return zero\n"); return 0; }
    if (!gm_alloc(1 << PAGE_SHIFT_GM, false, &zero_buf, &mem, &zmap, &fd, NULL)) {
        LOG("[vk] no zero page for the pools: shader global memory reads return zero\n");
        ptab = NULL;  // (the table's allocation stays)
        return 0;
    }
    memset(zmap, 0, 1 << PAGE_SHIFT_GM);
    zero_page = gm_address(zero_buf);
    for (uint64_t i = 0; i < tn / 8; i++) ptab[i] = zero_page;
    VkPhysicalDeviceMaintenance3Properties m3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m3};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &pp);
    piece_max = m3.maxMemoryAllocationSize & ~0xFFFFull;
    LOG("[vk] memory pools in GPU memory for shader global memory (pieces of up to %llu MB)\n", (unsigned long long)(piece_max >> 20));
    return 0;
}

static void reap_dead(bool all) {  // (pieces_lk held)
    uint64_t done = all ? UINT64_MAX : vk_completed_seq();
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].free_after <= done) {
            vkDestroyBuffer(g_vk.dev, dead[i].buf, NULL);
            vkFreeMemory(g_vk.dev, dead[i].mem, NULL);
        } else dead[k++] = dead[i];
    }
    ndead = k;
}

// One piece of a pool: [at, at + n) into GPU memory, keeping what it holds.
static bool piece_make(uint64_t at, uint64_t n) {
    HeapPiece p = {.at = at, .n = n};
    void *drv;
    int fd;
    uint32_t type;
    if (!gm_alloc(n, true, &p.buf, &p.mem, &drv, &fd, &type)) return false;
    static int checked;
    memcpy(drv, (void *)(uintptr_t)at, n);
    bool ok = mmap((void *)(uintptr_t)at, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != MAP_FAILED;
    if (ok && !checked) {  // (the first time: the fd's mapping is the driver's memory)
        volatile uint32_t *here = (volatile uint32_t *)(uintptr_t)at, keep = here[0];
        here[0] = keep ^ 0x5a5a5a5au;
        ok = ((volatile uint32_t *)drv)[0] == (keep ^ 0x5a5a5a5au);
        here[0] = keep;
        if (!ok) mmap((void *)(uintptr_t)at, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0),
                 memcpy((void *)(uintptr_t)at, drv, n);
        LOG("[vk] memory pools: %s (memory type %u)\n", ok ? "their GPU memory mapped in place" : "the fd's mapping is not the GPU memory", type);
        checked = ok ? 1 : -1;
    }
    close(fd);  // (the mapping stays)
    vkUnmapMemory(g_vk.dev, p.mem);
    if (!ok) {
        vkDestroyBuffer(g_vk.dev, p.buf, NULL);
        vkFreeMemory(g_vk.dev, p.mem, NULL);
        return false;
    }
    VkDeviceAddress a = gm_address(p.buf);
    uint64_t pg = (at - HEAP_BASE) >> PAGE_SHIFT_GM;
    for (uint64_t k = 0; k < n >> PAGE_SHIFT_GM; k++) ptab[pg + k] = a + (k << PAGE_SHIFT_GM);
    pthread_mutex_lock(&pieces_lk);
    if (npieces == cap_pieces) pieces = realloc(pieces, sizeof *pieces * (size_t)(cap_pieces = cap_pieces ? cap_pieces * 2 : 1024));
    int i = npieces;
    while (i > 0 && pieces[i - 1].at > at) i--;
    memmove(&pieces[i + 1], &pieces[i], sizeof *pieces * (size_t)(npieces - i));
    pieces[i] = p;
    npieces++;
    pool_bytes += n;
    pthread_mutex_unlock(&pieces_lk);
    return true;
}

// nvnMemoryPoolInitialize (objects.c): a pool in the heap, page-aligned, goes into GPU memory.
void backend_pool_created(uint64_t addr, uint64_t size) {
    if (!ptab || addr < HEAP_BASE || addr + size > HEAP_BASE + HEAP_MAX || !size || ((addr | size) & ((1 << PAGE_SHIFT_GM) - 1))) return;
    pthread_mutex_lock(&pieces_lk);
    reap_dead(false);
    pthread_mutex_unlock(&pieces_lk);
    for (uint64_t off = 0; off < size;) {
        uint64_t n = size - off < piece_max ? size - off : piece_max;
        if (!piece_make(addr + off, n)) {
            static int nlog;
            if (nlog++ < 8) LOG("[vk] pool %llx (%llu KB) not in GPU memory: shader reads of it return zero\n", (unsigned long long)addr,
                                (unsigned long long)(size >> 10));
        }
        off += n;
    }
}

// nvnMemoryPoolFinalize (objects.c, after rt_drain): its pages back to plain memory, keeping what they hold; the GPU
// memory is freed once the GPU is past everything submitted so far.
void backend_pool_destroyed(uint64_t addr, uint64_t size) {
    if (!ptab) return;
    pthread_mutex_lock(&pieces_lk);
    int k = 0;
    for (int i = 0; i < npieces; i++) {
        HeapPiece *p = &pieces[i];
        if (p->at < addr || p->at >= addr + size) { pieces[k++] = *p; continue; }
        uint64_t pg = (p->at - HEAP_BASE) >> PAGE_SHIFT_GM;
        for (uint64_t j = 0; j < p->n >> PAGE_SHIFT_GM; j++) ptab[pg + j] = zero_page;
        void *tmp = malloc(p->n);
        if (tmp) memcpy(tmp, (void *)(uintptr_t)p->at, p->n);
        mmap((void *)(uintptr_t)p->at, p->n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        if (tmp) memcpy((void *)(uintptr_t)p->at, tmp, p->n), free(tmp);
        pthread_mutex_lock(&seq_lock);
        p->free_after = submit_seq + 1;  // (work recorded but not submitted yet goes with the next submission)
        pthread_mutex_unlock(&seq_lock);
        if (ndead == cap_dead) dead = realloc(dead, sizeof *dead * (size_t)(cap_dead = cap_dead ? cap_dead * 2 : 256));
        dead[ndead++] = *p;
        pool_bytes -= p->n;
    }
    npieces = k;
    reap_dead(false);
    pthread_mutex_unlock(&pieces_lk);
}

// In-place reboot (vk_exec_reset, GPU idle; guest memory is replaced next): every piece freed, the table cleared.
static void pools_reset(void) {
    if (!ptab) return;
    pthread_mutex_lock(&pieces_lk);
    for (int i = 0; i < npieces; i++) {
        pieces[i].free_after = 0;
        if (ndead == cap_dead) dead = realloc(dead, sizeof *dead * (size_t)(cap_dead = cap_dead ? cap_dead * 2 : 256));
        dead[ndead++] = pieces[i];
    }
    npieces = 0, pool_bytes = 0;
    reap_dead(true);
    for (uint64_t i = 0; i < HEAP_MAX >> PAGE_SHIFT_GM; i++) ptab[i] = zero_page;
    pthread_mutex_unlock(&pieces_lk);
}

// The piece holding [addr, addr + size) (false: not in a pool's GPU memory).
static bool heap_chunk_at(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    bool ok = false;
    pthread_mutex_lock(&pieces_lk);
    int lo = 0, hi = npieces;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (pieces[mid].at + pieces[mid].n <= addr) lo = mid + 1; else hi = mid;
    }
    if (lo < npieces && pieces[lo].at <= addr && addr + size <= pieces[lo].at + pieces[lo].n) {
        *buf = pieces[lo].buf, *off = addr - pieces[lo].at;
        ok = true;
    }
    pthread_mutex_unlock(&pieces_lk);
    return ok;
}
// What shaders are translated for: the bytes global memory covers, its pages (log2; 0: one buffer).
static uint64_t gmem_limit(void) { return HEAP_MAX; }
static int gmem_chunk_shift(void) { return ptab ? PAGE_SHIFT_GM : 0; }
#else
uint64_t renderer_share_heap(uint64_t lo, uint64_t size) { (void)lo, (void)size; return 0; }  // (VK_EXT_external_memory_host)
void backend_pool_created(uint64_t addr, uint64_t size) { (void)addr, (void)size; }
void backend_pool_destroyed(uint64_t addr, uint64_t size) { (void)addr, (void)size; }
static void pools_reset(void) {}
static bool heap_chunk_at(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    (void)addr, (void)size, (void)buf, (void)off;
    return false;
}
static uint64_t gmem_limit(void) { return HEAP_MAX; }
static int gmem_chunk_shift(void) { return 0; }
#endif

static VkDescriptorBufferInfo gmem_info(void) {
#ifdef __ANDROID__
    if (ptab) return (VkDescriptorBufferInfo){ptab_buf, 0, (HEAP_MAX >> PAGE_SHIFT_GM) * 8};  // (the pools' page table)
#endif
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VK_NO_GMEM") != NULL || (!g_vk.host_import && !g_vk.heap_export);
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    static int64_t cap_mb = -1;  // LARECOMP_VK_GMEM_MB=<n>: only the heap's first n MB (what Android's 1 GB share gives)
    if (cap_mb < 0) cap_mb = getenv("LARECOMP_VK_GMEM_MB") ? atoll(getenv("LARECOMP_VK_GMEM_MB")) : 0;
    if (cap_mb > 0 && want > (uint64_t)cap_mb << 20) want = (uint64_t)cap_mb << 20;
    if (!off && want > gmem_size && !gmem_import(want)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// Sorted, merged guest memory intervals; the render thread is the only user. rs_note returns whether the set changed.
typedef struct { struct { uint64_t a, e; } r[512]; int n; } RangeSet;
static bool rs_note(RangeSet *s, uint64_t a, uint64_t n) {
    uint64_t e = a + (n ? n : 16);
    int i = 0, j;
    while (i < s->n && s->r[i].e < a) i++;
    for (j = i; j < s->n && s->r[j].a <= e; j++) {
        if (s->r[j].a < a) a = s->r[j].a;
        if (s->r[j].e > e) e = s->r[j].e;
    }
    if (j > i) {  // merged with [i, j)
        if (i + 1 == j && s->r[i].a == a && s->r[i].e == e) return false;  // (known)
        s->r[i].a = a; s->r[i].e = e;
        memmove(&s->r[i + 1], &s->r[j], (size_t)(s->n - j) * sizeof s->r[0]);
        s->n -= j - i - 1;
    } else {
        if (s->n == 512) { if (s->r[s->n - 1].e < e) s->r[s->n - 1].e = e; return true; }  // (full: widen the last, stays safe)
        memmove(&s->r[i + 1], &s->r[i], (size_t)(s->n - i) * sizeof s->r[0]);
        s->r[i].a = a; s->r[i].e = e;
        s->n++;
    }
    return true;
}
static bool rs_overlaps(const RangeSet *s, uint64_t a, uint64_t n) {
    uint64_t e = a + n;
    int lo = 0, hi = s->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s->r[mid].e <= a) lo = mid + 1; else hi = mid;
    }
    return lo < s->n && s->r[lo].a < e;
}

// Guest memory ranges the GPU may write: the storage buffers a compute dispatch binds (the game's compute shaders produce vertex
// data for some draws: a 16-byte stream here, a few more there). Vertex or index data inside one of them is read in place when
// the GPU runs, as the game expects, and is never copied when the draw is recorded (the copy would hold what the GPU has not
// written yet).
static RangeSet gpuw;
static void gpuw_note(uint64_t a, uint64_t n) {
    if (rs_note(&gpuw, a, n) && ENV("LARECOMP_LOG_GPUWRITES"))
        LOG("[gpuw] the GPU may write %llx-%llx (%d ranges)\n", (unsigned long long)a, (unsigned long long)(a + (n ? n : 16)), gpuw.n);
}
// A shader that stores to global memory with no storage buffer bound writes through pointers we cannot see: from then on every
// vertex / index range is read in place, as before the VRAM ring (slower, never stale).
static bool gpuw_unknown;
static void gpuw_stores(const ShaderInfo *si, const Range *sbufs, int n, const char *what, uint64_t code) {
    if (!si || !si->gmem_stores) return;
    bool any = false;
    for (int i = 0; i < n; i++) if (sbufs[i].addr) { gpuw_note(sbufs[i].addr, sbufs[i].size); any = true; }
    if (!any && !gpuw_unknown) {
        gpuw_unknown = true;
        LOG("[vk] %s %llx stores to global memory without a storage buffer: vertex and index data is read in place from now on\n", what, (unsigned long long)code);
    }
}
static bool gpuw_overlaps(uint64_t a, uint64_t n) { return gpuw_unknown || rs_overlaps(&gpuw, a, n); }

// Guest memory ranges the GPU may read in place when it runs, not from a copy made when the draw was recorded: the storage
// buffers bound to shaders that use global memory (the imported heap, gmem_info). A shader using global memory with none bound
// reads through pointers we cannot see: from then on any range may be read in place.
static RangeSet gpur;
static bool gpur_unknown;
static void gpur_loads(const ShaderInfo *si, const Range *sbufs, int n) {
    if (!si || !si->uses_gmem) return;
    bool any = false;
    for (int i = 0; i < n; i++) if (sbufs[i].addr) { rs_note(&gpur, sbufs[i].addr, sbufs[i].size); any = true; }
    if (!any && !gpur_unknown) {
        gpur_unknown = true;
        LOG("[vk] a shader uses global memory without a storage buffer: buffer copies wait for the GPU from now on\n");
    }
}
static bool gpur_overlaps(uint64_t a, uint64_t n) { return gpur_unknown || rs_overlaps(&gpur, a, n); }

// Vertex / index data read in place from the imported guest heap (renderer.m bind_guest does the same with
// g_guest_buffer): no per-draw copy, no index scan for the copy size. The game fences its CPU writes as on the
// Switch. Falls back to copies outside the heap or without the import; LARECOMP_VK_COPY_VERTICES=1 forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
    if (g_vk.heap_export) return heap_chunk_at(addr, size, buf, off);
    if (addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) return false;
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}
// Vertex / index range of a draw: in place from the game's memory (true) or to be copied (false). With the VRAM ring on, copied
// unless a compute dispatch may write it.
volatile int g_heal_tex, g_heal_vtx;  // (LARECOMP_HEAL_KEYS research keys, runtime/hle/input.c)
static bool vtx_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (g_heal_vtx) gpuw_unknown = true;
    if (ring_gpu_enabled() && !gpuw_overlaps(addr, size)) { stat_vtx_copied++; return false; }
    bool d = guest_direct(addr, size, buf, off);
    if (d) stat_vtx_direct++;
    return d;
}
bool vk_guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {  // (cbuf_info, above)
    return guest_direct(addr, size, buf, off);
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    g_cs_n = 0;
    gpuw.n = gpur.n = 0;  // (guest memory is replaced)
    keep_drop_all();
    keep_heap = 0;
    gcopy_gen++;
    if (gmem_buf && !g_vk.heap_export) {  // (Android's shared heap stays: renderer_share_heap maps it again)
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    if (!g_vk.heap_export) gmem_size = 0;
    gmem_tried = 0;
    pools_reset();
    st_defaults();
}

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    NvnTexture *tx = NULL;  // (the texture bound, for the mip bias below)
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        tx = e->tex;
        if (e->tex) {
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. LARECOMP_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = getenv("LARECOMP_VK_NO_TEX_MEMO") != NULL;
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)e->tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(e->tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = e->tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(e->tex, &sc[0], &sc[1]);
            wh[0] = e->tex->width; wh[1] = e->tex->height;
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_view[k];
                    if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);  // (2D snapshot as array: rare)
                }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) {
        NvnSamplerDesc *sd = &((NvnSamplerDesc *)st.spool->entries)[tsc];
        // The mip bias of TAA / FSR 3.1 (mip_bias): the 3D draws' textures the game loaded (not its render targets: a
        // mip chain read at an explicit level - bloom, the depth pyramid - would shift by it too)
        TexBackend *tb = tx ? (TexBackend *)tx->backend : NULL;
        if (g_mip_bias != 0 && st.rt.depth && st.ds[0] && tb && !tb->gpu_owned && tb->levels > 1 && !sd->compare_mode)
            smp = vk_sampler_biased(sd, g_mip_bias);
        else smp = vk_sampler(sd);
    }
    // a depth-compare lookup without a depth texture (the game's colour stand-in when it has no shadow map): a depth one
    // at 1.0 (vk_dummy_depth_view; a compare on a colour format is undefined in Vulkan)
    if (st_->shadow && (!view || !tx || !renderer_format_is_depth(tx->format))) view = vk_dummy_depth_view(st_->type);
    if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii) {
    VkDescriptorSet ds = vk_alloc_set(layout);
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    float rs[65][4] = {{1, 1, 0, 0}};
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding >= 100 && b->binding < 118) {
            bi[n] = mvo_prev_info(stage, si, (int)b->binding - 100, gx, gy, gz);  // (motion vectors: the previous frame's)
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->binding < 18 || b->binding > 21) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == 20) {  // (TAA: this frame's jitter)
            bi[n] = ubo_span(vk_transient_copy(taa_jitter(), 16, 256), 16);
        } else if (b->binding == 21) {  // (wide screens: ShaderKey.widen's scale)
            extern float widescreen_aspect(void);
            float wa = widescreen_aspect(), wv[4] = {wa > 1.0f ? (16.0f / 9.0f) / wa : 1.0f, (float)g_wide_ui, 0, 0};
            bi[n] = ubo_span(vk_transient_copy(wv, 16, 256), 16);
        } else if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++) { rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; }
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;

static void build_compute(void *arg) {
    CsProg *cp = arg;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { atomic_store(&cp->p.state, 2); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&stat_compiles, 1);
    if (r == VK_SUCCESS) atomic_fetch_add(&new_pipes, 1);
    atomic_store(&cp->p.state, r == VK_SUCCESS ? 1 : 2);
    if (r != VK_SUCCESS) LOG("[compute] pipeline failed: %d\n", r);
}

static struct CsEnt { uint64_t addr, content; CsProg *cp; } g_cs_tab[256];  // compute programs by code address and content

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr && tab[i].content == ss->content_hash) return tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    if (n < 256) { tab[n].addr = ss->code_addr; tab[n].content = ss->content_hash; tab[n].cp = cp; g_cs_n = n + 1; }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = gmem_limit();
    maxwell_gmem_chunk_shift = gmem_chunk_shift();
    ShaderKey k = {0};
    k.glsl = 1;
    cp->si = maxwell_translate((const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, STAGE_COMPUTE, &k);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)ss->code_addr);
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    run_job(build_compute, cp);
    return cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (!cp || atomic_load(&cp->p.state) != 1) { stat_skipped++; return; }
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u)", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2]);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    gpuw_stores(cp->si, st.sbuf[5], 16, "compute shader", st.program->stages[5].code_addr);
    gpur_loads(cp->si, st.sbuf[5], 16);
    for (int i = 0; i < 16; i++) if (st.sbuf[5][i].addr) gpuw_note(st.sbuf[5][i].addr, st.sbuf[5][i].size);  // (any storage buffer it has bound)
    nfeedback = 0;
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    stat_dispatches++;
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

static void draw_(CmdRec *r, bool indexed);
static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    draw_(r, indexed);
    PROF_ADD(prof_draw);
}
// A draw whose program or pipeline is still being built waits for it, up to a budget per frame, instead of being
// skipped (objects popping in on entering an area). At 30 fps a frame has ~27 ms to spare (uncapped, the overworld
// runs at ~160 fps), so 20 ms per frame hides the pop-in with a warm cache (~1 ms per program) without costing frames;
// a cold cache (first visit) spends the budget and skips the rest as before. LARECOMP_SHADER_WAIT_MS=<ms> (0: never).
// The budget follows the frame: 60 % of it (20 ms at 30 fps; 10 at 60, 5 at 120) - a fixed 20 ms made every frame with
// a new pipeline 25-30 ms long at 60 fps (most of the 1 % lows walking into new places).
static bool wait_built(_Atomic int *state) {
    if (atomic_load(state) != 0) return true;
    static int64_t fixed = -2;
    if (fixed == -2) fixed = getenv("LARECOMP_SHADER_WAIT_MS") ? atoi(getenv("LARECOMP_SHADER_WAIT_MS")) * 1000000ll : -1;
    extern uint64_t fps60_period_ns(void);
    int64_t budget = fixed >= 0 ? fixed : (int64_t)(fps60_period_ns() * 3 / 5);
    if (sync_shaders()) return true;  // (built already)
    extern uint64_t vk_frame_counter(void);
    static uint64_t frame;
    static int64_t spent;
    if (vk_frame_counter() != frame) { frame = vk_frame_counter(); spent = 0; }
    if (spent >= budget) return false;
    uint64_t t0 = SDL_GetTicksNS();
    while (atomic_load(state) == 0 && spent + (int64_t)(SDL_GetTicksNS() - t0) < budget) SDL_DelayNS(100000);
    uint64_t dt = SDL_GetTicksNS() - t0;
    spent += (int64_t)dt;
    stat_shader_wait_ns += dt;
    return atomic_load(state) != 0;
}

static uint64_t prof_tp_;
static void ui_snapshot_check(Prog *pr);  // (FSR 3's interface mask)

// A GL-style perspective matrix at m (row-major: the rows of P; column-major: its columns): P00, |P11|, A, B.
static bool ssao_proj_at(const float *m, float out[4]) {
    if (m[1] == 0 && m[3] == 0 && m[4] == 0 && m[7] == 0 && m[12] == 0 && m[13] == 0 && m[14] == -1 && m[15] == 0 &&
        m[0] > 0 && m[5] != 0 && m[10] < 0 && m[11] < 0) {
        out[0] = m[0], out[1] = fabsf(m[5]), out[2] = m[10], out[3] = m[11];
        return true;
    }
    if (m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 0 && m[6] == 0 && m[7] == 0 && m[11] == -1 && m[12] == 0 && m[13] == 0 &&
        m[15] == 0 && m[0] > 0 && m[5] != 0 && m[10] < 0 && m[14] < 0) {
        out[0] = m[0], out[1] = fabsf(m[5]), out[2] = m[10], out[3] = m[14];
        return true;
    }
    return false;
}

// The last two real frames' cameras (not the in-between images'): view-projection, projection, scene block
typedef struct { double vp[16]; float proj[4]; uint8_t blk[sizeof ao.scene[0]]; uint32_t n; uint64_t fr, real; bool ok; } CamReal;
static CamReal cam_real[3];
static int cam_ri;
static void mv_set_camera(const double vp[16], const uint8_t *blk, uint32_t n, uint64_t fr);
static void mv_replay_camera(uint64_t fr);

// The camera of this frame, from the projection found at p (row-major): the view matrix right before it (Let's Go's
// scene block: view +0, projection +64, view-projection +128), checked to be a rigid transform. vp = P x V, and the
// previous frame's kept when it is the frame just before (motion vectors: scene_hook_run). blk: the scene block.
static void mv_capture_camera(const float *p, const uint8_t *blk, uint32_t blk_size, uint64_t fr) {
    if (ao.vp_frame == fr || ao.poff < 64) return;
    const float *v = p - 16;
    bool rigid = v[12] == 0 && v[13] == 0 && v[14] == 0 && v[15] == 1;
    for (int r = 0; r < 3 && rigid; r++) {
        double l = (double)v[r * 4] * v[r * 4] + (double)v[r * 4 + 1] * v[r * 4 + 1] + (double)v[r * 4 + 2] * v[r * 4 + 2];
        rigid = fabs(l - 1) < 1e-3;
    }
    static bool warned;
    if (!rigid) {
        if (!warned) { warned = true; LOG("[mv] no rigid view matrix before the projection: no motion vectors\n"); }
        ao.prev_ok = false;
        return;
    }
    double vp[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += (double)p[r * 4 + k] * v[k * 4 + c];
            vp[r * 4 + c] = s;
        }
    uint32_t n = blk ? (blk_size < sizeof ao.scene[0] ? blk_size : (uint32_t)sizeof ao.scene[0]) : 0;
    {   // LARECOMP_CAM_LOG=1 (research): the camera's position in the world (cm) each frame - it follows the player,
        // so its speed is the walking speed (tools/walk_speed.sh: the same at 30, 60 and 120 fps?)
        static int cam_log = -1;
        if (cam_log < 0) cam_log = getenv("LARECOMP_CAM_LOG") != NULL;
        if (cam_log)  // (row-major rigid view: position = -R^T t)
            LOG("[cam] present %llu pos %.2f %.2f %.2f\n", (unsigned long long)renderer_present_count(),
                -(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]), -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]),
                -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11]));
    }
    {   // (a real frame's: the in-between image after it blends this one with the one before)
        CamReal *c = &cam_real[cam_ri = (cam_ri + 1) % 3];
        memcpy(c->vp, vp, sizeof vp);
        memcpy(c->proj, ao.proj, sizeof c->proj);
        if (n) memcpy(c->blk, blk, n);
        c->n = n, c->fr = fr, c->real = renderer_present_count(), c->ok = true;
    }
    mv_set_camera(vp, blk, n, fr);
}

// This image's camera: vp, and the scene block (blk, n bytes) for the objects without previous constants.
// The previous image of a real frame is the real frame before (the in-between images are side images: their camera is
// put back after each, ip_cam_restore); an in-between image's previous is N, the frame they are all made after.
static void mv_set_camera(const double vp[16], const uint8_t *blk, uint32_t n, uint64_t fr) {
    ao.prev_ok = ip_replaying ? ao.vp_frame != 0 : ao.vp_frame && ao.vp_real + 1 == renderer_present_count();
    ao.vp_real = renderer_present_count();
    memcpy(ao.vp_prev, ao.vp, sizeof ao.vp);
    memcpy(ao.vp, vp, sizeof ao.vp);
    ao.vp_frame = fr;
    ao.scene_cur ^= 1;
    if (n) memcpy(ao.scene[ao.scene_cur], blk, n);
    ao.scene_size[ao.scene_cur] = n;
}

// A word that jumps between N-1 and N (a cut, a slot reused): no blend. More than half its size and 100 units: a
// translation near 0 that changes sign is motion, not a jump - with 4 units the player's skeleton (in the middle of the
// screen, where the camera's coordinates are small) jumped in a quarter of its words, was kept at N whole, and the
// player moved at the game's rate in the in-between images (2x at 30: the image beside N, not halfway). Rotations (|v|
// <= 1) never jump; cuts and swapped models are caught per object and per frame (vk_interp_begin).
// LARECOMP_INTERP_JUMP_ABS=<units> (research).
static float ip_jump_abs = -1;
static bool ip_jumps(float a, float b) {
    if (ip_jump_abs < 0) ip_jump_abs = getenv("LARECOMP_INTERP_JUMP_ABS") ? (float)atof(getenv("LARECOMP_INTERP_JUMP_ABS")) : 100.0f;
    return fabsf(b - a) > 0.5f * (fabsf(a) + fabsf(b)) + ip_jump_abs;
}

// Frame N-2, N-1 and N three presents in a row at the game's same rate (vk_interp_begin): the in-between images follow
// the curve through the three (ip_cbuf, mv_replay_camera). LARECOMP_INTERP_CURVE=0 (comparison): straight from N-1 to N.
static bool ip_curve_ok;  // (three frames in a row and the curve on)
static bool ip_three_ok;  // (three frames in a row: N-2 known - ip_cbuf's alternating values)
static uint64_t ip_stats_curved;  // (words blended along the curve)
static uint64_t ip_stats_alt;     // (words alternating between two values: kept at N)
static uint64_t ip_stats_other_copy;  // (draws whose closest partner by content was another copy: kept at N)
static uint64_t ip_stats_taken;       // (draws whose partner was closer to another draw: kept at N, ip_pair_all)
static uint64_t ip_stats_sib;         // (cbufs of draws without a partner whose shared words followed a sibling)
static uint64_t ip_stats_sibw[4];     // (research: no sibling, with one, its cbuf not found, nothing shared moved)
// The quadratic through c (N-2, at -1), a (N-1, at 0) and b (N, at 1), at t
static float ip_curve3(float c, float a, float b, float t) { return a + t * (b - c) * 0.5f + t * t * (b - 2.0f * a + c) * 0.5f; }
// ... for a word that moved in both steps by a smooth change: its acceleration (b - 2a + c) at most a quarter of the two
// steps. Checked against the truth (a walk with turns at 120 fps, its camera; every 4th / 2nd frame taken as 30 / 60
// fps and the frames between predicted): with any acceleration the curve was better on average (-13 %) but worse in its
// worst 5 % at 60 (0.24 -> 0.40 cm: a step in the camera's follow is not a curve); within a quarter, never worse - at 30
// x 4 mean 0.203 -> 0.196 cm, 99th percentile 2.82 -> 2.55; at 60 x 2 mean 0.103 -> 0.090, the rest the same.
static bool ip_curve_fits(float c, float a, float b) {
    return a != b && c != a && fabsf(b - 2.0f * a + c) <= 0.25f * (fabsf(b - a) + fabsf(a - c));
}
// A blended word as the replay's (ip_cbuf): halfway (ip_t) between a and b unless it jumps (a cut: b)
static float ip_lerp(float a, float b, bool *jump) {
    if (a == b) return b;
    if (!isfinite(a) || !isfinite(b) || ip_jumps(a, b)) { *jump = true; return b; }
    return a + (b - a) * ip_t;
}

// The in-between image's camera: frame N's (the image rendered just before it) blended with N-1's - its draws take
// their constant buffers blended the same way. A jump between the two (a cut) keeps N's. No camera (no motion vectors,
// no occlusion) unless N came right before.
static void mv_replay_camera(uint64_t fr) {
    const CamReal *nw = &cam_real[cam_ri], *od = &cam_real[(cam_ri + 2) % 3], *o2 = &cam_real[(cam_ri + 1) % 3];
    // (the in-between images of a present come after N, at its present: N is this real frame, N-1 the one before it)
    if (!nw->ok || nw->real != renderer_present_count() || fr <= nw->fr) return;
    bool both = od->ok && od->real + 1 == nw->real;
    double vp[16];
    float proj[4];
    static uint8_t blk[sizeof ao.scene[0]];
    memcpy(vp, nw->vp, sizeof vp);
    memcpy(proj, nw->proj, sizeof proj);
    memcpy(blk, nw->blk, nw->n);
    if (both) {
        bool jump = false;
        double v2[16];
        float p2[4];
        for (int i = 0; i < 16; i++) v2[i] = ip_lerp((float)od->vp[i], (float)nw->vp[i], &jump);
        for (int i = 0; i < 4; i++) p2[i] = ip_lerp(od->proj[i], nw->proj[i], &jump);
        // (along the curve through N-2 too, as the draws' constants are: ip_cbuf)
        bool three = ip_curve_ok && o2->ok && o2->real + 2 == nw->real;
        for (int i = 0; i < 16 && three && !jump; i++)
            if (ip_curve_fits((float)o2->vp[i], (float)od->vp[i], (float)nw->vp[i]) && !ip_jumps((float)o2->vp[i], (float)od->vp[i]))
                v2[i] = ip_curve3((float)o2->vp[i], (float)od->vp[i], (float)nw->vp[i], ip_t);
        if (!jump) {
            memcpy(vp, v2, sizeof vp);
            memcpy(proj, p2, sizeof proj);
            if (od->n == nw->n)  // (float words a vec4 at a time, as ip_cbuf: integers and jumps keep N)
                for (uint32_t g = 0; g + 16 <= nw->n; g += 16) {
                    float a[4], b[4], c[4], o[4];
                    memcpy(a, od->blk + g, 16);
                    memcpy(b, nw->blk + g, 16);
                    bool j = false, cv = three && o2->n == nw->n && memcmp(od->blk + g, o2->blk + g, 16) != 0;
                    if (cv) memcpy(c, o2->blk + g, 16);
                    for (int i = 0; i < 4; i++) {
                        uint32_t ua, ub;
                        memcpy(&ua, &a[i], 4);
                        memcpy(&ub, &b[i], 4);
                        if (ua != ub && (((ua >> 23) & 0xFF) == 0xFF || ((ub >> 23) & 0xFF) == 0xFF)) j = true;
                        if (cv && a[i] != b[i] && (!isfinite(c[i]) || ip_jumps(c[i], a[i]) || !ip_curve_fits(c[i], a[i], b[i]))) cv = false;
                    }
                    for (int i = 0; i < 4; i++)
                        o[i] = cv && a[i] != b[i] && isfinite(a[i]) && isfinite(b[i]) && !ip_jumps(a[i], b[i]) ? ip_curve3(c[i], a[i], b[i], ip_t)
                                                                                                             : ip_lerp(a[i], b[i], &j);
                    if (!j) memcpy(blk + g, o, 16);
                }
        }
    }
    memcpy(ao.proj, proj, sizeof proj);
    ao.proj_frame = fr;
    {   // LARECOMP_CAM_LOG=1: the in-between images' camera too (the view in the blended scene block), at their t
        static int cam_log = -1;
        if (cam_log < 0) cam_log = getenv("LARECOMP_CAM_LOG") != NULL;
        if (cam_log && nw->n >= 64) {
            float v[16];
            memcpy(v, blk, sizeof v);
            LOG("[cam] present %llu t %.3f pos %.2f %.2f %.2f\n", (unsigned long long)renderer_present_count(), ip_t,
                -(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]), -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]),
                -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11]));
        }
    }
    mv_set_camera(vp, blk, nw->n, fr);
}

// A camera whose view (the 64 bytes before the projection) does not move the world: at the origin, as a sky's. Not the
// scene's. (In battles, every few frames the scene draws' slot holds one - all of their draws that frame - and the
// scene's camera is nowhere as a projection of its own: the occlusion and the indirect light read the depth with the
// wrong camera in those frames and the right one in the others, and flickered. Seen at the end of a trainer battle.)
static bool cam_at_origin(const float *p) {
    const float *v = p - 16;
    return fabsf(v[3]) < 1 && fabsf(v[7]) < 1 && fabsf(v[11]) < 1;
}

// The scene's projection, from the vertex constant buffers of its draws: where it was last found, else a scan (once
// per frame). Let's Go: row-major, near 32, far 10000 (cm).
static void ssao_find_projection(void) {
    uint64_t fr = fx_frame();
    if (ao.proj_frame == fr) return;
    if (ao.pbind >= 0) {
        Range u = st.ubuf[0][ao.pbind];
        const uint8_t *blk = (const uint8_t *)(uintptr_t)u.addr;
        uint32_t bsz = (uint32_t)u.size;
        if (ip_replaying) {  // (the in-between image: N-1's and N's cameras blended, as its draws are)
            mv_replay_camera(fr);
            return;
        }
        float proj[4];
        const uint8_t *wb = wide_block(blk, bsz);  // (wide screens: the camera as the draws have it)
        if (wb) blk = wb;
        if (blk && ao.poff + 64 <= bsz && ssao_proj_at((const float *)(blk + ao.poff), proj) &&
            !cam_at_origin((const float *)(blk + ao.poff))) {
            memcpy(ao.proj, proj, sizeof ao.proj);
            ao.proj_frame = fr;
            mv_capture_camera((const float *)(blk + ao.poff), blk, bsz, fr);
            return;
        }
    }
    if (ip_replaying) return;  // (no scan there)
    if (ao.scan_frame == fr) return;
    ao.scan_frame = fr;
    float proj[4];
    for (int b = 0; b < 18; b++) {
        Range u = st.ubuf[0][b];
        if (!u.addr || u.size < 64) continue;
        uint64_t n = u.size > 4096 ? 4096 : u.size;
        for (uint32_t o = 64; o + 64 <= n; o += 16)
            if (ssao_proj_at((const float *)(uintptr_t)(u.addr + o), proj) && !cam_at_origin((const float *)(uintptr_t)(u.addr + o))) {
                if (ao.pbind != b || ao.poff != o) LOG("[ssao] projection in vertex cbuf %d at %u: near %.3g far %.3g\n", b, o,
                                                      proj[3] / (proj[2] - 1), proj[3] / (proj[2] + 1));
                memcpy(ao.proj, proj, sizeof ao.proj);
                ao.pbind = b, ao.poff = o, ao.proj_frame = fr;
                mv_capture_camera((const float *)(uintptr_t)(u.addr + o), (const uint8_t *)(uintptr_t)u.addr, (uint32_t)u.size, fr);
                return;
            }
    }
}

// As the scene pass ends: no camera this frame (above) - the last one, if recent, standing still
static void ssao_settle_camera(void) {
    uint64_t fr = fx_frame();
    if (ao.proj_frame == fr || !ao.vp_frame || ao.vp_frame >= fr || ao.vp_frame + 8 < fr) return;
    ao.proj_frame = fr;
    mv_set_camera(ao.vp, ao.scene[ao.scene_cur], ao.scene_size[ao.scene_cur], fr);
}

// Each draw in the scene pass: opaque ones are counted (and give the projection); the occlusion runs when the pass
// ends (vk_end_pass), with all of them drawn. (It ran at the first blended draw after 16 opaque ones: Let's Go draws
// blended ones among its opaque ones, and how many opaque draws came before the first changes with the view - 34 to 68
// of about 130 walking in Pewter's gym - so the objects drawn after it went in and out of the occlusion and the
// indirect light from one frame to the next: rocks and characters flickered as the camera moved.) A draw without the
// depth test after them is no longer the 3D scene - a fade drawn over it (the title screen's from white), the game's
// full-screen passes: the occlusion goes in before it (at the pass's end it went onto the fade too: grass showed
// through the title's white). True: the pass must begin again.
static bool ssao_draw_hook(void) {
    if (ao.busy || ao.done_frame == fx_frame()) return false;
    if (!(st.color_state[0] & 1) && st.ds[0] && st.ds[1]) { ao.opaque++; ssao_find_projection(); }
    if (st.ds[0] || ao.opaque < 16) return false;
    vk_end_pass();
    return true;
}

// The index and vertex buffers frame N's draws were given (its copies of them in the ring, or the game's heap in place),
// by the draw's place in the frame: the replays run the same command lists, and took the same ranges again - the index
// scans and the copies (0.8 ms of a replay's 2.9 on the CPU, 30 fps x 3, Pallet Town) - from memory the game may be
// writing for the next frame by then. A replay's draw binds N's instead, while their ring chunks were not taken again.
// LARECOMP_INTERP_VTX_MEMO=0 (comparison): the replays copy as before.
typedef struct { uint32_t gen, used, icount; bool idx; VkBuffer ib; VkDeviceSize ioff; VkIndexType itype;
                 int ick; uint32_t iep; VkBuffer vb[17]; VkDeviceSize voff[17]; int vck[17]; uint32_t vep[17]; } VtxMemo;
static VtxMemo *vtx_memo;
static uint32_t vtx_memo_cap, vtx_memo_gen = 1;
static uint64_t vtx_memo_hits, vtx_memo_miss;
static bool vtx_memo_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_INTERP_VTX_MEMO") ? atoi(getenv("LARECOMP_INTERP_VTX_MEMO")) : 1;
    return on && ip_enabled() && !ip_vtx_draw;
}
static VtxMemo *vtx_memo_at(uint32_t i, bool grow) {
    if (i >= vtx_memo_cap) {
        if (!grow) return NULL;
        uint32_t nc = i < 4096 ? 8192 : i * 2;
        vtx_memo = realloc(vtx_memo, nc * sizeof *vtx_memo);
        memset(vtx_memo + vtx_memo_cap, 0, (nc - vtx_memo_cap) * sizeof *vtx_memo);
        vtx_memo_cap = nc;
    }
    return &vtx_memo[i];
}
// A vertex stream bound, and (frame N's draws: vm) kept for the replays
static void vb_bind(VkCommandBuffer cb, int s, VkBuffer b, VkDeviceSize off, VtxMemo *vm) {
    vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &b, &off);
    if (vm) vm->vb[s] = b, vm->voff[s] = off, vm->vck[s] = span_chunk(b, &vm->vep[s]);
}

static void draw_(CmdRec *r, bool indexed) {
    if (prof) prof_tp_ = SDL_GetTicksNS();
    mvo_draw = 0;
    if (!st.program) { SKIP(0); return; }
    if (ip_enabled()) ip_draw_ord = ip_ord++;  // (the replays run the same lists: the n-th draw of one is frame N's n-th)
    if (ip_enabled()) {  // (frame interpolation: what identifies this draw from one frame to the next)
        // what the draw is (program, vertex buffer, parameters), and which game object it belongs to: the same mesh
        // drawn for two objects (bushes, Pidgeys) is told apart by the object, not by the order the game drew them in
        uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)(uintptr_t)st.program ^ ((uint64_t)indexed << 63);
        h = (h ^ st.vbuf[0].addr) * 0x100000001B3ull;
        for (int i = 0; i < draw_nargs(indexed); i++) h = (h ^ r->x[i]) * 0x100000001B3ull;
        if (st.obj) h = (h ^ st.obj) * 0x100000001B3ull, ip_stats_tagged++;
        else ip_stats_untagged++;
        ip_draw_sig = h;
        uint64_t sh = 0x84222325cbf29ce4ull ^ (uint64_t)(uintptr_t)st.program ^ ((uint64_t)indexed << 63);
        for (int i = 0; i < draw_nargs(indexed); i++) sh = (sh ^ r->x[i]) * 0x100000001B3ull;
        ip_draw_shape = sh;
        ip_draw_occ = ip_occ_next(h);
        ip_part_set = false, ip_sib = -1;
        {   // LARECOMP_INTERP_SIG_LOG=<present>:<cbuf size>: what identifies each draw binding a cbuf of that size, at that
            // present and the next (research: draws that find no match in the previous frame)
            static long at = -2, sz;
            if (at == -2) { const char *e = getenv("LARECOMP_INTERP_SIG_LOG"); at = e ? atol(e) : -1; sz = e && strchr(e, ':') ? atol(strchr(e, ':') + 1) : 0; }
            ip_sig_dbg = at >= 0 && !ip_replaying && (long)renderer_present_count() >= at && (long)renderer_present_count() <= at + 1 ? sz : 0;
            static int farlog = -1;
            if (farlog < 0)
                farlog = getenv("LARECOMP_INTERP_FAR_LOG") != NULL || getenv("LARECOMP_INTERP_KEPT_LOG") != NULL ||
                         getenv("LARECOMP_INTERP_KEEP_SIG") != NULL;
            if (ip_sig_dbg || (farlog && ip_replaying)) {
                ip_sig_txt_n = snprintf(ip_sig_txt, sizeof ip_sig_txt, "prog %p vbuf %#llx obj %#llx args", (void *)st.program,
                                        (unsigned long long)st.vbuf[0].addr, (unsigned long long)st.obj);
                for (int i = 0; i < draw_nargs(indexed) && ip_sig_txt_n < (int)sizeof ip_sig_txt - 24; i++)
                    ip_sig_txt_n += snprintf(ip_sig_txt + ip_sig_txt_n, sizeof ip_sig_txt - ip_sig_txt_n, " %llx", (unsigned long long)r->x[i]);
            }
        }
        ip_pick_set = false;  // (ip_cbuf: this draw's previous occurrence, picked at its first cbuf)
        ip_kept_why = 0;
        // 3D draws (a depth target with the depth test on) are blended, with or without a game object: the ground and the
        // scenery have none, and they must take the same (blended) light matrices as the shadow map the objects are drawn
        // into, or shadows slide away from their casters. Interface and full-screen passes don't test depth: frame N.
        ip_draw_obj = st.rt.depth != NULL && st.ds[0];
        { static int all = -1; if (all < 0) all = getenv("LARECOMP_INTERP_ALL3D") != NULL; if (all) ip_draw_obj = true; }  // (research)
        // In the 60 fps mode's menus (the Pokédex, the bag...) the 2D draws are blended too: lists and panels slide there.
        // A 2D draw takes the strict jump test in ip_cbuf (a recycled row or glyph moves far: it keeps frame N). Not with
        // frame generation: the interface stays as the game draws it, frame N's, in every in-between image.
        ip_draw_2d = !ip_draw_obj;
        extern bool fps60_menu(void);
        if (fps60_menu() && !g_settings.frame_gen) ip_draw_obj = true;
        static int vtx = -1;  // (LARECOMP_INTERP_VTX=1, research: ip_vtx_stream - off, it broke the tall grass)
        if (vtx < 0) vtx = getenv("LARECOMP_INTERP_VTX") ? atoi(getenv("LARECOMP_INTERP_VTX")) : 0;
        ip_vtx_draw = vtx && ip_draw_obj;
        if (!ip_replaying) ip_draw_record();
        else if (ip_draw_obj && !ip_draw_2d) ip_pair_draw();  // (3D: its partner by all its cbufs; else ip_pick, by order)
    }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.jitter = jitter_on() && st.rt.depth && st.rt.depth == ao.dtex && st.ds[0];  // (TAA, FSR 3)
    {   // wide screens: draws into the screen-shaped images (scalable: the scene, its depth, its effects' smaller ones)
        extern float widescreen_aspect(void);
        TexBackend *c0 = st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->backend : NULL;
        TexBackend *d0 = st.rt.depth ? st.rt.depth->backend : NULL;
        vkey.widen = widescreen_aspect() > 1.0f && ((c0 && (c0->scalable || c0->presented)) || (d0 && d0->scalable));
    }
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (pr) wait_built(&pr->state);
    if (!pr || atomic_load(&pr->state) != 1) { SKIP(pr ? 1 + (atomic_load(&pr->state) > 1) : 3); return; }
    {   // fragment-shader hash: interior backdrop detection (widescreen), skip lists, item twinkle (renderer.m)
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            last_fs = fs;
            last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
        }
        static uint64_t skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = getenv("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { stat_skipped++; return; }
        if (g_debug_frame && getenv("LARECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
    }
    {   // (research, LARECOMP_COUNT_TEX=<w>x<h>: per frame, the draws and instances sampling a fragment texture of that size)
        static int tw = -1, th;
        if (tw < 0) { const char *e = getenv("LARECOMP_COUNT_TEX"); tw = 0; if (e) sscanf(e, "%dx%d", &tw, &th); }
        if (tw > 0) {
            static uint64_t frame = ~0ull;
            static unsigned nd, ni;
                        if (renderer_present_count() != frame) {
                if (frame != ~0ull) LOG("[counttex] frame %llu: %u draws %u instances\n", (unsigned long long)frame, nd, ni);
                frame = renderer_present_count(), nd = ni = 0;
            }
            for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
                NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                if (t && (int)t->width == tw && (int)t->height == th) { nd++; ni += (unsigned)(indexed ? r->x[6] : r->x[4]); break; }
            }
        }
    }
    if (g_debug_frame) debug_draw(pr, r, indexed);
    else if ((pr->vi->attr_mask >> 11) & 1) {  // skinned draws (a11: bone indices)
        static int n;
        static int64_t dump_frame = -2;  // LARECOMP_DUMP_SKIN_FRAME: dump the first one from that frame on
        if (dump_frame == -2) dump_frame = getenv("LARECOMP_DUMP_SKIN_FRAME") ? atoll(getenv("LARECOMP_DUMP_SKIN_FRAME")) : -1;
        if (dump_frame >= 0 && renderer_present_count() >= (uint64_t)dump_frame) {
            debug_draw(pr, r, indexed);  // (debug_draw writes build/skin/ once)
            dump_frame = -1;
        } else if (getenv("LARECOMP_LOG_SKIN") && n++ % 50 == 0 && n < 1000) {
            debug_draw(pr, r, indexed);
        }
    }
    resolve_feedback(pr);
    // (FSR 3, DittoSR: the interface mask; frame generation: the real frames' mask, for their in-between images - ip_ui_keep)
    extern float widescreen_aspect(void);  // (wide screens: the interface's image, ui_scene_rt, for its centred 16:9 box)
    if ((fsr3_on() || gsr2_on() || (g_settings.frame_gen && ip_enabled()) || widescreen_aspect() > 1.0f) &&
        !ip_replaying)
        ui_snapshot_check(pr);
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    if (g_settings.contact_on) sun_probe();  // (the shadow map's light: the contact shadows' sun)
    begin_pass();
    if (ao.qual && ssao_draw_hook()) begin_pass();  // (the occlusion went in before this draw: the pass again)
    // per-object motion vectors: the scene pass's depth-tested draws that are opaque, or blended but writing depth (the
    // door and window frames, foliage cards: what is seen there is them), take the program's velocity variant
    bool wvel = ao.vel && st.ds[0] && (!(st.color_state[0] & 1) || st.ds[1]);
    if (wvel) {
        ShaderKey vk2 = vkey, fk2 = fkey;
        vk2.velocity = fk2.velocity = 1;
        fk2.vel_rt = (uint8_t)ao.vel_rt;
        Prog *pv = get_program(st.program, &vk2, &fk2);
        if (pv) wait_built(&pv->state);
        if (pv && atomic_load(&pv->state) == 1 && pv->vi->vel && pv->fi->vel) {
            pr = pv, mvo_draw = mvo_draw_key(r, indexed);
            mvo_check(pv->vi);
        } else wvel = false;
    }
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { SKIP(4); return; }

    uint64_t prof_tk_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_pre += prof_tk_ - prof_tp_;
    // ---- pipeline
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = pr;
    k.ncolor = pass_ncolor;
    // (only the pass's own slots: the ones past them keep what the last pass with more targets set, so a depth-only
    // pass's pipelines got several keys and were built again for each - LAS Recomp 3921201)
    for (uint32_t i = 0; i < pass_ncolor && i < 8; i++) k.color[i] = pass_color[i];
    k.depth = pass_depth;
    k.stencil = pass_stencil;
    k.samples = pass_samples;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr_bind[i] = ZERO_BINDING;
            k.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.attr_off[i] = off;
        k.attr_bind[i] = stream;
        k.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
        if (k.divisor[s] > 1 && !g_vk.divisor) k.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || k.color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
            k.blend_on |= (uint8_t)(1u << i);
            memcpy(k.blend[i], st.blend[i], 8);
        }
    }
    if (ao.vel && ao.vel_rt < 8) {  // (the velocity target: written by the velocity draws only)
        k.color[ao.vel_rt] = VK_FORMAT_R16G16B16A16_SFLOAT;
        k.write_mask[ao.vel_rt] = wvel ? 0xF : 0;
        k.blend_on &= (uint8_t)~(1u << ao.vel_rt);
    }
    k.a2c = st.ms[0xC] != 0;
    k.topo_class = cls;
    k.poly_fill = st.poly[2] != 1;
    if (pr->fi->uses_shpcf) k.shadow_q = (uint8_t)g_settings.shadow_filter;
    uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
    if (!ph) ph = 1;
    uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    while (pipe_tab[pslot].key && pipe_tab[pslot].key != ph) pslot = (pslot + 1) & (PIPE_TAB - 1);
    Pipe *pp = pipe_tab[pslot].pipe;
    if (!pp) {
        pp = calloc(1, sizeof *pp);
        pp->k = k;
        pipe_tab[pslot].key = ph;
        pipe_tab[pslot].pipe = pp;
        run_job(build_pipeline, pp);
    }
    wait_built(&pp->state);
    if (atomic_load(&pp->state) != 1) { SKIP(5 + (atomic_load(&pp->state) > 1)); return; }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe);
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("LARECOMP_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    vkCmdSetDepthTestEnable(cb, dt);
    vkCmdSetDepthWriteEnable(cb, dt && st.ds[1]);
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). LARECOMP_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("LARECOMP_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCmdSetDepthCompareOp(cb, dop);
    bool sten = st.ds[3] && pass_stencil;
    vkCmdSetStencilTestEnable(cb, sten);
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilOp(cb, face, stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                              compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF));
            vkCmdSetStencilCompareMask(cb, face, st.stencil_vmask[f] & 0xFF);
            vkCmdSetStencilWriteMask(cb, face, st.stencil_mask[f] & 0xFF);
            vkCmdSetStencilReference(cb, face, st.stencil_ref[f]);
        }
    }
    vkCmdSetBlendConstants(cb, st.blend_color);

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    vkCmdSetCullMode(cb, cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // LARECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("LARECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    vkCmdSetFrontFace(cb, front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(cb, st.prim_restart && native && (mode == 3 || mode == 5));
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        vkCmdSetDepthBiasEnable(cb, on);
        vkCmdSetDepthBias(cb, f[1], f[2], f[0]);
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    float tint[8] = {0};  // (the per-draw tint the translator can apply to colour target 0: none)
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale and the wide screens'
    // interface placement depend on the decision: both sets after it (stage_set fills tex_sc for stage 1).
    if (prof) prof_dyn += SDL_GetTicksNS() - prof_td_;
    g_wide_ui = 0;
    {   // wide screens: camera-attached effects sized for 16:9 stretched, not widened (ShaderKey.widen's 3) - the battle's
        // intro (the speed lines and the blurred layers in front of the camera: in a wider view their edges showed the
        // field behind); LARECOMP_WIDE_STRETCH=<fs hash>,... adds others (research). Depth-tested 3D otherwise widened
        // whatever its projection (4: the Pokedex's model is orthographic).
        extern bool widescreen_effect_stretch(uint64_t fs_hash);
        static char list[512] = {1};
        if (list[0] == 1) snprintf(list, sizeof list, "%s", getenv("LARECOMP_WIDE_STRETCH") ? getenv("LARECOMP_WIDE_STRETCH") : "");
        char h[20];
        snprintf(h, sizeof h, "%016llx", (unsigned long long)g_cur_fs_hash);
        if (vkey.widen && (widescreen_effect_stretch(g_cur_fs_hash) || (list[0] && strstr(list, h)))) g_wide_ui = 3;
        else if (vkey.widen && dt) g_wide_ui = 4;
    }
    bool wide_vp = false;  // (the interface placed by its vertex shader: the viewport the whole image's)
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
    }
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    // (wide screens: also the full-screen images the game draws its interface into before its last pass - two, one
    // frame each - their draws without a depth test; the 3D ones there, with the scene's depth, stay the scene's)
    TexBackend *rt0b = st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->backend : NULL;
    bool ui_rt = widescreen_aspect() > 1.0f && rt0b && rt0b->scalable && st.rt.color[0]->width >= 1280 && !dt;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && (renderer_texture_presented(st.rt.color[0]) || ui_rt)) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) {
            // (a scene image: not the interface's own 2x images and text cache, scaled too - runtime/ui2x.c, font_hires.c)
            NvnTexture *t0 = shader_texture_object(1, &pr->fi->tex[0]);
            if (t0 && t0->backend && ((TexBackend *)t0->backend)->scalable) reads_scene = true;
        }
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
            // wide screens, a draw over the whole screen (its viewport): placed by its vertex shader instead
            // (ShaderKey.widen) - in the box but for what touches the screen's sides; a background (a 16:9 image of
            // 480 or more, in one piece or several) covers the window. The box stays a smaller scissor's.
            bool full_vp = st.viewport[0][0] == 0 && st.viewport[0][1] == 0 && st.viewport[0][2] >= st.rt.color[0]->width &&
                           st.viewport[0][3] >= st.rt.color[0]->height;
            if (widescreen_aspect() > 1.0f && full_vp) {
                NvnTexture *t0 = pr->fi->ntex >= 1 ? shader_texture_object(1, &pr->fi->tex[0]) : NULL;
                bool img = t0 && t0->backend && !((TexBackend *)t0->backend)->scalable && tex_wh[1][0][0] >= 480 &&
                           fabsf((float)tex_wh[1][0][0] / (float)tex_wh[1][0][1] - 16.0f / 9.0f) < 0.05f;  // (not the scene's)
                g_wide_ui = img ? 2 : 1;
                wide_vp = true;
            }
        }
    }
    {   // (research, LARECOMP_COUNT_FS=<fs hash>: draws with that fragment shader per frame, logged when the count changes)
        static uint64_t want = 1, frame = ~0ull;
        static unsigned n, last = ~0u;
        if (want == 1) { const char *e = getenv("LARECOMP_COUNT_FS"); want = e ? strtoull(e, NULL, 16) : 0; }
        if (want) {
                        if (renderer_present_count() != frame) {
                if (frame != ~0ull && n != last) LOG("[countfs] frame %llu: %u\n", (unsigned long long)frame, n);
                if (frame != ~0ull) last = n;
                frame = renderer_present_count(), n = 0;
            }
            if (g_cur_fs_hash == want) n++;
        }
    }
    {
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        if (wide_vp) set_viewport(vp[0] * pass_sx, vp[1] * pass_sy, vp[2] * pass_sx, vp[3] * pass_sy, zn, zf);
        else set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f wide %d%s\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy, g_wide_ui, wide_vp ? " (full viewport)" : "");
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = prof ? SDL_GetTicksNS() : 0;
    gpuw_stores(pr->vi, st.sbuf[0], 16, "vertex shader", st.program->stages[0].code_addr);
    gpuw_stores(pr->fi, st.sbuf[1], 16, "fragment shader", st.program->stages[1].code_addr);
    gpur_loads(pr->vi, st.sbuf[0], 16);
    gpur_loads(pr->fi, st.sbuf[1], 16);
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL);
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii);
    g_wide_ui = 0;  // (this draw's: written; the next scissor - a clear's - is the box's again)
    if (prof) prof_sets += SDL_GetTicksNS() - prof_t1_;
    VkDescriptorSet sets[2] = {vset, fset};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, 0, NULL);

    uint64_t prof_t2_ = prof ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    VtxMemo *vm = vtx_memo_on() ? vtx_memo_at(ip_draw_ord, !ip_replaying) : NULL;
    bool vm_hit = false;
    if (vm && ip_replaying) {  // (frame N's own buffers, when still valid: vtx_memo)
        vm_hit = vm->gen == vtx_memo_gen && vm->used == k.used_streams && vm->idx == use_index && (!vm->idx || span_still(vm->ick, vm->iep));
        for (int s = 0; s < 17 && vm_hit; s++)
            if ((k.used_streams >> s) & 1) vm_hit = span_still(vm->vck[s], vm->vep[s]);
        if (vm_hit) {
            if (!indexed) first = (uint32_t)r->x[1], base_inst = (uint32_t)r->x[3], inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
            else base_vertex = (int32_t)r->x[4], base_inst = (uint32_t)r->x[5], inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
            icount = vm->icount;
            if (use_index) vkCmdBindIndexBuffer(cb, vm->ib, vm->ioff, vm->itype);
            for (int s = 0; s < 17; s++)
                if ((k.used_streams >> s) & 1) vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &vm->vb[s], &vm->voff[s]);
            vtx_memo_hits++;
        } else vtx_memo_miss++;
        vm = NULL;  // (nothing to keep from a replay)
    }
    if (vm) vm->gen = 0;  // (frame N's draw: kept below, once all of it is known)
    if (!vm_hit) {
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && vtx_direct(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = transient_gpu_copy(src, (uint64_t)count * isz + 4, 4);
                { uint32_t m = scan_max_index(src, isz, count, restart); if (m > max_index) max_index = m; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index) vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
    if (vm) vm->idx = use_index, vm->ib = ib.buf, vm->ioff = ib.off, vm->itype = itype_vk, vm->icount = icount,
            vm->ick = use_index ? span_chunk(ib.buf, &vm->iep) : -1;

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (ip_vtx_draw && ip_enabled() && v.addr && v.size && k.stride[s]) {  // (frame interpolation: ip_vtx_stream)
                if (scan_src) {
                    { uint32_t m = scan_max_index(scan_src, scan_isz, icount, scan_restart); if (m > max_index) max_index = m; }
                    max_index += 1;
                    if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                    scan_src = NULL;
                }
                uint64_t need = k.divisor[s] ? ((uint64_t)base_inst + inst + k.divisor[s] - 1) / k.divisor[s] * k.stride[s]
                                             : (uint64_t)max_index * k.stride[s];
                if (need > v.size) need = v.size;
                VkSpan bl;
                if (ip_vtx_stream(s, v.addr, need, k.stride[s], k.divisor[s] != 0, &bl)) {
                    vb_bind(cb, s, bl.buf, bl.off, vm);
                    continue;
                }
            }
            if (v.addr && v.size && vtx_direct(v.addr, v.size, &dbuf, &doff)) {
                vb_bind(cb, s, dbuf, doff, vm);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                { uint32_t m = scan_max_index(scan_src, scan_isz, icount, scan_restart); if (m > max_index) max_index = m; }
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = k.stride[s], need;
            if (!stride) need = 256;
            else if (k.divisor[s]) need = ((uint64_t)base_inst + inst + k.divisor[s] - 1) / k.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else {
                sp = vertex_copy(v.addr, need ? need : 16, v.size, k.divisor[s] == 0);
                if (ENV("LARECOMP_LOG_VCOPY") && g_gpuprof_frame)  // (research: what a profiled frame copies, per stream)
                    LOG("[vcopy] vs %llx stream %d addr %llx need %llu of bound %llu stride %llu %s max_index %u base_vertex %d base_inst %u inst %u\n",
                        (unsigned long long)st.program->stages[0].code_addr, s, (unsigned long long)v.addr, (unsigned long long)need,
                        (unsigned long long)v.size, (unsigned long long)stride, k.divisor[s] ? "instance" : "vertex", max_index, base_vertex, base_inst, inst);
            }
        }
        vb_bind(cb, s, sp.buf, sp.off, vm);
    }
    if (vm) vm->used = k.used_streams, vm->gen = vtx_memo_gen;
    }  // (!vm_hit)

    if (prof) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    // LARECOMP_GPUPROF_DRAWS=1 (with LARECOMP_GPUPROF_AT): a mark before every draw, so each row is one draw's GPU time
    static int gp_draw_marks = -1;
    if (gp_draw_marks < 0) gp_draw_marks = getenv("LARECOMP_GPUPROF_DRAWS") != NULL;
    if (g_gpuprof_frame && gp_draw_marks)
        gp_mark(cb, "draw vs %llx fs %016llx fsaddr %llx %s %u inst %u", (unsigned long long)st.program->stages[0].code_addr,
                (unsigned long long)g_cur_fs_hash, (unsigned long long)st.program->stages[1].code_addr, use_index ? "elements" : "arrays", use_index ? icount : count, inst);
    // Experiment (research): LARECOMP_EXP_VS=<vertex program address, hex> with LARECOMP_EXP_MODE=skip | scissor | half
    // changes only the draws of that program: no draw, a 1x1 scissor (no pixel work, vertex work kept) or half the instances.
    static uint64_t exp_vs;
    static int exp_mode = -1;
    if (exp_mode < 0) {
        const char *v = getenv("LARECOMP_EXP_VS"), *m = getenv("LARECOMP_EXP_MODE");
        exp_vs = v ? strtoull(v, NULL, 16) : 0;
        exp_mode = !exp_vs || !m ? 0 : !strcmp(m, "skip") ? 1 : !strcmp(m, "scissor") ? 2 : !strcmp(m, "half") ? 3 : 0;
    }
    // (LARECOMP_EXP_ALT=1: only on odd frames, so one run holds both versions of the same scene)
    static int exp_alt = -1;
    if (exp_alt < 0) exp_alt = getenv("LARECOMP_EXP_ALT") != NULL;
    bool exp_hit = exp_mode && st.program->stages[0].code_addr == exp_vs && (!exp_alt || (vk_frame_counter() & 1));
    uint32_t inst_eff = exp_hit && exp_mode == 3 && inst > 1 ? inst / 2 : inst;
    if (exp_hit && exp_mode == 2) { VkRect2D one = {{0, 0}, {1, 1}}; vkCmdSetScissor(cb, 0, 1, &one); }
    bool gp_q = g_gpuprof_frame && gp_draw_marks && gps_qp && gp_n && gps_n < GP_MAX;
    if (gp_q) vkCmdBeginQuery(cb, gps_qp, (uint32_t)gps_n, 0);
    // LARECOMP_INTERP_DROP=<mask> (research): the in-between images leave out the 3D draws that kept frame N's constants
    // for these reasons (bit 1 no match, 2 object rejected, 3 jump: 2 / 4 / 8) - what is missing shows what they are
    static int ip_drop = -1;
    if (ip_drop < 0) ip_drop = getenv("LARECOMP_INTERP_DROP") ? atoi(getenv("LARECOMP_INTERP_DROP")) : 0;
    if (ip_drop && ip_replaying && ip_kept_why && (ip_drop & (1 << ip_kept_why))) { /* left out */ }
    else if (exp_hit && exp_mode == 1) { /* skipped */ }
    else if (!use_index) vkCmdDraw(cb, count, inst_eff, first, base_inst);
    else vkCmdDrawIndexed(cb, icount, inst_eff, 0, base_vertex, base_inst);
    if (gp_q) { vkCmdEndQuery(cb, gps_qp, (uint32_t)gps_n); gp_stat[gp_n - 1] = gps_n + 1; gps_n++; }
    if (exp_hit && exp_mode == 2) apply_scissor();
    stat_draws++;
    g_draws_total++;
    cum_draws++;
    if (g_gpuprof_frame && gp_n && !gp_draw_marks) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x\n", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    LOG("[draw]   attribs:");  // format / stream / offset per attribute, and the streams' strides
    for (int i = 0; i < st.nattrib; i++)
        if ((pr->vi->attr_mask >> i) & 1)
            LOG(" a%d=f%#x/s%u/+%u", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
    for (int s = 0; s < st.nstream && s < 4; s++) LOG(" | stream%d stride %u", s, st.stream[s][0]);
    LOG("\n");
    if (getenv("LARECOMP_DUMP_SKIN") && ((pr->vi->attr_mask >> 11) & 1)) {  // one skinned draw's inputs -> build/skin/
        static int dumped;
        const char *at_frame = getenv("LARECOMP_DUMP_SKIN_FRAME");  // (the first one from that frame on)
        if (at_frame ? renderer_present_count() >= (uint64_t)atoll(at_frame) && !dumped++
                     : ++dumped == (atoi(getenv("LARECOMP_DUMP_SKIN")) > 1 ? atoi(getenv("LARECOMP_DUMP_SKIN")) : 1)) {  // (or the Nth skinned draw)
            mkdir("build/skin", 0755);
            char path[256];
            FILE *f;
            snprintf(path, sizeof path, "build/skin/info.txt");
            if ((f = fopen(path, "w"))) {
                fprintf(f, "vs %llx stride %u\n", (unsigned long long)st.program->stages[0].code_addr, st.stream[0][0]);
                for (int i = 0; i < st.nattrib; i++)
                    if ((pr->vi->attr_mask >> i) & 1)
                        fprintf(f, "attr %d fmt %#x off %u\n", i, st.attrib[i] & 0xFF, st.attrib[i] >> 13);
                for (int c = 0; c < 18; c++)
                    if ((pr->vi->cbuf_mask >> (c + 3)) & 1)
                        fprintf(f, "cb %d size %u\n", c + 3, (unsigned)st.ubuf[0][c].size);
                fclose(f);
            }
            uint64_t vsz = (uint64_t)st.stream[0][0] * 64;
            if (st.vbuf[0].addr && (f = fopen("build/skin/verts.bin", "wb"))) { fwrite((void *)(uintptr_t)st.vbuf[0].addr, 1, vsz < st.vbuf[0].size ? vsz : st.vbuf[0].size, f); fclose(f); }
            for (int c = 0; c < 18; c++) {
                if (!((pr->vi->cbuf_mask >> (c + 3)) & 1) || !st.ubuf[0][c].addr) continue;
                snprintf(path, sizeof path, "build/skin/cb%d.bin", c + 3);
                if ((f = fopen(path, "wb"))) { fwrite((void *)(uintptr_t)st.ubuf[0][c].addr, 1, st.ubuf[0][c].size, f); fclose(f); }
            }
            {   // compiler constants (cb1) live after the shader code
                NvnShaderStage *ss = &st.program->stages[0];
                if (ss->const_offset && (f = fopen("build/skin/cb1.bin", "wb"))) {
                    fwrite((void *)(uintptr_t)(ss->code_addr + ss->const_offset), 1, ss->const_size ? ss->const_size : 256, f);
                    fclose(f);
                }
            }
            LOG("[skin] dumped draw inputs of vs %llx to build/skin/\n", (unsigned long long)st.program->stages[0].code_addr);
            extern uint64_t g_skin_watch;  // (renderer_stats: is this cbuf filled later, by the GPU?)
            g_skin_watch = st.ubuf[0][0].addr;
            const uint32_t *w = (const uint32_t *)(uintptr_t)g_skin_watch;
            LOG("[skin] cb3 %llx at draw: row8 %08x %08x row9 %08x %08x\n", (unsigned long long)g_skin_watch, w[32], w[33], w[36], w[37]);
        }
    }
    if (getenv("LARECOMP_DEBUG_VSCB"))
        LOG("[draw]   vbuf0 %llx+%llu vbuf1 %llx+%llu\n", (unsigned long long)st.vbuf[0].addr,
            (unsigned long long)st.vbuf[0].size, (unsigned long long)st.vbuf[1].addr, (unsigned long long)st.vbuf[1].size);
    if (getenv("LARECOMP_DEBUG_VSCB") && st.vbuf[0].addr) {  // first vertices: bytes of attrs 11 / 12
        const uint8_t *v = (const uint8_t *)(uintptr_t)st.vbuf[0].addr;
        uint32_t stride = st.stream[0][0];
        for (int i = 0; i < 4 && stride; i++) {
            const uint8_t *p = v + (uint64_t)i * stride;
            LOG("[draw]   v%d pos %.3f %.3f %.3f", i, ((const float *)p)[0], ((const float *)p)[1], ((const float *)p)[2]);
            for (int a = 11; a <= 12; a++) {
                if (!((pr->vi->attr_mask >> a) & 1)) continue;
                const uint8_t *q = p + (st.attrib[a] >> 13);
                LOG(" a%d %u %u %u %u", a, q[0], q[1], q[2], q[3]);
            }
            LOG("\n");
        }
    }
    for (int c = 0; c < 18 && getenv("LARECOMP_DEBUG_VSCB"); c++) {  // dynamically indexed vertex cbufs (bone palettes)
        if (!((pr->vi->cbuf_dynamic >> (c + 3)) & 1) || !st.ubuf[0][c].addr) continue;
        const float *u = (const float *)(uintptr_t)st.ubuf[0][c].addr;
        LOG("[draw]   vcb%d %llx size %u:", c, (unsigned long long)st.ubuf[0][c].addr, (unsigned)st.ubuf[0][c].size);
        int n = (int)(st.ubuf[0][c].size / 4), bad = 0, ident = 0;
        float mx = 0;
        for (int k = 0; k < n; k++) {
            float f = u[k];
            if (f != f || f > 1e6f || f < -1e6f) bad++;
            else if (fabsf(f) > mx) mx = fabsf(f);
        }
        for (int m = 0; m + 16 <= n; m += 16) {
            bool id = true;
            for (int k = 0; k < 16; k++) if (u[m + k] != ((k % 5) == 0 ? 1.0f : 0.0f)) id = false;
            ident += id;
        }
        LOG(" floats %d, non-finite/huge %d, max |x| %.4g, identity 4x4 %d of %d | m1:", n, bad, mx, ident, n / 16);
        for (int k = 16; k < 32 && k < n; k++) LOG(" %.4g", u[k]);
        LOG(" | m5:");
        for (int k = 80; k < 96 && k < n; k++) LOG(" %.4g", u[k]);
        LOG("\n");
    }
    if (getenv("LARECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18; c++) {
            if (!((pr->fi->cbuf_mask >> c) & 1) || !st.ubuf[1][c].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[1][c].addr;
            LOG("[draw]   fcb%d %llx size %u:", c, (unsigned long long)st.ubuf[1][c].addr, (unsigned)st.ubuf[1][c].size);
            for (int k = 0; k < 96 && k * 4 < (int)st.ubuf[1][c].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s | target %u flags %#x layers %u bh %u addr %llx "
            "stride %llu%s%s '%s'\n", i, (void *)t, t->width, t->height, t->format,
            b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0, b && b->scalable ? " scalable" : "",
            t->target, t->flags, t->layout.layers, t->layout.block_height_log2, (unsigned long long)t->addr,
            (unsigned long long)t->stride, b && b->gpu_owned ? " gpu-owned" : "", b && b->dynamic ? " dynamic" : "", t->label);
    }
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    if (getenv("LARECOMP_LOG_BUF2TEX") && (t->format == 0x1 || t->format == 0xd)) {  // (movie planes)
        static int n;
        if (n++ < 12) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)src;
            unsigned sum = 0;
            for (int i = 0; i < 4096; i++) sum += p[(uint64_t)i * 97 % (row * (uint64_t)rg[4] ? row * (uint64_t)rg[4] : 1)];
            LOG("[vk] buf2tex src %llx -> %ux%u fmt %#x region %d,%d %dx%d row %llu sample-sum %u\n",
                (unsigned long long)src, t->width, t->height, t->format, rg[0], rg[1], rg[3], rg[4],
                (unsigned long long)row, sum);
        }
    }
    extern void vk_tex_unscale(NvnTexture *t);
    vk_tex_unscale(t);  // the copied data is full size: no output-resolution scaling for this texture
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    if (b->font_k > 1) return;  // (a glyph cache drawn at K x: its glyphs come from that copy, textures.c)
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();  // the CPU may read the result right after the fence
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s->width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s->height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d->width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame)
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

// push constants: flip, crop, colour grade
static VkPipeline present_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 8; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

// fmt: the output's format; blend 1: the output multiplies what is there (SSAO onto the game's scene, colour only),
// 2: replaces the colour, 3: is added to it (GI)
static VkPipeline post_pipeline_ex(const char *fs, VkFormat fmt, int blend) {
    // (about 30 passes: with 16 entries, FSR 3.1 + SSAO + GI made some of them again every frame)
    static struct { const char *fs; VkFormat fmt; int blend; VkPipeline p; } cache[64];
    for (int i = 0; i < 64; i++) if (cache[i].p && cache[i].fs == fs && cache[i].fmt == fmt && cache[i].blend == blend) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[6];  // (6: the optical flow's in-between image, flow_image)
        for (uint32_t i = 0; i < 6; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 6, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};  // (32 for most; a matrix: the motion vectors)
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    if (blend == 1)
        cba = (VkPipelineColorBlendAttachmentState){.blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                    .dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR, .colorBlendOp = VK_BLEND_OP_ADD,
                                                    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                    .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0x7};
    if (blend == 2) cba.colorWriteMask = 0x7;  // (replace the colour, keep alpha)
    if (blend == 3)  // (added to the colour: the indirect light)
        cba = (VkPipelineColorBlendAttachmentState){.blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE, .colorBlendOp = VK_BLEND_OP_ADD,
                                                    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                    .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0x7};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 64; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].fmt = fmt; cache[i].blend = blend; cache[i].p = p; break; }
    return p;
}
static VkPipeline post_pipeline(const char *fs) { return post_pipeline_ex(fs, VK_FORMAT_R8G8B8A8_UNORM, 0); }

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS, 5: colour adjustment, 6, 7: SSAO (occlusion, blur), 8: motion vectors (RGBA16F),
// 9: their RGBA8 encoding (LARECOMP_MV_DUMP), 10, 11: SSAO history (RG16F, ping-pong), 12, 13: the debug views,
// 14: (unused), 15-19: GI (work, history, debug: gi_run), 20: the scene pass's velocity target (per-object motion vectors), 21, 22: TAA history, 23: the scene depth as R32F, 24: FSR 3's output (storage), 25: the image
// before the interface, 26: FSR 3's interface mask, 27: an in-between image's TAA result (its history left as it was),
// 28, 29: N's SSAO / GI moved to an in-between image, 30: an in-between image resolved against N's FSR 3.1 output,
// 31: that image sharpened, 32: debanding (the last post pass), 33, 34: contact shadows (mask, blur: contact_run), 35: their
// debug view, 36: an in-between image with frame N's interface (ip_ui_keep), 37: FSR 3.1's output with the interface
// composited after it, 38, 39: the GI-size distance (R16, log2) and normals (gi_run). The in-between images at a
// reduced size (vk_tex_lo_begin) have a set of their own (64 on). 40-46: the optical flow's (flow_*), 47: DittoSR's output
// lined, its interface after it, 50: its motion and depth clip (render size, RGBA16F), 51, 52: its history (output
// size, ping-pong: gsr2_run), 63: the convert pass's output's other half (ping-pong with 50), 128, 129: DittoSR's
// change (x2_change.frag):
// sized for them, they were made again at every switch between a real frame and an in-between image, and the real
// frames' histories (SSAO, GI, TAA) went with them.
static TexBackend *post_image_fmt(int i, uint32_t w, uint32_t h, VkFormat fmt) {
    static TexBackend img[256];  // (128 on: more slots, each with its reduced twin 64 further)
    TexBackend *d = &img[i + (vk_tex_lo_active() ? 64 : 0)];
    if (d->image && (d->width != w || d->height != h || d->format != fmt)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        if (i == 24) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;  // (FSR 3 writes its output from compute)
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = fmt; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}
static TexBackend *post_image(int i, uint32_t w, uint32_t h) { return post_image_fmt(i, w, h, VK_FORMAT_R8G8B8A8_UNORM); }

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
// (any view: pipe made for its format by post_pipeline_ex; load: keep what is there - blending onto it)
static void post_pass_view(VkCommandBuffer cb, VkPipeline pipe, const VkImageView *srcs, int n, VkImageView dv, uint32_t dw,
                           uint32_t dh, const float *pc, uint32_t pcn, bool clear, bool load) {
    vk_post_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dv,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dw, dh}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[6];
    VkWriteDescriptorSet w[6];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dw, (float)dh, 0, 1};
    VkRect2D sc = {{0, 0}, {dw, dh}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcn, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    VkPipeline pipe = post_pipeline(fs);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    post_pass_view(cb, pipe, srcs, n, post_view(dst), dst->width, dst->height, pc, 32, clear, false);
}

// 4x4 inverse (row-major, Gauss-Jordan with partial pivoting). False: singular.
static bool mat4_inverse(const double *m, double *out) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        double inv = 1 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c || a[r][c] == 0) continue;
            double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][c + 4];
    return true;
}

// Camera motion vectors of this frame (post image 8, RG16F at the depth's size; velocity = this uv - the previous
// frame's): the previous view-projection x the inverse of this one, in double, applied per pixel to its depth.
// No previous frame (a scene's first, a skipped frame): the identity (no motion).
// LARECOMP_MV_YSIGN=-1: NDC y up at row 0 instead (validation). Returns the vectors' image, NULL if none.
static TexBackend *mv_run(VkCommandBuffer cb, VkImageView dv, uint32_t w, uint32_t h, float mo, uint64_t fr) {
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, inv[16];
    bool moved = ao.vp_frame == fr && ao.prev_ok && mat4_inverse(ao.vp, inv);
    if (moved)
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                double s = 0;
                for (int k = 0; k < 4; k++) s += ao.vp_prev[r * 4 + k] * inv[k * 4 + c];
                m[r * 4 + c] = s;
            }
    static float ysign;
    if (!ysign) ysign = getenv("LARECOMP_MV_YSIGN") ? (float)atof(getenv("LARECOMP_MV_YSIGN")) : 1;
    float pc[20];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) pc[c * 4 + r] = (float)m[r * 4 + c];  // (GLSL mat4: columns)
    pc[16] = mo, pc[17] = ysign, pc[18] = 0, pc[19] = 0;
    VkPipeline p = post_pipeline_ex("mv.frag", VK_FORMAT_R16G16B16A16_SFLOAT, 0);
    TexBackend *mv = post_image_fmt(8, w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    // the objects' own velocities where the scene pass wrote them this frame (level 2), the camera's elsewhere
    bool objs = ao.vel_img && ao.vel_frame == fr && ao.vel_img->width == w && ao.vel_img->height == h;
    VkImageView src[4] = {dv, objs ? post_view(ao.vel_img) : vk_dummy_view(TEXTYPE_2D)};
    post_pass_view(cb, p, src, 2, post_view(mv), w, h, pc, sizeof pc, false, false);
    {
        static uint64_t last;
        if (renderer_present_count() - last >= 600 && (mvo_stats[0] | mvo_stats[1] | mvo_stats[2])) {
            last = renderer_present_count();
            LOG("[mv] objects: previous constants found %llu, missing %llu, rejected %llu\n", (unsigned long long)mvo_stats[0],
                (unsigned long long)mvo_stats[1], (unsigned long long)mvo_stats[2]);
            memset(mvo_stats, 0, sizeof mvo_stats);
        }
    }
    // LARECOMP_MV_DUMP=<frame>,...: the vectors encoded in RGBA8 (0.5 + 32 x velocity) -> build/frames/mv_<frame>.png
    static const char *dump = (const char *)1;
    if (dump == (const char *)1) dump = getenv("LARECOMP_MV_DUMP");
    for (const char *q = mv_nodump ? NULL : dump; q && *q;) {
        long at = strtol(q, (char **)&q, 10);
        if (at == (long)fr) {
            TexBackend *enc = post_image(9, w, h);
            float pe[8] = {0, 0, 0, 0, 1.0f / (float)w, 1.0f / (float)h, 32, 0};
            VkImageView src[3] = {post_view(mv)};
            post_pass_view(cb, post_pipeline("mv_show.frag"), src, 1, post_view(enc), w, h, pe, 32, false, false);
            vk_full_barrier(cb);
            char path[96];
            snprintf(path, sizeof path, "build/frames/mv_%05ld.png", at);
            renderer_save_texture_tex(enc, path);
            LOG("[mv] frame %ld: %s, previous frame %s\n", at, path, moved ? "matched" : "missing (no motion)");
        }
        if (*q == ',') q++; else break;
    }
    return mv;
}

// Screen-space indirect light (F12 GI; shaders.c gi_*), in the scene hook before the occlusion: the depth and the
// normals (rebuilt from it) taken to 1 / gi_div of each axis (post images 38, 39) -> trace there (15) -> blur across
// (16) -> blur down (15) -> temporal (history 17 / 18, along the camera motion vectors) -> added onto the scene,
// edge-aware (the debug view: 19, at the present).
// (4K, GI high - 1/2, 4 rays x 8 steps -, Pallet Town, RTX 5070 Ti, LARECOMP_GPUPROF_AT medians: 1.24 -> 0.95 ms, of
// it the trace 0.87 -> 0.63 and depth + normals 0.08 -> 0.04 - a full-size normal pass before, now the GI's pixels
// only; GI low 0.48 -> 0.36)
static void gi_run(VkCommandBuffer cb, VkImageView dv, uint32_t w, uint32_t h, float mo, TexBackend *mv, uint64_t fr) {
    gp_mark(cb, "ours: GI %ux%u%s", w, h, ip_replaying ? " (in-between)" : "");
    uint32_t div = (uint32_t)g_settings.gi_div, lw = (w + div - 1) / div, lh = (h + div - 1) / div;
    // (pipelines and images before any rendering begins)
    VkPipeline p_n = post_pipeline("gi_normal.frag"), p_d = post_pipeline_ex("gi_depth.frag", VK_FORMAT_R16_UNORM, 0);
    VkPipeline p_t = post_pipeline_ex("gi_trace.frag", VK_FORMAT_R16G16B16A16_SFLOAT, 0);
    VkPipeline p_b = post_pipeline_ex("gi_blur.frag", VK_FORMAT_R16G16B16A16_SFLOAT, 0);
    VkPipeline p_tm = post_pipeline_ex("gi_temporal.frag", VK_FORMAT_R16G16B16A16_SFLOAT, 0);
    VkPipeline p_c = post_pipeline_ex("gi_composite.frag", ao.cfmt, 3);
    VkPipeline p_dbg = post_pipeline("gi_composite.frag");
    TexBackend *ln = post_image(39, lw, lh), *ld = post_image_fmt(38, lw, lh, VK_FORMAT_R16_UNORM);
    TexBackend *g0 = post_image_fmt(15, lw, lh, VK_FORMAT_R16G16B16A16_SFLOAT), *g1 = post_image_fmt(16, lw, lh, VK_FORMAT_R16G16B16A16_SFLOAT);
    // N's light (the last real frame's) is what an in-between image composites: a side image (vk_interp_replay)
    static TexBackend *gi_last;
    TexBackend *res = gi_last;
    uint64_t rf = renderer_present_count();  // (real frames: the history chains by these)
    float pn[16] = {ao.proj[0], ao.proj[1], ao.proj[2], ao.proj[3], 0, 0, 0, 0, 0, 0, 0, 0, mo, 0.75f, (float)lw, (float)lh};
    if (ip_replaying) {
        if (!gi_last) return;
        if (vk_tex_lo_active())  // (reduced: N's normals are another size - its own, from its depth)
            post_pass_view(cb, p_n, &dv, 1, post_view(ln), lw, lh, pn, sizeof pn, false, false);
        if (mv) {  // (moved to where this image's pixels were in N: post image 29)
            TexBackend *wp = post_image_fmt(29, lw, lh, VK_FORMAT_R16G16B16A16_SFLOAT);
            float pw[8] = {0, 0, 0, 0, 1.0f / (float)lw, 1.0f / (float)lh};
            VkImageView wi[4] = {post_view(gi_last), post_view(mv)};
            post_pass_view(cb, post_pipeline_ex("reproject.frag", VK_FORMAT_R16G16B16A16_SFLOAT, 0), wi, 2, post_view(wp), lw, lh,
                           pw, sizeof pw, false, false);
            res = wp;
        }
    } else {
        bool temporal = g_settings.gi_temporal && mv;
        float turn = temporal ? (float)fmod((double)rf * 0.6180339887, 1.0) : 0.0f;
        float pc[16];
        memcpy(pc, pn, sizeof pc);
        post_pass_view(cb, p_d, &dv, 1, post_view(ld), lw, lh, pc, sizeof pc, false, false);
        post_pass_view(cb, p_n, &dv, 1, post_view(ln), lw, lh, pc, sizeof pc, false, false);
        // trace: b = radius, thickness, origin bias, max receiver distance (60 m); c = rays, steps, turn, luminance clamp
        pc[4] = g_settings.gi_radius, pc[5] = g_settings.gi_thickness, pc[6] = 3, pc[7] = 6000;
        pc[8] = (float)g_settings.gi_rays, pc[9] = (float)g_settings.gi_steps, pc[10] = turn, pc[11] = GI_LUMA_CLAMP;
        VkImageView ti[4] = {post_view(ld), post_view(ln), ao.cview, dv};
        gp_mark(cb, "ours: GI trace");
        post_pass_view(cb, p_t, ti, 4, post_view(g0), lw, lh, pc, sizeof pc, false, false);
        float pb[16] = {ao.proj[0], ao.proj[1], ao.proj[2], ao.proj[3], 1, 0, 1, 0, 0, 0, 0, 0, mo, 0, (float)lw, (float)lh};
        VkImageView bx[4] = {post_view(g0), post_view(ln)};
        gp_mark(cb, "ours: GI blur, temporal");
        post_pass_view(cb, p_b, bx, 2, post_view(g1), lw, lh, pb, sizeof pb, false, false);
        pb[4] = 0, pb[5] = 1;
        VkImageView by[4] = {post_view(g1), post_view(ln)};
        post_pass_view(cb, p_b, by, 2, post_view(g0), lw, lh, pb, sizeof pb, false, false);
        res = g0;
        if (temporal) {  // (no history after a gap, a cut or a change of size)
            static uint64_t hist_frame;
            static uint32_t hist_w, hist_h;
            TexBackend *hp = post_image_fmt(17 + (int)((rf + 1) & 1), lw, lh, VK_FORMAT_R16G16B16A16_SFLOAT);
            TexBackend *hc = post_image_fmt(17 + (int)(rf & 1), lw, lh, VK_FORMAT_R16G16B16A16_SFLOAT);
            bool have = hist_frame + 1 == rf && ao.prev_ok && hist_w == lw && hist_h == lh;
            float pt[16] = {ao.proj[0], ao.proj[1], ao.proj[2], ao.proj[3], 0, 0.85f, 0, have ? 0.0f : 1.0f,
                            0, 0, 0, 0, mo, 0, (float)lw, (float)lh};
            VkImageView tt[4] = {post_view(g0), post_view(mv), post_view(hp)};
            post_pass_view(cb, p_tm, tt, 3, post_view(hc), lw, lh, pt, sizeof pt, false, false);
            hist_frame = rf, hist_w = lw, hist_h = lh;
            res = hc;
        }
        gi_last = res;
    }
    gp_mark(cb, "ours: GI composite");
    // composite: b.x = intensity x diffuse reflectance (0.5); c.xy = 1 / the output's size
    float strength = g_settings.gi_intensity * 0.5f;
    float pcm[16] = {ao.proj[0], ao.proj[1], ao.proj[2], ao.proj[3], strength, 0, 0, 0,
                     1.0f / (float)ao.cw, 1.0f / (float)ao.ch, 0, 0, mo, 0, (float)lw, (float)lh};
    VkImageView ci[4] = {dv, post_view(res), post_view(ln)};
    if (g_settings.gi_debug) {  // the light alone (x GI_DEBUG_EXPOSURE, tone mapped), shown at the present
        TexBackend *enc = post_image(19, w, h);
        float pd[16];
        memcpy(pd, pcm, sizeof pd);
        static float expo = -1;  // (LARECOMP_GI_DEBUG_EXPOSURE: another exposure for the view)
        if (expo < 0) expo = getenv("LARECOMP_GI_DEBUG_EXPOSURE") ? (float)atof(getenv("LARECOMP_GI_DEBUG_EXPOSURE")) : GI_DEBUG_EXPOSURE;
        pd[5] = 1, pd[6] = expo, pd[8] = 1.0f / (float)w, pd[9] = 1.0f / (float)h;
        post_pass_view(cb, p_dbg, ci, 3, post_view(enc), w, h, pd, sizeof pd, false, false);
        debug_view = enc, debug_view_frame = fr;
    }
    post_pass_view(cb, p_c, ci, 3, ao.cview, ao.cw, ao.ch, pcm, sizeof pcm, false, true);
    static bool logged;
    if (!logged) {
        logged = true;
        LOG("[gi] scene %ux%u, GI %ux%u (1/%u), %d rays x %d steps, radius %.0f\n", ao.cw, ao.ch, lw, lh, div, g_settings.gi_rays,
            g_settings.gi_steps, g_settings.gi_radius);
    }
}

// ---- Contact shadows (graphics page): short rays toward the sun in the scene's depth, for what the game's 2048² shadow
// map is too coarse to show - feet, grass, small props on the ground. The sun is the shadow map's light: that pass (a
// square depth target of its own, drawn before the scene) has in its draws' vertex cbuf 0 the light's view-projection
// times the draw's model, column-major and orthographic (its last row 0 0 0 1, the other three at right angles); its
// third row is the light's depth axis. The world's draws (no rotation of their own: most of them) agree on it: a vote
// over the pass keeps theirs (Boyer-Moore majority). Pallet Town: (0.39, -0.82, -0.42), the sun high in front-left.
static struct { float cand[3]; int votes; uint64_t frame; } sun;
static void sun_probe(void) {
    NvnTexture *dt = st.rt.depth;
    if (!dt || dt->width != dt->height || dt->width < 512) return;
    Range u = st.ubuf[0][0];
    if (!u.addr || u.size < 64) return;
    const float *m = (const float *)(uintptr_t)u.addr;
    if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) return;
    const float r[3][3] = {{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[2], m[6], m[10]}};
    float len[3];
    for (int i = 0; i < 3; i++) len[i] = sqrtf(r[i][0] * r[i][0] + r[i][1] * r[i][1] + r[i][2] * r[i][2]);
    if (!(len[0] > 1e-12f && len[1] > 1e-12f && len[2] > 1e-12f)) return;
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (fabsf(r[i][0] * r[j][0] + r[i][1] * r[j][1] + r[i][2] * r[j][2]) > 1e-3f * len[i] * len[j]) return;
    float d[3] = {r[2][0] / len[2], r[2][1] / len[2], r[2][2] / len[2]};
    uint64_t fr = fx_frame();
    if (sun.frame != fr) sun.frame = fr, sun.votes = 0;
    bool same = fabsf(d[0] - sun.cand[0]) < 1e-3f && fabsf(d[1] - sun.cand[1]) < 1e-3f && fabsf(d[2] - sun.cand[2]) < 1e-3f;
    if (!sun.votes) memcpy(sun.cand, d, sizeof d), sun.votes = 1;
    else sun.votes += same ? 1 : -1;
}
// Toward the sun in the world, this frame's (false: no shadow map this frame - an interior without sun, a menu)
static bool sun_dir(uint64_t fr, float out[3]) {
    if (sun.frame != fr || sun.votes <= 0) return false;
    float s = sun.cand[1] > 0 ? 1.0f : -1.0f;  // (the light travels along its depth axis, downward: toward it is up)
    for (int i = 0; i < 3; i++) out[i] = sun.cand[i] * s;
    return true;
}
// After the occlusion (ssao_run): the shadows, blurred along the depth as the occlusion is, multiplied onto the scene
static void contact_run(VkCommandBuffer cb, VkImageView dv, uint32_t w, uint32_t h, float mo, uint64_t fr) {
    float L[3];
    uint32_t bs = ao.scene_size[ao.scene_cur];
    // (none on the title screen: its close-up of Pikachu put the shadows of its own arms onto its chest as jagged
    // triangles - the rays are made for the world's scale, feet and props on the ground: runtime/hle/fps60.c game_title)
    extern bool game_title(void);
    if (game_title() || !sun_dir(fr, L) || ao.poff < 64 || ao.poff > bs) return;
    // into the view space the shaders rebuild from the depth (x right, y as the image's rows, z the distance): the
    // camera's rigid view (row-major, the 64 bytes before the projection in the scene block: mv_capture_camera) is GL's,
    // looking down -z, its y the rows' (checked on the shadows alone, ssao_debug: under the player's feet and a lamp's
    // cap with it; above the grass clumps with y turned). LARECOMP_CONTACT_FLIP=1 (research) turns y the other way.
    float v[16];
    memcpy(v, ao.scene[ao.scene_cur] + ao.poff - 64, sizeof v);
    float lv[3];
    for (int r = 0; r < 3; r++) lv[r] = v[r * 4] * L[0] + v[r * 4 + 1] * L[1] + v[r * 4 + 2] * L[2];
    static float flip = 0, env_strength = -2;
    if (!flip) flip = getenv("LARECOMP_CONTACT_FLIP") ? -1.0f : 1.0f;
    if (env_strength == -2) env_strength = getenv("LARECOMP_CONTACT_STRENGTH") ? (float)atof(getenv("LARECOMP_CONTACT_STRENGTH")) : -1;
    float strength = env_strength >= 0 ? env_strength : g_settings.contact_strength;
    // (the ray's length, its steps, and how far behind the depth a hit counts: settings_apply_levels - 100 units, 6
    // steps, 30 units - or the F12 Advanced page's)
    float pc[12] = {ao.proj[0], ao.proj[1], ao.proj[2], ao.proj[3], g_settings.contact_length, strength, mo,
                    (float)g_settings.contact_steps, lv[0], flip * lv[1], -lv[2], g_settings.contact_thickness};
    VkPipeline p_c = post_pipeline("contact.frag"), p_blur = post_pipeline("ssao_blur.frag");
    VkPipeline p_apply = post_pipeline_ex("ssao_apply.frag", ao.cfmt, 1);
    TexBackend *m0 = post_image(33, w, h), *m1 = post_image(34, w, h);
    gp_mark(cb, "ours: contact shadows%s", ip_replaying ? " (in-between)" : "");
    post_pass_view(cb, p_c, &dv, 1, post_view(m0), w, h, pc, sizeof pc, false, false);
    float pb[8] = {ao.proj[0], ao.proj[1], ao.proj[2], ao.proj[3], 1, 0, mo, 0};
    VkImageView bx[4] = {post_view(m0), dv};
    post_pass_view(cb, p_blur, bx, 2, post_view(m1), w, h, pb, 32, false, false);
    pb[4] = 0, pb[5] = 1;
    VkImageView by[4] = {post_view(m1), dv};
    post_pass_view(cb, p_blur, by, 2, post_view(m0), w, h, pb, 32, false, false);
    VkImageView ai[4] = {post_view(m0)};
    if (g_settings.ssao_debug && !g_settings.ssao) {  // (the shadows alone when the occlusion is off: ssao_debug's view)
        TexBackend *enc = post_image(35, w, h);
        float pe[8] = {0, 0, 0, 0, 1.0f / (float)w, 1.0f / (float)h};
        post_pass_view(cb, post_pipeline("ssao_apply.frag"), ai, 1, post_view(enc), w, h, pe, 32, false, false);
        debug_view = enc, debug_view_frame = fr;
    }
    float pa[8] = {0, 0, 0, 0, 1.0f / (float)ao.cw, 1.0f / (float)ao.ch};
    post_pass_view(cb, p_apply, ai, 1, ao.cview, ao.cw, ao.ch, pa, 32, false, true);
    static bool logged;
    if (!logged) {
        logged = true;
        LOG("[contact] sun %.3f %.3f %.3f (world), %.3f %.3f %.3f (view)\n", L[0], L[1], L[2], pc[8], pc[9], pc[10]);
    }
}

// The scene hook (outside any pass: vk_end_pass), once per frame as the scene pass ends: camera motion vectors, the
// indirect light, then the occlusion - from the depth at its size -> blur across -> blur down ->
// multiplied onto the scene's colour. The debug views replace the colour (the motion vectors' last).
static void ssao_run(void) {
    // (the marks: motion vectors, GI, SSAO, contact shadows, each its own row - the SSAO's used to start here and so
    // measured the motion vectors, while the GI's row went on through the SSAO: "GI 3.1 ms" was GI + SSAO)
    gp_mark(vk_cmd(), "ours: camera motion vectors%s", ip_replaying ? " (in-between)" : "");
    uint64_t fr = fx_frame();
    ssao_settle_camera();
    ao.done_frame = fr;
    TexBackend *d = ao.depth;
    if (ao.proj_frame != fr || !d || !d->image || !(d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || d->samples > 1) return;
    ao.busy = true;
    VkCommandBuffer cb = vk_cmd();
    static const VkComponentMapping id = {0};
    VkImageView dv = vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT);
    uint32_t w = d->width, h = d->height;
    float mo = nvn_depth_mode() == 0 ? 1.0f : 0.0f;
    TexBackend *mv = mv_wanted() ? mv_run(cb, dv, w, h, mo, fr) : NULL;
    cb = vk_cmd();  // (a dump submits the command buffer: renderer_save_texture_tex)
    if (g_settings.gi && g_settings.gi_intensity > 0) gi_run(cb, dv, w, h, mo, mv, fr);
    if (g_settings.ssao) {
        gp_mark(cb, "ours: SSAO%s", ip_replaying ? " (in-between)" : "");
        // (pipelines and images before any rendering begins)
        VkPipeline p_ao = post_pipeline("ssao.frag"), p_blur = post_pipeline("ssao_blur.frag");
        VkPipeline p_apply = post_pipeline_ex("ssao_apply.frag", ao.cfmt, 1);
        TexBackend *a0 = post_image(6, w, h), *a1 = post_image(7, w, h);
        // N's occlusion (the last real frame's) is what an in-between image applies: a side image (vk_interp_replay)
        static TexBackend *ao_last;
        TexBackend *res = ao_last;
        uint64_t rf = renderer_present_count();  // (real frames: the history chains by these)
        if (ip_replaying) {
            if (!ao_last || (!vk_tex_lo_active() && (ao_last->width != w || ao_last->height != h))) res = NULL;
            else if (mv) {  // (moved to where this image's pixels were in N: post image 28)
                TexBackend *wp = post_image_fmt(28, w, h, VK_FORMAT_R16G16_SFLOAT);
                float pw[8] = {0, 0, 0, 0, 1.0f / (float)w, 1.0f / (float)h};
                VkImageView wi[4] = {post_view(ao_last), post_view(mv)};
                post_pass_view(cb, post_pipeline_ex("reproject.frag", VK_FORMAT_R16G16_SFLOAT, 0), wi, 2, post_view(wp), w, h,
                               pw, 32, false, false);
                res = wp;
            }
        } else {
            // temporal: the sample spiral turns every frame (golden ratio steps), the history follows the camera
            bool temporal = g_settings.ssao_temporal && mv;
            float turn = temporal ? (float)fmod((double)rf * 0.6180339887, 1.0) : 0.0f;
            // half the intensity on the title screen and in cutscenes (their close-ups and lit scenes took it too dark),
            // the setting's in the world and in battles (runtime/hle/fps60.c game_title, runtime/hle/cutscene.c)
            extern bool game_title(void), game_cutscene(void);
            float ao_k = game_title() || game_cutscene() ? 0.5f : 1.0f;
            float pc[12] = {ao.proj[0], ao.proj[1], ao.proj[2], ao.proj[3], g_settings.ssao_radius, g_settings.ssao_intensity * ao_k, mo,
                            0.08f * (float)h, turn, (float)g_settings.ssao_samples};
            post_pass_view(cb, p_ao, &dv, 1, post_view(a0), w, h, pc, sizeof pc, false, false);
            float pb[8] = {ao.proj[0], ao.proj[1], ao.proj[2], ao.proj[3], 1, 0, mo, 0};
            VkImageView bx[4] = {post_view(a0), dv};
            post_pass_view(cb, p_blur, bx, 2, post_view(a1), w, h, pb, 32, false, false);
            pb[4] = 0, pb[5] = 1;
            VkImageView by[4] = {post_view(a1), dv};
            post_pass_view(cb, p_blur, by, 2, post_view(a0), w, h, pb, 32, false, false);
            res = a0;
            if (temporal) {  // history ping-pong (post images 10 / 11, RG16F: occlusion, distance); none after a gap or a cut
                static uint64_t hist_frame;
                static uint32_t hist_w, hist_h;
                TexBackend *hp = post_image_fmt(10 + (int)((rf + 1) & 1), w, h, VK_FORMAT_R16G16_SFLOAT);
                TexBackend *hc = post_image_fmt(10 + (int)(rf & 1), w, h, VK_FORMAT_R16G16_SFLOAT);
                bool have = hist_frame + 1 == rf && ao.prev_ok && hist_w == w && hist_h == h;
                float pt[8] = {ao.proj[0], ao.proj[1], ao.proj[2], ao.proj[3], 0, 0.9f, mo, have ? 0.0f : 1.0f};
                VkImageView ti[4] = {post_view(a0), dv, post_view(mv), post_view(hp)};
                post_pass_view(cb, post_pipeline_ex("ssao_temporal.frag", VK_FORMAT_R16G16_SFLOAT, 0), ti, 4, post_view(hc), w, h,
                               pt, 32, false, false);
                hist_frame = rf, hist_w = w, hist_h = h;
                res = hc;
            }
            ao_last = res;
        }
        if (res) {
        float pa[8] = {0, 0, 0, 0, 1.0f / (float)ao.cw, 1.0f / (float)ao.ch};
        VkImageView ai[4] = {post_view(res)};
        if (g_settings.ssao_debug) {  // (the occlusion alone, grey: shown at the present)
            TexBackend *enc = post_image(12, w, h);
            float pe[8] = {0, 0, 0, 0, 1.0f / (float)w, 1.0f / (float)h};
            post_pass_view(cb, post_pipeline("ssao_apply.frag"), ai, 1, post_view(enc), w, h, pe, 32, false, false);
            debug_view = enc, debug_view_frame = fr;
        }
        // LARECOMP_SSAO_DUMP=<frame>,...: the occlusion as applied (after the blur and the history) -> build/frames/ao_<frame>.png
        static const char *dump = (const char *)1;
        if (dump == (const char *)1) dump = getenv("LARECOMP_SSAO_DUMP");
        for (const char *q = dump; q && *q;) {
            long at = strtol(q, (char **)&q, 10);
            if (at == (long)fr) {
                TexBackend *enc = post_image(9, w, h);
                float pe[8] = {0, 0, 0, 0, 1.0f / (float)w, 1.0f / (float)h, 0};
                post_pass_view(cb, post_pipeline("ssao_apply.frag"), ai, 1, post_view(enc), w, h, pe, 32, false, false);
                vk_full_barrier(cb);
                char path[96];
                snprintf(path, sizeof path, "build/frames/ao_%05ld.png", at);
                renderer_save_texture_tex(enc, path);
                cb = vk_cmd();  // (submitted by the save)
            }
            if (*q == ',') q++; else break;
        }
        post_pass_view(cb, p_apply, ai, 1, ao.cview, ao.cw, ao.ch, pa, 32, false, true);
        static bool logged;
        if (!logged) {
            logged = true;
            LOG("[ssao] scene %ux%u, depth %ux%u (format %d), %u opaque draws before it\n", ao.cw, ao.ch, w, h, (int)d->format, ao.opaque);
        }
        }
    }
    if (g_settings.contact_on) contact_run(cb, dv, w, h, mo, fr);
    if (mv && g_settings.mv_debug) {  // (x right red, y up green, still: grey; 16 x velocity; shown at the present)
        TexBackend *enc = post_image(13, w, h);
        float ps[8] = {0, 0, 0, 0, 1.0f / (float)w, 1.0f / (float)h, 16, 0};
        VkImageView src[4] = {post_view(mv)};
        post_pass_view(cb, post_pipeline("mv_show.frag"), src, 1, post_view(enc), w, h, ps, 32, false, false);
        debug_view = enc, debug_view_frame = fr;
    }
    vk_full_barrier(cb);
    ao.busy = false;
}

// ================================================================== TAA
// The scene's draws on its depth are jittered by a sub-pixel offset each frame (ShaderKey.jitter: Halton 2, 3 over 8
// frames) and the HDR scene, complete (opaque, blended, the game's full-screen passes on it), is resolved against its
// history just before the game moves to another target (its bloom): begin_pass. The velocity carries no jitter.
static float taa_jit[4];          // this frame's offset in clip units (x, y), for binding 20
static uint64_t taa_jit_frame = ~0ull;
// FSR 3.1 ran at the last present (render width / output width for its jitter sequence): the scene is jittered and
// its motion vectors and depth handed to it; TAA steps aside (FSR anti-aliases)
static bool fsr3_engaged;
static uint32_t fsr3_rw, fsr3_rh, fsr3_ow;
// (DittoSR (FSR4), with FSR 3.1's inputs: as FSR 3.1 here - its jitter sequence too)
static bool fsr3_up(void) { return g_settings.upscaler == UPSCALE_FSR3 || UPSCALE_IS_FSR4(g_settings.upscaler); }
static TexBackend *post_image(int i, uint32_t w, uint32_t h);
static bool fsr3_on(void) { return fsr3_up() && fsr3_engaged; }
// DittoSR ran at the last present (gsr2_run): the scene jittered as for FSR 3.1, a Halton sequence of ceil(8 x ratio^2)
// phases (AMD's count for the ratio, which Qualcomm's GSR 2 sample takes too)
static bool gsr2_engaged;
static uint32_t gsr2_rw, gsr2_ow;
static bool gsr2_on(void) { return UPSCALE_IS_DITTOSR(g_settings.upscaler) && gsr2_engaged; }
static bool taa_on(void) { return AA_ON(g_settings.antialias) == AA_TAA && !fsr3_on(); }
static bool jitter_on(void) { return taa_on() || fsr3_on() || gsr2_on(); }
static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}
static const float *taa_jitter(void) {
    uint64_t fr = fx_frame();
    if (taa_jit_frame != fr) {
        taa_jit_frame = fr;
        // the sequence: the real frames' by their count, the in-between images' by theirs (by fx_frame, k images a
        // frame, the real frames took every k-th phase: half of FSR 3.1's eight at 2x)
        uint64_t seq = ip_replaying ? ip_replays : renderer_present_count();
        uint32_t i = (uint32_t)(seq % 8) + 1;
        float w = ao.cw ? (float)ao.cw : 1280, h = ao.ch ? (float)ao.ch : 720;
        float jx = halton(i, 2) - 0.5f, jy = halton(i, 3) - 0.5f;
        if (fsr3_on()) fsr3_jitter(fsr3_rw, fsr3_ow, seq, &jx, &jy);  // (FSR's own sequence, for its ratio)
        else if (gsr2_on() && gsr2_rw) {
            float r = (float)gsr2_ow / (float)gsr2_rw;
            uint32_t phases = (uint32_t)ceilf(8.0f * r * r), k = (uint32_t)(seq % phases) + 1;
            jx = halton(k, 2) - 0.5f, jy = halton(k, 3) - 0.5f;
        }
        taa_jit[0] = jx * 2.0f / w;  // (a pixel is 2 / size in clip units: the content moves by (jx, jy) pixels)
        taa_jit[1] = jy * 2.0f / h;
        taa_jit[2] = jx, taa_jit[3] = jy;
    }
    return taa_jit;
}

static void taa_run(void) {
    gp_mark(vk_cmd(), "ours: TAA / motion vectors%s", ip_replaying ? " (in-between)" : "");
    uint64_t fr = fx_frame();
    ao.taa_frame = fr;
    TexBackend *d = ao.depth;
    if (!d || !d->image || !(d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || d->samples > 1 || !ao.cview) return;
    VkCommandBuffer cb = vk_cmd();
    static const VkComponentMapping id = {0};
    VkImageView dv = vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT);
    float mo = nvn_depth_mode() == 0 ? 1.0f : 0.0f;
    // the motion vectors again: the blended draws after the scene hook have written their velocities since
    mv_nodump = true;
    ssao_settle_camera();
    TexBackend *mv = ao.proj_frame == fr ? mv_run(cb, dv, d->width, d->height, mo, fr) : NULL;
    mv_nodump = false;
    cb = vk_cmd();
    if (!taa_on()) { vk_full_barrier(cb); return; }  // (FSR 3: the motion vectors only; it resolves at the present)
    uint32_t w = ao.cw, h = ao.ch;
    // the history: N's result (taa_last). A real frame's goes on in the other slot (real frames chain by
    // renderer_present_count); an in-between image reads N's and writes post image 27, leaving the history as it was
    static uint64_t hist_frame;
    static uint32_t hist_w, hist_h;
    static TexBackend *taa_last;
    uint64_t rf = renderer_present_count();
    VkPipeline p_taa = post_pipeline_ex("taa.frag", VK_FORMAT_R16G16B16A16_SFLOAT, 0);
    VkPipeline p_copy = post_pipeline_ex("copy.frag", ao.cfmt, 2);
    TexBackend *hc = post_image_fmt(ip_replaying ? 27 : 21 + (int)(rf & 1), w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    bool have = mv && taa_last && (ip_replaying ? hist_frame == rf : hist_frame + 1 == rf && hist_w == w && hist_h == h) && ao.prev_ok;
    TexBackend *hp = have ? taa_last : post_image_fmt(21 + (int)((rf + 1) & 1), w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    float pc[8] = {ao.proj[0], ao.proj[1], ao.proj[2], ao.proj[3], have ? 0.0f : 1.0f, TAA_HISTORY, mo, 0};
    VkImageView src[4] = {ao.cview, post_view(hp), mv ? post_view(mv) : vk_dummy_view(TEXTYPE_2D), dv};
    post_pass_view(cb, p_taa, src, 4, post_view(hc), w, h, pc, 32, false, false);
    float pk[8] = {0, 0, 0, 0, 1.0f / (float)w, 1.0f / (float)h};
    VkImageView ci[4] = {post_view(hc)};
    post_pass_view(cb, p_copy, ci, 1, ao.cview, w, h, pk, 32, false, true);
    vk_full_barrier(cb);
    if (!ip_replaying) hist_frame = rf, hist_w = w, hist_h = h, taa_last = hc;
    static bool logged;
    if (!logged) {
        logged = true;
        LOG("[taa] scene %ux%u, history %s, jitter %.3f %.3f px\n", w, h, have ? "kept" : "reset",
            taa_jit[0] * (float)w * 0.5f, taa_jit[1] * (float)h * 0.5f);
    }
}

static void ui_snapshot_check(Prog *pr) {
    uint64_t fr = fx_frame();
    if (!st.rt.ncolor || !st.rt.color[0]) return;
    if (vk_tex_lo_active()) return;  // (a reduced in-between image: resolved against N, no mask; ui_scene_rt stays N's)
    TexBackend *tb = st.rt.color[0]->backend;
    if (!tb || !tb->image) return;
    bool presented = renderer_texture_presented(st.rt.color[0]);
    if (!presented && tb != ui_scene_rt) return;
    if (!pr->fi->ntex) return;  // (an untextured fill: a fade, not the interface)
    // reading a scalable render target: the scene's composite, not the interface (the interface's own 2x images and the
    // high-resolution text cache are scaled too, but not scalable: runtime/ui2x.c, font_hires.c). At any scale - at the
    // game's own size (1) the copy was made before the composite and the mask covered the whole image: every
    // in-between image of the optical flow was N's
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        TexBackend *b = t ? t->backend : NULL;
        if (b && b != tb && b->scalable) {
            if (presented && b->width == tb->width && b->height == tb->height)  // (the last pass: its input, at this size)
                ui_final_src = ui_scene_rt = b, ui_final_frame = fr;
            return;
        }
    }
    if (ui_snap_frame == fr) return;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    TexBackend *snap = post_image(25, tb->width, tb->height);
    float pk[8] = {0, 0, 0, 0, 1.0f / (float)tb->width, 1.0f / (float)tb->height};
    VkImageView src[4] = {raw_view(tb)};
    post_pass_view(cb, post_pipeline("copy.frag"), src, 1, post_view(snap), tb->width, tb->height, pk, 32, false, false);
    vk_full_barrier(cb);
    ui_snap_frame = fr, ui_snap = snap, ui_snap_of = tb;
    st.rt_dirty = true;  // (the pass begins again for this draw)
}

// The interface mask of image b (post image 26: where it differs from the copy made before the frame's first interface
// draw, ui_snapshot_check), or NULL. FSR 3.1 takes it; frame generation keeps the real frame's for its in-between images
// (ip_ui_mask): their interface is frame N's, drawn with N's constants.
static TexBackend *ip_ui_mask;
static uint64_t ip_ui_mask_real;  // (the present it was made at)
static TexBackend *ui_mask_build(VkCommandBuffer cb, TexBackend *b, uint64_t fr) {
    // the image the copy was made of, as it is now: the presented one, or the last pass's input (before its colour
    // grading, like the copy)
    TexBackend *now = ui_snap_of == b ? b : ui_snap_of == ui_final_src && ui_final_frame == fr ? ui_final_src : NULL;
    if (ui_snap_frame != fr || !ui_snap || !now || ui_snap->width != b->width || ui_snap->height != b->height) return NULL;
    TexBackend *m = post_image(26, b->width, b->height);
    float pz[8] = {0};
    VkImageView mi[4] = {raw_view(now), post_view(ui_snap)};
    post_pass_view(cb, post_pipeline("ui_mask.frag"), mi, 2, post_view(m), b->width, b->height, pz, 32, false, false);
    vk_full_barrier(cb);
    static long dump_at = -2;  // (LARECOMP_FSR3_MASK_DUMP=<frame>: the mask -> build/frames/uimask_<frame>.png)
    if (dump_at == -2) dump_at = getenv("LARECOMP_FSR3_MASK_DUMP") ? atol(getenv("LARECOMP_FSR3_MASK_DUMP")) : -1;
    if (dump_at >= 0 && ((long)fr == dump_at || (long)renderer_present_count() == dump_at)) {  // (with frame generation: the present)
        char path[64];
        snprintf(path, sizeof path, "build/frames/uimask_%05ld.png", dump_at);
        renderer_save_texture_tex(m, path);
    }
    if (!ip_side_post) ip_ui_mask = m, ip_ui_mask_real = renderer_present_count();
    return m;
}
// An in-between image's interface, frame N's: resolved against N's image along the motion vectors of the scene under it
// (side_resolve.frag), static text and boxes took N's shifted by the scene's motion - the battle's text doubled in the
// images between two frames, clean in the real ones (FSR 3.1 has the mask), a flicker at the game's rate. Where N's
// mask says interface, the image takes N's processed image at the same place instead: the same interface, as sharp
// (after the RCAS that follows the resolve: not sharpened twice). in: the resolved image; nimg: N's, the same size.
static TexBackend *ip_ui_keep(VkCommandBuffer cb, TexBackend *in, TexBackend *nimg, bool nraw) {
    if (!ip_ui_mask || ip_ui_mask_real != renderer_present_count() || !nimg) return in;
    static int off = -1;  // (LARECOMP_INTERP_UI_KEEP=0: comparison, the interface resolved like the scene)
    if (off < 0) off = getenv("LARECOMP_INTERP_UI_KEEP") && !atoi(getenv("LARECOMP_INTERP_UI_KEEP"));
    if (off) return in;
    TexBackend *out = post_image(36, in->width, in->height);
    float pc[8] = {1.0f / (float)in->width, 1.0f / (float)in->height};
    VkImageView si[4] = {post_view(in), nraw ? raw_view(nimg) : post_view(nimg), post_view(ip_ui_mask)};
    post_pass_view(cb, post_pipeline("ui_keep.frag"), si, 3, post_view(out), in->width, in->height, pc, 32, false, false);
    out->sx = in->sx, out->sy = in->sy, out->scalable = in->scalable;
    return out;
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static double post_last_ow, post_last_oh;  // the output size post_process was last asked for
static float g_present_grade[4];  // the colour grade post_process leaves to the present's draw ([3] > 0.5: apply it)
static VkImageView post_process_ex(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d, TexBackend **res);
// DittoSR (UPSCALE_DITTOSR; gsr2_* names the path it grew from, Snapdragon GSR 2): GSR 2's convert pass (shaders.c
// gsr2_convert.frag: the motion - the nearest of the 3x3's - and how far each pixel's depth stands out from its
// neighbours', render size), x2_change.frag (how much of the image changed), then our upscale x2_upscale.frag against the
// previous output moved along the motion (output size, a ping-pong history gamma-encoded as the image, 10 bits a
// channel: in RGBA16F GSR 2's reads and writes took 2.5 ms of an Adreno 830's frame at 2133 x 1200); x2_final.frag after
// it (post_process_ex). in: the image (render size, jittered by taa_jitter), mv: the motion vectors.
static TexBackend *gsr2_mda;  // (gsr2_run's convert pass output this frame: x2_final.frag's motion)
static TexBackend *gsr2_run(VkCommandBuffer cb, TexBackend *in, TexBackend *mv, uint32_t ow, uint32_t oh) {
    static int cur;
    static uint32_t hw, hh;  // (the history's size)
    static uint64_t last;    // (the real frame it last ran: one missed, the history starts again)
    uint32_t rw = in->width, rh = in->height;
    bool reset = !ao.prev_ok || hw != ow || hh != oh || last + 1 != renderer_present_count();
    hw = ow, hh = oh, last = renderer_present_count();
    // (the convert pass's output, ping-pong 50 / 63: the rejection by a changed motion reads the previous frame's)
    static int mcur;
    TexBackend *mdap = post_image_fmt(mcur ? 63 : 50, rw, rh, VK_FORMAT_R16G16B16A16_SFLOAT);
    mcur ^= 1;
    TexBackend *mda = post_image_fmt(mcur ? 63 : 50, rw, rh, VK_FORMAT_R16G16B16A16_SFLOAT);
    VkFormat hf = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    TexBackend *prev = post_image_fmt(51 + cur, ow, oh, hf);
    cur ^= 1;
    TexBackend *out = post_image_fmt(51 + cur, ow, oh, hf);
    // (112 bytes of constants: x2_upscale.frag's, as the standalone DittoSR's upscale)
    float pc[28] = {(float)rw, (float)rh, (float)ow, (float)oh, 1.0f / (float)rw, 1.0f / (float)rh, 1.0f / (float)ow, 1.0f / (float)oh,
                    taa_jit[2], taa_jit[3], (float)ow / (float)rw, 0,
                    // tan(horizontal fov / 2) for the convert pass's depth clip, then the reset (its output: this frame's
                    // image alone)
                    (float)rw / ((float)rh * ao.proj[1]), 0, reset ? 1.0f : 0.0f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static const VkComponentMapping id = {0};
    VkImageView dv = vk_image_view(ao.depth, VK_IMAGE_VIEW_TYPE_2D, ao.depth->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView ci[2] = {dv, post_view(mv)};
    post_pass_view(cb, post_pipeline_ex("gsr2_convert.frag", VK_FORMAT_R16G16B16A16_SFLOAT, 0), ci, 2, post_view(mda), rw, rh, pc,
                   64, false, false);
    // (the upscale's knobs into its constants only, after the convert pass - it reads d.x as the field of view: 0 there
    // marked every pixel's depth as standing out, the history all but unused. Research: LARECOMP_EXP2_ALPHA, _CONF,
    // _GAMMA - 0 their defaults)
    static float k[4] = {-1, 0, 0, 0};
    if (k[0] < 0) {
        k[3] = getenv("LARECOMP_EXP2_WIDTH") ? (float)atof(getenv("LARECOMP_EXP2_WIDTH")) : 0.8f;  // (the narrow Gaussian 0.8 wide)
        // (at rest, a pixel stable for 3 frames: x2_upscale.frag's stability level - 0.05 before it, as stable as GSR 2)
        k[0] = getenv("LARECOMP_EXP2_ALPHA") ? (float)atof(getenv("LARECOMP_EXP2_ALPHA")) : 0.035f;
        k[1] = getenv("LARECOMP_EXP2_CONF") ? (float)atof(getenv("LARECOMP_EXP2_CONF")) : 0.05f;  // (the narrow estimate alone from 0.05)
        k[2] = getenv("LARECOMP_EXP2_GAMMA") ? (float)atof(getenv("LARECOMP_EXP2_GAMMA")) : 0;
    }
    pc[12] = k[0], pc[13] = k[1], pc[15] = k[2], pc[11] = k[3];  // (c.w: the narrow Gaussian's width x, _WIDTH)
    // (research: LARECOMP_EXP2_JX / _JY = the jitter's sign per axis, as the image's pixels see it)
    static float jx = 0, jy = 0;
    if (!jx) jx = getenv("LARECOMP_EXP2_JX") ? (float)atof(getenv("LARECOMP_EXP2_JX")) : 1, jy = getenv("LARECOMP_EXP2_JY") ? (float)atof(getenv("LARECOMP_EXP2_JY")) : 1;
    pc[8] *= jx, pc[9] *= jy;
    // (the share by nearness - b.xy: the new frame's share x 0.05 where no jittered sample landed close, x 3 where one
    // did; LARECOMP_EXP2_AFAR, _ANEAR)
    static float af = -1, an = 0;
    if (af < 0) af = getenv("LARECOMP_EXP2_AFAR") ? (float)atof(getenv("LARECOMP_EXP2_AFAR")) : 0.05f,
                an = getenv("LARECOMP_EXP2_ANEAR") ? (float)atof(getenv("LARECOMP_EXP2_ANEAR")) : 3;
    pc[4] = af, pc[5] = an;
    // (the reaction to changes, the edge-directed Gaussian, thin lines kept: LARECOMP_EXP2_REACT, _EDGE, _LOCK - e)
    static float ke[3] = {-1, 0, 0};
    if (ke[0] < 0) {
        ke[0] = getenv("LARECOMP_EXP2_REACT") ? (float)atof(getenv("LARECOMP_EXP2_REACT")) : 1;  // (the menu closed: 10 frames late -> as DittoSR (FSR4))
        ke[1] = getenv("LARECOMP_EXP2_EDGE") ? (float)atof(getenv("LARECOMP_EXP2_EDGE")) : 1;
        ke[2] = getenv("LARECOMP_EXP2_LOCK") ? (float)atof(getenv("LARECOMP_EXP2_LOCK")) : 0;
    }
    pc[16] = ke[0], pc[17] = ke[1], pc[18] = ke[2];
    // (f and g: moving, the new frame's base share, the colour's Gaussian width, b.y; the history clipped by its low
    // frequencies; the disocclusion (0) or GSR 2's depth clip (1); the clip width moving; how far the box keeps within the
    // samples' min / max. LARECOMP_EXP2_MSHARE, _MWIDTH, _MNEAR, _SPLIT, _DEPTHMODE, _MGAMMA, _MMINMAX)
    static float kf[7] = {-1};
    if (kf[0] < 0) {
        static const char *const ne[7] = {"LARECOMP_EXP2_MSHARE", "LARECOMP_EXP2_MWIDTH", "LARECOMP_EXP2_MNEAR", "LARECOMP_EXP2_SPLIT",
                                          "LARECOMP_EXP2_DEPTHMODE", "LARECOMP_EXP2_MGAMMA", "LARECOMP_EXP2_MMINMAX"};
        static const float dv[7] = {0.15f, 1.6f, 1.5f, 1, 0, 1.5f, 1};
        for (int i = 0; i < 7; i++) kf[i] = getenv(ne[i]) ? (float)atof(getenv(ne[i])) : dv[i];
    }
    for (int i = 0; i < 7; i++) pc[20 + i] = kf[i];
    pc[27] = g_settings.dittosr_frame_weight;  // (g.w: x the new frame's share)
    // (the fast settling: how much of the image changed against its history - x2_change.frag, 4 x 4 R16F, then
    // averaged into 1 x 1)
    TexBackend *chg = post_image_fmt(128, 1, 1, VK_FORMAT_R16_SFLOAT);
    if (!reset) {
        TexBackend *c4 = post_image_fmt(129, 4, 4, VK_FORMAT_R16_SFLOAT);
        VkPipeline pch = post_pipeline_ex("x2_change.frag", VK_FORMAT_R16_SFLOAT, 0);
        float pz[8] = {0}, p1[8] = {1};
        VkImageView gi[3] = {post_view(prev), post_view(mda), raw_view(in)};
        post_pass_view(cb, pch, gi, 3, post_view(c4), 4, 4, pz, 32, false, false);
        VkImageView ri[3] = {post_view(c4), post_view(mda), raw_view(in)};
        post_pass_view(cb, pch, ri, 3, post_view(chg), 1, 1, p1, 32, false, false);
    }
    VkImageView ui[5] = {post_view(prev), post_view(mda), raw_view(in), post_view(mdap), post_view(chg)};
    gp_mark(cb, "ours: DittoSR upscale");
    post_pass_view(cb, post_pipeline_ex("x2_upscale.frag", hf, 0), ui, 5, post_view(out), ow, oh, pc, 112, false, false);
    gsr2_mda = mda;
    return out;
}
static TexBackend *hold_copy_into(TexBackend *d, TexBackend *src);
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    return post_process_ex(cb, b, ow_d, oh_d, NULL);
}
extern uint64_t fps60_period_ns(void);  // (runtime/hle/fps60.c: the game's frame time now)
static bool flow_on(void);  // (frame generation by the optical flow: below)
// res (may be NULL): the image behind the view returned (b when nothing was done)
static VkImageView post_process_ex(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d, TexBackend **res) {
    post_last_ow = ow_d, post_last_oh = oh_d;
    gp_mark(cb, "ours: post %ux%u -> %.0fx%.0f%s", b->width, b->height, ow_d, oh_d, ip_side_post ? " (in-between)" : "");
    VkImageView v = raw_view(b);
    TexBackend *vb = b;  // (the image behind v: FSR 3 takes the image itself)
    TexBackend *rb_ = b;  // (... and behind the result: *res)
    if (res) *res = b;
    // a debug view of this frame or the one before (F12), at the game image's size: shown instead of it
    if ((g_settings.ssao_debug || g_settings.mv_debug || g_settings.gi_debug) && debug_view && debug_view_frame + 2 >= fx_frame() &&
        debug_view->width == b->width && debug_view->height == b->height)
        v = post_view(debug_view), vb = debug_view, rb_ = debug_view;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool upscale = ow > b->width + 1 || oh > b->height + 1;
    // FSR 3: an upscale, its DLL, and this frame's scene depth and motion vectors at the image's size
    uint64_t fr = fx_frame();
    // (Native AA: FSR 3.1 at the same size, as anti-aliasing)
    bool fsr3 = fsr3_up() && (upscale || g_settings.fsr3_preset == FSR3_NATIVE_AA) && fsr3_available() &&
                (!UPSCALE_IS_FSR4(g_settings.upscaler) || dittosr_available()) &&
                ao.depth && ao.taa_frame == fr &&
                ao.depth->width == b->width && ao.depth->height == b->height && ao.proj_frame == fr;
    // DittoSR: the same, without a DLL (an in-between image of frame generation: resolved against N's output, as FSR 3.1's
    // are)
    bool gsr2 = UPSCALE_IS_DITTOSR(g_settings.upscaler) && (upscale || g_settings.fsr3_preset == FSR3_NATIVE_AA) && ao.depth &&
                ao.taa_frame == fr && ao.depth->width == b->width && ao.depth->height == b->height && ao.proj_frame == fr;
    bool smaa = AA_ON(g_settings.antialias) == AA_SMAA && !fsr3 && !gsr2;
    bool fsr = (g_settings.upscaler == UPSCALE_FSR1 || (fsr3_up() && !fsr3) ||
                (UPSCALE_IS_DITTOSR(g_settings.upscaler) && !gsr2)) && upscale;  // (an upscale only)
    float grade[3] = {g_settings.brightness, g_settings.contrast, g_settings.saturation};  // (F12 menu)
    bool color = fabsf(grade[0] - 1) > 0.005f || fabsf(grade[1] - 1) > 0.005f || fabsf(grade[2] - 1) > 0.005f;
    bool sharpen = taa_on() && !fsr && !fsr3 && !gsr2 && g_settings.taa_sharpness > 0.01f;  // (TAA softens)
    if (!fsr3) fsr3_engaged = false;
    if (!gsr2 && !ip_side_post) gsr2_engaged = false;
    if (!ip_side_post) {  // (the real frames': the in-between images are rendered from the same constants)
        static float env = -100;
        if (env == -100) env = getenv("LARECOMP_MIP_BIAS") ? (float)atof(getenv("LARECOMP_MIP_BIAS")) : -99;
        bool temporal = fsr3 || gsr2 || taa_on();
        float off = env != -99 ? env : fsr3 || gsr2 ? -1.0f : -0.5f;
        float mb = temporal && off != 0 ? log2f((float)b->width / (float)ow) + off : 0;
        mb = roundf(mb * 4) / 4;
        // (DittoSR: its own floor, LARECOMP_EXP2_MIPMIN - at 3x the ratio asks -2.6)
        static float x2mip = 1;
        if (x2mip > 0) x2mip = getenv("LARECOMP_EXP2_MIPMIN") ? (float)atof(getenv("LARECOMP_EXP2_MIPMIN")) : -2.6f;
        float mlo = UPSCALE_IS_DITTOSR(g_settings.upscaler) ? x2mip : -2.0f;
        mb = mb < mlo ? mlo : mb > 0 ? 0 : mb;
        if (mb != g_mip_bias) LOG("[post] mip bias %.2f\n", mb);
        g_mip_bias = mb;
    }
    // Debanding and a dither of one 8-bit step, last (deband.frag): on unless LARECOMP_DEBAND=0 (comparison). Off on
    // Android unless LARECOMP_DEBAND=1: ~12% of a phone's GPU frame at 1080p, for banding hardly seen on its screen.
    static int deband = -1;
#ifdef __ANDROID__
    if (deband < 0) deband = getenv("LARECOMP_DEBAND") ? atoi(getenv("LARECOMP_DEBAND")) : 0;
#else
    if (deband < 0) deband = getenv("LARECOMP_DEBAND") ? atoi(getenv("LARECOMP_DEBAND")) : 1;
#endif
    // The colour grade in the present's own draw (present.frag) when nothing comes after it here:
    // one full-size pass (written to memory and read back) less. Every image reaching the window goes through the present.
    // Not with the optical flow and nothing else here: it starts from N's processed image (flow_prepare), and without
    // one there were no in-between images at all - Android, no anti-aliasing nor debanding: 30 fps with 4x on. The grade
    // stays here then, an identity one too.
    bool flow_src = flow_on() && !smaa && !sharpen && !fsr && !fsr3 && !gsr2 && !deband;
    if (flow_src) color = true;
    bool grade_late = color && !sharpen && !fsr && !fsr3 && !gsr2 && !deband && !flow_src;
    g_present_grade[0] = grade[0], g_present_grade[1] = grade[1], g_present_grade[2] = grade[2];
    g_present_grade[3] = grade_late ? 1.0f : 0.0f;
    if (grade_late) color = false;
    if (!smaa && !fsr && !color && !sharpen && !fsr3 && !gsr2 && !deband) { if (res) *res = rb_; return v; }
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    // (frame generation without FSR 3.1: the real frame's interface mask, for its reduced in-between images' resolve -
    // ip_lo_resolve, ip_ui_keep)
    if (!fsr3 && !gsr2 && !ip_side_post && ip_enabled() && g_settings.frame_gen && ui_mask_build(cb, b, fr)) cb = vk_cmd();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height), *weights = post_image(1, b->width, b->height);
        TexBackend *out = post_image(2, b->width, b->height);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out), rb_ = out;
    }
    if (color) {  // after SMAA (its edges are found on the game's own colours), before FSR (at the smaller size)
        float pc[8] = {grade[0], grade[1], grade[2]};
        TexBackend *c = post_image(5, b->width, b->height);
        VkImageView ci[3] = {v};
        post_pass(cb, "color.frag", ci, 1, c, pc, false);
        v = post_view(c), vb = c, rb_ = c;
    }
    if (sharpen) {  // RCAS at the image's size: sharpness 0..1 -> 2..0 stops (AMD's scale: 0 the sharpest)
        TexBackend *r = post_image(4, b->width, b->height);
        float pr[8] = {exp2f(-2.0f * (1.0f - g_settings.taa_sharpness)), 0, 0, 0, (float)b->width, (float)b->height};
        VkImageView ri_[3] = {v};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r), rb_ = r;
    }
    if (fsr3 || gsr2) {  // FSR 3.1 / DittoSR: the image (after the colour pass), the scene depth (FSR: as R32F), the motion vectors
        TexBackend *in = vb;
        // an in-between image (frame interpolation): resolved against N's FSR 3.1 output instead - FSR's history goes on
        // with the real frames only (side_resolve.frag, post image 30); it needs neither the depth copy nor the mask
        TexBackend *nh = ip_side_post ? ip_n_post(ow, oh) : NULL;
        // (OptiScaler injected, LARECOMP_FSR3_OPTISCALER=1: the image copied into one of ours that lives on. OptiScaler
        // keeps the image views of a dispatch until the next one, and through a loading screen or a menu - no dispatch
        // - the game freed its own image: destroying the view later, the NVIDIA driver jumped into freed memory)
        static TexBackend fsr_in;
        if (fsr3 && !nh && fsr3_optiscaler()) in = hold_copy_into(&fsr_in, in);
        TexBackend *dr = nh || !fsr3 ? NULL : post_image_fmt(23, b->width, b->height, VK_FORMAT_R32_SFLOAT);
        TexBackend *out = fsr3 ? post_image_fmt(24, ow, oh, VK_FORMAT_R8G8B8A8_UNORM) : NULL;
        TexBackend *mv = post_image_fmt(8, ao.depth->width, ao.depth->height, VK_FORMAT_R16G16B16A16_SFLOAT);
        static const VkComponentMapping id = {0};
        float pz[8] = {0};
        if (dr) {
            VkImageView dv = vk_image_view(ao.depth, VK_IMAGE_VIEW_TYPE_2D, ao.depth->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT);
            post_pass_view(cb, post_pipeline_ex("depth_r32.frag", VK_FORMAT_R32_SFLOAT, 0), &dv, 1, post_view(dr), b->width, b->height,
                           pz, 32, false, false);
            vk_full_barrier(cb);
        }
        float A = ao.proj[2], B = ao.proj[3];
        VkImage mask = VK_NULL_HANDLE;
        TexBackend *mt = nh ? NULL : ui_mask_build(cb, b, fr);
        if (mt) mask = mt->image, cb = vk_cmd();  // (a dump submits the command buffer)
        // (OptiScaler: the motion vectors and the mask copied too - post images 8 and 26 are made again at another size
        // by the frames without FSR 3.1 in between, a menu or another scene's depth. And once a mask was given, one every
        // frame - cleared where there is none: OptiScaler 10.0.0-pre1 destroys the last dispatch's mask view without
        // forgetting it, and a dispatch without a mask left it to be destroyed again at the next one - the crash in the
        // NVIDIA driver (FSR2_Vk.cpp, fsrTransparencyView), right after a loading screen or a menu)
        static TexBackend fsr_mv, fsr_mask;
        if (fsr3 && !nh && fsr3_optiscaler()) {
            mv = hold_copy_into(&fsr_mv, mv);
            if (mt) mask = hold_copy_into(&fsr_mask, mt)->image;
            else if (fsr_mask.image && fsr_mask.width == b->width && fsr_mask.height == b->height) {
                VkCommandBuffer c2 = vk_cmd();
                vk_barrier(c2, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkClearColorValue zero = {{0}};
                VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(c2, fsr_mask.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
                vk_barrier(c2, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
                mask = fsr_mask.image;
            }
            cb = vk_cmd();
        }
        gp_mark(cb, "ours: %s%s", !fsr3 ? "DittoSR" : g_settings.upscaler == UPSCALE_DITTO_FSR4 ? "DittoSR (FSR4)" : "FSR 3.1",
                ip_side_post ? " side resolve (in-between)" : "");
        Fsr3Dispatch d = {.color = in->image, .depth = dr ? dr->image : VK_NULL_HANDLE, .mv = mv->image, .mask = mask,
                          .output = out ? out->image : VK_NULL_HANDLE,
                          .rw = b->width, .rh = b->height, .ow = ow, .oh = oh, .mvw = mv->width, .mvh = mv->height,
                          .jx = taa_jit[2], .jy = taa_jit[3],
                          // the vectors are this uv - the previous one; FSR takes previous - this, in pixels
                          .mv_scale_x = -(float)b->width, .mv_scale_y = -(float)b->height,
                          .near_ = B / (A - 1), .far_ = B / (A + 1), .fov_y = 2.0f * atanf(1.0f / ao.proj[1]),
                          .meters_per_unit = 0.01f, .frame_ms = (float)(fps60_period_ns() / 1e6), .sharpness = g_settings.fsr3_sharpness,
                          .reset = !ao.prev_ok};
        if (nh) {
            TexBackend *so = post_image(30, ow, oh);
            float ps[8] = {1.0f / (float)ow, 1.0f / (float)oh, 1.0f / (float)in->width, 1.0f / (float)in->height, TAA_HISTORY};
            VkImageView si[4] = {raw_view(in), post_view(nh), post_view(mv)};  // (the bytes, as FSR reads them)
            post_pass_view(cb, post_pipeline("side_resolve.frag"), si, 3, post_view(so), ow, oh, ps, 32, false, false);
            v = post_view(so), rb_ = so;
            // ... and sharpened as FSR 3.1 sharpens the real frames (RCAS, the same strength: its stops 2 - 2 x sharpness),
            // or the in-between images were softer than the real ones and the detail pulsed at the frame rate
            // (DittoSR: its sharpening only where the image holds still, its in-between images as N's output - neither
            // RCAS nor the line pass. LARECOMP_EXP2_MIDLINE: 0 none (default), 1 the line pass, 2 RCAS then the line pass)
            static int midline = -1;
            if (midline < 0) midline = getenv("LARECOMP_EXP2_MIDLINE") ? atoi(getenv("LARECOMP_EXP2_MIDLINE")) : 0;
            bool ditto = UPSCALE_IS_DITTOSR(g_settings.upscaler), exp_line = midline > 0 && ditto;
            if (g_settings.fsr3_sharpness > 0.01f && (!ditto || midline == 2)) {
                TexBackend *r = post_image(31, ow, oh);
                // (0.6 stops weaker than FSR's: the Catmull-Rom history keeps detail of its own - at FSR's strength the
                // in-between images were 4 - 10 % sharper than the real frames, at 0.7 weaker within 3 %; research:
                // LARECOMP_SIDE_SHARP_STOPS)
                // Rendered smaller (vk_tex_lo_begin) the image brings less detail of its own: at 75 % 0.2 stops weaker,
                // at 50 % 0.05 (at 0.6 they were 5 % / 11 % less detailed than the real frames; now within 1 - 3 %)
                static float env = -2;
                if (env == -2) env = getenv("LARECOMP_SIDE_SHARP_STOPS") ? (float)atof(getenv("LARECOMP_SIDE_SHARP_STOPS")) : -1;
                float lf = vk_tex_lo_active() ? ip_lo_factor() : 1.0f;
                float extra = env != -1 ? env : lf >= 0.75f ? 0.6f - 1.6f * (1.0f - lf) : 0.2f - 0.6f * (0.75f - lf);
                float pr[8] = {exp2f(-2.0f * (1.0f - g_settings.fsr3_sharpness) - extra), 0, 0, 0, (float)ow, (float)oh};
                VkImageView ri_[3] = {v};
                post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
                v = post_view(r), rb_ = r;
            }
            if (exp_line) {  // (DittoSR's line pass, as on its real frames)
                // (its own strengths: the history is N's, lined already - LARECOMP_EXP2_MIDLINE_P=<d>,<l>,<b>)
                static float q[3] = {-1, 0, 0};
                if (q[0] < 0) {
                    q[0] = 0.3f, q[1] = -0.5f, q[2] = 0.5f;  // (4x re-rendered: the face's detail 99.5 %, its lines within 1.2 of 255)
                    if (getenv("LARECOMP_EXP2_MIDLINE_P")) sscanf(getenv("LARECOMP_EXP2_MIDLINE_P"), "%f,%f,%f", &q[0], &q[1], &q[2]);
                }
                TexBackend *r = post_image(rb_ == so ? 31 : 30, ow, oh);
                float pl[8] = {q[0], q[1], q[2], 0, (float)ow, (float)oh};
                VkImageView li[3] = {v};
                post_pass(cb, "ditto_line.frag", li, 1, r, pl, false);
                v = post_view(r), rb_ = r;
            }
            TexBackend *u = ip_ui_keep(cb, rb_, nh, false);  // (its interface: N's, ip_ui_keep)
            v = post_view(u), rb_ = u;
        } else if (fsr3 && !vk_tex_lo_active() &&
                   (UPSCALE_IS_FSR4(g_settings.upscaler) ? dittosr_dispatch(cb, &d) : fsr3_dispatch(cb, &d))) {  // (a reduced image never: below, FSR 1)
            // (its compute writes: the next post pass's barrier, or the end's, waits for them)
            v = post_view(out), rb_ = out;
            fsr3_engaged = true, fsr3_rw = b->width, fsr3_rh = b->height, fsr3_ow = ow;
            // The interface after the upscale: FSR 3.1 rebuilds the image as the jittered scene's, the interface too (it is
            // drawn into the same image, not jittered) - with the mask as its transparency and composition one it takes
            // little history there, but each frame's interface lands where that frame's jitter puts it: at 4K (Quality,
            // 1.5x) text and boxes moved a pixel from one frame to the next (shifts of 1 px, the frame's jitter), at the
            // game's rate under in-between images that keep N's. Where the mask says interface, the output takes the
            // image itself, Catmull-Rom from the render size (it is not jittered): still. LARECOMP_FSR3_UI_COMPOSITE=0
            // compares.
            static int uic = -1;
            if (uic < 0) uic = getenv("LARECOMP_FSR3_UI_COMPOSITE") ? atoi(getenv("LARECOMP_FSR3_UI_COMPOSITE")) : 1;
            if (uic && mt) {
                TexBackend *c = post_image(37, ow, oh);
                float pc[8] = {1.0f / (float)ow, 1.0f / (float)oh};
                VkImageView ci[4] = {post_view(out), raw_view(in), post_view(mt)};
                post_pass_view(cb, post_pipeline("ui_composite.frag"), ci, 3, post_view(c), ow, oh, pc, 32, false, false);
                v = post_view(c), rb_ = c;
            }
        } else if (gsr2 && !vk_tex_lo_active() && !ip_side_post) {  // (an in-between image without N's output: FSR 1)
            TexBackend *g = gsr2_run(cb, in, mv, ow, oh);
            v = post_view(g), rb_ = g;
            gsr2_engaged = true, gsr2_rw = b->width, gsr2_ow = ow;
            // Then in one pass: the sharpening and the interface after the upscale, as FSR 3.1's (above): still where
            // the mask says interface (x2_final.frag). The sharpening as the standalone DittoSR's: contrast-adaptive (each
            // channel against its 4 neighbours, by as much as they leave room before black or white), only where the
            // image holds still - none from 2 output pixels a frame, less with a low stability level - the slider x 0.2
            // (1: CAS's strongest)
            static int uic = -1;
            if (uic < 0) uic = getenv("LARECOMP_FSR3_UI_COMPOSITE") ? atoi(getenv("LARECOMP_FSR3_UI_COMPOSITE")) : 1;
            bool ui = uic && mt;
            gp_mark(cb, "ours: DittoSR sharpening, interface");
            TexBackend *c = post_image(47, ow, oh);
            float pf[8] = {0.2f * g_settings.fsr3_sharpness, 0, 0, ui ? 1.0f : 0.0f, (float)ow, (float)oh, 1.0f / (float)ow, 1.0f / (float)oh};
            VkImageView fi[4] = {v, raw_view(in), ui ? post_view(mt) : raw_view(in), post_view(gsr2_mda)};
            post_pass_view(cb, post_pipeline("x2_final.frag"), fi, 4, post_view(c), ow, oh, pf, 32, false, false);
            v = post_view(c), rb_ = c;
        } else {
            fsr3_engaged = false;
            if (!ip_side_post) gsr2_engaged = false;
            fsr = true;  // (FSR 1 below, this time)
        }
    }
    if (fsr3 || gsr2) gp_mark(cb, "ours: upscaler done");  // (LOCAL experiment: the upscalers' own GPU time, to here)
    if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r), rb_ = r;
    }
    if (deband) {  // (at the final size: the radius, 12 pixels at 1080p, follows it)
        uint32_t dw = rb_->width, dh = rb_->height;
        TexBackend *d = post_image(32, dw, dh);
        float pd[8] = {1.0f / (float)dw, 1.0f / (float)dh, 1.25f / 255.0f, 0, (float)dw, (float)dh, 12.0f * (float)dh / 1080.0f};
        VkImageView di[3] = {v};
        post_pass(cb, "deband.frag", di, 1, d, pd, false);
        v = post_view(d), rb_ = d;
    }
    vk_post_barrier(cb);  // (the present's draw samples it; a keep copy waits with a barrier of its own)
    vk_frame_resume();
    if (res) *res = rb_;
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1;
        // (5: DittoSR (FSR4) - a switch between it and FSR 3.1 at the same sizes is a line too)
        int f = fsr3 && fsr3_engaged ? (UPSCALE_IS_FSR4(g_settings.upscaler) ? 5 : 3) : gsr2 && gsr2_engaged ? 4 : fsr;
        if (!ip_side_post && (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || f != pfs))
            LOG("[post] %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "",
                f == 5 ? " dittosr_fsr4" : f == 4 ? " dittosr" : f == 3 ? " fsr3.1" : f ? " fsr1" : "", ow, oh);
        if (!ip_side_post) piw = b->width, pih = b->height, pow_ = ow, poh = oh, psm = smaa, pfs = f;  // (the real frames')
    }
    return v;
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
static VkImageView ip_post_view(TexBackend *b, double ow, double oh);
static float ip_present_sharp;  // (the image drawn now: its IpKeep's sharp - the optical flow's sharpening, flow_image)
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    double ow = pv[6] > 0 ? vw / pv[6] : vw, oh = pv[7] > 0 ? vh / pv[7] : vh;
    ip_present_sharp = 0;
    VkImageView src = ip_post_view(b, ow, oh);  // (an image of a present kept until now: processed when it was made)
    if (!src) src = post_process(cb, b, ow, oh), ip_present_sharp = 0;
    gp_mark(cb, "ours: present to the window, menus%s", g_present_grade[3] > 0.5f ? ", grade" : "");
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    float pc[12];
    memcpy(pc, pv, 32);
    pc[1] = ip_present_sharp;  // (a.y: the optical flow's sharpening, flow_image)
    memcpy(pc + 8, g_present_grade, 16);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[12] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// ================================================================== executor (renderer.m exec_list)
static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    if (depth == 0) st.obj = 0, st.obj_draw = 0;  // (a submitted list starts with no object; its called lists inherit)
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        if (ip_replaying && ip_skip_op(r->op)) { p += r->size; continue; }
        uint64_t op_t0 = prof && r->op != CMD_CALL ? SDL_GetTicksNS() : 0;
        switch (r->op) {
        case CMD_END: return;
        case CMD_OBJECT: st.obj = r->x[0]; st.obj_draw = 0; { static int d; if (getenv("LARECOMP_OBJDBG") && vk_frame_counter() == 1450 && d++ < 80) LOG("[objdbg] depth %d object %llx\n", depth,(unsigned long long)r->x[0]); } break;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            uint64_t obj = st.obj;  // (a called list names objects of its own: they end with it)
            uint32_t obj_draw = st.obj_draw;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) {
                st.obj = obj, st.obj_draw = obj_draw;
                exec_list(h[i], depth + 1);
            }
            st.obj = obj, st.obj_draw = obj_draw;
            break;
        }
        case CMD_BIND_PROGRAM: bind_program_stages((NvnProgram *)(uintptr_t)r->x[0], (uint32_t)r->x[1]); break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = getenv("LARECOMP_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (getenv("LARECOMP_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x color %.3f %.3f %.3f %.3f\n", (unsigned long long)renderer_present_count(),
                    index, st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2],
                    ((const float *)x)[0], ((const float *)x)[1], ((const float *)x)[2], ((const float *)x)[3]);
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            acopy_drain();  // (queued copies read guest memory as it was when their draws were recorded)
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            // (x[0] the sync - x[1] is the condition. With the render thread the sync took this list's ticket when the
            // game submitted it: objects.c nvn_list_submitted)
            if (r->x[0] && !rt_on_render_thread()) nvn_sync_set_seq(r->x[0], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow;
            // the value is our translation time, not GPU time. LARECOMP_VK_COUNTER=frozen: the same timestamp for
            // every counter within a presented frame (experiment).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            acopy_drain();  // (it writes guest memory below)
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            static int frozen = -1;  // 1: frozen; 2: ideal (LARECOMP_VK_COUNTER=ideal: a 33.3 ms frame, 1 ms per counter in it)
            if (frozen < 0) { const char *e = getenv("LARECOMP_VK_COUNTER"); frozen = !e ? 0 : !strcmp(e, "frozen") ? 1 : !strcmp(e, "ideal") ? 2 : 0; }
            static uint64_t frame_seen = ~0ull, frame_ts, in_frame;
            extern uint64_t vk_frame_counter(void);
            if (vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = SDL_GetTicksNS(); in_frame = 0; }
            uint64_t v = frozen == 1 ? frame_ts : frozen == 2 ? frame_seen * 33333333ull + in_frame * 1000000ull : SDL_GetTicksNS();
            in_frame++;
            if (out) { out[0] = 0; out[1] = v; }
            static int clog = -1;  // LARECOMP_COUNTER_LOG=1: each counter, with the draws recorded since the previous one
            if (clog < 0) clog = getenv("LARECOMP_COUNTER_LOG") != NULL;
            static uint64_t last_draws;
            if (clog) LOG("[counter] frame %llu #%llu value %llu draws %llu\n", (unsigned long long)frame_seen, (unsigned long long)in_frame,
                          (unsigned long long)v, (unsigned long long)(g_draws_total - last_draws));
            last_draws = g_draws_total;
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: acopy_drain(); copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER: {
            // The game's own GPU copy, done by the CPU in command order. Waiting for the GPU first (all the work submitted so far)
            // is only needed when the GPU writes the source or the destination, or reads the destination in place when it runs
            // (vertex / index data with the VRAM ring off, the storage buffers of shaders using global memory): the draws recorded
            // before it took their constants and vertex data as copies. Viridian City: two copies a frame (112 and 32 bytes, constants),
            // each one a wait of ~1.2 ms on the render thread. LARECOMP_VK_COPY_WAIT=1: always wait (as before).
            uint64_t src = r->x[0], dst = r->x[1], n = r->x[2];
            bool wait = ENV("LARECOMP_VK_COPY_WAIT") || !ring_gpu_enabled() || gpuw_overlaps(src, n) || gpuw_overlaps(dst, n) || gpur_overlaps(dst, n);
            if (ENV("LARECOMP_LOG_B2B")) {
                static int nlog;
                if (nlog++ < 60) LOG("[b2b] frame %llu %llx -> %llx, %llu bytes; %s\n", (unsigned long long)vk_frame_counter(), (unsigned long long)src,
                                     (unsigned long long)dst, (unsigned long long)n, wait ? "waits for the GPU" : "no wait");
            }
            if (wait) renderer_flush();
            else acopy_drain();
            memmove((void *)(uintptr_t)dst, (void *)(uintptr_t)src, n);
            guest_copy_flush();  // (constant copies shared within the submission are stale now, as after UpdateUniformBuffer)
            break;
        }
        default: break;
        }
        if (op_t0 && r->op < CMD_COUNT) prof_op_ns[r->op] += SDL_GetTicksNS() - op_t0, prof_op_n[r->op]++;
        p += r->size;
    }
}

void backend_execute(uint64_t handle) {
    if (ENV("LARECOMP_EXP_NOEXEC")) return;  // (research: the game's chain without the backend's work; the picture stays blank)
    PROF_T0();
    if (ip_enabled()) ip_note_list(handle);
    exec_list(handle, 0);
    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = getenv("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
    if (sync_submit) renderer_flush();
    else renderer_submit();
}

// ================================================================== frame interpolation (prototype)
// LARECOMP_INTERP=1: 60 fps from the game's 30 by drawing an in-between frame (docs/PESQUISA-60FPS.md). The game's
// logic advances one step per frame, so the frame rate can't simply be raised; instead, like RT64 (Zelda64Recomp)
// and Ship of Harkinian, frame N's draws are run a second time with their constant buffers (camera, world, bone
// matrices) blended halfway between N-1 and N, and that image is shown half a frame before N.
//  - recording: the command lists submitted in a frame (and a hash of their records), the exec state at the frame's
//    start, and a copy of every vertex / fragment constant buffer a draw binds (cbuf 3 and up), keyed by stage,
//    binding, the program, vertex buffer 0, the draw's parameters and the occurrence of that key in the frame;
//  - replay (vk_interp_render, at the present): the same lists from the same start state, skipping what writes guest
//    memory (compute, copies to buffers, inline uniform updates, syncs, counters); each constant buffer is the blend of
//    its two copies, word by word, for normal floats close enough (other words, and draws without a match, keep N);
//  - a list whose records changed since it ran (the game reused its memory) cancels that frame's in-between image.
// (obj: the game object of the draw; stage / k: its cbuf slot; chg: which of its first 64 vec4s changed since its
// partner in the frame before, chg_known once the replay compared them - ip_cbuf's held values)
// pkey: its partner's key in the frame before, once a replay paired them (pkey_ok) - the curve's third point (ip_cbuf)
// (d2: bound by a 2D draw - no depth test: the interface - left out of the per-object cut check, vk_interp_begin)
// (slot: where it is in the frame's table - cleared by it, not by the whole table: ip_frame_clear)
typedef struct { uint64_t key; uint32_t off, size; uint64_t obj; uint8_t stage, k; bool chg_known, pkey_ok, d2; uint64_t chg, pkey; uint32_t slot; } IpEntry;
// A draw of the recorded frame: its identity, occurrence and shape (ip_pair_draw), and its cbufs (entries first ..
// first + count - 1, recorded one after the other while it binds them)
// (vfirst / vcount: its vertex streams recorded, IpVtx: ip_vtx_stream)
typedef struct { uint64_t sig, shape; uint32_t occ, first, count, vfirst, vcount, slot; } IpDraw;
typedef struct { uint32_t off, size, stride; uint8_t s; } IpVtx;
typedef struct {
    IpEntry *e;
    uint32_t n, cap;
    uint8_t *data;
    uint64_t used, dcap;
    uint32_t *tab;  // open addressing: key -> e index + 1
    uint64_t lists[256], hashes[256];
    int nlists;
    bool overflow, valid;
    uint64_t real, period;  // its present (renderer_present_count) and the game's frame time then (fps60_period_ns)
    uint64_t destroyed;  // g_nvn_destroyed when the frame started
    __typeof__(st) start;
    IpDraw *dr;  // its draws (ip_pair_draw)
    uint32_t ndr, drcap;
    uint32_t *drtab;  // open addressing: (sig, occ) -> dr index + 1
    uint32_t *dr_by_shape, dr_by_shape_cap;  // draw indices sorted by shape, built when first needed
    // each draw's partner in the frame before (ip_pair_all, once for the present's in-between images): its index there,
    // IP_PAIR_ANY (none found: ip_cbuf's ip_pick decides) or IP_PAIR_NONE (frame N's constants)
    int32_t *pair;
    int32_t *sib;  // (for a draw without a partner: a draw of N of its shape that has one, or -1)
    float *pair_dist;
    uint32_t paircap;
    bool paired;
    bool dr_sorted;
    IpVtx *vx;  // the draws' small vertex streams (ip_vtx_stream): their bytes in vdata
    uint32_t nvx, vxcap;
    uint8_t *vdata;
    uint64_t vused, vcap;
} IpFrame;
extern uint64_t g_nvn_destroyed;  // (objects.c: textures, pools, programs the game finalized)
#define IP_TAB (1u << 16)
static IpFrame ipf[3];
static int ipc;  // ipf[ipc]: the frame being recorded; ipf[IP_PREV]: the one before; ipf[IP_PREV2]: the one before that
#define IP_PREV ((ipc + 2) % 3)
#define IP_PREV2 ((ipc + 1) % 3)
static float ip_t = 0.5f;
static uint32_t ip_occ[IP_TAB];  // occurrences of each base key in this frame / replay (by slot)
static uint64_t ip_occ_key[IP_TAB];
// A slot is in use in this frame / replay when its generation is the current one: a reset is a new generation, not a
// clear of the 512 KB of keys (it was one per replay and per frame)
static uint32_t ip_occ_gen[IP_TAB], ip_occ_cur = 1;
static uint64_t ip_stats[4];  // blended, kept (no match), words kept, frames replayed
// LARECOMP_INTERP_DIAG=1: what the in-between image is made of (docs/PESQUISA-60FPS.md, "Diagnóstico da interpolação")
typedef struct { int stage, k; uint32_t size; uint64_t draws, nomatch, tagged, words, changed, blended, jumps, nonfloat; } IpDiag;
static IpDiag ip_diag[96];
static int ip_ndiag;
static uint64_t ip_rej[8];  // frames without an in-between image: no previous frame, overflow, destroyed, format, list changed
static IpDiag *ip_diag_for(int stage, int k, uint32_t size) {
    for (int i = 0; i < ip_ndiag; i++)
        if (ip_diag[i].stage == stage && ip_diag[i].k == k && ip_diag[i].size == size) return &ip_diag[i];
    if (ip_ndiag >= 96) return &ip_diag[95];
    ip_diag[ip_ndiag] = (IpDiag){.stage = stage, .k = k, .size = size};
    return &ip_diag[ip_ndiag++];
}
static void ip_diag_dump(void) {
    LOG("[interp-diag] rejected frames: no-prev %llu overflow %llu destroyed %llu format %llu list-changed %llu cut %llu; replayed %llu\n",
        (unsigned long long)ip_rej[0], (unsigned long long)ip_rej[1], (unsigned long long)ip_rej[2], (unsigned long long)ip_rej[3],
        (unsigned long long)ip_rej[4], (unsigned long long)ip_rej[5], (unsigned long long)ip_stats[3]);
    for (int i = 0; i < ip_ndiag; i++) {
        const IpDiag *d = &ip_diag[i];
        LOG("[interp-diag] stage %d cbuf %d size %u: binds %llu (tagged %llu, no match %llu) words %llu changed %llu blended %llu jumps %llu nonfloat %llu\n",
            d->stage, d->k, d->size, (unsigned long long)d->draws, (unsigned long long)d->tagged, (unsigned long long)d->nomatch,
            (unsigned long long)d->words, (unsigned long long)d->changed, (unsigned long long)d->blended, (unsigned long long)d->jumps,
            (unsigned long long)d->nonfloat);
    }
    memset(ip_diag, 0, sizeof ip_diag);
    ip_ndiag = 0;
    memset(ip_rej, 0, sizeof ip_rej);
}

static uint64_t ip_mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0x100000001B3ull; }

static void ip_occ_reset(void) { if (++ip_occ_cur == 0) memset(ip_occ_gen, 0, sizeof ip_occ_gen), ip_occ_cur = 1; }

static uint64_t ip_last_base;  // ip_key's last key without its occurrence, and that occurrence
static uint32_t ip_last_occ;
// The draw's occurrence is counted per draw (its identity), not per cbuf slot: a draw that leaves a slot unbound (one
// of the tall grass' 31 draws binds no vertex cbuf 4) put that slot's count one behind the others for every draw after
// it - each grass clump then took its neighbour's matrices (cbuf 4) with its own camera (cbuf 3), and was blended
// towards it: clumps squashed and shifted, holes in the grass in the in-between images while walking.
static uint32_t ip_occ_next(uint64_t sig) {
    if (!sig) sig = 1;
    uint32_t s = (uint32_t)(sig ^ (sig >> 29)) & (IP_TAB - 1);
    while (ip_occ_gen[s] == ip_occ_cur && ip_occ_key[s] != sig) s = (s + 1) & (IP_TAB - 1);
    if (ip_occ_gen[s] != ip_occ_cur) { ip_occ_gen[s] = ip_occ_cur; ip_occ_key[s] = sig; ip_occ[s] = 0; }
    return ip_occ[s]++;
}
static uint64_t ip_key(int stage, int k) {
    uint64_t base = ip_mix(ip_mix(ip_draw_sig, (uint64_t)stage), (uint64_t)k);
    if (!base) base = 1;
    ip_last_base = base, ip_last_occ = ip_draw_occ;
    return ip_mix(base, ip_draw_occ);
}

static const IpEntry *ip_find(const IpFrame *f, uint64_t key) {
    if (!f->tab) return NULL;
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (IP_TAB - 1);
    while (f->tab[s]) {
        const IpEntry *e = &f->e[f->tab[s] - 1];
        if (e->key == key) return e;
        s = (s + 1) & (IP_TAB - 1);
    }
    return NULL;
}

static void ip_frame_clear(IpFrame *f) {
    f->used = 0; f->nlists = 0; f->overflow = false; f->valid = false; f->dr_sorted = false;
    f->nvx = 0; f->vused = 0; f->paired = false;
    // (the slots its entries took, not the whole tables: 2 x 256 KB a frame for a few thousand entries)
    if (f->drtab) for (uint32_t i = 0; i < f->ndr; i++) f->drtab[f->dr[i].slot] = 0;
    if (f->tab) for (uint32_t i = 0; i < f->n; i++) f->tab[f->e[i].slot] = 0;
    f->n = 0, f->ndr = 0;
}

static void ip_record(uint64_t key, const void *data, uint32_t size, int stage, int k) {
    IpFrame *f = &ipf[ipc];
    if (!f->tab) f->tab = calloc(IP_TAB, sizeof *f->tab);
    if (f->n >= IP_TAB / 2) { f->overflow = true; return; }
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 4096; f->e = realloc(f->e, f->cap * sizeof *f->e); }
    if (f->used + size > f->dcap) { f->dcap = (f->used + size) * 2 + (1 << 20); f->data = realloc(f->data, f->dcap); }
    memcpy(f->data + f->used, data, size);
    f->e[f->n] = (IpEntry){key, (uint32_t)f->used, size, st.obj, (uint8_t)stage, (uint8_t)k};
    f->e[f->n].d2 = ip_draw_2d;
    if (f->ndr) {  // (one more cbuf of the draw being recorded)
        IpDraw *d = &f->dr[f->ndr - 1];
        if (d->sig == ip_draw_sig && d->occ == ip_draw_occ && d->first + d->count == f->n) d->count++;
    }
    f->used += size;
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (IP_TAB - 1);
    while (f->tab[s]) s = (s + 1) & (IP_TAB - 1);
    f->e[f->n].slot = s;
    f->tab[s] = ++f->n;
}

// cbuf_info, constant buffers 3+ of the vertex / fragment stage: recording copies them (returns NULL: bind as usual);
// the replay returns the blended data to bind.
// Objects whose buffers changed too much between N-1 and N to be blended (vk_interp_render's check, before the replay):
// a camera cut, a model swapped (another level of detail, another skeleton) or two draws paired wrongly. Blending them
// mixes unrelated poses - characters stretched across the screen, Pokémon torn apart. They keep frame N's pose.
#define IP_REJ 1024
static uint64_t ip_rej_obj[IP_REJ];
static bool ip_obj_rejected(uint64_t obj) {
    uint32_t s = (uint32_t)((obj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
    while (ip_rej_obj[s]) { if (ip_rej_obj[s] == obj) return true; s = (s + 1) & (IP_REJ - 1); }
    return false;
}
// Which of the previous frame's occurrences of a draw this one is (the replay): a mesh drawn several times without an
// object of its own (flower bushes, grass) is told apart only by the order of its draws, which shifts when one is
// culled or comes into view - the n-th took another bush's constants and was blended towards it (the ghosts that made
// the world's interpolation go, docs/PESQUISA-60FPS.md). The occurrence with the closest copy of this cbuf is taken
// (this one's first, kept unless another is closer), at the draw's first cbuf, and kept for its others.
static uint32_t ip_pick_occ;
static uint32_t ip_pick(uint64_t base, uint32_t occ, const IpEntry *n) {
    const IpFrame *f = &ipf[IP_PREV];
    if (!n || !ip_find(f, ip_mix(base, occ ? 0 : 1))) return occ;  // (one occurrence: itself)
    const uint32_t *nd = (const uint32_t *)(ipf[ipc].data + n->off);
    uint32_t pick = occ;
    float best = INFINITY;
    int gap = 0;
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t c = i == 0 ? occ : i - 1 < occ ? i - 1 : i;  // (this occurrence first, then all the others)
        const IpEntry *o = ip_find(f, ip_mix(base, c));
        if (!o) { if (i > 0 && ++gap > 8) break; continue; }  // (draws that leave this slot unbound: gaps)
        gap = 0;
        if (o->size != n->size) continue;
        float d = mvo_dist((const uint32_t *)(f->data + o->off), nd, n->size / 4, best);
        if (d < best) best = d, pick = c;
        if (best == 0) break;
    }
    return pick;
}

// A whole buffer that jumps between a (N-1) and b (N): a word changed by more than ip_jumps allows (3D draws)
static bool ip_buf_jumps(const uint32_t *a, const uint32_t *b, uint32_t nw) {
    for (uint32_t i = 0; i < nw; i++) {
        if (a[i] == b[i]) continue;
        uint32_t ea = (a[i] >> 23) & 0xFF, eb = (b[i] >> 23) & 0xFF;
        if (!ea || !eb || ea == 0xFF || eb == 0xFF) continue;
        float fa, fb;
        memcpy(&fa, &a[i], 4);
        memcpy(&fb, &b[i], 4);
        if (ip_jumps(fa, fb)) return true;
    }
    return false;
}

// The recorded frame's draws (draw_, before its cbufs): looked up by (identity, occurrence) and by shape
static void ip_draw_record(void) {
    IpFrame *f = &ipf[ipc];
    if (f->ndr >= IP_TAB / 2) { f->overflow = true; return; }
    if (!f->drtab) f->drtab = calloc(IP_TAB, sizeof *f->drtab);
    if (f->ndr == f->drcap) { f->drcap = f->drcap ? f->drcap * 2 : 4096; f->dr = realloc(f->dr, f->drcap * sizeof *f->dr); }
    f->dr[f->ndr] = (IpDraw){ip_draw_sig, ip_draw_shape, ip_draw_occ, f->n, 0};
    uint64_t key = ip_mix(ip_draw_sig, ip_draw_occ);
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (IP_TAB - 1);
    while (f->drtab[s]) s = (s + 1) & (IP_TAB - 1);
    f->dr[f->ndr].slot = s;
    f->drtab[s] = ++f->ndr;
}
static const IpDraw *ip_draw_find(const IpFrame *f, uint64_t sig, uint32_t occ) {
    if (!f->drtab) return NULL;
    uint64_t key = ip_mix(sig, occ);
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (IP_TAB - 1);
    while (f->drtab[s]) {
        const IpDraw *d = &f->dr[f->drtab[s] - 1];
        if (d->sig == sig && d->occ == occ) return d;
        s = (s + 1) & (IP_TAB - 1);
    }
    return NULL;
}
// How far draw c (N-1) is from draw n (N): all their cbufs, which must be the same slots and sizes (else infinite);
// stops past `limit`
static float ip_draw_dist(const IpDraw *n, const IpDraw *c, float limit) {
    const IpFrame *fn = &ipf[ipc], *fc = &ipf[IP_PREV];
    if (c->count != n->count || !n->count) return INFINITY;
    float d = 0;
    for (uint32_t i = 0; i < n->count && d < limit; i++) {
        const IpEntry *en = &fn->e[n->first + i], *ec = &fc->e[c->first + i];
        if (en->stage != ec->stage || en->k != ec->k || en->size != ec->size) return INFINITY;
        d += mvo_dist((const uint32_t *)(fc->data + ec->off), (const uint32_t *)(fn->data + en->off), en->size / 4, limit - d);
    }
    return d;
}
// A partner found by content (not the draw's own identity) that is another copy of the object: a word moved by more than
// a quarter of the two values and 50 units. Viridian, walking right: a mesh drawn twice (at x 1420 and 2670), the second
// coming into the shadow map's view in N - the only candidate in N-1 was the first, 1250 units away (not a jump by
// ip_jumps: half the two values and 100), and the in-between images drew it there: the shadow of a bush on the road
// where there is none. Its own motion between two frames is far below that.
static bool ip_draw_other_copy(const IpDraw *n, const IpDraw *c) {
    const IpFrame *fn = &ipf[ipc], *fc = &ipf[IP_PREV];
    for (uint32_t i = 0; i < n->count; i++) {
        const IpEntry *en = &fn->e[n->first + i], *ec = &fc->e[c->first + i];
        const float *a = (const float *)(fc->data + ec->off), *b = (const float *)(fn->data + en->off);
        for (uint32_t w = 0; w < en->size / 4; w++)
            if (a[w] != b[w] && isfinite(a[w]) && isfinite(b[w]) && fabsf(b[w] - a[w]) > 0.25f * (fabsf(a[w]) + fabsf(b[w])) + 50.0f)
                return true;
    }
    return false;
}
static bool ip_draw_jumps(const IpDraw *n, const IpDraw *c) {
    const IpFrame *fn = &ipf[ipc], *fc = &ipf[IP_PREV];
    for (uint32_t i = 0; i < n->count; i++) {
        const IpEntry *en = &fn->e[n->first + i], *ec = &fc->e[c->first + i];
        if (ip_buf_jumps((const uint32_t *)(fc->data + ec->off), (const uint32_t *)(fn->data + en->off), en->size / 4)) return true;
    }
    return false;
}
static const IpFrame *ip_shape_frame;
static int ip_shape_cmp(const void *x, const void *y) {
    uint64_t a = ip_shape_frame->dr[*(const uint32_t *)x].shape, b = ip_shape_frame->dr[*(const uint32_t *)y].shape;
    return a < b ? -1 : a > b;
}
// Which of the previous frame's draws this 3D draw is (the replay, before its cbufs): of the draws of the same shape
// (program and arguments, any vertex buffer address or object), the one whose cbufs, all of them together, are closest
// to this draw's - its own identity and occurrence first, kept unless another is closer; none that jumps. Picked by one
// cbuf at a time (the first), a cbuf every copy shares decided nothing: the tall grass' 63 clumps have the same camera
// cbuf (3) and their own offsets in the next (4), and ties took any clump's offsets - clumps squashed, shifted and
// missing in the in-between images while walking (offsets 252 -> 49 units where the wind moves them 1 or 2). The game
// also reuses vertex buffers between copies, so the address in the identity can name another copy.
#define IP_PAIR_ANY (-1)
#define IP_PAIR_NONE (-2)
// The partner of draw n (N) in the frame before, as above; *d: how far (0: the same constants)
static int32_t ip_pair_find(const IpDraw *n, float *d_out) {
    IpFrame *fc = &ipf[IP_PREV];
    *d_out = INFINITY;
    if (!n->count || !fc->ndr) return IP_PAIR_ANY;
    const IpDraw *o0 = ip_draw_find(fc, n->sig, n->occ), *best = o0;
    float bd = o0 ? ip_draw_dist(n, o0, INFINITY) : INFINITY;
    if (bd > 0) {
        if (!fc->dr_sorted) {
            if (fc->dr_by_shape_cap < fc->ndr)
                fc->dr_by_shape_cap = fc->ndr * 2, fc->dr_by_shape = realloc(fc->dr_by_shape, fc->dr_by_shape_cap * sizeof *fc->dr_by_shape);
            for (uint32_t i = 0; i < fc->ndr; i++) fc->dr_by_shape[i] = i;
            ip_shape_frame = fc;
            qsort(fc->dr_by_shape, fc->ndr, sizeof *fc->dr_by_shape, ip_shape_cmp);
            fc->dr_sorted = true;
        }
        uint32_t lo = 0, hi = fc->ndr;
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (fc->dr[fc->dr_by_shape[m]].shape < n->shape) lo = m + 1; else hi = m; }
        for (uint32_t i = lo, tried = 0; i < fc->ndr && tried < 2048 && bd > 0; i++, tried++) {
            const IpDraw *c = &fc->dr[fc->dr_by_shape[i]];
            if (c->shape != n->shape) break;
            if (c == o0) continue;
            float d = ip_draw_dist(n, c, bd);
            if (d < bd) bd = d, best = c;
        }
    }
    if (best && best != o0 && ip_draw_jumps(n, best)) best = o0, bd = o0 ? ip_draw_dist(n, o0, INFINITY) : INFINITY;  // (the closest jumps: the identity's own, if any)
    static int other = -1;  // (LARECOMP_INTERP_OTHER_COPY=0: comparison, the check off)
    if (other < 0) other = getenv("LARECOMP_INTERP_OTHER_COPY") ? atoi(getenv("LARECOMP_INTERP_OTHER_COPY")) : 1;
    if (other && best && best != o0 && ip_draw_other_copy(n, best)) {  // (another copy of it: none - frame N's constants, ip_cbuf)
        if (!o0) { ip_stats_other_copy++; return IP_PAIR_NONE; }
        best = o0, bd = ip_draw_dist(n, o0, INFINITY);
    }
    if (!best) return IP_PAIR_ANY;
    if (best != o0) ip_stats_content++;
    *d_out = bd;
    return (int32_t)(best - fc->dr);
}
// Every draw's partner, once a present (the first in-between image), one to one: a draw of N-1 is the partner of the
// draw of N closest to it, the others that wanted it get none and keep frame N's constants. A copy of a mesh that comes
// into view in N takes the place of one in the order of its draws (Pewter, walking right: a tree drawn three times in
// the shadow map, then four, the new one second - 900 - 1250 units from any copy of N-1), and its closest partner is
// another copy's - which that copy, the same in both frames, matches exactly. Drawn there, its shadow showed where there
// is none in the in-between images (and in Viridian the same, a copy 1250 units away). The test for another copy alone
// (ip_draw_other_copy) is loose for coordinates in the thousands; this is not a matter of distance.
static int ip_pair_cmp(const void *x, const void *y) {
    const IpFrame *f = &ipf[ipc];
    float a = f->pair_dist[*(const uint32_t *)x], b = f->pair_dist[*(const uint32_t *)y];
    return a < b ? -1 : a > b;
}
// How far draw c (also N) is from draw n: as ip_draw_dist, both of this frame
static float ip_draw_dist_nn(const IpDraw *n, const IpDraw *c, float limit) {
    const IpFrame *fn = &ipf[ipc];
    if (c->count != n->count || !n->count) return INFINITY;
    float d = 0;
    for (uint32_t i = 0; i < n->count && d < limit; i++) {
        const IpEntry *en = &fn->e[n->first + i], *ec = &fn->e[c->first + i];
        if (en->stage != ec->stage || en->k != ec->k || en->size != ec->size) return INFINITY;
        d += mvo_dist((const uint32_t *)(fn->data + ec->off), (const uint32_t *)(fn->data + en->off), en->size / 4, limit - d);
    }
    return d;
}
static void ip_pair_all(void) {
    IpFrame *f = &ipf[ipc], *fc = &ipf[IP_PREV];
    f->paired = true;
    if (f->paircap < f->ndr) {
        f->paircap = f->ndr * 2;
        f->pair = realloc(f->pair, f->paircap * sizeof *f->pair);
        f->pair_dist = realloc(f->pair_dist, f->paircap * sizeof *f->pair_dist);
        f->sib = realloc(f->sib, f->paircap * sizeof *f->sib);
    }
    uint32_t *order = malloc((f->ndr + 1) * sizeof *order), no = 0;
    for (uint32_t i = 0; i < f->ndr; i++) {
        f->pair[i] = ip_pair_find(&f->dr[i], &f->pair_dist[i]);
        if (f->pair[i] >= 0) order[no++] = i;
    }
    static int one = -1;  // (LARECOMP_INTERP_ONE_TO_ONE=0: comparison, a partner may serve several draws)
    if (one < 0) one = getenv("LARECOMP_INTERP_ONE_TO_ONE") ? atoi(getenv("LARECOMP_INTERP_ONE_TO_ONE")) : 1;
    if (one && no) {
        qsort(order, no, sizeof *order, ip_pair_cmp);
        uint8_t *taken = calloc(fc->ndr + 1, 1);
        for (uint32_t j = 0; j < no; j++) {
            uint32_t i = order[j];
            if (taken[f->pair[i]]) { f->pair[i] = IP_PAIR_NONE; ip_stats_taken++; }
            else taken[f->pair[i]] = 1;
        }
        free(taken);
    }
    free(order);
    // A draw left without a partner (a copy coming into view) and a draw of N of the same shape that has one: its
    // sibling (ip_cbuf - the words the two share move as the sibling's do)
    uint32_t *by_shape = malloc((f->ndr + 1) * sizeof *by_shape), nb = 0;
    for (uint32_t i = 0; i < f->ndr; i++) {
        f->sib[i] = -1;
        if (f->pair[i] >= 0) by_shape[nb++] = i;
    }
    ip_shape_frame = f;
    qsort(by_shape, nb, sizeof *by_shape, ip_shape_cmp);
    for (uint32_t i = 0; i < f->ndr; i++) {
        if (f->pair[i] != IP_PAIR_NONE) continue;
        uint32_t lo = 0, hi = nb;
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (f->dr[by_shape[m]].shape < f->dr[i].shape) lo = m + 1; else hi = m; }
        float bd = INFINITY;  // (the nearest: a copy beside it moves on the screen as it would)
        for (uint32_t j = lo, tried = 0; j < nb && tried < 512 && f->dr[by_shape[j]].shape == f->dr[i].shape; j++, tried++) {
            float d = ip_draw_dist_nn(&f->dr[i], &f->dr[by_shape[j]], bd);
            if (d < bd) bd = d, f->sib[i] = (int32_t)by_shape[j];
        }
    }
    free(by_shape);
}
// This 3D draw's partner (the replay, before its cbufs)
static void ip_pair_draw(void) {
    IpFrame *f = &ipf[ipc], *fc = &ipf[IP_PREV];
    const IpDraw *n = ip_draw_find(f, ip_draw_sig, ip_draw_occ);
    if (!n) return;
    if (!f->paired) ip_pair_all();
    int32_t p = f->pair[n - f->dr];
    if (p == IP_PAIR_ANY) return;
    if (p == IP_PAIR_NONE) { ip_part_set = true, ip_part_sig = 0, ip_part_occ = ~0u, ip_sib = f->sib[n - f->dr]; return; }
    ip_part_set = true, ip_part_sig = fc->dr[p].sig, ip_part_occ = fc->dr[p].occ;
}

// LARECOMP_INTERP_KEPT_LOG's window: those presents' first in-between image
static bool ip_kept_log_now(void) {
    static long at = -2, last;
    if (at == -2) {
        const char *e = getenv("LARECOMP_INTERP_KEPT_LOG");
        at = e ? atol(e) : -1, last = e && strchr(e, ':') ? atol(strchr(e, ':') + 1) : at;
    }
    long pc_ = (long)renderer_present_count();
    return at >= 0 && pc_ >= at && pc_ <= last && ip_t < 0.5f + 1e-3f && ip_t > 1.0f / 9;
}
// LARECOMP_INTERP_VTX=1 (research, off): vertex data the game writes each frame (the CPU's: sprites, particles,
// instance transforms - Viridian has 102 instances of a mesh with a matrix each in four instance streams, changed every
// frame) moves at the game's rate in the in-between images: the replay blends constant buffers only. Measured, it was
// not worth it in the scenes tried: the swaying flowers already come halfway through their cbufs, and the tall grass
// clumps beside the sign went missing in an in-between image (another clump's streams of the same size paired). It
// works so: a blended draw's streams up to 64 KB are recorded
// with the frame (N's bytes then come from the record, not from memory the game may be writing again) and, in the
// replay, with a partner stream of the same size and stride in N-1 (the draw ip_pair_draw / ip_pick paired): an
// instance stream's 32-bit float attributes are blended (its transform), a vertex stream's position only (the first
// attribute) - its texture coordinates and colours stay N's: a sprite changed is not smeared. A vertex or instance whose
// value moved more than 35 % of the stream's extent there and 5 % of its size (a particle reborn elsewhere, a quad reused
// for another glyph) keeps N - the size's share lets a rotation's terms, the same in every instance, sway.
#define IP_VTX_MAX 65536
static uint64_t ip_stats_vtx;  // (vertices blended)
static const IpVtx *ip_vtx_find(const IpFrame *f, const IpDraw *d, int s) {
    for (uint32_t i = 0; d && i < d->vcount; i++)
        if (f->vx[d->vfirst + i].s == s) return &f->vx[d->vfirst + i];
    return NULL;
}
static bool ip_vtx_stream(int s, uint64_t addr, uint64_t need, uint32_t stride, bool inst, VkSpan *out) {
    if (!stride || need > IP_VTX_MAX || need < stride) return false;
    if (!ip_replaying) {  // (recording: bound as usual)
        IpFrame *f = &ipf[ipc];
        if (!f->ndr) return false;
        IpDraw *d = &f->dr[f->ndr - 1];
        if (d->sig != ip_draw_sig || d->occ != ip_draw_occ || f->vused + need > (32u << 20)) return false;
        if (f->nvx == f->vxcap) f->vxcap = f->vxcap ? f->vxcap * 2 : 1024, f->vx = realloc(f->vx, f->vxcap * sizeof *f->vx);
        if (f->vused + need > f->vcap) f->vcap = (f->vused + need) * 2 + (1 << 20), f->vdata = realloc(f->vdata, f->vcap);
        memcpy(f->vdata + f->vused, (const void *)(uintptr_t)addr, need);
        if (!d->vcount) d->vfirst = f->nvx;
        if (d->vfirst + d->vcount != f->nvx) return false;  // (one run per draw)
        f->vx[f->nvx++] = (IpVtx){(uint32_t)f->vused, (uint32_t)need, stride, (uint8_t)s};
        d->vcount++;
        f->vused += need;
        return false;
    }
    const IpFrame *fn = &ipf[ipc], *fo = &ipf[IP_PREV];
    const IpDraw *dn = ip_draw_find(fn, ip_draw_sig, ip_draw_occ);
    const IpDraw *dp = ip_part_set ? ip_draw_find(fo, ip_part_sig, ip_part_occ)
                                   : ip_draw_find(fo, ip_draw_sig, ip_pick_set ? ip_pick_occ : ip_draw_occ);
    const IpVtx *vn = ip_vtx_find(fn, dn, s);
    if (ip_kept_log_now()) {  // (LARECOMP_INTERP_KEPT_LOG: each small stream of a blended draw, its position attribute)
        const IpVtx *v2 = ip_vtx_find(fo, dp, s);
        LOG("[interp-kept] present %llu vertex stream %d need %llu stride %u: N %u, N-1 %u%s; a0 fmt %#x stream %u +%u (%d attribs): %s\n",
            (unsigned long long)renderer_present_count(), s, (unsigned long long)need, stride, vn ? vn->size : 0, v2 ? v2->size : 0,
            vn && v2 && vn->size == v2->size && !memcmp(fn->vdata + vn->off, fo->vdata + v2->off, vn->size) ? " (same)" : "",
            st.nattrib ? st.attrib[0] & 0xFF : 0, st.nattrib ? (st.attrib[0] >> 8) & 0x1F : 0, st.nattrib ? st.attrib[0] >> 13 : 0,
            st.nattrib, ip_sig_txt);
    }
    if (!vn || vn->size != need || vn->stride != stride) return false;
    VkSpan sp = vk_transient(need + 16, 16);
    const uint8_t *bn = fn->vdata + vn->off;
    memcpy(sp.ptr, bn, need);
    *out = sp;
    const IpVtx *vo = ip_vtx_find(fo, dp, s);
    bool rejected = !ip_draw_2d && ip_obj_rejected(st.obj ? st.obj : 1);
    if (!vo || vo->size != need || vo->stride != stride || rejected || !memcmp(fo->vdata + vo->off, bn, need))
        return true;  // (N's own bytes)
    const uint8_t *bo = fo->vdata + vo->off;
    uint8_t *dst = sp.ptr;
    uint32_t nv = need / stride;
    for (int ai = 0; ai < st.nattrib && ai < 32 && (inst || ai == 0); ai++) {
        uint32_t pa = st.attrib[ai], fmt = pa & 0xFF, comps = fmt == 0x0A ? 1 : fmt == 0x16 ? 2 : fmt == 0x22 ? 3 : fmt == 0x2E ? 4 : 0;
        uint32_t aoff = pa >> 13;
        if (!comps || ((pa >> 8) & 0x1F) != (uint32_t)s || aoff + comps * 4 > stride) continue;
        float lo[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, hi[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
        for (uint32_t v = 0; v < nv; v++)
            for (uint32_t c = 0; c < comps; c++) {
                float x;
                memcpy(&x, bn + v * stride + aoff + c * 4, 4);
                if (isfinite(x)) lo[c] = fminf(lo[c], x), hi[c] = fmaxf(hi[c], x);
            }
        for (uint32_t v = 0; v < nv; v++) {
            float a[4], b[4];
            memcpy(a, bo + v * stride + aoff, comps * 4);
            memcpy(b, bn + v * stride + aoff, comps * 4);
            bool ok = memcmp(a, b, comps * 4) != 0;
            for (uint32_t c = 0; c < comps && ok; c++)
                if (!isfinite(a[c]) || !isfinite(b[c]) ||
                    fabsf(b[c] - a[c]) > 0.35f * (hi[c] - lo[c]) + 0.05f * fmaxf(fabsf(a[c]), fabsf(b[c])))
                    ok = false;
            if (!ok) continue;
            for (uint32_t c = 0; c < comps; c++) b[c] = a[c] + (b[c] - a[c]) * ip_t;
            memcpy(dst + v * stride + aoff, b, comps * 4);
            ip_stats_vtx++;
        }
    }
    return true;
}

// The blend of each cbuf, decided once a present (the point of "record once, run k - 1 times"): what ip_cbuf_blend
// decides word by word - kept at N, switched half-way (held values), along the line or the curve through N-2 - does not
// depend on t, only its result does. The present's first in-between image decides and keeps it (kinds and the
// polynomial's coefficients: v(t) = c0 + t (c1 + t c2)), the others only evaluate it - with 3 or 4 images a present the
// blend's work was most of the replays' constant buffer time (0.8 of their 1.1 ms on the CPU, 30 fps x 3, Pallet Town).
// Per entry of the recorded frame (its index): 1 none (bound as recorded), 2 frame N's own, 3 frame N's with the words
// of `cnt` sparse entries from off blended (most of a buffer stays N's: copied whole, then those words written).
typedef struct { uint32_t gen, kind, cnt; uint64_t off; } IpPlan;
// kind 1: the polynomial; 2: a switch at t = 0.5 (c0 / c1: the bits); 3: a term of a turning matrix (ip_rot_blend):
// (c3 + t c4) (c0 + sin(c5 t) c1 + (1 - cos(c5 t)) c2)
#define IP_PC_N 6
typedef struct { uint32_t i, kind; float c[IP_PC_N]; } IpPlanWord;
static IpPlan *ip_plan;
static uint32_t ip_plan_cap, ip_plan_cur = 1;
static IpPlanWord *ip_pw;  // the present's blended words
static uint64_t ip_pwn, ip_pwcap;
static uint8_t *ip_pk;  // (the entry being decided, per word: 0 frame N's, else its kind)
static float *ip_pc;    // (... and c0, c1, c2)
static bool ip_plan_on;  // (this present has more than one in-between image: vk_interp_begin)
static bool ip_plan_verify;
static uint8_t *ip_cap_k;  // (the entry being decided: its words' kinds and coefficients, ip_cbuf_blend fills them)
static float *ip_cap_c;
static bool ip_plan_has(uint32_t ni) { return ip_plan_on && ni < ip_plan_cap && ip_plan[ni].gen == ip_plan_cur; }
static const void *ip_plan_eval(uint32_t ni, const IpEntry *n) {
    const IpPlan *pl = &ip_plan[ni];
    const uint8_t *nd = ipf[ipc].data + n->off;
    if (pl->kind == 1) return NULL;
    if (pl->kind == 2) return nd;
    static uint32_t out[0x10000 / 4];
    memcpy(out, nd, n->size);
    float t = ip_t;
    for (const IpPlanWord *w = ip_pw + pl->off, *e = w + pl->cnt; w < e; w++) {
        if (w->kind == 1) { float v = w->c[0] + t * (w->c[1] + t * w->c[2]); memcpy(&out[w->i], &v, 4); }
        else if (w->kind == 3) {
            float a = w->c[5] * t, v = (w->c[3] + t * w->c[4]) * (w->c[0] + sinf(a) * w->c[1] + (1.0f - cosf(a)) * w->c[2]);
            memcpy(&out[w->i], &v, 4);
        } else memcpy(&out[w->i], t < 0.5f ? &w->c[0] : &w->c[1], 4);
    }
    return out;
}
static bool ip_plan_capture_begin(const IpEntry *n) {
    if (!ip_plan_on) return false;
    const IpFrame *f = &ipf[ipc];
    if (ip_plan_cap < f->n) {
        uint32_t nc = f->n * 2;
        ip_plan = realloc(ip_plan, nc * sizeof *ip_plan);
        memset(ip_plan + ip_plan_cap, 0, (nc - ip_plan_cap) * sizeof *ip_plan);
        ip_plan_cap = nc;
    }
    if (!ip_pk) ip_pk = malloc(0x10000 / 4), ip_pc = malloc(0x10000 / 4 * IP_PC_N * sizeof *ip_pc);
    ip_cap_k = ip_pk;
    ip_cap_c = ip_pc;
    memset(ip_cap_k, 0, n->size / 4);  // (its words only)
    return true;
}
static const void *ip_plan_capture_end(uint32_t ni, const IpEntry *n, const void *r) {
    ip_cap_k = NULL, ip_cap_c = NULL;
    const uint8_t *nd = ipf[ipc].data + n->off;
    IpPlan *pl = &ip_plan[ni];
    pl->gen = ip_plan_cur, pl->kind = !r ? 1 : r == nd ? 2 : 3, pl->off = ip_pwn, pl->cnt = 0;
    if (pl->kind != 3) return r;
    uint32_t nw = n->size / 4;
    if (ip_pwn + nw > ip_pwcap) ip_pwcap = (ip_pwn + nw) * 2, ip_pw = realloc(ip_pw, ip_pwcap * sizeof *ip_pw);
    for (uint32_t i = 0; i < nw; i++)
        if (ip_pk[i]) {
            IpPlanWord *w = &ip_pw[ip_pwn + pl->cnt++];
            w->i = i, w->kind = ip_pk[i];
            memcpy(w->c, &ip_pc[i * IP_PC_N], sizeof w->c);
        }
    ip_pwn += pl->cnt;
    if (!pl->cnt) { pl->kind = 2; return nd; }  // (nothing blended: frame N's words as they are)
    return ip_plan_eval(ni, n);  // (this image too: every image of the present by the same arithmetic)
}

// Matrices that turn (ip_cbuf_blend, after its blend): three vec4 rows whose 3 x 3 part is a rotation with a uniform
// scale in N-1 and in N, turned between them by more than 3 degrees, and blended along the line - they go along the
// arc instead: A(t) = A(N-1) Rot(axis, angle t) (Rodrigues), the scale along the line. Blended term by term, a matrix
// half-way between two orientations is not a rotation any more - it shrinks and shears what it moves (by cos(angle/2):
// 30 degrees a frame, 3 %; 90, 29 %): the tall grass rustling round a wild Pokemon, a clump's eight bones swinging tens of
// degrees a frame, drew blades cut short and bent in the in-between images (Route 2). The translation (the rows' fourth
// words) stays along the line. LARECOMP_INTERP_SLERP=0 (comparison): term by term.
static uint64_t ip_stats_rot;  // (matrices blended along the arc)
static bool ip_rot_of(const float m[9], float *s) {  // (rows orthogonal and of one length: *s)
    float n0 = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), n1 = sqrtf(m[3] * m[3] + m[4] * m[4] + m[5] * m[5]),
          n2 = sqrtf(m[6] * m[6] + m[7] * m[7] + m[8] * m[8]);
    if (!(n0 > 1e-6f) || fabsf(n1 - n0) > 2e-3f * n0 || fabsf(n2 - n0) > 2e-3f * n0) return false;
    float d01 = m[0] * m[3] + m[1] * m[4] + m[2] * m[5], d02 = m[0] * m[6] + m[1] * m[7] + m[2] * m[8],
          d12 = m[3] * m[6] + m[4] * m[7] + m[5] * m[8], n2s = n0 * n0;
    if (fabsf(d01) > 2e-3f * n2s || fabsf(d02) > 2e-3f * n2s || fabsf(d12) > 2e-3f * n2s) return false;
    float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (!(det > 0)) return false;
    *s = n0;
    return true;
}
static void ip_rot_blend(const uint32_t *a, const uint32_t *b, uint32_t *buf, uint32_t nw) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_INTERP_SLERP") ? atoi(getenv("LARECOMP_INTERP_SLERP")) : 1;
    if (!on) return;
    for (uint32_t g = 0; g + 11 <= nw && g < 1024; g += 4) {
        static const int idx[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
        float ma[9], mb[9], sa, sb;
        bool lin = true, any = false;
        for (int j = 0; j < 9 && lin; j++) {
            uint32_t w = g + idx[j];
            memcpy(&ma[j], &a[w], 4), memcpy(&mb[j], &b[w], 4);
            if (!isfinite(ma[j]) || !isfinite(mb[j])) { lin = false; break; }
            if (a[w] == b[w]) { lin = buf[w] == b[w]; continue; }
            any = true;
            float v = ma[j] + (mb[j] - ma[j]) * ip_t, got;  // (blended along the line by the pass before: the same bits)
            memcpy(&got, &buf[w], 4);
            if (got != v || (ip_cap_k && ip_cap_k[w] != 1)) lin = false;
        }
        if (!lin || !any || !ip_rot_of(ma, &sa) || !ip_rot_of(mb, &sb) || sb > 2 * sa || sa > 2 * sb) continue;
        float ra[9], rb[9];
        for (int j = 0; j < 9; j++) ra[j] = ma[j] / sa, rb[j] = mb[j] / sb;
        float rr[9];  // ra^T rb
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) rr[r * 3 + c] = ra[r] * rb[c] + ra[3 + r] * rb[3 + c] + ra[6 + r] * rb[6 + c];
        float cs = (rr[0] + rr[4] + rr[8] - 1) * 0.5f;
        cs = cs > 1 ? 1 : cs < -1 ? -1 : cs;
        float th = acosf(cs), sn = sinf(th);
        if (th < 0.05f || th > 2.6f || sn < 1e-3f) continue;  // (3 to 150 degrees: below, the line is the arc)
        float ax = (rr[7] - rr[5]) / (2 * sn), ay = (rr[2] - rr[6]) / (2 * sn), az = (rr[3] - rr[1]) / (2 * sn);
        float kk[9] = {0, -az, ay, az, 0, -ax, -ay, ax, 0}, k2[9], ak[9], ak2[9];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                k2[r * 3 + c] = kk[r * 3] * kk[c] + kk[r * 3 + 1] * kk[3 + c] + kk[r * 3 + 2] * kk[6 + c];
            }
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                ak[r * 3 + c] = ra[r * 3] * kk[c] + ra[r * 3 + 1] * kk[3 + c] + ra[r * 3 + 2] * kk[6 + c];
                ak2[r * 3 + c] = ra[r * 3] * k2[c] + ra[r * 3 + 1] * k2[3 + c] + ra[r * 3 + 2] * k2[6 + c];
            }
        float at = th * ip_t, s = sa + (sb - sa) * ip_t;
        for (int j = 0; j < 9; j++) {
            uint32_t w = g + idx[j];
            float v = s * (ra[j] + sinf(at) * ak[j] + (1 - cosf(at)) * ak2[j]);
            memcpy(&buf[w], &v, 4);
            if (ip_cap_k) {
                float *c = &ip_cap_c[w * IP_PC_N];
                ip_cap_k[w] = 3, c[0] = ra[j], c[1] = ak[j], c[2] = ak2[j], c[3] = sa, c[4] = sb - sa, c[5] = th;
            }
        }
        ip_stats_rot++;
        g += 8;  // (its rows done: the next matrix after them)
    }
}

static const void *ip_cbuf_blend(int stage, int k, uint32_t size, const IpEntry *n);
static const void *ip_cbuf(int stage, int k, uint64_t addr, uint32_t size) {
    if (stage > 1) return NULL;
    uint64_t key = ip_key(stage, k);
    static const char *sig_args = (const char *)1;
    if (sig_args == (const char *)1) sig_args = getenv("LARECOMP_INTERP_SIG_ARGS");  // (only draws whose identity has this text; any size)
    if (ip_sig_dbg && (sig_args ? strstr(ip_sig_txt, sig_args) != NULL : (long)size == ip_sig_dbg)) {
        LOG("[interp-sig] present %llu stage %d cbuf %d occ %u 3d %d: %s\n", (unsigned long long)renderer_present_count(), stage, k,
            ip_last_occ, (int)ip_draw_obj, ip_sig_txt);
        if (getenv("LARECOMP_INTERP_SIG_DUMP") && (long)ip_last_occ < atol(getenv("LARECOMP_INTERP_SIG_DUMP"))) {  // (its words, as floats)
            const float *fv = (const float *)(uintptr_t)addr;
            static char line[40000]; int ln = 0;
            for (uint32_t i = 0; i < size / 4 && ln < 39000; i++) ln += snprintf(line + ln, sizeof line - ln, " %g", fv[i]);
            LOG("[interp-sig]   words:%s\n", line);
        }
    }
    if (!ip_replaying) { ip_record(key, (const void *)(uintptr_t)addr, size, stage, k); return NULL; }
    const IpEntry *n = ip_find(&ipf[ipc], key);
    if (!n) return ip_cbuf_blend(stage, k, size, n);
    uint32_t ni = (uint32_t)(n - ipf[ipc].e);
    if (ip_plan_has(ni)) {  // (decided at the present's first in-between image)
        const void *pe = ip_plan_eval(ni, n);
        if (ip_plan_verify) {  // (LARECOMP_INTERP_PLAN=2, validation: the blend done again, compared)
            static uint32_t keep[0x10000 / 4];
            if (pe && pe != ipf[ipc].data + n->off) memcpy(keep, pe, n->size), pe = keep;
            const void *full = ip_cbuf_blend(stage, k, size, n);
            static uint64_t nchk, ndiff, nkind;
            static float worst;
            nchk++;
            if ((full == NULL) != (pe == NULL)) nkind++;
            else if (full && pe) {
                const float *x = full, *y = pe;
                for (uint32_t i = 0; i < n->size / 4; i++)
                    if (memcmp(&x[i], &y[i], 4)) {
                        ndiff++;
                        float d = isfinite(x[i]) && isfinite(y[i]) ? fabsf(x[i] - y[i]) / fmaxf(fabsf(x[i]), 1.0f) : INFINITY;
                        if (d > worst) worst = d;
                    }
            }
            if (nchk % 20000 == 0 || nchk == 1000)
                LOG("[interp-plan] %llu cbufs checked: %llu words differ (worst relative %g), %llu kept / blended mismatches\n",
                    (unsigned long long)nchk, (unsigned long long)ndiff, (double)worst, (unsigned long long)nkind);
        }
        return pe;
    }
    bool cap = ip_plan_capture_begin(n);
    const void *r = ip_cbuf_blend(stage, k, size, n);
    return cap ? ip_plan_capture_end(ni, n, r) : r;
}
static const void *ip_cbuf_blend(int stage, int k, uint32_t size, const IpEntry *n) {
    const IpEntry *o;
    if (ip_part_set) o = ip_find(&ipf[IP_PREV], ip_mix(ip_mix(ip_mix(ip_part_sig, (uint64_t)stage), (uint64_t)k), ip_part_occ));
    else {
        if (!ip_pick_set) ip_pick_occ = ip_pick(ip_last_base, ip_last_occ, n), ip_pick_set = true;
        o = ip_find(&ipf[IP_PREV], ip_mix(ip_last_base, ip_pick_occ));
    }
    static int diag = -1;
    if (diag < 0) diag = getenv("LARECOMP_INTERP_DIAG") ? 1 : 0;
    IpDiag *dg = diag ? ip_diag_for(stage, k, size) : NULL;
    if (dg) { dg->draws++; if (ip_draw_obj) dg->tagged++; if (!o) dg->nomatch++; }
    if (!n) return NULL;
    const uint8_t *nd = ipf[ipc].data + n->off;
    {   // LARECOMP_INTERP_KEEP_OBJ=<hex object> (research): that object's draws keep frame N's constants
        static uint64_t keep_obj = 1;
        if (keep_obj == 1) keep_obj = getenv("LARECOMP_INTERP_KEEP_OBJ") ? strtoull(getenv("LARECOMP_INTERP_KEEP_OBJ"), NULL, 16) : 0;
        if (keep_obj && st.obj == keep_obj) return nd;
        // LARECOMP_INTERP_KEEP_RT=<width> (research): the draws into a target of that width (the game's size) keep N
        static long keep_rt = -1;
        if (keep_rt < 0) keep_rt = getenv("LARECOMP_INTERP_KEEP_RT") ? atol(getenv("LARECOMP_INTERP_KEEP_RT")) : 0;
        if (keep_rt) {
            NvnTexture *t0 = st.rt.ncolor && st.rt.color[0] ? st.rt.color[0] : st.rt.depth;
            if (t0 && (long)t0->width == keep_rt) return nd;
        }
        // LARECOMP_INTERP_KEEP_SIG=<text>[|<text>...] (research): the draws whose identity (as LARECOMP_INTERP_SIG_LOG prints
        // it: program, vertex buffer, object, arguments) has one of these texts keep frame N's constants
        static const char *keep_sig = (const char *)1;
        if (keep_sig == (const char *)1) keep_sig = getenv("LARECOMP_INTERP_KEEP_SIG");
        for (const char *q = keep_sig; q && *q;) {
            const char *e = strchr(q, '|');
            size_t len = e ? (size_t)(e - q) : strlen(q);
            char one[96];
            if (len && len < sizeof one) {
                memcpy(one, q, len), one[len] = 0;
                if (strstr(ip_sig_txt, one)) return nd;
            }
            if (!e) break;
            q = e + 1;
        }
    }
    // (the object check is the 3D draws': an interface element fading or scaling changes by large steps; a 2D draw has
    // the strict jump test below instead)
    bool rejected = ip_draw_obj && !ip_draw_2d && ip_obj_rejected(st.obj ? st.obj : 1);
    if (rejected) ip_kept_why = 2;
    {   // LARECOMP_INTERP_KEPT_LOG=<present>[:<last>] (research): at those presents' first in-between image, each cbuf kept at frame N
        // and why (2d: no depth test; no-match; rejected; jump below), with how much it changed from its partner in N-1 (by
        // order, ip_pick): a draw kept at N that changed is motion shown at the game's rate
        long pc_ = (long)renderer_present_count();
        if (ip_kept_log_now()) {
            const char *why = !ip_draw_obj ? "2d" : rejected ? "rejected" : !o || o->size != n->size ? "no-match" : NULL;
            if (why) {
                uint32_t ch = 0; float mx = 0;
                if (o && o->size == n->size) {
                    const float *fa = (const float *)(ipf[IP_PREV].data + o->off), *fb = (const float *)nd;
                    for (uint32_t i = 0; i < n->size / 4; i++)
                        if (fa[i] != fb[i]) { ch++; if (isfinite(fa[i]) && isfinite(fb[i]) && fabsf(fb[i] - fa[i]) > mx) mx = fabsf(fb[i] - fa[i]); }
                }
                NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
                LOG("[interp-kept] present %ld %s stage %d cbuf %d size %u changed %u (max %g) rt %ux%u depth %d blend %d: %s\n", pc_, why, stage, k,
                    size, ch, mx, t0 ? t0->width : 0, t0 ? t0->height : 0, st.rt.depth != NULL, (int)(st.color_state[0] & 1), ip_sig_txt);
            }
        }
    }
    if (!ip_draw_obj || rejected) { ip_stats[1]++; return nd; }  // interface and other draws no object owns: their order and text change between frames, a blend smears them
    // A copy coming into view, without a partner (ip_pair_all): each of its words changes from N as the nearest copy of
    // it that has one (its sibling) does at that word - the sibling's own blend, applied as a change. Where the two are
    // still (where they stand in the world, their pose) it stays N's; where the camera or the light is folded in, it
    // moves as the copy beside it. Kept at N whole it was drawn with N's camera in images whose camera is part of the way
    // from N-1: grass clumps coming into view while the camera scrolled sat ahead of the ground around them (Route 2,
    // walking down out of the gate). LARECOMP_INTERP_SIBLING=0 (comparison): kept at N whole.
    static int sib_on = -1;
    if (sib_on < 0) sib_on = getenv("LARECOMP_INTERP_SIBLING") ? atoi(getenv("LARECOMP_INTERP_SIBLING")) : 1;
    if (!o && sib_on && ip_part_set && ip_part_sig == 0) ip_stats_sibw[ip_sib < 0 ? 0 : 1]++;
    if (!o && sib_on && ip_sib >= 0 && ip_part_set) {
        const IpFrame *fn = &ipf[ipc], *fp = &ipf[IP_PREV];
        const IpDraw *sd = &fn->dr[ip_sib];
        int32_t sp = fn->pair[ip_sib];
        const IpEntry *sn = NULL, *so = NULL;
        for (uint32_t i = 0; sp >= 0 && i < sd->count; i++) {
            const IpEntry *e = &fn->e[sd->first + i];
            if (e->stage == stage && e->k == k && e->size == n->size) { sn = e; break; }
        }
        if (sn) {
            const IpDraw *pd = &fp->dr[sp];
            for (uint32_t i = 0; i < pd->count; i++) {
                const IpEntry *e = &fp->e[pd->first + i];
                if (e->stage == stage && e->k == k && e->size == n->size) { so = e; break; }
            }
        }
        if (sn && so) {
            static uint32_t sbuf[0x10000 / 4];
            const uint32_t *b = (const uint32_t *)nd, *s1 = (const uint32_t *)(fn->data + sn->off), *s0 = (const uint32_t *)(fp->data + so->off);
            uint32_t nw = n->size / 4, moved = 0;
            memcpy(sbuf, nd, n->size);
            bool jump = false;
            for (uint32_t w = 0; w < nw && !jump; w++) {
                if (s0[w] == s1[w]) continue;
                float f0, f1, fb;
                memcpy(&f0, &s0[w], 4);
                memcpy(&f1, &s1[w], 4);
                memcpy(&fb, &b[w], 4);
                if (!isfinite(f0) || !isfinite(f1) || !isfinite(fb)) continue;
                if (ip_jumps(f0, f1)) jump = true;  // (the sibling's own jumps: N whole, as before)
            }
            for (uint32_t w = 0; w < nw && !jump; w++) {
                if (s0[w] == s1[w]) continue;
                float f0, f1, fb;
                memcpy(&f0, &s0[w], 4);
                memcpy(&f1, &s1[w], 4);
                memcpy(&fb, &b[w], 4);
                if (!isfinite(f0) || !isfinite(f1) || !isfinite(fb)) continue;
                float v = fb - (f1 - f0) * (1.0f - ip_t);
                memcpy(&sbuf[w], &v, 4);
                if (ip_cap_k) ip_cap_k[w] = 1, ip_cap_c[w * IP_PC_N] = fb - (f1 - f0), ip_cap_c[w * IP_PC_N + 1] = f1 - f0, ip_cap_c[w * IP_PC_N + 2] = 0;
                moved++;
            }
            if (moved) { ip_stats_sib++; return sbuf; }
            ip_stats_sibw[3]++;
        } else ip_stats_sibw[2]++;
    }
    if (!o || o->size != n->size) { ip_stats[1]++; if (!ip_kept_why) ip_kept_why = 1; return nd; }
    static uint32_t buf[0x10000 / 4];
    const uint32_t *a = (const uint32_t *)(ipf[IP_PREV].data + o->off), *b = (const uint32_t *)nd;
    uint32_t nw = n->size / 4;
    {   // which vec4s changed from N-1 to N, kept with N's entry: the next frame's replay tells held values by it
        uint64_t chg = 0;
        for (uint32_t i = 0; i < nw && i < 256; i++) if (a[i] != b[i]) chg |= 1ull << (i / 4);
        ((IpEntry *)n)->chg = chg, ((IpEntry *)n)->chg_known = true;
        ((IpEntry *)n)->pkey = o->key, ((IpEntry *)n)->pkey_ok = true;
    }
    // The curve's third point: N-1's partner in N-2, as N-1's own replay paired them (ip_curve_ok: three frames in a row
    // at the same rate)
    const uint32_t *c3 = NULL;
    if (ip_three_ok && o->pkey_ok && o->chg_known) {
        const IpEntry *e2 = ip_find(&ipf[IP_PREV2], o->pkey);
        if (e2 && e2->size == o->size) c3 = (const uint32_t *)(ipf[IP_PREV2].data + e2->off);
    }
    static int held_off = -1;  // (LARECOMP_INTERP_NO_HELD=1: research, held values blended like the others)
    if (held_off < 0) held_off = getenv("LARECOMP_INTERP_NO_HELD") != NULL;
    static int alt_off = -1;  // (LARECOMP_INTERP_NO_ALT=1: comparison, alternating values blended like the others)
    if (alt_off < 0) alt_off = getenv("LARECOMP_INTERP_NO_ALT") != NULL;
    // A jump anywhere in the buffer (a bone, a matrix row) keeps all of it at N: blending the rest would pull part of a
    // skeleton or of a matrix halfway and leave the other part, which stretches the model.
    for (uint32_t i = 0; i < nw; i++) {
        if (a[i] == b[i]) continue;
        uint32_t ea = (a[i] >> 23) & 0xFF, eb = (b[i] >> 23) & 0xFF;
        if (!ea || !eb || ea == 0xFF || eb == 0xFF) continue;
        float fa, fb;
        memcpy(&fa, &a[i], 4);
        memcpy(&fb, &b[i], 4);
        bool jump = ip_draw_2d ? fabsf(fb - fa) > 0.3f * fmaxf(fabsf(fa), fabsf(fb)) && fabsf(fb - fa) > 0.02f  // (2D: a big move is a recycled row or glyph, not motion)
                               : ip_jumps(fa, fb);
        if (jump) {
            if (ip_kept_log_now())
                LOG("[interp-kept] present %llu jump%s stage %d cbuf %d size %u word %u %g -> %g: %s\n",
                    (unsigned long long)renderer_present_count(), ip_draw_2d ? " (2d)" : "", stage, k, size, i, fa, fb, ip_sig_txt);
            if (!ip_kept_why) ip_kept_why = 3;
            if (dg) dg->jumps += nw;
            ip_stats[1]++;
            return nd;
        }
    }
    // Blended a vec4 at a time, all or nothing: a matrix row with some components blended and others kept (one of them
    // crossing zero, or an integer in a float's place) is not a transformation any more - it shears and shrinks the model.
    // Words that are equal need no test; the others must be finite floats (0 counts) without a jump.
    for (uint32_t g = 0; g < nw; g += 4) {
        uint32_t m = nw - g < 4 ? nw - g : 4;
        int bad = 0, changed = 0, jump = 0;
        for (uint32_t i = g; i < g + m; i++) {
            if (a[i] == b[i]) continue;
            changed++;
            uint32_t ea = (a[i] >> 23) & 0xFF, eb = (b[i] >> 23) & 0xFF;
            bool fa_ok = ea != 0xFF && (ea || !(a[i] & 0x7FFFFF)), fb_ok = eb != 0xFF && (eb || !(b[i] & 0x7FFFFF));
            float fa, fb;
            memcpy(&fa, &a[i], 4);
            memcpy(&fb, &b[i], 4);
            if (!fa_ok || !fb_ok) bad++;
            else if (ip_jumps(fa, fb)) jump++;
        }
        if (dg) { dg->words += m; dg->changed += changed; }
        // A vec4 back to N-2's value (every changed word of it, bit for bit) alternates between two states a frame at a
        // time, it doesn't move (Viridian: words of the ground's fragment cbuf 7, 0 and 108 every other frame) - a quarter
        // or half of the way between the two is neither. It keeps N.
        if (c3 && changed && !bad && !alt_off) {
            bool alt = true;
            for (uint32_t i = g; i < g + m && alt; i++) if (a[i] != b[i] && c3[i] != b[i]) alt = false;
            if (alt) {
                for (uint32_t i = g; i < g + m; i++) buf[i] = b[i];
                ip_stats_alt += changed;
                continue;
            }
        }
        // A vec4 that held still from N-2 to N-1 and changes now is a step between states, not motion: a sprite's frame
        // in an atlas (the Pokémon's icons in the team screen, the cursor), an expression or a blink (eyes) - blended,
        // the in-between images sampled half-way across the atlas, a mix of two frames or a neighbour's cell (flicker).
        // It switches half-way instead. Motion changes every frame and is blended.
        if (!bad && !jump && changed && !held_off && g < 256 && o->chg_known && !(o->chg & (1ull << (g / 4)))) {
            for (uint32_t i = g; i < g + m; i++) {
                buf[i] = ip_t < 0.5f ? a[i] : b[i];
                if (ip_cap_k) ip_cap_k[i] = 2, memcpy(&ip_cap_c[i * IP_PC_N], &a[i], 4), memcpy(&ip_cap_c[i * IP_PC_N + 1], &b[i], 4);
            }
            ip_stats_held += changed;
            continue;
        }
        if (!bad && !jump) {
            // Along the curve through N-2, N-1 and N (a vec4 that moved in both steps, without a jump or a non-float in
            // N-2's): turns, accelerations and the camera's rotation follow their path - a straight line from N-1 to N
            // cut the corners, and the speed changed at each real frame (at 30 fps x 4: three images on a chord, then a
            // kink). The quadratic through the three at t: a + t (b - c) / 2 + t^2 (b - 2a + c) / 2 (ip_curve3).
            bool curve = c3 && g < 256 && (o->chg & (1ull << (g / 4)));
            for (uint32_t i = g; i < g + m && curve; i++) {
                if (a[i] == b[i]) continue;
                uint32_t ec = (c3[i] >> 23) & 0xFF;
                float fa, fc;
                memcpy(&fa, &a[i], 4);
                memcpy(&fc, &c3[i], 4);
                float fb;
                memcpy(&fb, &b[i], 4);
                if (ec == 0xFF || (!ec && (c3[i] & 0x7FFFFF)) || ip_jumps(fc, fa) || !ip_curve_fits(fc, fa, fb)) curve = false;
            }
            for (uint32_t i = g; i < g + m; i++) {
                if (a[i] == b[i]) { buf[i] = b[i]; continue; }
                float fa, fb, v;
                memcpy(&fa, &a[i], 4);
                memcpy(&fb, &b[i], 4);
                if (curve) {
                    float fc;
                    memcpy(&fc, &c3[i], 4);
                    v = ip_curve3(fc, fa, fb, ip_t);
                    if (ip_cap_k) ip_cap_k[i] = 1, ip_cap_c[i * IP_PC_N] = fa, ip_cap_c[i * IP_PC_N + 1] = (fb - fc) * 0.5f,
                                  ip_cap_c[i * IP_PC_N + 2] = (fb - 2.0f * fa + fc) * 0.5f;
                } else {
                    v = fa + (fb - fa) * ip_t;
                    if (ip_cap_k) ip_cap_k[i] = 1, ip_cap_c[i * IP_PC_N] = fa, ip_cap_c[i * IP_PC_N + 1] = fb - fa, ip_cap_c[i * IP_PC_N + 2] = 0;
                }
                memcpy(&buf[i], &v, 4);
            }
            if (curve) ip_stats_curved += changed;
            if (dg) dg->blended += changed;
        } else {
            for (uint32_t i = g; i < g + m; i++) buf[i] = b[i];
            ip_stats[2] += changed;
            if (dg) { dg->jumps += jump ? changed : 0; dg->nonfloat += bad ? changed : 0; }
        }
    }
    ip_rot_blend(a, b, buf, nw);
    if (n->size > nw * 4) memcpy((uint8_t *)buf + nw * 4, nd + nw * 4, n->size - nw * 4);
    ip_stats[0]++;
    if (ip_draw_2d && ip_kept_log_now()) {
        uint32_t ch = 0, same = 0;
        for (uint32_t i = 0; i < nw; i++) if (a[i] != b[i]) { ch++; if (buf[i] == b[i]) same++; }
        if (ch) LOG("[interp-kept] present %llu 2d blended stage %d cbuf %d size %u: %u changed, %u of them at N%s: %s\n",
                    (unsigned long long)renderer_present_count(), stage, k, size, ch, same, c3 ? " (curve)" : "", ip_sig_txt);
    }
    {   // LARECOMP_INTERP_FAR_LOG=<units> (research): blended words this far from frame N's, per draw
        static float far = -1;
        if (far < 0) far = getenv("LARECOMP_INTERP_FAR_LOG") ? (float)atof(getenv("LARECOMP_INTERP_FAR_LOG")) : 0;
        if (far > 0) {
            float mx = 0; uint32_t at = 0;
            for (uint32_t i = 0; i < nw; i++) {
                float x, y; memcpy(&x, &buf[i], 4); memcpy(&y, &b[i], 4);
                if (isfinite(x) && isfinite(y) && fabsf(x - y) > mx) mx = fabsf(x - y), at = i;
            }
            if (mx > far) {
                float x, y, z; memcpy(&x, &a[at], 4); memcpy(&y, &b[at], 4); memcpy(&z, &buf[at], 4);
                NvnTexture *t0 = st.rt.ncolor && st.rt.color[0] ? st.rt.color[0] : st.rt.depth;
                LOG("[interp-far] present %llu t %.2f rt %u stage %d cbuf %d size %u occ %u%s: word %u N-1 %g N %g blended %g (%s)\n",
                    (unsigned long long)renderer_present_count(), ip_t, t0 ? t0->width : 0, stage, k, size, ip_draw_occ,
                    ip_part_set ? " (by content)" : "", at, x, y, z, ip_sig_txt);
            }
        }
    }
    return buf;
}

// hash of a command list's records (JUMPs and CALLs followed), to tell whether its memory still holds what ran
static uint64_t ip_list_hash(uint64_t handle, int depth) {
    uint64_t h = 0xcbf29ce484222325ull, p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op >= CMD_COUNT || r->size < sizeof(CmdRec) || r->size > (1u << 20)) return 0;
        const uint64_t *w = (const uint64_t *)r;
        for (uint32_t i = 0; i < r->size / 8; i++) h = ip_mix(h, w[i]);
        if (r->op == CMD_END) return h;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (r->op == CMD_CALL) {
            const uint64_t *hs = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) h = ip_mix(h, ip_list_hash(hs[i], depth + 1));
        }
        p += r->size;
    }
    return 0;
}

// The replay's own copy of the frame's command lists (vk_interp_begin). The game records its next frame into the same
// command memory while the render thread replays this one, so a replay straight from it could read a record half
// rewritten: a called list's handle that was garbage (0x5000000001) stopped the game in exec_list (issue #21). The copy
// is hashed as it is taken - equal to the hash noted when the list ran, it is what ran - and the replay runs it: JUMPs
// followed (left out of the copy), CALLs pointing at the copies of their lists. Reads stay inside the guest window.
static uint8_t *ip_copy_buf;
static size_t ip_copy_cap, ip_copy_len;
static uint64_t ip_copy_lists[256];
static bool ip_copy_full;  // the last copy ran out of room

static uint64_t ip_list_copy(uint64_t handle, int depth, uint64_t *hash) {
    uint64_t h = 0xcbf29ce484222325ull, p = handle, first = 0;
    for (int guard = 0; guard < 10000000; guard++) {
        if (p < GUEST_LO || p + sizeof(CmdRec) > GUEST_HI) return 0;
        if (ip_copy_len + sizeof(CmdRec) > ip_copy_cap) { ip_copy_full = true; return 0; }
        CmdRec *r = (CmdRec *)(ip_copy_buf + ip_copy_len);
        memcpy(r, (const void *)(uintptr_t)p, sizeof(CmdRec));
        uint32_t size = r->size;
        if (r->op >= CMD_COUNT || size < sizeof(CmdRec) || size > (1u << 20) || (size & 7) || p + size > GUEST_HI) return 0;
        if (ip_copy_len + size > ip_copy_cap) { ip_copy_full = true; return 0; }
        memcpy(r->extra, (const uint8_t *)(uintptr_t)p + sizeof(CmdRec), size - sizeof(CmdRec));
        const uint64_t *w = (const uint64_t *)r;
        for (uint32_t i = 0; i < size / 8; i++) h = ip_mix(h, w[i]);
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }  // (not kept: the next record is copied in its place)
        if (!first) first = (uint64_t)(uintptr_t)r;
        ip_copy_len += size;
        if (r->op == CMD_END) { *hash = h; return first; }
        if (r->op == CMD_CALL && r->x[0] && depth < 8) {  // (the called lists go after a JUMP over them)
            if (ip_copy_len + sizeof(CmdRec) > ip_copy_cap) { ip_copy_full = true; return 0; }
            CmdRec *j = (CmdRec *)(ip_copy_buf + ip_copy_len);
            memset(j, 0, sizeof *j);
            j->op = CMD_JUMP, j->size = sizeof(CmdRec);
            ip_copy_len += sizeof(CmdRec);
            uint64_t *hs = (uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) {
                if (sizeof(CmdRec) + (i + 1) * 8 > size) return 0;
                uint64_t ch = 0;
                if (!(hs[i] = ip_list_copy(hs[i], depth + 1, &ch))) return 0;
                h = ip_mix(h, ch);
            }
            j->x[0] = (uint64_t)(uintptr_t)(ip_copy_buf + ip_copy_len);
        }
        p += size;
    }
    return 0;
}

static void ip_note_list(uint64_t handle) {
    IpFrame *f = &ipf[ipc];
    if (f->nlists >= 256) { f->overflow = true; return; }
    f->hashes[f->nlists] = ip_list_hash(handle, 0);
    f->lists[f->nlists++] = handle;
}

// The images of a present, kept until they are shown (device.c shows them on schedule, after the replays that wrote
// the game's textures): the presented texture as it was (raw) and, when post_process does something to it (FSR 3,
// SMAA, the colour pass), the result - made right away, while the scene's depth, motion vectors, jitter and interface
// mask are this image's. Slots 0 .. 7: the in-between images; IP_KEEP_N: frame N's own. Two sets, one per present
// in turn: a present's images wait for their time (device.c) while the next present's are rendered.
// An in-between image drawn into a window image right after it is made (device.c, the present queue: the default) is
// not copied: its processed image is drawn from where post_process_ex left it (direct), before the next replay writes
// there in the command buffer's order. At 4K a copy was ~66 MB of traffic and two full barriers an image.
#define IP_KEEP_N 8
// (sharp: the optical flow's sharpening left to the present's draw - flow_image - its strength; 0 none)
typedef struct { TexBackend raw, post; TexBackend *direct; bool post_ok, raw_ok; double ow, oh; float sharp; } IpKeep;
static IpKeep ip_keep_sets[2][IP_KEEP_N + 1], *ip_keep = ip_keep_sets[0];
static TexBackend *hold_copy_into(TexBackend *d, TexBackend *src);
static VkImageView ip_post_view(TexBackend *b, double ow, double oh) {
    for (int i = 0; i < 2 * (IP_KEEP_N + 1); i++) {
        IpKeep *k = &ip_keep_sets[0][0] + i;
        if (b != &k->raw) continue;
        if (!k->post_ok) return VK_NULL_HANDLE;
        k->post_ok = false;
        ip_present_sharp = k->sharp;
        TexBackend *d = k->direct;
        k->direct = NULL;
        if (fabs(ow - k->ow) > 0.5 || fabs(oh - k->oh) > 0.5) return VK_NULL_HANDLE;  // (the window changed size)
        return post_view(d ? d : &k->post);
    }
    return VK_NULL_HANDLE;
}
static TexBackend *ip_lo_resolve(TexBackend *lo);
static TexBackend *ip_keep_image(TexBackend *b, int slot, bool now) {
    IpKeep *k = &ip_keep[slot];
    k->post_ok = k->raw_ok = false;
    k->direct = NULL;
    k->sharp = 0;
    TexBackend *res = NULL, *in = b;
    // (a reduced in-between image without FSR 3.1: back to N's size against N's image first, then processed as N was)
    if (slot != IP_KEEP_N && vk_tex_lo_active() && !fsr3_on() && !gsr2_on()) in = ip_lo_resolve(b);
    if (post_last_ow > 0 && post_last_oh > 0) {
        ip_side_post = slot != IP_KEEP_N;  // (an in-between image: FSR 3.1's history is the real frames' alone)
        post_process_ex(vk_cmd(), in, post_last_ow, post_last_oh, &res);
        ip_side_post = false;
    } else if (in != b) res = in;
    if (res && res != b) {  // (the processed image is what is shown: the raw one is only its handle, not copied)
        if (now && slot != IP_KEEP_N) k->direct = res;  // (drawn before anything writes it again: no copy)
        else hold_copy_into(&k->post, res);
        k->post_ok = true, k->ow = post_last_ow, k->oh = post_last_oh;
        // (N's own image, whole, when the reduced in-between images are resolved against it: ip_lo_resolve)
        if (slot == IP_KEEP_N && ip_lo_factor() < 0.99f && !fsr3_on() && !gsr2_on()) hold_copy_into(&k->raw, b), k->raw_ok = true;
        else if (!k->raw.image) hold_copy_into(&k->raw, b);
        k->raw.sx = b->sx, k->raw.sy = b->sy, k->raw.scalable = b->scalable;
        return &k->raw;
    }
    hold_copy_into(&k->raw, b);  // (nothing done to it: the raw copy is shown, as b would be)
    k->raw_ok = true;
    return &k->raw;
}
// The in-between images' render size, x the real frames' on each axis: full, frame generation "Auto"'s own when the GPU
// is behind (ip_auto_mult: 75 %, then 50 %, before a lower multiplier), or LARECOMP_FG_SCALE=<f> (research, fixed).
// Measured at 4K (FSR 3.1 native, SSAO, GI high), GPU per in-between image:
// 3.65 ms at full size, 2.52 at 75 %, 1.63 at 50 % (the game's scene pass 1.27 / 0.79 / 0.41); detail within 1 - 3 % of
// the real frames (RCAS adjusted: ip_side_sharp_stops), halfway placement the same (93 % of the moving blocks); flicker
// 1.60 / 1.66 / 1.89 % of the blocks (the walk in the grass, 3x).
static float ip_auto_scale = 1;  // (frame generation "Auto": this present's, ip_auto_mult)
static float ip_lo_factor(void) {
    static float env = -1;
    if (env < 0) env = getenv("LARECOMP_FG_SCALE") ? (float)atof(getenv("LARECOMP_FG_SCALE")) : 0;
    float f = env > 0 ? env : g_settings.frame_gen == FG_AUTO ? ip_auto_scale : 1.0f;
    return f < 0.25f ? 0.25f : f > 1 ? 1 : f;
}
// A reduced in-between image without FSR 3.1, back to N's size (post image 30 of the reduced set): N's image taken
// where the motion vectors say each pixel was in N, clipped to the reduced image's 3x3 there (side_resolve.frag, as
// the FSR 3.1 path does against FSR's output)
static TexBackend *ip_lo_resolve(TexBackend *lo) {
    IpKeep *n = &ip_keep[IP_KEEP_N];
    if (!n->raw_ok || !n->raw.image) return lo;
    TexBackend *out = post_image(30, n->raw.width, n->raw.height);
    TexBackend *mv = ao.depth && ao.proj_frame == fx_frame() && mv_wanted()
                   ? post_image_fmt(8, ao.depth->width, ao.depth->height, VK_FORMAT_R16G16B16A16_SFLOAT) : NULL;
    VkCommandBuffer cb = vk_cmd();
    gp_mark(cb, "ours: reduced image resolve (in-between)");
    // ... then sharpened (RCAS): the clip to the reduced image's 3x3 and its share soften it - without, 13 - 17 % less
    // detailed than the real frames at 75 %, 21 % at 50 %. 1 stop at 75 % (0.9 at 50 %) with SMAA, 1.3 (1.15) with TAA,
    // whose own sharpening follows (post_process_ex): within 3 % of the real frames. (Research: LARECOMP_LO_SHARP=<stops>,
    // negative: none; LARECOMP_LO_HIST=<weight of N's image>, default the TAA's 0.9.)
    static float hist = -1, env_stops = -100;
    if (hist < 0) hist = getenv("LARECOMP_LO_HIST") ? (float)atof(getenv("LARECOMP_LO_HIST")) : TAA_HISTORY;
    if (env_stops == -100) env_stops = getenv("LARECOMP_LO_SHARP") ? (float)atof(getenv("LARECOMP_LO_SHARP")) : -99;
    float stops = env_stops != -99 ? env_stops : (taa_on() ? 1.3f : 1.0f) - 0.5f * (0.75f - ip_lo_factor());
    float ps[8] = {1.0f / (float)out->width, 1.0f / (float)out->height, 1.0f / (float)lo->width, 1.0f / (float)lo->height, hist};
    VkImageView si[4] = {raw_view(lo), raw_view(&n->raw), mv ? post_view(mv) : vk_dummy_view(TEXTYPE_2D)};
    post_pass_view(cb, post_pipeline("side_resolve.frag"), si, 3, post_view(out), out->width, out->height, ps, 32, false, false);
    if (stops >= 0) {  // (the post passes after it wait with their own barriers; a keep copy with its own)
        TexBackend *r = post_image(31, out->width, out->height);
        float pr[8] = {exp2f(-stops), 0, 0, 0, (float)out->width, (float)out->height};
        VkImageView ri_[3] = {post_view(out)};
        post_pass_view(cb, post_pipeline("rcas.frag"), ri_, 1, post_view(r), out->width, out->height, pr, 32, false, false);
        out = r;
    }
    out->sx = n->raw.sx, out->sy = n->raw.sy, out->scalable = n->raw.scalable;
    return ip_ui_keep(cb, out, &n->raw, true);  // (its interface: N's)
}
// N's processed image (made at vk_interp_begin, before the in-between images), when it is at ow x oh
static TexBackend *ip_n_post(double ow, double oh) {
    IpKeep *k = &ip_keep[IP_KEEP_N];
    return k->post_ok && fabs(ow - k->ow) < 0.5 && fabs(oh - k->oh) < 0.5 ? &k->post : NULL;
}
// ---- Frame generation by optical flow (fg_method = flow): the in-between images made from N-1's and N's processed
// images (as they are shown: after FSR 3.1 / TAA / SMAA, the colour pass and the debanding), moved toward the image's
// time along a motion field - no draw runs again. The field: the motion vectors the renderer has (camera and objects:
// mv_run), and where they miss what the image does (particles, the geometry the CPU writes, transparency, shadows
// moving over still ground) an image-based estimate - block matching on the luminance at ~640 wide (flow_match,
// flow_refine), taken per pixel only where it is clearly better (flow_pick). Then per image (flow_warp.frag): N's
// pixel that is at q at t, mixed with N-1's where it was unless N-1's depth shows it covered then (N alone); a pixel
// uncovered since t (covered in N) from N-1 along the background's motion; N's interface as it is (its mask).
// Post images: 38, 39 N's field (one per keep set: N-1's is the other), 40 its dilation, 41 the luminance, 42, 43 the
// blocks (matched, refined), 44 the per-pixel pick, 45 the in-between image, 46 it sharpened (flow_sharp.frag).
// Measured (FSR 3.1 native, SSAO medium, GI high; the walk in the grass, 3x): GPU at 4K 0.90 ms a present (luminance
// 0.12, matching 0.2, refining 0.1, pick 0.05, field and dilation 0.38) + 0.71 an image: 1.16 ms per in-between image
// against the re-rendering's 3.90; at 1280 x 720 0.23 against 1.29. Flicker as the re-rendering's (720p: 0.76 / 0.93 %
// of the blocks at 2x / 3x, re-rendering 0.83 / 0.93), detail within 1.3 % of the real frames (re-rendering +3 - 4 %),
// 91 % of the moving blocks halfway (the same). Pacing at 240 Hz, 30 fps x 4: 90 % of the intervals right at 4K
// (re-rendering 39 %), 96.5 % at 720p (76 %).
// Research: LARECOMP_FLOW_MATCH=0 (the motion vectors alone, no block matching), LARECOMP_FLOW_RADIUS=<pixels of the
// luminance> (the search, default 4), LARECOMP_FLOW_OCCLUSION=0 (no depth test: N-1 and N always mixed where found),
// LARECOMP_FLOW_DEBUG=1 (tinted: red the flow's vectors, green N alone, blue N-1 alone).
static bool flow_on(void) { return g_settings.frame_gen && g_settings.fg_method == FG_FLOW; }
static struct {
    TexBackend *img[2];      // N's processed image, per keep set (ip_keep_sets): N-1's is the other set's
    uint64_t img_real[2];    // (the present it was kept at)
    uint64_t field_real[2];  // N's field (post image 38 + set) made at that present, and its size
    uint32_t fw[2], fh[2];
    bool fp;                 // this present: N-1's field there, the size of N's (the depth test, N-1's interface)
    float t;                 // the image's time (vk_interp_replay)
} flow;
static int flow_set(void) { return ip_keep == ip_keep_sets[1]; }
// N's kept image noted (re-rendering too: switched to the flow, its first present already has N-1's image)
static void flow_note_n(void) {
    int s = flow_set();
    IpKeep *kn = &ip_keep[IP_KEEP_N];
    flow.img[s] = kn->post_ok ? &kn->post : NULL, flow.img_real[s] = renderer_present_count();
}
static int flow_env(const char *name, int def) { const char *e = getenv(name); return e ? atoi(e) : def; }

// At vk_interp_begin, after N's image was kept: N's field (the next present's N-1 field), and the flow from N-1 when its
// image is there. False: no in-between images this present (N alone).
static bool flow_prepare(void) {
    int s = flow_set();
    uint64_t real = renderer_present_count(), fr = fx_frame();
    flow_note_n();  // (only a processed image: deband, at least)
    TexBackend *c1 = flow.img[s], *c0 = flow.img[!s];
    if (!c1) return false;
    bool prev = c0 && c0->image && flow.img_real[!s] + 1 == real && c0->width == c1->width && c0->height == c1->height;
    TexBackend *d = ao.depth && ao.depth->image && (ao.depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && ao.depth->samples <= 1 &&
                    ao.proj_frame == fr ? ao.depth : NULL;
    TexBackend *mv = d && ao.done_frame == fr && mv_wanted() ? post_image_fmt(8, d->width, d->height, VK_FORMAT_R16G16B16A16_SFLOAT) : NULL;
    uint32_t fw = d ? d->width : c1->width, fh = d ? d->height : c1->height;
    bool mask = ip_ui_mask && ip_ui_mask_real == real;
    static int match = -1, radius = -1;
    if (match < 0) match = flow_env("LARECOMP_FLOW_MATCH", 1), radius = flow_env("LARECOMP_FLOW_RADIUS", 4);
    const VkFormat hf = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImageView dummy = vk_dummy_view(TEXTYPE_2D);
    vk_end_pass();
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    VkCommandBuffer cb = vk_cmd();
    gp_mark(cb, "ours: flow field %ux%u%s", fw, fh, prev ? "" : " (no N-1)");
    TexBackend *pick = NULL;
    if (prev && match) {  // (the block matching: N-1's image needed)
        uint32_t lw = c1->width < 640 ? c1->width : 640, lh = (uint32_t)lround((double)c1->height * lw / c1->width);
        if (lh < 8) lh = 8;
        const uint32_t G = 4;
        uint32_t gw = (lw + G - 1) / G, gh = (lh + G - 1) / G;
        VkPipeline p_l = post_pipeline_ex("flow_luma.frag", hf, 0), p_m = post_pipeline_ex("flow_match.frag", hf, 0);
        VkPipeline p_r = post_pipeline_ex("flow_refine.frag", hf, 0), p_p = post_pipeline_ex("flow_pick.frag", hf, 0);
        TexBackend *L = post_image_fmt(41, lw, lh, hf), *b0 = post_image_fmt(42, gw, gh, hf), *b1 = post_image_fmt(43, gw, gh, hf);
        pick = post_image_fmt(44, lw, lh, hf);
        float pl[8] = {1.0f / (float)lw, 1.0f / (float)lh, mask ? 1.0f : 0.0f};
        VkImageView li[3] = {post_view(c1), post_view(c0), mask ? post_view(ip_ui_mask) : dummy};
        post_pass_view(cb, p_l, li, 3, post_view(L), lw, lh, pl, 32, false, false);
        float pm[8] = {1.0f / (float)lw, 1.0f / (float)lh, (float)radius, (float)G, mv ? 1.0f : 0.0f};
        VkImageView mvv = mv ? post_view(mv) : dummy;
        VkImageView mi[3] = {post_view(L), mvv, dummy};
        gp_mark(cb, "ours: flow match %ux%u", gw, gh);
        post_pass_view(cb, p_m, mi, 3, post_view(b0), gw, gh, pm, 32, false, false);
        VkImageView ri[3] = {post_view(L), mvv, post_view(b0)};
        gp_mark(cb, "ours: flow refine");
        post_pass_view(cb, p_r, ri, 3, post_view(b1), gw, gh, pm, 32, false, false);
        VkImageView pi[3] = {post_view(L), mvv, post_view(b1)};
        gp_mark(cb, "ours: flow pick %ux%u", lw, lh);
        post_pass_view(cb, p_p, pi, 3, post_view(pick), lw, lh, pm, 32, false, false);
    }
    VkPipeline p_f = post_pipeline_ex("flow_field.frag", hf, 0), p_d = post_pipeline_ex("flow_dilate.frag", hf, 0);
    TexBackend *F = post_image_fmt(38 + s, fw, fh, hf), *D = post_image_fmt(40, fw, fh, hf);
    static const VkComponentMapping id = {0};
    VkImageView dv = d ? vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT) : dummy;
    float pf[12] = {ao.proj[0], ao.proj[1], ao.proj[2], ao.proj[3], nvn_depth_mode() == 0 ? 1.0f : 0.0f, mv ? 1.0f : 0.0f,
                    d ? 1.0f : 0.0f, mask ? 1.0f : 0.0f, pick ? 1.0f : 0.0f, 0, (float)fw, (float)fh};
    VkImageView fi[4] = {mv ? post_view(mv) : dummy, dv, pick ? post_view(pick) : dummy, mask ? post_view(ip_ui_mask) : dummy};
    gp_mark(cb, "ours: flow field pass");
    post_pass_view(cb, p_f, fi, 4, post_view(F), fw, fh, pf, sizeof pf, false, false);
    float pz[8] = {0};
    VkImageView di[1] = {post_view(F)};
    gp_mark(cb, "ours: flow dilate");
    post_pass_view(cb, p_d, di, 1, post_view(D), fw, fh, pz, 32, false, false);
    vk_post_barrier(cb);
    vk_frame_resume();
    flow.field_real[s] = real, flow.fw[s] = fw, flow.fh[s] = fh;
    flow.fp = flow.field_real[!s] + 1 == real && flow.fw[!s] == fw && flow.fh[!s] == fh;
    return prev;
}

// The in-between image at flow.t (flow_warp.frag) kept in slot `slot` as vk_interp_keep keeps a re-rendered one: b
// (N's presented texture) is only its handle
static TexBackend *flow_image(TexBackend *b, int slot, bool now) {
    int s = flow_set();
    IpKeep *k = &ip_keep[slot], *kn = &ip_keep[IP_KEEP_N];
    TexBackend *c1 = flow.img[s], *c0 = flow.img[!s];
    k->post_ok = k->raw_ok = false;
    k->direct = NULL;
    const VkFormat hf = VK_FORMAT_R16G16B16A16_SFLOAT;
    static int occl = -1, dbg = -1;
    if (occl < 0) occl = flow_env("LARECOMP_FLOW_OCCLUSION", 1), dbg = flow_env("LARECOMP_FLOW_DEBUG", 0);
    uint32_t w = c1->width, h = c1->height;
    bool mask = ip_ui_mask && ip_ui_mask_real == renderer_present_count();
    VkImageView dummy = vk_dummy_view(TEXTYPE_2D);
    static float sharp = -1;
    if (sharp < 0) sharp = getenv("LARECOMP_FLOW_SHARP") ? (float)atof(getenv("LARECOMP_FLOW_SHARP")) : 0.2f;
    VkPipeline p = post_pipeline("flow_warp.frag"), ps = post_pipeline("flow_sharp.frag");
    TexBackend *out = post_image(45, w, h);
    TexBackend *F = post_image_fmt(38 + s, flow.fw[s], flow.fh[s], hf), *D = post_image_fmt(40, flow.fw[s], flow.fh[s], hf);
    TexBackend *Fp = flow.fp ? post_image_fmt(38 + !s, flow.fw[s], flow.fh[s], hf) : NULL;
    vk_end_pass();
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    VkCommandBuffer cb = vk_cmd();
    gp_mark(cb, "ours: flow t=%.2f (in-between)", flow.t);
    float pw[12] = {flow.t, Fp ? 1.0f : 0.0f, mask ? 1.0f : 0.0f, occl ? 1.0f : 0.0f, 1.0f / (float)w, 1.0f / (float)h, 0, 0,
                    dbg ? 1.0f : 0.0f};
    VkImageView wi[6] = {post_view(c0), post_view(c1), post_view(F), post_view(D), Fp ? post_view(Fp) : dummy,
                         mask ? post_view(ip_ui_mask) : dummy};
    post_pass_view(cb, p, wi, 6, post_view(out), w, h, pw, sizeof pw, false, false);
    // The sharpening in the present's own draw when it shows the image pixel for pixel (the output's size: present.frag,
    // a.y) - one full-size pass written and read back less (at 2670 x 1200 on the phone, ~0.7 ms of the GPU's ~12 a
    // frame at its top clock). Else a pass of its own. LARECOMP_FLOW_SHARP_PASS=1: always its own pass (comparison).
    static int sharp_pass = -1;
    if (sharp_pass < 0) sharp_pass = getenv("LARECOMP_FLOW_SHARP_PASS") ? atoi(getenv("LARECOMP_FLOW_SHARP_PASS")) : 0;
    k->sharp = 0;
    if (sharp > 0 && !sharp_pass && w == (uint32_t)lround(kn->ow) && h == (uint32_t)lround(kn->oh)) k->sharp = sharp;
    else if (sharp > 0) {
        TexBackend *o2 = post_image(46, w, h);
        float pz[8] = {sharp};
        VkImageView si[1] = {post_view(out)};
        gp_mark(cb, "ours: flow sharpen");
        post_pass_view(cb, ps, si, 1, post_view(o2), w, h, pz, 32, false, false);
        out = o2;
    }
    vk_post_barrier(cb);  // (the present's draw samples it; a keep copy waits with a barrier of its own)
    vk_frame_resume();
    if (now) k->direct = out;  // (drawn before the next image writes it: no copy)
    else hold_copy_into(&k->post, out);
    k->post_ok = true, k->ow = kn->ow, k->oh = kn->oh;
    if (!k->raw.image || k->raw.width != b->width || k->raw.height != b->height || k->raw.format != b->format) hold_copy_into(&k->raw, b);
    k->raw.sx = b->sx, k->raw.sy = b->sy, k->raw.scalable = b->scalable;
    return &k->raw;
}

// The presented texture b after an in-between image was rendered into it: kept in slot `slot` (0 .. 7) to be shown -
// now (drawn into the window before the next replay: no copy) or later. (Optical flow: made here, flow_image.)
static bool ip_flow;  // this present's in-between images are the optical flow's (vk_interp_begin)
TexBackend *vk_interp_keep(TexBackend *b, int slot, bool now) {
    if (ip_flow) return flow_image(b, slot < 0 ? 0 : slot >= IP_KEEP_N ? IP_KEEP_N - 1 : slot, now);
    TexBackend *r = ip_keep_image(vk_tex_lo(b), slot < 0 ? 0 : slot >= IP_KEEP_N ? IP_KEEP_N - 1 : slot, now);
    vk_tex_lo_end();  // (a reduced image: vk_interp_replay began it)
    return r;
}
static TexBackend *hold_copy_into(TexBackend *d, TexBackend *src) {
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                .imageType = VK_IMAGE_TYPE_2D, .format = src->format, .extent = {src->width, src->height, 1},
                                .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    d->sx = src->sx; d->sy = src->sy; d->scalable = src->scalable;
    VkCommandBuffer cb = vk_cmd();
    gp_mark(cb, "ours: keep copy %ux%u", src->width, src->height);
    vk_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {src->width, src->height, 1}};
    vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    vk_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    return d;
}

// Frame generation "Auto" (frame_gen = auto): the multiplier of the world's rate. Its candidates, 2 .. 4: those whose
// rate the display's refresh shows evenly (60 fps at 240 Hz: 4x, 240 images a second, one every refresh; 3x would be
// 180, every 1.33 refreshes - uneven), the largest first; none even (60 fps at 144 Hz): the largest at or below the
// refresh. With variable refresh (LARECOMP_FG_VRR=1: the display shows any rate evenly) the largest at or below it.
// Then the GPU: when the frames come late (the measured period 8 % over the game's for half a second's presents in a
// row: the in-between images cost more than the GPU has) the in-between images are rendered smaller first - 75 %, then
// 50 % of the real frames' size on each axis (ip_lo_factor; at 4K 3.65 / 2.52 / 1.63 ms an image, detail within 1 - 3 %
// of the real frames) - and only then the next candidate down, at full size again (60 fps at 240 Hz: 4x at 100 / 75 /
// 50 %, then 2x at 100 / 75 / 50 %: each step costs less than the one before; 4x at 50 % is 4.9 ms, 2x at full size
// 3.65) - a lower multiplier after a second late; none in the 3 s after the game's rate
// changed (entering the world: the loading's hitches and shader builds came down to 2x for 10 s); back up after 10 s on time, the wait doubling
// each time it had to come down again within 5 s of going up (up to 2 min) - as the dynamic resolution does
// (output_scale.c). Measured at 4K (FSR 3.1 native, SSAO, GI high, 240 Hz): 60 fps x 4 needs ~16.8 ms of GPU a frame
// (8.8 the real one, 3 x 2.5 the in-between ones) for 16.7 - it comes down to 2x.
// LARECOMP_FG_DISPLAY_HZ=<hz> (research) stands in for the display's refresh.
static int ip_auto_mult(uint64_t period) {
    extern double vk_display_hz(void);
    extern uint64_t fps60_period_ns(void);
    static double env_hz = -1;
    static int vrr = -1;
    if (env_hz < 0) env_hz = getenv("LARECOMP_FG_DISPLAY_HZ") ? atof(getenv("LARECOMP_FG_DISPLAY_HZ")) : 0;
    if (vrr < 0) vrr = getenv("LARECOMP_FG_VRR") != NULL;
    double hz = env_hz > 0 ? env_hz : vk_display_hz();
    if (hz < 20) hz = 60;  // (unknown: the common case)
    // (another refresh only when it holds for 60 presents: with images presented in bursts the compositor's timing read
    // 206 - 222 Hz now and then at 240, and each reading started the levels again from the top - 4x / 2x a second)
    static double stable_hz;
    static int hz_odd;
    if (!stable_hz || fabs(hz - stable_hz) < 0.03 * stable_hz) stable_hz = stable_hz ? stable_hz : hz, hz_odd = 0;
    else if (++hz_odd >= 60) stable_hz = hz, hz_odd = 0;
    hz = stable_hz;
    double fps = g_settings.fps > 0 ? g_settings.fps : 30;
    int cand[3], nc = 0;
    for (int m = 4; m >= 2; m--) {  // (even on the refresh first)
        double r = hz / (fps * m);
        if (!vrr && r >= 0.99 && fabs(r - lround(r)) < 0.02) cand[nc++] = m;
    }
    for (int m = 4; m >= 2 && !nc; m--)
        if (fps * m <= hz * 1.01) cand[nc++] = m;
    if (!nc) cand[nc++] = 2;
    // the GPU keeping up: by the presents' period against the game's own for this frame. Level l: candidate l / 3 at
    // the (l % 3)-th size
    static const float scales[3] = {1.0f, 0.75f, 0.5f};
    // (the optical flow: one level per candidate - its images come from the shown ones, at their size)
    int per = g_settings.fg_method == FG_FLOW ? 1 : 3;
    int nl = nc * per;
    static int level, late, good, last_level = -1, key_per;
    static double wait_s = 10, last_raise_t = -100;
    static uint64_t last_t;
    static double key_hz, key_fps, key_want, grace_until;
    if (key_hz != hz || key_fps != fps || key_per != per)  // (another display, rate or method: again)
        key_hz = hz, key_fps = fps, key_per = per, level = 0, late = good = 0;
    uint64_t now = SDL_GetTicksNS();
    double t = now / 1e9;
    double want = (double)fps60_period_ns();
    if (want != key_want) key_want = want, grace_until = t + 3, late = good = 0;
    // ... or images presented before their time: the window images held drawn ran out (the driver gives 8: 5 held).
    // The optical flow draws a present's images at once, and at 4K the GPU finishes them ~16 ms after the present (the
    // timeline's lead): at 60 fps x 4 two presents' images waited at once and went 2 - 3 to a refresh (240 Hz: 43 % of
    // the intervals on one refresh); now it comes down to 2x as the re-rendering does
    extern uint64_t vk_fg_forced_presents(void);
    static uint64_t forced_seen;
    uint64_t forced_now = vk_fg_forced_presents();
    bool forced = forced_now != forced_seen && per == 1;  // (the optical flow's: the re-rendering as it was)
    forced_seen = forced_now;
    if (last_t && now - last_t < 200000000ull && want > 0 && t >= grace_until) {  // (not after a pause: loading, a menu's stop)
        if ((double)period > want * 1.08 || forced) { late++; good = 0; }
        else { good++; if ((double)period < want * 1.03) late = 0; }
        double late_s = (level + 1) % per ? 0.5 : 1.0;  // (a smaller size after half a second late, a lower multiplier after one)
        if (late >= (int)(late_s * 1e9 / want) && level < nl - 1) {
            level++, late = good = 0;
            if (t - last_raise_t < 5) wait_s = fmin(wait_s * 2, 120);
        } else if (level > 0 && good >= (int)(wait_s * 1e9 / want)) {
            level--, good = 0, last_raise_t = t;
        }
    }
    last_t = now;
    if (level >= nl) level = nl - 1;
    int m = cand[level / per];
    ip_auto_scale = scales[level % per];
    if (level != last_level) {
        LOG("[fg] auto: %dx, in-between images at %.0f %% (display %.0f Hz%s, world %.0f fps%s; frames every %.1f ms for %.1f)\n",
            m, ip_auto_scale * 100, hz, vrr ? ", variable refresh" : "", fps, level ? ", the GPU behind: lowered" : "",
            period / 1e6, want / 1e6);
        last_level = level;
    }
    return m;
}

// Images per present with interpolation, for a frame that came `period` ns after the one before: N and k - 1 in-between
// images, at (1 .. k-1) / k of the way from N-1 to N. Frame generation (F12 menu, frame_gen = 2 / 3 / 4): that many
// times the world's frame rate (fps: 30, 60, 120), whatever this frame's own - a menu or a battle at the game's 30 in
// the 60 fps mode gets twice the images the world does, and both reach the same rate. Otherwise 2 (the 60 fps mode's
// menus). LARECOMP_INTERP_MULT=<k> (research) fixes k.
static int ip_images(uint64_t period) {
    static int env = -1;
    if (env < 0) {
        const char *e = getenv("LARECOMP_INTERP_MULT");
        env = e ? atoi(e) : 0;
        if (env) env = env < 2 ? 2 : env > 8 ? 8 : env;
    }
    if (env) return env;
    if (g_settings.frame_gen >= FG_AUTO) {
        // this frame's rate as the game means it (runtime/hle/fps60.c): the world's (fps) in the 60 / 120 fps mode, the
        // game's own 30 in menus, battles, the start - not the period measured: a frame the GPU could not finish in time
        // came later, got more images (k from the period, or the base taken as 30 from it) and was later still
        extern uint64_t fps60_period_ns(void);
        int m = g_settings.frame_gen == FG_AUTO ? ip_auto_mult(period) : g_settings.frame_gen;
        long k = lround((double)fps60_period_ns() * g_settings.fps * m / 1e9);
        return k < 2 ? 2 : k > 8 ? 8 : (int)k;
    }
    return 2;
}
int vk_interp_images(void) { return ip_k; }  // (after vk_interp_begin)

// At the present of frame N (presented texture b, `period` ns after N-1): true when frame N-1 was recorded too and N's
// lists are intact - N's own image is then kept in *hold, and vk_interp_replay(t) renders the image at t (0..1, N-1 to
// N) into the game's textures (b holds it), as many times as wanted (vk_interp_images: how many to show); false: nothing
// to show before N. Then vk_interp_end, either way.
static bool ip_began;  // (vk_interp_end moves recording on only after a present that had it on)
static bool ip_lo;     // this present's in-between images are rendered at ip_lo_factor (vk_interp_begin)
// LARECOMP_INTERP_REJ_LOG=1: each present without in-between images and why, each object kept at N
static bool ip_rej_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_INTERP_REJ_LOG") != NULL;
    return on;
}

bool vk_interp_begin(TexBackend *b, TexBackend **hold, uint64_t period) {
    ip_began = false;
    ip_flow = false;
    if (!ip_enabled()) {
        // Off (the world, a battle): what was recorded the last time it was on is stale - a menu opened later blended
        // with that old scene (a flash when the menu opened while walking)
        ipf[0].valid = ipf[1].valid = ipf[2].valid = false;
        return false;
    }
    ip_began = true;
    ip_keep = ip_keep_sets[ip_keep == ip_keep_sets[0]];
    ip_k = ip_images(period);
    IpFrame *cur = &ipf[ipc], *prev = &ipf[IP_PREV], *prev2 = &ipf[IP_PREV2];
    {
        extern uint64_t fps60_period_ns(void);
        cur->real = renderer_present_count(), cur->period = fps60_period_ns();
        static int curve = -1;
        if (curve < 0) curve = getenv("LARECOMP_INTERP_CURVE") ? atoi(getenv("LARECOMP_INTERP_CURVE")) : 1;
        ip_three_ok = prev2->valid && prev->valid && !prev2->overflow && prev->real == cur->real - 1 &&
                      prev2->real == cur->real - 2 && prev->period == cur->period && prev2->period == cur->period;
        ip_curve_ok = curve && ip_three_ok;
    }
    bool ok = prev->valid && cur->valid && cur->nlists > 0 && !cur->overflow && !prev->overflow && b && b->image &&
              b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && b->samples <= 1 && cur->destroyed == g_nvn_destroyed;
    bool structural = ok;
    if (ok && !ip_copy_buf && (ip_copy_buf = malloc((size_t)8 << 20))) ip_copy_cap = (size_t)8 << 20;
    ip_copy_len = 0;
    ip_copy_full = false;
    for (int i = 0; ok && i < cur->nlists; i++) {
        uint64_t h = 0;
        ip_copy_lists[i] = ip_copy_buf ? ip_list_copy(cur->lists[i], 0, &h) : 0;
        if (!cur->hashes[i] || !ip_copy_lists[i] || h != cur->hashes[i]) ok = false;
    }
    if (ip_copy_full && ip_copy_cap < ((size_t)256 << 20)) {  // (out of room: no image this time, a larger copy next)
        uint8_t *nb = malloc(ip_copy_cap * 2);
        if (nb) free(ip_copy_buf), ip_copy_buf = nb, ip_copy_cap *= 2;
        LOG("[interp] the replay's copy of the command lists: %zu MB\n", ip_copy_cap >> 20);
    }
    if (!ok) {
        if (!prev->valid || !cur->valid || cur->nlists == 0) ip_rej[0]++;
        else if (cur->overflow || prev->overflow) ip_rej[1]++;
        else if (cur->destroyed != g_nvn_destroyed) ip_rej[2]++;
        else if (!structural) ip_rej[3]++;
        else ip_rej[4]++;
        if (ip_rej_log())
            LOG("[interp-rej] present %llu: %s (valid %d/%d lists %d overflow %d/%d destroyed %d/%d)\n",
                (unsigned long long)renderer_present_count(),
                !prev->valid || !cur->valid || cur->nlists == 0 ? "no previous frame" : cur->overflow || prev->overflow ? "overflow"
                : cur->destroyed != g_nvn_destroyed ? "a texture destroyed" : !structural ? "format" : "a command list changed",
                prev->valid, cur->valid, cur->nlists, prev->overflow, cur->overflow, (int)cur->destroyed, (int)g_nvn_destroyed);
    }
    // The optical flow needs N-1's image, not its lists: only the cut check below is theirs (when they are intact)
    bool flow_ok = flow_on() && b && b->image && b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && b->samples <= 1, lists_ok = ok;
    if (ok) {
        // Per object, before the replay: the share of its changed float words that changed by a large step. Above
        // LARECOMP_INTERP_OBJ_LIMIT (0.4) the object is not blended (ip_obj_rejected); when the rejected objects hold
        // most of the frame's changes (a camera cut), there is no in-between image at all. (0.2 before: a walking
        // skeleton at 30 fps has 0.20 - 0.25, and was kept at N one frame in two - with the camera still, the whole
        // in-between image went, a 33 ms step several times a second at 2x. Swapped models and cuts: 0.5 - 0.95.)
        typedef struct { uint64_t obj, changed, big; } ObjAcc;
        static ObjAcc acc[IP_REJ];
        memset(acc, 0, sizeof acc);
        memset(ip_rej_obj, 0, sizeof ip_rej_obj);
        uint64_t changed = 0, big = 0, rej_changed = 0;
        int nrej = 0, nobj = 0;
        for (uint32_t i = 0; i < cur->n; i++) {
            const IpEntry *n = &cur->e[i], *o = ip_find(prev, n->key);
            if (!o || o->size != n->size || n->d2) continue;
            uint64_t gobj = n->obj ? n->obj : 1;  // (draws without an object: one group)
            const float *fa = (const float *)(prev->data + o->off), *fb = (const float *)(cur->data + n->off);
            uint64_t c = 0, bg = 0;
            for (uint32_t w = 0; w < n->size / 4; w++) {
                float a = fa[w], v = fb[w];
                if (a == v || !isfinite(a) || !isfinite(v)) continue;
                float m = fmaxf(fabsf(a), fabsf(v));
                if (m < 1e-3f || m > 1e7f) continue;
                c++;
                if (fabsf(v - a) > 0.3f * m && fabsf(v - a) > 0.1f) bg++;
            }
            if (!c) continue;
            uint32_t sl = (uint32_t)((gobj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
            while (acc[sl].obj && acc[sl].obj != gobj) sl = (sl + 1) & (IP_REJ - 1);
            if (!acc[sl].obj) { acc[sl].obj = gobj; nobj++; }
            acc[sl].changed += c, acc[sl].big += bg;
            changed += c, big += bg;
        }
        extern bool fps60_menu(void);
        bool menu = fps60_menu();  // (menus: the cut checks are for battles; a menu screen changes by large steps all the time)
        static double obj_limit = -1;
        if (obj_limit < 0) obj_limit = getenv("LARECOMP_INTERP_OBJ_LIMIT") ? atof(getenv("LARECOMP_INTERP_OBJ_LIMIT")) : 0.4;
        for (int i = 0; i < IP_REJ; i++)
            if (!menu && acc[i].obj && acc[i].changed >= 8 && (double)acc[i].big > obj_limit * (double)acc[i].changed) {
                uint32_t sl = (uint32_t)((acc[i].obj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
                while (ip_rej_obj[sl]) sl = (sl + 1) & (IP_REJ - 1);
                ip_rej_obj[sl] = acc[i].obj;
                nrej++;
                rej_changed += acc[i].changed;
                if (ip_rej_log())
                    LOG("[interp-obj] present %llu: object %#llx rejected, %llu of %llu changed words big (%.2f)\n",
                        (unsigned long long)renderer_present_count(), (unsigned long long)acc[i].obj, (unsigned long long)acc[i].big,
                        (unsigned long long)acc[i].changed, (double)acc[i].big / (double)acc[i].changed);
            }
        if (getenv("LARECOMP_INTERP_DIAG") || (ip_rej_log() && nrej))
            LOG("[interp-cut] present %llu: objects %d rejected %d (%llu of %llu changed words; big %llu)\n",
                (unsigned long long)renderer_present_count(), nobj, nrej, (unsigned long long)rej_changed, (unsigned long long)changed,
                (unsigned long long)big);
        if (!menu && changed && rej_changed * 10 > changed * 6) { ok = false; ip_rej[5]++; }
    }
    if (flow_ok) {
        // Every present keeps N's image and makes its field (the next one moves from them); without N-1's, or after a
        // cut, N alone (k = 1: shown on the same timeline)
        vk_end_pass();
        ip_flow = true, ip_lo = false, ip_plan_on = false;
        *hold = ip_keep_image(b, IP_KEEP_N, false);
        if (!flow_prepare() || (lists_ok && !ok)) ip_k = 1;
        return true;
    }
    if (ok) {
        vk_end_pass();
        // (the blends decided at this present's first in-between image serve the others: ip_plan_*)
        if (!++ip_plan_cur) ip_plan_cur = 1, memset(ip_plan, 0, ip_plan_cap * sizeof *ip_plan);
        ip_pwn = 0;
        static int plan_env = -1;  // (LARECOMP_INTERP_PLAN=0: comparison, every image decides its blends again; 2: both, compared)
        if (plan_env < 0) plan_env = getenv("LARECOMP_INTERP_PLAN") ? atoi(getenv("LARECOMP_INTERP_PLAN")) : 1, LOG("[interp-plan] mode %d\n", plan_env);
        ip_plan_on = plan_env && ip_k > 2, ip_plan_verify = plan_env == 2;
        *hold = ip_keep_image(b, IP_KEEP_N, false);
        flow_note_n();
        // Reduced in-between images: only with frame N's full-size image to resolve them against (FSR 3.1's output, or
        // N's own image)
        ip_lo = ip_lo_factor() < 0.99f &&
                (fsr3_on() || gsr2_on() ? ip_n_post(post_last_ow, post_last_oh) != NULL : ip_keep[IP_KEEP_N].raw_ok && post_last_ow > 0);
    }
    return ok;
}

// The in-between images are side images: they read N's temporal state (camera, object constants, SSAO / GI results,
// TAA history) and leave it as it was, so the real frames chain N-1, N, N+1 in time order whatever comes between them
// (counted as frames of their own they ran the histories back and forth: N, then images before it in time, then N+1 -
// the GI and SSAO accumulation shimmering in blotches, TAA / FSR 3.1 fed the past after the future).
static struct {
    double vp[16], vp_prev[16];
    uint64_t vp_frame, vp_real;
    bool prev_ok;
    uint8_t scene[2][4096];
    uint32_t scene_size[2];
    int scene_cur;
    float proj[4];
} ip_cam;
static void ip_cam_save(void) {
    memcpy(ip_cam.vp, ao.vp, sizeof ao.vp), memcpy(ip_cam.vp_prev, ao.vp_prev, sizeof ao.vp_prev);
    ip_cam.vp_frame = ao.vp_frame, ip_cam.vp_real = ao.vp_real, ip_cam.prev_ok = ao.prev_ok;
    memcpy(ip_cam.scene, ao.scene, sizeof ao.scene), memcpy(ip_cam.scene_size, ao.scene_size, sizeof ao.scene_size);
    ip_cam.scene_cur = ao.scene_cur;
    memcpy(ip_cam.proj, ao.proj, sizeof ao.proj);
}
static void ip_cam_restore(void) {
    memcpy(ao.vp, ip_cam.vp, sizeof ao.vp), memcpy(ao.vp_prev, ip_cam.vp_prev, sizeof ao.vp_prev);
    ao.vp_frame = ip_cam.vp_frame, ao.vp_real = ip_cam.vp_real, ao.prev_ok = ip_cam.prev_ok;
    memcpy(ao.scene, ip_cam.scene, sizeof ao.scene), memcpy(ao.scene_size, ip_cam.scene_size, sizeof ao.scene_size);
    ao.scene_cur = ip_cam.scene_cur;
    memcpy(ao.proj, ip_cam.proj, sizeof ao.proj);
}

// The image at t (0..1, from N-1 to N) into the game's textures (after vk_interp_begin said yes)
void vk_interp_replay(float t) {
    if (ip_flow) { flow.t = t; return; }  // (the optical flow: made at vk_interp_keep)
    gp_mark(vk_cmd(), "ours: replay t=%.2f", t);
    IpFrame *cur = &ipf[ipc];
    ip_t = t;
    {   // LARECOMP_INTERP_TFIX=<t> (research): every in-between image at that t (1: should be frame N itself)
        static float tf = -2;
        if (tf == -2) tf = getenv("LARECOMP_INTERP_TFIX") ? (float)atof(getenv("LARECOMP_INTERP_TFIX")) : -1;
        if (tf >= 0) ip_t = tf;
    }
    ip_cam_save();
    mvo_replay_begin();
    if (ip_lo) vk_end_pass(), vk_tex_lo_begin(ip_lo_factor());  // (ended by vk_interp_keep, after its resolve)
    __typeof__(st) end = st;
    st = cur->start;
    st.rt_dirty = true;
    ip_replaying = true;
    ip_ord = 0;
    ip_occ_reset();
    for (int i = 0; i < cur->nlists; i++) exec_list(ip_copy_lists[i], 0);  // (the copies: ip_list_copy)
    vk_end_pass();
    ip_replaying = false;
    ip_replays++;
    ip_cam_restore();
    st = end;
    st.rt_dirty = true;
    ip_stats[3]++;
    if (ip_stats[3] % 300 == 1)
        LOG("[interp] frame %llu: %llu blended, %llu without a match, %llu words kept, %llu stepped, %llu alternating, %llu curved, %llu draws paired by content (replays %llu)\n",
            (unsigned long long)vk_frame_counter(), (unsigned long long)ip_stats[0], (unsigned long long)ip_stats[1],
            (unsigned long long)ip_stats[2], (unsigned long long)ip_stats_held, (unsigned long long)ip_stats_alt, (unsigned long long)ip_stats_curved,
            (unsigned long long)ip_stats_content, (unsigned long long)ip_stats[3]);
    if (ip_stats[3] % 300 == 0 && getenv("LARECOMP_INTERP_DIAG")) ip_diag_dump();
    if (ip_stats[3] % 300 == 1)
        LOG("[interp] draws with an object %llu, without %llu; vertices blended %llu; another copy as partner %llu, partner taken %llu (sibling-moved cbufs %llu; %llu %llu %llu %llu); matrices along the arc %llu; buffers of frame N reused %llu, copied again %llu\n",
            (unsigned long long)ip_stats_tagged, (unsigned long long)ip_stats_untagged, (unsigned long long)ip_stats_vtx,
            (unsigned long long)ip_stats_other_copy, (unsigned long long)ip_stats_taken, (unsigned long long)ip_stats_sib,
            (unsigned long long)ip_stats_sibw[0], (unsigned long long)ip_stats_sibw[1], (unsigned long long)ip_stats_sibw[2], (unsigned long long)ip_stats_sibw[3], (unsigned long long)ip_stats_rot,
            (unsigned long long)vtx_memo_hits, (unsigned long long)vtx_memo_miss);
}

// After the present's images: the next frame records into the other slot, from the state this one ended with
void vk_interp_end(void) {
    if (!ip_began) return;
    ip_began = false;
    ipc = (ipc + 1) % 3;
    ip_frame_clear(&ipf[ipc]);
    ip_ord = 0, vtx_memo_gen++;  // (the next frame's draws: vtx_memo)
    ipf[ipc].start = st;
    ipf[ipc].destroyed = g_nvn_destroyed;
    ipf[ipc].valid = true;
    ip_occ_reset();
}
