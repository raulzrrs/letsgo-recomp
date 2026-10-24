// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

#include "../../paths.h"
#include "../../settings.h"
#include "../../ui/menu.h"
#include "vk.h"
#include "fsr3.h"
#include "../output_scale.h"
#include "../renderer.h"

VkG g_vk;
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// LARECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    bool validate = getenv("LARECOMP_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LARecomp",
                             .pEngineName = "LARecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

bool g_pipe_exec_stats;  // LARECOMP_PIPE_STATS and the driver has VK_KHR_pipeline_executable_properties

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first (LARECOMP_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("LARECOMP_GPU") ? atoi(getenv("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { g_vk.qfam = i; break; }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
}

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

static bool g_present_log;
static VkQueue present_q;  // the queue the scheduled images are presented on (create_device): the second, or the only one
// The swapchain is used by two threads with frame interpolation (acquire: the render thread; present: present_main), and
// Vulkan wants both externally synchronised: sc_lk around each call. Acquiring never waits holding it (vk_frame_begin).
static pthread_mutex_t sc_lk = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_sc_stale;  // (the present thread saw the swapchain out of date: the render thread recreates it)
static uint64_t g_sc_gen;  // swapchain generation (images acquired from an older one are not presented)

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[12];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    // LARECOMP_PRESENT_LOG=<file>: when each window image reached the screen (present_log_*: VK_KHR_present_wait)
    g_present_log = getenv("LARECOMP_PRESENT_LOG") && has_ext(ep, ne, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                    has_ext(ep, ne, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    if (g_present_log) ext[n_ext++] = VK_KHR_PRESENT_ID_EXTENSION_NAME, ext[n_ext++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
    else if (getenv("LARECOMP_PRESENT_LOG")) LOG("[vk] LARECOMP_PRESENT_LOG: the driver has no VK_KHR_present_wait\n");
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    // LARECOMP_PIPE_STATS: what the driver says about each compiled shader (registers, spills, ...), exec.c pipe_exec_report
    g_pipe_exec_stats = getenv("LARECOMP_PIPE_STATS") && has_ext(ep, ne, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    if (g_pipe_exec_stats) ext[n_ext++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
    else if (getenv("LARECOMP_PIPE_STATS")) LOG("[vk] LARECOMP_PIPE_STATS: the driver has no VK_KHR_pipeline_executable_properties\n");
    g_fault_ext = fault;
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, .pipelineExecutableInfo = VK_TRUE,
        .pNext = f.pNext};
    if (g_pipe_exec_stats) f.pNext = &pe;
    VkPhysicalDevicePresentWaitFeaturesKHR pw = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
                                                 .presentWait = VK_TRUE, .pNext = f.pNext};
    VkPhysicalDevicePresentIdFeaturesKHR pid = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
                                                .presentId = VK_TRUE, .pNext = &pw};
    if (g_present_log) f.pNext = &pid;
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->pipelineStatisticsQuery = h->pipelineStatisticsQuery;  // (LARECOMP_GPUPROF_STATS: invocations per draw)
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    if (getenv("LARECOMP_NO_ROBUST")) b->robustBufferAccess = VK_FALSE;  // (research: what the bounds checks cost)
    b->shaderClipDistance = h->shaderClipDistance;
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    static const float prio[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    // two queues when the family has them: the second only presents (device.c present_q: the scheduled images of frame
    // interpolation, drawn ahead and presented at their time without waiting behind the next frame's work)
    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nqf, NULL);
    VkQueueFamilyProperties qfp[16];
    if (nqf > 16) nqf = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nqf, qfp);
    uint32_t nqueues = g_vk.qfam < nqf && qfp[g_vk.qfam].queueCount >= 2 ? 2 : 1;
    VkDeviceQueueCreateInfo qi[1] = {{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                      .queueCount = nqueues, .pQueuePriorities = prio}};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    present_q = g_vk.queue;
    if (nqueues == 2 && !getenv("LARECOMP_ONE_QUEUE")) vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 1, &present_q);
    LOG("[vk] %s\n", present_q != g_vk.queue ? "a second queue for presenting" : "one queue");

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save)
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = fopen("cache/vk_pipelines.bin", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
}

void vk_pipeline_cache_save(void) {  // (exit and exec.c's periodic save: one writer of the .tmp at a time)
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS || !sz) { pthread_mutex_unlock(&lock); return; }
    void *data = malloc(sz);
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS) {
        mkdir("cache", 0755);
        FILE *f = fopen("cache/vk_pipelines.bin.tmp", "wb");
        if (f) {
            bool ok = fwrite(data, 1, sz, f) == sz;
            fclose(f);
            if (ok) { remove("cache/vk_pipelines.bin"); rename("cache/vk_pipelines.bin.tmp", "cache/vk_pipelines.bin"); }
        }
    }
    free(data);    pthread_mutex_unlock(&lock);
}

