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
// A pipeline linked from graphics pipeline libraries (VK_EXT_graphics_pipeline_library) keeps the
// records of its libraries, which the application may destroy once it has linked. An edit makes
// every library again from its record, with the edited stage in whichever library holds it, and
// links the replacement from those; the new libraries are retired with the replacement.
//
// Shader objects (VK_EXT_shader_object) are edited the same way without a pipeline: a replacement
// VkShaderEXT is made from the recorded create info with the new code and bound wherever the
// application binds the original (vkCmdBindShadersEXT pre-hook). A shader created linked to others
// (VK_SHADER_CREATE_LINK_STAGE_BIT_EXT) has to be bound with the rest of its set, so its whole set is
// made again, unlinked, and every member is substituted. Shaders created from a binary cannot be
// edited.
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

namespace vkinsp
{

struct DeviceData;

// Owns the memory of a deep-copied create info chain.
struct Arena
{
    std::vector<std::unique_ptr<uint8_t[]>> blocks;
    void* Alloc(size_t size);
    template <typename T>
    T* Copy(const T* src, size_t count = 1)
    {
        if (!src || !count)
            return nullptr;
        T* dst = static_cast<T*>(Alloc(sizeof(T) * count));
        memcpy(dst, src, sizeof(T) * count);
        return dst;
    }
    const char* CopyString(const char* s);
};

class ShaderEditor
{
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
    VkPipeline Resolve(VkPipeline pipeline)
    {
        if (!_anyActive.load(std::memory_order_relaxed))
            return pipeline;
        return ResolveSlow(pipeline);
    }
    // The original a bound pipeline is the active replacement of, VK_NULL_HANDLE when it is not one
    // (a capture records a bind of the replacement with the original beside it, hooks.cpp).
    VkPipeline OriginalOf(VkPipeline bound);
    VkShaderEXT OriginalShaderOf(VkShaderEXT bound);

    // Shader objects: remember how each was made; a destroyed one is forgotten at the next present.
    void OnCreateShaders(VkDevice device, uint32_t count, const VkShaderCreateInfoEXT* infos, const VkShaderEXT* shaders);
    void OnDestroyShader(uint64_t handle);
    // The shaders to bind in place of `shaders` (`storage` holds them when any is substituted).
    const VkShaderEXT* ResolveShaders(uint32_t count, const VkShaderEXT* shaders, std::vector<VkShaderEXT>& storage)
    {
        if (!_anyShaderActive.load(std::memory_order_relaxed) || !shaders)
            return shaders;
        return ResolveShadersSlow(count, shaders, storage);
    }

    // From the UI (transport thread). Both answer with a ShaderReplaced message. `pipelineId` is
    // a VkPipeline's object id, or a VkShaderEXT's.
    void Replace(uint64_t pipelineId, VkShaderStageFlagBits stage, std::vector<uint32_t> spirv);
    void Restore(uint64_t pipelineId, VkShaderStageFlagBits stage);   // stage 0 = every stage

    // Per frame: forgets destroyed pipelines and destroys retired replacements once the device is idle.
    void OnPresent(DeviceData* dev);

private:
    ShaderEditor() = default;

