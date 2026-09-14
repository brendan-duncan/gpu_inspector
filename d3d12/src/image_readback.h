// Live image read-back: the Inspect panel's RequestImage {id, mip, layer} answered with ImageData
// and the pixels of one subresource of an ID3D12Resource, while the application runs.
//
// A command list of the library's own, executed on the device's first direct queue after
// whatever the application has queued, copies the subresource into a readback buffer (with the
// barriers the resource's tracked state needs, COPY_SOURCE and back); a fence is waited for on the
// transport's receiver thread, and the bytes go out under the protocol's format name. A depth
// texture is read as its depth aspect. Multisampled textures go through a resolve of the
// capture's (color only).
#pragma once

#include "common.h"

namespace dxinsp {

/** Serves one request; sends ImageData (with an error when the image cannot be read). */
void ReadBackImage(uint64_t objectId, uint32_t mip, uint32_t layer);

/** A direct queue of the device the library can execute on (the application's first, or one of its own). */
ID3D12CommandQueue* ReadbackQueue(ID3D12Device* device);
/** Notes a queue the application created, so read-backs can use one of its direct queues. */
void OnQueueCreated(ID3D12Device* device, ID3D12CommandQueue* queue, D3D12_COMMAND_LIST_TYPE type);
void OnQueueReleased(ID3D12CommandQueue* queue);

}  // namespace dxinsp
