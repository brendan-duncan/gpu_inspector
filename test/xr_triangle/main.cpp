// A minimal OpenXR + Vulkan application for Android headsets (Quest): a NativeActivity that
// renders a colored triangle in front of the user into one stereo swapchain, with multiview
// (both eyes in one render pass through gl_ViewIndex). It exercises what the inspector needs
// from an XR application: no swapchain present (the runtime composites), frames ending at the
// application's fence wait, and a multiview render pass whose two layers the capture reads back.
//
// Built by tools/build_xr_triangle.py with the NDK and the Khronos OpenXR loader for Android.
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "shaders.inl"   // generated: kVertSpv / kFragSpv (uint32_t arrays)

#define TAG "xr_triangle"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define XR_CHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { LOGE("%s failed: %d (%s:%d)", #x, (int)r_, __FILE__, __LINE__); abort(); } } while (0)
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOGE("%s failed: %d (%s:%d)", #x, (int)r_, __FILE__, __LINE__); abort(); } } while (0)

namespace {

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

// OpenXR's asymmetric fov to a Vulkan clip space projection (depth 0..1, y down).
Mat4 Projection(const XrFovf& fov, float nearZ, float farZ) {
    const float l = tanf(fov.angleLeft), r = tanf(fov.angleRight), d = tanf(fov.angleDown), u = tanf(fov.angleUp);
    Mat4 p{};
    p.m[0] = 2.0f / (r - l);
    p.m[5] = -2.0f / (u - d);
    p.m[8] = (r + l) / (r - l);
    p.m[9] = -(u + d) / (u - d);
    p.m[10] = farZ / (nearZ - farZ);
    p.m[11] = -1.0f;
    p.m[14] = (farZ * nearZ) / (nearZ - farZ);
    return p;
}

// The inverse of a rigid pose (rotation quaternion + position): the view matrix.
Mat4 ViewFromPose(const XrPosef& pose) {
    const float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z, w = pose.orientation.w;
    Mat4 rot = Identity();
    rot.m[0] = 1 - 2 * (y * y + z * z); rot.m[4] = 2 * (x * y - z * w);     rot.m[8] = 2 * (x * z + y * w);
    rot.m[1] = 2 * (x * y + z * w);     rot.m[5] = 1 - 2 * (x * x + z * z); rot.m[9] = 2 * (y * z - x * w);
    rot.m[2] = 2 * (x * z - y * w);     rot.m[6] = 2 * (y * z + x * w);     rot.m[10] = 1 - 2 * (x * x + y * y);
    // Inverse: transpose the rotation, negate the translation rotated by it.
    Mat4 inv = Identity();
    for (int c = 0; c < 3; ++c)
        for (int rr = 0; rr < 3; ++rr) inv.m[c * 4 + rr] = rot.m[rr * 4 + c];
    const float px = pose.position.x, py = pose.position.y, pz = pose.position.z;
    inv.m[12] = -(inv.m[0] * px + inv.m[4] * py + inv.m[8] * pz);
    inv.m[13] = -(inv.m[1] * px + inv.m[5] * py + inv.m[9] * pz);
    inv.m[14] = -(inv.m[2] * px + inv.m[6] * py + inv.m[10] * pz);
    return inv;
}

Mat4 RotationY(float a) {
    Mat4 r = Identity();
    r.m[0] = cosf(a); r.m[8] = sinf(a);
    r.m[2] = -sinf(a); r.m[10] = cosf(a);
    return r;
}

// ------------------------------------------------------------------------------------ app

struct SwapchainImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
};

struct App {
    android_app* android = nullptr;
    bool resumed = false;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool exitRequested = false;
    std::vector<XrViewConfigurationView> configViews;
    std::vector<XrView> views;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t swapchainFormat = 0;
    uint32_t width = 0, height = 0, viewCount = 0;
    std::vector<SwapchainImage> images;

    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel = nullptr;
    PFN_vkSetDebugUtilsObjectNameEXT setName = nullptr;
    uint64_t frameCount = 0;

    // ---------------------------------------------------------------------------- OpenXR

    void CreateInstance() {
        // The loader must learn about the JavaVM / activity before anything else on Android.
        PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
        XR_CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initializeLoader));
        XrLoaderInitInfoAndroidKHR loaderInit{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loaderInit.applicationVM = android->activity->vm;
        loaderInit.applicationContext = android->activity->clazz;
        XR_CHECK(initializeLoader((const XrLoaderInitInfoBaseHeaderKHR*)&loaderInit));

        const char* extensions[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
        XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        androidInfo.applicationVM = android->activity->vm;
        androidInfo.applicationActivity = android->activity->clazz;
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        ci.next = &androidInfo;
        strcpy(ci.applicationInfo.applicationName, "xr_triangle");
        ci.applicationInfo.applicationVersion = 1;
        strcpy(ci.applicationInfo.engineName, "none");
        ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ci.enabledExtensionCount = 2;
        ci.enabledExtensionNames = extensions;
        XR_CHECK(xrCreateInstance(&ci, &instance));
        XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
        XR_CHECK(xrGetInstanceProperties(instance, &props));
        LOGI("OpenXR runtime: %s %u.%u.%u", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));

        XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
        si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XR_CHECK(xrGetSystem(instance, &si, &system));
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        XR_CHECK(xrGetSystemProperties(instance, system, &sp));
        LOGI("system: %s", sp.systemName);
    }

    template <typename F>
    F XrProc(const char* name) {
        PFN_xrVoidFunction fn = nullptr;
        XR_CHECK(xrGetInstanceProcAddr(instance, name, &fn));
        return (F)fn;
    }

    // ---------------------------------------------------------------------------- Vulkan

    void CreateVulkan() {
        auto getRequirements = XrProc<PFN_xrGetVulkanGraphicsRequirements2KHR>("xrGetVulkanGraphicsRequirements2KHR");
        XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        XR_CHECK(getRequirements(instance, system, &req));
        LOGI("Vulkan %u.%u required", XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported));

        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "xr_triangle";
        ai.pEngineName = "none";
        ai.apiVersion = VK_API_VERSION_1_1;
        const char* instanceExtensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &ai;
        ici.enabledExtensionCount = 1;
        ici.ppEnabledExtensionNames = instanceExtensions;
        XrVulkanInstanceCreateInfoKHR xici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        xici.systemId = system;
        xici.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xici.vulkanCreateInfo = &ici;
        VkResult vkRes = VK_SUCCESS;
        auto createInstance = XrProc<PFN_xrCreateVulkanInstanceKHR>("xrCreateVulkanInstanceKHR");
        XR_CHECK(createInstance(instance, &xici, &vkInstance, &vkRes));
        if (vkRes != VK_SUCCESS) {
            // Without debug utils (an older loader): try again without the extension.
            ici.enabledExtensionCount = 0;
            XR_CHECK(createInstance(instance, &xici, &vkInstance, &vkRes));
        }
        VK_CHECK(vkRes);

        auto getDevice = XrProc<PFN_xrGetVulkanGraphicsDevice2KHR>("xrGetVulkanGraphicsDevice2KHR");
        XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        gdi.systemId = system;
        gdi.vulkanInstance = vkInstance;
        XR_CHECK(getDevice(instance, &gdi, &gpu));
        VkPhysicalDeviceProperties gp;
        vkGetPhysicalDeviceProperties(gpu, &gp);
        LOGI("GPU: %s", gp.deviceName);

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families.data());
        queueFamily = 0;
        for (uint32_t i = 0; i < familyCount; ++i)
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { queueFamily = i; break; }

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
        multiview.multiview = VK_TRUE;
        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &multiview;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pEnabledFeatures = &features;
        XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        xdci.systemId = system;
        xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xdci.vulkanPhysicalDevice = gpu;
        xdci.vulkanCreateInfo = &dci;
        auto createDevice = XrProc<PFN_xrCreateVulkanDeviceKHR>("xrCreateVulkanDeviceKHR");
        XR_CHECK(createDevice(instance, &xdci, &device, &vkRes));
        VK_CHECK(vkRes);
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        beginLabel = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(vkInstance, "vkCmdBeginDebugUtilsLabelEXT");
        endLabel = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(vkInstance, "vkCmdEndDebugUtilsLabelEXT");
        setName = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(vkInstance, "vkSetDebugUtilsObjectNameEXT");

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily;
        VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &commandPool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = commandPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &cai, &commandBuffer));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));
    }

    void Name(VkObjectType type, uint64_t handle, const char* name) {
        if (!setName) return;
        VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        ni.objectType = type;
        ni.objectHandle = handle;
        ni.pObjectName = name;
        setName(device, &ni);
    }

    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
        return 0;
    }

    // ---------------------------------------------------------------------------- session

    void CreateSession() {
        XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        binding.instance = vkInstance;
        binding.physicalDevice = gpu;
        binding.device = device;
        binding.queueFamilyIndex = queueFamily;
        binding.queueIndex = 0;
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
        sci.next = &binding;
        sci.systemId = system;
        XR_CHECK(xrCreateSession(instance, &sci, &session));

        XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rsci.poseInReferenceSpace.orientation.w = 1.0f;
        XR_CHECK(xrCreateReferenceSpace(session, &rsci, &space));

        uint32_t count = 0;
        XR_CHECK(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr));
        configViews.assign(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        XR_CHECK(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, configViews.data()));
        viewCount = count;
        views.assign(count, {XR_TYPE_VIEW});
        width = configViews[0].recommendedImageRectWidth;
        height = configViews[0].recommendedImageRectHeight;
        LOGI("%u views of %ux%u", viewCount, width, height);

        // One swapchain with a layer per view, rendered with multiview.
        uint32_t formatCount = 0;
        XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr));
        std::vector<int64_t> formats(formatCount);
        XR_CHECK(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()));
        swapchainFormat = formats[0];
        for (int64_t f : formats) if (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB) { swapchainFormat = f; break; }
        XrSwapchainCreateInfo swci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        swci.format = swapchainFormat;
        swci.sampleCount = 1;
        swci.width = width;
        swci.height = height;
        swci.faceCount = 1;
        swci.arraySize = viewCount;
        swci.mipCount = 1;
        XR_CHECK(xrCreateSwapchain(session, &swci, &swapchain));
        uint32_t imageCount = 0;
        XR_CHECK(xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr));
        std::vector<XrSwapchainImageVulkan2KHR> xrImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(swapchain, imageCount, &imageCount, (XrSwapchainImageBaseHeader*)xrImages.data()));
        LOGI("swapchain: %u images, format %lld", imageCount, (long long)swapchainFormat);

        CreateRenderPass();
        CreatePipeline();
        CreateDepth();
        images.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            images[i].image = xrImages[i].image;
            char name[32];
            snprintf(name, sizeof(name), "XR swapchain image %u", i);
            Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)images[i].image, name);
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = images[i].image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            vci.format = (VkFormat)swapchainFormat;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, viewCount};
            VK_CHECK(vkCreateImageView(device, &vci, nullptr, &images[i].view));
            VkImageView attachments[] = {images[i].view, depthView};
            VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbci.renderPass = renderPass;
            fbci.attachmentCount = 2;
            fbci.pAttachments = attachments;
            fbci.width = width;
            fbci.height = height;
            fbci.layers = 1;   // multiview: the view mask selects the layers
            VK_CHECK(vkCreateFramebuffer(device, &fbci, nullptr, &images[i].framebuffer));
        }
    }

    void CreateRenderPass() {
        VkAttachmentDescription atts[2]{};
        atts[0].format = (VkFormat)swapchainFormat;
        atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // The runtime hands the image over in COLOR_ATTACHMENT_OPTIMAL and expects it back so.
        atts[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
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
        // Multiview: both eyes in one pass, gl_ViewIndex picks the eye's matrix.
        const uint32_t viewMask = (1u << viewCount) - 1;
        VkRenderPassMultiviewCreateInfo mv{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
        mv.subpassCount = 1;
        mv.pViewMasks = &viewMask;
        mv.correlationMaskCount = 1;
        mv.pCorrelationMasks = &viewMask;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.pNext = &mv;
        rpci.attachmentCount = 2;
        rpci.pAttachments = atts;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sp;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(device, &rpci, nullptr, &renderPass));
        Name(VK_OBJECT_TYPE_RENDER_PASS, (uint64_t)renderPass, "Stereo pass");
    }

    void CreateDepth() {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_D24_UNORM_S8_UINT;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = viewCount;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device, &ici, nullptr, &depthImage));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, depthImage, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &depthMemory));
        VK_CHECK(vkBindImageMemory(device, depthImage, depthMemory, 0));
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = depthImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vci.format = VK_FORMAT_D24_UNORM_S8_UINT;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, viewCount};
        VK_CHECK(vkCreateImageView(device, &vci, nullptr, &depthView));
        Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)depthImage, "Stereo depth");
    }

    VkShaderModule Module(const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
        return m;
    }

    void CreatePipeline() {
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4) * 2};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout));

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
        VkViewport viewport{0, 0, (float)width, (float)height, 0, 1};
        VkRect2D scissor{{0, 0}, {width, height}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.pViewports = &viewport;
        vp.scissorCount = 1;
        vp.pScissors = &scissor;
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
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &blend;
        pci.layout = pipelineLayout;
        pci.renderPass = renderPass;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline));
        Name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)pipeline, "Triangle pipeline");
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
    }

    // ---------------------------------------------------------------------------- frame

    void HandleSessionState(const XrEventDataSessionStateChanged& e) {
        state = e.state;
        LOGI("session state %d", (int)state);
        switch (state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XR_CHECK(xrBeginSession(session, &bi));
                running = true;
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                running = false;
                XR_CHECK(xrEndSession(session));
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                exitRequested = true;
                break;
            default: break;
        }
    }

    void PollEvents() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
                HandleSessionState(*reinterpret_cast<const XrEventDataSessionStateChanged*>(&event));
            else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
                exitRequested = true;
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void RenderFrame() {
        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XR_CHECK(xrWaitFrame(session, &fwi, &fs));
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        XR_CHECK(xrBeginFrame(session, &fbi));

        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::vector<XrCompositionLayerProjectionView> projViews(viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
        uint32_t layerCount = 0;

        if (fs.shouldRender) {
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = space;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            uint32_t located = 0;
            XR_CHECK(xrLocateViews(session, &vli, &vs, viewCount, &located, views.data()));

            uint32_t imageIndex = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XR_CHECK(xrAcquireSwapchainImage(swapchain, &ai, &imageIndex));
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            XR_CHECK(xrWaitSwapchainImage(swapchain, &wi));

            // The previous frame's work (the fence wait is what the inspector's frame boundary
            // keys on for applications without a swapchain present).
            VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
            VK_CHECK(vkResetFences(device, 1, &fence));
            Record(imageIndex, fs.predictedDisplayTime);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &commandBuffer;
            VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));

            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            XR_CHECK(xrReleaseSwapchainImage(swapchain, &ri));

            for (uint32_t v = 0; v < viewCount; ++v) {
                projViews[v].pose = views[v].pose;
                projViews[v].fov = views[v].fov;
                projViews[v].subImage.swapchain = swapchain;
                projViews[v].subImage.imageRect = {{0, 0}, {(int32_t)width, (int32_t)height}};
                projViews[v].subImage.imageArrayIndex = v;
            }
            layer.space = space;
            layer.viewCount = viewCount;
            layer.views = projViews.data();
            layerCount = 1;
            frameCount++;
        }

        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = layerCount;
        fei.layers = layers;
        XR_CHECK(xrEndFrame(session, &fei));
    }

    void Record(uint32_t imageIndex, XrTime time) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(commandBuffer, &bi));
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = "Stereo scene";
        label.color[0] = 0.2f; label.color[1] = 0.8f; label.color[2] = 0.4f; label.color[3] = 1.0f;
        if (beginLabel) beginLabel(commandBuffer, &label);

        VkClearValue clears[2]{};
        clears[0].color = {{0.05f, 0.05f, 0.12f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = renderPass;
        rpbi.framebuffer = images[imageIndex].framebuffer;
        rpbi.renderArea = {{0, 0}, {width, height}};
        rpbi.clearValueCount = 2;
        rpbi.pClearValues = clears;
        vkCmdBeginRenderPass(commandBuffer, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        // A triangle two meters ahead, slowly turning.
        const float seconds = (float)((double)time * 1e-9);
        Mat4 model = RotationY(seconds * 0.5f);
        model.m[14] = -2.0f;
        Mat4 mvp[2];
        for (uint32_t v = 0; v < viewCount && v < 2; ++v)
            mvp[v] = Multiply(Multiply(Projection(views[v].fov, 0.05f, 100.0f), ViewFromPose(views[v].pose)), model);
        vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), mvp);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(commandBuffer);
        if (endLabel) endLabel(commandBuffer);
        VK_CHECK(vkEndCommandBuffer(commandBuffer));
    }

    void Shutdown() {
        if (device) vkDeviceWaitIdle(device);
        for (auto& i : images) {
            if (i.framebuffer) vkDestroyFramebuffer(device, i.framebuffer, nullptr);
            if (i.view) vkDestroyImageView(device, i.view, nullptr);
        }
        images.clear();
        if (swapchain) xrDestroySwapchain(swapchain);
        if (depthView) vkDestroyImageView(device, depthView, nullptr);
        if (depthImage) vkDestroyImage(device, depthImage, nullptr);
        if (depthMemory) vkFreeMemory(device, depthMemory, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        if (space) xrDestroySpace(space);
        if (session) xrDestroySession(session);
        if (device) vkDestroyDevice(device, nullptr);
        if (vkInstance) vkDestroyInstance(vkInstance, nullptr);
        if (instance) xrDestroyInstance(instance);
    }
};

void OnAppCommand(android_app* app, int32_t cmd) {
    App* self = (App*)app->userData;
    switch (cmd) {
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

    self.CreateInstance();
    self.CreateVulkan();
    self.CreateSession();

    while (!app->destroyRequested && !self.exitRequested) {
        // Events: block while nothing runs, poll while rendering.
        for (;;) {
            int events = 0;
            android_poll_source* source = nullptr;
            const int timeout = (!self.resumed && !self.running) || app->destroyRequested ? -1 : 0;
            if (ALooper_pollOnce(timeout, nullptr, &events, (void**)&source) < 0) break;
            if (source) source->process(app, source);
        }
        self.PollEvents();
        if (self.running) self.RenderFrame();
    }
    self.Shutdown();
    LOGI("exiting after %llu frames", (unsigned long long)self.frameCount);
}
