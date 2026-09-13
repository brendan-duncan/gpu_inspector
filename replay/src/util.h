// Helpers shared by the replay's translation units: reading the manifest's JSON, classifying
// commands, shader stage names, editing pNext chains, and the overdraw count shader.
#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_set>

#include "json.h"

namespace vkreplay {

inline std::string Str(const JValue* v) { return v && v->IsString() ? std::string(v->Str()) : std::string(); }

inline uint64_t IdOf(const JValue* v) {
    const JValue* id = v ? v->Get("__id") : nullptr;
    return id ? id->Uint() : 0;
}

inline bool StartsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

inline bool IsEndPass(std::string_view m) {
    return m == "vkCmdEndRenderPass" || m == "vkCmdEndRenderPass2" || m == "vkCmdEndRenderPass2KHR" ||
           m == "vkCmdEndRendering" || m == "vkCmdEndRenderingKHR";
}

inline bool IsBeginRenderPass(std::string_view m) {
    return m == "vkCmdBeginRenderPass" || m == "vkCmdBeginRenderPass2" || m == "vkCmdBeginRenderPass2KHR";
}

/** Views a multiview mask renders; 1 for a mask of 0 (no multiview). */
inline uint32_t ViewCount(uint32_t mask) {
    uint32_t n = 0;
    for (; mask; mask &= mask - 1) ++n;
    return n ? n : 1;
}

inline bool IsBeginRendering(std::string_view m) { return m == "vkCmdBeginRendering" || m == "vkCmdBeginRenderingKHR"; }

/** A command that does GPU work of its own: what per-draw timing and counters measure. */
inline bool IsAction(std::string_view m) { return StartsWith(m, "vkCmdDraw") || StartsWith(m, "vkCmdDispatch"); }

/** The layer's names for shader stages, which name a pipeline's SPIR-V payloads ("fragment:main"). */
inline const char* StageName(VkShaderStageFlagBits stage) {
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT: return "vertex";
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return "tess_control";
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tess_eval";
        case VK_SHADER_STAGE_GEOMETRY_BIT: return "geometry";
        case VK_SHADER_STAGE_FRAGMENT_BIT: return "fragment";
        case VK_SHADER_STAGE_COMPUTE_BIT: return "compute";
        case VK_SHADER_STAGE_TASK_BIT_EXT: return "task";
        case VK_SHADER_STAGE_MESH_BIT_EXT: return "mesh";
        default: return "shader";
    }
}

/**
 * Unlinks the structs of the given types from a decoded pNext chain (arena memory the replay
 * owns): parts of a create info that tie an object to the capturing machine, like external memory.
 */
inline const void* StripPNext(const void* head, std::initializer_list<VkStructureType> drop) {
    auto* first = static_cast<VkBaseOutStructure*>(const_cast<void*>(head));
    VkBaseOutStructure* kept = nullptr;
    VkBaseOutStructure* tail = nullptr;
    for (VkBaseOutStructure* p = first; p;) {
        VkBaseOutStructure* next = p->pNext;
        if (std::find(drop.begin(), drop.end(), p->sType) == drop.end()) {
            p->pNext = nullptr;
            if (tail) tail->pNext = p; else kept = p;
            tail = p;
        }
        p = next;
    }
    return kept;
}


/**
 * The overdraw count shader, blended additively into an R16_SFLOAT target, one per fragment:
 *   #version 450
 *   layout(location = 0) out float count;
 *   void main() { count = 1.0; }
 * compiled with glslangValidator -V --target-env vulkan1.0, spirv-opt --strip-debug -O, and
 * checked with spirv-val.
 */
inline const uint32_t kCountFragmentSpirv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000000a, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000008, 0x00030010, 0x00000004,
    0x00000007, 0x00040047, 0x00000008, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040020, 0x00000007, 0x00000003,
    0x00000006, 0x0004003b, 0x00000007, 0x00000008, 0x00000003, 0x0004002b, 0x00000006, 0x00000009,
    0x3f800000, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0003003e, 0x00000008, 0x00000009, 0x000100fd, 0x00010038,
};

inline float HalfToFloat(uint16_t h) {
    const int sign = (h >> 15) ? -1 : 1;
    const int exponent = (h >> 10) & 0x1F;
    const int mantissa = h & 0x3FF;
    if (exponent == 0) return sign * std::ldexp((float)mantissa, -24);
    if (exponent == 31) return mantissa ? NAN : sign * INFINITY;
    return sign * std::ldexp((float)(mantissa + 1024), exponent - 25);
}

