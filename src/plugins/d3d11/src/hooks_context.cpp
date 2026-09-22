// The device context: every call an application makes to draw. Each hook forwards, follows what
// the call bound in the context's shadow state (state.h), and, while a capture records, writes
// the call with its arguments (capture.h). Draws and dispatches carry the state snapshot the
// capture takes just before them; the pass boundaries are the capture's business.
//
// A context cannot be hooked the way the other objects are: its vtable lives inside the context
// object, and the runtime rewrites entries (the draws, above all) as the pipeline state changes,
// undoing any patch. So the application is given a RecordingContext, a proxy forwarding every
// method to the real context (gen/d3d11_context_proxy.gen.h), with the recorded ones overridden
// here. The library's own calls go to the real context and so are never seen here.
#include "hooks.h"

#include "../gen/d3d11_context_proxy.gen.h"
#include "../gen/d3d11_enums.gen.h"
#include "capture.h"
#include "serialize.h"
#include "state.h"

#include <d3d11_1.h>

// ID3D11CommandList adds nothing to ID3D11DeviceChild's seven slots.
constexpr uint32_t kDeviceChildSlots = 7;

#include <algorithm>

namespace d3d11insp {

namespace {

void ClearShadow(Context* c) {
    c->state = PipelineState();
}

/**
 * The device context the application is given: every call forwards to the real one, and the ones
 * the capture follows are overridden below. One per real context, made when the context is first
 * seen (WrapContext) and destroyed with it.
 */
class RecordingContext final : public ContextProxy {
public:
    RecordingContext(ID3D11DeviceContext4* real, Context* ctx) : ContextProxy(real), ctx_(ctx) {}

    // IUnknown and ID3D11DeviceChild: the proxy stands for the real object's identity.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;

    void STDMETHODCALLTYPE VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE VSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void STDMETHODCALLTYPE VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void STDMETHODCALLTYPE HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE HSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE HSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void STDMETHODCALLTYPE HSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE HSSetShader(ID3D11HullShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void STDMETHODCALLTYPE DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE DSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE DSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void STDMETHODCALLTYPE DSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE DSSetShader(ID3D11DomainShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void STDMETHODCALLTYPE GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE GSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE GSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void STDMETHODCALLTYPE GSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE GSSetShader(ID3D11GeometryShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void STDMETHODCALLTYPE PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE PSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void STDMETHODCALLTYPE PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void STDMETHODCALLTYPE CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE CSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant, const UINT* pNumConstants) override;
    void STDMETHODCALLTYPE CSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void STDMETHODCALLTYPE CSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE CSSetShader(ID3D11ComputeShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) override;
    void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout* pInputLayout) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) override;
    void STDMETHODCALLTYPE Draw(UINT VertexCount, UINT StartVertexLocation) override;
    void STDMETHODCALLTYPE DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override;
    void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE DrawAuto() override;
    void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override;
    void STDMETHODCALLTYPE DrawInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override;
    void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;
    void STDMETHODCALLTYPE DispatchIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView) override;
    void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) override;
    void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) override;
    void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef) override;
    void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState* pRasterizerState) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT NumBuffers, ID3D11Buffer* const* ppSOTargets, const UINT* pOffsets) override;
    void STDMETHODCALLTYPE SetPredication(ID3D11Predicate* pPredicate, BOOL PredicateValue) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* pUnorderedAccessView, const UINT Values[4]) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* pUnorderedAccessView, const FLOAT Values[4]) override;
    void STDMETHODCALLTYPE ClearView(ID3D11View* pView, const FLOAT Color[4], const D3D11_RECT* pRect, UINT NumRects) override;
    void STDMETHODCALLTYPE DiscardResource(ID3D11Resource* pResource) override;
    void STDMETHODCALLTYPE DiscardView(ID3D11View* pResourceView) override;
    void STDMETHODCALLTYPE DiscardView1(ID3D11View* pResourceView, const D3D11_RECT* pRects, UINT NumRects) override;
    HRESULT STDMETHODCALLTYPE Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) override;
    void STDMETHODCALLTYPE Unmap(ID3D11Resource* pResource, UINT Subresource) override;
    void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override;
    void STDMETHODCALLTYPE UpdateSubresource1(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch, UINT CopyFlags) override;
    void STDMETHODCALLTYPE CopyResource(ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource) override;
    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox) override;
    void STDMETHODCALLTYPE CopySubresourceRegion1(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox, UINT CopyFlags) override;
    void STDMETHODCALLTYPE CopyStructureCount(ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset, ID3D11UnorderedAccessView* pSrcView) override;
    void STDMETHODCALLTYPE ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override;
    void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) override;
    void STDMETHODCALLTYPE Begin(ID3D11Asynchronous* pAsync) override;
    void STDMETHODCALLTYPE End(ID3D11Asynchronous* pAsync) override;
    void STDMETHODCALLTYPE ClearState() override;
    void STDMETHODCALLTYPE Flush() override;
    void STDMETHODCALLTYPE Flush1(D3D11_CONTEXT_TYPE ContextType, HANDLE hEvent) override;
    void STDMETHODCALLTYPE ExecuteCommandList(ID3D11CommandList* pCommandList, BOOL RestoreContextState) override;
    HRESULT STDMETHODCALLTYPE FinishCommandList(BOOL RestoreDeferredContextState, ID3D11CommandList** ppCommandList) override;
    void STDMETHODCALLTYPE SwapDeviceContextState(ID3DDeviceContextState* pState, ID3DDeviceContextState** ppPreviousState) override;
    void STDMETHODCALLTYPE BeginEventInt(LPCWSTR pLabel, INT Data) override;
    void STDMETHODCALLTYPE EndEvent() override;
    void STDMETHODCALLTYPE SetMarkerInt(LPCWSTR pLabel, INT Data) override;

private:
    Context* ctx_;
};

// The interfaces the proxy answers QueryInterface for with itself: the ones it implements, when
// the real context implements them too. Anything else (ID3DUserDefinedAnnotation, ID3D11Multithread,
// the video interfaces) is the real object's.
const IID* const kProxiedInterfaces[] = {
    &__uuidof(IUnknown), &__uuidof(ID3D11DeviceChild), &__uuidof(ID3D11DeviceContext), &__uuidof(ID3D11DeviceContext1),
    &__uuidof(ID3D11DeviceContext2), &__uuidof(ID3D11DeviceContext3), &__uuidof(ID3D11DeviceContext4),
};

