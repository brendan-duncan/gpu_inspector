// One description of each D3D12 struct a capture holds, read by two visitors: the decoder
// (dx_decode.h), which fills the struct from the JSON the capture library wrote
// (src/d3d12/src/serialize.cpp), and the source emitter (dx_source.h), which spells the filled
// struct as C++ for Export to C++. D3D12 has no registry to generate these from, as vk.xml is for
// the Vulkan replay, so they are written once here and both directions stay in step.
//
// A visitor V provides:
//   Int(name, field)                        any integer
//   Float(name, field), Bool(name, field)
//   Enum(name, field, table, count, type)   a value by name
//   Flags(name, field, table, count, type)  "A | B"
//   Struct(name, field)                     a nested struct, through Reflect
//   Object(name, field, class)              a COM pointer, by the capture's object id
//   Array(name, pointer, countName, count)  an array of structs, with its count field
//   Ints(name, pointer, countName, count, type)   an array of integers
//   FixedFloats / FixedInts / FixedEnums / FixedStructs   in-place arrays
//   String(name, field)                     a narrow string
//   Address(name, field)                    a GPU virtual address, {address, buffer, offset}
//   CpuHandle(name, field)                  a CPU descriptor handle, {heap, index}
//   ComponentMapping(name, field)           an SRV's Shader4ComponentMapping
// A union's arm is chosen by its discriminator, which is always visited before it.
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <iterator>

#include "d3d12_enums.gen.h"
#include "formats.h"

