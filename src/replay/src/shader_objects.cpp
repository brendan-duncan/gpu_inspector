// Shader objects in the replay's analyses. A draw bound with vkCmdBindShadersEXT has no pipeline to
// copy: its stages are shader objects of their own and every piece of its state is dynamic. So the
// copy an analysis makes of such a draw is the draw's own shader objects with the fragment stage
// bound to one of the replay's (or kept), and what a pipeline copy would have baked in -- blending,
// writes, the tests, culling, the sample count -- set right before the draw, over what the
// application set (ShaderObjectEdit). Shader objects draw only in dynamic rendering, so a pass of the
// replay's own that holds one is begun that way, and the pipeline copies drawn beside it are made
// for it (PassUsesShaderObjects).
#include "replayer.h"

#include <algorithm>
#include <string>
#include <vector>

#include "util.h"

namespace vkreplay
{

void Replayer::NoteShaderObjectSet(ShaderObjectSets& sets, const std::string& method, const JValue& args)
{
    if (method == "vkCmdSetViewportWithCount" || method == "vkCmdSetViewportWithCountEXT")
        sets.viewports = std::max<uint32_t>(1, (uint32_t)(args.Get("viewportCount") ? args.Get("viewportCount")->Uint() : 1));
    else if (method == "vkCmdSetColorWriteEnableEXT")
        sets.colorWriteEnable = true;
    else if (method == "vkCmdSetLogicOpEnableEXT")
        sets.logicOpEnable = true;
    else if (method == "vkCmdSetAlphaToOneEnableEXT")
        sets.alphaToOne = true;
}

void Replayer::ApplyShaderObjectEdit(VkCommandBuffer cb, const ShaderObjectEdit& e, const ShaderObjectSets& sets)
{
    // The core entry point where the device has it, the extension's otherwise.
    const auto depthTest = _fns.CmdSetDepthTestEnable ? _fns.CmdSetDepthTestEnable : _fns.CmdSetDepthTestEnableEXT;
    const auto depthWrite = _fns.CmdSetDepthWriteEnable ? _fns.CmdSetDepthWriteEnable : _fns.CmdSetDepthWriteEnableEXT;
    const auto depthBounds = _fns.CmdSetDepthBoundsTestEnable ? _fns.CmdSetDepthBoundsTestEnable : _fns.CmdSetDepthBoundsTestEnableEXT;
    const auto stencilTest = _fns.CmdSetStencilTestEnable ? _fns.CmdSetStencilTestEnable : _fns.CmdSetStencilTestEnableEXT;
    const auto stencilOp = _fns.CmdSetStencilOp ? _fns.CmdSetStencilOp : _fns.CmdSetStencilOpEXT;
    const auto cullMode = _fns.CmdSetCullMode ? _fns.CmdSetCullMode : _fns.CmdSetCullModeEXT;
    const auto scissors = _fns.CmdSetScissorWithCount ? _fns.CmdSetScissorWithCount : _fns.CmdSetScissorWithCountEXT;

    if (const uint32_t n = e.colorAttachments)
    {
        const std::vector<VkBool32> blend(n, e.blendAdd ? VK_TRUE : VK_FALSE);
        const std::vector<VkColorComponentFlags> masks(n, e.writeMask);
        if (_fns.CmdSetColorBlendEnableEXT)
            _fns.CmdSetColorBlendEnableEXT(cb, 0, n, blend.data());
        if (e.blendAdd && _fns.CmdSetColorBlendEquationEXT)
        {
            VkColorBlendEquationEXT add{};
            add.srcColorBlendFactor = add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            add.srcAlphaBlendFactor = add.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            add.colorBlendOp = add.alphaBlendOp = VK_BLEND_OP_ADD;
            const std::vector<VkColorBlendEquationEXT> equations(n, add);
            _fns.CmdSetColorBlendEquationEXT(cb, 0, n, equations.data());
        }
        if (_fns.CmdSetColorWriteMaskEXT)
            _fns.CmdSetColorWriteMaskEXT(cb, 0, n, masks.data());
        // These two are only set where the application set them: their features may be off.
        if (sets.colorWriteEnable && _fns.CmdSetColorWriteEnableEXT)
        {
            const std::vector<VkBool32> enable(n, VK_TRUE);
            _fns.CmdSetColorWriteEnableEXT(cb, n, enable.data());
        }
        if (sets.logicOpEnable && _fns.CmdSetLogicOpEnableEXT)
            _fns.CmdSetLogicOpEnableEXT(cb, VK_FALSE);
    }
    if (e.singleSample)
    {
        const VkSampleMask all = ~0u;
        if (_fns.CmdSetRasterizationSamplesEXT)
            _fns.CmdSetRasterizationSamplesEXT(cb, VK_SAMPLE_COUNT_1_BIT);
        if (_fns.CmdSetSampleMaskEXT)
            _fns.CmdSetSampleMaskEXT(cb, VK_SAMPLE_COUNT_1_BIT, &all);
        if (_fns.CmdSetAlphaToCoverageEnableEXT)
            _fns.CmdSetAlphaToCoverageEnableEXT(cb, VK_FALSE);
        if (sets.alphaToOne && _fns.CmdSetAlphaToOneEnableEXT)
            _fns.CmdSetAlphaToOneEnableEXT(cb, VK_FALSE);
    }
    if (!e.depthTest && depthTest)
        depthTest(cb, VK_FALSE);
    if (!e.depthTest && depthBounds)
        depthBounds(cb, VK_FALSE);
    if (!e.depthWrite && depthWrite)
        depthWrite(cb, VK_FALSE);
    if (!e.stencilTest && stencilTest)
        stencilTest(cb, VK_FALSE);
    if (!e.stencilWrite)
        _fns.CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    if (e.stencilCounter && stencilTest && stencilOp)
    {
        stencilTest(cb, VK_TRUE);
        stencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
            VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_COMPARE_OP_EQUAL);
        _fns.CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
        _fns.CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    }
    if (e.cullNone && cullMode)
        cullMode(cb, VK_CULL_MODE_NONE);
    if (e.wireframe && _fns.CmdSetPolygonModeEXT)
    {
        _fns.CmdSetPolygonModeEXT(cb, VK_POLYGON_MODE_LINE);
        _fns.CmdSetLineWidth(cb, 1.0f);
    }
    // Shader objects take their scissors with a count, which must match the viewports'.
    if (e.scissor && scissors)
    {
        const std::vector<VkRect2D> rects(sets.viewports, *e.scissor);
        scissors(cb, (uint32_t)rects.size(), rects.data());
    }
}