HRESULT STDMETHODCALLTYPE RecordingContext::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    for (const IID* iid : kProxiedInterfaces) {
        if (riid != *iid) continue;
        IUnknown* real = nullptr;
        HRESULT hr = real_->QueryInterface(riid, (void**)&real);
        if (FAILED(hr)) { *ppvObject = nullptr; return hr; }
        // The real object's reference stands for the one the caller gets on the proxy.
        *ppvObject = static_cast<ID3D11DeviceContext4*>(this);
        return S_OK;
    }
    return real_->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE RecordingContext::Release() {
    ID3D11DeviceContext4* real = real_;
    const ULONG count = real->Release();
    // A deferred context dies with its last reference. The immediate context is the device's: an
    // application may fetch and release it every frame, and it lives on until the device goes
    // (DestroyContextsOf), so its proxy, its shadow state and its open pass stay with it.
    if (count == 0 && ctx_->deferred) DestroyContext(real);
    return count;
}

HRESULT STDMETHODCALLTYPE RecordingContext::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    HRESULT hr = real_->SetPrivateData(guid, DataSize, pData);
    if (SUCCEEDED(hr) && pData && DataSize) {
        if (guid == WKPDID_D3DDebugObjectName) OnObjectNamed(real_, std::string((const char*)pData, ((const char*)pData)[DataSize - 1] == 0 ? DataSize - 1 : DataSize));
        else if (guid == WKPDID_D3DDebugObjectNameW) OnObjectNamed(real_, Narrow((const wchar_t*)pData, DataSize / sizeof(wchar_t)));
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Shader stages

template <size_t N>
void SetSlots(std::array<ID3D11ShaderResourceView*, N>& slots, UINT start, UINT count, ID3D11ShaderResourceView* const* views) {
    for (UINT i = 0; i < count && start + i < N; ++i) slots[start + i] = views ? views[i] : nullptr;
}

template <size_t N>
void SetSlots(std::array<ID3D11SamplerState*, N>& slots, UINT start, UINT count, ID3D11SamplerState* const* views) {
    for (UINT i = 0; i < count && start + i < N; ++i) slots[start + i] = views ? views[i] : nullptr;
}

void SetConstantBuffers(Context* c, int stage, UINT start, UINT count, ID3D11Buffer* const* buffers, const UINT* first, const UINT* num) {
    StageBindings& b = c->state.stages[stage];
    for (UINT i = 0; i < count && start + i < b.constantBuffers.size(); ++i) {
        ConstantBufferBinding& cb = b.constantBuffers[start + i];
        cb.buffer = buffers ? buffers[i] : nullptr;
        cb.first = first ? first[i] : 0;
        cb.count = num ? num[i] : 0;
    }
}

#define STAGE_HOOKS(Prefix, StageIndex, ShaderType)                                                                                    \
    void STDMETHODCALLTYPE RecordingContext::Prefix##SetConstantBuffers(UINT StartSlot, UINT NumBuffers,             \
                                                             ID3D11Buffer* const* ppConstantBuffers) {                                  \
        real_->Prefix##SetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);                                               \
        Context* c = ctx_;                                                                                                        \
        if (!c) return;                                                                                                                \
        SetConstantBuffers(c, StageIndex, StartSlot, NumBuffers, ppConstantBuffers, nullptr, nullptr);                                  \
        if (Recording()) {                                                                                                             \
            Args a;                                                                                                                    \
            a.u("StartSlot", StartSlot).u("NumBuffers", NumBuffers).refs("ppConstantBuffers", ppConstantBuffers, ppConstantBuffers ? NumBuffers : 0); \
            Record(c, #Prefix "SetConstantBuffers", a.str());                                                                          \
        }                                                                                                                              \
    }                                                                                                                                  \
    void STDMETHODCALLTYPE RecordingContext::Prefix##SetConstantBuffers1(UINT StartSlot, UINT NumBuffers,            \
                                                              ID3D11Buffer* const* ppConstantBuffers, const UINT* pFirstConstant,       \
                                                              const UINT* pNumConstants) {                                              \
        real_->Prefix##SetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);               \
        Context* c = ctx_;                                                                                                        \
        if (!c) return;                                                                                                                \
        SetConstantBuffers(c, StageIndex, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);                     \
        if (Recording()) {                                                                                                             \
            Args a;                                                                                                                    \
            a.u("StartSlot", StartSlot).u("NumBuffers", NumBuffers).refs("ppConstantBuffers", ppConstantBuffers, ppConstantBuffers ? NumBuffers : 0) \
                .uints("pFirstConstant", pFirstConstant, NumBuffers).uints("pNumConstants", pNumConstants, NumBuffers);                \
            Record(c, #Prefix "SetConstantBuffers1", a.str());                                                                         \
        }                                                                                                                              \
    }                                                                                                                                  \
    void STDMETHODCALLTYPE RecordingContext::Prefix##SetShaderResources(UINT StartSlot, UINT NumViews,               \
                                                             ID3D11ShaderResourceView* const* ppShaderResourceViews) {                  \
        real_->Prefix##SetShaderResources(StartSlot, NumViews, ppShaderResourceViews);                                             \
        Context* c = ctx_;                                                                                                        \
        if (!c) return;                                                                                                                \
        SetSlots(c->state.stages[StageIndex].resources, StartSlot, NumViews, ppShaderResourceViews);                                   \
        if (Recording()) {                                                                                                             \
            Args a;                                                                                                                    \
            a.u("StartSlot", StartSlot).u("NumViews", NumViews).refs("ppShaderResourceViews", ppShaderResourceViews, ppShaderResourceViews ? NumViews : 0); \
            Record(c, #Prefix "SetShaderResources", a.str());                                                                          \
        }                                                                                                                              \
    }                                                                                                                                  \
    void STDMETHODCALLTYPE RecordingContext::Prefix##SetSamplers(UINT StartSlot, UINT NumSamplers,                   \
                                                      ID3D11SamplerState* const* ppSamplers) {                                         \
        real_->Prefix##SetSamplers(StartSlot, NumSamplers, ppSamplers);                                                            \
        Context* c = ctx_;                                                                                                        \
        if (!c) return;                                                                                                                \
        SetSlots(c->state.stages[StageIndex].samplers, StartSlot, NumSamplers, ppSamplers);                                            \
        if (Recording()) {                                                                                                             \
            Args a;                                                                                                                    \
            a.u("StartSlot", StartSlot).u("NumSamplers", NumSamplers).refs("ppSamplers", ppSamplers, ppSamplers ? NumSamplers : 0);   \
            Record(c, #Prefix "SetSamplers", a.str());                                                                                 \
        }                                                                                                                              \
    }                                                                                                                                  \
    void STDMETHODCALLTYPE RecordingContext::Prefix##SetShader(ShaderType* pShader,                                  \
                                                    ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) {            \
        real_->Prefix##SetShader(pShader, ppClassInstances, NumClassInstances);                                                    \
        Context* c = ctx_;                                                                                                        \
        if (!c) return;                                                                                                                \
        c->state.stages[StageIndex].shader = pShader;                                                                                  \
        if (Recording()) {                                                                                                             \
            Args a;                                                                                                                    \
            a.ref("pShader", pShader, #ShaderType).u("NumClassInstances", NumClassInstances);                                          \
            Record(c, #Prefix "SetShader", a.str());                                                                                   \
        }                                                                                                                              \
    }

STAGE_HOOKS(VS, VS, ID3D11VertexShader)
STAGE_HOOKS(HS, HS, ID3D11HullShader)
STAGE_HOOKS(DS, DS, ID3D11DomainShader)
STAGE_HOOKS(GS, GS, ID3D11GeometryShader)
STAGE_HOOKS(PS, PS, ID3D11PixelShader)
STAGE_HOOKS(CS, CS, ID3D11ComputeShader)

void STDMETHODCALLTYPE RecordingContext::CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) {
    real_->CSSetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
    Context* c = ctx_;
    if (!c) return;
    for (UINT i = 0; i < NumUAVs && StartSlot + i < c->state.csUavs.size(); ++i) c->state.csUavs[StartSlot + i] = ppUnorderedAccessViews ? ppUnorderedAccessViews[i] : nullptr;
    if (Recording()) {
        Args a;
        a.u("StartSlot", StartSlot).u("NumUAVs", NumUAVs).refs("ppUnorderedAccessViews", ppUnorderedAccessViews, ppUnorderedAccessViews ? NumUAVs : 0).uints("pUAVInitialCounts", pUAVInitialCounts, NumUAVs);
        Record(c, "CSSetUnorderedAccessViews", a.str());
    }
}

// ---------------------------------------------------------------------------------------------
// Input assembler

void STDMETHODCALLTYPE RecordingContext::IASetInputLayout(ID3D11InputLayout* pInputLayout) {
    real_->IASetInputLayout(pInputLayout);
    Context* c = ctx_;
    if (!c) return;
    c->state.inputLayout = pInputLayout;
    if (Recording()) {
        Args a;
        a.ref("pInputLayout", pInputLayout, "ID3D11InputLayout");
        Record(c, "IASetInputLayout", a.str());
    }
}

void STDMETHODCALLTYPE RecordingContext::IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets) {
    real_->IASetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
    Context* c = ctx_;
    if (!c) return;
    for (UINT i = 0; i < NumBuffers && StartSlot + i < c->state.vertexBuffers.size(); ++i) {
        VertexBufferBinding& vb = c->state.vertexBuffers[StartSlot + i];
        vb.buffer = ppVertexBuffers ? ppVertexBuffers[i] : nullptr;
        vb.stride = pStrides ? pStrides[i] : 0;
        vb.offset = pOffsets ? pOffsets[i] : 0;
    }
    if (Recording()) {
        Args a;
        a.u("StartSlot", StartSlot).u("NumBuffers", NumBuffers).refs("ppVertexBuffers", ppVertexBuffers, ppVertexBuffers ? NumBuffers : 0)
            .uints("pStrides", pStrides, NumBuffers).uints("pOffsets", pOffsets, NumBuffers);
        Record(c, "IASetVertexBuffers", a.str());
    }
}

