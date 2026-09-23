#include "hud.h"

#include "frame_pause.h"
#include "hud_shaders.gen.h"
#include "layer.h"
#include "resources.h"

#include <chrono>
#include <cstring>

namespace vkinsp
{

Hud& Hud::Get()
{
    static Hud* instance = new Hud();
    return *instance;
}

void Hud::SetEnabled(bool on)
{
    const bool was = _enabled.exchange(on, std::memory_order_relaxed);
    if (was != on)
        Log("in-app HUD %s", on ? "on" : "off");
}

void Hud::OnSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t count, const VkImage* images)
{
    (void)device;
    if (!swapchain || !images || !count)
        return;
    std::lock_guard lock(_mutex);
    auto& list = _pendingImages[(uint64_t)(uintptr_t)swapchain];
    list.assign(images, images + count);
}

// -----------------------------------------------------------------------------------------------
// Setup

static int FindHostMemoryType(DeviceData* dev, uint32_t typeBits)
{
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < dev->memoryProperties.memoryTypeCount; ++i)
    {
        if (!(typeBits & (1u << i)))
            continue;
        if ((dev->memoryProperties.memoryTypes[i].propertyFlags & wanted) == wanted)
            return (int)i;
    }
    return -1;
}

Hud::DeviceResources* Hud::Resources(DeviceData* dev, VkQueue queue)
{
    auto it = _devices.find(dev->device);
    if (it != _devices.end())
    {
        DeviceResources& r = it->second;
        return r.failed ? nullptr : &r;
    }

    DeviceResources r;
    r.device = dev->device;
    const DeviceDispatch& d = dev->dispatch;

    // The presenting queue's family: the overlay is submitted on that queue, so its command pool
    // has to come from the same family.
    auto fail = [&](const char* why) -> DeviceResources* {
        Log("HUD: %s; the HUD will not be drawn on this device", why);
        r.failed = true;
        _devices[dev->device] = std::move(r);
        return nullptr;
    };

    {
        std::lock_guard lock(dev->queueMutex);
        auto q = dev->queueFamilies.find(queue);
        if (q == dev->queueFamilies.end())
            return fail("the present queue's family is unknown");
        r.family = q->second;
    }

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(kHudVertSpv);
    smci.pCode = kHudVertSpv;
    if (d.CreateShaderModule(dev->device, &smci, nullptr, &r.vert) != VK_SUCCESS)
        return fail("the vertex shader would not compile");
    smci.codeSize = sizeof(kHudFragSpv);
    smci.pCode = kHudFragSpv;
    if (d.CreateShaderModule(dev->device, &smci, nullptr, &r.frag) != VK_SUCCESS)
        return fail("the fragment shader would not compile");

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(float) * 2;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (d.CreatePipelineLayout(dev->device, &plci, nullptr, &r.pipelineLayout) != VK_SUCCESS)
        return fail("the pipeline layout would not be created");

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = r.family;
    if (d.CreateCommandPool(dev->device, &cpci, nullptr, &r.pool) != VK_SUCCESS)
        return fail("the command pool would not be created");

    constexpr size_t kRing = 4;
    r.frames.resize(kRing);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = r.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& f : r.frames)
    {
        if (d.AllocateCommandBuffers(dev->device, &cbai, &f.cb) != VK_SUCCESS)
            return fail("a command buffer would not be allocated");
        // A command buffer allocated from inside a layer skips the loader trampoline that stamps
        // the dispatch pointer of dispatchable objects; layers below (the validation layer) look
        // their state up by it, so set it to the device's, as the loader would. Same as
        // image_readback.cpp and capture.cpp. Without it the validation layer dereferences a null
        // layer-data pointer the first time the HUD resets this command buffer, and the crash only
        // appears when validation is in the chain -- the driver never reads that field.
        *reinterpret_cast<void**>(f.cb) = *reinterpret_cast<void**>(dev->device);
        if (d.CreateFence(dev->device, &fci, nullptr, &f.fence) != VK_SUCCESS)
            return fail("a fence would not be created");
        if (d.CreateSemaphore(dev->device, &sci, nullptr, &f.done) != VK_SUCCESS)
            return fail("a semaphore would not be created");
    }

    Log("HUD: ready on queue family %u", r.family);
    _devices[dev->device] = std::move(r);
    return &_devices[dev->device];
}

