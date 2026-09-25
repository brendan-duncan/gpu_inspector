// Minimal Vulkan test application: a rotating textured cube with a depth buffer, re-recording
// its command buffer every frame. Exercises what the inspector needs: buffers, images, samplers,
// descriptor sets, push constants, debug labels, per-frame command recording and presentation.
//
// Usage: vkinsp_triangle [--frames N] [--width W] [--height H] [--capture-at N] [--churn]
//
// The window is resizable: the swapchain, depth buffer and framebuffers are recreated when the
// window size changes (or when acquire/present report the swapchain out of date), which also
// exercises the inspector's handling of object destruction and swapchain replacement.

#include <vulkan/vulkan.h>

#include "gpu_inspector.h"

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
#include <timeapi.h>
#else
#include <xcb/xcb.h>
#endif

#define CHECK(x)                                                                         \
    do                                                                                   \
    {                                                                                    \
        VkResult r_ = (x);                                                               \
        if (r_ != VK_SUCCESS)                                                            \
        {                                                                                \
            fprintf(stderr, "%s failed: %d (%s:%d)\n", #x, (int)r_, __FILE__, __LINE__); \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

namespace
{

struct Vertex
{
    float pos[3];
    float color[3];
    float uv[2];
};

struct Mat4
{
    float m[16];
};

Mat4 Mul(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Mat4 Perspective(float fovy, float aspect, float zn, float zf)
{
    float f = 1.0f / tanf(fovy / 2);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = -f;  // Vulkan clip space: y down
    r.m[10] = zf / (zn - zf);
    r.m[11] = -1;
    r.m[14] = (zn * zf) / (zn - zf);
    return r;
}

Mat4 Translate(float x, float y, float z)
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

Mat4 RotateY(float a)
{
    Mat4 r{};
    r.m[0] = cosf(a);
    r.m[2] = -sinf(a);
    r.m[5] = 1;
    r.m[8] = sinf(a);
    r.m[10] = cosf(a);
    r.m[15] = 1;
    return r;
}

Mat4 RotateX(float a)
{
    Mat4 r{};
    r.m[0] = 1;
    r.m[5] = cosf(a);
    r.m[6] = sinf(a);
    r.m[9] = -sinf(a);
    r.m[10] = cosf(a);
    r.m[15] = 1;
    return r;
}

std::vector<char> ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", path.c_str());
        exit(1);
    }
    std::vector<char> data((size_t)f.tellg());
    f.seekg(0);
    f.read(data.data(), (std::streamsize)data.size());
    return data;
}

std::string ExeDir()
{
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

// Mip levels of the checker texture (8x8 -> 1x1).
constexpr uint32_t kTextureMips = 4;

struct App
{
    uint32_t width = 640, height = 480;
    int maxFrames = -1;
    int captureAt = 0;        // --capture-at: ask the inspector for a capture at this frame (gpu_inspector.h)
    bool captureAsked = false;
    // --churn: what a memory capture is for. Every frame makes a small buffer with its own
    // allocation and frees the one made two frames before (transient allocations), and every 30th
    // frame makes one that is kept until exit (a slow leak).
    bool churn = false;
    struct ChurnBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    std::vector<ChurnBuffer> churnRecent, churnKept;
    bool badScissor = false;
    // --half-scissor: the scissor keeps the left half of the target, so the cube is cut down the
    // middle. It is what the Viewport / Scissor overlay is for: a draw that runs and rasterizes and
    // still has no pixels where they are expected.
    bool halfScissor = false;
    bool leak = false;
    // --hazard: every frame the vertex buffer is written with vkCmdUpdateBuffer in a command
    // buffer submitted on its own, with no semaphore or barrier before the main submission's
    // draw reads it, so synchronization validation reports the hazard at vkQueueSubmit (a
    // hazard against the previous frame would not do: a capture waits for the GPU first).
    bool hazard = false;
    // --occluded: the cube is drawn a second time where it already is, so every fragment of the
    // second draw fails the depth test (LESS against its own depth): the inspector's depth test
    // overlay has a draw that is all rejected.
    bool occluded = false;
    // --no-cull: the cube is drawn with back faces kept, so every pixel it covers has two fragments
    // of the *same* draw -- the near face and the far one. That is what a pixel history's
    // per-fragment breakdown is for, and what a draw's own entry can only report the winner of.
    bool noCull = false;
    // --inside-out: half the cube's faces, wound the other way, so its own cull mode throws every
    // one of them away and the draw puts nothing on the screen -- the bug the Backface Cull overlay
    // exists to find, which otherwise looks like a draw that ran and did nothing. A whole cube
    // cannot show it: whichever way it is wound, some face still points at the camera.
    bool insideOut = false;
    // --prerecord: record one command buffer per swapchain image up front and resubmit them
    // every frame (the tint and the wave then stand still), like engines with static command
    // buffers; a capture needs the inspector's "Record all command buffers". A second prerecorded
    // buffer, submitted with the first, clears the left half of the image in a pass of its own, so
    // the first buffer's pass has a result the second overwrites within one submission.
    bool prerecord = false;
    // --push-template: the cube's uniform buffer and texture are pushed through a descriptor update
    // template (vkCmdPushDescriptorSetWithTemplateKHR) instead of a bound set, whose data the
    // capture snapshots and the replay pushes again as plain writes.
    bool pushTemplate = false;
    bool descriptorBuffer = false;
    // --alpha-test: the cube's fragment shader is alpha.frag, which discards the checker's dark
    // squares: alpha-tested geometry, whose overdraw counts only the fragments it keeps.
    bool alphaTest = false;
    // --device-local-descriptors: --descriptor-buffer, with the descriptor buffer in memory the host
    // never maps, filled by a copy from a staging buffer, so the layer cannot read it where it is bound.
    bool deviceLocalDescriptors = false;
    bool compileHitch = false;
    int hitchEvery = 0;       // --hitch-every N: stall 100 ms inside every Nth frame, for Capture on hitch
    int stallMs = 0;          // --stall <ms>: sleep this long every frame, so vsynced presents miss refreshes
    bool outOfBounds = false;
    VkPipeline hitchPipeline{};
    // --pipeline-library: the cube pipeline is linked from two graphics pipeline libraries (vertex
    // input and pre-rasterization with the vertex shader; fragment shader and output), which live
    // as long as the pipeline linked from them (VK_EXT_graphics_pipeline_library).
    bool pipelineLibrary = false;
    VkPipeline pipelineLibraries[2]{};
    // --shader-object: the cube is drawn with linked vertex and fragment shader objects
    // (VK_EXT_shader_object) and every piece of state set dynamically, instead of its pipeline.
    bool shaderObject = false;
    VkShaderEXT shaders[2]{};
    // --mixed: --shader-object, and the cube drawn a second time where it already is (as --occluded)
    // with its pipeline, made for dynamic rendering: one pass holding both kinds of draw.
    bool mixed = false;
    // --suspend: the cube's pass is dynamic rendering split across two command buffers, suspended
    // at the end of the frame's buffer and resumed in a second one submitted right after it
    // (VK_RENDERING_SUSPENDING_BIT / VK_RENDERING_RESUMING_BIT). Nothing may be recorded between
    // the two parts, so the layer's per-pass timestamps, queries and read-back copies must not be.
    bool suspend = false;
    // --stencil: the depth buffer has a stencil aspect (D24S8 or D32S8), cleared by the pass and
    // written with 1 wherever the cube draws, so a capture reads a stencil target back too.
    bool stencil = false;
    // --ray-tracing: each frame rebuilds a top-level acceleration structure over one triangle's
    // bottom-level structure and traces a 256x256 storage image with a raygen, miss and closest hit
    // pipeline (VK_KHR_ray_tracing_pipeline), so a capture has a ray tracing pipeline, its shader
    // groups, acceleration structures and a vkCmdTraceRaysKHR.
    bool rayTracing = false;
    // --static-blas (with --ray-tracing): the bottom level is built once at start-up and never
    // again, as most engines do, so a capture holds no build of it and the layer has to read its
    // inputs back when the capture begins (CaptureManager::ReadBackEarlierStructures).
    bool staticBlas = false;
    // --shader-record (implies --ray-tracing): the hit group's shader record carries the device
    // address of a tint buffer no descriptor set binds, read through a buffer reference
    // (rt_record.rchit), so a capture's binding table holds an address the replay must translate.
    bool shaderRecord = false;
    struct DeviceBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        void* mapped = nullptr;
    };
    struct RayTracing
    {
        PFN_vkCreateAccelerationStructureKHR createAS;
        PFN_vkDestroyAccelerationStructureKHR destroyAS;
        PFN_vkGetAccelerationStructureBuildSizesKHR buildSizes;
        PFN_vkGetAccelerationStructureDeviceAddressKHR asAddress;
        PFN_vkCmdBuildAccelerationStructuresKHR build;
        PFN_vkCreateRayTracingPipelinesKHR createPipelines;
        PFN_vkGetRayTracingShaderGroupHandlesKHR groupHandles;
        PFN_vkCmdTraceRaysKHR trace;
        DeviceBuffer vertices, instances, blasMemory, tlasMemory, scratch, sbt, tint;
        VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory imageMemory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkStridedDeviceAddressRegionKHR raygen{}, miss{}, hit{}, callable{};
    } rt{};
    // --descriptor-buffer: the set's descriptors live in a buffer the application owns rather
    // than in a set object, so a draw names them by an offset into memory (docs/VULKAN.md).
    struct DescriptorBufferFns
    {
        PFN_vkGetDescriptorSetLayoutSizeEXT layoutSize;
        PFN_vkGetDescriptorSetLayoutBindingOffsetEXT bindingOffset;
        PFN_vkGetDescriptorEXT getDescriptor;
        PFN_vkCmdBindDescriptorBuffersEXT bindBuffers;
        PFN_vkCmdSetDescriptorBufferOffsetsEXT setOffsets;
        VkPhysicalDeviceDescriptorBufferPropertiesEXT props{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        void* mapped = nullptr;
    } db{};
    struct ShaderObjectFns
    {
        PFN_vkCreateShadersEXT create;
        PFN_vkDestroyShaderEXT destroy;
        PFN_vkCmdBindShadersEXT bind;
        PFN_vkCmdSetViewportWithCount viewport;
        PFN_vkCmdSetScissorWithCount scissor;
        PFN_vkCmdSetRasterizerDiscardEnable rasterizerDiscard;
        PFN_vkCmdSetCullMode cull;
        PFN_vkCmdSetFrontFace frontFace;
        PFN_vkCmdSetDepthTestEnable depthTest;
        PFN_vkCmdSetDepthWriteEnable depthWrite;
        PFN_vkCmdSetDepthCompareOp depthCompare;
        PFN_vkCmdSetDepthBiasEnable depthBias;
        PFN_vkCmdSetStencilTestEnable stencilTest;
        PFN_vkCmdSetPrimitiveTopology topology;
        PFN_vkCmdSetPrimitiveRestartEnable primitiveRestart;
        PFN_vkCmdSetVertexInputEXT vertexInput;
        PFN_vkCmdSetPolygonModeEXT polygonMode;
        PFN_vkCmdSetRasterizationSamplesEXT samples;
        PFN_vkCmdSetSampleMaskEXT sampleMask;
        PFN_vkCmdSetAlphaToCoverageEnableEXT alphaToCoverage;
        PFN_vkCmdSetColorBlendEnableEXT blendEnable;
        PFN_vkCmdSetColorWriteMaskEXT writeMask;
    } so{};
    // --second-device / --second-queue: a second stream of work each frame, a 256x256 offscreen
    // target cleared in a render pass of its own, on a VkDevice of its own (same GPU) or on a second
    // queue of the main device, so a capture has passes, timings and read-backs from both.
    enum class Side
    {
        None,
        Device,
        Queue
    } side = Side::None;
    struct SideWork
    {
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkRenderPass pass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    } sideWork;
    VkDescriptorUpdateTemplate pushUpdateTemplate{};
    PFN_vkCmdPushDescriptorSetWithTemplateKHR pushWithTemplate = nullptr;   // not in every loader's import library
    struct PushData
    {
        VkDescriptorBufferInfo uniform;
        VkDescriptorImageInfo texture;
    } pushData{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;  // --msaa: 4x, resolved into the swapchain
    // --offscreen: render into an image of our own and never present, like an OpenXR
    // application whose runtime composites (the inspector's frame boundaries without presents).
    bool offscreen = false;
    // --multiview: the main pass renders both views of a two-layer stereo target at once (view mask
    // 0b11, cube_mv.vert shifting each view the other way), as an XR application's eyes are drawn;
    // the layers are then blitted side by side into the swapchain image.
    bool multiview = false;
    // --dynamic-rendering: the main pass in dynamic rendering, drawn with the cube's pipeline made
    // for it (with --multiview, its view mask in the rendering info and the pipeline).
    bool dynamicRenderingPass = false;
    // --persistent: every frame reads state the frames before it left behind, which a capture
    // must hold for its replay to match (see RecordPersistent).
    bool persistent = false;
    // --heavy: the cube's fragment shader is heavy.frag, whose functions cost known amounts (the
    // Shader Flame Graph's measurements by ablation).
    bool heavy = false;
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
    VkImage msaaImage{};        // multisampled color target (--msaa)
    VkDeviceMemory msaaMemory{};
    VkImageView msaaView{};
    VkImage stereoImage{};      // --multiview: the two-layer color target, one layer per view
    VkDeviceMemory stereoMemory{};
    VkImageView stereoView{};
    VkImage offImage{};         // --offscreen: the color target that stands in for the swapchain image
    VkDeviceMemory offMemory{};
    VkImageView offView{};
    VkFramebuffer offFramebuffer{};
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

    // --persistent: small images that keep their contents from frame to frame, one render pass
    // that loads them, and a staging buffer per frame slot the host writes every frame.
    static const uint32_t kPersistSize = 64;
    static const uint32_t kStagingSize = 16;
    struct PersistImage
    {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkFramebuffer framebuffer{};
    };
    VkRenderPass persistPass{};
    PersistImage trail, source, copy;

    VkCommandPool commandPool{};
    static const int kFramesInFlight = 2;
    VkBuffer persistStaging[kFramesInFlight]{};
    VkDeviceMemory persistStagingMemory[kFramesInFlight]{};
    void* persistStagingMapped[kFramesInFlight]{};
    VkCommandBuffer commandBuffers[kFramesInFlight]{};
    VkCommandBuffer hazardBuffers[kFramesInFlight]{};   // --hazard: the vertex update, submitted first
    VkCommandBuffer suspendBuffers[kFramesInFlight]{};  // --suspend: the resumed half of the pass, submitted second
    std::vector<VkCommandBuffer> prerecorded;            // --prerecord: one per swapchain image
    std::vector<VkCommandBuffer> overlays;               // --prerecord: the overlay pass, one per image
    std::vector<VkFramebuffer> overlayFramebuffers;
    VkRenderPass overlayPass{};
    VkSemaphore imageAvailable[kFramesInFlight]{};
    VkSemaphore renderFinished[kFramesInFlight]{};
    VkFence inFlight[kFramesInFlight]{};
    int frameSlot = 0;
    uint64_t frameCount = 0;

    // --------------------------------------------------------------------------------- window
#if defined(_WIN32)
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
    {
        App* app = (App*)GetWindowLongPtrA(h, GWLP_USERDATA);
        if (msg == WM_CLOSE || msg == WM_DESTROY)
        {
            if (app)
                app->quit = true;
            return 0;
        }
        if (msg == WM_KEYDOWN && w == VK_ESCAPE && app)
            app->quit = true;
        if (msg == WM_SIZE && app)
            app->resized = true;
        return DefWindowProcA(h, msg, w, l);
    }

    void CreateWindowNative()
    {
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

    void PumpEvents()
    {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
#else
    void CreateWindowNative()
    {
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

    void PumpEvents()
    {
        while (xcb_generic_event_t* e = xcb_poll_for_event(conn))
        {
            uint8_t type = e->response_type & 0x7f;
            if (type == XCB_KEY_PRESS)
            {
                quit = true;
            }
            else if (type == XCB_CONFIGURE_NOTIFY)
            {
                auto* c = reinterpret_cast<xcb_configure_notify_event_t*>(e);
                if (c->width != width || c->height != height)
                {
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
    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags props)
    {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
                return i;
        fprintf(stderr, "no memory type\n");
        exit(1);
    }

    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buf,
        VkDeviceMemory& mem, const char* name, bool deviceAddress = false)
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage | (deviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CHECK(vkCreateBuffer(device, &bci, nullptr, &buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buf, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        if (deviceAddress)
            mai.pNext = &flags;
        CHECK(vkAllocateMemory(device, &mai, nullptr, &mem));
        CHECK(vkBindBufferMemory(device, buf, mem, 0));
        Name(VK_OBJECT_TYPE_BUFFER, (uint64_t)buf, name);
    }

    void Name(VkObjectType type, uint64_t handle, const char* name)
    {
        if (!setName)
            return;
        VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        ni.objectType = type;
        ni.objectHandle = handle;
        ni.pObjectName = name;
        setName(device, &ni);
    }

    VkShaderModule LoadShader(const char* file)
    {
        std::vector<char> code = ReadFile(ExeDir() + file);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m;
        CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
        return m;
    }

    VkCommandBuffer BeginOneShot()
    {
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

    void EndOneShot(VkCommandBuffer cb)
    {
        CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        vkFreeCommandBuffers(device, commandPool, 1, &cb);
    }

    // --------------------------------------------------------------------------------- setup
    void InitVulkan()
    {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "vkinsp_triangle";
        ai.pEngineName = "none";
        // --shader-object and --suspend draw in dynamic rendering, core in 1.3; --ray-tracing needs
        // 1.2's buffer device addresses and SPIR-V 1.4.
        // 1.2 for the modes that take a buffer's device address: vkGetBufferDeviceAddress is core
        // there, and on a 1.1 instance the loader has no entry point for it to call.
        ai.apiVersion = shaderObject || suspend || dynamicRenderingPass ? VK_API_VERSION_1_3
            : rayTracing || descriptorBuffer    ? VK_API_VERSION_1_2
                                                : VK_API_VERSION_1_1;
        std::vector<const char*> instExts = {
            VK_KHR_SURFACE_EXTENSION_NAME,
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
            if (strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                debugUtils = true;
        if (debugUtils)
            instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

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
        for (VkPhysicalDevice g : gpus)
        {
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qf.data());
            for (uint32_t i = 0; i < qn; ++i)
            {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(g, i, surface, &present);
                if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
                {
                    gpu = g;
                    queueFamily = i;
                    break;
                }
            }
            if (gpu)
                break;
        }
        if (!gpu)
        {
            fprintf(stderr, "no suitable GPU\n");
            exit(1);
        }
        vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);

        float prio[2] = {1.0f, 1.0f};
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = prio;
        if (side == Side::Queue)
        {
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qf.data());
            if (qf[queueFamily].queueCount < 2)
            {
                fprintf(stderr, "--second-queue: the graphics queue family has a single queue\n");
                exit(1);
            }
            qci.queueCount = 2;
        }
        std::vector<const char*> devExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if (pushTemplate)
            devExts.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        if (descriptorBuffer)
        {
            devExts.push_back(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
            // What VK_EXT_descriptor_buffer is defined on top of, and so must be enabled with it.
            devExts.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
            devExts.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
            devExts.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        }
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
        if (pipelineLibrary)
        {
            devExts.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
            devExts.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
            gpl.graphicsPipelineLibrary = VK_TRUE;
            dci.pNext = &gpl;
        }
        VkPhysicalDeviceShaderObjectFeaturesEXT soFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
        VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        VkPhysicalDeviceMultiviewFeatures multiviewFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
        if (multiview)
        {
            // Core in 1.1. The modes it does not combine with change the targets it renders to, and
            // shader objects cannot draw multiview at all: the view mask is state an implementation
            // needs when it compiles, which a shader object does not have (VK_EXT_shader_object,
            // issue 5.9). The validation layer does not say so, and the second view comes out empty.
            if (samples != VK_SAMPLE_COUNT_1_BIT || stencil || offscreen || suspend || prerecord || pipelineLibrary || shaderObject)
            {
                fprintf(stderr, "--multiview does not combine with --msaa, --stencil, --offscreen, --suspend, --prerecord, --pipeline-library, --shader-object or --mixed\n");
                exit(1);
            }
            multiviewFeatures.multiview = VK_TRUE;
            multiviewFeatures.pNext = (void*)dci.pNext;
            dci.pNext = &multiviewFeatures;
        }
        if (shaderObject || suspend || dynamicRenderingPass)
        {
            // Dynamic rendering (core in the 1.3 instance these modes ask for): what shader objects
            // draw in, and what a pass can be suspended and resumed in. Its targets here are
            // single-sampled, and the split pass is recorded every frame.
            if (samples != VK_SAMPLE_COUNT_1_BIT || stencil || (suspend && prerecord))
            {
                fprintf(stderr, "--shader-object and --suspend do not combine with --msaa or --stencil, nor --suspend with --prerecord\n");
                exit(1);
            }
            dynamicRendering.dynamicRendering = VK_TRUE;
            dynamicRendering.pNext = (void*)dci.pNext;
            dci.pNext = &dynamicRendering;
        }
        if (shaderObject)
        {
            // The extension brings the dynamic state commands it needs with it.
            devExts.push_back(VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
            soFeatures.shaderObject = VK_TRUE;
            soFeatures.pNext = (void*)dci.pNext;
            dci.pNext = &soFeatures;
        }
        VkPhysicalDeviceDescriptorBufferFeaturesEXT dbFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        VkPhysicalDeviceBufferDeviceAddressFeatures dbAddress{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        if (descriptorBuffer)
        {
            // A descriptor buffer is named by its device address, and so is every buffer a
            // descriptor in it points at, so the two features go together.
            dbFeatures.descriptorBuffer = VK_TRUE;
            dbFeatures.pNext = (void*)dci.pNext;
            dbAddress.bufferDeviceAddress = VK_TRUE;
            dbAddress.pNext = &dbFeatures;
            dci.pNext = &dbAddress;
        }
        VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddress{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        if (rayTracing)
        {
            devExts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            devExts.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            devExts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            bufferAddress.bufferDeviceAddress = VK_TRUE;
            asFeatures.accelerationStructure = VK_TRUE;
            rtFeatures.rayTracingPipeline = VK_TRUE;
            rtFeatures.pNext = (void*)dci.pNext;
            asFeatures.pNext = &rtFeatures;
            bufferAddress.pNext = &asFeatures;
            dci.pNext = &bufferAddress;
        }
        dci.enabledExtensionCount = (uint32_t)devExts.size();
        dci.ppEnabledExtensionNames = devExts.data();
        CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        if (stencil)
        {
            // A depth-stencil format: every device offers one of these two as an attachment.
            for (VkFormat f : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT})
            {
                VkFormatProperties props{};
                vkGetPhysicalDeviceFormatProperties(gpu, f, &props);
                if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                {
                    depthFormat = f;
                    break;
                }
            }
        }
        if (side == Side::Device)
        {
            // Its own device on the same GPU, with the same single queue and no extensions.
            VkDeviceCreateInfo sdci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            VkDeviceQueueCreateInfo sqci = qci;
            sqci.queueCount = 1;
            sdci.queueCreateInfoCount = 1;
            sdci.pQueueCreateInfos = &sqci;
            CHECK(vkCreateDevice(gpu, &sdci, nullptr, &sideWork.device));
            vkGetDeviceQueue(sideWork.device, queueFamily, 0, &sideWork.queue);
        }
        else if (side == Side::Queue)
        {
            sideWork.device = device;
            vkGetDeviceQueue(device, queueFamily, 1, &sideWork.queue);
        }
        if (rayTracing)
        {
            auto fn = [&](const char* name) { return vkGetDeviceProcAddr(device, name); };
            rt.createAS = (PFN_vkCreateAccelerationStructureKHR)fn("vkCreateAccelerationStructureKHR");
            rt.destroyAS = (PFN_vkDestroyAccelerationStructureKHR)fn("vkDestroyAccelerationStructureKHR");
            rt.buildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)fn("vkGetAccelerationStructureBuildSizesKHR");
            rt.asAddress = (PFN_vkGetAccelerationStructureDeviceAddressKHR)fn("vkGetAccelerationStructureDeviceAddressKHR");
            rt.build = (PFN_vkCmdBuildAccelerationStructuresKHR)fn("vkCmdBuildAccelerationStructuresKHR");
            rt.createPipelines = (PFN_vkCreateRayTracingPipelinesKHR)fn("vkCreateRayTracingPipelinesKHR");
            rt.groupHandles = (PFN_vkGetRayTracingShaderGroupHandlesKHR)fn("vkGetRayTracingShaderGroupHandlesKHR");
            rt.trace = (PFN_vkCmdTraceRaysKHR)fn("vkCmdTraceRaysKHR");
            if (!rt.createAS || !rt.build || !rt.createPipelines || !rt.trace)
            {
                fprintf(stderr, "--ray-tracing: the device has no ray tracing\n");
                exit(1);
            }
        }
        if (shaderObject)
        {
            auto fn = [&](const char* name) { return vkGetDeviceProcAddr(device, name); };
            so.create = (PFN_vkCreateShadersEXT)fn("vkCreateShadersEXT");
            so.destroy = (PFN_vkDestroyShaderEXT)fn("vkDestroyShaderEXT");
            so.bind = (PFN_vkCmdBindShadersEXT)fn("vkCmdBindShadersEXT");
            so.viewport = (PFN_vkCmdSetViewportWithCount)fn("vkCmdSetViewportWithCountEXT");
            so.scissor = (PFN_vkCmdSetScissorWithCount)fn("vkCmdSetScissorWithCountEXT");
            so.rasterizerDiscard = (PFN_vkCmdSetRasterizerDiscardEnable)fn("vkCmdSetRasterizerDiscardEnableEXT");
            so.cull = (PFN_vkCmdSetCullMode)fn("vkCmdSetCullModeEXT");
            so.frontFace = (PFN_vkCmdSetFrontFace)fn("vkCmdSetFrontFaceEXT");
            so.depthTest = (PFN_vkCmdSetDepthTestEnable)fn("vkCmdSetDepthTestEnableEXT");
            so.depthWrite = (PFN_vkCmdSetDepthWriteEnable)fn("vkCmdSetDepthWriteEnableEXT");
            so.depthCompare = (PFN_vkCmdSetDepthCompareOp)fn("vkCmdSetDepthCompareOpEXT");
            so.depthBias = (PFN_vkCmdSetDepthBiasEnable)fn("vkCmdSetDepthBiasEnableEXT");
            so.stencilTest = (PFN_vkCmdSetStencilTestEnable)fn("vkCmdSetStencilTestEnableEXT");
            so.topology = (PFN_vkCmdSetPrimitiveTopology)fn("vkCmdSetPrimitiveTopologyEXT");
            so.primitiveRestart = (PFN_vkCmdSetPrimitiveRestartEnable)fn("vkCmdSetPrimitiveRestartEnableEXT");
            so.vertexInput = (PFN_vkCmdSetVertexInputEXT)fn("vkCmdSetVertexInputEXT");
            so.polygonMode = (PFN_vkCmdSetPolygonModeEXT)fn("vkCmdSetPolygonModeEXT");
            so.samples = (PFN_vkCmdSetRasterizationSamplesEXT)fn("vkCmdSetRasterizationSamplesEXT");
            so.sampleMask = (PFN_vkCmdSetSampleMaskEXT)fn("vkCmdSetSampleMaskEXT");
            so.alphaToCoverage = (PFN_vkCmdSetAlphaToCoverageEnableEXT)fn("vkCmdSetAlphaToCoverageEnableEXT");
            so.blendEnable = (PFN_vkCmdSetColorBlendEnableEXT)fn("vkCmdSetColorBlendEnableEXT");
            so.writeMask = (PFN_vkCmdSetColorWriteMaskEXT)fn("vkCmdSetColorWriteMaskEXT");
            if (!so.create || !so.bind || !so.vertexInput || !so.writeMask || !so.viewport)
            {
                fprintf(stderr, "--shader-object: the device has no VK_EXT_shader_object\n");
                exit(1);
            }
        }
        if (descriptorBuffer)
        {
            auto fn = [&](const char* name) { return vkGetDeviceProcAddr(device, name); };
            db.layoutSize = (PFN_vkGetDescriptorSetLayoutSizeEXT)fn("vkGetDescriptorSetLayoutSizeEXT");
            db.bindingOffset = (PFN_vkGetDescriptorSetLayoutBindingOffsetEXT)fn("vkGetDescriptorSetLayoutBindingOffsetEXT");
            db.getDescriptor = (PFN_vkGetDescriptorEXT)fn("vkGetDescriptorEXT");
            db.bindBuffers = (PFN_vkCmdBindDescriptorBuffersEXT)fn("vkCmdBindDescriptorBuffersEXT");
            db.setOffsets = (PFN_vkCmdSetDescriptorBufferOffsetsEXT)fn("vkCmdSetDescriptorBufferOffsetsEXT");
            if (!db.layoutSize || !db.bindingOffset || !db.getDescriptor || !db.bindBuffers || !db.setOffsets)
            {
                fprintf(stderr, "--descriptor-buffer: the device has no VK_EXT_descriptor_buffer\n");
                exit(1);
            }
            VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            props2.pNext = &db.props;
            vkGetPhysicalDeviceProperties2(gpu, &props2);
        }
        if (pushTemplate)
        {
            pushWithTemplate = (PFN_vkCmdPushDescriptorSetWithTemplateKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetWithTemplateKHR");
            if (!pushWithTemplate)
            {
                fprintf(stderr, "--push-template: the device has no VK_KHR_push_descriptor\n");
                exit(1);
            }
        }

        if (debugUtils)
        {
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
        if (hazard)
            CHECK(vkAllocateCommandBuffers(device, &cbai, hazardBuffers));
        if (suspend)
            CHECK(vkAllocateCommandBuffers(device, &cbai, suspendBuffers));
        for (int i = 0; i < kFramesInFlight; ++i)
        {
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
    void CreateSwapchain(VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE)
    {
        VkSurfaceCapabilitiesKHR caps;
        CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
        uint32_t fn = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fn);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fn, formats.data());
        VkSurfaceFormatKHR chosen = formats[0];
        for (auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
            {
                chosen = f;
                break;
            }
        colorFormat = chosen.format;
        if (caps.currentExtent.width != 0xFFFFFFFF)
        {
            width = caps.currentExtent.width;
            height = caps.currentExtent.height;
        }
        else
        {
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
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (multiview ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0u);
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = oldSwapchain;
        CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain));
        if (oldSwapchain)
            vkDestroySwapchainKHR(device, oldSwapchain, nullptr);

        uint32_t count = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
        swapImages.resize(count);
        vkGetSwapchainImagesKHR(device, swapchain, &count, swapImages.data());
        swapViews.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
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
        ici.arrayLayers = multiview ? 2 : 1;
        ici.samples = samples;
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
        dvci.viewType = multiview ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        dvci.format = depthFormat;
        dvci.subresourceRange = {(VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u), 0, 1, 0, multiview ? 2u : 1u};
        CHECK(vkCreateImageView(device, &dvci, nullptr, &depthView));
        Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)depthImage, "Depth buffer");

        if (multiview)
        {
            // The stereo target: a layer per view, blitted into the swapchain image after the pass.
            VkImageCreateInfo mvci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            mvci.imageType = VK_IMAGE_TYPE_2D;
            mvci.format = colorFormat;
            mvci.extent = {width, height, 1};
            mvci.mipLevels = 1;
            mvci.arrayLayers = 2;
            mvci.samples = VK_SAMPLE_COUNT_1_BIT;
            mvci.tiling = VK_IMAGE_TILING_OPTIMAL;
            mvci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            mvci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            CHECK(vkCreateImage(device, &mvci, nullptr, &stereoImage));
            VkMemoryRequirements sreq;
            vkGetImageMemoryRequirements(device, stereoImage, &sreq);
            VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            smai.allocationSize = sreq.size;
            smai.memoryTypeIndex = FindMemoryType(sreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            CHECK(vkAllocateMemory(device, &smai, nullptr, &stereoMemory));
            CHECK(vkBindImageMemory(device, stereoImage, stereoMemory, 0));
            VkImageViewCreateInfo svci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            svci.image = stereoImage;
            svci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            svci.format = colorFormat;
            svci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
            CHECK(vkCreateImageView(device, &svci, nullptr, &stereoView));
            Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)stereoImage, "Stereo color");
        }

        if (offscreen)
        {
            VkImageCreateInfo oci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            oci.imageType = VK_IMAGE_TYPE_2D;
            oci.format = colorFormat;
            oci.extent = {width, height, 1};
            oci.mipLevels = 1;
            oci.arrayLayers = 1;
            oci.samples = VK_SAMPLE_COUNT_1_BIT;
            oci.tiling = VK_IMAGE_TILING_OPTIMAL;
            oci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            oci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            CHECK(vkCreateImage(device, &oci, nullptr, &offImage));
            VkMemoryRequirements oreq;
            vkGetImageMemoryRequirements(device, offImage, &oreq);
            VkMemoryAllocateInfo omai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            omai.allocationSize = oreq.size;
            omai.memoryTypeIndex = FindMemoryType(oreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            CHECK(vkAllocateMemory(device, &omai, nullptr, &offMemory));
            CHECK(vkBindImageMemory(device, offImage, offMemory, 0));
            VkImageViewCreateInfo ovci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            ovci.image = offImage;
            ovci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ovci.format = colorFormat;
            ovci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            CHECK(vkCreateImageView(device, &ovci, nullptr, &offView));
            Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)offImage, "Offscreen color");
        }
        // Multisampled color target, resolved into the swapchain image by the render pass.
        if (samples != VK_SAMPLE_COUNT_1_BIT)
        {
            VkImageCreateInfo mci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            mci.imageType = VK_IMAGE_TYPE_2D;
            mci.format = colorFormat;
            mci.extent = {width, height, 1};
            mci.mipLevels = 1;
            mci.arrayLayers = 1;
            mci.samples = samples;
            mci.tiling = VK_IMAGE_TILING_OPTIMAL;
            mci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            mci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            CHECK(vkCreateImage(device, &mci, nullptr, &msaaImage));
            VkMemoryRequirements mreq;
            vkGetImageMemoryRequirements(device, msaaImage, &mreq);
            VkMemoryAllocateInfo mmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mmai.allocationSize = mreq.size;
            mmai.memoryTypeIndex = FindMemoryType(mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            CHECK(vkAllocateMemory(device, &mmai, nullptr, &msaaMemory));
            CHECK(vkBindImageMemory(device, msaaImage, msaaMemory, 0));
            VkImageViewCreateInfo mvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            mvci.image = msaaImage;
            mvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            mvci.format = colorFormat;
            mvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            CHECK(vkCreateImageView(device, &mvci, nullptr, &msaaView));
            Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)msaaImage, "MSAA color");
        }
    }

    void CreateRenderPass()
    {
        // Attachment 0 is the color target (the swapchain image, or with --msaa the multisampled
        // image resolved into attachment 2, the swapchain image), 1 the depth buffer.
        const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;
        VkAttachmentDescription atts[3]{};
        atts[0].format = colorFormat;
        atts[0].samples = samples;
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[0].finalLayout = multiview ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            : msaa || offscreen        ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                       : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        atts[2].format = colorFormat;
        atts[2].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[2].finalLayout = offscreen ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        atts[1].format = depthFormat;
        atts[1].samples = samples;
        atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;   // nothing reads the depth buffer after the pass
        atts[1].stencilLoadOp = stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;   // --stencil: the capture has to store it for its read-back
        atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &colorRef;
        sp.pResolveAttachments = msaa ? &resolveRef : nullptr;
        sp.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstStageMask = dep.srcStageMask;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = msaa ? 3 : 2;
        rpci.pAttachments = atts;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sp;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dep;
        // --multiview: both views in the one subpass, each into the layer of its index.
        const uint32_t viewMask = 0b11;
        VkRenderPassMultiviewCreateInfo mv{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
        mv.subpassCount = 1;
        mv.pViewMasks = &viewMask;
        mv.correlationMaskCount = 1;
        mv.pCorrelationMasks = &viewMask;
        if (multiview)
            rpci.pNext = &mv;
        CHECK(vkCreateRenderPass(device, &rpci, nullptr, &renderPass));
    }

    void CreateFramebuffers()
    {
        const uint32_t count = (uint32_t)swapViews.size();
        framebuffers.resize(count);
        if (offscreen)
        {
            const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;
            VkImageView views[] = {msaa ? msaaView : offView, depthView, offView};
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = renderPass;
            fci.attachmentCount = msaa ? 3 : 2;
            fci.pAttachments = views;
            fci.width = width;
            fci.height = height;
            fci.layers = 1;
            CHECK(vkCreateFramebuffer(device, &fci, nullptr, &offFramebuffer));
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;
            VkImageView views[] = {msaa ? msaaView : multiview ? stereoView : swapViews[i], depthView, swapViews[i]};
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = renderPass;
            fci.attachmentCount = msaa ? 3 : 2;
            fci.pAttachments = views;
            fci.width = width;
            fci.height = height;
            fci.layers = 1;
            CHECK(vkCreateFramebuffer(device, &fci, nullptr, &framebuffers[i]));
        }
        if (overlayPass)
        {
            overlayFramebuffers.resize(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                fci.renderPass = overlayPass;
                fci.attachmentCount = 1;
                fci.pAttachments = &swapViews[i];
                fci.width = width;
                fci.height = height;
                fci.layers = 1;
                CHECK(vkCreateFramebuffer(device, &fci, nullptr, &overlayFramebuffers[i]));
            }
        }
        if (prerecord && computePipeline)
            PrerecordAll();
    }

    // --ray-tracing: a buffer with a device address, optionally host visible (mapped).
    DeviceBuffer CreateDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, const char* name)
    {
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
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(device, &mai, nullptr, &b.memory));
        CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        if (host)
            CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
        VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ai.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(device, &ai);
        Name(VK_OBJECT_TYPE_BUFFER, (uint64_t)b.buffer, name);
        return b;
    }

    void DestroyDeviceBuffer(DeviceBuffer& b)
    {
        if (b.buffer)
            vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory)
            vkFreeMemory(device, b.memory, nullptr);
        b = DeviceBuffer{};
    }

    // --ray-tracing: the triangle's bottom-level structure (built once), the top-level structure
    // over it (rebuilt every frame in Record), the output image and the pipeline with its table.
    void CreateRayTracing()
    {
        if (!rayTracing)
            return;
        const float tri[9] = {-0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f};
        rt.vertices = CreateDeviceBuffer(sizeof(tri), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, "RT triangle");
        memcpy(rt.vertices.mapped, tri, sizeof(tri));

        auto makeStructure = [&](VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR& geometry, uint32_t primitives,
                                 DeviceBuffer& storage, VkAccelerationStructureKHR& out, const char* name) -> VkDeviceSize {
            VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            info.type = type;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.geometryCount = 1;
            info.pGeometries = &geometry;
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            rt.buildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);
            storage = CreateDeviceBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, name);
            VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            ci.buffer = storage.buffer;
            ci.size = sizes.accelerationStructureSize;
            ci.type = type;
            CHECK(rt.createAS(device, &ci, nullptr, &out));
            Name(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, (uint64_t)out, name);
            return sizes.buildScratchSize;
        };

        VkAccelerationStructureGeometryKHR triangles{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        triangles.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        triangles.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        triangles.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.geometry.triangles.vertexData.deviceAddress = rt.vertices.address;
        triangles.geometry.triangles.vertexStride = 3 * sizeof(float);
        triangles.geometry.triangles.maxVertex = 2;
        triangles.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        VkDeviceSize blasScratch = makeStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, triangles, 1, rt.blasMemory, rt.blas, "RT triangle BLAS");

        VkAccelerationStructureDeviceAddressInfoKHR addr{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addr.accelerationStructure = rt.blas;
        VkAccelerationStructureInstanceKHR instance{};
        instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1.0f;
        instance.mask = 0xFF;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = rt.asAddress(device, &addr);
        rt.instances = CreateDeviceBuffer(sizeof(instance), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, "RT instances");
        memcpy(rt.instances.mapped, &instance, sizeof(instance));
        VkDeviceSize tlasScratch = makeStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, TlasGeometry(), 1, rt.tlasMemory, rt.tlas, "RT scene TLAS");
        rt.scratch = CreateDeviceBuffer(std::max(blasScratch, tlasScratch), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, "RT scratch");

        // The bottom level once, now.
        VkCommandBuffer cb = BeginOneShot();
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.dstAccelerationStructure = rt.blas;
        info.geometryCount = 1;
        info.pGeometries = &triangles;
        info.scratchData.deviceAddress = rt.scratch.address;
        VkAccelerationStructureBuildRangeInfoKHR range{1, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        rt.build(cb, 1, &info, &ranges);
        EndOneShot(cb);

        // The output: a storage image the ray generation shader writes.
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {256, 256, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        CHECK(vkCreateImage(device, &ici, nullptr, &rt.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, rt.image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(device, &mai, nullptr, &rt.imageMemory));
        CHECK(vkBindImageMemory(device, rt.image, rt.imageMemory, 0));
        Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)rt.image, "RT output");
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = rt.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ici.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CHECK(vkCreateImageView(device, &vci, nullptr, &rt.view));
        cb = BeginOneShot();
        VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = rt.image;
        toGeneral.subresourceRange = vci.subresourceRange;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
        EndOneShot(cb);

        // The scene and the image, for the ray generation shader.
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslci.bindingCount = 2;
        dslci.pBindings = bindings;
        CHECK(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &rt.setLayout));
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 1;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = sizes;
        CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &rt.pool));
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = rt.pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &rt.setLayout;
        CHECK(vkAllocateDescriptorSets(device, &dsai, &rt.set));
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &rt.tlas;
        VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, rt.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].pNext = &asWrite;
        writes[0].dstSet = rt.set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[1].dstSet = rt.set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &rt.setLayout;
        CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &rt.layout));

        // Ray generation, miss and closest hit, in three groups.
        const VkShaderStageFlagBits kinds[3] = {VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
        const char* files[3] = {"rt.rgen.spv", "rt.rmiss.spv", shaderRecord ? "rt_record.rchit.spv" : "rt.rchit.spv"};
        VkPipelineShaderStageCreateInfo stages[3]{};
        for (int i = 0; i < 3; ++i)
        {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = kinds[i];
            stages[i].module = LoadShader(files[i]);
            stages[i].pName = "main";
        }
        VkRayTracingShaderGroupCreateInfoKHR groups[3]{};
        for (int i = 0; i < 3; ++i)
        {
            groups[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            groups[i].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            groups[i].generalShader = groups[i].closestHitShader = groups[i].anyHitShader = groups[i].intersectionShader = VK_SHADER_UNUSED_KHR;
        }
        groups[0].generalShader = 0;
        groups[1].generalShader = 1;
        groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[2].closestHitShader = 2;
        VkRayTracingPipelineCreateInfoKHR rpci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        rpci.stageCount = 3;
        rpci.pStages = stages;
        rpci.groupCount = 3;
        rpci.pGroups = groups;
        rpci.maxPipelineRayRecursionDepth = 1;
        rpci.layout = rt.layout;
        CHECK(rt.createPipelines(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rpci, nullptr, &rt.pipeline));
        Name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)rt.pipeline, "RT pipeline");
        for (auto& s : stages)
            vkDestroyShaderModule(device, s.module, nullptr);

        // The shader binding table: one group per record, aligned as the device asks.
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &props;
        vkGetPhysicalDeviceProperties2(gpu, &props2);
        const uint32_t handle = props.shaderGroupHandleSize;
        // Each region starts at a multiple of shaderGroupBaseAlignment, so the records are that far apart.
        const uint32_t align = std::max(props.shaderGroupHandleAlignment, props.shaderGroupBaseAlignment);
        // --shader-record: after its handle, the hit record holds the tint buffer's address.
        const VkDeviceSize recordData = shaderRecord ? sizeof(VkDeviceAddress) : 0;
        const VkDeviceSize stride = (handle + recordData + align - 1) / align * align;
        std::vector<uint8_t> handles(3 * handle);
        CHECK(rt.groupHandles(device, rt.pipeline, 0, 3, handles.size(), handles.data()));
        rt.sbt = CreateDeviceBuffer(3 * stride, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, true, "RT shader binding table");
        for (int i = 0; i < 3; ++i)
            memcpy(static_cast<uint8_t*>(rt.sbt.mapped) + i * stride, handles.data() + i * handle, handle);
        if (shaderRecord)
        {
            rt.tint = CreateDeviceBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "RT hit record tint");
            const float tint[4] = {1.0f, 0.5f, 0.25f, 1.0f};
            memcpy(rt.tint.mapped, tint, sizeof(tint));
            memcpy(static_cast<uint8_t*>(rt.sbt.mapped) + 2 * stride + handle, &rt.tint.address, sizeof(rt.tint.address));
        }
        rt.raygen = {rt.sbt.address, stride, stride};
        rt.miss = {rt.sbt.address + stride, stride, stride};
        rt.hit = {rt.sbt.address + 2 * stride, stride, stride};
    }

    // The top-level structure's one geometry: the instance buffer.
    VkAccelerationStructureGeometryKHR TlasGeometry()
    {
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        g.geometry.instances.data.deviceAddress = rt.instances.address;
        return g;
    }

    // Each frame: the top level rebuilt, then one trace into the storage image.
    void RecordRayTracing(VkCommandBuffer cb)
    {
        if (!rayTracing)
            return;
        // The bottom level is rebuilt every frame beside the top one, as an engine with deforming
        // geometry does. It also means a capture holds the build of everything it traces against:
        // a bottom level built once before the capture cannot be rebuilt by a replay, which then
        // traces against an empty structure (docs/REPLAY.md). --static-blas leaves it as built at start-up.
        if (!staticBlas)
        {
            VkAccelerationStructureGeometryKHR triangles{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            triangles.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            triangles.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            triangles.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            triangles.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            triangles.geometry.triangles.vertexData.deviceAddress = rt.vertices.address;
            triangles.geometry.triangles.vertexStride = 3 * sizeof(float);
            triangles.geometry.triangles.maxVertex = 2;
            triangles.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
            VkAccelerationStructureBuildGeometryInfoKHR blasInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            blasInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            blasInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            blasInfo.dstAccelerationStructure = rt.blas;
            blasInfo.geometryCount = 1;
            blasInfo.pGeometries = &triangles;
            blasInfo.scratchData.deviceAddress = rt.scratch.address;
            VkAccelerationStructureBuildRangeInfoKHR blasRange{1, 0, 0, 0};
            const VkAccelerationStructureBuildRangeInfoKHR* blasRanges = &blasRange;
            rt.build(cb, 1, &blasInfo, &blasRanges);
            // The top level below reads it, and they share the scratch buffer.
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        }
        VkAccelerationStructureGeometryKHR geometry = TlasGeometry();
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.dstAccelerationStructure = rt.tlas;
        info.geometryCount = 1;
        info.pGeometries = &geometry;
        info.scratchData.deviceAddress = rt.scratch.address;
        VkAccelerationStructureBuildRangeInfoKHR range{1, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        rt.build(cb, 1, &info, &ranges);
        // The trace below reads the structure this build writes, and nothing else orders them.
        // Without this the rays are traced against whatever the top level held when the trace
        // reached it, which on this driver happened to be the finished build often enough to look
        // correct — until a replay ran the same commands with different timing and every ray missed.
        VkMemoryBarrier built{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        built.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        built.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1, &built, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt.layout, 0, 1, &rt.set, 0, nullptr);
        rt.trace(cb, &rt.raygen, &rt.miss, &rt.hit, &rt.callable, 256, 256, 1);
    }

    void DestroyRayTracing()
    {
        if (!rayTracing)
            return;
        vkDestroyPipeline(device, rt.pipeline, nullptr);
        vkDestroyPipelineLayout(device, rt.layout, nullptr);
        vkDestroyDescriptorPool(device, rt.pool, nullptr);
        vkDestroyDescriptorSetLayout(device, rt.setLayout, nullptr);
        vkDestroyImageView(device, rt.view, nullptr);
        vkDestroyImage(device, rt.image, nullptr);
        vkFreeMemory(device, rt.imageMemory, nullptr);
        rt.destroyAS(device, rt.tlas, nullptr);
        rt.destroyAS(device, rt.blas, nullptr);
        for (DeviceBuffer* b : {&rt.vertices, &rt.instances, &rt.blasMemory, &rt.tlasMemory, &rt.scratch, &rt.sbt, &rt.tint})
            DestroyDeviceBuffer(*b);
    }

    // --second-device / --second-queue: the side target, its pass and its command buffer.
    void CreateSide()
    {
        if (side == Side::None)
            return;
        VkDevice d = sideWork.device;
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = queueFamily;
        CHECK(vkCreateCommandPool(d, &cpci, nullptr, &sideWork.pool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = sideWork.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        CHECK(vkAllocateCommandBuffers(d, &cai, &sideWork.cb));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CHECK(vkCreateFence(d, &fci, nullptr, &sideWork.fence));

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {256, 256, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        CHECK(vkCreateImage(d, &ici, nullptr, &sideWork.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d, sideWork.image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(d, &mai, nullptr, &sideWork.memory));
        CHECK(vkBindImageMemory(d, sideWork.image, sideWork.memory, 0));
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = sideWork.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ici.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CHECK(vkCreateImageView(d, &vci, nullptr, &sideWork.view));

        VkAttachmentDescription att{};
        att.format = ici.format;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sp;
        CHECK(vkCreateRenderPass(d, &rpci, nullptr, &sideWork.pass));
        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass = sideWork.pass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &sideWork.view;
        fbci.width = 256;
        fbci.height = 256;
        fbci.layers = 1;
        CHECK(vkCreateFramebuffer(d, &fbci, nullptr, &sideWork.framebuffer));
    }

    // One frame of side work: the target cleared to a color that follows the time.
    void DrawSide(float t)
    {
        if (side == Side::None)
            return;
        VkDevice d = sideWork.device;
        CHECK(vkWaitForFences(d, 1, &sideWork.fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(d, 1, &sideWork.fence));
        CHECK(vkResetCommandBuffer(sideWork.cb, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(vkBeginCommandBuffer(sideWork.cb, &bi));
        VkClearValue clear{};
        clear.color = {{0.2f, 0.5f + 0.5f * sinf(t), 0.3f, 1.0f}};
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = sideWork.pass;
        rpbi.framebuffer = sideWork.framebuffer;
        rpbi.renderArea = {{0, 0}, {256, 256}};
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &clear;
        vkCmdBeginRenderPass(sideWork.cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(sideWork.cb);
        CHECK(vkEndCommandBuffer(sideWork.cb));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &sideWork.cb;
        CHECK(vkQueueSubmit(sideWork.queue, 1, &si, sideWork.fence));
    }

    void DestroySide()
    {
        if (side == Side::None)
            return;
        VkDevice d = sideWork.device;
        vkDeviceWaitIdle(d);
        vkDestroyFramebuffer(d, sideWork.framebuffer, nullptr);
        vkDestroyRenderPass(d, sideWork.pass, nullptr);
        vkDestroyImageView(d, sideWork.view, nullptr);
        vkDestroyImage(d, sideWork.image, nullptr);
        vkFreeMemory(d, sideWork.memory, nullptr);
        vkDestroyFence(d, sideWork.fence, nullptr);
        vkDestroyCommandPool(d, sideWork.pool, nullptr);
        if (side == Side::Device)
            vkDestroyDevice(d, nullptr);
    }

    // --prerecord: a pass that loads the presented image and clears its left half.
    void CreateOverlayPass()
    {
        if (offscreen || samples != VK_SAMPLE_COUNT_1_BIT)
            return;
        VkAttachmentDescription att{};
        att.format = colorFormat;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sp;
        CHECK(vkCreateRenderPass(device, &rpci, nullptr, &overlayPass));
        Name(VK_OBJECT_TYPE_RENDER_PASS, (uint64_t)overlayPass, "Overlay");
    }

    void RecordOverlay(VkCommandBuffer cb, uint32_t imageIndex)
    {
        CHECK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        CHECK(vkBeginCommandBuffer(cb, &bi));
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = overlayPass;
        rpbi.framebuffer = overlayFramebuffers[imageIndex];
        rpbi.renderArea = {{0, 0}, {width, height}};
        vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.colorAttachment = 0;
        clear.clearValue.color = {{0.8f, 0.1f, 0.6f, 1.0f}};
        VkClearRect rect{{{0, 0}, {width / 2, height}}, 0, 1};
        vkCmdClearAttachments(cb, 1, &clear, 1, &rect);
        vkCmdEndRenderPass(cb);
        CHECK(vkEndCommandBuffer(cb));
    }

    // --prerecord: one command buffer per swapchain image, recorded now and resubmitted as is.
    void PrerecordAll()
    {
        const uint32_t count = (uint32_t)framebuffers.size();
        if (prerecorded.size() != count)
        {
            if (!prerecorded.empty())
                vkFreeCommandBuffers(device, commandPool, (uint32_t)prerecorded.size(), prerecorded.data());
            prerecorded.assign(count, VK_NULL_HANDLE);
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = commandPool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = count;
            CHECK(vkAllocateCommandBuffers(device, &cai, prerecorded.data()));
        }
        for (uint32_t i = 0; i < count; ++i)
            Record(prerecorded[i], i, 0.0f);
        if (!overlayPass)
            return;
        if (overlays.size() != count)
        {
            if (!overlays.empty())
                vkFreeCommandBuffers(device, commandPool, (uint32_t)overlays.size(), overlays.data());
            overlays.assign(count, VK_NULL_HANDLE);
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = commandPool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = count;
            CHECK(vkAllocateCommandBuffers(device, &cai, overlays.data()));
        }
        for (uint32_t i = 0; i < count; ++i)
            RecordOverlay(overlays[i], i);
    }

    // Everything sized by the window, except the swapchain itself (see CreateSwapchain).
    void DestroySwapchainResources()
    {
        for (auto fb : framebuffers)
            vkDestroyFramebuffer(device, fb, nullptr);
        framebuffers.clear();
        for (auto fb : overlayFramebuffers)
            vkDestroyFramebuffer(device, fb, nullptr);
        overlayFramebuffers.clear();
        vkDestroyImageView(device, depthView, nullptr);
        vkDestroyImage(device, depthImage, nullptr);
        vkFreeMemory(device, depthMemory, nullptr);
        depthView = VK_NULL_HANDLE;
        depthImage = VK_NULL_HANDLE;
        depthMemory = VK_NULL_HANDLE;
        if (stereoView)
            vkDestroyImageView(device, stereoView, nullptr);
        if (stereoImage)
            vkDestroyImage(device, stereoImage, nullptr);
        if (stereoMemory)
            vkFreeMemory(device, stereoMemory, nullptr);
        stereoView = VK_NULL_HANDLE;
        stereoImage = VK_NULL_HANDLE;
        stereoMemory = VK_NULL_HANDLE;
        if (msaaView)
            vkDestroyImageView(device, msaaView, nullptr);
        if (msaaImage)
            vkDestroyImage(device, msaaImage, nullptr);
        if (msaaMemory)
            vkFreeMemory(device, msaaMemory, nullptr);
        msaaView = VK_NULL_HANDLE;
        msaaImage = VK_NULL_HANDLE;
        msaaMemory = VK_NULL_HANDLE;
        if (offFramebuffer)
            vkDestroyFramebuffer(device, offFramebuffer, nullptr);
        if (offView)
            vkDestroyImageView(device, offView, nullptr);
        if (offImage)
            vkDestroyImage(device, offImage, nullptr);
        if (offMemory)
            vkFreeMemory(device, offMemory, nullptr);
        offFramebuffer = VK_NULL_HANDLE;
        offView = VK_NULL_HANDLE;
        offImage = VK_NULL_HANDLE;
        offMemory = VK_NULL_HANDLE;
        for (auto v : swapViews)
            vkDestroyImageView(device, v, nullptr);
        swapViews.clear();
        swapImages.clear();
    }

    // Returns false while the window has no drawable area (minimized); try again later.
    bool RecreateSwapchain()
    {
        VkSurfaceCapabilitiesKHR caps;
        CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
        if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0)
            return false;
        if (caps.currentExtent.width == 0xFFFFFFFF && (width == 0 || height == 0))
            return false;
        vkDeviceWaitIdle(device);
        DestroySwapchainResources();
        CreateSwapchain(swapchain);
        CreateFramebuffers();
        resized = false;
        return true;
    }

    void CreateResources()
    {
        // Cube geometry
        const float p = 0.5f;
        Vertex verts[24];
        uint16_t indices[36];
        const float faces[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const float colors[6][3] = {{1, 0.3f, 0.3f}, {0.3f, 1, 0.3f}, {0.3f, 0.3f, 1}, {1, 1, 0.3f}, {1, 0.3f, 1}, {0.3f, 1, 1}};
        int v = 0, ix = 0;
        for (int f = 0; f < 6; ++f)
        {
            const float* n = faces[f];
            float u[3] = {n[1], n[2], n[0]};
            float w[3] = {n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
            for (int c = 0; c < 4; ++c)
            {
                float su = (c == 1 || c == 2) ? 1.f : -1.f;
                float sv = (c >= 2) ? 1.f : -1.f;
                for (int k = 0; k < 3; ++k)
                    verts[v].pos[k] = p * (n[k] + su * u[k] + sv * w[k]);
                memcpy(verts[v].color, colors[f], sizeof(verts[v].color));
                verts[v].uv[0] = su * 0.5f + 0.5f;
                verts[v].uv[1] = sv * 0.5f + 0.5f;
                ++v;
            }
            uint16_t b = (uint16_t)(f * 4);
            uint16_t quad[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
            for (int k = 0; k < 6; ++k)
                indices[ix++] = quad[k];
        }
        VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        CreateBuffer(sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (hazard ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0), host, vertexBuffer, vertexMemory, "Cube vertices");
        CreateBuffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host, indexBuffer, indexMemory, "Cube indices");
        CreateBuffer(sizeof(Mat4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, uniformBuffer, uniformMemory, "Cube uniforms",
            descriptorBuffer);
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
            for (uint32_t x = 0; x < ts; ++x)
            {
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
        ici.mipLevels = kTextureMips;   // a mip chain, blitted from level 0 (the inspector reads every mip back)
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kTextureMips, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {ts, ts, 1};
        vkCmdCopyBufferToImage(upload, staging, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        // Each level is a blit of the previous one (level i-1 goes to TRANSFER_SRC first).
        for (uint32_t level = 1; level < kTextureMips; ++level)
        {
            VkImageMemoryBarrier toSrc = b;
            toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, 1};
            vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
            blit.srcOffsets[1] = {(int32_t)std::max(1u, ts >> (level - 1)), (int32_t)std::max(1u, ts >> (level - 1)), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            blit.dstOffsets[1] = {(int32_t)std::max(1u, ts >> level), (int32_t)std::max(1u, ts >> level), 1};
            vkCmdBlitImage(upload, texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }
        // Levels 0..n-2 are TRANSFER_SRC now, the last one TRANSFER_DST: all to shader read.
        VkImageMemoryBarrier toRead[2] = {b, b};
        toRead[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kTextureMips - 1, 0, 1};
        toRead[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, kTextureMips - 1, 1, 0, 1};
        for (auto& t : toRead)
        {
            t.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            t.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            t.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, toRead);
        EndOneShot(upload);
        vkDestroyBuffer(device, staging, nullptr);
        vkFreeMemory(device, stagingMem, nullptr);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = texture;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kTextureMips, 0, 1};
        CHECK(vkCreateImageView(device, &vci, nullptr, &textureView));
        VkSamplerCreateInfo smci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        smci.magFilter = VK_FILTER_NEAREST;
        smci.minFilter = VK_FILTER_NEAREST;
        smci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        smci.addressModeU = smci.addressModeV = smci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        smci.maxLod = (float)kTextureMips;
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
        if (pushTemplate)
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        if (descriptorBuffer)
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
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
        VkDescriptorBufferInfo dbi{uniformBuffer, 0, sizeof(Mat4)};
        VkDescriptorImageInfo dii{sampler, textureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (pushTemplate)
        {
            pushData.uniform = dbi;
            pushData.texture = dii;
            VkDescriptorUpdateTemplateEntry entries[2]{};
            entries[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, offsetof(PushData, uniform), sizeof(VkDescriptorBufferInfo)};
            entries[1] = {1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, offsetof(PushData, texture), sizeof(VkDescriptorImageInfo)};
            VkDescriptorUpdateTemplateCreateInfo tci{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
            tci.descriptorUpdateEntryCount = 2;
            tci.pDescriptorUpdateEntries = entries;
            tci.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
            tci.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            tci.pipelineLayout = pipelineLayout;
            tci.set = 0;
            CHECK(vkCreateDescriptorUpdateTemplate(device, &tci, nullptr, &pushUpdateTemplate));
        }
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = descriptorPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &setLayout;
        if (!pushTemplate && !descriptorBuffer)
            CHECK(vkAllocateDescriptorSets(device, &dsai, &descriptorSet));
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
        if (!pushTemplate && !descriptorBuffer)
            vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        if (descriptorBuffer)
        {
            // The set's bytes, laid out as the driver wants them: its size and the offset of each
            // binding come from the layout, and each descriptor itself from vkGetDescriptorEXT,
            // which is the only way to make one.
            VkDeviceSize size = 0;
            db.layoutSize(device, setLayout, &size);
            CreateBuffer(size,
                VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                    (deviceLocalDescriptors ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0),
                deviceLocalDescriptors ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : host, db.buffer, db.memory, "Cube descriptors", true);
            // The descriptors are written where the host can reach: the buffer itself, or a staging
            // buffer copied into it below.
            VkBuffer staging = VK_NULL_HANDLE;
            VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
            void* target = nullptr;
            if (deviceLocalDescriptors)
            {
                CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, staging, stagingMemory, "Cube descriptor staging");
                CHECK(vkMapMemory(device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &target));
            }
            else
            {
                CHECK(vkMapMemory(device, db.memory, 0, VK_WHOLE_SIZE, 0, &db.mapped));
                target = db.mapped;
            }
            VkBufferDeviceAddressInfo bdai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            bdai.buffer = db.buffer;
            db.address = vkGetBufferDeviceAddress(device, &bdai);
            bdai.buffer = uniformBuffer;
            VkDescriptorAddressInfoEXT uniform{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
            uniform.address = vkGetBufferDeviceAddress(device, &bdai);
            uniform.range = sizeof(Mat4);
            uniform.format = VK_FORMAT_UNDEFINED;

            VkDeviceSize at = 0;
            VkDescriptorGetInfoEXT get{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            get.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            get.data.pUniformBuffer = &uniform;
            db.bindingOffset(device, setLayout, 0, &at);
            db.getDescriptor(device, &get, db.props.uniformBufferDescriptorSize, (char*)target + at);

            get.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            get.data.pCombinedImageSampler = &dii;
            db.bindingOffset(device, setLayout, 1, &at);
            db.getDescriptor(device, &get, db.props.combinedImageSamplerDescriptorSize, (char*)target + at);
            if (deviceLocalDescriptors)
            {
                VkCommandBuffer upload = BeginOneShot();
                const VkBufferCopy region{0, 0, size};
                vkCmdCopyBuffer(upload, staging, db.buffer, 1, &region);
                EndOneShot(upload);
                vkUnmapMemory(device, stagingMemory);
                vkDestroyBuffer(device, staging, nullptr);
                vkFreeMemory(device, stagingMemory, nullptr);
            }
        }

        // Pipeline
        VkShaderModule vs = LoadShader(multiview ? "cube_mv.vert.spv" : "cube.vert.spv");
        VkShaderModule fs = LoadShader(heavy ? "heavy.frag.spv" : alphaTest ? "alpha.frag.spv" : "cube.frag.spv");
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
        rs.cullMode = noCull ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        rs.frontFace = insideOut ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = samples;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        if (stencil)
        {
            // Every fragment that passes writes 1 into the stencil buffer.
            VkStencilOpState op{};
            op.failOp = op.depthFailOp = VK_STENCIL_OP_KEEP;
            op.passOp = VK_STENCIL_OP_REPLACE;
            op.compareOp = VK_COMPARE_OP_ALWAYS;
            op.compareMask = op.writeMask = 0xFF;
            op.reference = 1;
            ds.stencilTestEnable = VK_TRUE;
            ds.front = ds.back = op;
        }
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
        if (descriptorBuffer)
            gpci.flags |= VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        // --suspend draws with this pipeline in dynamic rendering: the targets' formats stand in
        // for the render pass.
        VkPipelineRenderingCreateInfo dynamicTargets{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        dynamicTargets.colorAttachmentCount = 1;
        dynamicTargets.pColorAttachmentFormats = &colorFormat;
        dynamicTargets.depthAttachmentFormat = depthFormat;
        dynamicTargets.viewMask = multiview ? 0b11 : 0;
        if (suspend || mixed || dynamicRenderingPass)
        {
            gpci.pNext = &dynamicTargets;
            gpci.renderPass = VK_NULL_HANDLE;
        }
        if (pipelineLibrary)
        {
            // Library 1: vertex input interface and pre-rasterization shaders (the vertex stage).
            VkGraphicsPipelineLibraryCreateInfoEXT vertexParts{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
            vertexParts.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
            VkGraphicsPipelineCreateInfo vlib{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            vlib.pNext = &vertexParts;
            vlib.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
            vlib.stageCount = 1;
            vlib.pStages = &stages[0];
            vlib.pVertexInputState = &vi;
            vlib.pInputAssemblyState = &ia;
            vlib.pViewportState = &vp;
            vlib.pRasterizationState = &rs;
            vlib.pDynamicState = &dsci;
            vlib.layout = pipelineLayout;
            vlib.renderPass = renderPass;
            // Library 2: the fragment shader and the fragment output interface.
            VkGraphicsPipelineLibraryCreateInfoEXT fragmentParts{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
            fragmentParts.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
            VkGraphicsPipelineCreateInfo flib{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            flib.pNext = &fragmentParts;
            flib.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
            flib.stageCount = 1;
            flib.pStages = &stages[1];
            flib.pMultisampleState = &ms;
            flib.pDepthStencilState = &ds;
            flib.pColorBlendState = &blend;
            flib.layout = pipelineLayout;
            flib.renderPass = renderPass;
            CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &vlib, nullptr, &pipelineLibraries[0]));
            CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &flib, nullptr, &pipelineLibraries[1]));
            Name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)pipelineLibraries[0], "Cube vertex library");
            Name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)pipelineLibraries[1], "Cube fragment library");
            VkPipelineLibraryCreateInfoKHR link{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
            link.libraryCount = 2;
            link.pLibraries = pipelineLibraries;
            VkGraphicsPipelineCreateInfo linked{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            linked.pNext = &link;
            linked.layout = pipelineLayout;
            linked.renderPass = renderPass;
            CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &linked, nullptr, &pipeline));
        }
        else
        {
            CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline));
        }
        Name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)pipeline, "Cube pipeline");
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
        if (shaderObject)
        {
            // The same code as linked shader objects, with the pipeline layout's set layout and push constants.
            std::vector<char> vcode = ReadFile(ExeDir() + (multiview ? "cube_mv.vert.spv" : "cube.vert.spv"));
            std::vector<char> fcode = ReadFile(ExeDir() + (heavy ? "heavy.frag.spv" : alphaTest ? "alpha.frag.spv" : "cube.frag.spv"));
            VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
            VkShaderCreateInfoEXT sci[2]{};
            for (int i = 0; i < 2; ++i)
            {
                sci[i].sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
                sci[i].flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
                sci[i].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
                sci[i].pName = "main";
                sci[i].setLayoutCount = 1;
                sci[i].pSetLayouts = &setLayout;
                sci[i].pushConstantRangeCount = 1;
                sci[i].pPushConstantRanges = &range;
            }
            sci[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            sci[0].nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
            sci[0].codeSize = vcode.size();
            sci[0].pCode = vcode.data();
            sci[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            sci[1].codeSize = fcode.size();
            sci[1].pCode = fcode.data();
            CHECK(so.create(device, 2, sci, nullptr, shaders));
            Name(VK_OBJECT_TYPE_SHADER_EXT, (uint64_t)shaders[0], "Cube vertex shader");
            Name(VK_OBJECT_TYPE_SHADER_EXT, (uint64_t)shaders[1], "Cube fragment shader");
        }
    }

    void CreatePersistImage(PersistImage& p, uint32_t mips, const char* name)
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {kPersistSize, kPersistSize, 1};
        ici.mipLevels = mips;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        CHECK(vkCreateImage(device, &ici, nullptr, &p.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, p.image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(device, &mai, nullptr, &p.memory));
        CHECK(vkBindImageMemory(device, p.image, p.memory, 0));
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = p.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ici.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CHECK(vkCreateImageView(device, &vci, nullptr, &p.view));
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = persistPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &p.view;
        fci.width = fci.height = kPersistSize;
        fci.layers = 1;
        CHECK(vkCreateFramebuffer(device, &fci, nullptr, &p.framebuffer));
        Name(VK_OBJECT_TYPE_IMAGE, (uint64_t)p.image, name);
    }

    void CreatePersistent()
    {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sp;
        CHECK(vkCreateRenderPass(device, &rpci, nullptr, &persistPass));
        CreatePersistImage(trail, 2, "Persistent trail");
        CreatePersistImage(source, 1, "Persistent source");
        CreatePersistImage(copy, 1, "Persistent copy");
        VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (int i = 0; i < kFramesInFlight; ++i)
        {
            CreateBuffer(kStagingSize * kStagingSize * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, persistStaging[i], persistStagingMemory[i], "Persistent staging");
            CHECK(vkMapMemory(device, persistStagingMemory[i], 0, VK_WHOLE_SIZE, 0, &persistStagingMapped[i]));
        }

        // Every first mip GENERAL and cleared; the trail's second mip stays TRANSFER_SRC between frames.
        VkCommandBuffer cb = BeginOneShot();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier barriers[4] = {b, b, b, b};
        barriers[0].image = trail.image;
        barriers[1].image = source.image;
        barriers[2].image = copy.image;
        barriers[3].image = trail.image;
        for (VkImageMemoryBarrier& c : barriers)
            c.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[3].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[3].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[3].subresourceRange.baseMipLevel = 1;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 4, barriers);
        const VkClearColorValue dark{{0.05f, 0.05f, 0.08f, 1.0f}};
        for (VkImage image : {trail.image, source.image, copy.image})
            vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_GENERAL, &dark, 1, &b.subresourceRange);
        EndOneShot(cb);
    }

    // An access of the first mip of a persistent image ending and the next beginning, in its layout
    // (synchronization validation wants each change of use marked).
    struct Use
    {
        VkAccessFlags access;
        VkPipelineStageFlags stage;
    };
    static constexpr Use kTransferRead{VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    static constexpr Use kTransferWrite{VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    static constexpr Use kAttachment{VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    void Hand(VkCommandBuffer cb, VkImage image, Use from, Use to)
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = from.access;
        b.dstAccessMask = to.access;
        vkCmdPipelineBarrier(cb, from.stage, to.stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    void ClearRect(VkCommandBuffer cb, const PersistImage& p, int32_t x, int32_t y, uint32_t size, uint64_t n)
    {
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = persistPass;
        rpbi.framebuffer = p.framebuffer;
        rpbi.renderArea = {{0, 0}, {kPersistSize, kPersistSize}};
        vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0};
        clear.clearValue.color.float32[0] = (float)((n * 37) & 255) / 255.0f;
        clear.clearValue.color.float32[1] = (float)((n * 91) & 255) / 255.0f;
        clear.clearValue.color.float32[2] = (float)((n * 53) & 255) / 255.0f;
        clear.clearValue.color.float32[3] = 1.0f;
        VkClearRect rect{{{x, y}, {size, size}}, 0, 1};
        vkCmdClearAttachments(cb, 1, &clear, 1, &rect);
        vkCmdEndRenderPass(cb);
    }

    // --persistent: before anything writes them, the frame reads what earlier frames left in
    //  - the source image, copied whole into the copy image (a transfer read);
    //  - a staging buffer the host writes every frame, copied into part of the copy image;
    //  - the trail and source images, which render passes load and add a square to;
    //  - the trail's second mip, blitted from the first, which stays TRANSFER_SRC between frames
    //    while the first mip is GENERAL (initial layouts differ per subresource).
    void RecordPersistent(VkCommandBuffer cb)
    {
        const uint64_t n = frameCount;
        auto* texels = static_cast<uint8_t*>(persistStagingMapped[frameSlot]);
        for (uint32_t y = 0; y < kStagingSize; ++y)
            for (uint32_t x = 0; x < kStagingSize; ++x)
            {
                uint8_t* px = &texels[(y * kStagingSize + x) * 4];
                const bool on = ((x / 4 + y / 4 + n) & 1) != 0;
                px[0] = on ? (uint8_t)(n * 29) : 20;
                px[1] = on ? 200 : (uint8_t)(n * 13);
                px[2] = 60;
                px[3] = 255;
            }
        // Where the previous frame left them: source and copy rendered to, the trail's first mip blitted from.
        VkImageCopy whole{};
        whole.srcSubresource = whole.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        whole.extent = {kPersistSize, kPersistSize, 1};
        Hand(cb, source.image, kAttachment, kTransferRead);
        Hand(cb, copy.image, kAttachment, kTransferWrite);
        vkCmdCopyImage(cb, source.image, VK_IMAGE_LAYOUT_GENERAL, copy.image, VK_IMAGE_LAYOUT_GENERAL, 1, &whole);
        VkBufferImageCopy upload{};
        upload.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        upload.imageOffset = {24, 24, 0};
        upload.imageExtent = {kStagingSize, kStagingSize, 1};
        Hand(cb, copy.image, kTransferWrite, kTransferWrite);
        vkCmdCopyBufferToImage(cb, persistStaging[frameSlot], copy.image, VK_IMAGE_LAYOUT_GENERAL, 1, &upload);

        Hand(cb, trail.image, kTransferRead, kAttachment);
        ClearRect(cb, trail, (int32_t)((n * 3) % 56), (int32_t)((n * 7) % 56), 8, n);
        Hand(cb, trail.image, kAttachment, kTransferRead);
        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = trail.image;
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 0, 1};
        toDst.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t)kPersistSize, (int32_t)kPersistSize, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
        blit.dstOffsets[1] = {(int32_t)kPersistSize / 2, (int32_t)kPersistSize / 2, 1};
        vkCmdBlitImage(cb, trail.image, VK_IMAGE_LAYOUT_GENERAL, trail.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        VkImageMemoryBarrier toSrc = toDst;
        std::swap(toSrc.oldLayout, toSrc.newLayout);
        std::swap(toSrc.srcAccessMask, toSrc.dstAccessMask);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

        Hand(cb, source.image, kTransferRead, kAttachment);
        ClearRect(cb, source, (int32_t)((n * 5) % 60), (int32_t)((n * 2) % 60), 4, n + 7);
        Hand(cb, copy.image, kTransferWrite, kAttachment);
        ClearRect(cb, copy, 4, 4, 6, n + 13);
    }

    // Records one frame's commands: the compute pass, then the cube in the main pass.
    void Record(VkCommandBuffer cb, uint32_t imageIndex, float t)
    {
        CHECK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        // Prerecorded buffers may be resubmitted while a previous submission is still pending.
        bi.flags = prerecord ? VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT : VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(vkBeginCommandBuffer(cb, &bi));
        if (persistent && !prerecord)
            RecordPersistent(cb);

        // Compute first: two dispatches refreshing the wave buffer, then a barrier. The inspector
        // times the run as one compute pass.
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout, 0, 1, &computeSet, 0, nullptr);
        // --oob: tells the shader there are four times as many elements as the buffer holds and
        // dispatches for them, so it writes past the end. Nothing on the CPU can see that: the
        // index is computed on the GPU, and only GPU-assisted validation reports it.
        const uint32_t waveCount = outOfBounds ? kWaveCount * 4 : kWaveCount;
        struct
        {
            float time;
            uint32_t count;
        } wavePush{t, waveCount};
        vkCmdPushConstants(cb, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(wavePush), &wavePush);
        vkCmdDispatch(cb, waveCount / 64, 1, 1);
        vkCmdDispatch(cb, waveCount / 64, 1, 1);
        VkMemoryBarrier waveBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        waveBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        waveBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &waveBarrier, 0, nullptr, 0, nullptr);

        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = "Main Pass";
        label.color[0] = 0.2f;
        label.color[1] = 0.6f;
        label.color[2] = 1.0f;
        label.color[3] = 1.0f;
        RecordRayTracing(cb);
        if (beginLabel)
            beginLabel(cb, &label);

        VkClearValue clears[2]{};
        clears[0].color = {{0.1f, 0.1f, 0.15f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = renderPass;
        rpbi.framebuffer = offscreen ? offFramebuffer : framebuffers[imageIndex];
        rpbi.renderArea = {{0, 0}, {width, height}};
        rpbi.clearValueCount = 2;
        rpbi.pClearValues = clears;
        if (hazard)
        {
            // The first vertex wiggles, written by a submission of its own that nothing waits
            // for before the draw below reads the buffer (see --hazard).
            VkCommandBuffer hb = hazardBuffers[frameSlot];
            CHECK(vkBeginCommandBuffer(hb, &bi));
            const float wiggle[3] = {-0.5f + 0.1f * sinf(t * 3.0f), -0.5f, -0.5f};
            vkCmdUpdateBuffer(hb, vertexBuffer, 0, sizeof(wiggle), wiggle);
            CHECK(vkEndCommandBuffer(hb));
            VkSubmitInfo hsi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            hsi.commandBufferCount = 1;
            hsi.pCommandBuffers = &hb;
            CHECK(vkQueueSubmit(queue, 1, &hsi, VK_NULL_HANDLE));
        }
        // Shader objects can only draw in dynamic rendering, and a pass is only suspended there:
        // the same targets and clears, with the layout transitions the render pass would have made.
        const bool dynamic = shaderObject || suspend || dynamicRenderingPass;
        const VkImageView colorView = multiview ? stereoView : offscreen ? offView : swapViews[imageIndex];
        const VkImage colorImage = multiview ? stereoImage : offscreen ? offImage : swapImages[imageIndex];
        const uint32_t layers = multiview ? 2 : 1;
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        if (dynamic)
        {
            VkImageMemoryBarrier toTargets[2]{};
            for (auto& b : toTargets)
            {
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
            toTargets[0].image = colorImage;
            toTargets[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toTargets[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            toTargets[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
            toTargets[1].image = depthImage;
            toTargets[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            toTargets[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            toTargets[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                0, 0, nullptr, 0, nullptr, 2, toTargets);
            color.imageView = colorView;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue = clears[0];
            depth.imageView = depthView;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth.clearValue = clears[1];
            ri.renderArea = {{0, 0}, {width, height}};
            ri.layerCount = 1;
            ri.viewMask = multiview ? 0b11 : 0;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &color;
            ri.pDepthAttachment = &depth;
            if (suspend)
                ri.flags = VK_RENDERING_SUSPENDING_BIT;
            vkCmdBeginRendering(cb, &ri);
        }
        else
        {
            vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        }
        VkViewport viewport{0, 0, (float)width, (float)height, 0, 1};
        // --bad-scissor: a negative offset is a validation error (VUID-vkCmdSetScissor-x-00595),
        // used to exercise the inspector's validation message reporting.
        VkRect2D scissor{{badScissor ? -1 : 0, 0}, {halfScissor ? width / 2 : width, height}};
        RecordCubeDraw(cb, viewport, scissor, t);
        if (suspend)
        {
            // The pass is suspended with the frame's buffer, and resumed in the second buffer with
            // the same rendering info, where the cube is drawn again (in place, so the depth test
            // rejects it), before the pass ends for good. The debug label spans both.
            vkCmdEndRendering(cb);
            CHECK(vkEndCommandBuffer(cb));
            cb = suspendBuffers[frameSlot];
            CHECK(vkResetCommandBuffer(cb, 0));
            CHECK(vkBeginCommandBuffer(cb, &bi));
            ri.flags = VK_RENDERING_RESUMING_BIT;
            vkCmdBeginRendering(cb, &ri);
            RecordCubeDraw(cb, viewport, scissor, t);
        }
        if (dynamic)
        {
            vkCmdEndRendering(cb);
            VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toPresent.srcQueueFamilyIndex = toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toPresent.image = colorImage;
            toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            // The stereo target is blitted next, as the render pass leaves it for.
            toPresent.newLayout = multiview ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                : offscreen                 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                            : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            toPresent.dstAccessMask = multiview ? VK_ACCESS_TRANSFER_READ_BIT : 0;
            toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                multiview ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);
        }
        else
        {
            vkCmdEndRenderPass(cb);
        }
        if (multiview)
            BlitStereo(cb, imageIndex);
        if (endLabel)
            endLabel(cb);
        CHECK(vkEndCommandBuffer(cb));
    }

    // --multiview: the stereo target's two layers side by side in the swapchain image, the left view
    // on the left, and the swapchain image then ready to present.
    void BlitStereo(VkCommandBuffer cb, uint32_t imageIndex)
    {
        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = swapImages[imageIndex];
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
        VkImageBlit blits[2]{};
        for (uint32_t v = 0; v < 2; ++v)
        {
            blits[v].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, v, 1};
            blits[v].srcOffsets[1] = {(int32_t)width, (int32_t)height, 1};
            blits[v].dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blits[v].dstOffsets[0] = {(int32_t)(v * width / 2), 0, 0};
            blits[v].dstOffsets[1] = {(int32_t)((v + 1) * width / 2), (int32_t)height, 1};
        }
        vkCmdBlitImage(cb, stereoImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            2, blits, VK_FILTER_LINEAR);
        VkImageMemoryBarrier toPresent = toDst;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);
    }

    // The cube's draw inside the main pass: its pipeline or shader objects, state, bindings and the draw
    // (a second, identical draw with --occluded).
    void RecordCubeDraw(VkCommandBuffer cb, const VkViewport& viewport, const VkRect2D& scissor, float t)
    {
        if (shaderObject)
        {
            // Shader objects: the shaders, and all the state a pipeline would have carried.
            const VkShaderStageFlagBits stageBits[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
            so.bind(cb, 2, stageBits, shaders);
            so.viewport(cb, 1, &viewport);
            so.scissor(cb, 1, &scissor);
            so.rasterizerDiscard(cb, VK_FALSE);
            so.cull(cb, noCull ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            so.frontFace(cb, insideOut ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
            so.depthTest(cb, VK_TRUE);
            so.depthWrite(cb, VK_TRUE);
            so.depthCompare(cb, VK_COMPARE_OP_LESS);
            so.depthBias(cb, VK_FALSE);
            so.stencilTest(cb, VK_FALSE);
            so.topology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            so.primitiveRestart(cb, VK_FALSE);
            VkVertexInputBindingDescription2EXT binding{VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
            binding.binding = 0;
            binding.stride = sizeof(Vertex);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            binding.divisor = 1;
            VkVertexInputAttributeDescription2EXT attrs[3]{};
            const VkFormat formats[3] = {VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32_SFLOAT};
            const uint32_t offsets[3] = {offsetof(Vertex, pos), offsetof(Vertex, color), offsetof(Vertex, uv)};
            for (uint32_t i = 0; i < 3; ++i)
            {
                attrs[i].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
                attrs[i].location = i;
                attrs[i].format = formats[i];
                attrs[i].offset = offsets[i];
            }
            so.vertexInput(cb, 1, &binding, 3, attrs);
            so.polygonMode(cb, VK_POLYGON_MODE_FILL);
            so.samples(cb, samples);
            const VkSampleMask mask = 0xFFFFFFFF;
            so.sampleMask(cb, samples, &mask);
            so.alphaToCoverage(cb, VK_FALSE);
            const VkBool32 blendOff = VK_FALSE;
            so.blendEnable(cb, 0, 1, &blendOff);
            const VkColorComponentFlags all = 0xF;
            so.writeMask(cb, 0, 1, &all);
        }
        else
        {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
        }
        if (pushTemplate)
            pushWithTemplate(cb, pushUpdateTemplate, pipelineLayout, 0, &pushData);
        else if (descriptorBuffer)
        {
            VkDescriptorBufferBindingInfoEXT binding{VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
            binding.address = db.address;
            binding.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
            db.bindBuffers(cb, 1, &binding);
            const uint32_t bufferIndex = 0;
            const VkDeviceSize setOffset = 0;
            db.setOffsets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &bufferIndex, &setOffset);
        }
        else
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cb, indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        float tint = 0.5f + 0.5f * sinf(t);
        vkCmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &tint);
        vkCmdDrawIndexed(cb, insideOut ? 18 : 36, 1, 0, 0, 0);
        if (mixed)
        {
            // The pipeline in place of the shader objects: the bindings and push constants carry over.
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
        }
        if (occluded || mixed)
            vkCmdDrawIndexed(cb, 36, 1, 0, 0, 0);
    }

    // --------------------------------------------------------------------------------- frame
    // Returns false when nothing was drawn (window minimized or swapchain being replaced).
    /**
     * --compile-hitch: builds a pipeline in the middle of the frame, the way an engine does when it
     * meets a material it has not compiled yet. The point is the stall, so the pipeline is made
     * without a cache and thrown away again: the timeline should show the frame stopping for it
     * (**Where the CPU went**, "Creating pipelines").
     */
    void CompileHitch()
    {
        if (!compileHitch)
            return;
        if (hitchPipeline)
            vkDestroyPipeline(device, hitchPipeline, nullptr);
        VkShaderModule cs = LoadShader("wave.comp.spv");
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = cs;
        cpci.stage.pName = "main";
        cpci.layout = computePipelineLayout;
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &hitchPipeline));
        Name(VK_OBJECT_TYPE_PIPELINE, (uint64_t)hitchPipeline, "Compile hitch");
        vkDestroyShaderModule(device, cs, nullptr);
    }

    void Churn()
    {
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        ChurnBuffer scratch;
        CreateBuffer(64 * 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, scratch.buffer, scratch.memory, "Churn: per-frame scratch");
        churnRecent.push_back(scratch);
        if (churnRecent.size() > 2)
        {
            // Never bound to anything the GPU runs, so it can go without waiting for the frame.
            vkDestroyBuffer(device, churnRecent.front().buffer, nullptr);
            vkFreeMemory(device, churnRecent.front().memory, nullptr);
            churnRecent.erase(churnRecent.begin());
        }
        if (frameCount % 30 == 0)
        {
            ChurnBuffer kept;
            CreateBuffer(1024 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, kept.buffer, kept.memory, "Churn: kept forever");
            churnKept.push_back(kept);
        }
    }

    bool DrawFrame(float t)
    {
        if (resized && !RecreateSwapchain())
            return false;
        CompileHitch();
        // --hitch-every: the application's own work stalling the frame, which no call the layer
        // times accounts for. One frame in N, so a timing run has a hitch to trigger on.
        if (hitchEvery > 0 && frameCount > 0 && (int)(frameCount % hitchEvery) == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // --stall: every frame late, so a vsynced present misses refreshes and the display repeats
        // the frame before it, which is what the dropped-frame count has to see.
        if (stallMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
        VkFence fence = inFlight[frameSlot];
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        uint32_t imageIndex = 0;
        if (!offscreen)
        {
            VkResult ar = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable[frameSlot], VK_NULL_HANDLE, &imageIndex);
            if (ar == VK_ERROR_OUT_OF_DATE_KHR)
            {
                resized = true;
                return false;
            }
            if (ar != VK_SUBOPTIMAL_KHR)
                CHECK(ar);
        }
        CHECK(vkResetFences(device, 1, &fence));

        Mat4 proj = Perspective(1.0f, (float)width / (float)height, 0.1f, 10.0f);
        Mat4 view = Translate(0, 0, -2.5f);
        Mat4 model = Mul(RotateX(t * 0.7f), RotateY(t));
        Mat4 mvp = Mul(proj, Mul(view, model));
        memcpy(uniformMapped, &mvp, sizeof(mvp));

        // --prerecord: the frame's command buffer was recorded when the swapchain was created
        // (see CreateFramebuffers), the way engines that record once and resubmit work; the
        // inspector then needs "Record all command buffers" to see its commands.
        VkCommandBuffer cbs[2] = {prerecord ? prerecorded[imageIndex] : commandBuffers[frameSlot], VK_NULL_HANDLE};
        VkCommandBuffer cb = cbs[0];
        if (!prerecord)
            Record(cb, imageIndex, t);
        if (prerecord && !overlays.empty())
            cbs[1] = overlays[imageIndex];
        if (suspend)
            cbs[1] = suspendBuffers[frameSlot];   // the resumed half of the pass, in the same submission

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = offscreen ? 0 : 1;
        si.pWaitSemaphores = &imageAvailable[frameSlot];
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = cbs[1] ? 2 : 1;
        si.pCommandBuffers = cbs;
        si.signalSemaphoreCount = offscreen ? 0 : 1;
        si.pSignalSemaphores = &renderFinished[frameSlot];
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        DrawSide(t);
        if (offscreen)
        {
            // No present: pace the loop like a 90 Hz headset's runtime instead, on a steady
            // schedule (a sleep after the frame's own work would drift and jitter).
            using namespace std::chrono;
            static steady_clock::time_point next = steady_clock::now();
            next += nanoseconds(11111111);
            std::this_thread::sleep_until(next);
            frameSlot = (frameSlot + 1) % kFramesInFlight;
            frameCount++;
            return true;
        }

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderFinished[frameSlot];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR)
            resized = true;
        else
            CHECK(pr);
        frameSlot = (frameSlot + 1) % kFramesInFlight;
        frameCount++;
        return true;
    }

    void CreateCompute()
    {
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

    void Cleanup()
    {
        vkDeviceWaitIdle(device);
        for (auto* list : {&churnRecent, &churnKept})
        {
            for (const ChurnBuffer& b : *list)
            {
                vkDestroyBuffer(device, b.buffer, nullptr);
                vkFreeMemory(device, b.memory, nullptr);
            }
            list->clear();
        }
        DestroySide();
        DestroyRayTracing();
        // --leak: leave the sampler and the wave buffer alive so the inspector's leak report has
        // something to report at vkDestroyDevice.
        if (leak)
        {
            sampler = VK_NULL_HANDLE;
            waveBuffer = VK_NULL_HANDLE;
        }
        if (persistent)
        {
            for (PersistImage* p : {&trail, &source, &copy})
            {
                vkDestroyFramebuffer(device, p->framebuffer, nullptr);
                vkDestroyImageView(device, p->view, nullptr);
                vkDestroyImage(device, p->image, nullptr);
                vkFreeMemory(device, p->memory, nullptr);
            }
            for (int i = 0; i < kFramesInFlight; ++i)
            {
                vkDestroyBuffer(device, persistStaging[i], nullptr);
                vkFreeMemory(device, persistStagingMemory[i], nullptr);
            }
            vkDestroyRenderPass(device, persistPass, nullptr);
        }
        vkDestroyPipeline(device, computePipeline, nullptr);
        vkDestroyPipelineLayout(device, computePipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, computeSetLayout, nullptr);
        vkDestroyBuffer(device, waveBuffer, nullptr);
        vkFreeMemory(device, waveMemory, nullptr);
        for (int i = 0; i < kFramesInFlight; ++i)
        {
            vkDestroySemaphore(device, imageAvailable[i], nullptr);
            vkDestroySemaphore(device, renderFinished[i], nullptr);
            vkDestroyFence(device, inFlight[i], nullptr);
        }
        if (overlayPass)
            vkDestroyRenderPass(device, overlayPass, nullptr);
        if (pushUpdateTemplate)
            vkDestroyDescriptorUpdateTemplate(device, pushUpdateTemplate, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        for (VkPipeline lib : pipelineLibraries)
            if (lib)
                vkDestroyPipeline(device, lib, nullptr);
        for (VkShaderEXT s : shaders)
            if (s)
                so.destroy(device, s, nullptr);
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
        if (db.buffer)
        {
            if (db.mapped)
                vkUnmapMemory(device, db.memory);
            vkDestroyBuffer(device, db.buffer, nullptr);
            vkFreeMemory(device, db.memory, nullptr);
        }
        if (hitchPipeline)
            vkDestroyPipeline(device, hitchPipeline, nullptr);
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

    int Run()
    {
        CreateWindowNative();
        InitVulkan();
        CreateSwapchain();
        CreateRenderPass();
        if (prerecord)
            CreateOverlayPass();
        CreateFramebuffers();
        CreateResources();
        CreateCompute();
        if (persistent)
            CreatePersistent();
        CreateSide();
        CreateRayTracing();
        if (prerecord)
            PrerecordAll();
        auto start = std::chrono::steady_clock::now();
        while (!quit && (maxFrames < 0 || (int)frameCount < maxFrames))
        {
            PumpEvents();
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
            // Asked again each frame until somebody is there to hear it: the inspector connects a
            // few frames after the device is made.
            if (captureAt > 0 && (int)frameCount >= captureAt && !captureAsked)
            {
                char label[48];
                snprintf(label, sizeof label, "asked at frame %d", captureAt);   // the tab's name
                captureAsked = gpu_inspector_capture_named(1, label) != 0;
            }
            if (churn)
                Churn();
            if (!DrawFrame(t))
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        Cleanup();
        return 0;
    }
};

} // namespace

int RunApp(int argc, char** argv)
{
    App app;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            app.maxFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capture-at") && i + 1 < argc)
            app.captureAt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--churn"))
            app.churn = true;
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)
            app.width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)
            app.height = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bad-scissor"))
            app.badScissor = true;
        else if (!strcmp(argv[i], "--half-scissor"))
            app.halfScissor = true;
        else if (!strcmp(argv[i], "--leak"))
            app.leak = true;
        else if (!strcmp(argv[i], "--hazard"))
            app.hazard = true;
        else if (!strcmp(argv[i], "--occluded"))
            app.occluded = true;
        else if (!strcmp(argv[i], "--no-cull"))
            app.noCull = true;
        else if (!strcmp(argv[i], "--inside-out"))
            app.insideOut = true;
        else if (!strcmp(argv[i], "--prerecord"))
            app.prerecord = true;
        else if (!strcmp(argv[i], "--push-template"))
            app.pushTemplate = true;
        else if (!strcmp(argv[i], "--descriptor-buffer"))
            app.descriptorBuffer = true;
        else if (!strcmp(argv[i], "--alpha-test"))
            app.alphaTest = true;
        else if (!strcmp(argv[i], "--multiview"))
            app.multiview = true;
        else if (!strcmp(argv[i], "--dynamic-rendering"))
            app.dynamicRenderingPass = true;
        else if (!strcmp(argv[i], "--device-local-descriptors"))
            app.descriptorBuffer = app.deviceLocalDescriptors = true;
        else if (!strcmp(argv[i], "--compile-hitch"))
            app.compileHitch = true;
        else if (!strcmp(argv[i], "--hitch-every") && i + 1 < argc)
            app.hitchEvery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stall") && i + 1 < argc)
            app.stallMs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--oob"))
            app.outOfBounds = true;
        else if (!strcmp(argv[i], "--pipeline-library"))
            app.pipelineLibrary = true;
        else if (!strcmp(argv[i], "--shader-object"))
            app.shaderObject = true;
        else if (!strcmp(argv[i], "--mixed"))
            app.shaderObject = app.mixed = true;
        else if (!strcmp(argv[i], "--suspend"))
            app.suspend = true;
        else if (!strcmp(argv[i], "--stencil"))
            app.stencil = true;
        else if (!strcmp(argv[i], "--ray-tracing"))
            app.rayTracing = true;
        else if (!strcmp(argv[i], "--static-blas"))
            app.staticBlas = true;
        else if (!strcmp(argv[i], "--shader-record"))
            app.shaderRecord = app.rayTracing = true;
        else if (!strcmp(argv[i], "--second-device"))
            app.side = App::Side::Device;
        else if (!strcmp(argv[i], "--second-queue"))
            app.side = App::Side::Queue;
        else if (!strcmp(argv[i], "--persistent"))
            app.persistent = true;
        else if (!strcmp(argv[i], "--heavy"))
            app.heavy = true;
        else if (!strcmp(argv[i], "--msaa"))
            app.samples = VK_SAMPLE_COUNT_4_BIT;
        else if (!strcmp(argv[i], "--offscreen"))
        {
            app.offscreen = true;
#ifdef _WIN32
            timeBeginPeriod(1);   // 1 ms scheduler granularity for the frame pacing
#endif
        }
    }
    return app.Run();
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return RunApp(__argc, __argv);
}
#else
int main(int argc, char** argv)
{
    return RunApp(argc, argv);
}
#endif