/** Commands an overdraw pass leaves out: pass structure, clears, queries and labels. */
inline const std::unordered_set<std::string> kOverdrawSkipped = {
    "vkCmdBeginRenderPass", "vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR", "vkCmdEndRenderPass", "vkCmdEndRenderPass2",
    "vkCmdEndRenderPass2KHR", "vkCmdBeginRendering", "vkCmdBeginRenderingKHR", "vkCmdEndRendering", "vkCmdEndRenderingKHR",
    "vkCmdNextSubpass", "vkCmdNextSubpass2", "vkCmdNextSubpass2KHR", "vkCmdClearAttachments", "vkCmdExecuteCommands",
    "vkCmdBeginQuery", "vkCmdEndQuery", "vkCmdBeginQueryIndexedEXT", "vkCmdEndQueryIndexedEXT", "vkCmdWriteTimestamp",
    "vkCmdWriteTimestamp2", "vkCmdWriteTimestamp2KHR", "vkCmdBeginDebugUtilsLabelEXT", "vkCmdEndDebugUtilsLabelEXT",
    "vkCmdInsertDebugUtilsLabelEXT", "vkCmdDebugMarkerBeginEXT", "vkCmdDebugMarkerEndEXT", "vkCmdDebugMarkerInsertEXT",
    // Dynamic blend and multisample state would undo the additive count into one single-sampled target.
    "vkCmdSetColorWriteEnableEXT", "vkCmdSetColorBlendEnableEXT", "vkCmdSetColorBlendEquationEXT", "vkCmdSetColorWriteMaskEXT",
    "vkCmdSetColorBlendAdvancedEXT", "vkCmdSetLogicOpEXT", "vkCmdSetLogicOpEnableEXT", "vkCmdSetRasterizationSamplesEXT",
    "vkCmdSetSampleMaskEXT", "vkCmdSetAlphaToCoverageEnableEXT", "vkCmdSetAlphaToOneEnableEXT",
};

/** Depth and stencil state the count of every rasterized fragment leaves out. */
inline const std::unordered_set<std::string> kDepthState = {
    "vkCmdSetDepthTestEnable", "vkCmdSetDepthTestEnableEXT", "vkCmdSetDepthWriteEnable", "vkCmdSetDepthWriteEnableEXT",
    "vkCmdSetDepthCompareOp", "vkCmdSetDepthCompareOpEXT", "vkCmdSetStencilTestEnable", "vkCmdSetStencilTestEnableEXT",
    "vkCmdSetStencilOp", "vkCmdSetStencilOpEXT", "vkCmdSetDepthBoundsTestEnable", "vkCmdSetDepthBoundsTestEnableEXT",
};

/** Commands whose state carries into a pass from before it: re-issued ahead of an overdraw pass. */
inline bool IsStateCommand(std::string_view m) {
    return StartsWith(m, "vkCmdSet") || StartsWith(m, "vkCmdBindDescriptorSets") || StartsWith(m, "vkCmdBindVertexBuffers") ||
           StartsWith(m, "vkCmdBindIndexBuffer") || StartsWith(m, "vkCmdPushConstants") || m == "vkCmdBindPipeline" ||
           StartsWith(m, "vkCmdPushDescriptorSet");
}

/** Dynamic states of color output: a pipeline copy that changes writes or blending makes them static. */
inline const std::initializer_list<VkDynamicState> kColorOutputDynamicStates = {
    VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,
    VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_ADVANCED_EXT, VK_DYNAMIC_STATE_LOGIC_OP_EXT,
    VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT,
};

/** Dynamic multisample states: a pipeline copy that renders single-sampled makes them static. */
inline const std::initializer_list<VkDynamicState> kMultisampleDynamicStates = {
    VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT, VK_DYNAMIC_STATE_SAMPLE_MASK_EXT, VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT,
    VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT,
};

/** A VkClearValue as the layer writes it (every member of the union): the exact bits, from color.uint32. */
inline VkClearValue ClearValueOf(const JValue& v) {
    VkClearValue clear{};
    if (const JValue* color = v.Get("color")) {
        if (const JValue* bits = color->Get("uint32"); bits && bits->IsArray())
            for (uint32_t k = 0; k < 4 && k < bits->count; ++k) clear.color.uint32[k] = (uint32_t)bits->items[k].Uint();
    }
    return clear;
}

} // namespace vkreplay
