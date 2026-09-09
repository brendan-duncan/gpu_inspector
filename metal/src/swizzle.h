// Objective-C method interception for Metal's driver-private classes.
//
// Metal's API is protocols: id<MTLDevice>, id<MTLCommandBuffer>, id<MTLRenderCommandEncoder>.
// The objects behind them are concrete classes the driver owns, they are not exported by
// Metal.framework (only the descriptor classes are), and they differ per GPU vendor —
// AGXG*Family* on Apple Silicon, MTLIGAccel*/MTLIOAccel* elsewhere. There is no list to hook
// against ahead of time.
//
// So the classes are discovered at run time instead: the interposed MTLCreateSystemDefaultDevice
// takes object_getClass() of the device it returns and hooks that, and each hook that returns
// another Metal object hooks the class of what it returns, once, the first time it is seen. That
// walks the whole tree without naming a single private class.
//
// The objects themselves are never wrapped. This is the same decision the Vulkan layer makes for
// handles (docs/ARCHITECTURE.md, "Handles: pass-through"): the application keeps the real object,
// state lives in side tables keyed by the pointer, and nothing has to be unwrapped on the way
// back out. It matters more here than in Vulkan, because CoreAnimation and MetalKit inspect the
// objects they are handed and a proxy class does not survive that.
#pragma once

#import <objc/runtime.h>

namespace mtlinsp {

/**
 * Replaces `cls`'s implementation of `sel` with `replacement`, once per class.
 *
 * Returns false if the class has no such method — expected, since a protocol carries optional
 * methods and API levels differ. Safe to call repeatedly for the same class and selector.
 */
bool Hook(Class cls, SEL sel, IMP replacement);

/**
 * The implementation `sel` had before it was hooked, for the receiver's class. A hook always
 * calls this and forwards to it; returning without doing so would drop the application's call.
 *
 * Resolved through the Method the receiver's class holds for the selector, which is the same
 * Method that was replaced — including when it lives on an ancestor shared with sibling classes,
 * where a per-class lookup would come up empty. See the note on g_originals in swizzle.mm.
 */
IMP Original(id self, SEL sel);

/**
 * True the first time a class is passed in, false afterwards. The hook installers are called on
 * every object they see, not only new ones — CoreAnimation asks for the device repeatedly, and a
 * command buffer is created per frame — so this keeps the work and the log to the first sighting.
 */
bool FirstSighting(Class cls);

/** Hooks every method of a class the library knows, once, the first time an object of it appears. */
void HookDeviceClass(id device);
void HookCommandQueueClass(id queue);
void HookCommandBufferClass(id commandBuffer);
void HookRenderEncoderClass(id encoder);
void HookComputeEncoderClass(id encoder);
void HookBlitEncoderClass(id encoder);

/**
 * Suppresses everything but the outermost observation of one application call.
 *
 * Metal wraps its own objects when its validation layers are on: with MTL_DEBUG_LAYER the
 * application holds an MTLDebugCommandBuffer, with MTL_SHADER_VALIDATION as well that wraps an
 * MTLGPUDebugCommandBuffer, and only the innermost is the driver's AGX* object. Each forwards to
 * the next, and since the hooks are installed per class every level is intercepted, so one
 * `presentDrawable:` from the application arrives here two or three times.
 *
 * The outermost call is the application's, so that is the one recorded; the rest are Metal
 * talking to itself. Every level still forwards — only the recording is skipped — so the
 * application's behaviour is unchanged either way.
 *
 * Per thread, because Metal allows encoding from several threads at once.
 */
class Reentry {
public:
    Reentry();
    ~Reentry();
    /** True when this is the application's call rather than one wrapper calling the next. */
    bool outermost() const { return outermost_; }

private:
    bool outermost_;
};

/** Logging: `MTLINSP_LOG=1` in the environment, matching the layer's VKINSP_LOG. */
bool LogEnabled();
void Log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/** The class name of an object, for the log. */
const char *ClassName(id object);

}  // namespace mtlinsp