void STDMETHODCALLTYPE RecordingContext::IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) {
    real_->IASetIndexBuffer(pIndexBuffer, Format, Offset);
    Context* c = ctx_;
    if (!c) return;
    c->state.indexBuffer = pIndexBuffer;
    c->state.indexFormat = Format;
    c->state.indexOffset = Offset;
    if (Recording()) {
        Args a;
        a.ref("pIndexBuffer", pIndexBuffer, "ID3D11Buffer").e("Format", ToString_DXGI_FORMAT(Format), Format).u("Offset", Offset);
        Record(c, "IASetIndexBuffer", a.str());
    }
}

void STDMETHODCALLTYPE RecordingContext::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) {
    real_->IASetPrimitiveTopology(Topology);
    Context* c = ctx_;
    if (!c) return;
    c->state.topology = Topology;
    if (Recording()) {
        Args a;
        a.e("Topology", ToString_D3D_PRIMITIVE_TOPOLOGY(Topology), Topology);
        Record(c, "IASetPrimitiveTopology", a.str());
    }
}

// ---------------------------------------------------------------------------------------------
// Draws and dispatches

// Around a draw: the pass begins and the state is snapshotted, the draw runs, and what it wrote
// is noted; `call` is the forwarded draw.
#define DRAW_PROLOGUE(call)                                       Context* c = ctx_;                                            std::string state;                                            if (c) state = BeforeDraw(c, p);                              call;                                                         if (!c) return;                                               AfterDraw(c);                                                 if (!Recording()) return;

void STDMETHODCALLTYPE RecordingContext::Draw(UINT VertexCount, UINT StartVertexLocation) {
    DrawParams p;
    p.count = VertexCount;
    p.start = StartVertexLocation;
    DRAW_PROLOGUE(real_->Draw(VertexCount, StartVertexLocation));
    Args a;
    a.u("VertexCount", VertexCount).u("StartVertexLocation", StartVertexLocation);
    Record(c, "Draw", a.str(), std::move(state));
}

void STDMETHODCALLTYPE RecordingContext::DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) {
    DrawParams p;
    p.indexed = true;
    p.count = IndexCount;
    p.start = StartIndexLocation;
    p.baseVertex = BaseVertexLocation;
    DRAW_PROLOGUE(real_->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation));
    Args a;
    a.u("IndexCount", IndexCount).u("StartIndexLocation", StartIndexLocation).i("BaseVertexLocation", BaseVertexLocation);
    Record(c, "DrawIndexed", a.str(), std::move(state));
}

void STDMETHODCALLTYPE RecordingContext::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) {
    DrawParams p;
    p.count = VertexCountPerInstance;
    p.start = StartVertexLocation;
    p.instances = InstanceCount;
    p.startInstance = StartInstanceLocation;
    DRAW_PROLOGUE(real_->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation));
    Args a;
    a.u("VertexCountPerInstance", VertexCountPerInstance).u("InstanceCount", InstanceCount).u("StartVertexLocation", StartVertexLocation).u("StartInstanceLocation", StartInstanceLocation);
    Record(c, "DrawInstanced", a.str(), std::move(state));
}

