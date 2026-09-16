// A Vulkan path tracer: the final scene of "Ray Tracing in One Weekend"
// (https://raytracing.github.io/books/RayTracingInOneWeekend.html) on VK_KHR_ray_tracing_pipeline,
// progressively accumulated one frame at a time. The counterpart of test/path_tracer/d3d12 and
// test/path_tracer/metal, and something to debug ray tracing with:
//
//   - three bottom-level structures of bounding boxes (one per material, built in one call with
//     their scratch at three offsets into one buffer) under a top-level structure of three
//     instances, each with a custom index (its first sphere) and a shader binding table offset
//     (its material), with memory barriers between the builds and the trace that reads them;
//   - a ray tracing pipeline with an intersection shader shared by three procedural hit groups,
//     one closest hit shader per material, a miss shader and a ray generation shader that walks
//     each path in a loop;
//   - a storage image that every frame reads and writes (the running mean), so a frame depends
//     on the frames before it, and an output image blitted to the swapchain.
//
// Usage: vkinsp_path_tracer [--frames N] [--width W] [--height H] [--spp N] [--depth N]
//                           [--no-accumulate] [--rebuild]
//
//   --spp N           samples per pixel per frame (1)
//   --depth N         bounces per path (50, the book's)
//   --no-accumulate   every frame stands alone: noisy, but independent of the frames before it
//   --rebuild         rebuild every acceleration structure in every frame, so a captured frame
//                     holds the builds as well as the trace
//
// The window is resizable; a new size starts the accumulation again.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <xcb/xcb.h>
#endif

#include "../scene.h"

#define CHECK(x)                                                                     \
    do {                                                                             \
        VkResult r_ = (x);                                                           \
        if (r_ != VK_SUCCESS) {                                                      \
            fprintf(stderr, "%s failed: %d (%s:%d)\n", #x, (int)r_, __FILE__, __LINE__); \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

namespace {

std::vector<char> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path.c_str());
        exit(1);
    }
    std::vector<char> data((size_t)f.tellg());
    f.seekg(0);
    f.read(data.data(), (std::streamsize)data.size());
    return data;
}

std::string ExeDir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    return s.substr(0, s.find_last_of("\\/") + 1);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string s(buf, n > 0 ? (size_t)n : 0);
    return s.substr(0, s.find_last_of('/') + 1);
#endif
}

VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a) {
    return a ? (v + a - 1) / a * a : v;
}

// A buffer with a device address, optionally host visible (mapped).
struct DeviceBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceAddress address = 0;
};

struct StorageImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

constexpr VkFormat kOutputFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kAccumulationFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkShaderStageFlags kRayStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                                          VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
constexpr uint32_t M = rtiow::kMaterialCount;

// The pipeline's groups: raygen, miss, then one procedural hit group per material, each the
// shared intersection shader and the material's closest hit shader.
constexpr uint32_t kGroupCount = 2 + M;

struct App {
    uint32_t width = 960, height = 540;
    int maxFrames = -1;
    uint32_t samplesPerFrame = 1;
    uint32_t maxDepth = 50;
    bool accumulate = true;
    bool rebuild = false;
    bool quit = false;
    bool resized = false;

#if defined(_WIN32)
    HWND hwnd = nullptr;
#else
    xcb_connection_t* conn = nullptr;
    xcb_window_t window = 0;
#endif

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    // The ray tracing entry points, through vkGetDeviceProcAddr: the SDK's import library does not
    // export them.
    struct {
        PFN_vkCreateAccelerationStructureKHR createAS = nullptr;
        PFN_vkDestroyAccelerationStructureKHR destroyAS = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR buildSizes = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR asAddress = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR build = nullptr;
        PFN_vkCreateRayTracingPipelinesKHR createPipelines = nullptr;
        PFN_vkGetRayTracingShaderGroupHandlesKHR groupHandles = nullptr;
        PFN_vkCmdTraceRaysKHR trace = nullptr;
    } rt;
    PFN_vkSetDebugUtilsObjectNameEXT setName = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel = nullptr;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> swapImages;
    std::vector<VkSemaphore> renderFinished;   // one per swapchain image
    VkSemaphore imageAvailable = VK_NULL_HANDLE;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer frameCommands = VK_NULL_HANDLE;
    VkFence frameDone = VK_NULL_HANDLE;
    uint64_t frameCount = 0;
    uint32_t accumulated = 0;   // frames in the running mean