// ================================================================== swapchain
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. LARECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    const char *e = getenv("LARECOMP_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    // (+ 5: frame interpolation keeps a present's images drawn and acquired until their time - device.c backend_present)
    uint32_t count = caps.minImageCount + 5;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, NULL);
    if (g_vk.sc_count > 8) g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

static void recreate_swapchain(void) {
    pthread_mutex_lock(&sc_lk);
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
    g_sc_gen++;
    g_sc_stale = false;
    pthread_mutex_unlock(&sc_lk);
}
// Acquire a window image without waiting while holding sc_lk (the present thread may need it to free one)
static VkResult acquire_image(VkSemaphore sem, uint32_t *img) {
    for (int tries = 0;; tries++) {
        pthread_mutex_lock(&sc_lk);
        VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, 0, sem, VK_NULL_HANDLE, img);
        pthread_mutex_unlock(&sc_lk);
        if (r != VK_NOT_READY && r != VK_TIMEOUT) return r;
        if (tries > 20000) return r;  // (~4 s: something is wrong; the caller skips the image)
        SDL_DelayPrecise(200000);
    }
}

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 10
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) return false;  // minimised
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_wait_seq(slot_seq[slot]);
    if (g_sc_stale) recreate_swapchain();
    VkResult r = acquire_image(acquire_sem[slot], &cur_frame.image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        r = acquire_image(acquire_sem[slot], &cur_frame.image);
    }
    static unsigned acquire_fails;  // (a window the driver keeps reporting out of date: logged once per 600, not every frame)
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        if (acquire_fails++ % 600 == 0) LOG("[vk] acquire failed: %d (%u times)\n", r, acquire_fails);
        return false;
    }
    acquire_fails = 0;
    if (trace) LOG("[vk] acquired image %u\n", cur_frame.image);
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) vkCmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// LARECOMP_PRESENT_LOG=<file>: each window image's present call and the time it reached the screen (a thread waits
// for each with vkWaitForPresentKHR): "id,frame,image,nimg,present_ms,shown_ms" (ms since the first image)
static struct { uint64_t id, frame, t; int image, nimg; } pl_q[256];
static uint64_t pl_head, pl_tail, pl_id, pl_t0;
static pthread_mutex_t pl_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pl_cv = PTHREAD_COND_INITIALIZER;
static void *present_log_main(void *arg) {
    FILE *f = arg;
    PFN_vkWaitForPresentKHR wait = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(g_vk.dev, "vkWaitForPresentKHR");
    fputs("id,frame,image,nimg,present_ms,shown_ms\n", f);
    for (;;) {
        pthread_mutex_lock(&pl_lk);
        while (pl_head == pl_tail) pthread_cond_wait(&pl_cv, &pl_lk);
        __typeof__(pl_q[0]) e = pl_q[pl_head % 256];
        pthread_mutex_unlock(&pl_lk);
        VkResult r = wait(g_vk.dev, g_vk.swapchain, e.id, 200000000);
        uint64_t now = SDL_GetTicksNS();
        pthread_mutex_lock(&pl_lk);
        pl_head++;
        pthread_mutex_unlock(&pl_lk);
        fprintf(f, "%llu,%llu,%d,%d,%.3f,%.3f%s\n", (unsigned long long)e.id, (unsigned long long)e.frame, e.image, e.nimg,
                (e.t - pl_t0) / 1e6, (now - pl_t0) / 1e6, r == VK_SUCCESS ? "" : ",fail");
        if (e.id % 64 == 0) fflush(f);
    }
    return NULL;
}
static uint64_t pl_frame; static int pl_image, pl_nimg;
void vk_present_log_tag(uint64_t frame, int image, int nimg) { pl_frame = frame, pl_image = image, pl_nimg = nimg; }

static void recreate_swapchain(void);
// Present window image img (drawn and submitted, done_sem[img] signalled when done) on queue q; frame / image / nimg:
// what it is (LARECOMP_PRESENT_LOG). From the present thread (other_thread) an out-of-date swapchain is only noted.
static void present_swap_image(uint32_t img, VkQueue q, uint64_t frame, int image, int nimg, bool other_thread) {
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] present image %u\n", img);
    VkPresentIdKHR pidi = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1};
    uint64_t id = 0;
    if (g_present_log) {
        static bool started;
        if (!started) {
            started = true;
            pl_t0 = SDL_GetTicksNS();
            FILE *f = fopen(getenv("LARECOMP_PRESENT_LOG"), "w");
            pthread_t th;
            if (f) pthread_create(&th, NULL, present_log_main, f);
            else g_present_log = false;
        }
        id = ++pl_id;
        pidi.pPresentIds = &id;
        pi.pNext = &pidi;
    }
    uint64_t tpres = SDL_GetTicksNS();
    pthread_mutex_lock(&sc_lk);
    VkResult r = vkQueuePresentKHR(q, &pi);
    pthread_mutex_unlock(&sc_lk);
    if (g_present_log) {
        pthread_mutex_lock(&pl_lk);
        if (pl_tail - pl_head < 256) {
            pl_q[pl_tail % 256].id = id, pl_q[pl_tail % 256].frame = frame, pl_q[pl_tail % 256].t = tpres;
            pl_q[pl_tail % 256].image = image, pl_q[pl_tail % 256].nimg = nimg;
            pl_tail++;
            pthread_cond_signal(&pl_cv);
        }
        pthread_mutex_unlock(&pl_lk);
    }
    if (trace) LOG("[vk] presented: %d\n", r);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        if (other_thread) g_sc_stale = true;
        else recreate_swapchain();
    }
}

