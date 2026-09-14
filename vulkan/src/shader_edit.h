// Live shader editing, the native counterpart of WebGPU Inspector's shader editor.
//
// The application's pipelines are immutable, so an edited shader is applied by building a
// replacement pipeline: the layer keeps a deep copy of every pipeline's create info, creates a
// new VkShaderModule from the recompiled SPIR-V the UI sends (ReplaceShader), creates the
// replacement pipeline from the copied create info with that stage swapped, and from then on
// substitutes the replacement whenever the application binds the original (vkCmdBindPipeline
// pre-hook). Everything else the pipeline uses (layouts, render pass, descriptor sets) is shared
// with the original. RestoreShader drops the substitution. Retired replacements are destroyed
// at the next present after waiting for the device, since command buffers may still use them.
//
// Command buffers recorded before the edit keep binding the original until re-recorded.
//
// Locking: _mutex guards the records and may be held while calling the tracker; the tracker's
// destroy path only reaches this class through OnDestroyPipeline, which takes _retiredMutex
// alone, so the two never lock in opposite orders.
#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkinsp {

struct DeviceData;

// Owns the memory of a deep-copied create info chain.
struct Arena {
    std::vector<std::unique_ptr<uint8_t[]>> blocks;
    void* Alloc(size_t size);
    template <typename T>
    T* Copy(const T* src, size_t count = 1) {
        if (!src || !count) return nullptr;
        T* dst = static_cast<T*>(Alloc(sizeof(T) * count));
        memcpy(dst, src, sizeof(T) * count);
        return dst;
    }
    const char* CopyString(const char* s);
};

class ShaderEditor {
public:
    static ShaderEditor& Get();

    // Creation hooks: remember how each pipeline was made.
    void OnCreateGraphicsPipelines(VkDevice device, uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
                                   const VkPipeline* pipelines);
    void OnCreateComputePipelines(VkDevice device, uint32_t count, const VkComputePipelineCreateInfo* infos,
                                  const VkPipeline* pipelines);
    // Called from the tracker's destroy path (with its lock held): only queues the cleanup.
    void OnDestroyPipeline(uint64_t handle);

    // The pipeline to bind in place of `pipeline` (itself when it has no active edit).
    VkPipeline Resolve(VkPipeline pipeline) {
        if (!_anyActive.load(std::memory_order_relaxed)) return pipeline;
        return ResolveSlow(pipeline);
    }

    // From the UI (transport thread). Both answer with a ShaderReplaced message.
    void Replace(uint64_t pipelineId, VkShaderStageFlagBits stage, std::vector<uint32_t> spirv);
    void Restore(uint64_t pipelineId, VkShaderStageFlagBits stage);   // stage 0 = every stage

    // Per frame: forgets destroyed pipelines and destroys retired replacements once the device is idle.
    void OnPresent(DeviceData* dev);

private:
    ShaderEditor() = default;

    struct Record {
        VkDevice device = VK_NULL_HANDLE;
        Arena arena;
        VkGraphicsPipelineCreateInfo* graphics = nullptr;
        VkComputePipelineCreateInfo* compute = nullptr;
        // Inline stage code (VkShaderModuleCreateInfo in a stage's pNext), kept apart from the
        // stage's chain so an edited stage can drop it and an untouched stage can prepend it.
        std::vector<const VkShaderModuleCreateInfo*> inlineCode;   // per stage (compute: one entry)
        std::string note;                 // parts of the create info that could not be kept
        bool unsupported = false;         // pipeline libraries
        std::map<VkShaderStageFlagBits, VkShaderModule> edits;   // replacement modules per stage
        std::map<VkShaderStageFlagBits, std::shared_ptr<std::vector<uint8_t>>> editCode;
        VkPipeline replacement = VK_NULL_HANDLE;
        uint64_t replacementId = 0;       // tracker id of the replacement pipeline
    };

    VkPipeline ResolveSlow(VkPipeline pipeline);
    bool Rebuild(VkPipeline original, Record& rec, std::string& error);
    void Retire(VkDevice device, VkPipeline pipeline, VkShaderModule module);
    void Reply(uint64_t pipelineId, VkShaderStageFlagBits stage, bool ok, const std::string& message, uint64_t replacementId);

    std::shared_mutex _mutex;
    std::unordered_map<VkPipeline, std::unique_ptr<Record>> _records;   // by original handle
    std::unordered_map<VkPipeline, VkPipeline> _active;                 // original -> replacement
    std::atomic<bool> _anyActive{false};

    std::mutex _retiredMutex;
    std::vector<VkPipeline> _destroyed;   // originals destroyed by the application, to forget
    std::vector<std::pair<VkDevice, VkPipeline>> _retiredPipelines;
    std::vector<std::pair<VkDevice, VkShaderModule>> _retiredModules;
};

// Base64 -> bytes; false on malformed input.
bool DecodeBase64(const std::string& text, std::vector<uint8_t>& out);

} // namespace vkinsp
