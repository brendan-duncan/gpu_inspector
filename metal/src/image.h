// Reading a live texture's contents, for the Inspect panel's image view.
//
// The counterpart of the Vulkan layer's RequestImage handling. Where a capture reads a render
// target back as a side effect of the frame it is recording, this is on demand and out of band:
// the UI asks for one texture, at one mip and one array layer, and the answer is an ImageData
// message with the pixels.
//
// It costs a command buffer and a wait on the GPU, on whatever thread the UI message arrived on.
// That is acceptable for a click in the object list and would not be for anything per-frame.
#pragma once

#include <cstdint>

#import <objc/runtime.h>

namespace mtlinsp {

/** Answers the UI's `RequestImage` with an `ImageData` message, pixels or error. */
void SendImageData(uint64_t objectId, uint32_t mip, uint32_t layer);

}  // namespace mtlinsp
