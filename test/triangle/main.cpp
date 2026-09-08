// Minimal Vulkan test application: a rotating textured cube with a depth buffer, re-recording
// its command buffer every frame. Exercises what the inspector needs: buffers, images, samplers,
// descriptor sets, push constants, debug labels, per-frame command recording and presentation.
//
// Usage: vkinsp_triangle [--frames N] [--width W] [--height H]
//
// The window is resizable: the swapchain, depth buffer and framebuffers are recreated when the
// window size changes (or when acquire/present report the swapchain out of date), which also
// exercises the inspector's handling of object destruction and swapchain replacement.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <xcb/xcb.h>
#endif

#define CHECK(x)                                                                     \
    do {                                                                             \
        VkResult r_ = (x);                                                           \
        if (r_ != VK_SUCCESS) {                                                      \
            fprintf(stderr, "%s failed: %d (%s:%d)\n", #x, (int)r_, __FILE__, __LINE__); \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

namespace {

struct Vertex {
    float pos[3];
    float color[3];
    float uv[2];
};

struct Mat4 {
    float m[16];
};

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Mat4 Perspective(float fovy, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy / 2);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = -f;  // Vulkan clip space: y down
    r.m[10] = zf / (zn - zf);
    r.m[11] = -1;
    r.m[14] = (zn * zf) / (zn - zf);
    return r;
}

Mat4 Translate(float x, float y, float z) {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

Mat4 RotateY(float a) {
    Mat4 r{};
    r.m[0] = cosf(a); r.m[2] = -sinf(a); r.m[5] = 1; r.m[8] = sinf(a); r.m[10] = cosf(a); r.m[15] = 1;
    return r;
}

Mat4 RotateX(float a) {
    Mat4 r{};
    r.m[0] = 1; r.m[5] = cosf(a); r.m[6] = sinf(a); r.m[9] = -sinf(a); r.m[10] = cosf(a); r.m[15] = 1;
    return r;
}

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

struct App {
    uint32_t width = 640, height = 480;
    int maxFrames = -1;
    bool badScissor = false;
    bool leak = false;
    bool resized = false;   // swapchain must be recreated before the next frame

#if defined(_WIN32)
    HWND hwnd = nullptr;
    bool quit = false;
#else
    xcb_connection_t* conn = nullptr;
    xcb_window_t window = 0;
    bool quit = false;
#endif

    VkInstance instance{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice gpu{};
    uint32_t queueFamily = 0;
    VkDevice device{};
    VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memProps{};
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel = nullptr;
    PFN_vkSetDebugUtilsObjectNameEXT setName = nullptr;

    VkSwapchainKHR swapchain{};
    VkFormat colorFormat = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::vector<VkFramebuffer> framebuffers;
    VkImage depthImage{};
    VkDeviceMemory depthMemory{};
    VkImageView depthView{};
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkRenderPass renderPass{};

    VkBuffer vertexBuffer{}, indexBuffer{}, uniformBuffer{};
    VkDeviceMemory vertexMemory{}, indexMemory{}, uniformMemory{};
    void* uniformMapped = nullptr;
    VkImage texture{};
    VkDeviceMemory textureMemory{};
    VkImageView textureView{};
    VkSampler sampler{};

    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};
    VkDescriptorPool descriptorPool{};
    VkDescriptorSet descriptorSet{};

    // Compute: a wave buffer refreshed every frame (wave.comp), so captures have dispatches.
    static const uint32_t kWaveCount = 1024;
    VkBuffer waveBuffer{};
    VkDeviceMemory waveMemory{};
    VkDescriptorSetLayout computeSetLayout{};
    VkPipelineLayout computePipelineLayout{};
    VkPipeline computePipeline{};
    VkDescriptorSet computeSet{};

    VkCommandPool commandPool{};
    static const int kFramesInFlight = 2;
    VkCommandBuffer commandBuffers[kFramesInFlight]{};
    VkSemaphore imageAvailable[kFramesInFlight]{};
    VkSemaphore renderFinished[kFramesInFlight]{};
    VkFence inFlight[kFramesInFlight]{};
    int frameSlot = 0;
    uint64_t frameCount = 0;

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
        wc.lpszClassName = "vkinsp_triangle";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&wc);
        RECT r{0, 0, (LONG)width, (LONG)height};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowA(wc.lpszClassName, "GPU Inspector test: cube", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                             wc.hInstance, nullptr);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)this);
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

    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buf,
                      VkDeviceMemory& mem, const char* name) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CHECK(vkCreateBuffer(device, &bci, nullptr, &buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buf, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
        CHECK(vkAllocateMemory(device, &mai, nullptr, &mem));
        CHECK(vkBindBufferMemory(device, buf, mem, 0));
        Name(VK_OBJECT_TYPE_BUFFER, (uint64_t)buf, name);
    }

    void Name(VkObjectType type, uint64_t handle, const char* name) {
        if (!setName) return;
        VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        ni.objectType = type;
        ni.objectHandle = handle;
        ni.pObjectName = name;
        setName(device, &ni);
    }

    VkShaderModule LoadShader(const char* file) {
        std::vector<char> code = ReadFile(ExeDir() + file);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m;
        CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
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

    // --------------------------------------------------------------------------------- setup
    void InitVulkan() {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "vkinsp_triangle";
        ai.pEngineName = "none";
        ai.apiVersion = VK_API_VERSION_1_1;
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

        uint32_t gpuCount = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr));
        std::vector<VkPhysicalDevice> gpus(gpuCount);
        CHECK(vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data()));
        for (VkPhysicalDevice g : gpus) {
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
            fprintf(stderr, "no suitable GPU\n");
            exit(1);
        }
        vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = devExts;
        CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

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
        cbai.commandBufferCount = kFramesInFlight;
        CHECK(vkAllocateCommandBuffers(device, &cbai, commandBuffers));
        for (int i = 0; i < kFramesInFlight; ++i) {
            VkSemaphoreCreateInfo sci2{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            CHECK(vkCreateSemaphore(device, &sci2, nullptr, &imageAvailable[i]));
            CHECK(vkCreateSemaphore(device, &sci2, nullptr, &renderFinished[i]));
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            CHECK(vkCreateFence(device, &fci, nullptr, &inFlight[i]));
        }
    }

    // Creates the swapchain, its image views and the depth buffer for the current window size.
    // Pass the previous swapchain when recreating after a resize; it is destroyed here.
    void CreateSwapchain(VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE) {
        VkSurfaceCapabilitiesKHR caps;
        CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
        uint32_t fn = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fn);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fn, formats.data());
        VkSurfaceFormatKHR chosen = formats[0];
        for (auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }
        colorFormat = chosen.format;
        if (caps.currentExtent.width != 0xFFFFFFFF) {
            width = caps.currentExtent.width;
            height = caps.currentExtent.height;
        } else {
            width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, width));
            height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, height));
        }

        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface;
        sci.minImageCount = caps.minImageCount + 1 > caps.maxImageCount && caps.maxImageCount ? caps.maxImageCount : caps.minImageCount + 1;
        sci.imageFormat = chosen.format;
        sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = {width, height};
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = oldSwapchain;
        CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain));
        if (oldSwapchain) vkDestroySwapchainKHR(device, oldSwapchain, nullptr);

        uint32_t count = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
        swapImages.resize(count);
        vkGetSwapchainImagesKHR(device, swapchain, &count, swapImages.data());
        swapViews.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = swapImages[i];
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = colorFormat;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            CHECK(vkCreateImageView(device, &vci, nullptr, &swapViews[i]));
            Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)swapImages[i], "Swapchain image");
        }

        // Depth buffer
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = depthFormat;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        CHECK(vkCreateImage(device, &ici, nullptr, &depthImage));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, depthImage, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(device, &mai, nullptr, &depthMemory));
        CHECK(vkBindImageMemory(device, depthImage, depthMemory, 0));
        VkImageViewCreateInfo dvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        dvci.image = depthImage;
        dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        dvci.format = depthFormat;
        dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        CHECK(vkCreateImageView(device, &dvci, nullptr, &depthView));
        Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)depthImage, "Depth buffer");
    }

    void CreateRenderPass() {
        VkAttachmentDescription atts[2]{};
        atts[0].format = colorFormat;
        atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        atts[1].format = depthFormat;
        atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &colorRef;
        sp.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstStageMask = dep.srcStageMask;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 2;
        rpci.pAttachments = atts;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sp;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dep;
        CHECK(vkCreateRenderPass(device, &rpci, nullptr, &renderPass));
    }

    void CreateFramebuffers() {
        const uint32_t count = (uint32_t)swapViews.size();
        framebuffers.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            VkImageView views[] = {swapViews[i], depthView};
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = renderPass;
            fci.attachmentCount = 2;
            fci.pAttachments = views;
            fci.width = width;
            fci.height = height;
            fci.layers = 1;
            CHECK(vkCreateFramebuffer(device, &fci, nullptr, &framebuffers[i]));
        }
    }

    // Everything sized by the window, except the swapchain itself (see CreateSwapchain).
    void DestroySwapchainResources() {
        for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        framebuffers.clear();
        vkDestroyImageView(device, depthView, nullptr);
        vkDestroyImage(device, depthImage, nullptr);
        vkFreeMemory(device, depthMemory, nullptr);
        depthView = VK_NULL_HANDLE;
        depthImage = VK_NULL_HANDLE;
        depthMemory = VK_NULL_HANDLE;
        for (auto v : swapViews) vkDestroyImageView(device, v, nullptr);
        swapViews.clear();
        swapImages.clear();
    }

    // Returns false while the window has no drawable area (minimized); try again later.
    bool RecreateSwapchain() {
        VkSurfaceCapabilitiesKHR caps;
        CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
        if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;
        if (caps.currentExtent.width == 0xFFFFFFFF && (width == 0 || height == 0)) return false;
        vkDeviceWaitIdle(device);
        DestroySwapchainResources();
        CreateSwapchain(swapchain);
        CreateFramebuffers();
        resized = false;
        return true;
    }

    void CreateResources() {
        // Cube geometry
        const float p = 0.5f;
        Vertex verts[24];
        uint16_t indices[36];
        const float faces[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const float colors[6][3] = {{1, 0.3f, 0.3f}, {0.3f, 1, 0.3f}, {0.3f, 0.3f, 1}, {1, 1, 0.3f}, {1, 0.3f, 1}, {0.3f, 1, 1}};
        int v = 0, ix = 0;
        for (int f = 0; f < 6; ++f) {
            const float* n = faces[f];
            float u[3] = {n[1], n[2], n[0]};
            float w[3] = {n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
            for (int c = 0; c < 4; ++c) {
                float su = (c == 1 || c == 2) ? 1.f : -1.f;
                float sv = (c >= 2) ? 1.f : -1.f;
                for (int k = 0; k < 3; ++k) verts[v].pos[k] = p * (n[k] + su * u[k] + sv * w[k]);
                memcpy(verts[v].color, colors[f], sizeof(verts[v].color));
                verts[v].uv[0] = su * 0.5f + 0.5f;
                verts[v].uv[1] = sv * 0.5f + 0.5f;
                ++v;
            }
            uint16_t b = (uint16_t)(f * 4);
            uint16_t quad[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
            for (int k = 0; k < 6; ++k) indices[ix++] = quad[k];
        }
        VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        CreateBuffer(sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host, vertexBuffer, vertexMemory, "Cube vertices");
        CreateBuffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host, indexBuffer, indexMemory, "Cube indices");
        CreateBuffer(sizeof(Mat4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, uniformBuffer, uniformMemory, "Cube uniforms");
        void* map;
        CHECK(vkMapMemory(device, vertexMemory, 0, VK_WHOLE_SIZE, 0, &map));
        memcpy(map, verts, sizeof(verts));
        vkUnmapMemory(device, vertexMemory);
        CHECK(vkMapMemory(device, indexMemory, 0, VK_WHOLE_SIZE, 0, &map));
        memcpy(map, indices, sizeof(indices));
        vkUnmapMemory(device, indexMemory);
        CHECK(vkMapMemory(device, uniformMemory, 0, VK_WHOLE_SIZE, 0, &uniformMapped));

        // Checker texture (8x8 RGBA8) uploaded through a staging buffer.
        const uint32_t ts = 8;
        uint8_t pixels[ts * ts * 4];
        for (uint32_t y = 0; y < ts; ++y)
            for (uint32_t x = 0; x < ts; ++x) {
                uint8_t c = ((x + y) & 1) ? 255 : 90;
                uint8_t* px = &pixels[(y * ts + x) * 4];
                px[0] = px[1] = px[2] = c;
                px[3] = 255;
            }
        VkBuffer staging;
        VkDeviceMemory stagingMem;
        CreateBuffer(sizeof(pixels), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, staging, stagingMem, "Texture staging");
        CHECK(vkMapMemory(device, stagingMem, 0, VK_WHOLE_SIZE, 0, &map));
        memcpy(map, pixels, sizeof(pixels));
        vkUnmapMemory(device, stagingMem);

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {ts, ts, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        CHECK(vkCreateImage(device, &ici, nullptr, &texture));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, texture, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(device, &mai, nullptr, &textureMemory));
        CHECK(vkBindImageMemory(device, texture, textureMemory, 0));
        Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)texture, "Checker texture");

        VkCommandBuffer upload = BeginOneShot();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = texture;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {ts, ts, 1};
        vkCmdCopyBufferToImage(upload, staging, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        EndOneShot(upload);
        vkDestroyBuffer(device, staging, nullptr);
        vkFreeMemory(device, stagingMem, nullptr);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = texture;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CHECK(vkCreateImageView(device, &vci, nullptr, &textureView));
        VkSamplerCreateInfo smci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        smci.magFilter = VK_FILTER_NEAREST;
        smci.minFilter = VK_FILTER_NEAREST;
        smci.addressModeU = smci.addressModeV = smci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        smci.maxLod = 1.0f;
        CHECK(vkCreateSampler(device, &smci, nullptr, &sampler));

        // Descriptors
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslci.bindingCount = 2;
        dslci.pBindings = bindings;
        CHECK(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &setLayout));
        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &setLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout));

        VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 2;
        dpci.poolSizeCount = 3;
        dpci.pPoolSizes = sizes;
        CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &descriptorPool));
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = descriptorPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &setLayout;
        CHECK(vkAllocateDescriptorSets(device, &dsai, &descriptorSet));
        VkDescriptorBufferInfo dbi{uniformBuffer, 0, sizeof(Mat4)};
        VkDescriptorImageInfo dii{sampler, textureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &dbi;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &dii;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        // Pipeline
        VkShaderModule vs = LoadShader("cube.vert.spv");
        VkShaderModule fs = LoadShader("cube.frag.spv");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        VkVertexInputBindingDescription vib{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription via[3] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vib;
        vi.vertexAttributeDescriptionCount = 3;
        vi.pVertexAttributeDescriptions = via;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &cba;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsci{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dsci.dynamicStateCount = 2;
        dsci.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms;
        gpci.pDepthStencilState = &ds;
        gpci.pColorBlendState = &blend;
        gpci.pDynamicState = &dsci;
        gpci.layout = pipelineLayout;
        gpci.renderPass = renderPass;
        CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline));
        Name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)pipeline, "Cube pipeline");
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
    }

    // --------------------------------------------------------------------------------- frame
    // Returns false when nothing was drawn (window minimized or swapchain being replaced).
    bool DrawFrame(float t) {
        if (resized && !RecreateSwapchain()) return false;
        VkFence fence = inFlight[frameSlot];
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        uint32_t imageIndex;
        VkResult ar = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable[frameSlot], VK_NULL_HANDLE, &imageIndex);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
            resized = true;
            return false;
        }
        if (ar != VK_SUBOPTIMAL_KHR) CHECK(ar);
        CHECK(vkResetFences(device, 1, &fence));

        Mat4 proj = Perspective(1.0f, (float)width / (float)height, 0.1f, 10.0f);
        Mat4 view = Translate(0, 0, -2.5f);
        Mat4 model = Mul(RotateX(t * 0.7f), RotateY(t));
        Mat4 mvp = Mul(proj, Mul(view, model));
        memcpy(uniformMapped, &mvp, sizeof(mvp));

        VkCommandBuffer cb = commandBuffers[frameSlot];
        CHECK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(vkBeginCommandBuffer(cb, &bi));

        // Compute first: two dispatches refreshing the wave buffer, then a barrier. The inspector
        // times the run as one compute pass.
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout, 0, 1, &computeSet, 0, nullptr);
        struct { float time; uint32_t count; } wavePush{t, kWaveCount};
        vkCmdPushConstants(cb, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(wavePush), &wavePush);
        vkCmdDispatch(cb, kWaveCount / 64, 1, 1);
        vkCmdDispatch(cb, kWaveCount / 64, 1, 1);
        VkMemoryBarrier waveBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        waveBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        waveBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &waveBarrier, 0, nullptr, 0, nullptr);

        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = "Main Pass";
        label.color[0] = 0.2f; label.color[1] = 0.6f; label.color[2] = 1.0f; label.color[3] = 1.0f;
        if (beginLabel) beginLabel(cb, &label);

        VkClearValue clears[2]{};
        clears[0].color = {{0.1f, 0.1f, 0.15f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = renderPass;
        rpbi.framebuffer = framebuffers[imageIndex];
        rpbi.renderArea = {{0, 0}, {width, height}};
        rpbi.clearValueCount = 2;
        rpbi.pClearValues = clears;
        vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport viewport{0, 0, (float)width, (float)height, 0, 1};
        // --bad-scissor: a negative offset is a validation error (VUID-vkCmdSetScissor-x-00595),
        // used to exercise the inspector's validation message reporting.
        VkRect2D scissor{{badScissor ? -1 : 0, 0}, {width, height}};
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cb, indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        float tint = 0.5f + 0.5f * sinf(t);
        vkCmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &tint);
        vkCmdDrawIndexed(cb, 36, 1, 0, 0, 0);
        vkCmdEndRenderPass(cb);
        if (endLabel) endLabel(cb);
        CHECK(vkEndCommandBuffer(cb));

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &imageAvailable[frameSlot];
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderFinished[frameSlot];
        CHECK(vkQueueSubmit(queue, 1, &si, fence));

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderFinished[frameSlot];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR) resized = true;
        else CHECK(pr);
        frameSlot = (frameSlot + 1) % kFramesInFlight;
        frameCount++;
        return true;
    }

    void CreateCompute() {
        VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        CreateBuffer(kWaveCount * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, waveBuffer, waveMemory, "Wave");

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslci.bindingCount = 1;
        dslci.pBindings = &binding;
        CHECK(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &computeSetLayout));
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(uint32_t)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &computeSetLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &computePipelineLayout));

        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = descriptorPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &computeSetLayout;
        CHECK(vkAllocateDescriptorSets(device, &dsai, &computeSet));
        VkDescriptorBufferInfo dbi{waveBuffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = computeSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

        VkShaderModule cs = LoadShader("wave.comp.spv");
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = cs;
        cpci.stage.pName = "main";
        cpci.layout = computePipelineLayout;
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &computePipeline));
        Name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)computePipeline, "Wave compute");
        vkDestroyShaderModule(device, cs, nullptr);
    }

    void Cleanup() {
        vkDeviceWaitIdle(device);
        // --leak: leave the sampler and the wave buffer alive so the inspector's leak report has
        // something to report at vkDestroyDevice.
        if (leak) {
            sampler = VK_NULL_HANDLE;
            waveBuffer = VK_NULL_HANDLE;
        }
        vkDestroyPipeline(device, computePipeline, nullptr);
        vkDestroyPipelineLayout(device, computePipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, computeSetLayout, nullptr);
        vkDestroyBuffer(device, waveBuffer, nullptr);
        vkFreeMemory(device, waveMemory, nullptr);
        for (int i = 0; i < kFramesInFlight; ++i) {
            vkDestroySemaphore(device, imageAvailable[i], nullptr);
            vkDestroySemaphore(device, renderFinished[i], nullptr);
            vkDestroyFence(device, inFlight[i], nullptr);
        }
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        vkDestroySampler(device, sampler, nullptr);
        vkDestroyImageView(device, textureView, nullptr);
        vkDestroyImage(device, texture, nullptr);
        vkFreeMemory(device, textureMemory, nullptr);
        vkDestroyBuffer(device, vertexBuffer, nullptr);
        vkFreeMemory(device, vertexMemory, nullptr);
        vkDestroyBuffer(device, indexBuffer, nullptr);
        vkFreeMemory(device, indexMemory, nullptr);
        vkDestroyBuffer(device, uniformBuffer, nullptr);
        vkFreeMemory(device, uniformMemory, nullptr);
        DestroySwapchainResources();
        vkDestroyRenderPass(device, renderPass, nullptr);
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

    int Run() {
        CreateWindowNative();
        InitVulkan();
        CreateSwapchain();
        CreateRenderPass();
        CreateFramebuffers();
        CreateResources();
        CreateCompute();
        auto start = std::chrono::steady_clock::now();
        while (!quit && (maxFrames < 0 || (int)frameCount < maxFrames)) {
            PumpEvents();
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
            if (!DrawFrame(t)) std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        Cleanup();
        return 0;
    }
};

} // namespace

int RunApp(int argc, char** argv) {
    App app;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) app.maxFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) app.width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) app.height = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bad-scissor")) app.badScissor = true;
        else if (!strcmp(argv[i], "--leak")) app.leak = true;
    }
    return app.Run();
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return RunApp(__argc, __argv);
}
#else
int main(int argc, char** argv) {
    return RunApp(argc, argv);
}
#endif