namespace dxreplay {

#define DX_TABLE(T) dxinsp::kEnum_##T, std::size(dxinsp::kEnum_##T), #T
#define R_INT(f) v.Int(#f, s.f)
#define R_FLOAT(f) v.Float(#f, s.f)
#define R_BOOL(f) v.Bool(#f, s.f)
#define R_ENUM(f, T) v.Enum(#f, s.f, DX_TABLE(T))
#define R_FLAGS(f, T) v.Flags(#f, s.f, DX_TABLE(T))
#define R_STRUCT(f) v.Struct(#f, s.f)
#define R_OBJECT(f, C) v.Object(#f, s.f, C)
#define R_ARRAY(p, n) v.Array(#p, s.p, #n, s.n)

template <typename V> void Reflect(V& v, DXGI_SAMPLE_DESC& s) { R_INT(Count); R_INT(Quality); }

template <typename V> void Reflect(V& v, D3D12_HEAP_PROPERTIES& s) {
    R_ENUM(Type, D3D12_HEAP_TYPE);
    R_ENUM(CPUPageProperty, D3D12_CPU_PAGE_PROPERTY);
    R_ENUM(MemoryPoolPreference, D3D12_MEMORY_POOL);
    R_INT(CreationNodeMask);
    R_INT(VisibleNodeMask);
}

template <typename V> void Reflect(V& v, D3D12_RESOURCE_DESC& s) {
    R_ENUM(Dimension, D3D12_RESOURCE_DIMENSION);
    R_INT(Alignment);
    R_INT(Width);
    R_INT(Height);
    R_INT(DepthOrArraySize);
    R_INT(MipLevels);
    R_ENUM(Format, DXGI_FORMAT);
    R_STRUCT(SampleDesc);
    R_ENUM(Layout, D3D12_TEXTURE_LAYOUT);
    R_FLAGS(Flags, D3D12_RESOURCE_FLAGS);
}

template <typename V> void Reflect(V& v, D3D12_DEPTH_STENCIL_VALUE& s) { R_FLOAT(Depth); R_INT(Stencil); }

template <typename V> void Reflect(V& v, D3D12_CLEAR_VALUE& s) {
    R_ENUM(Format, DXGI_FORMAT);
    // No discriminator of its own: the format says which arm the application filled.
    if (dxinsp::FormatOf(s.Format).depth) R_STRUCT(DepthStencil);
    else v.FixedFloats("Color", s.Color, 4);
}

template <typename V> void Reflect(V& v, D3D12_BOX& s) { R_INT(left); R_INT(top); R_INT(front); R_INT(right); R_INT(bottom); R_INT(back); }
template <typename V> void Reflect(V& v, D3D12_RECT& s) { R_INT(left); R_INT(top); R_INT(right); R_INT(bottom); }

template <typename V> void Reflect(V& v, D3D12_VIEWPORT& s) {
    R_FLOAT(TopLeftX); R_FLOAT(TopLeftY); R_FLOAT(Width); R_FLOAT(Height); R_FLOAT(MinDepth); R_FLOAT(MaxDepth);
}

template <typename V> void Reflect(V& v, D3D12_SUBRESOURCE_FOOTPRINT& s) {
    R_ENUM(Format, DXGI_FORMAT); R_INT(Width); R_INT(Height); R_INT(Depth); R_INT(RowPitch);
}
template <typename V> void Reflect(V& v, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& s) { R_INT(Offset); R_STRUCT(Footprint); }

template <typename V> void Reflect(V& v, D3D12_TEXTURE_COPY_LOCATION& s) {
    R_OBJECT(pResource, "ID3D12Resource");
    R_ENUM(Type, D3D12_TEXTURE_COPY_TYPE);
    if (s.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) R_STRUCT(PlacedFootprint);
    else R_INT(SubresourceIndex);
}

// ---------------------------------------------------------------------------------------------
// Views and samplers

template <typename V> void Reflect(V& v, D3D12_SHADER_RESOURCE_VIEW_DESC& s) {
    R_ENUM(Format, DXGI_FORMAT);
    R_ENUM(ViewDimension, D3D12_SRV_DIMENSION);
    v.ComponentMapping("Shader4ComponentMapping", s.Shader4ComponentMapping);
    switch (s.ViewDimension) {
        case D3D12_SRV_DIMENSION_BUFFER: {
            auto b = v.Nested("Buffer");
            b.Int("FirstElement", s.Buffer.FirstElement); b.Int("NumElements", s.Buffer.NumElements);
            b.Int("StructureByteStride", s.Buffer.StructureByteStride); b.Flags("Flags", s.Buffer.Flags, DX_TABLE(D3D12_BUFFER_SRV_FLAGS));
            break;
        }
        case D3D12_SRV_DIMENSION_TEXTURE1D: {
            auto b = v.Nested("Texture1D");
            b.Int("MostDetailedMip", s.Texture1D.MostDetailedMip); b.Int("MipLevels", s.Texture1D.MipLevels);
            b.Float("ResourceMinLODClamp", s.Texture1D.ResourceMinLODClamp);
            break;
        }
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: {
            auto b = v.Nested("Texture1DArray");
            b.Int("MostDetailedMip", s.Texture1DArray.MostDetailedMip); b.Int("MipLevels", s.Texture1DArray.MipLevels);
            b.Int("FirstArraySlice", s.Texture1DArray.FirstArraySlice); b.Int("ArraySize", s.Texture1DArray.ArraySize);
            b.Float("ResourceMinLODClamp", s.Texture1DArray.ResourceMinLODClamp);
            break;
        }
        case D3D12_SRV_DIMENSION_TEXTURE2D: {
            auto b = v.Nested("Texture2D");
            b.Int("MostDetailedMip", s.Texture2D.MostDetailedMip); b.Int("MipLevels", s.Texture2D.MipLevels);
            b.Int("PlaneSlice", s.Texture2D.PlaneSlice); b.Float("ResourceMinLODClamp", s.Texture2D.ResourceMinLODClamp);
            break;
        }
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: {
            auto b = v.Nested("Texture2DArray");
            b.Int("MostDetailedMip", s.Texture2DArray.MostDetailedMip); b.Int("MipLevels", s.Texture2DArray.MipLevels);
            b.Int("FirstArraySlice", s.Texture2DArray.FirstArraySlice); b.Int("ArraySize", s.Texture2DArray.ArraySize);
            b.Int("PlaneSlice", s.Texture2DArray.PlaneSlice); b.Float("ResourceMinLODClamp", s.Texture2DArray.ResourceMinLODClamp);
            break;
        }
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: {
            auto b = v.Nested("Texture2DMSArray");
            b.Int("FirstArraySlice", s.Texture2DMSArray.FirstArraySlice); b.Int("ArraySize", s.Texture2DMSArray.ArraySize);
            break;
        }
        case D3D12_SRV_DIMENSION_TEXTURE3D: {
            auto b = v.Nested("Texture3D");
            b.Int("MostDetailedMip", s.Texture3D.MostDetailedMip); b.Int("MipLevels", s.Texture3D.MipLevels);
            b.Float("ResourceMinLODClamp", s.Texture3D.ResourceMinLODClamp);
            break;
        }
        case D3D12_SRV_DIMENSION_TEXTURECUBE: {
            auto b = v.Nested("TextureCube");
            b.Int("MostDetailedMip", s.TextureCube.MostDetailedMip); b.Int("MipLevels", s.TextureCube.MipLevels);
            b.Float("ResourceMinLODClamp", s.TextureCube.ResourceMinLODClamp);
            break;
        }
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: {
            auto b = v.Nested("TextureCubeArray");
            b.Int("MostDetailedMip", s.TextureCubeArray.MostDetailedMip); b.Int("MipLevels", s.TextureCubeArray.MipLevels);
            b.Int("First2DArrayFace", s.TextureCubeArray.First2DArrayFace); b.Int("NumCubes", s.TextureCubeArray.NumCubes);
            b.Float("ResourceMinLODClamp", s.TextureCubeArray.ResourceMinLODClamp);
            break;
        }
        case D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE: {
            auto b = v.Nested("RaytracingAccelerationStructure");
            b.Address("Location", s.RaytracingAccelerationStructure.Location);
            break;
        }
        default: break;   // TEXTURE2DMS has no members
    }
}

template <typename V> void Reflect(V& v, D3D12_UNORDERED_ACCESS_VIEW_DESC& s) {
    R_ENUM(Format, DXGI_FORMAT);
    R_ENUM(ViewDimension, D3D12_UAV_DIMENSION);
    switch (s.ViewDimension) {
        case D3D12_UAV_DIMENSION_BUFFER: {
            auto b = v.Nested("Buffer");
            b.Int("FirstElement", s.Buffer.FirstElement); b.Int("NumElements", s.Buffer.NumElements);
            b.Int("StructureByteStride", s.Buffer.StructureByteStride); b.Int("CounterOffsetInBytes", s.Buffer.CounterOffsetInBytes);
            b.Flags("Flags", s.Buffer.Flags, DX_TABLE(D3D12_BUFFER_UAV_FLAGS));
            break;
        }
        case D3D12_UAV_DIMENSION_TEXTURE1D: { auto b = v.Nested("Texture1D"); b.Int("MipSlice", s.Texture1D.MipSlice); break; }
        case D3D12_UAV_DIMENSION_TEXTURE1DARRAY: {
            auto b = v.Nested("Texture1DArray");
            b.Int("MipSlice", s.Texture1DArray.MipSlice); b.Int("FirstArraySlice", s.Texture1DArray.FirstArraySlice);
            b.Int("ArraySize", s.Texture1DArray.ArraySize);
            break;
        }
        case D3D12_UAV_DIMENSION_TEXTURE2D: {
            auto b = v.Nested("Texture2D");
            b.Int("MipSlice", s.Texture2D.MipSlice); b.Int("PlaneSlice", s.Texture2D.PlaneSlice);
            break;
        }
        case D3D12_UAV_DIMENSION_TEXTURE2DARRAY: {
            auto b = v.Nested("Texture2DArray");
            b.Int("MipSlice", s.Texture2DArray.MipSlice); b.Int("FirstArraySlice", s.Texture2DArray.FirstArraySlice);
            b.Int("ArraySize", s.Texture2DArray.ArraySize); b.Int("PlaneSlice", s.Texture2DArray.PlaneSlice);
            break;
        }
        case D3D12_UAV_DIMENSION_TEXTURE3D: {
            auto b = v.Nested("Texture3D");
            b.Int("MipSlice", s.Texture3D.MipSlice); b.Int("FirstWSlice", s.Texture3D.FirstWSlice); b.Int("WSize", s.Texture3D.WSize);
            break;
        }
        default: break;
    }
}

template <typename V> void Reflect(V& v, D3D12_RENDER_TARGET_VIEW_DESC& s) {
    R_ENUM(Format, DXGI_FORMAT);
    R_ENUM(ViewDimension, D3D12_RTV_DIMENSION);
    switch (s.ViewDimension) {
        case D3D12_RTV_DIMENSION_BUFFER: {
            auto b = v.Nested("Buffer");
            b.Int("FirstElement", s.Buffer.FirstElement); b.Int("NumElements", s.Buffer.NumElements);
            break;
        }
        case D3D12_RTV_DIMENSION_TEXTURE1D: { auto b = v.Nested("Texture1D"); b.Int("MipSlice", s.Texture1D.MipSlice); break; }
        case D3D12_RTV_DIMENSION_TEXTURE1DARRAY: {
            auto b = v.Nested("Texture1DArray");
            b.Int("MipSlice", s.Texture1DArray.MipSlice); b.Int("FirstArraySlice", s.Texture1DArray.FirstArraySlice);
            b.Int("ArraySize", s.Texture1DArray.ArraySize);
            break;
        }
        case D3D12_RTV_DIMENSION_TEXTURE2D: {
            auto b = v.Nested("Texture2D");
            b.Int("MipSlice", s.Texture2D.MipSlice); b.Int("PlaneSlice", s.Texture2D.PlaneSlice);
            break;
        }
        case D3D12_RTV_DIMENSION_TEXTURE2DARRAY: {
            auto b = v.Nested("Texture2DArray");
            b.Int("MipSlice", s.Texture2DArray.MipSlice); b.Int("FirstArraySlice", s.Texture2DArray.FirstArraySlice);
            b.Int("ArraySize", s.Texture2DArray.ArraySize); b.Int("PlaneSlice", s.Texture2DArray.PlaneSlice);
            break;
        }
        case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY: {
            auto b = v.Nested("Texture2DMSArray");
            b.Int("FirstArraySlice", s.Texture2DMSArray.FirstArraySlice); b.Int("ArraySize", s.Texture2DMSArray.ArraySize);
            break;
        }
        case D3D12_RTV_DIMENSION_TEXTURE3D: {
            auto b = v.Nested("Texture3D");
            b.Int("MipSlice", s.Texture3D.MipSlice); b.Int("FirstWSlice", s.Texture3D.FirstWSlice); b.Int("WSize", s.Texture3D.WSize);
            break;
        }
        default: break;
    }
}

template <typename V> void Reflect(V& v, D3D12_DEPTH_STENCIL_VIEW_DESC& s) {
    R_ENUM(Format, DXGI_FORMAT);
    R_ENUM(ViewDimension, D3D12_DSV_DIMENSION);
    R_FLAGS(Flags, D3D12_DSV_FLAGS);
    switch (s.ViewDimension) {
        case D3D12_DSV_DIMENSION_TEXTURE1D: { auto b = v.Nested("Texture1D"); b.Int("MipSlice", s.Texture1D.MipSlice); break; }
        case D3D12_DSV_DIMENSION_TEXTURE1DARRAY: {
            auto b = v.Nested("Texture1DArray");
            b.Int("MipSlice", s.Texture1DArray.MipSlice); b.Int("FirstArraySlice", s.Texture1DArray.FirstArraySlice);
            b.Int("ArraySize", s.Texture1DArray.ArraySize);
            break;
        }
        case D3D12_DSV_DIMENSION_TEXTURE2D: { auto b = v.Nested("Texture2D"); b.Int("MipSlice", s.Texture2D.MipSlice); break; }
        case D3D12_DSV_DIMENSION_TEXTURE2DARRAY: {
            auto b = v.Nested("Texture2DArray");
            b.Int("MipSlice", s.Texture2DArray.MipSlice); b.Int("FirstArraySlice", s.Texture2DArray.FirstArraySlice);
            b.Int("ArraySize", s.Texture2DArray.ArraySize);
            break;
        }
        case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY: {
            auto b = v.Nested("Texture2DMSArray");
            b.Int("FirstArraySlice", s.Texture2DMSArray.FirstArraySlice); b.Int("ArraySize", s.Texture2DMSArray.ArraySize);
            break;
        }
        default: break;
    }
}

template <typename V> void Reflect(V& v, D3D12_CONSTANT_BUFFER_VIEW_DESC& s) { v.Address("BufferLocation", s.BufferLocation); R_INT(SizeInBytes); }

template <typename V> void Reflect(V& v, D3D12_SAMPLER_DESC& s) {
    R_ENUM(Filter, D3D12_FILTER);
    R_ENUM(AddressU, D3D12_TEXTURE_ADDRESS_MODE);
    R_ENUM(AddressV, D3D12_TEXTURE_ADDRESS_MODE);
    R_ENUM(AddressW, D3D12_TEXTURE_ADDRESS_MODE);
    R_FLOAT(MipLODBias);
    R_INT(MaxAnisotropy);
    R_ENUM(ComparisonFunc, D3D12_COMPARISON_FUNC);
    v.FixedFloats("BorderColor", s.BorderColor, 4);
    R_FLOAT(MinLOD);
    R_FLOAT(MaxLOD);
}

template <typename V> void Reflect(V& v, D3D12_VERTEX_BUFFER_VIEW& s) { v.Address("BufferLocation", s.BufferLocation); R_INT(SizeInBytes); R_INT(StrideInBytes); }
template <typename V> void Reflect(V& v, D3D12_INDEX_BUFFER_VIEW& s) { v.Address("BufferLocation", s.BufferLocation); R_INT(SizeInBytes); R_ENUM(Format, DXGI_FORMAT); }
template <typename V> void Reflect(V& v, D3D12_STREAM_OUTPUT_BUFFER_VIEW& s) {
    v.Address("BufferLocation", s.BufferLocation); R_INT(SizeInBytes); v.Address("BufferFilledSizeLocation", s.BufferFilledSizeLocation);
}

// ---------------------------------------------------------------------------------------------
// Pipelines. Shader bytecode is not in the arguments (the capture keeps it as the pipeline's blobs),
// so the stages are filled by the replayer, not here.

template <typename V> void Reflect(V& v, D3D12_INPUT_ELEMENT_DESC& s) {
    v.String("SemanticName", s.SemanticName);
    R_INT(SemanticIndex);
    R_ENUM(Format, DXGI_FORMAT);
    R_INT(InputSlot);
    R_INT(AlignedByteOffset);
    R_ENUM(InputSlotClass, D3D12_INPUT_CLASSIFICATION);
    R_INT(InstanceDataStepRate);
}
template <typename V> void Reflect(V& v, D3D12_INPUT_LAYOUT_DESC& s) { R_ARRAY(pInputElementDescs, NumElements); }

template <typename V> void Reflect(V& v, D3D12_SO_DECLARATION_ENTRY& s) {
    R_INT(Stream); v.String("SemanticName", s.SemanticName); R_INT(SemanticIndex); R_INT(StartComponent); R_INT(ComponentCount); R_INT(OutputSlot);
}
template <typename V> void Reflect(V& v, D3D12_STREAM_OUTPUT_DESC& s) {
    R_ARRAY(pSODeclaration, NumEntries);
    v.Ints("pBufferStrides", s.pBufferStrides, "NumStrides", s.NumStrides, "UINT");
    R_INT(RasterizedStream);
}

template <typename V> void Reflect(V& v, D3D12_RENDER_TARGET_BLEND_DESC& s) {
    R_BOOL(BlendEnable);
    R_BOOL(LogicOpEnable);
    R_ENUM(SrcBlend, D3D12_BLEND);
    R_ENUM(DestBlend, D3D12_BLEND);
    R_ENUM(BlendOp, D3D12_BLEND_OP);
    R_ENUM(SrcBlendAlpha, D3D12_BLEND);
    R_ENUM(DestBlendAlpha, D3D12_BLEND);
    R_ENUM(BlendOpAlpha, D3D12_BLEND_OP);
    R_ENUM(LogicOp, D3D12_LOGIC_OP);
    R_FLAGS(RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE);
}
template <typename V> void Reflect(V& v, D3D12_BLEND_DESC& s) {
    R_BOOL(AlphaToCoverageEnable);
    R_BOOL(IndependentBlendEnable);
    v.FixedStructs("RenderTarget", s.RenderTarget, 8);
}

template <typename V> void Reflect(V& v, D3D12_RASTERIZER_DESC& s) {
    R_ENUM(FillMode, D3D12_FILL_MODE);
    R_ENUM(CullMode, D3D12_CULL_MODE);
    R_BOOL(FrontCounterClockwise);
    R_INT(DepthBias);
    R_FLOAT(DepthBiasClamp);
    R_FLOAT(SlopeScaledDepthBias);
    R_BOOL(DepthClipEnable);
    R_BOOL(MultisampleEnable);
    R_BOOL(AntialiasedLineEnable);
    R_INT(ForcedSampleCount);
    R_ENUM(ConservativeRaster, D3D12_CONSERVATIVE_RASTERIZATION_MODE);
}

template <typename V> void Reflect(V& v, D3D12_DEPTH_STENCILOP_DESC& s) {
    R_ENUM(StencilFailOp, D3D12_STENCIL_OP);
    R_ENUM(StencilDepthFailOp, D3D12_STENCIL_OP);
    R_ENUM(StencilPassOp, D3D12_STENCIL_OP);
    R_ENUM(StencilFunc, D3D12_COMPARISON_FUNC);
}
template <typename V> void Reflect(V& v, D3D12_DEPTH_STENCIL_DESC& s) {
    R_BOOL(DepthEnable);
    R_ENUM(DepthWriteMask, D3D12_DEPTH_WRITE_MASK);
    R_ENUM(DepthFunc, D3D12_COMPARISON_FUNC);
    R_BOOL(StencilEnable);
    R_INT(StencilReadMask);
    R_INT(StencilWriteMask);
    R_STRUCT(FrontFace);
    R_STRUCT(BackFace);
}
template <typename V> void Reflect(V& v, D3D12_DEPTH_STENCIL_DESC1& s) {
    R_BOOL(DepthEnable);
    R_ENUM(DepthWriteMask, D3D12_DEPTH_WRITE_MASK);
    R_ENUM(DepthFunc, D3D12_COMPARISON_FUNC);
    R_BOOL(StencilEnable);
    R_INT(StencilReadMask);
    R_INT(StencilWriteMask);
    R_STRUCT(FrontFace);
    R_STRUCT(BackFace);
    R_BOOL(DepthBoundsTestEnable);
}

/** The shader stages and the cached blob are the replayer's to fill (see above); everything else is here. */
template <typename V> void Reflect(V& v, D3D12_GRAPHICS_PIPELINE_STATE_DESC& s) {
    R_OBJECT(pRootSignature, "ID3D12RootSignature");
    v.Bytecode("VS", s.VS, "vertex");
    v.Bytecode("PS", s.PS, "fragment");
    v.Bytecode("DS", s.DS, "tess_eval");
    v.Bytecode("HS", s.HS, "tess_control");
    v.Bytecode("GS", s.GS, "geometry");
    R_STRUCT(StreamOutput);
    R_STRUCT(BlendState);
    R_INT(SampleMask);
    R_STRUCT(RasterizerState);
    R_STRUCT(DepthStencilState);
    R_STRUCT(InputLayout);
    R_ENUM(IBStripCutValue, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
    R_ENUM(PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE);
    R_INT(NumRenderTargets);
    v.FixedEnums("RTVFormats", s.RTVFormats, 8, s.NumRenderTargets, DX_TABLE(DXGI_FORMAT));
    R_ENUM(DSVFormat, DXGI_FORMAT);
    R_STRUCT(SampleDesc);
    R_INT(NodeMask);
    R_FLAGS(Flags, D3D12_PIPELINE_STATE_FLAGS);
}
template <typename V> void Reflect(V& v, D3D12_COMPUTE_PIPELINE_STATE_DESC& s) {
    R_OBJECT(pRootSignature, "ID3D12RootSignature");
    v.Bytecode("CS", s.CS, "compute");
    R_INT(NodeMask);
    R_FLAGS(Flags, D3D12_PIPELINE_STATE_FLAGS);
}

// ---------------------------------------------------------------------------------------------
// Root signatures, heaps, queues, signatures

template <typename V> void Reflect(V& v, D3D12_DESCRIPTOR_RANGE& s) {
    R_ENUM(RangeType, D3D12_DESCRIPTOR_RANGE_TYPE); R_INT(NumDescriptors); R_INT(BaseShaderRegister); R_INT(RegisterSpace);
    R_INT(OffsetInDescriptorsFromTableStart);
}
template <typename V> void Reflect(V& v, D3D12_DESCRIPTOR_RANGE1& s) {
    R_ENUM(RangeType, D3D12_DESCRIPTOR_RANGE_TYPE); R_INT(NumDescriptors); R_INT(BaseShaderRegister); R_INT(RegisterSpace);
    R_FLAGS(Flags, D3D12_DESCRIPTOR_RANGE_FLAGS);
    R_INT(OffsetInDescriptorsFromTableStart);
}

template <typename V, typename Parameter> void ReflectRootParameter(V& v, Parameter& s, bool flags) {
    R_ENUM(ParameterType, D3D12_ROOT_PARAMETER_TYPE);
    switch (s.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            auto b = v.Nested("DescriptorTable");
            b.Array("pDescriptorRanges", s.DescriptorTable.pDescriptorRanges, "NumDescriptorRanges", s.DescriptorTable.NumDescriptorRanges);
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
            auto b = v.Nested("Constants");
            b.Int("ShaderRegister", s.Constants.ShaderRegister); b.Int("RegisterSpace", s.Constants.RegisterSpace);
            b.Int("Num32BitValues", s.Constants.Num32BitValues);
            break;
        }
        default: {
            auto b = v.Nested("Descriptor");
            b.Int("ShaderRegister", s.Descriptor.ShaderRegister); b.Int("RegisterSpace", s.Descriptor.RegisterSpace);
            (void)flags;
            break;
        }
    }
    R_ENUM(ShaderVisibility, D3D12_SHADER_VISIBILITY);
}
template <typename V> void Reflect(V& v, D3D12_ROOT_PARAMETER& s) { ReflectRootParameter(v, s, false); }
template <typename V> void Reflect(V& v, D3D12_ROOT_PARAMETER1& s) {
    ReflectRootParameter(v, s, true);
    if (s.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV || s.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ||
        s.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV) {
        auto b = v.Nested("Descriptor");
        b.Flags("Flags", s.Descriptor.Flags, DX_TABLE(D3D12_ROOT_DESCRIPTOR_FLAGS));
    }
}

template <typename V> void Reflect(V& v, D3D12_STATIC_SAMPLER_DESC& s) {
    R_ENUM(Filter, D3D12_FILTER);
    R_ENUM(AddressU, D3D12_TEXTURE_ADDRESS_MODE);
    R_ENUM(AddressV, D3D12_TEXTURE_ADDRESS_MODE);
    R_ENUM(AddressW, D3D12_TEXTURE_ADDRESS_MODE);
    R_FLOAT(MipLODBias);
    R_INT(MaxAnisotropy);
    R_ENUM(ComparisonFunc, D3D12_COMPARISON_FUNC);
    R_ENUM(BorderColor, D3D12_STATIC_BORDER_COLOR);
    R_FLOAT(MinLOD);
    R_FLOAT(MaxLOD);
    R_INT(ShaderRegister);
    R_INT(RegisterSpace);
    R_ENUM(ShaderVisibility, D3D12_SHADER_VISIBILITY);
}

template <typename V> void Reflect(V& v, D3D12_ROOT_SIGNATURE_DESC& s) {
    R_ARRAY(pParameters, NumParameters);
    R_ARRAY(pStaticSamplers, NumStaticSamplers);
    R_FLAGS(Flags, D3D12_ROOT_SIGNATURE_FLAGS);
}
template <typename V> void Reflect(V& v, D3D12_ROOT_SIGNATURE_DESC1& s) {
    R_ARRAY(pParameters, NumParameters);
    R_ARRAY(pStaticSamplers, NumStaticSamplers);
    R_FLAGS(Flags, D3D12_ROOT_SIGNATURE_FLAGS);
}
template <typename V> void Reflect(V& v, D3D12_VERSIONED_ROOT_SIGNATURE_DESC& s) {
    R_ENUM(Version, D3D_ROOT_SIGNATURE_VERSION);
    if (s.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) R_STRUCT(Desc_1_0);
    else if (s.Version == D3D_ROOT_SIGNATURE_VERSION_1_1) R_STRUCT(Desc_1_1);
}

template <typename V> void Reflect(V& v, D3D12_DESCRIPTOR_HEAP_DESC& s) {
    R_ENUM(Type, D3D12_DESCRIPTOR_HEAP_TYPE); R_INT(NumDescriptors); R_FLAGS(Flags, D3D12_DESCRIPTOR_HEAP_FLAGS); R_INT(NodeMask);
}
template <typename V> void Reflect(V& v, D3D12_COMMAND_QUEUE_DESC& s) {
    R_ENUM(Type, D3D12_COMMAND_LIST_TYPE); R_ENUM(Priority, D3D12_COMMAND_QUEUE_PRIORITY); R_FLAGS(Flags, D3D12_COMMAND_QUEUE_FLAGS); R_INT(NodeMask);
}
template <typename V> void Reflect(V& v, D3D12_QUERY_HEAP_DESC& s) { R_ENUM(Type, D3D12_QUERY_HEAP_TYPE); R_INT(Count); R_INT(NodeMask); }

template <typename V> void Reflect(V& v, D3D12_INDIRECT_ARGUMENT_DESC& s) {
    R_ENUM(Type, D3D12_INDIRECT_ARGUMENT_TYPE);
    switch (s.Type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: { auto b = v.Nested("VertexBuffer"); b.Int("Slot", s.VertexBuffer.Slot); break; }
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: {
            auto b = v.Nested("Constant");
            b.Int("RootParameterIndex", s.Constant.RootParameterIndex); b.Int("DestOffsetIn32BitValues", s.Constant.DestOffsetIn32BitValues);
            b.Int("Num32BitValuesToSet", s.Constant.Num32BitValuesToSet);
            break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW: {
            auto b = v.Nested("ConstantBufferView"); b.Int("RootParameterIndex", s.ConstantBufferView.RootParameterIndex); break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: {
            auto b = v.Nested("ShaderResourceView"); b.Int("RootParameterIndex", s.ShaderResourceView.RootParameterIndex); break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
            auto b = v.Nested("UnorderedAccessView"); b.Int("RootParameterIndex", s.UnorderedAccessView.RootParameterIndex); break;
        }
        default: break;   // draws and dispatches carry no parameters
    }
}
template <typename V> void Reflect(V& v, D3D12_COMMAND_SIGNATURE_DESC& s) {
    R_INT(ByteStride);
    R_ARRAY(pArgumentDescs, NumArgumentDescs);
    R_INT(NodeMask);
}

// ---------------------------------------------------------------------------------------------
// Commands

template <typename V> void Reflect(V& v, D3D12_RESOURCE_BARRIER& s) {
    R_ENUM(Type, D3D12_RESOURCE_BARRIER_TYPE);
    R_FLAGS(Flags, D3D12_RESOURCE_BARRIER_FLAGS);
    switch (s.Type) {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION: {
            auto b = v.Nested("Transition");
            b.Object("pResource", s.Transition.pResource, "ID3D12Resource");
            b.Int("Subresource", s.Transition.Subresource);
            b.Flags("StateBefore", s.Transition.StateBefore, DX_TABLE(D3D12_RESOURCE_STATES));
            b.Flags("StateAfter", s.Transition.StateAfter, DX_TABLE(D3D12_RESOURCE_STATES));
            break;
        }
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING: {
            auto b = v.Nested("Aliasing");
            b.Object("pResourceBefore", s.Aliasing.pResourceBefore, "ID3D12Resource");
            b.Object("pResourceAfter", s.Aliasing.pResourceAfter, "ID3D12Resource");
            break;
        }
        case D3D12_RESOURCE_BARRIER_TYPE_UAV: {
            auto b = v.Nested("UAV");
            b.Object("pResource", s.UAV.pResource, "ID3D12Resource");
            break;
        }
        default: break;
    }
}

template <typename V> void Reflect(V& v, D3D12_RENDER_PASS_BEGINNING_ACCESS& s) {
    R_ENUM(Type, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE);
    if (s.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
        auto b = v.Nested("Clear");
        b.Struct("ClearValue", s.Clear.ClearValue);
    }
}

template <typename V> void Reflect(V& v, D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS& s) {
    R_INT(SrcSubresource); R_INT(DstSubresource); R_INT(DstX); R_INT(DstY); R_STRUCT(SrcRect);
}
template <typename V> void Reflect(V& v, D3D12_RENDER_PASS_ENDING_ACCESS& s) {
    R_ENUM(Type, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE);
    if (s.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE) {
        auto b = v.Nested("Resolve");
        b.Object("pSrcResource", s.Resolve.pSrcResource, "ID3D12Resource");
        b.Object("pDstResource", s.Resolve.pDstResource, "ID3D12Resource");
        b.Array("pSubresourceParameters", s.Resolve.pSubresourceParameters, "SubresourceCount", s.Resolve.SubresourceCount);
        b.Enum("Format", s.Resolve.Format, DX_TABLE(DXGI_FORMAT));
        b.Enum("ResolveMode", s.Resolve.ResolveMode, DX_TABLE(D3D12_RESOLVE_MODE));
        b.Bool("PreserveResolveSource", s.Resolve.PreserveResolveSource);
    }
}
template <typename V> void Reflect(V& v, D3D12_RENDER_PASS_RENDER_TARGET_DESC& s) {
    v.CpuHandle("cpuDescriptor", s.cpuDescriptor);
    R_STRUCT(BeginningAccess);
    R_STRUCT(EndingAccess);
}
template <typename V> void Reflect(V& v, D3D12_RENDER_PASS_DEPTH_STENCIL_DESC& s) {
    v.CpuHandle("cpuDescriptor", s.cpuDescriptor);
    R_STRUCT(DepthBeginningAccess);
    R_STRUCT(StencilBeginningAccess);
    R_STRUCT(DepthEndingAccess);
    R_STRUCT(StencilEndingAccess);
}

template <typename V> void Reflect(V& v, D3D12_DISCARD_REGION& s) {
    v.Array("pRects", s.pRects, "NumRects", s.NumRects);
    R_INT(FirstSubresource);
    R_INT(NumSubresources);
}

// ---------------------------------------------------------------------------------------------
// Ray tracing
//
// A build and a trace name everything they read by GPU virtual address, which is exactly what
// Address() exists for: the capture wrote each one as the buffer and offset it resolved to
// (src/d3d12/src/raytracing.h), so the same descriptions fill with this machine's addresses.
//
// Two things these do not carry, and the replay has to put right itself (dx_raytracing.cpp): the
// bytes inside an instance buffer, which hold the captured process's bottom level addresses, and
// the bytes inside a binding table, which hold the captured state object's shader identifiers.

template <typename V> void Reflect(V& v, D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE& s) {
    v.Address("StartAddress", s.StartAddress);
    R_INT(StrideInBytes);
}

template <typename V> void Reflect(V& v, D3D12_GPU_VIRTUAL_ADDRESS_RANGE& s) {
    v.Address("StartAddress", s.StartAddress);
    R_INT(SizeInBytes);
}

template <typename V> void Reflect(V& v, D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE& s) {
    v.Address("StartAddress", s.StartAddress);
    R_INT(SizeInBytes);
    R_INT(StrideInBytes);
}

template <typename V> void Reflect(V& v, D3D12_DISPATCH_RAYS_DESC& s) {
    R_STRUCT(RayGenerationShaderRecord);
    R_STRUCT(MissShaderTable);
    R_STRUCT(HitGroupTable);
    R_STRUCT(CallableShaderTable);
    R_INT(Width);
    R_INT(Height);
    R_INT(Depth);
}

template <typename V> void Reflect(V& v, D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& s) {
    v.Address("Transform3x4", s.Transform3x4);
    R_ENUM(IndexFormat, DXGI_FORMAT);
    R_ENUM(VertexFormat, DXGI_FORMAT);
    R_INT(IndexCount);
    R_INT(VertexCount);
    v.Address("IndexBuffer", s.IndexBuffer);
    R_STRUCT(VertexBuffer);
}

template <typename V> void Reflect(V& v, D3D12_RAYTRACING_GEOMETRY_AABBS_DESC& s) {
    R_INT(AABBCount);
    R_STRUCT(AABBs);
}

template <typename V> void Reflect(V& v, D3D12_RAYTRACING_GEOMETRY_DESC& s) {
    R_ENUM(Type, D3D12_RAYTRACING_GEOMETRY_TYPE);
    R_FLAGS(Flags, D3D12_RAYTRACING_GEOMETRY_FLAGS);
    // The union's arm follows the type, which is always visited first.
    if (s.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) R_STRUCT(AABBs);
    else R_STRUCT(Triangles);
}

/**
 * A build's inputs. `ppGeometryDescs` (D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS) is read as an
 * ordinary array and the layout becomes ARRAY: the geometries are the same either way, and the
 * pointer array only existed so the application could gather them from wherever it kept them.
 */
template <typename V> void Reflect(V& v, D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& s) {
    R_ENUM(Type, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE);
    R_FLAGS(Flags, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS);
    R_INT(NumDescs);
    R_ENUM(DescsLayout, D3D12_ELEMENTS_LAYOUT);
    if (s.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
        v.Address("InstanceDescs", s.InstanceDescs);
    } else if (s.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
        v.Array("pGeometryDescs", s.pGeometryDescs, "NumDescs", s.NumDescs);
        if (!s.pGeometryDescs) {
            v.Array("ppGeometryDescs", s.pGeometryDescs, "NumDescs", s.NumDescs);
            if (s.pGeometryDescs) s.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        }
    }
}

template <typename V> void Reflect(V& v, D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& s) {
    v.Address("DestAccelerationStructureData", s.DestAccelerationStructureData);
    R_STRUCT(Inputs);
    v.Address("SourceAccelerationStructureData", s.SourceAccelerationStructureData);
    v.Address("ScratchAccelerationStructureData", s.ScratchAccelerationStructureData);
}

template <typename V> void Reflect(V& v, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC& s) {
    v.Address("DestBuffer", s.DestBuffer);
    R_ENUM(InfoType, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE);
}

#undef R_INT
#undef R_FLOAT
#undef R_BOOL
#undef R_ENUM
#undef R_FLAGS
#undef R_STRUCT
#undef R_OBJECT
#undef R_ARRAY

} // namespace dxreplay