void STDMETHODCALLTYPE RecordingContext::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) {
    DrawParams p;
    p.indexed = true;
    p.count = IndexCountPerInstance;
    p.start = StartIndexLocation;
    p.baseVertex = BaseVertexLocation;
    p.instances = InstanceCount;
    p.startInstance = StartInstanceLocation;
    DRAW_PROLOGUE(real_->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation));
    Args a;
    a.u("IndexCountPerInstance", IndexCountPerInstance).u("InstanceCount", InstanceCount).u("StartIndexLocation", StartIndexLocation)
        .i("BaseVertexLocation", BaseVertexLocation).u("StartInstanceLocation", StartInstanceLocation);
    Record(c, "DrawIndexedInstanced", a.str(), std::move(state));
}

void STDMETHODCALLTYPE RecordingContext::DrawAuto() {
    DrawParams p;
    p.drawAuto = true;
    DRAW_PROLOGUE(real_->DrawAuto());
    Record(c, "DrawAuto", "{}", std::move(state));
}

void STDMETHODCALLTYPE RecordingContext::DrawIndexedInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) {
    DrawParams p;
    p.indexed = true;
    p.indirect = true;
    p.argsBuffer = pBufferForArgs;
    p.argsOffset = AlignedByteOffsetForArgs;
    DRAW_PROLOGUE(real_->DrawIndexedInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs));
    Args a;
    a.ref("pBufferForArgs", pBufferForArgs, "ID3D11Buffer").u("AlignedByteOffsetForArgs", AlignedByteOffsetForArgs);
    Record(c, "DrawIndexedInstancedIndirect", a.str(), std::move(state));
}

void STDMETHODCALLTYPE RecordingContext::DrawInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) {
    DrawParams p;
    p.indirect = true;
    p.argsBuffer = pBufferForArgs;
    p.argsOffset = AlignedByteOffsetForArgs;
    DRAW_PROLOGUE(real_->DrawInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs));
    Args a;
    a.ref("pBufferForArgs", pBufferForArgs, "ID3D11Buffer").u("AlignedByteOffsetForArgs", AlignedByteOffsetForArgs);
    Record(c, "DrawInstancedIndirect", a.str(), std::move(state));
}

void STDMETHODCALLTYPE RecordingContext::Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    Context* c = ctx_;
    std::string state;
    if (c) state = BeforeDispatch(c, false, nullptr, 0);
    real_->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    if (!c) return;
    AfterDispatch(c);
    if (!Recording()) return;
    Args a;
    a.u("ThreadGroupCountX", ThreadGroupCountX).u("ThreadGroupCountY", ThreadGroupCountY).u("ThreadGroupCountZ", ThreadGroupCountZ);
    Record(c, "Dispatch", a.str(), std::move(state));
}

void STDMETHODCALLTYPE RecordingContext::DispatchIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) {
    Context* c = ctx_;
    std::string state;
    if (c) state = BeforeDispatch(c, true, pBufferForArgs, AlignedByteOffsetForArgs);
    real_->DispatchIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
    if (!c) return;
    AfterDispatch(c);
    if (!Recording()) return;
    Args a;
    a.ref("pBufferForArgs", pBufferForArgs, "ID3D11Buffer").u("AlignedByteOffsetForArgs", AlignedByteOffsetForArgs);
    Record(c, "DispatchIndirect", a.str(), std::move(state));
}

// ---------------------------------------------------------------------------------------------
// Output merger, rasterizer, stream output

void SetRenderTargets(Context* c, UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView) {
    PipelineState& s = c->state;
    const UINT n = std::min<UINT>(NumViews, (UINT)s.renderTargets.size());
    for (UINT i = 0; i < s.renderTargets.size(); ++i) s.renderTargets[i] = i < n && ppRenderTargetViews ? ppRenderTargetViews[i] : nullptr;
    s.renderTargetCount = ppRenderTargetViews ? n : 0;
    // Trailing nulls do not count as targets.
    while (s.renderTargetCount && !s.renderTargets[s.renderTargetCount - 1]) --s.renderTargetCount;
    s.depthStencil = pDepthStencilView;
}

void STDMETHODCALLTYPE RecordingContext::OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView) {
    Context* c = ctx_;
    if (c) {
        // The pass on the previous targets ends before this call is recorded, so the call is the
        // first thing after it (the GLES plugin's order too).
        SetRenderTargets(c, NumViews, ppRenderTargetViews, pDepthStencilView);
        AfterSetRenderTargets(c);
        if (Recording()) {
            Args a;
            a.u("NumViews", NumViews).refs("ppRenderTargetViews", ppRenderTargetViews, ppRenderTargetViews ? NumViews : 0).ref("pDepthStencilView", pDepthStencilView, "ID3D11DepthStencilView");
            Record(c, "OMSetRenderTargets", a.str());
        }
    }
    real_->OMSetRenderTargets(NumViews, ppRenderTargetViews, pDepthStencilView);
}

void STDMETHODCALLTYPE RecordingContext::OMSetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) {
    Context* c = ctx_;
    if (c) {
        if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) SetRenderTargets(c, NumRTVs, ppRenderTargetViews, pDepthStencilView);
        if (NumUAVs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS) {
            for (UINT i = 0; i < NumUAVs && UAVStartSlot + i < c->state.psUavs.size(); ++i) c->state.psUavs[UAVStartSlot + i] = ppUnorderedAccessViews ? ppUnorderedAccessViews[i] : nullptr;
        }
        AfterSetRenderTargets(c);
    }
    if (c && Recording()) {
        Args a;
        const bool keepTargets = NumRTVs == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL;
        const bool keepUavs = NumUAVs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS;
        if (keepTargets) a.s("NumRTVs", "D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL"); else a.u("NumRTVs", NumRTVs);
        a.refs("ppRenderTargetViews", ppRenderTargetViews, ppRenderTargetViews && !keepTargets ? NumRTVs : 0).ref("pDepthStencilView", pDepthStencilView, "ID3D11DepthStencilView");
        a.u("UAVStartSlot", UAVStartSlot);
        if (keepUavs) a.s("NumUAVs", "D3D11_KEEP_UNORDERED_ACCESS_VIEWS"); else a.u("NumUAVs", NumUAVs);
        a.refs("ppUnorderedAccessViews", ppUnorderedAccessViews, ppUnorderedAccessViews && !keepUavs ? NumUAVs : 0).uints("pUAVInitialCounts", pUAVInitialCounts, keepUavs ? 0 : NumUAVs);
        Record(c, "OMSetRenderTargetsAndUnorderedAccessViews", a.str());
    }
    real_->OMSetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, pDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
}

