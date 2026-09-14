// Live shader editing: the UI's ReplaceShader {pipeline, stage, spirv} carries DXBC or DXIL
// bytecode (the field keeps its Vulkan name); the library keeps a copy of every pipeline's
// description, makes a replacement with the stage swapped, registers it as an object of its own
// ("<name> (edited)") and binds it instead of the original in SetPipelineState from then on.
// RestoreShader drops the edit. The counterpart of src/vulkan/src/shader_edit.h.
#pragma once

#include "common.h"

#include <string>
#include <vector>

namespace dxinsp {

class ShaderEditor {
public:
    static ShaderEditor& Get();

    /** A pipeline the application created, with its description copied (bytecode included). */
    void OnGraphicsPipelineCreated(ID3D12Device* device, ID3D12PipelineState* pipeline, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
    void OnComputePipelineCreated(ID3D12Device* device, ID3D12PipelineState* pipeline, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc);
    void OnStreamPipelineCreated(ID3D12Device* device, ID3D12PipelineState* pipeline, const D3D12_PIPELINE_STATE_STREAM_DESC& desc);
    void OnPipelineReleased(ID3D12PipelineState* pipeline);

    /**
     * Replaces one stage. `stage` is the UI's stage name. Returns false with `error`; on success
     * `replacementId` is the new object's id and `note` says what could not be kept (a cached PSO).
     */
    bool Replace(uint64_t pipelineId, const std::string& stage, const std::vector<uint8_t>& bytecode,
                 std::string& error, uint64_t& replacementId, std::string& note);
    /** Drops the edit of one stage (or of every stage when `stage` is empty). */
    bool Restore(uint64_t pipelineId, const std::string& stage, std::string& error);

    /** The pipeline to bind in place of `pipeline`: its replacement when edited, else itself. Lock-free when nothing is edited. */
    ID3D12PipelineState* Substitute(ID3D12PipelineState* pipeline);

    /** Retired replacements are destroyed at a later present, once no list can reference them. */
    void OnPresent();

private:
    ShaderEditor() = default;
    struct Impl;
    Impl* _impl = nullptr;
    Impl& impl();
};

}  // namespace dxinsp