// End rendering into the window image and present it - or, g_defer_present, leave it drawn for present_swap_image
// (g_deferred_img). shot_path: also save the window image as PNG.
static bool g_defer_present, g_deferred_ok;
static uint32_t g_deferred_img;
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    if (g_defer_present) g_deferred_img = img, g_deferred_ok = true;  // (presented later: present_swap_image)
    else present_swap_image(img, g_vk.queue, pl_frame, pl_image, pl_nimg, false);
    cur_frame.ok = false;
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        vk_save_png(shot_path, px, w, h);
    }
}

// LARECOMP_SHOT_RAW=<file>: the numbered series (".../frame_NNNNN.png") goes to one raw file instead, no PNG encoding:
// per frame a record of u32 frame number + 480x270 RGB (box-filtered), read by tools/bateria_scan.py. Several times faster
// than PNGs (and ffmpeg) for a 900-frame route.
#define RAW_W 480
#define RAW_H 270
static bool shot_raw(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    static FILE *f; static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("LARECOMP_SHOT_RAW");
        if (e && *e) { f = fopen(e, "wb"); if (f) setvbuf(f, NULL, _IOFBF, 1 << 20); else LOG("[vk] cannot open %s\n", e); }
    }
    const char *n = strstr(path, "frame_");
    if (!f || !n) return false;
    static uint8_t out[RAW_W * RAW_H * 3];
    for (uint32_t y = 0; y < RAW_H; y++) {
        uint32_t y0 = (uint64_t)y * h / RAW_H, y1 = (uint64_t)(y + 1) * h / RAW_H; if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t x = 0; x < RAW_W; x++) {
            uint32_t x0 = (uint64_t)x * w / RAW_W, x1 = (uint64_t)(x + 1) * w / RAW_W; if (x1 <= x0) x1 = x0 + 1;
            uint32_t s[3] = {0, 0, 0};
            for (uint32_t yy = y0; yy < y1; yy++)
                for (uint32_t xx = x0; xx < x1; xx++) { const uint8_t *p = rgba + ((uint64_t)yy * w + xx) * 4; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; }
            uint32_t c = (y1 - y0) * (x1 - x0);
            uint8_t *o = out + ((uint64_t)y * RAW_W + x) * 3;
            o[0] = (uint8_t)(s[0] / c); o[1] = (uint8_t)(s[1] / c); o[2] = (uint8_t)(s[2] / c);
        }
    }
    uint32_t fn = (uint32_t)atol(n + 6);
    fwrite(&fn, 4, 1, f); fwrite(out, sizeof out, 1, f); fflush(f);  // (the game leaves with _exit)
    if (getenv("LARECOMP_SHOT_RAW_LAST") && (long)fn >= atol(getenv("LARECOMP_SHOT_RAW_LAST"))) { LOG("[vk] raw frames done at %u\n", fn); fflush(stderr); _exit(0); }
    return true;
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    if (shot_raw(path, rgba, w, h)) return;
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
static uint64_t g_window_images;  // (images presented: present_image)
uint64_t renderer_present_count(void) { return total_frames; }
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
void backend_represent(void);
static void represent_job(void *unused) { (void)unused; backend_represent(); }
void backend_represent(void) {
    if (rt_active() && !rt_on_render_thread()) { rt_call(represent_job, NULL); return; }  // (the keyboard dialog: game thread)
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
}