void STDMETHODCALLTYPE RecordingContext::OMSetBlendState(ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) {
    real_->OMSetBlendState(pBlendState, BlendFactor, SampleMask);
    Context* c = ctx_;
    if (!c) return;
    c->state.blendState = pBlendState;
    for (int i = 0; i < 4; ++i) c->state.blendFactor[i] = BlendFactor ? BlendFactor[i] : 1.0f;
    c->state.sampleMask = SampleMask;
    if (Recording()) {
        Args a;
        a.ref("pBlendState", pBlendState, "ID3D11BlendState").floats("BlendFactor", BlendFactor, 4).u("SampleMask", SampleMask);
        Record(c, "OMSetBlendState", a.str());
    }
}

void STDMETHODCALLTYPE RecordingContext::OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef) {
    real_->OMSetDepthStencilState(pDepthStencilState, StencilRef);
    Context* c = ctx_;
    if (!c) return;
    c->state.depthStencilState = pDepthStencilState;
    c->state.stencilRef = StencilRef;
    if (Recording()) {
        Args a;
        a.ref("pDepthStencilState", pDepthStencilState, "ID3D11DepthStencilState").u("StencilRef", StencilRef);
        Record(c, "OMSetDepthStencilState", a.str());
    }
}

void STDMETHODCALLTYPE RecordingContext::RSSetState(ID3D11RasterizerState* pRasterizerState) {
    real_->RSSetState(pRasterizerState);
    Context* c = ctx_;
    if (!c) return;
    c->state.rasterizerState = pRasterizerState;
    if (Recording()) {
        Args a;
        a.ref("pRasterizerState", pRasterizerState, "ID3D11RasterizerState");
        Record(c, "RSSetState", a.str());
    }
}

void STDMETHODCALLTYPE RecordingContext::RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) {
    real_->RSSetViewports(NumViewports, pViewports);
    Context* c = ctx_;
    if (!c) return;
    const UINT n = std::min<UINT>(NumViewports, (UINT)c->state.viewports.size());
    for (UINT i = 0; i < n && pViewports; ++i) c->state.viewports[i] = pViewports[i];
    c->state.viewportCount = pViewports ? n : 0;
    if (Recording()) {
        Args a;
        a.u("NumViewports", NumViewports);
        WriteViewports(a.key("pViewports"), pViewports, NumViewports);
        Record(c, "RSSetViewports", a.str());
    }
}

void STDMETHODCALLTYPE RecordingContext::RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) {
    real_->RSSetScissorRects(NumRects, pRects);
    Context* c = ctx_;
    if (!c) return;
    const UINT n = std::min<UINT>(NumRects, (UINT)c->state.scissors.size());
    for (UINT i = 0; i < n && pRects; ++i) c->state.scissors[i] = pRects[i];
    c->state.scissorCount = pRects ? n : 0;
    if (Recording()) {
        Args a;
        a.u("NumRects", NumRects);
        WriteRects(a.key("pRects"), pRects, NumRects);
        Record(c, "RSSetScissorRects", a.str());
    }
}

void STDMETHODCALLTYPE RecordingContext::SOSetTargets(UINT NumBuffers, ID3D11Buffer* const* ppSOTargets, const UINT* pOffsets) {
    real_->SOSetTargets(NumBuffers, ppSOTargets, pOffsets);
    Context* c = ctx_;
    if (!c) return;
    for (UINT i = 0; i < c->state.streamOutput.size(); ++i) c->state.streamOutput[i] = i < NumBuffers && ppSOTargets ? ppSOTargets[i] : nullptr;
    if (Recording()) {
        Args a;
        a.u("NumBuffers", NumBuffers).refs("ppSOTargets", ppSOTargets, ppSOTargets ? NumBuffers : 0).uints("pOffsets", pOffsets, NumBuffers);
        Record(c, "SOSetTargets", a.str());
    }
}

void STDMETHODCALLTYPE RecordingContext::SetPredication(ID3D11Predicate* pPredicate, BOOL PredicateValue) {
    real_->SetPredication(pPredicate, PredicateValue);
    Context* c = ctx_;
    if (!c) return;
    c->state.predicate = pPredicate;
    c->state.predicateValue = PredicateValue;
    if (Recording()) {
        Args a;
        a.ref("pPredicate", pPredicate, "ID3D11Predicate").b("PredicateValue", PredicateValue != 0);
        Record(c, "SetPredication", a.str());
    }
}

// ---------------------------------------------------------------------------------------------
// Clears and discards

void STDMETHODCALLTYPE RecordingContext::ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]) {
    Context* c = ctx_;
    if (c) BeforeClear(c, pRenderTargetView, false, false);
    real_->ClearRenderTargetView(pRenderTargetView, ColorRGBA);
    if (!c || !Recording()) return;
    Args a;
    a.ref("pRenderTargetView", pRenderTargetView, "ID3D11RenderTargetView").floats("ColorRGBA", ColorRGBA, 4);
    Record(c, "ClearRenderTargetView", a.str());
}

void STDMETHODCALLTYPE RecordingContext::ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) {
    Context* c = ctx_;
    if (c) BeforeClear(c, pDepthStencilView, (ClearFlags & D3D11_CLEAR_DEPTH) != 0, (ClearFlags & D3D11_CLEAR_STENCIL) != 0);
    real_->ClearDepthStencilView(pDepthStencilView, ClearFlags, Depth, Stencil);
    if (!c || !Recording()) return;
    Args a;
    a.ref("pDepthStencilView", pDepthStencilView, "ID3D11DepthStencilView");
    Flags_D3D11_CLEAR_FLAG(a.key("ClearFlags"), ClearFlags);
    a.d("Depth", Depth).u("Stencil", Stencil);
    Record(c, "ClearDepthStencilView", a.str());
}

void STDMETHODCALLTYPE RecordingContext::ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* pUnorderedAccessView, const UINT Values[4]) {
    Context* c = ctx_;
    if (c) BeforeClear(c, pUnorderedAccessView, false, false);
    real_->ClearUnorderedAccessViewUint(pUnorderedAccessView, Values);
    if (!c || !Recording()) return;
    Args a;
    a.ref("pUnorderedAccessView", pUnorderedAccessView, "ID3D11UnorderedAccessView").uints("Values", Values, 4);
    Record(c, "ClearUnorderedAccessViewUint", a.str());
}

void STDMETHODCALLTYPE RecordingContext::ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* pUnorderedAccessView, const FLOAT Values[4]) {
    Context* c = ctx_;
    if (c) BeforeClear(c, pUnorderedAccessView, false, false);
    real_->ClearUnorderedAccessViewFloat(pUnorderedAccessView, Values);
    if (!c || !Recording()) return;
    Args a;
    a.ref("pUnorderedAccessView", pUnorderedAccessView, "ID3D11UnorderedAccessView").floats("Values", Values, 4);
    Record(c, "ClearUnorderedAccessViewFloat", a.str());
}

