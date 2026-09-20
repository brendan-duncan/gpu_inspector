// Support for the exported frame: the device, uploads from the data file, descriptor handles by
// heap and slot, and the render targets read back and compared with what the capture holds. Nothing
// in here is specific to the frame; the frame_*.cpp files are.
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

extern ID3D12Device* device;

/** Prints what failed and exits with code 2. */
[[noreturn]] void Fail(const char* what, HRESULT result);
[[noreturn]] void Fail(const std::string& message);
#define DX_CHECK(call) do { const HRESULT dx_result_ = (call); if (FAILED(dx_result_)) Fail(#call, dx_result_); } while (0)

// The data file: shader bytecode, texture and buffer contents, and the captured targets, by offset.
bool LoadData(const std::string& path);
const void* Data(uint64_t offset, uint64_t size);

/** The adapter with this name, else the first hardware one; the debug layer when asked for. */
void CreateDeviceOn(const char* adapterName, D3D_FEATURE_LEVEL level, bool debugLayer);
const char* AdapterName();
ID3D12RootSignature* CreateRootSignatureFrom(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc);
/** A command list with an allocator of its own, closed: every recording begins with a Reset. */
ID3D12GraphicsCommandList* CreateClosedCommandList(D3D12_COMMAND_LIST_TYPE type);

/** A later interface of a command list; the program stops where this runtime does not have it. */
template <typename T>
T* As(ID3D12GraphicsCommandList* list) {
    T* out = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&out))) || !out) Fail("this runtime lacks a command list interface the frame uses");
    out->Release();   // the list itself keeps the object alive
    return out;
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(ID3D12DescriptorHeap* heap, UINT index);
D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(ID3D12DescriptorHeap* heap, UINT index);

// One-time command lists, executed on a queue of the support's own and waited for.
ID3D12GraphicsCommandList* BeginOneTime();
void EndOneTime(ID3D12GraphicsCommandList* list);
void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

/** Tight rows of one subresource in an upload's data: bytes per row, rows, and slices of a volume. */
struct TextureRegion {
    UINT subresource;
    UINT64 rowBytes;
    UINT rows;
    UINT slices;
};
/** Copies the regions, one after another in `data`, into a texture that is in COMMON; they are left in COPY_DEST. */
void UploadTexture(ID3D12Resource* texture, const TextureRegion* regions, UINT count, const void* data, UINT64 size);
/** Writes a buffer's range: mapped for an upload heap, else copied through a staging buffer from and back to `state`. */
void UploadBuffer(ID3D12Resource* buffer, UINT64 offset, const void* data, UINT64 size, D3D12_RESOURCE_STATES state, bool uploadHeap);

/**
 * Copies `count` subresources of a render target at this point of the list, from the state it is in,
 * to compare with `captured` once the list has run. A multisampled target (`resolveFormat` set) is
 * resolved first. `aspect`: 0 colour, 1 depth, 2 stencil.
 */
void ReadbackTexture(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, const char* name, UINT firstSubresource, UINT count,
                     D3D12_RESOURCE_STATES state, DXGI_FORMAT resolveFormat, DXGI_FORMAT format, int aspect, UINT width, UINT height,
                     UINT64 rowBytes, UINT rows, const void* captured, UINT64 capturedSize);
void ExecuteAndWait(ID3D12CommandQueue* queue, ID3D12CommandList* const* lists, UINT count);
/** Compares the submission's read-backs with the capture's copies. */
void CompleteReadbacks();
/** Prints every comparison and writes the images to `directory`; the process exit code: 0 all identical, 1 otherwise. */
int ReportResults(const std::string& directory, bool writeImages);

// The window (the default; --batch compares the targets instead). The frame runs again and again,
// and what it leaves on screen is copied to a swap chain of the support's own and presented.
/**
 * Opens a window of the output's size with a swap chain to show it in, and from then on the frame's
 * read-backs are not taken (ReadbackTexture returns at once): they are --batch's. False, with the
 * reason printed, when there is nothing to show it on or the output is not something a swap chain
 * can hold (a format no display takes, a multisampled texture).
 */
bool OpenOutputWindow(ID3D12Resource* output, const char* title);
/** Whether a present waits for the display (the default); without, the frame runs as fast as it can, which is what to time. */
void SetOutputVsync(bool on);
/** Copies the output, which is in `state`, to the back buffer and presents; false once the window was closed. */
bool PresentOutput(ID3D12Resource* output, D3D12_RESOURCE_STATES state);
/** Closes the window and prints how many frames it showed; the process exit code. */
int CloseOutputWindow();
void DestroySupport();