    // The scene.
    rtiow::Scene scene;
    DeviceBuffer spheres, cameraBuffer;
    DeviceBuffer aabbs[M];
    DeviceBuffer blasMemory[M];
    VkAccelerationStructureKHR blas[M]{};
    VkDeviceSize blasScratchOffset[M]{};
    DeviceBuffer instances, tlasMemory, scratch;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;

    // The images, sized to the swapchain.
    StorageImage output, accumulation;

    // The pipeline and its binding table.
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    DeviceBuffer sbt;
    VkStridedDeviceAddressRegionKHR raygenRegion{}, missRegion{}, hitRegion{}, callableRegion{};

    // --------------------------------------------------------------------------------- window
#if defined(_WIN32)
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        App* app = (App*)GetWindowLongPtrA(h, GWLP_USERDATA);
        if (msg == WM_CLOSE || msg == WM_DESTROY) {
            if (app) app->quit = true;
            return 0;
        }
        if (msg == WM_KEYDOWN && w == VK_ESCAPE && app) app->quit = true;
        if (msg == WM_SIZE && app) app->resized = true;
        return DefWindowProcA(h, msg, w, l);
    }

    void CreateWindowNative() {
        WNDCLASSA wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "vkinsp_path_tracer";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&wc);
        RECT r{0, 0, (LONG)width, (LONG)height};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowA(wc.lpszClassName, "GPU Inspector test: Vulkan path tracer", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                             wc.hInstance, nullptr);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        resized = false;   // the WM_SIZE of creation
    }

    void PumpEvents() {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
#else
    void CreateWindowNative() {
        conn = xcb_connect(nullptr, nullptr);
        const xcb_setup_t* setup = xcb_get_setup(conn);
        xcb_screen_t* screen = xcb_setup_roots_iterator(setup).data;
        window = xcb_generate_id(conn);
        uint32_t values[] = {screen->black_pixel, XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, (uint16_t)width, (uint16_t)height, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
        xcb_map_window(conn, window);
        xcb_flush(conn);
    }

    void PumpEvents() {
        while (xcb_generic_event_t* e = xcb_poll_for_event(conn)) {
            uint8_t type = e->response_type & 0x7f;
            if (type == XCB_KEY_PRESS) {
                quit = true;
            } else if (type == XCB_CONFIGURE_NOTIFY) {
                auto* c = reinterpret_cast<xcb_configure_notify_event_t*>(e);
                if (c->width != width || c->height != height) {
                    width = c->width;
                    height = c->height;
                    resized = true;
                }
            }
            free(e);
        }
    }
#endif

    // --------------------------------------------------------------------------------- helpers
    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags props) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) return i;
        fprintf(stderr, "no memory type\n");
        exit(1);
    }

    void Name(VkObjectType type, uint64_t handle, const char* name) {
        if (!setName) return;
        VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        ni.objectType = type;
        ni.objectHandle = handle;
        ni.pObjectName = name;
        setName(device, &ni);
    }

    void BeginLabel(VkCommandBuffer cb, const char* name) {
        if (!beginLabel) return;
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = name;
        beginLabel(cb, &label);
    }

    void EndLabel(VkCommandBuffer cb) {
        if (endLabel) endLabel(cb);
    }

    // Every buffer here has a device address: the acceleration structure inputs and the binding
    // table need one, and the rest do not mind.
    DeviceBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, const char* name) {
        DeviceBuffer b;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        CHECK(vkCreateBuffer(device, &bci, nullptr, &b.buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &flags;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                                      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(device, &mai, nullptr, &b.memory));
        CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        if (host) CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
        VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ai.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(device, &ai);
        Name(VK_OBJECT_TYPE_BUFFER, (uint64_t)b.buffer, name);
        return b;
    }

    DeviceBuffer CreateBufferWithData(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, const char* name) {
        DeviceBuffer b = CreateBuffer(size, usage, true, name);
        memcpy(b.mapped, data, (size_t)size);
        return b;
    }

    void DestroyBuffer(DeviceBuffer& b) {
        if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device, b.memory, nullptr);
        b = DeviceBuffer{};
    }

    StorageImage CreateStorageImage(VkFormat format, VkImageUsageFlags usage, const char* name) {
        StorageImage s;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | usage;
        CHECK(vkCreateImage(device, &ici, nullptr, &s.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, s.image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(device, &mai, nullptr, &s.memory));
        CHECK(vkBindImageMemory(device, s.image, s.memory, 0));
        Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)s.image, name);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = s.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CHECK(vkCreateImageView(device, &vci, nullptr, &s.view));
        return s;
    }

    void DestroyStorageImage(StorageImage& s) {
        if (s.view) vkDestroyImageView(device, s.view, nullptr);
        if (s.image) vkDestroyImage(device, s.image, nullptr);
        if (s.memory) vkFreeMemory(device, s.memory, nullptr);
        s = StorageImage{};
    }

    VkShaderModule LoadShader(const char* file) {
        std::vector<char> code = ReadFile(ExeDir() + file);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m;
        CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
        Name(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)m, file);
        return m;
    }

    VkCommandBuffer BeginOneShot() {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = commandPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb;
        CHECK(vkAllocateCommandBuffers(device, &ai, &cb));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(vkBeginCommandBuffer(cb, &bi));
        return cb;
    }

    void EndOneShot(VkCommandBuffer cb) {
        CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        vkFreeCommandBuffers(device, commandPool, 1, &cb);
    }

    static void Barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags dstAccess,
                        VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // --------------------------------------------------------------------------------- setup
    void InitVulkan() {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "vkinsp_path_tracer";
        ai.pEngineName = "none";
        ai.apiVersion = VK_API_VERSION_1_2;   // buffer device addresses and SPIR-V 1.4
        std::vector<const char*> instExts = {VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(_WIN32)
                                             VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#else
                                             VK_KHR_XCB_SURFACE_EXTENSION_NAME,
#endif
        };
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> avail(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, avail.data());
        bool debugUtils = false;
        for (auto& e : avail)
            if (strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) debugUtils = true;
        if (debugUtils) instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &ai;
        ici.enabledExtensionCount = (uint32_t)instExts.size();
        ici.ppEnabledExtensionNames = instExts.data();
        CHECK(vkCreateInstance(&ici, nullptr, &instance));

#if defined(_WIN32)
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleA(nullptr);
        sci.hwnd = hwnd;
        CHECK(vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface));