VkShaderEXT Replayer::ReplacementFragment(ReplacementShader which, uint64_t layoutFrom)
{
    const auto key = std::make_pair(which, layoutFrom);
    if (auto it = _replacementShaders.find(key); it != _replacementShaders.end())
        return it->second;
    _replacementShaders[key] = VK_NULL_HANDLE;   // a shader that cannot be made is not tried again
    VkShaderCreateInfoEXT from{};
    std::string error;
    if (!_fns.CreateShadersEXT || !ShaderObjectInfo(layoutFrom, from, error))
        return VK_NULL_HANDLE;
    VkShaderCreateInfoEXT info{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    info.pName = "main";
    info.setLayoutCount = from.setLayoutCount;
    info.pSetLayouts = from.pSetLayouts;
    info.pushConstantRangeCount = from.pushConstantRangeCount;
    info.pPushConstantRanges = from.pPushConstantRanges;
    switch (which)
    {
    case ReplacementShader::Count:
        info.pCode = kCountFragmentSpirv;
        info.codeSize = sizeof(kCountFragmentSpirv);
        break;
    case ReplacementShader::BackFace:
        info.pCode = kBackFaceFragmentSpirv;
        info.codeSize = sizeof(kBackFaceFragmentSpirv);
        break;
    case ReplacementShader::PrimitiveId:
        info.pCode = kPrimitiveIdFragmentSpirv;
        info.codeSize = sizeof(kPrimitiveIdFragmentSpirv);
        break;
    }
    VkShaderEXT shader = VK_NULL_HANDLE;
    if (_fns.CreateShadersEXT(_device, 1, &info, nullptr, &shader) != VK_SUCCESS)
        shader = VK_NULL_HANDLE;
    _arena.Reset();
    Track("VkShaderEXT", (uint64_t)shader);
    _replacementShaders[key] = shader;
    return shader;
}

void Replayer::BindFragmentShader(VkCommandBuffer cb, VkShaderEXT shader)
{
    const VkShaderStageFlagBits stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    _fns.CmdBindShadersEXT(cb, 1, &stage, &shader);
}

bool Replayer::ShaderObjectInfo(uint64_t shaderId, VkShaderCreateInfoEXT& info, std::string& error)
{
    const JValue* object = _capture->Object(shaderId);
    const JValue* args = object ? object->Get("args") : nullptr;
    if (!args || Str(object->Get("type")) != "VkShaderEXT")
    {
        error = "the shader object could not be made again";
        return false;
    }
    const size_t unresolved = _ctx.unresolved;
    const size_t problems = _ctx.problems.size();
    Args_vkCreateShadersEXT a{};
    DecodeArgs(_ctx, *args, a);
    const uint32_t index = object->Get("index") ? (uint32_t)object->Get("index")->Uint() : 0;
    const bool usable = a.pCreateInfos && index < a.createInfoCount && _ctx.unresolved == unresolved;
    _ctx.unresolved = unresolved;
    _ctx.problems.resize(problems);
    if (!usable)
    {
        _arena.Reset();
        error = "the shader object's create info names objects the replay does not have";
        return false;
    }
    info = a.pCreateInfos[index];
    return true;
}

VkShaderEXT Replayer::ShaderObjectWithCode(uint64_t shaderId, const uint32_t* words, size_t count, std::string& error)
{
    VkShaderCreateInfoEXT info{};
    if (!_fns.CreateShadersEXT || !ShaderObjectInfo(shaderId, info, error))
        return VK_NULL_HANDLE;
    // Made on its own, like every shader object the replay makes.
    info.flags &= ~(VkShaderCreateFlagsEXT)VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
    info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    info.pCode = words;
    info.codeSize = count * 4;
    VkShaderEXT shader = VK_NULL_HANDLE;
    const VkResult created = _fns.CreateShadersEXT(_device, 1, &info, nullptr, &shader);
    _arena.Reset();
    if (created != VK_SUCCESS || !shader)
    {
        error = "the edited shader object was refused (" + std::to_string(created) + ")";
        return VK_NULL_HANDLE;
    }
    Track("VkShaderEXT", (uint64_t)shader);
    return shader;
}

bool Replayer::PassUsesShaderObjects(const CommandGroup& group, uint32_t endIndex) const
{
    const JValue* commands = _capture->Commands();
    for (uint32_t i = group.first + 1; i < endIndex && i < commands->count; ++i)
    {
        const JValue& c = commands->items[i];
        const JValue* args = c.Get("args");
        if (!args || Str(c.Get("method")) != "vkCmdBindShadersEXT")
            continue;
        const JValue* stages = args->Get("pStages");
        const JValue* shaders = args->Get("pShaders");
        for (uint32_t k = 0; stages && k < stages->count; ++k)
            if (Str(&stages->items[k]) != "VK_SHADER_STAGE_COMPUTE_BIT" && shaders && shaders->IsArray() && k < shaders->count &&
                IdOf(&shaders->items[k]))
                return true;
    }
    return false;
}

bool Replayer::ReissueShaderObjectDraw(VkCommandBuffer cb, ReissueMode mode, bool depthTested, VkFormat depthFormat)
{
    if (!_reissueRendering || !_fns.CmdBindShadersEXT)
        return false;
    VkShaderEXT fragment = ReplacementFragment(mode == ReissueMode::BackFace ? ReplacementShader::BackFace : ReplacementShader::Count, _reissueLayoutShader);
    if (!fragment)
        return false;
    // A pipeline copy bound since the last shader-object draw made static what the application's
    // dynamic state had set: the whole of it again, in order, so the last value of each stays.
    if (_reissueStateStale)
    {
        const JValue* commands = _capture->Commands();
        for (uint32_t i : _reissueSetCommands)
        {
            const JValue& c = commands->items[i];
            if (ReplayFn fn = FindReplayCommand(Str(c.Get("method"))); fn && c.Get("args"))
                IssueCommand(fn, c, *c.Get("args"), cb);
            _arena.Reset();
        }
        _reissueStateStale = false;
    }
    BindFragmentShader(cb, fragment);
    // What OverdrawPipeline changes of a pipeline copy, mode for mode.
    const bool tested = depthTested && depthFormat != VK_FORMAT_UNDEFINED;
    ShaderObjectEdit e;
    e.colorAttachments = 1;
    e.blendAdd = true;
    e.writeMask = mode == ReissueMode::DepthOnly ? 0 : VK_COLOR_COMPONENT_R_BIT;
    e.singleSample = true;
    e.depthTest = e.depthWrite = e.stencilTest = e.stencilWrite = tested;
    if (mode == ReissueMode::StencilOnly)
        e.depthTest = e.depthWrite = false;
    e.cullNone = mode == ReissueMode::BackFace;
    e.wireframe = mode == ReissueMode::Wireframe;
    ApplyShaderObjectEdit(cb, e, _reissueSets);
    return true;
}

} // namespace vkreplay
