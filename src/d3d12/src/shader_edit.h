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

/**
 * What a measurement changes in its copy of a graphics pipeline (overdraw.h). Everything else --
 * the vertex, hull, domain, geometry and mesh stages, the input layout, the rasterizer state, the
 * root signature -- is the application's, so the copy rasterizes exactly what the original did.
 */
struct PipelineVariant {
    /** Replaces the pixel shader when `pixelShaderSize` is not zero. */
    const void* pixelShader = nullptr;
    size_t pixelShaderSize = 0;
    /** One render target of this format, blended ONE + ONE and writing red only (the overdraw count). */
    DXGI_FORMAT countFormat = DXGI_FORMAT_UNKNOWN;
    /** The depth-stencil format of the attachment the measurement binds (UNKNOWN: none). */
    bool setDepthFormat = false;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    bool disableDepth = false;         // DepthEnable FALSE
    bool disableDepthWrite = false;    // DepthWriteMask ZERO
    bool disableStencil = false;       // StencilEnable FALSE
    bool disableStencilWrites = false; // every stencil write mask cleared (the operations still run)
    bool disableCull = false;          // CullMode NONE
    bool disableColorWrites = false;   // every target's write mask cleared
    bool singleSample = false;         // one sample, no alpha to coverage
};

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

    /**
     * A copy of a graphics pipeline with the measurement's changes (overdraw.h), cached on the
     * pipeline under `key` and destroyed with it. The pointer is borrowed: a caller whose command
     * list may still run takes a reference of its own. False with `error` when the pipeline's
     * description was not recorded, when it is not a graphics pipeline, or when the copy did not
     * build (an embedded root signature in the pixel shader it replaces, a stream subobject this
     * library does not know).
     */
    bool VariantPipeline(ID3D12PipelineState* pipeline, uint64_t key, const PipelineVariant& variant,
                         ID3D12PipelineState** out, std::string& error);
    /** Whether the pipeline's vertex (or mesh) bytecode is a DXIL container: its copy needs a DXIL pixel shader. */
    bool PipelineIsDxil(ID3D12PipelineState* pipeline);

private:
    ShaderEditor() = default;
    struct Impl;
    Impl* _impl = nullptr;
    Impl& impl();
};

}  // namespace dxinsp