#else
        VkXcbSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
        sci.connection = conn;
        sci.window = window;
        CHECK(vkCreateXcbSurfaceKHR(instance, &sci, nullptr, &surface));
#endif

        // The first GPU with ray tracing pipelines and a queue that draws and presents.
        const char* const devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                       VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
        uint32_t gpuCount = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr));
        std::vector<VkPhysicalDevice> gpus(gpuCount);
        CHECK(vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data()));
        for (VkPhysicalDevice g : gpus) {
            uint32_t en = 0;
            vkEnumerateDeviceExtensionProperties(g, nullptr, &en, nullptr);
            std::vector<VkExtensionProperties> exts(en);
            vkEnumerateDeviceExtensionProperties(g, nullptr, &en, exts.data());
            bool all = true;
            for (const char* want : devExts) {
                bool found = false;
                for (auto& e : exts) found = found || strcmp(e.extensionName, want) == 0;
                all = all && found;
            }
            if (!all) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qf.data());
            for (uint32_t i = 0; i < qn; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(g, i, surface, &present);
                if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    gpu = g;
                    queueFamily = i;
                    break;
                }
            }
            if (gpu) break;
        }
        if (!gpu) {
            fprintf(stderr, "no GPU with VK_KHR_ray_tracing_pipeline and a presenting queue\n");
            exit(1);
        }
        vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu, &props);
        printf("device: %s\n", props.deviceName);

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddress{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        bufferAddress.bufferDeviceAddress = VK_TRUE;
        asFeatures.accelerationStructure = VK_TRUE;
        rtFeatures.rayTracingPipeline = VK_TRUE;
        asFeatures.pNext = &rtFeatures;
        bufferAddress.pNext = &asFeatures;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &bufferAddress;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = (uint32_t)(sizeof(devExts) / sizeof(devExts[0]));
        dci.ppEnabledExtensionNames = devExts;
        CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        auto fn = [&](const char* name) { return vkGetDeviceProcAddr(device, name); };
        rt.createAS = (PFN_vkCreateAccelerationStructureKHR)fn("vkCreateAccelerationStructureKHR");
        rt.destroyAS = (PFN_vkDestroyAccelerationStructureKHR)fn("vkDestroyAccelerationStructureKHR");
        rt.buildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)fn("vkGetAccelerationStructureBuildSizesKHR");
        rt.asAddress = (PFN_vkGetAccelerationStructureDeviceAddressKHR)fn("vkGetAccelerationStructureDeviceAddressKHR");
        rt.build = (PFN_vkCmdBuildAccelerationStructuresKHR)fn("vkCmdBuildAccelerationStructuresKHR");
        rt.createPipelines = (PFN_vkCreateRayTracingPipelinesKHR)fn("vkCreateRayTracingPipelinesKHR");
        rt.groupHandles = (PFN_vkGetRayTracingShaderGroupHandlesKHR)fn("vkGetRayTracingShaderGroupHandlesKHR");
        rt.trace = (PFN_vkCmdTraceRaysKHR)fn("vkCmdTraceRaysKHR");
        if (!rt.createAS || !rt.destroyAS || !rt.buildSizes || !rt.asAddress || !rt.build || !rt.createPipelines || !rt.groupHandles || !rt.trace) {
            fprintf(stderr, "the device has no ray tracing entry points\n");
            exit(1);
        }

        if (debugUtils) {
            beginLabel = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT");
            endLabel = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT");
            setName = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT");
        }

        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = queueFamily;
        CHECK(vkCreateCommandPool(device, &cpci, nullptr, &commandPool));
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = commandPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        CHECK(vkAllocateCommandBuffers(device, &cbai, &frameCommands));
        Name(VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)frameCommands, "Frame commands");

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CHECK(vkCreateFence(device, &fci, nullptr, &frameDone));
        VkSemaphoreCreateInfo sci2{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CHECK(vkCreateSemaphore(device, &sci2, nullptr, &imageAvailable));
    }

    // Returns false when the surface has no area (minimized).
    bool CreateSwapchain() {
        VkSurfaceCapabilitiesKHR caps;
        CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
        if (caps.currentExtent.width != 0xFFFFFFFF) {
            width = caps.currentExtent.width;
            height = caps.currentExtent.height;
        }
        if (width == 0 || height == 0) return false;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            fprintf(stderr, "the surface's images cannot be blitted to\n");
            exit(1);
        }

        // A UNORM format: the ray generation shader applies the book's gamma itself, and an sRGB
        // swapchain would encode it a second time.
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, formats.data());
        VkSurfaceFormatKHR chosen = formats[0];
        for (auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
                chosen = f;
                break;
            }
        if (chosen.format != VK_FORMAT_B8G8R8A8_UNORM && chosen.format != VK_FORMAT_R8G8B8A8_UNORM)
            fprintf(stderr, "no UNORM swapchain format; the image will be too bright\n");
        swapFormat = chosen.format;

        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface;
        sci.minImageCount = std::max(caps.minImageCount, (uint32_t)2);
        if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
        sci.imageFormat = chosen.format;
        sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = {width, height};
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = swapchain;
        VkSwapchainKHR created;
        CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &created));
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = created;

        vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
        swapImages.resize(n);
        vkGetSwapchainImagesKHR(device, swapchain, &n, swapImages.data());
        for (VkSemaphore s : renderFinished) vkDestroySemaphore(device, s, nullptr);
        renderFinished.assign(n, VK_NULL_HANDLE);
        for (auto& s : renderFinished) {
            VkSemaphoreCreateInfo sci2{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            CHECK(vkCreateSemaphore(device, &sci2, nullptr, &s));
        }
        return true;
    }

    // The output and accumulation images at the swapchain's size, bound to the descriptor set,
    // and the camera for its aspect ratio. The accumulation starts again.
    void CreateSizedResources() {
        output = CreateStorageImage(kOutputFormat, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "Path tracer output");
        accumulation = CreateStorageImage(kAccumulationFormat, 0, "Path tracer accumulation");
        VkCommandBuffer cb = BeginOneShot();
        for (VkImage image : {output.image, accumulation.image})
            Barrier(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
        EndOneShot(cb);

        VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, accumulation.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2]{};
        for (int i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = 1 + i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        rtiow::Camera camera = rtiow::MakeCamera(width, height);
        memcpy(cameraBuffer.mapped, &camera, sizeof(camera));
        accumulated = 0;
    }

    void DestroySizedResources() {
        DestroyStorageImage(output);
        DestroyStorageImage(accumulation);
    }

    // Returns false when there is nothing to draw into (minimized).
    bool Recreate() {
        resized = false;
        CHECK(vkDeviceWaitIdle(device));
        DestroySizedResources();
        if (!CreateSwapchain()) {
            resized = true;   // try again once the window has an area
            return false;
        }
        CreateSizedResources();
        return true;
    }

    VkAccelerationStructureGeometryKHR BlasGeometry(uint32_t m) {
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        g.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
        g.geometry.aabbs.data.deviceAddress = aabbs[m].address;
        g.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);
        return g;
    }

    VkAccelerationStructureGeometryKHR TlasGeometry() {
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        g.geometry.instances.data.deviceAddress = instances.address;
        return g;
    }

    // Sizes a structure, then creates it over a buffer of its own. Returns the build's scratch size.
    VkDeviceSize CreateStructure(VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR& geometry,
                                 uint32_t primitives, DeviceBuffer& storage, VkAccelerationStructureKHR& out, const char* name) {
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = type;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &geometry;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        rt.buildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);
        storage = CreateBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, name);
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = storage.buffer;
        ci.size = sizes.accelerationStructureSize;
        ci.type = type;
        CHECK(rt.createAS(device, &ci, nullptr, &out));
        Name(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, (uint64_t)out, name);
        return sizes.buildScratchSize;
    }

    void CreateScene() {
        scene = rtiow::MakeScene();
        printf("scene: %zu spheres (%u Lambertian, %u metal, %u dielectric)\n", scene.spheres.size(),
               scene.count[rtiow::kLambertian], scene.count[rtiow::kMetal], scene.count[rtiow::kDielectric]);
        spheres = CreateBufferWithData(scene.spheres.data(), scene.spheres.size() * sizeof(rtiow::Sphere),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "Spheres");
        cameraBuffer = CreateBuffer(sizeof(rtiow::Camera), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, "Camera");

        VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &asProps;
        vkGetPhysicalDeviceProperties2(gpu, &props2);
        const VkDeviceSize scratchAlign = asProps.minAccelerationStructureScratchOffsetAlignment;

        // One scratch buffer. The bottom levels are built in one call, so each has its own stretch
        // of it; the top level is built after them and starts at the beginning again.
        static_assert(sizeof(rtiow::Aabb) == sizeof(VkAabbPositionsKHR));
        VkDeviceSize blasScratch = 0;
        for (uint32_t m = 0; m < M; ++m) {
            std::vector<rtiow::Aabb> boxes = rtiow::Bounds(scene, (rtiow::Material)m);
            std::string name = std::string(rtiow::MaterialName(m)) + " AABBs";
            aabbs[m] = CreateBufferWithData(boxes.data(), boxes.size() * sizeof(rtiow::Aabb),
                                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, name.c_str());
            name = std::string(rtiow::MaterialName(m)) + " BLAS";
            VkDeviceSize size = CreateStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, BlasGeometry(m), scene.count[m],
                                                blasMemory[m], blas[m], name.c_str());
            blasScratchOffset[m] = blasScratch;
            blasScratch = AlignUp(blasScratch + size, scratchAlign);
        }

        // One instance per material: the identity transform, the material's first sphere as the
        // custom index and the material as the offset into the hit records.
        VkAccelerationStructureInstanceKHR records[M]{};
        for (uint32_t m = 0; m < M; ++m) {
            VkAccelerationStructureDeviceAddressInfoKHR addr{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
            addr.accelerationStructure = blas[m];
            VkAccelerationStructureInstanceKHR& r = records[m];
            r.transform.matrix[0][0] = r.transform.matrix[1][1] = r.transform.matrix[2][2] = 1.0f;
            r.instanceCustomIndex = scene.first[m];
            r.mask = 0xFF;
            r.instanceShaderBindingTableRecordOffset = m;
            r.accelerationStructureReference = rt.asAddress(device, &addr);
        }
        instances = CreateBufferWithData(records, sizeof(records), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                         "Scene instances");
        VkDeviceSize tlasScratch = CreateStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, TlasGeometry(), M, tlasMemory, tlas,
                                                   "Scene TLAS");
        scratch = CreateBuffer(std::max(blasScratch, tlasScratch), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, "Build scratch");

        if (!rebuild) {
            VkCommandBuffer cb = BeginOneShot();
            RecordBuilds(cb);
            EndOneShot(cb);
        }
    }

    // A memory barrier after acceleration structure builds, before anything that reads what they
    // built. The NVIDIA driver (RTX 4080, 2026-09) takes it as the point where the builds must
    // have finished, as a D3D12 UAV barrier is: without one, a top level built after its bottom
    // levels in the same command buffer traces as though they were empty, a trace after a top
    // level build sees it half built (or loses the device), and a top level that reuses the
    // bottom levels' scratch loses the device. The validation layer reports none of it.
    void BuildBarrier(VkCommandBuffer cb, VkPipelineStageFlags readers) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, readers, 0, 1, &mb, 0, nullptr, 0,
                             nullptr);
    }

    // The three bottom levels in one call, then the top level over them in a second.
    void RecordBuilds(VkCommandBuffer cb) {
        BeginLabel(cb, "Build acceleration structures");
        VkAccelerationStructureGeometryKHR geometries[M];
        VkAccelerationStructureBuildGeometryInfoKHR infos[M];
        VkAccelerationStructureBuildRangeInfoKHR ranges[M];
        const VkAccelerationStructureBuildRangeInfoKHR* rangePtrs[M];
        for (uint32_t m = 0; m < M; ++m) {
            geometries[m] = BlasGeometry(m);
            infos[m] = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            infos[m].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            infos[m].flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            infos[m].mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            infos[m].dstAccelerationStructure = blas[m];
            infos[m].geometryCount = 1;
            infos[m].pGeometries = &geometries[m];
            infos[m].scratchData.deviceAddress = scratch.address + blasScratchOffset[m];
            ranges[m] = {scene.count[m], 0, 0, 0};
            rangePtrs[m] = &ranges[m];
        }
        rt.build(cb, M, infos, rangePtrs);
        BuildBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);

        VkAccelerationStructureGeometryKHR geometry = TlasGeometry();
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.dstAccelerationStructure = tlas;
        info.geometryCount = 1;
        info.pGeometries = &geometry;
        info.scratchData.deviceAddress = scratch.address;
        VkAccelerationStructureBuildRangeInfoKHR range{M, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* rangePtr = &range;
        rt.build(cb, 1, &info, &rangePtr);
        BuildBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
        EndLabel(cb);
    }

    void CreatePipeline() {
        // 0 the scene, 1 the output, 2 the running mean, 3 the spheres, 4 the camera. Every
        // binding is visible to every stage, since common.glsl declares them all in each.
        VkDescriptorSetLayoutBinding bindings[5] = {
            {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, kRayStages, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kRayStages, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kRayStages, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRayStages, nullptr},
            {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kRayStages, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslci.bindingCount = 5;
        dslci.pBindings = bindings;
        CHECK(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &setLayout));
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 1;
        dpci.poolSizeCount = 4;
        dpci.pPoolSizes = sizes;
        CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &setLayout;
        CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
        Name(VK_OBJECT_TYPE_DESCRIPTOR_SET, (uint64_t)set, "Path tracer set");

        // The bindings that never change; the images are written by CreateSizedResources.
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &tlas;
        VkDescriptorBufferInfo sphereInfo{spheres.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo cameraInfo{cameraBuffer.buffer, 0, sizeof(rtiow::Camera)};
        VkWriteDescriptorSet writes[3]{};
        for (auto& w : writes) {
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set;
            w.descriptorCount = 1;
        }
        writes[0].pNext = &asWrite;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[1].dstBinding = 3;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &sphereInfo;
        writes[2].dstBinding = 4;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &cameraInfo;
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

        VkPushConstantRange push{kRayStages, 0, sizeof(rtiow::FrameParams)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &setLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &layout));

        struct StageFile {
            VkShaderStageFlagBits stage;
            const char* file;
        };
        const StageFile files[] = {
            {VK_SHADER_STAGE_RAYGEN_BIT_KHR, "path.rgen.spv"},
            {VK_SHADER_STAGE_MISS_BIT_KHR, "sky.rmiss.spv"},
            {VK_SHADER_STAGE_INTERSECTION_BIT_KHR, "sphere.rint.spv"},
            {VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "lambertian.rchit.spv"},   // kLambertian
            {VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "metal.rchit.spv"},        // kMetal
            {VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "dielectric.rchit.spv"},   // kDielectric
        };
        const uint32_t stageCount = sizeof(files) / sizeof(files[0]);
        const uint32_t kIntersectionStage = 2, kFirstHitStage = 3;
        std::vector<VkPipelineShaderStageCreateInfo> stages(stageCount);
        for (uint32_t i = 0; i < stageCount; ++i) {
            stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[i].stage = files[i].stage;
            stages[i].module = LoadShader(files[i].file);
            stages[i].pName = "main";
        }
        VkRayTracingShaderGroupCreateInfoKHR groups[kGroupCount]{};
        for (auto& g : groups) {
            g.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            g.generalShader = g.closestHitShader = g.anyHitShader = g.intersectionShader = VK_SHADER_UNUSED_KHR;
        }
        groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[0].generalShader = 0;
        groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[1].generalShader = 1;
        for (uint32_t m = 0; m < M; ++m) {
            groups[2 + m].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
            groups[2 + m].intersectionShader = kIntersectionStage;
            groups[2 + m].closestHitShader = kFirstHitStage + m;
        }
        VkRayTracingPipelineCreateInfoKHR rpci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        rpci.stageCount = stageCount;
        rpci.pStages = stages.data();
        rpci.groupCount = kGroupCount;
        rpci.pGroups = groups;
        rpci.maxPipelineRayRecursionDepth = 1;   // the ray generation shader walks the path itself
        rpci.layout = layout;
        CHECK(rt.createPipelines(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rpci, nullptr, &pipeline));
        Name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)pipeline, "Path tracer pipeline");
        for (auto& s : stages) vkDestroyShaderModule(device, s.module, nullptr);

        // The binding table: [raygen] [miss] [Lambertian, metal, dielectric hits], each region at
        // a multiple of the base alignment and the records in a region a stride apart.
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &props;
        vkGetPhysicalDeviceProperties2(gpu, &props2);
        const uint32_t handleSize = props.shaderGroupHandleSize;
        const VkDeviceSize stride = AlignUp(handleSize, props.shaderGroupHandleAlignment);
        const VkDeviceSize base = std::max<VkDeviceSize>(props.shaderGroupBaseAlignment, 1);
        const VkDeviceSize missOffset = AlignUp(stride, base);
        const VkDeviceSize hitOffset = AlignUp(missOffset + stride, base);
        const VkDeviceSize tableSize = hitOffset + M * stride;

        std::vector<uint8_t> handles(kGroupCount * handleSize);
        CHECK(rt.groupHandles(device, pipeline, 0, kGroupCount, handles.size(), handles.data()));
        sbt = CreateBuffer(tableSize, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, true, "Shader binding table");
        auto* table = static_cast<uint8_t*>(sbt.mapped);
        memset(table, 0, (size_t)tableSize);
        memcpy(table, &handles[0], handleSize);
        memcpy(table + missOffset, &handles[1 * handleSize], handleSize);
        for (uint32_t m = 0; m < M; ++m) memcpy(table + hitOffset + m * stride, &handles[(2 + m) * handleSize], handleSize);
        raygenRegion = {sbt.address, stride, stride};
        missRegion = {sbt.address + missOffset, stride, stride};
        hitRegion = {sbt.address + hitOffset, stride, M * stride};
    }

    // --------------------------------------------------------------------------------- frame
    void DrawFrame() {
        if (resized && !Recreate()) return;
        CHECK(vkWaitForFences(device, 1, &frameDone, VK_TRUE, UINT64_MAX));

        uint32_t imageIndex = 0;
        VkResult ar = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
            resized = true;
            return;
        }
        if (ar != VK_SUBOPTIMAL_KHR) CHECK(ar);
        CHECK(vkResetFences(device, 1, &frameDone));

        VkCommandBuffer cb = frameCommands;
        CHECK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cb, &bi));

        if (rebuild) RecordBuilds(cb);

        rtiow::FrameParams params{accumulate ? accumulated : (uint32_t)frameCount, samplesPerFrame, maxDepth, accumulate ? 1u : 0u};
        BeginLabel(cb, "Path trace");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, layout, kRayStages, 0, sizeof(params), &params);
        rt.trace(cb, &raygenRegion, &missRegion, &hitRegion, &callableRegion, width, height, 1);
        EndLabel(cb);

        BeginLabel(cb, "Present");
        VkImage target = swapImages[imageIndex];
        Barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t)width, (int32_t)height, 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[1] = blit.srcOffsets[1];
        vkCmdBlitImage(cb, output.image, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);
        Barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        EndLabel(cb);
        CHECK(vkEndCommandBuffer(cb));

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &imageAvailable;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderFinished[imageIndex];
        CHECK(vkQueueSubmit(queue, 1, &si, frameDone));

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderFinished[imageIndex];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR) resized = true;
        else CHECK(pr);

        ++frameCount;
        ++accumulated;
        if (frameCount % 100 == 0) printf("frame %llu: %u samples per pixel\n", (unsigned long long)frameCount,
                                          (accumulate ? accumulated : 1) * samplesPerFrame);
    }

    void Cleanup() {
        vkDeviceWaitIdle(device);
        DestroySizedResources();
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        DestroyBuffer(sbt);
        rt.destroyAS(device, tlas, nullptr);
        for (uint32_t m = 0; m < M; ++m) {
            rt.destroyAS(device, blas[m], nullptr);
            DestroyBuffer(blasMemory[m]);
            DestroyBuffer(aabbs[m]);
        }
        for (DeviceBuffer* b : {&tlasMemory, &instances, &scratch, &spheres, &cameraBuffer}) DestroyBuffer(*b);
        for (VkSemaphore s : renderFinished) vkDestroySemaphore(device, s, nullptr);
        vkDestroySemaphore(device, imageAvailable, nullptr);
        vkDestroyFence(device, frameDone, nullptr);
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

    int Run() {
        CreateWindowNative();
        InitVulkan();
        CreateScene();
        CreatePipeline();
        if (CreateSwapchain()) CreateSizedResources();
        else resized = true;
        while (!quit && (maxFrames < 0 || frameCount < (uint64_t)maxFrames)) {
            PumpEvents();
            if (quit) break;
            DrawFrame();
        }
        Cleanup();
        return 0;
    }
};

int RunApp(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // a lost device exits before a buffer would be flushed
    App app;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) app.maxFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) app.width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) app.height = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spp") && i + 1 < argc) app.samplesPerFrame = std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) app.maxDepth = std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-accumulate")) app.accumulate = false;
        else if (!strcmp(argv[i], "--rebuild")) app.rebuild = true;
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 1;
        }
    }
    return app.Run();
}

} // namespace

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return RunApp(__argc, __argv);
}
#else
int main(int argc, char** argv) {
    return RunApp(argc, argv);
}
#endif
