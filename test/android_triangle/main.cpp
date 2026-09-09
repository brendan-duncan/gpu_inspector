// The phone test application: a NativeActivity that renders a ring of triangles into a
// swapchain with Vulkan, the Android counterpart of test/triangle (which needs a window system
// of its own) and the headset-less sibling of test/xr_triangle. Built debuggable by
// tools/build_android_triangle.py so the inspector's layer can be loaded into it; the objects
// carry debug names, and the frame ends at vkQueuePresentKHR like on the desktop.
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "shaders.inl"   // generated: kVertSpv / kFragSpv (uint32_t arrays)

#define TAG "android_triangle"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOGE("%s failed: %d", #x, (int)r_); abort(); } } while (0)

namespace {

constexpr uint32_t kTriangles = 48;   // the ring (kTriangles in tri.vert)
constexpr int kFramesInFlight = 2;

// ------------------------------------------------------------------------------------ math

struct Mat4 { float m[16]; };   // column-major

Mat4 Identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

// A symmetric perspective for Vulkan clip space (depth 0..1, y down).
Mat4 Perspective(float fovY, float aspect, float nearZ, float farZ) {
    const float f = 1.0f / tanf(fovY * 0.5f);
    Mat4 p{};
    p.m[0] = f / aspect;
    p.m[5] = -f;
    p.m[10] = farZ / (nearZ - farZ);
    p.m[11] = -1.0f;
    p.m[14] = (farZ * nearZ) / (nearZ - farZ);
    return p;
}

Mat4 RotationY(float a) {
    Mat4 r = Identity();
    r.m[0] = cosf(a); r.m[8] = sinf(a);
    r.m[2] = -sinf(a); r.m[10] = cosf(a);
    return r;
}

Mat4 RotationZ(float a) {
    Mat4 r = Identity();
    r.m[0] = cosf(a); r.m[4] = -sinf(a);
    r.m[1] = sinf(a); r.m[5] = cosf(a);
    return r;
}

// ------------------------------------------------------------------------------------ app

struct App {
    android_app* android = nullptr;
    bool resumed = false;
    ANativeWindow* window = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers[kFramesInFlight]{};
    VkSemaphore imageAvailable[kFramesInFlight]{};
    VkSemaphore renderFinished[kFramesInFlight]{};
    VkFence inFlight[kFramesInFlight]{};
    int frameSlot = 0;
    uint64_t frameCount = 0;
    bool recreate = false;
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel = nullptr;
    PFN_vkSetDebugUtilsObjectNameEXT setName = nullptr;

    // -------------------------------------------------------------------------- instance / device