void STDMETHODCALLTYPE RecordingContext::ClearView(ID3D11View* pView, const FLOAT Color[4], const D3D11_RECT* pRect, UINT NumRects) {
    Context* c = ctx_;
    // A clear of part of the view keeps the rest: only a whole clear is what a pass starts from.
    if (c && NumRects == 0) BeforeClear(c, pView, true, false);
    else if (c && pView) if (Object* v = Find(pView)) if (Object* r = FindById(v->resourceId)) ++r->generation;
    real_->ClearView(pView, Color, pRect, NumRects);
    if (!c || !Recording()) return;
    Args a;
    a.ref("pView", pView).floats("Color", Color, 4);
    WriteRects(a.key("pRect"), pRect, NumRects);
    a.u("NumRects", NumRects);
    Record(c, "ClearView", a.str());
}

void STDMETHODCALLTYPE RecordingContext::DiscardResource(ID3D11Resource* pResource) {
    Context* c = ctx_;
    if (c) BeforeDiscard(c, nullptr, pResource);
    real_->DiscardResource(pResource);
    if (!c || !Recording()) return;
    Args a;
    a.ref("pResource", pResource);
    Record(c, "DiscardResource", a.str());
}

void STDMETHODCALLTYPE RecordingContext::DiscardView(ID3D11View* pResourceView) {
    Context* c = ctx_;
    if (c) BeforeDiscard(c, pResourceView, nullptr);
    real_->DiscardView(pResourceView);
    if (!c || !Recording()) return;
    Args a;
    a.ref("pResourceView", pResourceView);
    Record(c, "DiscardView", a.str());
}

void STDMETHODCALLTYPE RecordingContext::DiscardView1(ID3D11View* pResourceView, const D3D11_RECT* pRects, UINT NumRects) {
    Context* c = ctx_;
    if (c && NumRects == 0) BeforeDiscard(c, pResourceView, nullptr);
    real_->DiscardView1(pResourceView, pRects, NumRects);
    if (!c || !Recording()) return;
    Args a;
    a.ref("pResourceView", pResourceView);
    WriteRects(a.key("pRects"), pRects, NumRects);
    a.u("NumRects", NumRects);
    Record(c, "DiscardView1", a.str());
}

// ---------------------------------------------------------------------------------------------
// Resources: maps, updates, copies

HRESULT STDMETHODCALLTYPE RecordingContext::Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) {
    HRESULT hr = real_->Map(pResource, Subresource, MapType, MapFlags, pMappedResource);
    Context* c = ctx_;
    if (!c) return hr;
    if (SUCCEEDED(hr) && MapType != D3D11_MAP_READ) OnResourceWritten(pResource);
    if (Recording()) {
        Args a;
        a.ref("pResource", pResource).u("Subresource", Subresource).e("MapType", ToString_D3D11_MAP(MapType), MapType);
        Flags_D3D11_MAP_FLAG(a.key("MapFlags"), MapFlags);
        if (pMappedResource && SUCCEEDED(hr)) Write(a.key("pMappedResource"), *pMappedResource); else a.null("pMappedResource");
        a.i("result", hr);
        Record(c, "Map", a.str());
    }
    return hr;
}

void STDMETHODCALLTYPE RecordingContext::Unmap(ID3D11Resource* pResource, UINT Subresource) {
    real_->Unmap(pResource, Subresource);
    Context* c = ctx_;
    if (!c || !Recording()) return;
    Args a;
    a.ref("pResource", pResource).u("Subresource", Subresource);
    Record(c, "Unmap", a.str());
}

/** How many bytes an update writes, when it can be told (a buffer's box or whole size). */
uint64_t UpdateBytes(ID3D11Resource* dst, const D3D11_BOX* box, UINT rowPitch, UINT depthPitch) {
    Object* o = Find(dst);
    if (!o) return 0;
    if (o->kind == ObjKind::Buffer) return box ? (box->right > box->left ? box->right - box->left : 0) : o->size;
    const UINT rows = box ? (box->bottom > box->top ? box->bottom - box->top : 0) : o->height;
    const UINT depth = box ? (box->back > box->front ? box->back - box->front : 1) : (o->kind == ObjKind::Texture3D ? o->depth : 1);
    if (depth > 1 && depthPitch) return (uint64_t)depthPitch * depth;
    return (uint64_t)rowPitch * rows;
}

void STDMETHODCALLTYPE RecordingContext::UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) {
    real_->UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
    Context* c = ctx_;
    if (!c) return;
    OnResourceWritten(pDstResource);
    if (!Recording()) return;
    Args a;
    a.ref("pDstResource", pDstResource).u("DstSubresource", DstSubresource);
    Write(a.key("pDstBox"), pDstBox);
    a.ptr("pSrcData", pSrcData).u("bytes", UpdateBytes(pDstResource, pDstBox, SrcRowPitch, SrcDepthPitch)).u("SrcRowPitch", SrcRowPitch).u("SrcDepthPitch", SrcDepthPitch);
    Record(c, "UpdateSubresource", a.str());
}

void STDMETHODCALLTYPE RecordingContext::UpdateSubresource1(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch, UINT CopyFlags) {
    real_->UpdateSubresource1(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch, CopyFlags);
    Context* c = ctx_;
    if (!c) return;
    OnResourceWritten(pDstResource);
    if (!Recording()) return;
    Args a;
    a.ref("pDstResource", pDstResource).u("DstSubresource", DstSubresource);
    Write(a.key("pDstBox"), pDstBox);
    a.ptr("pSrcData", pSrcData).u("bytes", UpdateBytes(pDstResource, pDstBox, SrcRowPitch, SrcDepthPitch)).u("SrcRowPitch", SrcRowPitch).u("SrcDepthPitch", SrcDepthPitch);
    Flags_D3D11_COPY_FLAGS(a.key("CopyFlags"), CopyFlags);
    Record(c, "UpdateSubresource1", a.str());
}

void STDMETHODCALLTYPE RecordingContext::CopyResource(ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource) {
    real_->CopyResource(pDstResource, pSrcResource);
    Context* c = ctx_;
    if (!c) return;
    OnResourceWritten(pDstResource);
    if (!Recording()) return;
    Args a;
    a.ref("pDstResource", pDstResource).ref("pSrcResource", pSrcResource);
    Record(c, "CopyResource", a.str());
}