    struct Record
    {
        VkDevice device = VK_NULL_HANDLE;
        Arena arena;
        VkGraphicsPipelineCreateInfo* graphics = nullptr;
        VkComputePipelineCreateInfo* compute = nullptr;
        // Inline stage code (VkShaderModuleCreateInfo in a stage's pNext), kept apart from the
        // stage's chain so an edited stage can drop it and an untouched stage can prepend it.
        std::vector<const VkShaderModuleCreateInfo*> inlineCode;   // per stage (compute: one entry)
        std::string note;                 // parts of the create info that could not be kept
        bool unsupported = false;         // a library whose creation was not recorded
        bool library = false;             // created with VK_PIPELINE_CREATE_LIBRARY_BIT_KHR
        // Each stage's SPIR-V as the tracker had it at creation ("<stage>:<entry point>"), for the
        // stages a rebuild leaves alone: the application's modules, and its libraries, may be gone.
        std::vector<std::pair<std::string, std::shared_ptr<std::vector<uint8_t>>>> blobs;
        // The libraries it was linked from, in the order of VkPipelineLibraryCreateInfoKHR.
        std::vector<std::shared_ptr<Record>> libraries;
        std::map<VkShaderStageFlagBits, VkShaderModule> edits;   // replacement modules per stage
        std::map<VkShaderStageFlagBits, std::shared_ptr<std::vector<uint8_t>>> editCode;
        VkPipeline replacement = VK_NULL_HANDLE;
        std::vector<VkPipeline> replacementLibraries;   // made for the replacement, retired with it
        uint64_t replacementId = 0;       // tracker id of the replacement pipeline
    };

    struct ShaderRecord
    {
        VkDevice device = VK_NULL_HANDLE;
        Arena arena;
        VkShaderCreateInfoEXT* info = nullptr;   // deep copy, code included
        std::string blobName;                    // "<stage>:<entry point>"
        std::string note;
        std::vector<VkShaderEXT> linked;         // the set it was created linked with (itself included), or empty
        std::shared_ptr<std::vector<uint8_t>> editCode;   // its edited SPIR-V, or null
        VkShaderEXT replacement = VK_NULL_HANDLE;
        uint64_t replacementId = 0;
    };

    VkPipeline ResolveSlow(VkPipeline pipeline);
    const VkShaderEXT* ResolveShadersSlow(uint32_t count, const VkShaderEXT* shaders, std::vector<VkShaderEXT>& storage);
    void ReplaceShaderObject(uint64_t id, VkShaderEXT original, VkShaderStageFlagBits stage, std::vector<uint32_t> spirv);
    void RestoreShaderObject(uint64_t id, VkShaderEXT original, VkShaderStageFlagBits stage);
    // Makes the replacements of a shader's set from the records and their edits, or drops them all
    // when nothing in the set is edited. Called with _mutex held exclusively.
    bool RebuildShaders(VkShaderEXT shader, std::string& error);
    bool Rebuild(VkPipeline original, Record& rec, std::string& error);
    // Makes a pipeline from a record with `edits` applied, its libraries first (added to `libraries`).
    VkPipeline Create(DeviceData* dev, Record& rec, const std::map<VkShaderStageFlagBits, VkShaderModule>& edits,
        std::vector<VkPipeline>& libraries, VkResult& result);
    static bool HasStage(const Record& rec, VkShaderStageFlagBits stage);
    void Retire(VkDevice device, VkPipeline pipeline, VkShaderModule module);
    void Reply(uint64_t pipelineId, VkShaderStageFlagBits stage, bool ok, const std::string& message, uint64_t replacementId);

    std::shared_mutex _mutex;
    std::unordered_map<VkPipeline, std::shared_ptr<Record>> _records;   // by original handle
    std::unordered_map<VkPipeline, VkPipeline> _active;                 // original -> replacement
    std::atomic<bool> _anyActive{false};
    std::unordered_map<VkShaderEXT, std::unique_ptr<ShaderRecord>> _shaderRecords;
    std::unordered_map<VkShaderEXT, VkShaderEXT> _activeShaders;         // original -> replacement
    std::atomic<bool> _anyShaderActive{false};

    std::mutex _retiredMutex;
    std::vector<VkPipeline> _destroyed;   // originals destroyed by the application, to forget
    std::vector<VkShaderEXT> _destroyedShaders;
    std::vector<std::pair<VkDevice, VkPipeline>> _retiredPipelines;
    std::vector<std::pair<VkDevice, VkShaderModule>> _retiredModules;
    std::vector<std::pair<VkDevice, VkShaderEXT>> _retiredShaders;
};

// Base64 -> bytes; false on malformed input.
bool DecodeBase64(const std::string& text, std::vector<uint8_t>& out);

} // namespace vkinsp