    void CreateVulkan() {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "android_triangle";
        ai.pEngineName = "none";
        ai.apiVersion = VK_API_VERSION_1_1;
        std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        for (auto& e : available) if (!strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &ai;
        ici.enabledExtensionCount = (uint32_t)extensions.size();
        ici.ppEnabledExtensionNames = extensions.data();
        VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
        beginLabel = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT");
        endLabel = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT");
        setName = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT");

        uint32_t gpuCount = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr));
        std::vector<VkPhysicalDevice> gpus(gpuCount);
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data()));
        if (gpus.empty()) { LOGE("no Vulkan device"); abort(); }
        gpu = gpus[0];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu, &props);
        LOGI("GPU: %s", props.deviceName);

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families.data());
        for (uint32_t i = 0; i < familyCount; ++i)
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { queueFamily = i; break; }

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = deviceExtensions;
        dci.pEnabledFeatures = &features;
        VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily;
        VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &commandPool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = commandPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = kFramesInFlight;
        VK_CHECK(vkAllocateCommandBuffers(device, &cai, commandBuffers));
        for (int i = 0; i < kFramesInFlight; ++i) {
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &imageAvailable[i]));
            VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &renderFinished[i]));
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VK_CHECK(vkCreateFence(device, &fci, nullptr, &inFlight[i]));
        }
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout));
    }

    void Name(VkObjectType type, uint64_t handle, const char* name) {
        if (!setName) return;
        VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        ni.objectType = type;
        ni.objectHandle = handle;
        ni.pObjectName = name;
        setName(device, &ni);
    }

    int32_t TryFindMemoryType(uint32_t bits, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return (int32_t)i;
        return -1;
    }

    VkShaderModule Module(const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
        return m;
    }

    // -------------------------------------------------------------------------- swapchain

    void CreateSwapchain() {
        VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        sci.window = window;
        VK_CHECK(vkCreateAndroidSurfaceKHR(instance, &sci, nullptr, &surface));
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, queueFamily, surface, &supported);
        if (!supported) { LOGE("the graphics queue cannot present"); abort(); }

        VkSurfaceCapabilitiesKHR caps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, formats.data());
        VkSurfaceFormatKHR chosen = formats[0];
        for (auto& f : formats) if (f.format == VK_FORMAT_R8G8B8A8_SRGB || f.format == VK_FORMAT_B8G8R8A8_SRGB) { chosen = f; break; }
        format = chosen.format;
        extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFF) extent = {(uint32_t)ANativeWindow_getWidth(window), (uint32_t)ANativeWindow_getHeight(window)};
        // Rendering in the display's native orientation avoids a rotation pass in the compositor:
        // the swapchain takes the surface's transform and the projection turns to match (see Mvp).
        transform = caps.currentTransform;
        if (transform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) std::swap(extent.width, extent.height);
        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

        VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swci.surface = surface;
        swci.minImageCount = imageCount;
        swci.imageFormat = format;
        swci.imageColorSpace = chosen.colorSpace;
        swci.imageExtent = extent;
        swci.imageArrayLayers = 1;
        swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swci.preTransform = transform;
        swci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swci.clipped = VK_TRUE;
        VK_CHECK(vkCreateSwapchainKHR(device, &swci, nullptr, &swapchain));
        Name(VK_OBJECT_TYPE_SWAPCHAIN_KHR, (uint64_t)swapchain, "Window swapchain");
        uint32_t got = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &got, nullptr);
        images.resize(got);
        vkGetSwapchainImagesKHR(device, swapchain, &got, images.data());
        LOGI("swapchain: %u images, %ux%u, format %d, transform %d", got, extent.width, extent.height, (int)format, (int)transform);

        if (!renderPass) CreateRenderPass();
        if (!pipeline) CreatePipeline();
        CreateDepth();
        views.resize(got);
        framebuffers.resize(got);
        for (uint32_t i = 0; i < got; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "Swapchain image %u", i);
            Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)images[i], name);
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = images[i];
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(device, &vci, nullptr, &views[i]));
            VkImageView attachments[] = {views[i], depthView};
            VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbci.renderPass = renderPass;
            fbci.attachmentCount = 2;
            fbci.pAttachments = attachments;
            fbci.width = extent.width;
            fbci.height = extent.height;
            fbci.layers = 1;
            VK_CHECK(vkCreateFramebuffer(device, &fbci, nullptr, &framebuffers[i]));
        }
    }

    void DestroySwapchain() {
        if (device) vkDeviceWaitIdle(device);
        for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        for (auto v : views) vkDestroyImageView(device, v, nullptr);
        framebuffers.clear();
        views.clear();
        images.clear();
        if (depthView) vkDestroyImageView(device, depthView, nullptr);
        if (depthImage) vkDestroyImage(device, depthImage, nullptr);
        if (depthMemory) vkFreeMemory(device, depthMemory, nullptr);
        depthView = VK_NULL_HANDLE; depthImage = VK_NULL_HANDLE; depthMemory = VK_NULL_HANDLE;
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        swapchain = VK_NULL_HANDLE;
        surface = VK_NULL_HANDLE;
    }

    void CreateRenderPass() {
        VkAttachmentDescription atts[2]{};
        atts[0].format = format;
        atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        atts[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
        atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;   // nothing reads the depth buffer after the pass
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
        VK_CHECK(vkCreateRenderPass(device, &rpci, nullptr, &renderPass));
        Name(VK_OBJECT_TYPE_RENDER_PASS, (uint64_t)renderPass, "Main pass");
    }

    void CreateDepth() {
        // Neither loaded nor stored: a transient attachment in lazily allocated memory when the
        // device has such memory, so a tiled GPU keeps it in tile memory only.
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_D24_UNORM_S8_UINT;
        ici.extent = {extent.width, extent.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device, &ici, nullptr, &depthImage));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, depthImage, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        int32_t lazy = TryFindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
        int32_t local = TryFindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mai.memoryTypeIndex = (uint32_t)(lazy >= 0 ? lazy : local >= 0 ? local : 0);
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &depthMemory));
        VK_CHECK(vkBindImageMemory(device, depthImage, depthMemory, 0));
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = depthImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_D24_UNORM_S8_UINT;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &vci, nullptr, &depthView));
        Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)depthImage, "Depth buffer");
    }

    void CreatePipeline() {
        VkShaderModule vs = Module(kVertSpv, sizeof(kVertSpv));
        VkShaderModule fs = Module(kFragSpv, sizeof(kFragSpv));
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamics;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
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
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pDynamicState = &dyn;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &blend;
        pci.layout = pipelineLayout;
        pci.renderPass = renderPass;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline));
        Name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)pipeline, "Ring pipeline");
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
    }

    // -------------------------------------------------------------------------- frame

    Mat4 Mvp(float seconds) {
        // The ring spins in its own plane and nods, two meters ahead.
        Mat4 model = Multiply(RotationY(0.6f * sinf(seconds * 0.4f)), RotationZ(seconds * 0.5f));
        model.m[14] = -2.5f;
        // The displayed orientation: undo the surface's rotation in clip space.
        float turn = 0.0f;
        if (transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) turn = -(float)M_PI_2;
        else if (transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) turn = (float)M_PI;
        else if (transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) turn = (float)M_PI_2;
        const bool sideways = transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR || transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
        const float aspect = sideways ? (float)extent.height / (float)extent.width : (float)extent.width / (float)extent.height;
        return Multiply(Multiply(RotationZ(turn), Perspective(1.1f, aspect, 0.1f, 100.0f)), model);
    }

    void RenderFrame(float seconds) {
        VkFence fence = inFlight[frameSlot];
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        uint32_t imageIndex = 0;
        VkResult ar = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable[frameSlot], VK_NULL_HANDLE, &imageIndex);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) { recreate = true; return; }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) VK_CHECK(ar);
        VK_CHECK(vkResetFences(device, 1, &fence));

        VkCommandBuffer cb = commandBuffers[frameSlot];
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));
        if (beginLabel) {
            VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
            label.pLabelName = "Scene";
            label.color[0] = 0.2f; label.color[1] = 0.8f; label.color[2] = 0.4f; label.color[3] = 1.0f;
            beginLabel(cb, &label);
        }
        VkClearValue clears[2]{};
        clears[0].color = {{0.05f, 0.05f, 0.12f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = renderPass;
        rpbi.framebuffer = framebuffers[imageIndex];
        rpbi.renderArea = {{0, 0}, extent};
        rpbi.clearValueCount = 2;
        rpbi.pClearValues = clears;
        vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport viewport{0, 0, (float)extent.width, (float)extent.height, 0, 1};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        Mat4 mvp = Mvp(seconds);
        vkCmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), &mvp);
        vkCmdDraw(cb, 3, kTriangles, 0, 0);   // the whole ring, instanced
        vkCmdEndRenderPass(cb);
        if (endLabel) endLabel(cb);
        VK_CHECK(vkEndCommandBuffer(cb));

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &imageAvailable[frameSlot];
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderFinished[frameSlot];
        VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderFinished[frameSlot];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) recreate = true;
        else if (pr != VK_SUCCESS) VK_CHECK(pr);
        frameSlot = (frameSlot + 1) % kFramesInFlight;
        frameCount++;
    }

    void Shutdown() {
        if (device) vkDeviceWaitIdle(device);
        DestroySwapchain();
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
        for (int i = 0; i < kFramesInFlight; ++i) {
            if (imageAvailable[i]) vkDestroySemaphore(device, imageAvailable[i], nullptr);
            if (renderFinished[i]) vkDestroySemaphore(device, renderFinished[i], nullptr);
            if (inFlight[i]) vkDestroyFence(device, inFlight[i], nullptr);
        }
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

void OnAppCommand(android_app* app, int32_t cmd) {
    App* self = (App*)app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            self->window = app->window;
            if (!self->instance) self->CreateVulkan();
            self->DestroySwapchain();
            self->CreateSwapchain();
            break;
        case APP_CMD_TERM_WINDOW:
            self->DestroySwapchain();
            self->window = nullptr;
            break;
        case APP_CMD_CONFIG_CHANGED:
        case APP_CMD_WINDOW_RESIZED:
            self->recreate = true;
            break;
        case APP_CMD_RESUME: self->resumed = true; break;
        case APP_CMD_PAUSE: self->resumed = false; break;
        default: break;
    }
}

}  // namespace

void android_main(android_app* app) {
    App self;
    self.android = app;
    app->userData = &self;
    app->onAppCmd = OnAppCommand;

    timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!app->destroyRequested) {
        for (;;) {
            int events = 0;
            android_poll_source* source = nullptr;
            const bool rendering = self.resumed && self.swapchain;
            if (ALooper_pollOnce(rendering ? 0 : -1, nullptr, &events, (void**)&source) < 0) break;
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
        }
        if (app->destroyRequested) break;
        if (self.recreate && self.window) {
            self.recreate = false;
            self.DestroySwapchain();
            self.CreateSwapchain();
        }
        if (self.resumed && self.swapchain) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            const float seconds = (float)(now.tv_sec - start.tv_sec) + (float)(now.tv_nsec - start.tv_nsec) * 1e-9f;
            self.RenderFrame(seconds);
        }
    }
    self.Shutdown();
    LOGI("exiting after %llu frames", (unsigned long long)self.frameCount);
}