// One image into the window: b (the game's presented texture, or one exec.c kept: vk_interp_keep), the overlay menu,
// the test shots. frame: the present it belongs to (the count when it was made); image / nimg: which of its images.
static void present_image(TexBackend *b, bool final, int image, int nimg, uint64_t frame) {
    static const float black[4] = {0, 0, 0, 1};
    vk_end_pass();  // (a scheduled image comes between the next frame's lists: their pass begins again after it)
    uint64_t t0 = SDL_GetTicksNS();
    bool have = vk_frame_begin(black);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        // render scale + placement in the window (the game's 16:9 image, fitted): runtime/nvn/output_scale.c;
        // no GPU timer here (-1): dynamic resolution goes by the frame time
        OutScale os;
        output_scale(dw, dh, false, SDL_GetTicksNS() / 1e9, -1, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh;
        float crop[4] = {0, 0, 1, 1};  // uv origin and size: the whole image
        {   // where the game image is in the window (pixels): the mouse as a finger on the touch screen (hle/input.c)
            extern double g_present_rect[4];
            g_present_rect[0] = vx, g_present_rect[1] = vy, g_present_rect[2] = vw, g_present_rect[3] = vh;
        }
        renderer_set_scale((float)sx, (float)sy);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        // The game's first two presents are a blue full-screen 2D quad drawn while it loads (then black, then the
        // controller screen): a flash on every start. Keep the window black for them. LARECOMP_HIDE_FIRST_FRAMES=<n>
        // (0: show everything).
        static long hide = -1;
        if (hide < 0) hide = getenv("LARECOMP_HIDE_FIRST_FRAMES") ? atol(getenv("LARECOMP_HIDE_FIRST_FRAMES")) : 2;
        if ((long)frame >= hide) vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
        if (final && menu_at && (long)frame + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("LARECOMP_WINDOW_SHOT") ? atol(getenv("LARECOMP_WINDOW_SHOT")) : 0;
        // (the in-between image: build/window_mid.png, with interpolation on)
        bool shot = win_shot && (long)frame + 1 == win_shot;
        // LARECOMP_WINDOW_SHOTS=<n>,<n>,...: the in-between image and the frame of each, build/frames/{mid,fin}_<n>.png
        static long list[512]; static int nlist = -1;
        if (nlist < 0) {
            nlist = 0;
            const char *e = getenv("LARECOMP_WINDOW_SHOTS");
            for (; e && *e && nlist < 512; nlist++) { list[nlist] = atol(e); e = strchr(e, ','); if (!e) { nlist++; break; } e++; }
        }
        static char lpath[96];
        extern long fps60_scene_start(void);
        static bool rel_list = false;
        { static bool init; if (!init) { init = true; rel_list = getenv("LARECOMP_WINDOW_SHOTS_REL") != NULL; } }
        // (k > 2: the in-between images are mid1 .. mid<k-1>)
        char mid[16];
        if (nimg > 2) snprintf(mid, sizeof mid, "mid%d", image + 1);
        else snprintf(mid, sizeof mid, "mid");
        static char wpath[64];
        snprintf(wpath, sizeof wpath, "build/window_%s.png", mid);
        const char *path = shot ? (final ? "build/window.png" : wpath) : NULL;
        for (int i = 0; i < nlist; i++)
            if (rel_list ? (fps60_scene_start() >= 0 && (long)frame + 1 - fps60_scene_start() == list[i])
                         : (long)frame + 1 == list[i]) {
                mkdir("build/frames", 0755);
                snprintf(lpath, sizeof lpath, "build/frames/%s_%ld.png", final ? "fin" : mid, list[i]);
                path = lpath;
            }
        extern void vk_present_log_tag(uint64_t frame, int image, int nimg);
        vk_present_log_tag(frame, image, nimg);
        vk_frame_end(path);
        g_window_images++;
        if (final && shot && getenv("LARECOMP_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    } else {
        renderer_submit();
    }
}

// ---- the images of a present, on schedule
// With frame interpolation a present has k images (exec.c vk_interp_*): the k - 1 in-between ones, rendered and kept
// at once (the next frame's recording clears what they are made of), then frame N's own, a k-th of the period apart.
// They wait here: the render thread shows them when they are due, between the next frame's jobs or while it has none
// (renderthread.c), so the game goes on meanwhile; without it this thread waits.
// Evenly apart, all of them: the first one is not shown as soon as it is rendered but `ip_lead` after the present (the
// replays' time on the GPU, learnt below), on a timeline that follows the game's presents smoothly. Shown at once, the
// first came early and the last on time: at 2x, 6.7 / 10 ms apart instead of 8.3 / 8.3 - the window's rate doubled
// but the motion went in steps of two, much of the smoothness gone (LARECOMP_PRESENT_LOG shows it).
typedef struct { TexBackend *b; bool final; int image, nimg; uint64_t frame, due, seq; bool pre; uint32_t sc_img; uint64_t gen; } Pending;
static Pending pend[16];
static int pend_n;
static uint64_t ip_lead = 2000000;  // ns from the present's time on the timeline to its first image
// An image shown at its time (sequence seq on the GPU): not done by then, more lead (up to 16 ms: at 4K the GPU finishes
// a present's images 9 - 12.5 ms after it); done, a little less, slowly
static void lead_adapt(uint64_t seq) {
    extern uint64_t vk_completed_seq(void);
    if (vk_completed_seq() < seq) ip_lead = ip_lead + 250000 > 16000000 ? 16000000 : ip_lead + 250000;
    else if (ip_lead > 500000) ip_lead -= 5000;
}

// A scheduled image to the window: drawn ahead (pre: presented from present_q - the next frame's work on the main queue
// doesn't hold it up), or drawn now
static void pending_show(const Pending *p) {
    if (!p->pre) { present_image(p->b, p->final, p->image, p->nimg, p->frame); return; }
    if (p->gen != g_sc_gen) return;  // (its swapchain is gone)
    present_swap_image(p->sc_img, present_q, p->frame, p->image, p->nimg, false);
}

// ---- the present thread: the images drawn ahead (pre) are presented here at their time - the render thread waits for
// the GPU inside some of the next frame's jobs (at 4K it runs ~10 ms behind), and an image due meanwhile went out with
// the next one (each present's last image ~8 ms late, beside the next present's first: one of the two lost)
static Pending pq[32];
static int pq_n;
static pthread_mutex_t pq_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pq_cv = PTHREAD_COND_INITIALIZER;
static void pq_show(const Pending *p, bool other_thread) {
    if (p->gen != g_sc_gen) return;
    if (p->seq) lead_adapt(p->seq);
    present_swap_image(p->sc_img, present_q, p->frame, p->image, p->nimg, other_thread);
}
static void *present_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&pq_lk);
    for (;;) {
        if (!pq_n) { pthread_cond_wait(&pq_cv, &pq_lk); continue; }
        uint64_t now = SDL_GetTicksNS(), due = pq[0].due;
        if (due > now) {  // (1 ms slices of the high-resolution timer: a timed wait on the condition woke up late)
            pthread_mutex_unlock(&pq_lk);
            SDL_DelayPrecise(due - now > 1000000 ? 1000000 : due - now);
            pthread_mutex_lock(&pq_lk);
            continue;
        }
        Pending p = pq[0];
        memmove(pq, pq + 1, (size_t)--pq_n * sizeof *pq);
        pthread_mutex_unlock(&pq_lk);
        pq_show(&p, true);
        pthread_mutex_lock(&pq_lk);
    }
    return NULL;
}
static void pq_push(const Pending *p) {
    static bool started;
    if (!started) {
        started = true;
        pthread_t th;
        pthread_create(&th, NULL, present_main, NULL);
    }
    pthread_mutex_lock(&pq_lk);
    if (pq_n < 32) pq[pq_n++] = *p;
    pthread_cond_signal(&pq_cv);
    pthread_mutex_unlock(&pq_lk);
}
// The oldest image held drawn, presented now (before acquiring another would wait for it); false: none
static bool pq_pop_oldest(void) {
    pthread_mutex_lock(&pq_lk);
    if (!pq_n) { pthread_mutex_unlock(&pq_lk); return false; }
    Pending p = pq[0];
    memmove(pq, pq + 1, (size_t)--pq_n * sizeof *pq);
    pthread_mutex_unlock(&pq_lk);
    pq_show(&p, false);
    return true;
}
// Images presented before their time because the window images held drawn ran out (backend_present): frame generation
// "Auto" takes them as frames come late (exec.c ip_auto_mult)
static uint64_t pq_forced;
uint64_t vk_fg_forced_presents(void) { return pq_forced; }
static int pq_count(void) {
    pthread_mutex_lock(&pq_lk);
    int n = pq_n;
    pthread_mutex_unlock(&pq_lk);
    return n;
}
static void pq_flush(void) { while (pq_pop_oldest()) {} }  // (an image shown now must come after them)

// The scheduled images due by now (all: every one, now - the next present's must not come before them)
void vk_pending_run(bool all) {
    while (pend_n) {
        if (!all && pend[0].due > SDL_GetTicksNS()) return;
        Pending p = pend[0];
        memmove(pend, pend + 1, (size_t)--pend_n * sizeof *pend);
        if (p.seq && !all) lead_adapt(p.seq);
        static int plog = -1;  // LARECOMP_PENDING_LOG=1: each scheduled image's lateness when it is shown
        if (plog < 0) plog = getenv("LARECOMP_PENDING_LOG") != NULL;
        if (plog) {
            LOG("[pend] f%llu %d/%d late %.2f ms (lead %.2f)%s\n", (unsigned long long)p.frame, p.image, p.nimg,
                ((int64_t)SDL_GetTicksNS() - (int64_t)p.due) / 1e6, ip_lead / 1e6, all ? " all" : "");
        }
        pending_show(&p);
    }
}

// The display's refresh (Windows: the compositor's timing, DwmGetCompositionTimingInfo): the last vertical blank on
// SDL's clock and the period, in ns; false when unknown
// The display's refresh rate (Windows' compositor timing), read at most once a second; 0 when unknown (frame
// generation "Auto": exec.c ip_auto_mult)
double vk_display_hz(void) {
#ifdef _WIN32
    static double hz;
    static uint64_t at;
    uint64_t now = SDL_GetTicksNS();
    if (at && now - at < 1000000000ull) return hz;
    at = now;
    DWM_TIMING_INFO ti = {.cbSize = sizeof ti};
    if (DwmGetCompositionTimingInfo(NULL, &ti) == S_OK && ti.rateRefresh.uiDenominator && ti.rateRefresh.uiNumerator)
        hz = (double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator;
    else if (DwmGetCompositionTimingInfo(NULL, &ti) == S_OK && ti.qpcRefreshPeriod) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        hz = (double)f.QuadPart / (double)ti.qpcRefreshPeriod;
    }
    return hz;
#else
    return 0;
#endif
}

static bool vblank_grid(uint64_t *vb, uint64_t *vp) {
#ifdef _WIN32
    static int off = -1;
    // (LARECOMP_FG_VRR=1: variable refresh shows each image when it comes - no grid to wait for)
    if (off < 0) off = getenv("LARECOMP_FG_NO_VSYNC_GRID") != NULL || getenv("LARECOMP_FG_VRR") != NULL;
    if (off) return false;
    DWM_TIMING_INFO ti = {.cbSize = sizeof ti};
    if (DwmGetCompositionTimingInfo(NULL, &ti) != S_OK || !ti.qpcRefreshPeriod || !ti.qpcVBlank) return false;
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    uint64_t t = SDL_GetTicksNS();
    double ns = 1e9 / (double)f.QuadPart;
    int64_t ago = (int64_t)((double)(now.QuadPart - (int64_t)ti.qpcVBlank) * ns);
    *vp = (uint64_t)((double)ti.qpcRefreshPeriod * ns);
    if (*vp < 2000000 || *vp > 50000000) return false;  // (500 .. 20 Hz: anything else is not a display's)
    *vb = t - (uint64_t)ago;
    return true;
#else
    (void)vb, (void)vp;
    return false;
#endif
}

// The images of the presents before `frame` still waiting (their kept images are about to be written again)
static void pending_run_before(uint64_t frame) {
    while (pend_n && pend[0].frame < frame) {
        Pending p = pend[0];
        memmove(pend, pend + 1, (size_t)--pend_n * sizeof *pend);
        pending_show(&p);
    }
}

void vk_pending_drop(void) {  // (a reboot: renderthread.c rt_reset, the render thread stopped)
    pend_n = 0;
    pthread_mutex_lock(&pq_lk);
    pq_n = 0;
    pthread_mutex_unlock(&pq_lk);
}

// When the next scheduled image is due (false: none)
bool vk_pending_next(uint64_t *due) {
    if (!pend_n) return false;
    *due = pend[0].due;
    return true;
}

// Without the render thread: wait for the scheduled images and show them
static void pending_wait_all(void) {
    while (pend_n) {
        uint64_t now = SDL_GetTicksNS();
        if (pend[0].due > now) SDL_DelayPrecise(pend[0].due - now);
        vk_pending_run(false);
    }
}

void backend_present(NvnTexture *t) {
    {   // (LARECOMP_HEAL_KEYS research key F5: every texture re-checks its memory)
        extern volatile int g_heal_tex;
        extern void backend_pool_written(uint64_t addr, uint64_t size);
        if (g_heal_tex) { g_heal_tex = 0; backend_pool_written(0x7000000000ull, 0x320000000ull); }
    }
    // (the images of the present before last still waiting: this one's replays write their kept images - exec.c keeps
    // two sets; the last present's can wait for their time)
    if (total_frames) pending_run_before(total_frames - 1);
    g_last_presented = t;
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    uint64_t t0 = SDL_GetTicksNS();
    static uint64_t last_present, period = 33333333;
    if (last_present && t0 - last_present > 5000000 && t0 - last_present < 100000000) period = (period * 7 + (t0 - last_present)) / 8;
    last_present = t0;
    {   // 60 fps mode: a menu's frames come every 33.3 ms, but the smoothed period still holds the world's 16.7 ms for its
        // first frames - the second image would come too early
        extern bool fps60_menu(void);
        if (fps60_menu()) period = 33333333;
    }
    // Frame interpolation (exec.c vk_interp_begin): k images per present (vk_interp_mult, from the period)
    TexBackend *hold = NULL;
    extern bool vk_interp_begin(TexBackend *b, TexBackend **hold, uint64_t period);
    extern void vk_interp_replay(float t);
    extern TexBackend *vk_interp_keep(TexBackend *b, int slot, bool now);
    extern void vk_interp_end(void);
    extern int vk_interp_images(void);
    bool inter = vk_interp_begin(b, &hold, period);
    int nimg = inter ? vk_interp_images() : 1;
    // the timeline: a period after the last present's time, pulled toward this one's (t0 jitters with the jobs before
    // it); a jump (a stall, a change of rate) takes t0
    static uint64_t tl;
    int64_t off = (int64_t)(t0 - (tl + period));
    if (!tl || off > (int64_t)period / 2 || off < -(int64_t)period / 2) tl = t0;
    else tl = tl + period + off / 8;
    if (!inter) vk_pending_run(true), pq_flush();  // (the game's own texture, shown now: after any image still waiting)
    extern uint64_t vk_pending_seq(void);
    uint64_t vbl = 0, vper = 0;
    bool have_grid = inter && vblank_grid(&vbl, &vper);
    static int one_queue_path = -1;  // (LARECOMP_FG_DRAW_AT_TIME=1: research, the images drawn at their time as before)
    if (one_queue_path < 0) one_queue_path = getenv("LARECOMP_FG_DRAW_AT_TIME") != NULL;
    for (int image = 0; image < nimg && pend_n < 16; image++) {
        bool final = image == nimg - 1;
        TexBackend *img = final ? (inter ? hold : b) : NULL;
        if (!final) {  // (rendered into the game's textures, then kept: the next replay writes them again)
            vk_interp_replay((float)(image + 1) / (float)nimg);
            img = vk_interp_keep(b, image, !one_queue_path);  // (drawn into a window image just below: not copied)
        }
        uint64_t due = inter ? tl + ip_lead + period * (uint64_t)image / (uint64_t)nimg : t0;
        // On the display's refresh grid: a quarter of a refresh after the blank before its time, so it is shown at the next
        // one. Off the grid, an image due near a blank went at that blank or the one after by microseconds: 1 / 3
        // refreshes apart instead of 2 (240 Hz, 120 images: 12 % of the intervals).
        if (inter && have_grid && due > vbl) due = vbl + (due - vbl) / vper * vper + vper / 4;
        Pending pe = {img, final, image, nimg, total_frames, due, inter ? vk_pending_seq() : 0};
        if (inter && !one_queue_path) {
            // Drawn into a window image now, right after its replay (the GPU does it before the next frame's work), and
            // only presented at its time from present_q. Drawn at its time on the one queue, it waited behind the next
            // frame's lists: at 4K a fifth of the images reached the screen with the next one (one of the two lost).
            // (Window images held drawn: at most the swapchain's less 3 - beyond, the oldest goes now, or acquiring
            // would wait for a present this thread makes.)
            while (pq_count() > 0 && pq_count() >= (int)g_vk.sc_count - 3) pq_pop_oldest(), pq_forced++;
            g_defer_present = true, g_deferred_ok = false;
            present_image(img, final, image, nimg, total_frames);
            g_defer_present = false;
            if (!g_deferred_ok) continue;  // (no window image: nothing to show)
            pe.pre = true, pe.sc_img = g_deferred_img, pe.gen = g_sc_gen, pe.seq = vk_pending_seq() - 1;
            pq_push(&pe);  // (presented by present_main at its time)
            continue;
        }
        pend[pend_n++] = pe;
        vk_pending_run(false);  // (the last present's images due meanwhile)
    }
    vk_interp_end();
    {   // LARECOMP_FG_GPULAG=1 (research): how long after the present the GPU finishes what was submitted by now (waits)
        static int lag = -1;
        if (lag < 0) lag = getenv("LARECOMP_FG_GPULAG") != NULL;
        if (lag && inter) {
            extern void vk_wait_seq(uint64_t seq);
            renderer_submit();
            uint64_t sq = vk_pending_seq() - 1, w0 = SDL_GetTicksNS();
            uint64_t done0 = 0;
            { extern uint64_t vk_completed_seq(void); done0 = vk_completed_seq(); }
            vk_wait_seq(sq);
            LOG("[gpulag] present %llu: prepared at +%.2f ms, GPU done at +%.2f ms (%llu submissions behind)\n",
                (unsigned long long)total_frames, (w0 - t0) / 1e6, (SDL_GetTicksNS() - t0) / 1e6, (unsigned long long)(sq - done0));
        }
    }
    if (inter) renderer_submit();  // (the GPU starts on the replays now, not when the first image is shown)
    if (!rt_active()) pending_wait_all();
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    frames++;
    total_frames++;
    g_frame_counter++;
    {   // Opt-in game-present intervals (not the extra menu image shown by the 60 fps mode).
        static FILE *csv;
        static uint64_t previous;
        static bool initialized;
        if (!initialized) {
            initialized = true;
            const char *path = getenv("LARECOMP_FRAMETIME_CSV");
            if (path && *path) {
                csv = fopen(path, "w");
                if (csv) fputs("present,dt_ms\n", csv);
                else LOG("[vk] cannot open frametime CSV: %s\n", path);
            }
        }
        if (csv) {
            uint64_t now = SDL_GetTicksNS();
            if (previous) fprintf(csv, "%llu,%.6f\n", (unsigned long long)total_frames, (now - previous) / 1e6);
            previous = now;
            if (total_frames % 60 == 0) fflush(csv);
            static long exit_at = -1;
            if (exit_at < 0) exit_at = getenv("LARECOMP_EXIT_AT_PRESENT") ? atol(getenv("LARECOMP_EXIT_AT_PRESENT")) : 0;
            if (exit_at && (long)total_frames >= exit_at) { LOG("[vk] exit at present %llu\n", (unsigned long long)total_frames); fflush(csv); fflush(stderr); _exit(0); }
        }
    }
    {   // LARECOMP_FG_AT=<present>:<k>,...: frame generation set to k (0, 2, 3, 4) at those presents, as the F12 menu does
        const char *e = getenv("LARECOMP_FG_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            int k = *p == ':' ? (int)strtol(p + 1, (char **)&p, 10) : 0;
            if (at == (long)total_frames) { g_settings.frame_gen = (k >= 2 && k <= 4) || k == FG_AUTO ? k : 0; LOG("[vk] frame generation %d (LARECOMP_FG_AT)\n", k); }
            if (*p == ',') p++; else break;
        }
    }
    {   // LARECOMP_FPS_AT=<present>:<fps>,...: the frame rate limit changed at those presents, as the F12 menu does
        const char *e = getenv("LARECOMP_FPS_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            int fps = *p == ':' ? (int)strtol(p + 1, (char **)&p, 10) : 30;
            if (at == (long)total_frames) { g_settings.fps = fps; LOG("[vk] frame rate limit %d (LARECOMP_FPS_AT)\n", fps); }
            if (*p == ',') p++; else break;
        }
    }
    {   // LARECOMP_SET_AT="<present>:<key> = <value>;...": a larecomp.ini line applied at that present, as the F12 menu
        // changes a setting while the game runs (tests of switching one: SSAO / GI levels, the anti-aliasing...)
        const char *e = getenv("LARECOMP_SET_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            const char *end = strchr(p, ';');
            if (*p == ':' && at == (long)total_frames) {
                char line[160];
                int n = (int)(end ? end - p - 1 : (long)strlen(p + 1));
                snprintf(line, sizeof line, "%.*s", n < 0 ? 0 : n, p + 1);
                extern void settings_set_line(const char *line);
                settings_set_line(line);
                LOG("[vk] setting \"%s\" (LARECOMP_SET_AT)\n", line);
            }
            if (!end) break;
            p = end + 1;
        }
    }
    {   // LARECOMP_RENDERDOC_AT=<frame>: under RenderDoc (renderdoccmd capture), capture the frame after that one
        extern void renderdoc_frame(uint64_t frame);
        renderdoc_frame(total_frames);
    }
    // (with the render thread the game thread runs these three when it queues the present: rt_present)
    extern void input_frame(uint64_t frame);
    if (!rt_on_render_thread()) input_frame(total_frames);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SLOW_FRAME_MS")) : 0;
    if (slow_ms > 0 && total_frames > 2600) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    if (!rt_on_render_thread()) fnprof_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    if (!rt_on_render_thread()) reboot_test_frame(total_frames);
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // LARECOMP_DEBUG_FRAMES=<n> (with LARECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // LARECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("LARECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // LARECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("LARECOMP_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > getenv("LARECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)total_frames);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    {   // LARECOMP_SHOT_REL=<n>,<n>,...: the game image n presents after a capture began (fps60.c marks it)
        extern long fps60_scene_start(void);
        static const char *rel = (const char *)1;
        if (rel == (const char *)1) { rel = getenv("LARECOMP_SHOT_REL"); if (rel) mkdir("build/frames", 0755); }
        long st = rel ? fps60_scene_start() : -1;
        for (const char *p = rel; st >= 0 && p && *p;) {
            long n = strtol(p, (char **)&p, 10);
            if ((long)total_frames == st + n) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/rel_%04ld.png", n);
                renderer_save_texture_tex(b, path);
            }
            if (*p == ',') p++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until LARECOMP_WINDOW_SHOT exits
        if (getenv("LARECOMP_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("LARECOMP_FPS_INTERVAL") ? atof(getenv("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        static uint64_t img_prev;  // (the images that reached the window: with frame interpolation, k per frame)
        if (last_fps_time > 0 && g_window_images - img_prev != frames)
            LOG("[vk] t=%.1f window %.1f fps (lead %.1f ms)\n", now, (double)(g_window_images - img_prev) / (now - last_fps_time),
                ip_lead / 1e6);
        img_prev = g_window_images;
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms (%s)\n", now, (unsigned long long)total_frames,
                frames / (now - last_fps_time), max_gap * 1000.0, renderer_stats());
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    settings_load();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    input_load_user_mappings();
    int win_w = 1280, win_h = 720;
    if (getenv("LARECOMP_WINDOW")) sscanf(getenv("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow(RT_APP_NAME, win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
    if (g_settings.fullscreen && !getenv("LARECOMP_WINDOW") && !getenv("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(window, true);
    create_instance();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
}

SDL_Window *ui_window(void) { return window; }

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    rt_reset();  // (the queued work belongs to the game that is going away)
    if (!g_vk.dev) return;
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16)) {
            if (e.type == SDL_EVENT_QUIT) {
                LOG("window closed\n");
                exit(0);
            }
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
    }
}
