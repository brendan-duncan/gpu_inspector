// Installing the Metal hooks. The mechanism is in swizzle.h; this is what gets hooked.
//
// Each installer is called with the first object of its kind that appears and hooks that object's
// class, so the tree is discovered from the interposed entry points downwards without naming a
// private class. They are cheap to call repeatedly — each stops at the first sighting of a class.
#pragma once

#import <objc/runtime.h>

namespace mtlinsp {

/**
 * Registers the device with the tracker and hooks its class. Called from the interposed entry
 * points, and from the CAMetalLayer hook for a device obtained some other way — a layer's
 * preferredDevice, MetalKit, CoreGraphics — since that is the one place every device on screen
 * passes through.
 */
void TrackDeviceObject(id device, const char *origin);

/** Hooks CAMetalLayer, by name: it is public, and a drawable's texture comes from nowhere else. */
void HookDrawableSource(void);

/** Hooks a CAMetalDrawable's class: its own `present` is also a frame boundary. */
void HookDrawableClass(id drawable);

/** Registers the commit-boundary frame logger with the capture side. Called once, on load. */
void InstallFrameLogging(void);

void HookDeviceClass(id device);
void HookHeapClass(id heap);
void HookLibraryClass(id library);
void HookTextureClass(id texture);
void HookBufferClass(id buffer);
void HookCommandQueueClass(id queue);
void HookCommandBufferClass(id commandBuffer);
void HookRenderEncoderClass(id encoder);
void HookParallelEncoderClass(id encoder);
void HookComputeEncoderClass(id encoder);
void HookBlitEncoderClass(id encoder);
/** Resource state and acceleration structure encoders: passes with no recorded commands yet. */
void HookOtherEncoderClass(id encoder);

}  // namespace mtlinsp
