// Pixel history: every event of the frame that touched one pixel of one image, and what each draw's
// fragments at that pixel met (after RenderDoc's vk_pixelhistory.cpp, simplified).
//
// Each pass that renders to the image is followed in two steps.
//   * Before the replay executes the pass, every attachment's state at the pass's start is copied
//     into an image of the replay's own (its contents when it loads, its clear value when it
//     clears), and the pixel is read from it: the "load" event.
//   * After the pass, its commands are issued again, one event at a time, into those copies,
//     inside a pass that loads what the last event left:
//       - a draw first runs with six copies of its pipeline, each writing nothing, under an
//         occlusion query and a one-pixel scissor: covered (no culling, no tests), facing (the
//         pipeline's culling), shaded (its fragment shader, which may discard), depth only,
//         stencil only, and every test; then once more into a target of the replay's own with its
//         fragment shader replaced by one writing gl_PrimitiveID, which says which primitive won
//         the pixel; then with its own pipeline, blending and depth writes;
//       - vkCmdClearAttachments runs as it is;
//       - bindings and dynamic state are issued between passes, where they stay in effect.
//     After each event the pixel, and the pass's depth at it, are read.
// A pass's state carries across the replay's begin and end of its own passes: they are
// compatible with the pass (the same attachments and subpasses, loading instead of clearing).
//
// A write from outside a render pass needs none of that: it lands in the image itself, so the pixel
// is read straight out of it once the command has run. A clear, a copy, a blit and a resolve say in
// their own arguments which image and which part of it they write; a dispatch or a trace writes
// through a descriptor, so what is known of it is what was bound (HistoryDirectWrite).
//
// A shader that asks for the depth and stencil tests before it (EarlyFragmentTests) has them on in
// the variant that measures the shader, because the hardware never runs it on a fragment they
// killed; the event says so, since it changes what the shaded count means.
//
// Not followed yet: layered passes past their first layer; every fragment of a draw separately (the
// primitive is the winning fragment's, and the value is the pixel after the whole draw); and a
// multisampled depth target, which cannot be resolved to be read.
#include "replayer.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "format_info.h"
#include "util.h"