void STDMETHODCALLTYPE RecordingContext::CopySubresourceRegion(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox) {
    real_->CopySubresourceRegion(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
    Context* c = ctx_;
    if (!c) return;
    OnResourceWritten(pDstResource);
    if (!Recording()) return;
    Args a;
    a.ref("pDstResource", pDstResource).u("DstSubresource", DstSubresource).u("DstX", DstX).u("DstY", DstY).u("DstZ", DstZ)
        .ref("pSrcResource", pSrcResource).u("SrcSubresource", SrcSubresource);
    Write(a.key("pSrcBox"), pSrcBox);
    Record(c, "CopySubresourceRegion", a.str());
}

void STDMETHODCALLTYPE RecordingContext::CopySubresourceRegion1(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource, const D3D11_BOX* pSrcBox, UINT CopyFlags) {
    real_->CopySubresourceRegion1(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox, CopyFlags);
    Context* c = ctx_;
    if (!c) return;
    OnResourceWritten(pDstResource);
    if (!Recording()) return;
    Args a;
    a.ref("pDstResource", pDstResource).u("DstSubresource", DstSubresource).u("DstX", DstX).u("DstY", DstY).u("DstZ", DstZ)
        .ref("pSrcResource", pSrcResource).u("SrcSubresource", SrcSubresource);
    Write(a.key("pSrcBox"), pSrcBox);
    Flags_D3D11_COPY_FLAGS(a.key("CopyFlags"), CopyFlags);
    Record(c, "CopySubresourceRegion1", a.str());
}

void STDMETHODCALLTYPE RecordingContext::CopyStructureCount(ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset, ID3D11UnorderedAccessView* pSrcView) {
    real_->CopyStructureCount(pDstBuffer, DstAlignedByteOffset, pSrcView);
    Context* c = ctx_;
    if (!c) return;
    OnResourceWritten(pDstBuffer);
    if (!Recording()) return;
    Args a;
    a.ref("pDstBuffer", pDstBuffer, "ID3D11Buffer").u("DstAlignedByteOffset", DstAlignedByteOffset).ref("pSrcView", pSrcView, "ID3D11UnorderedAccessView");
    Record(c, "CopyStructureCount", a.str());
}

void STDMETHODCALLTYPE RecordingContext::ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) {
    real_->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
    Context* c = ctx_;
    if (!c) return;
    OnResourceWritten(pDstResource);
    if (!Recording()) return;
    Args a;
    a.ref("pDstResource", pDstResource).u("DstSubresource", DstSubresource).ref("pSrcResource", pSrcResource).u("SrcSubresource", SrcSubresource)
        .e("Format", ToString_DXGI_FORMAT(Format), Format);
    Record(c, "ResolveSubresource", a.str());
}

void STDMETHODCALLTYPE RecordingContext::GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) {
    real_->GenerateMips(pShaderResourceView);
    Context* c = ctx_;
    if (!c) return;
    if (Object* v = Find(pShaderResourceView)) if (Object* r = FindById(v->resourceId)) ++r->generation;
    if (!Recording()) return;
    Args a;
    a.ref("pShaderResourceView", pShaderResourceView, "ID3D11ShaderResourceView");
    Record(c, "GenerateMips", a.str());
}

// ---------------------------------------------------------------------------------------------
// Queries, events, command lists, the rest

void STDMETHODCALLTYPE RecordingContext::Begin(ID3D11Asynchronous* pAsync) {
    real_->Begin(pAsync);
    Context* c = ctx_;
    if (!c || !Recording()) return;
    Args a;
    a.ref("pAsync", pAsync);
    Record(c, "Begin", a.str());
}

void STDMETHODCALLTYPE RecordingContext::End(ID3D11Asynchronous* pAsync) {
    real_->End(pAsync);
    Context* c = ctx_;
    if (!c || !Recording()) return;
    Args a;
    a.ref("pAsync", pAsync);
    Record(c, "End", a.str());
}

void STDMETHODCALLTYPE RecordingContext::ClearState() {
    Context* c = ctx_;
    if (c) EndOpenPass(c);
    real_->ClearState();
    if (!c) return;
    ClearShadow(c);
    if (Recording()) Record(c, "ClearState", "{}");
}

void STDMETHODCALLTYPE RecordingContext::Flush() {
    real_->Flush();
    Context* c = ctx_;
    if (c && Recording()) Record(c, "Flush", "{}");
}

void STDMETHODCALLTYPE RecordingContext::Flush1(D3D11_CONTEXT_TYPE ContextType, HANDLE hEvent) {
    real_->Flush1(ContextType, hEvent);
    Context* c = ctx_;
    if (c && Recording()) {
        Args a;
        a.e("ContextType", ToString_D3D11_CONTEXT_TYPE(ContextType), ContextType).ptr("hEvent", hEvent);
        Record(c, "Flush1", a.str());
    }
}

void STDMETHODCALLTYPE RecordingContext::ExecuteCommandList(ID3D11CommandList* pCommandList, BOOL RestoreContextState) {
    Context* c = ctx_;
    if (c) OnExecuteCommandList(c, pCommandList, RestoreContextState);
    real_->ExecuteCommandList(pCommandList, RestoreContextState);
    if (c && !RestoreContextState) ClearShadow(c);
}

HRESULT STDMETHODCALLTYPE RecordingContext::FinishCommandList(BOOL RestoreDeferredContextState, ID3D11CommandList** ppCommandList) {
    Context* c = ctx_;
    if (c && Recording()) {
        Args a;
        a.b("RestoreDeferredContextState", RestoreDeferredContextState != 0);
        Record(c, "FinishCommandList", a.str());
    }
    HRESULT hr = real_->FinishCommandList(RestoreDeferredContextState, ppCommandList);
    if (!c) return hr;
    ID3D11CommandList* list = SUCCEEDED(hr) && ppCommandList ? *ppCommandList : nullptr;
    if (list) {
        if (!VtableHooked(list)) HookDeviceChild(list, "ID3D11CommandList", kDeviceChildSlots, {});
        Object& o = Track(list, ObjKind::CommandList, "ID3D11CommandList", c->id, "FinishCommandList");
        if (o.args.empty()) {
            Describe(o, "context", JsonRef(c->id, "ID3D11DeviceContext"));
            Announce(o);
        }
    }
    OnFinishCommandList(c, list);
    if (!RestoreDeferredContextState) ClearShadow(c);
    return hr;
}

void STDMETHODCALLTYPE RecordingContext::SwapDeviceContextState(ID3DDeviceContextState* pState, ID3DDeviceContextState** ppPreviousState) {
    Context* c = ctx_;
    if (c) EndOpenPass(c);
    real_->SwapDeviceContextState(pState, ppPreviousState);
    if (!c) return;
    // The whole pipeline state is swapped for one the library never followed: nothing is known to be bound.
    ClearShadow(c);
    if (Recording()) {
        Args a;
        a.ptr("pState", pState);
        Record(c, "SwapDeviceContextState", a.str());
    }
}