bool Hud::EnsureSwapchain(DeviceData* dev, DeviceResources& r, VkSwapchainKHR sc, SwapchainResources*& out)
{
    const uint64_t key = (uint64_t)(uintptr_t)sc;
    SwapchainResources& s = r.swapchains[key];
    out = &s;

    SwapchainInfo info;
    if (!ResourceRegistry::Get().GetSwapchain(sc, info))
        return false;

    // Already built for this swapchain, at this size and format.
    if (s.renderPass && s.swapchain == sc && s.format == info.format &&
        s.extent.width == info.extent.width && s.extent.height == info.extent.height && !s.framebuffers.empty())
        return s.usable;

    DestroySwapchain(dev, s);
    s.swapchain = sc;
    s.format = info.format;
    s.extent = info.extent;

    // An image the application never asked to be a color attachment cannot be drawn into. Some
    // applications create a transfer-only swapchain and blit into it; there the HUD stays off.
    if (!(info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    {
        if (!s.complained)
        {
            Log("HUD: the swapchain has no color attachment usage, so the HUD cannot be drawn into it");
            s.complained = true;
        }
        return false;
    }

    auto images = _pendingImages.find(key);
    if (images == _pendingImages.end() || images->second.empty())
        return false;
    s.images = images->second;

    const DeviceDispatch& d = dev->dispatch;

    // Loads what the application drew and leaves the image exactly as it found it: the layer draws
    // after the application's own transition to PRESENT_SRC, so both ends of the pass are that
    // layout and the render pass does the round trip to COLOR_ATTACHMENT_OPTIMAL itself.
    VkAttachmentDescription attachment{};
    attachment.format = s.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    // Either side of the pass: the wait semaphore makes the application's rendering available, and
    // the signal makes the overlay's writes available to the presentation engine, but the layout
    // transitions the pass itself performs still need to be ordered against both.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = 0;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &attachment;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    if (d.CreateRenderPass(dev->device, &rpci, nullptr, &s.renderPass) != VK_SUCCESS)
    {
        Log("HUD: the render pass would not be created");
        return false;
    }

    s.views.resize(s.images.size(), VK_NULL_HANDLE);
    s.framebuffers.resize(s.images.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < s.images.size(); ++i)
    {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = s.images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = s.format;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (d.CreateImageView(dev->device, &ivci, nullptr, &s.views[i]) != VK_SUCCESS)
        {
            Log("HUD: an image view would not be created");
            DestroySwapchain(dev, s);
            return false;
        }
        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass = s.renderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &s.views[i];
        fbci.width = s.extent.width;
        fbci.height = s.extent.height;
        fbci.layers = 1;
        if (d.CreateFramebuffer(dev->device, &fbci, nullptr, &s.framebuffers[i]) != VK_SUCCESS)
        {
            Log("HUD: a framebuffer would not be created");
            DestroySwapchain(dev, s);
            return false;
        }
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r.vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r.frag;
    stages[1].pName = "main";

    // One instance per rectangle; the four vertices of the strip are computed from gl_VertexIndex.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(gpuhud::Rect);
    binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[0].offset = offsetof(gpuhud::Rect, x);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[1].offset = offsetof(gpuhud::Rect, r);
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &ds;
    gpci.layout = r.pipelineLayout;
    gpci.renderPass = s.renderPass;
    gpci.subpass = 0;
    if (d.CreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &s.pipeline) != VK_SUCCESS)
    {
        Log("HUD: the pipeline would not be created");
        DestroySwapchain(dev, s);
        return false;
    }

    s.usable = true;
    Log("HUD: drawing into a %ux%u swapchain of %u images", s.extent.width, s.extent.height, (uint32_t)s.images.size());
    return true;
}

bool Hud::EnsureVertexBuffer(DeviceData* dev, DeviceResources& r, Frame& f, VkDeviceSize bytes)
{
    (void)r;
    if (f.capacity >= bytes && f.vertices)
        return true;
    const DeviceDispatch& d = dev->dispatch;
    if (f.mapped)
    {
        d.UnmapMemory(dev->device, f.memory);
        f.mapped = nullptr;
    }
    if (f.vertices)
        d.DestroyBuffer(dev->device, f.vertices, nullptr);
    if (f.memory)
        d.FreeMemory(dev->device, f.memory, nullptr);
    f.vertices = VK_NULL_HANDLE;
    f.memory = VK_NULL_HANDLE;
    f.capacity = 0;

    // Rounded up so a HUD that gains a line does not reallocate every frame.
    VkDeviceSize size = 4096;
    while (size < bytes)
        size *= 2;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (d.CreateBuffer(dev->device, &bci, nullptr, &f.vertices) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req{};
    d.GetBufferMemoryRequirements(dev->device, f.vertices, &req);
    const int type = FindHostMemoryType(dev, req.memoryTypeBits);
    if (type < 0)
    {
        d.DestroyBuffer(dev->device, f.vertices, nullptr);
        f.vertices = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (d.AllocateMemory(dev->device, &mai, nullptr, &f.memory) != VK_SUCCESS)
    {
        d.DestroyBuffer(dev->device, f.vertices, nullptr);
        f.vertices = VK_NULL_HANDLE;
        return false;
    }
    if (d.BindBufferMemory(dev->device, f.vertices, f.memory, 0) != VK_SUCCESS)
        return false;
    if (d.MapMemory(dev->device, f.memory, 0, VK_WHOLE_SIZE, 0, &f.mapped) != VK_SUCCESS)
        return false;
    f.capacity = size;
    return true;
}

// -----------------------------------------------------------------------------------------------
// Timing
//
// Measured against the HUD's own timestamp rather than DeviceData::lastPresent. Two reasons, and
// the second one is a trap: DeviceData's counters are reset every time the frame report goes to
// the UI (EndFrame in layer.cpp), so the HUD needs its own window; and lastPresent is set *after*
// vkQueuePresentKHR returns, while the HUD draws *before* it is called. Measuring from one to the
// other would leave out the time the application spends blocked inside the present, which under
// FIFO is the wait for vblank -- the bulk of the frame. It read 2.64 ms and 379 FPS on a triangle
// that was in fact running at the display's 144 Hz.
//
// Draw is called at one fixed point in the application's frame loop, so the interval between two
// of its timestamps is a whole frame, blocking present included.

void Hud::UpdateTiming(DeviceData* dev, DeviceResources& r)
{
    (void)dev;
    const auto now = std::chrono::steady_clock::now();
    const auto previous = r.lastDraw;
    const uint64_t generation = gpuinsp::FramePause::Get().Generation();
    const bool acrossPause = generation != r.pauseGeneration;
    r.lastDraw = now;
    r.pauseGeneration = generation;
    if (previous.time_since_epoch().count() == 0)
        return;   // the first frame has nothing to measure
    // This interval spans a pause, so it is the length of the pause: a step out of a five-second
    // one was being read as a 4985 ms frame. Taken from the pause's own counter rather than from
    // the paused flag, which is read here before the present and can change while the application
    // is still inside it.
    if (acrossPause)
        return;
    const double ms = std::chrono::duration<double, std::milli>(now - previous).count();
    // An absurd interval -- a breakpoint, a minimized window, the HUD being switched on mid-run --
    // is dropped rather than smoothed in.
    if (ms <= 0 || ms > 10000)
        return;

    if (r.windowFrames == 0)
    {
        r.minMs = ms;
        r.maxMs = ms;
    }
    else
    {
        if (ms < r.minMs)
            r.minMs = ms;
        if (ms > r.maxMs)
            r.maxMs = ms;
    }
    r.windowMs += ms;
    r.windowFrames++;
    if (r.smoothedMs == 0)
    {
        r.smoothedMs = ms;
        r.shownMinMs = ms;
        r.shownMaxMs = ms;
    }
    // Published twice a second: often enough to follow a change, seldom enough to read.
    if (r.windowMs >= 500.0)
    {
        r.smoothedMs = r.windowMs / r.windowFrames;
        r.shownMinMs = r.minMs;
        r.shownMaxMs = r.maxMs;
        r.windowMs = 0;
        r.windowFrames = 0;
    }
}

// -----------------------------------------------------------------------------------------------
// Drawing

bool Hud::Draw(DeviceData* dev, VkQueue queue, const VkPresentInfoKHR* in, VkPresentInfoKHR& out,
    std::vector<VkSemaphore>& waits)
{
    if (!Enabled() || !dev || !in || !in->swapchainCount || !in->pSwapchains)
        return false;

    std::lock_guard lock(_mutex);
    DeviceResources* res = Resources(dev, queue);
    if (!res)
        return false;
    DeviceResources& r = *res;
    UpdateTiming(dev, r);
    if (r.smoothedMs <= 0)
        return false;   // nothing measured yet

    const DeviceDispatch& d = dev->dispatch;

    // The slot's previous submission must be done before its command buffer and vertex buffer are
    // written again. With four slots this has effectively always happened already.
    Frame& f = r.frames[r.next];
    if (f.submitted)
    {
        if (d.WaitForFences(dev->device, 1, &f.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS)
            return false;
        d.ResetFences(dev->device, 1, &f.fence);
        f.submitted = false;
    }

    // Build every swapchain's rectangles into one array, and remember each one's range.
    struct Target
    {
        SwapchainResources* s;
        uint32_t imageIndex;
        uint32_t first;
        uint32_t count;
    };
    std::vector<Target> targets;
    std::vector<gpuhud::Rect> rects;
    for (uint32_t i = 0; i < in->swapchainCount; ++i)
    {
        SwapchainResources* s = nullptr;
        if (!EnsureSwapchain(dev, r, in->pSwapchains[i], s) || !s || !s->usable)
            continue;
        const uint32_t imageIndex = in->pImageIndices ? in->pImageIndices[i] : 0;
        if (imageIndex >= s->framebuffers.size())
            continue;

        gpuhud::HudState state;
        state.frameMs = r.smoothedMs;
        state.minMs = r.shownMinMs;
        state.maxMs = r.shownMaxMs;
        state.refreshMs = dev->refreshMs;
        // frameIndex counts frames already ended, and EndFrame has not run for this one yet, so
        // the frame the user is about to look at is the next one.
        state.frame = dev->frameIndex + 1;
        state.paused = gpuinsp::FramePause::Get().Paused();
        state.backend = "VULKAN";
        const uint32_t first = (uint32_t)rects.size();
        gpuhud::BuildHud(rects, state, s->extent.width, s->extent.height, gpuhud::HudScale(s->extent.width));
        targets.push_back({s, imageIndex, first, (uint32_t)rects.size() - first});
    }
    if (targets.empty() || rects.empty())
        return false;

    const VkDeviceSize bytes = rects.size() * sizeof(gpuhud::Rect);
    if (!EnsureVertexBuffer(dev, r, f, bytes))
        return false;
    memcpy(f.mapped, rects.data(), (size_t)bytes);

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (d.ResetCommandBuffer(f.cb, 0) != VK_SUCCESS)
        return false;
    if (d.BeginCommandBuffer(f.cb, &cbbi) != VK_SUCCESS)
        return false;
    for (const Target& t : targets)
    {
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = t.s->renderPass;
        rpbi.framebuffer = t.s->framebuffers[t.imageIndex];
        rpbi.renderArea.extent = t.s->extent;
        d.CmdBeginRenderPass(f.cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        d.CmdBindPipeline(f.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, t.s->pipeline);
        VkViewport viewport{0, 0, (float)t.s->extent.width, (float)t.s->extent.height, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, t.s->extent};
        d.CmdSetViewport(f.cb, 0, 1, &viewport);
        d.CmdSetScissor(f.cb, 0, 1, &scissor);
        const VkDeviceSize offset = (VkDeviceSize)t.first * sizeof(gpuhud::Rect);
        d.CmdBindVertexBuffers(f.cb, 0, 1, &f.vertices, &offset);
        const float invSize[2] = {1.0f / (float)t.s->extent.width, 1.0f / (float)t.s->extent.height};
        d.CmdPushConstants(f.cb, r.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(invSize), invSize);
        d.CmdDraw(f.cb, 4, t.count, 0, 0);
        d.CmdEndRenderPass(f.cb);
    }
    if (d.EndCommandBuffer(f.cb) != VK_SUCCESS)
        return false;

    // The overlay waits on whatever the present was going to wait on, and the present waits on the
    // overlay instead. See the header: submitting on the same queue is not enough on its own.
    std::vector<VkPipelineStageFlags> stages(in->waitSemaphoreCount ? in->waitSemaphoreCount : 0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = in->waitSemaphoreCount;
    si.pWaitSemaphores = in->pWaitSemaphores;
    si.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &f.done;
    if (d.QueueSubmit(queue, 1, &si, f.fence) != VK_SUCCESS)
        return false;
    f.submitted = true;
    r.next = (r.next + 1) % r.frames.size();

    waits.assign(1, f.done);
    out = *in;
    out.waitSemaphoreCount = 1;
    out.pWaitSemaphores = waits.data();
    return true;
}

// -----------------------------------------------------------------------------------------------
// Teardown

void Hud::DestroySwapchain(DeviceData* dev, SwapchainResources& s)
{
    const DeviceDispatch& d = dev->dispatch;
    for (VkFramebuffer fb : s.framebuffers)
        if (fb)
            d.DestroyFramebuffer(dev->device, fb, nullptr);
    for (VkImageView v : s.views)
        if (v)
            d.DestroyImageView(dev->device, v, nullptr);
    if (s.pipeline)
        d.DestroyPipeline(dev->device, s.pipeline, nullptr);
    if (s.renderPass)
        d.DestroyRenderPass(dev->device, s.renderPass, nullptr);
    s.framebuffers.clear();
    s.views.clear();
    s.images.clear();
    s.pipeline = VK_NULL_HANDLE;
    s.renderPass = VK_NULL_HANDLE;
    s.usable = false;
}

void Hud::OnDestroySwapchain(DeviceData* dev, VkSwapchainKHR swapchain)
{
    if (!dev || !swapchain)
        return;
    std::lock_guard lock(_mutex);
    _pendingImages.erase((uint64_t)(uintptr_t)swapchain);
    auto it = _devices.find(dev->device);
    if (it == _devices.end())
        return;
    DeviceResources& r = it->second;
    auto s = r.swapchains.find((uint64_t)(uintptr_t)swapchain);
    if (s == r.swapchains.end())
        return;
    // An overlay drawn into these images may still be running: the application's own guarantee
    // covers its work, not the layer's.
    for (auto& f : r.frames)
        if (f.submitted)
            dev->dispatch.WaitForFences(dev->device, 1, &f.fence, VK_TRUE, 1000000000ull);
    DestroySwapchain(dev, s->second);
    r.swapchains.erase(s);
}

void Hud::OnDestroyDevice(DeviceData* dev)
{
    if (!dev)
        return;
    std::lock_guard lock(_mutex);
    auto it = _devices.find(dev->device);
    if (it == _devices.end())
        return;
    DeviceResources& r = it->second;
    const DeviceDispatch& d = dev->dispatch;
    for (auto& f : r.frames)
        if (f.submitted)
            d.WaitForFences(dev->device, 1, &f.fence, VK_TRUE, 1000000000ull);
    for (auto& s : r.swapchains)
    {
        _pendingImages.erase(s.first);
        DestroySwapchain(dev, s.second);
    }
    r.swapchains.clear();
    for (auto& f : r.frames)
    {
        if (f.mapped)
            d.UnmapMemory(dev->device, f.memory);
        if (f.vertices)
            d.DestroyBuffer(dev->device, f.vertices, nullptr);
        if (f.memory)
            d.FreeMemory(dev->device, f.memory, nullptr);
        if (f.done)
            d.DestroySemaphore(dev->device, f.done, nullptr);
        if (f.fence)
            d.DestroyFence(dev->device, f.fence, nullptr);
    }
    if (r.pool)
        d.DestroyCommandPool(dev->device, r.pool, nullptr);
    if (r.pipelineLayout)
        d.DestroyPipelineLayout(dev->device, r.pipelineLayout, nullptr);
    if (r.vert)
        d.DestroyShaderModule(dev->device, r.vert, nullptr);
    if (r.frag)
        d.DestroyShaderModule(dev->device, r.frag, nullptr);
    _devices.erase(it);
}

} // namespace vkinsp