namespace vkreplay
{

namespace
{

enum HistoryVariant
{
    kCovered = 0,
    kFacing,
    kShaded,
    kDepthOnly,
    kStencilOnly,
    kAllTests,
    kVariantCount
};

/** What the primitive-id pass writes into: one unsigned integer per pixel, read back as one. */
constexpr VkFormat kPrimitiveIdFormat = VK_FORMAT_R32_UINT;

/**
 * Fragments measured one by one per draw. A draw that puts more than this on one pixel is measured
 * up to here and the event says how many there were; in a frame worth following a pixel through,
 * a handful is the usual number and hundreds means the geometry is the problem, not the fragment.
 */
constexpr uint32_t kMaxHistoryFragments = 16;

bool IsDepthFormat(VkFormat f)
{
    return (vkinsp::FormatAspects(f) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
}

/** The layout an attachment copy stays in between the history's passes. */
VkImageLayout AttachmentLayout(VkFormat f)
{
    return IsDepthFormat(f) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

VkStencilOpState KeepOps(VkStencilOpState s)
{
    s.failOp = s.passOp = s.depthFailOp = VK_STENCIL_OP_KEEP;
    return s;
}

/**
 * Whether a fragment module declares EarlyFragmentTests. The mode belongs to an entry point, but a
 * captured module holds the one the pipeline names, and the mode exists for fragment stages alone,
 * so any declaration of it in the module is the one that applies.
 */
bool DeclaresEarlyFragmentTests(const uint8_t* data, size_t size)
{
    constexpr uint32_t kMagic = 0x07230203u;
    constexpr uint32_t kOpExecutionMode = 16;
    constexpr uint32_t kEarlyFragmentTests = 8;
    if (!data || size < 20 || size % 4)
        return false;
    const size_t count = size / 4;
    std::vector<uint32_t> words(count);
    std::memcpy(words.data(), data, size);   // the blob is not aligned for uint32_t reads
    if (words[0] != kMagic)
        return false;
    for (size_t at = 5; at < count;)
    {
        const uint32_t op = words[at] & 0xFFFFu;
        const uint32_t n = words[at] >> 16;
        if (!n || at + n > count)
            break;
        if (op == kOpExecutionMode && n >= 3 && words[at + 2] == kEarlyFragmentTests)
            return true;
        at += n;
    }
    return false;
}

VkRect2D RectOf(const JValue* v)
{
    VkRect2D r{};
    if (!v)
        return r;
    if (const JValue* o = v->Get("offset"))
    {
        r.offset.x = (int32_t)(o->Get("x") ? o->Get("x")->Int() : 0);
        r.offset.y = (int32_t)(o->Get("y") ? o->Get("y")->Int() : 0);
    }
    if (const JValue* e = v->Get("extent"))
    {
        r.extent.width = (uint32_t)(e->Get("width") ? e->Get("width")->Uint() : 0);
        r.extent.height = (uint32_t)(e->Get("height") ? e->Get("height")->Uint() : 0);
    }
    return r;
}

} // namespace

bool Replayer::HistoryEarlyFragmentTests(uint64_t pipelineId)
{
    auto it = _historyEarlyTests.find(pipelineId);
    if (it != _historyEarlyTests.end())
        return it->second;
    bool early = false;
    const JValue* object = _capture->Object(pipelineId);
    if (object)
    {
        // The stage's entry point names its blob; "main" is what a pipeline whose stage the capture
        // recorded without one is keyed by, and the only name in practice.
        const uint8_t* data = nullptr;
        size_t size = 0;
        for (const char* entry : {"main", ""})
        {
            if (_capture->Blob(*object, std::string(StageName(VK_SHADER_STAGE_FRAGMENT_BIT)) + ":" + entry, data, size))
                break;
            data = nullptr;
            size = 0;
        }
        early = DeclaresEarlyFragmentTests(data, size);
    }
    _historyEarlyTests[pipelineId] = early;
    return early;
}

VkPipeline Replayer::ViewPipeline(uint64_t pipelineId)
{
    const auto key = std::make_pair(pipelineId, _historyView);
    if (auto it = _historyViewPipelines.find(key); it != _historyViewPipelines.end())
        return it->second;
    _historyViewPipelines[key] = VK_NULL_HANDLE;   // a copy that cannot be made is not tried again
    VkPipeline pipeline = CopyGraphicsPipeline(pipelineId, "pixel history view", [&](PipelineCopy& p) {
        RenderSingleView(p);
        return true;
    });
    _historyViewPipelines[key] = pipeline;
    return pipeline;
}

void Replayer::RenderSingleView(PipelineCopy& p)
{
    if (!_historyView)
        return;
    if (_historyViewRenderPass)
    {
        p.info.renderPass = HistoryRenderPass(_historyViewRenderPass);
        return;
    }
    // Dynamic rendering: the formats the pipeline was made for, with the one view.
    for (auto* in = static_cast<const VkBaseInStructure*>(p.info.pNext); in; in = in->pNext)
    {
        if (in->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
            continue;
        auto* rendering = _arena.Make<VkPipelineRenderingCreateInfo>();
        *rendering = *reinterpret_cast<const VkPipelineRenderingCreateInfo*>(in);
        rendering->viewMask = _historyView;
        rendering->pNext = StripPNext(p.info.pNext, {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO});
        p.info.pNext = rendering;
        return;
    }
}

VkPipeline Replayer::HistoryPipeline(uint64_t pipelineId, int variant)
{
    const auto key = std::make_tuple(pipelineId, variant, _historyView);
    auto it = _historyPipelines.find(key);
    if (it != _historyPipelines.end())
        return it->second;
    _historyPipelines[key] = VK_NULL_HANDLE;
    // A shader that asks for the depth and stencil tests before it never runs on a fragment they
    // killed, so the variant that measures the shader keeps them on: with them off, a shader that
    // discards what the depth test would have killed is reported as discarding it, which is the
    // wrong answer to "why is this pixel not the color this draw writes".
    const bool early = HistoryEarlyFragmentTests(pipelineId);
    VkPipeline pipeline = CopyGraphicsPipeline(pipelineId, "pixel history", [&](PipelineCopy& p) {
        if (p.hasRasterization && p.rasterization.rasterizerDiscardEnable)
            return false;
        if (variant == kCovered || variant == kFacing)
            p.ReplaceFragment(CountModule());
        if (variant == kCovered && p.hasRasterization)
            p.rasterization.cullMode = VK_CULL_MODE_NONE;
        // The queries only count: nothing is written.
        for (VkPipelineColorBlendAttachmentState& a : p.blendAttachments)
        {
            a.blendEnable = VK_FALSE;
            a.colorWriteMask = 0;
        }
        p.blend.logicOpEnable = VK_FALSE;
        const bool depth = variant == kDepthOnly || variant == kAllTests || (early && variant == kShaded);
        const bool stencil = variant == kStencilOnly || variant == kAllTests || (early && variant == kShaded);
        if (p.hasDepthStencil)
        {
            p.depthStencil.depthWriteEnable = VK_FALSE;
            p.depthStencil.front = KeepOps(p.depthStencil.front);
            p.depthStencil.back = KeepOps(p.depthStencil.back);
            if (!depth)
            {
                p.depthStencil.depthTestEnable = VK_FALSE;
                p.depthStencil.depthBoundsTestEnable = VK_FALSE;
            }
            if (!stencil)
                p.depthStencil.stencilTestEnable = VK_FALSE;
        }
        p.RemoveDynamic(kColorOutputDynamicStates);
        p.RemoveDynamic({VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP});
        if (!depth)
            p.RemoveDynamic({VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE});
        if (!stencil)
            p.RemoveDynamic({VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE});
        if (variant == kCovered)
            p.RemoveDynamic({VK_DYNAMIC_STATE_CULL_MODE});
        // The one-pixel scissor.
        if (!p.HasDynamic(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT))
            p.AddDynamic(VK_DYNAMIC_STATE_SCISSOR);
        RenderSingleView(p);
        return true;
    });
    _historyPipelines[key] = pipeline;
    return pipeline;
}

// ---------------------------------------------------------------------------------------------
// The primitive id of the fragment that won the pixel. A draw's own occlusion counts say how many
// of its fragments reached the pixel and what they met; they cannot say *which* primitive wrote it,
// which is the question that identifies the geometry to go and look at. So the draw runs once more,
// its fragment shader replaced by one writing gl_PrimitiveID into a target of the replay's own,
// against the depth and stencil the event starts from: what is left in the pixel is the primitive
// of the fragment that won it, exactly as the depth test decided it.

VkRenderPass Replayer::HistoryIdRenderPass(VkFormat depthFormat)
{
    const auto key = std::make_pair(depthFormat, _historyView);
    auto it = _historyIdRenderPasses.find(key);
    if (it != _historyIdRenderPasses.end())
        return it->second;
    _historyIdRenderPasses[key] = VK_NULL_HANDLE;
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = kPrimitiveIdFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // only the render area, which is the pixel
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // The depth and stencil the history's own copies hold at this event, loaded and stored
    // unchanged: the pass writes neither, so storing them leaves the copies as they were.
    attachments[1].format = depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].initialLayout = attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    if (depthFormat != VK_FORMAT_UNDEFINED)
        subpass.pDepthStencilAttachment = &depth;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = depthFormat != VK_FORMAT_UNDEFINED ? 2 : 1;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    // The followed view of a multiview pass, into its layer.
    VkRenderPassMultiviewCreateInfo multiview{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    multiview.subpassCount = 1;
    multiview.pViewMasks = &_historyView;
    if (_historyView)
        info.pNext = &multiview;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (_fns.CreateRenderPass(_device, &info, nullptr, &rp) != VK_SUCCESS || !rp)
        return VK_NULL_HANDLE;
    Track("VkRenderPass", (uint64_t)rp);
    _historyIdRenderPasses[key] = rp;
    return rp;
}

VkPipeline Replayer::HistoryIdPipeline(uint64_t pipelineId, VkFormat depthFormat)
{
    const auto key = std::make_tuple(pipelineId, depthFormat, _historyView);
    auto it = _historyIdPipelines.find(key);
    if (it != _historyIdPipelines.end())
        return it->second;
    _historyIdPipelines[key] = VK_NULL_HANDLE;   // a copy that cannot be made is not tried again
    VkRenderPass rp = HistoryIdRenderPass(depthFormat);
    VkShaderModule module = PrimitiveIdModule();
    if (!rp || !module)
        return VK_NULL_HANDLE;
    VkPipeline pipeline = CopyGraphicsPipeline(pipelineId, "pixel history primitive", [&](PipelineCopy& p) {
        if (p.hasRasterization && p.rasterization.rasterizerDiscardEnable)
            return false;
        // The draw's own geometry, its own culling and its own depth and stencil tests; the
        // fragment shader replaced, since what it computes is not the answer here — which
        // primitive its fragment came from is.
        p.ReplaceFragment(module);
        VkPipelineColorBlendAttachmentState write{};
        write.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        p.blendAttachments = {write};
        p.blend = VkPipelineColorBlendStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        p.hasBlend = true;
        p.multisample = VkPipelineMultisampleStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        p.multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        p.hasMultisample = true;
        // The draw's own depth and stencil writes stay: this pass tests against a copy of the
        // pass's depth (PendingHistory::idDepthCopy), so a draw whose own fragments hide one
        // another leaves the primitive that really won, and the copy the later events read is
        // untouched. A pass whose depth could not be copied is drawn without them instead.
        if (p.hasDepthStencil && depthFormat == VK_FORMAT_UNDEFINED)
        {
            p.depthStencil.depthWriteEnable = VK_FALSE;
            p.depthStencil.front = KeepOps(p.depthStencil.front);
            p.depthStencil.back = KeepOps(p.depthStencil.back);
        }
        if (depthFormat == VK_FORMAT_UNDEFINED)
            p.hasDepthStencil = false;
        p.RemoveDynamic(kColorOutputDynamicStates);
        p.RemoveDynamic(kMultisampleDynamicStates);
        p.RemoveDynamic({VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP});
        if (!p.HasDynamic(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT))
            p.AddDynamic(VK_DYNAMIC_STATE_SCISSOR);
        p.info.pNext = StripPNext(p.info.pNext, {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO});
        p.info.renderPass = rp;
        p.info.subpass = 0;
        return true;
    });
    _historyIdPipelines[key] = pipeline;
    return pipeline;
}

// ---------------------------------------------------------------------------------------------
// The fragments of one draw
//
// A draw's own entry says how many of its fragments reached the pixel and which primitive won it;
// what it cannot say is what each of the others was. The frame is replayed a second time for that,
// and each such draw is run once per fragment with the stencil used as a counter: every fragment
// increments it, and the comparison lets through only the one that finds its own index there
// (RenderDoc's per-fragment pass works the same way, vk_pixelhistory.cpp). Each run writes into
// images of the replay's own, so nothing the followed pass holds changes -- once with the draw's
// own fragment shader, for what that fragment computed, and once with the primitive-id shader, for
// the primitive it came from.

VkFormat Replayer::HistoryFragmentDepthFormat()
{
    if (_historyFragmentDepthFormat != VK_FORMAT_UNDEFINED)
        return _historyFragmentDepthFormat;
    // One of these two is always supported as a depth-stencil attachment.
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_S8_UINT})
    {
        VkFormatProperties props{};
        _fns.GetPhysicalDeviceFormatProperties(_physical, f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            _historyFragmentDepthFormat = f;
            return f;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

VkRenderPass Replayer::HistoryFragmentRenderPass(VkFormat format)
{
    const auto key = std::make_pair(format, _historyView);
    auto it = _historyFragmentRenderPasses.find(key);
    if (it != _historyFragmentRenderPasses.end())
        return it->second;
    _historyFragmentRenderPasses[key] = VK_NULL_HANDLE;
    const VkFormat dsFormat = HistoryFragmentDepthFormat();
    if (dsFormat == VK_FORMAT_UNDEFINED)
        return VK_NULL_HANDLE;
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // only the render area, which is the pixel
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // The counter, cleared at every run so each one counts the draw's fragments from zero again.
    attachments[1].format = dsFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPassMultiviewCreateInfo multiview{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    multiview.subpassCount = 1;
    multiview.pViewMasks = &_historyView;
    if (_historyView)
        info.pNext = &multiview;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (_fns.CreateRenderPass(_device, &info, nullptr, &rp) != VK_SUCCESS || !rp)
        return VK_NULL_HANDLE;
    Track("VkRenderPass", (uint64_t)rp);
    _historyFragmentRenderPasses[key] = rp;
    return rp;
}

VkPipeline Replayer::HistoryFragmentPipeline(uint64_t pipelineId, VkFormat format, bool idPass)
{
    auto& cache = idPass ? _historyFragmentIdPipelines : _historyFragmentPipelines;
    const auto key = std::make_tuple(pipelineId, format, _historyView);
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    cache[key] = VK_NULL_HANDLE;   // a copy that cannot be made is not tried again
    VkRenderPass rp = HistoryFragmentRenderPass(format);
    VkShaderModule module = idPass ? PrimitiveIdModule() : VK_NULL_HANDLE;
    if (!rp || (idPass && !module))
        return VK_NULL_HANDLE;
    const char* what = idPass ? "pixel history fragment primitive" : "pixel history fragment";
    VkPipeline pipeline = CopyGraphicsPipeline(pipelineId, what, [&](PipelineCopy& p) {
        if (p.hasRasterization && p.rasterization.rasterizerDiscardEnable)
            return false;
        if (idPass)
            p.ReplaceFragment(module);
        // One attachment, written whole and unblended: what the fragment's shader computed, rather
        // than what it would have left in the pixel.
        VkPipelineColorBlendAttachmentState write{};
        write.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        p.blendAttachments = {write};
        p.blend = VkPipelineColorBlendStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        p.hasBlend = true;
        p.multisample = VkPipelineMultisampleStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        p.multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        p.hasMultisample = true;
        // The stencil counts this draw's fragments: each increments it, whether its comparison
        // passed or failed, so the one that finds its own index there is the one that writes. The
        // draw's own depth and stencil tests are off -- a fragment they killed still says what it
        // computed, and whether it passed is what the draw's own counts measure.
        VkStencilOpState isolate{};
        isolate.failOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        isolate.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        isolate.depthFailOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        isolate.compareOp = VK_COMPARE_OP_EQUAL;
        isolate.compareMask = 0xFF;
        isolate.writeMask = 0xFF;
        isolate.reference = 0;   // dynamic: the fragment's index
        p.depthStencil = VkPipelineDepthStencilStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        p.depthStencil.stencilTestEnable = VK_TRUE;
        p.depthStencil.front = isolate;
        p.depthStencil.back = isolate;
        p.hasDepthStencil = true;
        p.RemoveDynamic(kColorOutputDynamicStates);
        p.RemoveDynamic(kMultisampleDynamicStates);
        p.RemoveDynamic({VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK});
        p.AddDynamic(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
        if (!p.HasDynamic(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT))
            p.AddDynamic(VK_DYNAMIC_STATE_SCISSOR);
        p.info.pNext = StripPNext(p.info.pNext, {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO});
        p.info.renderPass = rp;
        p.info.subpass = 0;
        return true;
    });
    cache[key] = pipeline;
    return pipeline;
}

bool Replayer::PrepareHistoryFragments(PendingHistory& pending, const PassState& pass, uint32_t slots)
{
    if (!slots || pending.fragFormat == VK_FORMAT_UNDEFINED)
        return false;
    const VkFormat dsFormat = HistoryFragmentDepthFormat();
    if (dsFormat == VK_FORMAT_UNDEFINED)
        return false;
    pending.fragColor = CreateTransientImage(pending.fragFormat, pass.extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_SAMPLE_COUNT_1_BIT, pass.historyLayers);
    if (!pending.fragColor.image)
        return false;
    pending.fragDepth = CreateTransientImage(dsFormat, pass.extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SAMPLE_COUNT_1_BIT,
        pass.historyLayers);
    if (!pending.fragDepth.image)
        return false;
    if (!CreateStaging((VkDeviceSize)pending.targetTexel * slots, pending.fragValues))
        return false;
    if (!CreateStaging((VkDeviceSize)4 * slots, pending.fragIds))
        return false;
    pending.fragSlots = slots;
    return true;
}

VkRenderPass Replayer::HistoryRenderPass(uint64_t renderPassId)
{
    const auto key = std::make_pair(renderPassId, _historyView);
    auto it = _historyRenderPasses.find(key);
    if (it != _historyRenderPasses.end())
        return it->second;
    _historyRenderPasses[key] = VK_NULL_HANDLE;
    const JValue* object = _capture->Object(renderPassId);
    const JValue* args = object ? object->Get("args") : nullptr;
    if (!args)
        return VK_NULL_HANDLE;
    const std::string cmd = Str(object->Get("cmd"));
    const size_t problems = _ctx.problems.size();
    const size_t unresolved = _ctx.unresolved;
    VkRenderPass rp = VK_NULL_HANDLE;
    // Every attachment loads and stores, and stays in its attachment layout between passes.
    if (cmd == "vkCreateRenderPass")
    {
        Args_vkCreateRenderPass a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pCreateInfo)
        {
            VkRenderPassCreateInfo info = *a.pCreateInfo;
            std::vector<VkAttachmentDescription> attachments(info.pAttachments, info.pAttachments + info.attachmentCount);
            for (VkAttachmentDescription& att : attachments)
            {
                att.loadOp = att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                att.storeOp = att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
                att.initialLayout = att.finalLayout = AttachmentLayout(att.format);
            }
            info.pAttachments = attachments.data();
            // Following one view of a multiview pass: every subpass renders that view alone, and
            // what relates views to each other goes (a view offset or a correlation names others).
            std::vector<uint32_t> masks;
            for (auto* s = (VkBaseOutStructure*)const_cast<void*>(info.pNext); s && _historyView; s = s->pNext)
            {
                if (s->sType != VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)
                    continue;
                auto* mv = (VkRenderPassMultiviewCreateInfo*)s;
                masks.assign(mv->subpassCount, _historyView);
                mv->pViewMasks = masks.data();
                mv->dependencyCount = 0;
                mv->pViewOffsets = nullptr;
                mv->correlationMaskCount = 0;
                mv->pCorrelationMasks = nullptr;
            }
            _fns.CreateRenderPass(_device, &info, nullptr, &rp);
        }
    }
    else if (cmd == "vkCreateRenderPass2" || cmd == "vkCreateRenderPass2KHR")
    {
        Args_vkCreateRenderPass2 a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pCreateInfo)
        {
            VkRenderPassCreateInfo2 info = *a.pCreateInfo;
            std::vector<VkAttachmentDescription2> attachments(info.pAttachments, info.pAttachments + info.attachmentCount);
            for (VkAttachmentDescription2& att : attachments)
            {
                att.loadOp = att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                att.storeOp = att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
                att.initialLayout = att.finalLayout = AttachmentLayout(att.format);
            }
            info.pAttachments = attachments.data();
            std::vector<VkSubpassDescription2> subpasses(info.pSubpasses, info.pSubpasses + info.subpassCount);
            std::vector<VkSubpassDependency2> dependencies(info.pDependencies, info.pDependencies + info.dependencyCount);
            if (_historyView)
            {
                for (VkSubpassDescription2& s : subpasses)
                    if (s.viewMask)
                        s.viewMask = _historyView;
                for (VkSubpassDependency2& d : dependencies)
                    d.viewOffset = 0;
                info.pSubpasses = subpasses.data();
                info.pDependencies = dependencies.data();
                info.correlatedViewMaskCount = 0;
                info.pCorrelatedViewMasks = nullptr;
            }
            _fns.CreateRenderPass2(_device, &info, nullptr, &rp);
        }
    }
    _ctx.problems.resize(problems);
    _ctx.unresolved = unresolved;
    _arena.Reset();
    if (rp)
        Track("VkRenderPass", (uint64_t)rp);
    _historyRenderPasses[key] = rp;
    return rp;
}

Replayer::ScissorInfo Replayer::PipelineScissor(uint64_t pipelineId)
{
    auto it = _pipelineScissors.find(pipelineId);
    if (it != _pipelineScissors.end())
        return it->second;
    ScissorInfo info;
    if (_capture->Object(pipelineId))
    {
        // A pipeline linked from libraries has its viewport state in the pre-rasterization library's create info.
        info.dynamic = false;
        if (PipelineDynamic(pipelineId, "VK_DYNAMIC_STATE_SCISSOR"))
            info.dynamic = true;
        if (PipelineDynamic(pipelineId, "VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT"))
            info.dynamic = info.withCount = true;
        if (!info.dynamic)
        {
            if (const JValue* vs = PipelineState(pipelineId, "pViewportState", "PRE_RASTERIZATION_SHADERS"); vs && vs->Get("pScissors") && vs->Get("pScissors")->IsArray() && vs->Get("pScissors")->count)
            {
                info.rect = RectOf(&vs->Get("pScissors")->items[0]);
                info.hasRect = true;
            }
        }
    }
    _pipelineScissors[pipelineId] = info;
    return info;
}

uint32_t Replayer::CopyHistoryPixel(VkCommandBuffer cb, const PassState& pass, PendingHistory& pending, VkImageLayout layout)
{
    const uint32_t slot = pending.nextSlot++;
    const VkDeviceSize base = (VkDeviceSize)slot * (pending.targetTexel + pending.depthTexel);
    auto copy = [&](int attachment, VkDeviceSize offset) {
        const VkFormat format = pass.formats[attachment];
        const VkImage image = pass.shadows[attachment].image;
        const uint32_t layer = pass.historyLayer;
        const VkImageSubresourceRange range{vkinsp::FormatAspects(format), 0, 1, layer, 1};
        const VkImageLayout current = layout != VK_IMAGE_LAYOUT_UNDEFINED ? layout : AttachmentLayout(format);
        Barrier(cb, image, range, current, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImage source = image;
        VkOffset3D at{(int32_t)_options.history.x, (int32_t)_options.history.y, 0};
        // A multisampled attachment cannot be copied to a buffer at all, so the pixel's samples are
        // resolved into a one-pixel image of the replay's own and read from there: the value is what
        // the samples resolve to, which is what the image viewer shows of such a target as well.
        const bool resolving = pending.resolve.image && attachment == pass.historyAttachment;
        if (resolving)
        {
            const VkImageSubresourceRange one{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            // From UNDEFINED: the one texel it holds is overwritten, so its contents are worth nothing.
            Barrier(cb, pending.resolve.image, one, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageResolve r{};
            r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
            r.srcOffset = at;
            r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.extent = {1, 1, 1};
            _fns.CmdResolveImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pending.resolve.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            Barrier(cb, pending.resolve.image, one, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            source = pending.resolve.image;
            at = {0, 0, 0};
        }
        VkBufferImageCopy c{};
        c.bufferOffset = offset;
        c.imageSubresource = {IsDepthFormat(format) ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT, 0,
            resolving ? 0 : layer, 1};
        c.imageOffset = at;
        c.imageExtent = {1, 1, 1};
        _fns.CmdCopyImageToBuffer(cb, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pending.staging.buffer, 1, &c);
        Barrier(cb, image, range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, current);
    };
    copy(pass.historyAttachment, base);
    if (pending.depthTexel)
        copy(pass.historyDepthAttachment, base + pending.targetTexel);
    return slot;
}

void Replayer::PrepareHistory(VkCommandBuffer cb, PassState& pass, std::vector<PendingHistory>& histories)
{
    const auto& req = _options.history;
    PixelHistoryResult& out = _report->history;
    int target = -1;
    for (size_t a = 0; a < pass.views.size() && target < 0; ++a)
    {
        auto vit = _views.find(pass.views[a]);
        if (vit == _views.end())
            continue;
        const ViewRecord& v = vit->second;
        const uint32_t layers = v.range.layerCount == VK_REMAINING_ARRAY_LAYERS ? UINT32_MAX : std::max(1u, v.range.layerCount);
        if (v.image == req.image && v.range.baseMipLevel == req.mip && req.layer >= v.range.baseArrayLayer && req.layer - v.range.baseArrayLayer < layers)
            target = (int)a;
    }
    if (target < 0)
        return;
    ++_historyPasses;
    const std::string where = "command buffer " + std::to_string(pass.commandBuffer) + ", pass " + std::to_string(pass.index);
    auto note = [&](const std::string& why) { out.notes.push_back(where + ": " + why); };
    const ViewRecord& targetView = _views.find(pass.views[target])->second;
    // A layer past the view's first: a multiview pass renders it as the view of its index, which is
    // followed in a pass of that view alone (RecordHistory); a pass layered through gl_Layer is not.
    const uint32_t layer = req.layer - targetView.range.baseArrayLayer;
    if (layer && !pass.viewMask)
        return note("only the first layer of a layered pass is followed, unless the pass is multiview");
    if (pass.viewMask && !(pass.viewMask & (1u << layer)))
        return note("the multiview pass renders no view into layer " + std::to_string(req.layer));
    pass.historyLayer = layer;
    pass.historyLayers = layer + 1;
    if (!pass.extent.width || req.x >= pass.extent.width || req.y >= pass.extent.height)
        return note("the pixel is outside the pass's framebuffer");
    if (pass.formats.size() != pass.views.size())
        return note("the pass's attachment formats are not known");

    // Copies of every attachment, holding what the pass starts from.
    pass.shadows.assign(pass.views.size(), TransientImage{});
    int depthAttachment = -1;
    bool anyDepth = false;
    for (size_t a = 0; a < pass.views.size(); ++a)
    {
        auto vit = _views.find(pass.views[a]);
        auto iit = vit != _views.end() ? _images.find(vit->second.image) : _images.end();
        if (iit == _images.end())
            return note("an attachment of the pass was not replayed");
        const ImageRecord& image = iit->second;
        const ViewRecord& view = vit->second;
        const VkFormat format = pass.formats[a];
        const bool depth = IsDepthFormat(format);
        if ((int)a == target && depth && image.samples != VK_SAMPLE_COUNT_1_BIT)
            return note("the target is a multisampled depth attachment, which cannot be resolved to be read");
        if (depth)
            anyDepth = true;
        if (depth && depthAttachment < 0 && image.samples == VK_SAMPLE_COUNT_1_BIT)
            depthAttachment = (int)a;
        const VkImageUsageFlags usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        pass.shadows[a] = CreateTransientImage(format, pass.extent, usage, image.samples, pass.historyLayers);
        if (!pass.shadows[a].image)
            return note("no memory for copies of the pass's attachments");
        const VkImageAspectFlags aspects = vkinsp::FormatAspects(format);
        // Every layer up to the followed one, of which only that one is rendered and read.
        const VkImageSubresourceRange full{aspects, 0, 1, 0, pass.historyLayers};
        Barrier(cb, pass.shadows[a].image, full, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkAttachmentLoadOp load = a < pass.loadOps.size() ? pass.loadOps[a] : VK_ATTACHMENT_LOAD_OP_LOAD;
        const VkImageLayout before = a < pass.startLayouts.size() ? pass.startLayouts[a] : VK_IMAGE_LAYOUT_UNDEFINED;
        if (load != VK_ATTACHMENT_LOAD_OP_CLEAR && before != VK_IMAGE_LAYOUT_UNDEFINED)
        {
            const uint32_t from = view.range.baseArrayLayer + pass.historyLayer;
            const VkImageSubresourceRange srcRange{aspects, view.range.baseMipLevel, 1, from, 1};
            Barrier(cb, image.image, srcRange, before, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageCopy copy{};
            copy.srcSubresource = {aspects, view.range.baseMipLevel, from, 1};
            copy.dstSubresource = {aspects, 0, pass.historyLayer, 1};
            copy.extent = {std::min(pass.extent.width, std::max(1u, image.extent.width >> view.range.baseMipLevel)),
                std::min(pass.extent.height, std::max(1u, image.extent.height >> view.range.baseMipLevel)), 1};
            _fns.CmdCopyImage(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pass.shadows[a].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Barrier(cb, image.image, srcRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, before);
        }
        else
        {
            const VkClearValue clear = load == VK_ATTACHMENT_LOAD_OP_CLEAR && a < pass.clearValues.size() ? pass.clearValues[a] : VkClearValue{};
            if (depth)
                _fns.CmdClearDepthStencilImage(cb, pass.shadows[a].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear.depthStencil, 1, &full);
            else
                _fns.CmdClearColorImage(cb, pass.shadows[a].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear.color, 1, &full);
        }
    }

    PendingHistory pending;
    const VkFormat targetFormat = pass.formats[target];
    const VkSampleCountFlagBits targetSamples = _images.find(targetView.image) != _images.end()
        ? _images.find(targetView.image)->second.samples
        : VK_SAMPLE_COUNT_1_BIT;
    pending.targetTexel = vkinsp::FormatBlockInfo(targetFormat, IsDepthFormat(targetFormat) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT).bytes;
    if (!pending.targetTexel)
        return note("the image's format cannot be read back");
    if (depthAttachment >= 0)
        pending.depthTexel = vkinsp::FormatBlockInfo(pass.formats[depthAttachment], VK_IMAGE_ASPECT_DEPTH_BIT).bytes;
    pass.historyAttachment = target;
    pass.historyDepthAttachment = pending.depthTexel ? depthAttachment : -1;
    // A multisampled target is read through a resolve of the one pixel (see CopyHistoryPixel).
    if (targetSamples != VK_SAMPLE_COUNT_1_BIT)
    {
        pending.resolve = CreateTransientImage(targetFormat, {1, 1}, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_SAMPLE_COUNT_1_BIT);
        if (!pending.resolve.image)
            return note("no memory to resolve the multisampled target's pixel into");
        note("the target is multisampled: each value is what the pixel's samples resolve to, the counts are of samples rather than fragments, and no primitive is named (the primitive-id pass draws into a single-sampled target of its own)");
    }
    pending.fragFormat = targetFormat;
    if (out.format.empty())
        out.format = EnumName(kEnum_VkFormat, kEnumCount_VkFormat, targetFormat);
    if (out.depthFormat.empty() && pending.depthTexel)
        out.depthFormat = EnumName(kEnum_VkFormat, kEnumCount_VkFormat, pass.formats[depthAttachment]);

    // Room for the pass's events: its start, and every draw and clear up to its end.
    const JValue* commands = _capture->Commands();
    uint32_t draws = 0;
    uint32_t clears = 0;
    for (uint32_t i = pass.beginIndex + 1; i < commands->count; ++i)
    {
        const JValue& c = commands->items[i];
        const std::string m = Str(c.Get("method"));
        if (!c.Get("secondary") && IsEndPass(m))
            break;
        if (StartsWith(m, "vkCmdDraw"))
            ++draws;
        else if (m == "vkCmdClearAttachments")
            ++clears;
    }
    if (!CreateStaging((VkDeviceSize)(draws + clears + 1) * (pending.targetTexel + pending.depthTexel), pending.staging))
        return note("no staging memory for the pixel's values");
    // The second replay: the draws of this pass broken into fragments (kMaxHistoryFragments each).
    // Nothing else of the round is needed -- the values, the counts and the winning primitive are
    // the first replay's -- so its queries and its primitive-id target are left out below.
    if (_historyFragmentRound)
    {
        if (!draws || targetSamples != VK_SAMPLE_COUNT_1_BIT ||
            !PrepareHistoryFragments(pending, pass, draws * kMaxHistoryFragments))
        {
            note("the pass's draws could not be measured fragment by fragment");
        }
        else
        {
            // The primitive of each fragment is written as a uint, which needs a target of that
            // format beside the one holding what its shader computed.
            if (_primitiveIdAvailable)
            {
                pending.idTarget = CreateTransientImage(kPrimitiveIdFormat, pass.extent,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_SAMPLE_COUNT_1_BIT, pass.historyLayers);
                if (pending.idTarget.image)
                    Barrier(cb, pending.idTarget.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, pass.historyLayers}, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            }
            Barrier(cb, pending.fragColor.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, pass.historyLayers}, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            Barrier(cb, pending.fragDepth.image, {vkinsp::FormatAspects(HistoryFragmentDepthFormat()), 0, 1, 0, pass.historyLayers},
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
    }
    // The primitive-id pass: a target of the pass's size, and a slot per draw to read it from.
    // Single-sampled passes only — the target it draws into is single-sampled, and a render pass
    // mixes no sample counts — and only where gl_PrimitiveID can be read at all.
    // A pass whose depth cannot be attached to it is left out: without the depth the pass tests
    // against, the primitive left in the pixel would not be the one that won it.
    if (!_historyFragmentRound && draws && _primitiveIdAvailable && targetSamples == VK_SAMPLE_COUNT_1_BIT && (!anyDepth || depthAttachment >= 0))
    {
        pending.idTarget = CreateTransientImage(kPrimitiveIdFormat, pass.extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_SAMPLE_COUNT_1_BIT, pass.historyLayers);
        if (pending.idTarget.image && CreateStaging((VkDeviceSize)draws * 4, pending.ids))
        {
            pending.idSlots = draws;
            pending.idDepth = depthAttachment;
            Barrier(cb, pending.idTarget.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, pass.historyLayers}, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            // The depth the id pass tests against and writes: a copy of the pass's, so the draw's
            // own depth writes decide which of its fragments the pixel keeps.
            if (depthAttachment >= 0)
            {
                const VkFormat depthFormat = pass.formats[depthAttachment];
                pending.idDepthCopy = CreateTransientImage(depthFormat, pass.extent,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_SAMPLE_COUNT_1_BIT, pass.historyLayers);
                if (pending.idDepthCopy.image)
                    Barrier(cb, pending.idDepthCopy.image, {vkinsp::FormatAspects(depthFormat), 0, 1, 0, pass.historyLayers}, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                else
                    pending.idDepth = -1;   // without the copy the pass would write the events' own depth
            }
        }
        else
        {
            pending.idTarget = TransientImage{};
        }
    }
    if (draws && !_historyFragmentRound)
    {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = draws * kVariantCount;
        if (_fns.CreateQueryPool(_device, &qi, nullptr, &pending.queries) == VK_SUCCESS)
        {
            pending.queryCount = qi.queryCount;
            _fns.CmdResetQueryPool(cb, pending.queries, 0, pending.queryCount);
        }
    }

    if (!_historyFragmentRound)
    {
        PixelEvent start;
        start.kind = "load";
        start.command = pass.beginIndex;
        start.method = Str(commands->items[pass.beginIndex].Get("method"));
        start.detail = EnumName(kEnum_VkAttachmentLoadOp, kEnumCount_VkAttachmentLoadOp, target < (int)pass.loadOps.size() ? pass.loadOps[target] : VK_ATTACHMENT_LOAD_OP_LOAD);
        start.commandBuffer = pass.commandBuffer;
        start.frame = pass.frame;
        start.passIndex = pass.index;
        PendingHistory::Entry entry;
        entry.event = out.events.size();
        entry.slot = CopyHistoryPixel(cb, pass, pending, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        pending.entries.push_back(entry);
        out.events.push_back(std::move(start));
    }
    for (size_t a = 0; a < pass.shadows.size(); ++a)
    {
        Barrier(cb, pass.shadows[a].image, {vkinsp::FormatAspects(pass.formats[a]), 0, 1, 0, pass.historyLayers}, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            AttachmentLayout(pass.formats[a]));
    }
    pass.history = true;
    pass.historyPending = histories.size();
    histories.push_back(std::move(pending));
}

void Replayer::RecordHistory(VkCommandBuffer cb, const CommandGroup& group, PassState& pass, uint32_t endIndex, std::vector<PendingHistory>& histories)
{
    if (!pass.history)
        return;
    pass.history = false;
    PendingHistory& pending = histories[pass.historyPending];
    PixelHistoryResult& out = _report->history;
    const JValue* commands = _capture->Commands();
    const bool dynamic = pass.renderPass == 0;
    const std::string where = "command buffer " + std::to_string(pass.commandBuffer) + ", pass " + std::to_string(pass.index);
    // A multiview pass is followed in the view the pixel is in, alone (see _historyView).
    _historyView = pass.viewMask ? 1u << pass.historyLayer : 0;
    _historyViewRenderPass = pass.renderPass;
    struct ViewReset
    {
        Replayer& r;
        ~ViewReset()
        {
            r._historyView = 0;
            r._historyViewRenderPass = 0;
        }
    } viewReset{*this};
    const uint32_t layer = pass.historyLayer;
    const uint32_t layers = pass.historyLayers;

    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (!dynamic)
    {
        rp = HistoryRenderPass(pass.renderPass);
        if (rp)
        {
            std::vector<VkImageView> views;
            for (const TransientImage& s : pass.shadows)
                views.push_back(s.view);
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = rp;
            info.attachmentCount = (uint32_t)views.size();
            info.pAttachments = views.data();
            info.width = pass.extent.width;
            info.height = pass.extent.height;
            info.layers = 1;
            if (_fns.CreateFramebuffer(_device, &info, nullptr, &fb) == VK_SUCCESS)
                _transientFramebuffers.push_back(fb);
        }
        if (!rp || !fb)
        {
            out.notes.push_back(where + ": the pass could not be rebuilt to follow the pixel through it");
            return;
        }
    }

    uint32_t subpass = 0;
    uint64_t pipeline = 0;
    // Shader objects bound in place of a pipeline (pipeline is then 0), and the fragment one of them.
    bool shaderObjects = false;
    uint64_t fragmentShader = 0;
    uint64_t layoutShader = 0;   // any of them: the replay's fragment shaders are made with its layouts
    ShaderObjectSets sets;
    bool scissorSet = false;
    VkRect2D scissor{};
    std::vector<uint32_t> dynamicCommands;
    // The bindings the pass made (descriptor sets, vertex and index buffers), for one view of a multiview pass.
    std::vector<uint32_t> bindCommands;

    auto beginPass = [&]() {
        if (!dynamic)
        {
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = rp;
            begin.framebuffer = fb;
            begin.renderArea = {{0, 0}, pass.extent};
            _fns.CmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
            for (uint32_t s = 0; s < subpass; ++s)
                _fns.CmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
            return;
        }
        auto attachment = [&](int index, VkImageLayout layout) {
            VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            if (index >= 0)
            {
                a.imageView = pass.shadows[index].view;
                a.imageLayout = layout;
                a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            return a;
        };
        std::vector<VkRenderingAttachmentInfo> colors;
        for (int slot : pass.dynamicColorSlots)
            colors.push_back(attachment(slot, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
        VkRenderingAttachmentInfo depth = attachment(pass.dynamicDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo stencil = attachment(pass.dynamicStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, pass.extent};
        info.layerCount = 1;
        info.viewMask = _historyView;
        info.colorAttachmentCount = (uint32_t)colors.size();
        info.pColorAttachments = colors.data();
        if (pass.dynamicDepth >= 0)
            info.pDepthAttachment = &depth;
        if (pass.dynamicStencil >= 0)
            info.pStencilAttachment = &stencil;
        _fns.CmdBeginRendering(cb, &info);
    };
    auto endPass = [&]() {
        if (!dynamic)
            _fns.CmdEndRenderPass(cb);
        else
            _fns.CmdEndRendering(cb);
    };
    // A command recorded as it was captured; its problems were reported when the pass was replayed.
    auto issue = [&](uint32_t index) {
        const JValue& c = commands->items[index];
        const JValue* args = c.Get("args");
        ReplayFn fn = FindReplayCommand(Str(c.Get("method")));
        if (!fn || !args)
            return;
        const size_t problems = _ctx.problems.size();
        const size_t unresolved = _ctx.unresolved;
        IssueCommand(fn, c, *args, cb);
        _ctx.problems.resize(problems);
        _ctx.unresolved = unresolved;
        _arena.Reset();
    };
    // Bindings and dynamic state, issued between passes where they stay in effect.
    auto state = [&](uint32_t index) {
        const JValue& c = commands->items[index];
        const std::string m = Str(c.Get("method"));
        const JValue* args = c.Get("args");
        if (args && m == "vkCmdBindShadersEXT")
        {
            const JValue* stages = args->Get("pStages");
            const JValue* shaders = args->Get("pShaders");
            for (uint32_t k = 0; stages && k < stages->count; ++k)
            {
                const std::string stage = Str(&stages->items[k]);
                if (stage == "VK_SHADER_STAGE_COMPUTE_BIT")
                    continue;
                shaderObjects = true;
                pipeline = 0;
                const uint64_t id = shaders && shaders->IsArray() && k < shaders->count ? IdOf(&shaders->items[k]) : 0;
                if (id)
                    layoutShader = id;
                if (stage == "VK_SHADER_STAGE_FRAGMENT_BIT")
                    fragmentShader = id;
            }
            issue(index);
            return;
        }
        if (!args || !IsStateCommand(m))
            return;
        if (m == "vkCmdBindPipeline")
        {
            if (Str(args->Get("pipelineBindPoint")) == "VK_PIPELINE_BIND_POINT_GRAPHICS")
            {
                pipeline = IdOf(args->Get("pipeline"));
                shaderObjects = false;
                // One view of a multiview pass: the pipeline's copy made for it.
                if (_historyView)
                {
                    if (VkPipeline view = ViewPipeline(pipeline))
                        _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, view);
                    return;
                }
            }
        }
        else if (StartsWith(m, "vkCmdBindDescriptorSets") || StartsWith(m, "vkCmdBindVertexBuffers") ||
            StartsWith(m, "vkCmdBindIndexBuffer") || StartsWith(m, "vkCmdPushDescriptorSet"))
        {
            bindCommands.push_back(index);
        }
        else if (StartsWith(m, "vkCmdPushConstants"))
        {
            // Pushed again after every pipeline the history binds, with the dynamic state.
            dynamicCommands.push_back(index);
        }
        else if (StartsWith(m, "vkCmdSet"))
        {
            dynamicCommands.push_back(index);
            NoteShaderObjectSet(sets, m, *args);
            if (StartsWith(m, "vkCmdSetScissor"))
            {
                if (const JValue* list = args->Get("pScissors"); list && list->IsArray() && list->count)
                {
                    scissor = RectOf(&list->items[0]);
                    scissorSet = true;
                }
            }
        }
        issue(index);
    };
    // Where the pixel sits in a draw's scissor: the same answer for the queries, for the
    // primitive-id pass and for the event itself, so it is worked out once.
    struct ScissorPlace
    {
        VkRect2D rect{};
        bool inside = false;
        bool withCount = false;
    };
    auto placeOf = [&](uint64_t pipelineId) {
        ScissorPlace place;
        // Shader objects set their scissors dynamically, with a count.
        ScissorInfo info = pipelineId ? PipelineScissor(pipelineId) : ScissorInfo{};
        if (shaderObjects)
            info.withCount = true;
        place.rect = scissorSet ? scissor : VkRect2D{{0, 0}, pass.extent};
        if (!info.dynamic && info.hasRect)
            place.rect = info.rect;
        place.withCount = info.withCount;
        const int64_t x = _options.history.x;
        const int64_t y = _options.history.y;
        place.inside = x >= place.rect.offset.x && x < (int64_t)place.rect.offset.x + place.rect.extent.width &&
            y >= place.rect.offset.y && y < (int64_t)place.rect.offset.y + place.rect.extent.height;
        return place;
    };

    // The draw's own pipeline, as the pass binds it: in one view of a multiview pass, its copy for that view.
    auto ownPipeline = [&]() {
        return _historyView ? ViewPipeline(pipeline) : (VkPipeline)(uintptr_t)Handle(pipeline);
    };
    // The dynamic state set so far, again, after a pipeline was bound: only what it takes dynamically
    // (the copy's own list, or the captured pipeline's), since it may not set what the pipeline holds.
    auto issueDynamic = [&](VkPipeline copy) {
        // One view of a multiview pass: the bindings again too, in the pass instance the draw is in.
        // The validation layer reports what was bound before a multiview pass began as unbound in
        // it (the draws themselves find their buffers), so they are made again where it looks.
        if (_historyView)
            for (uint32_t b : bindCommands)
                issue(b);
        for (uint32_t d : dynamicCommands)
        {
            const std::string state = DynamicStateOf(Str(commands->items[d].Get("method")));
            if (state.empty() || TakesDynamic(copy, pipeline, state))
                issue(d);
        }
    };
    // One view of a multiview pass: the draw's own pipeline bound again inside the pass instance,
    // since the one bound before the single-view pass began is not what the draw is checked against.
    auto bindOwnInPass = [&]() {
        if (!_historyView || !pipeline || shaderObjects)
            return;
        _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ownPipeline());
        issueDynamic(VK_NULL_HANDLE);
    };
    // A shader-object draw has no pipeline to copy: its fragment stage is bound to `fragment` and its
    // dynamic state set again with the edit over it (shader_objects.cpp), then both put back.
    auto editShaders = [&](VkShaderEXT fragment, const ShaderObjectEdit& edit) {
        BindFragmentShader(cb, fragment);
        for (uint32_t d : dynamicCommands)
            issue(d);
        ApplyShaderObjectEdit(cb, edit, sets);
    };
    auto restoreShaders = [&]() {
        BindFragmentShader(cb, (VkShaderEXT)(uintptr_t)Handle(fragmentShader));
        for (uint32_t d : dynamicCommands)
            issue(d);
    };

    // The draw once more into the primitive-id target, in a render pass of the replay's own, with
    // the depth and stencil this event starts from: what stays in the pixel is the primitive of the
    // fragment that won it. Runs before the event's own pass instance, which writes that depth.
    auto measurePrimitive = [&](uint32_t index, const ScissorPlace& place, PendingHistory::Entry& entry) {
        if ((!pipeline && !shaderObjects) || !pending.idTarget.image || pending.nextId >= pending.idSlots)
            return;
        const VkFormat depthFormat = pending.idDepth >= 0 ? pass.formats[pending.idDepth] : VK_FORMAT_UNDEFINED;
        // Shader objects draw in dynamic rendering, into the same images.
        VkShaderEXT idShader = shaderObjects && _fns.CmdBeginRendering ? ReplacementFragment(ReplacementShader::PrimitiveId, layoutShader) : VK_NULL_HANDLE;
        VkRenderPass rp = shaderObjects ? VK_NULL_HANDLE : HistoryIdRenderPass(depthFormat);
        VkPipeline idPipeline = shaderObjects ? VK_NULL_HANDLE : HistoryIdPipeline(pipeline, depthFormat);
        if (shaderObjects ? !idShader : !rp || !idPipeline)
            return;
        if (!shaderObjects && !pending.idFramebuffer)
        {
            std::vector<VkImageView> views{pending.idTarget.view};
            if (pending.idDepth >= 0)
                views.push_back(pending.idDepthCopy.view);
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = rp;
            info.attachmentCount = (uint32_t)views.size();
            info.pAttachments = views.data();
            info.width = pass.extent.width;
            info.height = pass.extent.height;
            info.layers = 1;
            if (_fns.CreateFramebuffer(_device, &info, nullptr, &pending.idFramebuffer) != VK_SUCCESS)
            {
                pending.idFramebuffer = VK_NULL_HANDLE;
                return;
            }
            _transientFramebuffers.push_back(pending.idFramebuffer);
        }
        const uint32_t slot = pending.nextId++;
        const VkRect2D pixel{{(int32_t)_options.history.x, (int32_t)_options.history.y}, {1, 1}};
        // The depth this draw meets, copied out of the pass's own so the id pass may write it.
        if (pending.idDepth >= 0 && pending.idDepthCopy.image)
        {
            const VkImageAspectFlags aspects = vkinsp::FormatAspects(pass.formats[pending.idDepth]);
            const VkImageSubresourceRange range{aspects, 0, 1, 0, layers};
            Barrier(cb, pass.shadows[pending.idDepth].image, range, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Barrier(cb, pending.idDepthCopy.image, range, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy region{};
            region.srcSubresource = {aspects, 0, layer, 1};
            region.dstSubresource = region.srcSubresource;
            region.extent = {pass.extent.width, pass.extent.height, 1};
            _fns.CmdCopyImage(cb, pass.shadows[pending.idDepth].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                pending.idDepthCopy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            Barrier(cb, pass.shadows[pending.idDepth].image, range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            Barrier(cb, pending.idDepthCopy.image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
        // 0 for "no fragment of this draw wrote it": the shader writes the index plus one.
        VkClearValue clear{};
        if (shaderObjects)
        {
            // What HistoryIdRenderPass and HistoryIdPipeline make of a pipeline's draw.
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = pending.idTarget.view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue = clear;
            VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depth.imageView = pending.idDepthCopy.view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            const VkImageAspectFlags aspects = depthFormat != VK_FORMAT_UNDEFINED ? vkinsp::FormatAspects(depthFormat) : 0;
            VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
            info.renderArea = pixel;   // the clear and the rasterization are the one pixel
            info.layerCount = 1;
            info.viewMask = _historyView;
            info.colorAttachmentCount = 1;
            info.pColorAttachments = &color;
            info.pDepthAttachment = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? &depth : nullptr;
            info.pStencilAttachment = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? &depth : nullptr;
            _fns.CmdBeginRendering(cb, &info);
            ShaderObjectEdit edit;
            edit.colorAttachments = 1;
            edit.writeMask = VK_COLOR_COMPONENT_R_BIT;
            edit.singleSample = true;
            edit.depthTest = edit.depthWrite = edit.stencilTest = edit.stencilWrite = depthFormat != VK_FORMAT_UNDEFINED;
            edit.scissor = &pixel;
            editShaders(idShader, edit);
            issue(index);
            _fns.CmdEndRendering(cb);
            restoreShaders();
        }
        else
        {
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = rp;
            begin.framebuffer = pending.idFramebuffer;
            begin.renderArea = pixel;   // the clear and the rasterization are the one pixel
            begin.clearValueCount = 1;
            begin.pClearValues = &clear;
            _fns.CmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
            _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, idPipeline);
            issueDynamic(idPipeline);
            if (place.withCount)
                _fns.CmdSetScissorWithCount(cb, 1, &pixel);
            else
                _fns.CmdSetScissor(cb, 0, 1, &pixel);
            issue(index);
            _fns.CmdEndRenderPass(cb);
            _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ownPipeline());
            issueDynamic(VK_NULL_HANDLE);
        }
        const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        Barrier(cb, pending.idTarget.image, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = (VkDeviceSize)slot * 4;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        c.imageOffset = {pixel.offset.x, pixel.offset.y, 0};
        c.imageExtent = {1, 1, 1};
        _fns.CmdCopyImageToBuffer(cb, pending.idTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pending.ids.buffer, 1, &c);
        Barrier(cb, pending.idTarget.image, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // The event's own pass writes the depth this one copied: ordered, or the two overlap.
        if (pending.idDepth >= 0)
        {
            const VkImageSubresourceRange depth{vkinsp::FormatAspects(pass.formats[pending.idDepth]), 0, 1, 0, layers};
            Barrier(cb, pass.shadows[pending.idDepth].image, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
        entry.idSlot = (int32_t)slot;
    };

    // measureFragments for a shader-object draw: the same two runs per fragment, in dynamic
    // rendering, with the state HistoryFragmentPipeline bakes into a pipeline's copy set instead.
    auto measureShaderFragments = [&](uint32_t index, uint64_t count, size_t eventIndex) {
        const VkShaderEXT idShader = pending.idTarget.image ? ReplacementFragment(ReplacementShader::PrimitiveId, layoutShader) : VK_NULL_HANDLE;
        if (!_fns.CmdBeginRendering)
            return;
        const VkRect2D pixel{{(int32_t)_options.history.x, (int32_t)_options.history.y}, {1, 1}};
        const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        const VkImageAspectFlags aspects = vkinsp::FormatAspects(HistoryFragmentDepthFormat());
        ShaderObjectEdit edit;
        edit.colorAttachments = 1;
        edit.writeMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        edit.singleSample = true;
        edit.depthTest = edit.depthWrite = false;
        edit.stencilCounter = true;
        edit.scissor = &pixel;
        for (uint64_t f = 0; f < count && pending.nextFrag < pending.fragSlots; ++f)
        {
            const uint32_t slot = pending.nextFrag++;
            for (int run = 0; run < 2; ++run)
            {
                if (run == 1 && !idShader)
                    continue;
                const TransientImage& target = run == 0 ? pending.fragColor : pending.idTarget;
                VkRenderingAttachmentInfo out{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                out.imageView = target.view;
                out.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                out.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingAttachmentInfo counter{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                counter.imageView = pending.fragDepth.view;
                counter.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                counter.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                counter.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                counter.clearValue.depthStencil = {1.0f, 0};
                VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
                info.renderArea = pixel;   // the clears and the rasterization are the one pixel
                info.layerCount = 1;
                info.viewMask = _historyView;
                info.colorAttachmentCount = 1;
                info.pColorAttachments = &out;
                info.pDepthAttachment = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? &counter : nullptr;
                info.pStencilAttachment = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? &counter : nullptr;
                _fns.CmdBeginRendering(cb, &info);
                // The draw's own fragment shader for the value, the primitive-id one for the primitive.
                editShaders(run == 0 ? (VkShaderEXT)(uintptr_t)Handle(fragmentShader) : idShader, edit);
                _fns.CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)f);
                issue(index);
                _fns.CmdEndRendering(cb);
                Barrier(cb, target.image, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy c{};
                c.bufferOffset = run == 0 ? (VkDeviceSize)slot * pending.targetTexel : (VkDeviceSize)slot * 4;
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
                c.imageOffset = {pixel.offset.x, pixel.offset.y, 0};
                c.imageExtent = {1, 1, 1};
                _fns.CmdCopyImageToBuffer(cb, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    run == 0 ? pending.fragValues.buffer : pending.fragIds.buffer, 1, &c);
                Barrier(cb, target.image, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            }
            PendingHistory::FragmentEntry fe;
            fe.event = eventIndex;
            fe.index = (uint32_t)f;
            fe.slot = slot;
            pending.fragmentEntries.push_back(fe);
        }
        restoreShaders();
    };

    // The draw once per fragment, into the images of the fragment round: the stencil counts its
    // fragments and lets through the one whose index is the reference, so each run says what one
    // fragment computed and which primitive it came from.
    auto measureFragments = [&](uint32_t index, const ScissorPlace& place, size_t eventIndex) {
        if ((!pipeline && !shaderObjects) || !pending.fragColor.image || !pending.fragDepth.image)
            return;
        PixelEvent& e = out.events[eventIndex];
        // The fragments the draw rasterized: `facing` is `covered` with its own culling applied,
        // and culled geometry never became a fragment. A draw with one is what its own entry
        // already reports.
        const uint64_t rasterized = (e.testsMeasured & (1u << kFacing)) ? e.facing : e.covered;
        const uint64_t count = std::min<uint64_t>(rasterized, kMaxHistoryFragments);
        if (count < 2)
            return;
        if (shaderObjects)
            return measureShaderFragments(index, count, eventIndex);
        VkRenderPass valuePass = HistoryFragmentRenderPass(pending.fragFormat);
        VkPipeline valuePipeline = HistoryFragmentPipeline(pipeline, pending.fragFormat, false);
        if (!valuePass || !valuePipeline)
            return;
        VkRenderPass idPass = pending.idTarget.image ? HistoryFragmentRenderPass(kPrimitiveIdFormat) : VK_NULL_HANDLE;
        VkPipeline idPipeline = idPass ? HistoryFragmentPipeline(pipeline, kPrimitiveIdFormat, true) : VK_NULL_HANDLE;
        // One framebuffer per target, both over the same counter.
        auto framebufferOf = [&](VkRenderPass rp, const TransientImage& color, VkFramebuffer& out) {
            if (out)
                return true;
            std::vector<VkImageView> views{color.view, pending.fragDepth.view};
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = rp;
            info.attachmentCount = (uint32_t)views.size();
            info.pAttachments = views.data();
            info.width = pass.extent.width;
            info.height = pass.extent.height;
            info.layers = 1;
            if (_fns.CreateFramebuffer(_device, &info, nullptr, &out) != VK_SUCCESS)
            {
                out = VK_NULL_HANDLE;
                return false;
            }
            _transientFramebuffers.push_back(out);
            return true;
        };
        if (!framebufferOf(valuePass, pending.fragColor, pending.fragColorFramebuffer))
            return;
        if (idPipeline && !framebufferOf(idPass, pending.idTarget, pending.fragIdFramebuffer))
            idPipeline = VK_NULL_HANDLE;
        const VkRect2D pixel{{(int32_t)_options.history.x, (int32_t)_options.history.y}, {1, 1}};
        const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        for (uint64_t f = 0; f < count && pending.nextFrag < pending.fragSlots; ++f)
        {
            const uint32_t slot = pending.nextFrag++;
            // Two runs: the draw's own shader for the value, then the primitive-id shader. Each
            // starts from a cleared counter, so the fragment that matches `f` is the f-th one.
            for (int run = 0; run < 2; ++run)
            {
                VkPipeline copy = run == 0 ? valuePipeline : idPipeline;
                if (!copy)
                    continue;
                const TransientImage& target = run == 0 ? pending.fragColor : pending.idTarget;
                VkClearValue clears[2]{};
                clears[1].depthStencil = {1.0f, 0};
                VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                begin.renderPass = run == 0 ? valuePass : idPass;
                begin.framebuffer = run == 0 ? pending.fragColorFramebuffer : pending.fragIdFramebuffer;
                begin.renderArea = pixel;   // the clears and the rasterization are the one pixel
                begin.clearValueCount = 2;
                begin.pClearValues = clears;
                _fns.CmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
                _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, copy);
                issueDynamic(copy);
                if (place.withCount)
                    _fns.CmdSetScissorWithCount(cb, 1, &pixel);
                else
                    _fns.CmdSetScissor(cb, 0, 1, &pixel);
                _fns.CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)f);
                issue(index);
                _fns.CmdEndRenderPass(cb);
                Barrier(cb, target.image, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy c{};
                c.bufferOffset = run == 0 ? (VkDeviceSize)slot * pending.targetTexel : (VkDeviceSize)slot * 4;
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
                c.imageOffset = {pixel.offset.x, pixel.offset.y, 0};
                c.imageExtent = {1, 1, 1};
                _fns.CmdCopyImageToBuffer(cb, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    run == 0 ? pending.fragValues.buffer : pending.fragIds.buffer, 1, &c);
                Barrier(cb, target.image, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            }
            PendingHistory::FragmentEntry fe;
            fe.event = eventIndex;
            fe.index = (uint32_t)f;
            fe.slot = slot;
            pending.fragmentEntries.push_back(fe);
        }
        // The draw's own pipeline again, for the draw itself.
        _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ownPipeline());
        issueDynamic(VK_NULL_HANDLE);
    };

    auto event = [&](uint32_t index) {
        const JValue& c = commands->items[index];
        const std::string m = Str(c.Get("method"));
        if (StartsWith(m, "vkCmdNextSubpass"))
        {
            ++subpass;
            return;
        }
        const bool draw = StartsWith(m, "vkCmdDraw");
        if (!draw && m != "vkCmdClearAttachments")
        {
            state(index);
            return;
        }
        const ScissorPlace place = draw ? placeOf(pipeline) : ScissorPlace{};
        // The second replay adds no events: it finds the one this draw already has and breaks it
        // into fragments. The pass is still replayed around it, so each draw meets the depth,
        // stencil and color its own turn left.
        if (_historyFragmentRound)
        {
            if (draw && place.inside)
            {
                auto it = _historyEventIndex.find(std::make_tuple(pass.frame, pass.commandBuffer, pass.index, index));
                if (it != _historyEventIndex.end())
                    measureFragments(index, place, it->second);
            }
            beginPass();
            if (draw)
                bindOwnInPass();
            issue(index);
            endPass();
            return;
        }
        PixelEvent e;
        e.kind = draw ? "draw" : "clear";
        e.command = index;
        e.method = m;
        e.commandBuffer = pass.commandBuffer;
        e.frame = pass.frame;
        e.passIndex = pass.index;
        PendingHistory::Entry entry;
        entry.event = out.events.size();
        if (draw)
        {
            e.pipeline = pipeline;
            e.earlyTests = shaderObjects ? HistoryEarlyFragmentTests(fragmentShader) : pipeline && HistoryEarlyFragmentTests(pipeline);
            e.scissored = !place.inside;
            if (place.inside)
                measurePrimitive(index, place, entry);
        }
        beginPass();
        if (draw)
        {
            bindOwnInPass();
            if (place.inside && shaderObjects && pending.queries && pending.nextQuery + kVariantCount <= pending.queryCount)
            {
                // What HistoryPipeline makes of a pipeline's draw, variant for variant.
                entry.queryBase = (int32_t)pending.nextQuery;
                pending.nextQuery += kVariantCount;
                const VkRect2D pixel{{(int32_t)_options.history.x, (int32_t)_options.history.y}, {1, 1}};
                const VkShaderEXT own = (VkShaderEXT)(uintptr_t)Handle(fragmentShader);
                for (int v = 0; v < kVariantCount; ++v)
                {
                    const bool counting = v == kCovered || v == kFacing;
                    const VkShaderEXT fragment = counting ? ReplacementFragment(ReplacementShader::Count, layoutShader) : own;
                    if (counting && !fragment)
                        continue;
                    ShaderObjectEdit edit;
                    edit.colorAttachments = (uint32_t)pass.dynamicColorSlots.size();
                    edit.writeMask = 0;
                    edit.depthWrite = edit.stencilWrite = false;
                    edit.depthTest = v == kDepthOnly || v == kAllTests || (e.earlyTests && v == kShaded);
                    edit.stencilTest = v == kStencilOnly || v == kAllTests || (e.earlyTests && v == kShaded);
                    edit.cullNone = v == kCovered;
                    edit.scissor = &pixel;
                    editShaders(fragment, edit);
                    _fns.CmdBeginQuery(cb, pending.queries, (uint32_t)entry.queryBase + v, 0);
                    issue(index);
                    _fns.CmdEndQuery(cb, pending.queries, (uint32_t)entry.queryBase + v);
                    entry.issued |= 1u << v;
                }
                restoreShaders();
            }
            else if (place.inside && pipeline && pending.queries && pending.nextQuery + kVariantCount <= pending.queryCount)
            {
                entry.queryBase = (int32_t)pending.nextQuery;
                pending.nextQuery += kVariantCount;
                const VkRect2D pixel{{(int32_t)_options.history.x, (int32_t)_options.history.y}, {1, 1}};
                for (int v = 0; v < kVariantCount; ++v)
                {
                    VkPipeline copy = HistoryPipeline(pipeline, v);
                    if (!copy)
                        continue;
                    // Binding a pipeline with a state static undoes that state's dynamic value: set again.
                    _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, copy);
                    issueDynamic(copy);
                    if (place.withCount)
                        _fns.CmdSetScissorWithCount(cb, 1, &pixel);
                    else
                        _fns.CmdSetScissor(cb, 0, 1, &pixel);
                    _fns.CmdBeginQuery(cb, pending.queries, (uint32_t)entry.queryBase + v, 0);
                    issue(index);
                    _fns.CmdEndQuery(cb, pending.queries, (uint32_t)entry.queryBase + v);
                    entry.issued |= 1u << v;
                }
                // The draw's own pipeline and dynamic state again.
                _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ownPipeline());
                issueDynamic(VK_NULL_HANDLE);
            }
        }
        issue(index);
        endPass();
        entry.slot = CopyHistoryPixel(cb, pass, pending, VK_IMAGE_LAYOUT_UNDEFINED);
        pending.entries.push_back(entry);
        out.events.push_back(std::move(e));
    };

    // The state the pass inherited from its command buffer, then its own commands (secondary
    // command buffers' inline, where they executed).
    for (uint32_t i = group.first + 1; i < pass.beginIndex; ++i)
        if (!commands->items[i].Get("secondary"))
            state(i);
    for (uint32_t i = pass.beginIndex + 1; i < endIndex; ++i)
    {
        const JValue& c = commands->items[i];
        if (c.Get("secondary"))
            continue;
        if (Str(c.Get("method")) == "vkCmdExecuteCommands")
        {
            const JValue* list = c.Get("args") ? c.Get("args")->Get("pCommandBuffers") : nullptr;
            for (uint32_t s = 0; list && s < list->count; ++s)
            {
                const uint64_t id = IdOf(&list->items[s]);
                for (uint32_t j = i + 1; j < commands->count && commands->items[j].Get("secondary"); ++j)
                    if (commands->items[j].Get("secondary")->Uint() == id)
                        event(j);
            }
            continue;
        }
        event(i);
    }
}

bool Replayer::HistoryHasMultipleFragments() const
{
    // `facing` is what a draw rasterized; `covered` counts the geometry its culling threw away, so
    // a draw with one fragment after culling has nothing to break down.
    for (const PixelEvent& e : _report->history.events)
    {
        const uint64_t rasterized = (e.testsMeasured & (1u << kFacing)) ? e.facing : e.covered;
        if (rasterized > 1)
            return true;
    }
    return false;
}

void Replayer::CompleteHistory(std::vector<PendingHistory>& histories)
{
    PixelHistoryResult& out = _report->history;
    for (PendingHistory& h : histories)
    {
        const auto* bytes = static_cast<const uint8_t*>(h.staging.mapped);
        const uint32_t slotSize = h.targetTexel + h.depthTexel;
        for (const PendingHistory::Entry& entry : h.entries)
        {
            PixelEvent& e = out.events[entry.event];
            if (bytes && (VkDeviceSize)(entry.slot + 1) * slotSize <= h.staging.size)
            {
                const uint8_t* at = bytes + (size_t)entry.slot * slotSize;
                e.value.assign(at, at + h.targetTexel);
                if (h.depthTexel)
                    e.depth.assign(at + h.targetTexel, at + slotSize);
            }
            if (entry.idSlot >= 0)
            {
                const auto* ids = static_cast<const uint8_t*>(h.ids.mapped);
                if (ids && (VkDeviceSize)(entry.idSlot + 1) * 4 <= h.ids.size)
                {
                    uint32_t written = 0;
                    std::memcpy(&written, ids + (size_t)entry.idSlot * 4, 4);
                    // The shader writes the index plus one, so 0 is "no fragment of it wrote the pixel".
                    e.primitive = written ? (int64_t)(written - 1) : -2;
                }
            }
            if (_historyFragmentRound)
                continue;
            // What the second replay finds this event by, since it records none of its own.
            _historyEventIndex[std::make_tuple(e.frame, e.commandBuffer, e.passIndex, e.command)] = entry.event;
            if (entry.queryBase < 0 || !entry.issued)
                continue;
            uint64_t results[kVariantCount] = {};
            for (int v = 0; v < kVariantCount; ++v)
            {
                if (!(entry.issued & (1u << v)))
                    continue;
                _fns.GetQueryPoolResults(_device, h.queries, (uint32_t)entry.queryBase + v, 1, sizeof(uint64_t), &results[v], sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            }
            e.testsMeasured = entry.issued;
            e.covered = results[kCovered];
            e.facing = results[kFacing];
            e.shaded = results[kShaded];
            e.depthPassed = results[kDepthOnly];
            e.stencilPassed = results[kStencilOnly];
            e.passed = results[kAllTests];
        }
        // The fragment round's measurements: one value and one primitive per fragment.
        const auto* values = static_cast<const uint8_t*>(h.fragValues.mapped);
        const auto* fragIds = static_cast<const uint8_t*>(h.fragIds.mapped);
        for (const PendingHistory::FragmentEntry& fe : h.fragmentEntries)
        {
            if (fe.event >= out.events.size())
                continue;
            PixelEvent& e = out.events[fe.event];
            PixelFragment fragment;
            if (values && (VkDeviceSize)(fe.slot + 1) * h.targetTexel <= h.fragValues.size)
            {
                const uint8_t* at = values + (size_t)fe.slot * h.targetTexel;
                fragment.value.assign(at, at + h.targetTexel);
            }
            if (fragIds && (VkDeviceSize)(fe.slot + 1) * 4 <= h.fragIds.size)
            {
                uint32_t written = 0;
                std::memcpy(&written, fragIds + (size_t)fe.slot * 4, 4);
                // The shader writes the index plus one, so 0 is "this run wrote no fragment".
                fragment.primitive = written ? (int64_t)(written - 1) : -2;
            }
            if (e.fragments.size() <= fe.index)
                e.fragments.resize(fe.index + 1);
            e.fragments[fe.index] = std::move(fragment);
        }
        DestroyStaging(h.staging);
        if (h.ids.buffer)
            DestroyStaging(h.ids);
        if (h.fragValues.buffer)
            DestroyStaging(h.fragValues);
        if (h.fragIds.buffer)
            DestroyStaging(h.fragIds);
        if (h.queries)
            _fns.DestroyQueryPool(_device, h.queries, nullptr);
    }
    histories.clear();
}

// ---------------------------------------------------------------------------------------------
// Writes outside render passes: what a clear, a copy, a blit, a resolve or a dispatch did to the
// pixel. A pass is followed by replaying it (above); these need none of that — the write lands in
// the image itself, so the pixel is read straight out of it once the command has run.

namespace
{

/** Where a command's arguments say which image it writes, and which part of it. */
struct WriteShape
{
    const char* info;          // the *2 form's one struct, null when it has none
    const char* image;
    const char* layout;
    const char* regions;
    const char* subresource;   // null for a clear's ranges, which name no offset
    const char* offset;        // "dstOffset" / "imageOffset", or "dstOffsets" for a blit's box
    const char* extent;        // null for a blit, whose second offset is its extent
    const char* kind;
    const char* detail;
};

/** The shape of `method`, or null when the command writes no image the history could follow. */
const WriteShape* ShapeOf(const std::string& method)
{
    // The KHR forms are the core ones, and the 2 forms carry the same members inside one struct.
    std::string base = method;
    if (base.size() > 3 && base.compare(base.size() - 3, 3, "KHR") == 0)
        base.resize(base.size() - 3);
    const bool two = !base.empty() && base.back() == '2';
    if (two)
        base.pop_back();
    static const WriteShape kClearColor{nullptr, "image", "imageLayout", "pRanges", nullptr, nullptr, nullptr, "clear", "vkCmdClearColorImage"};
    static const WriteShape kClearDepth{nullptr, "image", "imageLayout", "pRanges", nullptr, nullptr, nullptr, "clear", "vkCmdClearDepthStencilImage"};
    static const WriteShape kCopyImage{"pCopyImageInfo", "dstImage", "dstImageLayout", "pRegions", "dstSubresource", "dstOffset", "extent", "copy", "copied from an image"};
    static const WriteShape kCopyBuffer{"pCopyBufferToImageInfo", "dstImage", "dstImageLayout", "pRegions", "imageSubresource", "imageOffset", "imageExtent", "copy", "copied from a buffer"};
    static const WriteShape kBlit{"pBlitImageInfo", "dstImage", "dstImageLayout", "pRegions", "dstSubresource", "dstOffsets", nullptr, "blit", "blitted from an image"};
    static const WriteShape kResolve{"pResolveImageInfo", "dstImage", "dstImageLayout", "pRegions", "dstSubresource", "dstOffset", "extent", "resolve", "resolved from a multisampled image"};
    if (base == "vkCmdClearColorImage")
        return &kClearColor;
    if (base == "vkCmdClearDepthStencilImage")
        return &kClearDepth;
    if (base == "vkCmdCopyImage")
        return &kCopyImage;
    if (base == "vkCmdCopyBufferToImage")
        return &kCopyBuffer;
    if (base == "vkCmdBlitImage")
        return &kBlit;
    if (base == "vkCmdResolveImage")
        return &kResolve;
    return nullptr;
}

uint32_t UintOf(const JValue* v, uint32_t fallback = 0)
{
    return v ? (uint32_t)v->Uint() : fallback;
}

int32_t IntOf(const JValue* v, int32_t fallback = 0)
{
    return v ? (int32_t)v->Int() : fallback;
}

/** Whether a subresource range or layers of one covers `mip` and `layer`. */
bool CoversSubresource(const JValue* sub, uint32_t mip, uint32_t layer, bool range)
{
    if (!sub)
        return false;
    const uint32_t baseMip = UintOf(sub->Get(range ? "baseMipLevel" : "mipLevel"));
    const uint32_t mips = range ? UintOf(sub->Get("levelCount"), 1) : 1;
    const uint32_t baseLayer = UintOf(sub->Get("baseArrayLayer"));
    const uint32_t layers = UintOf(sub->Get("layerCount"), 1);
    if (mip < baseMip || (mips != VK_REMAINING_MIP_LEVELS && mip >= baseMip + mips))
        return false;
    if (layer < baseLayer || (layers != VK_REMAINING_ARRAY_LAYERS && layer >= baseLayer + layers))
        return false;
    return true;
}

} // namespace

void Replayer::NoteHistoryBindings(const JValue* descriptors)
{
    const JValue* sets = descriptors ? descriptors->Get("sets") : nullptr;
    if (!sets || !sets->IsArray())
        return;
    const std::string bindPoint = Str(descriptors->Get("bindPoint"));
    auto& byIndex = _historyStorageBinds[bindPoint];
    for (uint32_t s = 0; s < sets->count; ++s)
    {
        const JValue& set = sets->items[s];
        const uint32_t index = set.Get("set") ? (uint32_t)set.Get("set")->Uint() : s;
        bool binds = false;
        const JValue* bindings = set.Get("bindings");
        for (uint32_t b = 0; bindings && b < bindings->count && !binds; ++b)
        {
            const JValue& binding = bindings->items[b];
            // Only a storage image: a sampled image bound to the same view is read, not written.
            if (Str(binding.Get("type")).find("STORAGE_IMAGE") == std::string::npos)
                continue;
            const JValue* list = binding.Get("descriptors");
            for (uint32_t k = 0; list && k < list->count; ++k)
            {
                if (list->items[k].IsNull())
                    continue;
                auto view = _views.find(IdOf(list->items[k].Get("imageView")));
                if (view == _views.end() || view->second.image != _options.history.image)
                    continue;
                if (view->second.range.baseMipLevel != _options.history.mip)
                    continue;
                const uint32_t layers = view->second.range.layerCount == VK_REMAINING_ARRAY_LAYERS ? UINT32_MAX
                                                                                                   : std::max(1u, view->second.range.layerCount);
                const uint32_t base = view->second.range.baseArrayLayer;
                if (_options.history.layer < base || _options.history.layer - base >= layers)
                    continue;
                binds = true;
                break;
            }
        }
        // A bind replaces what that set index held, whether or not the new one names the image.
        byIndex[index] = binds;
    }
}

Replayer::DirectWrite Replayer::HistoryDirectWrite(const std::string& method, const JValue& args)
{
    DirectWrite write;
    const auto& req = _options.history;
    // A dispatch or a trace writes through a descriptor, which its arguments do not name: what is
    // known is what was bound to it, kept by NoteHistoryBindings as the replay walks the binds. A
    // storage image is written in GENERAL and left there, so that is the layout the pixel is read
    // in — the same assumption the storage-image comparison makes (InjectStorageReadbacks).
    if (StartsWith(method, "vkCmdDispatch") || StartsWith(method, "vkCmdTraceRays"))
    {
        auto it = _images.find(req.image);
        if (it == _images.end() || !it->second.storage || it->second.samples != VK_SAMPLE_COUNT_1_BIT)
            return write;
        const bool trace = StartsWith(method, "vkCmdTraceRays");
        auto bound = _historyStorageBinds.find(trace ? "VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR" : "VK_PIPELINE_BIND_POINT_COMPUTE");
        if (bound == _historyStorageBinds.end())
            return write;
        bool any = false;
        for (const auto& [set, binds] : bound->second)
            any = any || binds;
        if (!any)
            return write;
        write.writes = true;
        write.kind = "compute";
        // Whether the shader wrote the pixel is not knowable from outside it; the value after the
        // command is, and it is what the event carries.
        write.detail = trace ? "traced with the image bound to be written" : "dispatched with the image bound to be written";
        write.layout = VK_IMAGE_LAYOUT_GENERAL;
        return write;
    }
    const WriteShape* shape = ShapeOf(method);
    if (!shape)
        return write;
    const JValue* root = &args;
    if (shape->info)
        if (const JValue* info = args.Get(shape->info))
            root = info;
    if (IdOf(root->Get(shape->image)) != req.image)
        return write;
    const JValue* regions = root->Get(shape->regions);
    if (!regions || !regions->IsArray())
        return write;
    for (uint32_t r = 0; r < regions->count; ++r)
    {
        const JValue& region = regions->items[r];
        if (!shape->subresource)
        {   // a clear's ranges: the whole mip, so only the subresource decides
            if (!CoversSubresource(&region, req.mip, req.layer, true))
                continue;
            write.writes = true;
            break;
        }
        if (!CoversSubresource(region.Get(shape->subresource), req.mip, req.layer, false))
            continue;
        int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (shape->extent)
        {
            const JValue* offset = region.Get(shape->offset);
            const JValue* extent = region.Get(shape->extent);
            x0 = IntOf(offset ? offset->Get("x") : nullptr);
            y0 = IntOf(offset ? offset->Get("y") : nullptr);
            x1 = x0 + (int32_t)UintOf(extent ? extent->Get("width") : nullptr);
            y1 = y0 + (int32_t)UintOf(extent ? extent->Get("height") : nullptr);
        }
        else
        {   // a blit writes the box between its two destination offsets, in either order
            const JValue* offsets = region.Get(shape->offset);
            if (!offsets || !offsets->IsArray() || offsets->count < 2)
                continue;
            const int32_t ax = IntOf(offsets->items[0].Get("x")), ay = IntOf(offsets->items[0].Get("y"));
            const int32_t bx = IntOf(offsets->items[1].Get("x")), by = IntOf(offsets->items[1].Get("y"));
            x0 = std::min(ax, bx);
            x1 = std::max(ax, bx);
            y0 = std::min(ay, by);
            y1 = std::max(ay, by);
        }
        if ((int64_t)req.x < x0 || (int64_t)req.x >= x1 || (int64_t)req.y < y0 || (int64_t)req.y >= y1)
            continue;
        write.writes = true;
        break;
    }
    if (!write.writes)
        return write;
    write.kind = shape->kind;
    write.detail = shape->detail;
    int64_t layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (const JValue* l = root->Get(shape->layout); l && l->IsString())
        LookupEnum(kEnum_VkImageLayout, kEnumCount_VkImageLayout, l->Str(), layout);
    write.layout = (VkImageLayout)layout;
    return write;
}

void Replayer::HistoryDirectPixel(VkCommandBuffer cb, const CommandGroup& group, const DirectWrite& write,
    std::vector<PendingHistory>& histories, uint32_t index, const std::string& method,
    uint32_t frame)
{
    PixelHistoryResult& out = _report->history;
    auto it = _images.find(_options.history.image);
    if (it == _images.end())
        return;
    const ImageRecord& image = it->second;
    const VkImageAspectFlags aspects = vkinsp::FormatAspects(image.format);
    const VkImageAspectFlags aspect = (aspects & VK_IMAGE_ASPECT_COLOR_BIT) ? (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT
                                                                            : (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT;
    const uint32_t texel = vkinsp::FormatBlockInfo(image.format, aspect).bytes;
    if (!texel)
    {
        out.notes.push_back("command " + std::to_string(index) + " " + method + ": the image's format cannot be read back");
        return;
    }

    // One staging buffer per command buffer, sized for the writes its commands could make.
    if (_historyDirect < 0)
    {
        uint32_t slots = 0;
        const JValue* commands = _capture->Commands();
        for (uint32_t i = group.first; i <= group.last && i < commands->count; ++i)
        {
            const std::string m = Str(commands->items[i].Get("method"));
            if (StartsWith(m, "vkCmdDispatch") || StartsWith(m, "vkCmdTraceRays") || ShapeOf(m))
                ++slots;
        }
        if (!slots)
            return;
        PendingHistory pending;
        pending.targetTexel = texel;
        pending.slots = slots;
        if (!CreateStaging((VkDeviceSize)slots * texel, pending.staging))
        {
            out.notes.push_back("no staging memory for the pixel's value outside the render passes");
            return;
        }
        _historyDirect = (int)histories.size();
        histories.push_back(std::move(pending));
    }
    PendingHistory& pending = histories[_historyDirect];
    if (pending.nextSlot >= pending.slots)
        return;
    const uint32_t slot = pending.nextSlot++;

    const VkImageSubresourceRange range{aspect, _options.history.mip, 1, _options.history.layer, 1};
    Barrier(cb, image.image, range, write.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = (VkDeviceSize)slot * pending.targetTexel;
    copy.imageSubresource = {aspect, _options.history.mip, _options.history.layer, 1};
    copy.imageOffset = {(int32_t)_options.history.x, (int32_t)_options.history.y, 0};
    copy.imageExtent = {1, 1, 1};
    _fns.CmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pending.staging.buffer, 1, &copy);
    Barrier(cb, image.image, range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, write.layout);

    if (out.format.empty())
        out.format = EnumName(kEnum_VkFormat, kEnumCount_VkFormat, image.format);
    PixelEvent e;
    e.kind = write.kind;
    e.command = index;
    e.method = method;
    e.detail = write.detail;
    e.commandBuffer = group.commandBuffer;
    e.frame = frame;
    e.passIndex = UINT32_MAX;
    PendingHistory::Entry entry;
    entry.event = out.events.size();
    entry.slot = slot;
    pending.entries.push_back(entry);
    out.events.push_back(std::move(e));
    ++_historyWrites;
}

} // namespace vkreplay
