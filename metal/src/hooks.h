// Installing the Metal hooks. The mechanism is in swizzle.h; this is what gets hooked.
//
// Each installer is called with the first object of its kind that appears and hooks that object's
// class, so the tree is discovered from the one interposed entry point downwards without naming a
// private class. They are cheap to call repeatedly — each stops at the first sighting of a class.
#pragma once

#import <objc/runtime.h>

namespace mtlinsp {

/** Registers the device with the tracker and hooks its class. Called from the interposed entry points. */
void TrackDeviceObject(id device);

/** Hooks CAMetalLayer, by name: it is public, and a drawable's texture comes from nowhere else. */
void HookDrawableSource(void);

void HookDeviceClass(id device);
void HookCommandQueueClass(id queue);
void HookCommandBufferClass(id commandBuffer);
void HookRenderEncoderClass(id encoder);
void HookComputeEncoderClass(id encoder);
void HookBlitEncoderClass(id encoder);

}  // namespace mtlinsp