void STDMETHODCALLTYPE RecordingContext::BeginEventInt(LPCWSTR pLabel, INT Data) {
    real_->BeginEventInt(pLabel, Data);
    Context* c = ctx_;
    if (!c) return;
    if (Recording()) {
        Args a;
        a.ws("Name", pLabel).i("Data", Data);
        Record(c, "BeginEvent", a.str());
    }
    AfterBeginEvent(c);
}

void STDMETHODCALLTYPE RecordingContext::EndEvent() {
    Context* c = ctx_;
    if (c) BeforeEndEvent(c);
    real_->EndEvent();
    if (c && Recording()) Record(c, "EndEvent", "{}");
}

void STDMETHODCALLTYPE RecordingContext::SetMarkerInt(LPCWSTR pLabel, INT Data) {
    real_->SetMarkerInt(pLabel, Data);
    Context* c = ctx_;
    if (c && Recording()) {
        Args a;
        a.ws("Name", pLabel).i("Data", Data);
        Record(c, "SetMarker", a.str());
    }
}

// ID3DUserDefinedAnnotation, which is how PIX-style events reach a D3D11 context: a separate
// interface of the context's, hooked on its own vtable. QueryInterface 0, AddRef 1, Release 2,
// BeginEvent 3, EndEvent 4, SetMarker 5, GetStatus 6.
enum AnnotationSlot : uint32_t { kAnnotationBeginEvent = 3, kAnnotationEndEvent = 4, kAnnotationSetMarker = 5, kAnnotationCount = 7 };
typedef INT(STDMETHODCALLTYPE* PFN_Annotation_BeginEvent)(ID3DUserDefinedAnnotation*, LPCWSTR);
typedef INT(STDMETHODCALLTYPE* PFN_Annotation_EndEvent)(ID3DUserDefinedAnnotation*);
typedef void(STDMETHODCALLTYPE* PFN_Annotation_SetMarker)(ID3DUserDefinedAnnotation*, LPCWSTR);

Context* AnnotationContext(ID3DUserDefinedAnnotation* This) {
    if (Internal()) return nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    {
        ScopedInternal internal;
        if (FAILED(This->QueryInterface(IID_PPV_ARGS(&ctx))) || !ctx) return nullptr;
        ctx->Release();
    }
    return FindContext(ctx);
}

INT STDMETHODCALLTYPE Hook_AnnotationBeginEvent(ID3DUserDefinedAnnotation* This, LPCWSTR Name) {
    INT r = Orig<PFN_Annotation_BeginEvent>(This, kAnnotationBeginEvent)(This, Name);
    Context* c = AnnotationContext(This);
    if (!c) return r;
    if (Recording()) {
        Args a;
        a.ws("Name", Name);
        Record(c, "BeginEvent", a.str());
    }
    AfterBeginEvent(c);
    return r;
}

INT STDMETHODCALLTYPE Hook_AnnotationEndEvent(ID3DUserDefinedAnnotation* This) {
    Context* c = AnnotationContext(This);
    if (c) BeforeEndEvent(c);
    INT r = Orig<PFN_Annotation_EndEvent>(This, kAnnotationEndEvent)(This);
    if (c && Recording()) Record(c, "EndEvent", "{}");
    return r;
}

void STDMETHODCALLTYPE Hook_AnnotationSetMarker(ID3DUserDefinedAnnotation* This, LPCWSTR Name) {
    Orig<PFN_Annotation_SetMarker>(This, kAnnotationSetMarker)(This, Name);
    Context* c = AnnotationContext(This);
    if (c && Recording()) {
        Args a;
        a.ws("Name", Name);
        Record(c, "SetMarker", a.str());
    }
}

void HookAnnotation(ID3D11DeviceContext* context) {
    ID3DUserDefinedAnnotation* annotation = nullptr;
    ScopedInternal internal;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&annotation))) || !annotation) return;
    if (!VtableHooked(annotation)) {
        HookVtable(annotation, "ID3DUserDefinedAnnotation", kAnnotationCount, {
            {kAnnotationBeginEvent, (void*)&Hook_AnnotationBeginEvent},
            {kAnnotationEndEvent, (void*)&Hook_AnnotationEndEvent},
            {kAnnotationSetMarker, (void*)&Hook_AnnotationSetMarker},
        });
    }
    annotation->Release();
}

}  // namespace

void DestroyContext(ID3D11DeviceContext* real) {
    ID3D11DeviceContext* proxy = nullptr;
    {
        LibraryState& s = State();
        std::lock_guard lock(s.mutex);
        auto it = s.contexts.find(real);
        if (it == s.contexts.end()) return;
        proxy = it->second->proxy;
        s.contexts.erase(it);
    }
    OnObjectReleased(real);
    delete static_cast<RecordingContext*>(static_cast<ID3D11DeviceContext4*>(proxy));
}

void DestroyContextsOf(ID3D11Device* device) {
    std::vector<ID3D11DeviceContext*> gone;
    {
        LibraryState& s = State();
        std::lock_guard lock(s.mutex);
        for (auto& [ptr, c] : s.contexts)
            if (c->device == device) gone.push_back(c->ptr);
    }
    for (ID3D11DeviceContext* real : gone) DestroyContext(real);
}

ID3D11DeviceContext* WrapContext(ID3D11DeviceContext* context, ID3D11Device* device, const char* cmd) {
    if (!context) return nullptr;
    // A proxy handed back to us (a context the application got from us and gave to a call that
    // returns it again) is already what the application should see.
    if (Context* mine = ContextOfProxy(context)) return mine->proxy;
    // One the application already has a proxy for (GetImmediateContext again): the same proxy.
    if (Context* known = FindContext(context); known && known->proxy) return known->proxy;
    HookAnnotation(context);
    Object& o = Track(context, ObjKind::Context, "ID3D11DeviceContext", IdOf(device), cmd);
    Context& c = ContextOf(context);
    if (o.args.empty()) {
        Describe(o, "type", JsonString(c.deferred ? "deferred" : "immediate"));
        Announce(o);
        Log("%s -> ID3D11DeviceContext %p (%s)", cmd, (void*)context, c.deferred ? "deferred" : "immediate");
    }
    if (!c.proxy) {
        ID3D11DeviceContext4* real = nullptr;
        {
            ScopedInternal internal;
            // The newest version the object has; the proxy calls only what the application asked
            // QueryInterface for, so a runtime short of ID3D11DeviceContext4 is fine.
            if (FAILED(context->QueryInterface(IID_PPV_ARGS(&real))) || !real) real = (ID3D11DeviceContext4*)context;
            else real->Release();
        }
        c.proxy = new RecordingContext(real, &c);
    }
    return c.proxy;
}

}  // namespace d3d11insp
