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
// objects they are handed and a proxy class does not survive that. (RenderDoc wraps, and has to
// add Metal's private MTLTextureImplementation protocol to its proxy so the driver's assert
// passes — exactly that problem.)
#pragma once

#import <objc/runtime.h>

namespace mtlinsp {

/**
 * Replaces `cls`'s implementation of `sel` with `replacement`, once per class.
 *
 * Returns false if the class has no such method — expected, since a protocol carries optional
 * methods and API levels differ — or if the class already carries a *different* replacement for
 * the selector, which would chain two hooks and confuse the original resolution below. Safe to
 * call repeatedly for the same class and selector.
 *
 * The original is published before the replacement is installed, so a call that lands on the
 * new implementation from another thread always finds what to forward to.
 */
bool Hook(Class cls, SEL sel, IMP replacement);

/**
 * True the first time a class is passed in, false afterwards. The hook installers are called on
 * every object they see, not only new ones — CoreAnimation asks for the device repeatedly, and a
 * command buffer is created per frame — so this keeps the work and the log to the first sighting.
 */
bool FirstSighting(Class cls);

/**
 * One intercepted call: whether it is the application's, and what it should forward to.
 *
 * Every hook constructs one of these first, forwards through `original()`, and records only when
 * `outermost()`. Two things are decided here:
 *
 * **Re-entry.** Metal wraps its own objects when its validation layers are on: with
 * MTL_DEBUG_LAYER the application holds an MTLDebugCommandBuffer, with MTL_SHADER_VALIDATION as
 * well that wraps an MTLGPUDebugCommandBuffer, and only the innermost is the driver's AGX* object.
 * Each forwards to the next, and since the hooks are installed per class every level is
 * intercepted, so one `presentDrawable:` from the application arrives two or three times. The
 * outermost call is the application's, so that is the one recorded; the rest are Metal talking to
 * itself. Every level still forwards, so behaviour is unchanged. Per thread, because Metal allows
 * encoding from several threads at once. The library's own Metal calls, issued from inside a hook,
 * are nested by construction and so never recorded either.
 *
 * **Which original.** A hook lives on the exact class it was installed on, and an instance of a
 * subclass reaches it by inheritance, so the original is found by walking up from the receiver's
 * class. The one case that walk gets wrong is a hooked subclass whose own override calls `super`
 * into a hooked ancestor: both levels run the same replacement, and resolving from the receiver's
 * class twice returns the subclass's override twice, forever. So a nested observation of the same
 * selector on the same object resolves from above where the outer one resolved. Metal's class
 * trees are shaped that way on some GPUs (only Apple Silicon is verified), which is why it is
 * handled rather than assumed away.
 *
 * Lookups are lock-free: the table of originals is an immutable snapshot replaced wholesale by
 * Hook(), which happens a few dozen times in a process, and read by every intercepted draw.
 */
class Reentry {
public:
    Reentry(id self, SEL sel);
    ~Reentry();
    /** True when this is the application's call rather than one wrapper calling the next. */
    bool outermost() const { return outermost_; }
    /** The implementation this hook shadows for this receiver. Cast to the method's signature. */
    IMP original() const { return original_; }

private:
    bool outermost_;
    IMP original_;
};

/**
 * Marks Metal calls the library issues on its own behalf — read-back blits, staging buffers, the
 * image read-back queue — so that the tracker does not announce them as the application's objects
 * and the capture does not record them as the application's commands. Per thread.
 */
class Internal {
public:
    Internal();
    ~Internal();
};
bool IsInternal();

/** Logging: `MTLINSP_LOG=1` in the environment, matching the layer's VKINSP_LOG. */
bool LogEnabled();
void Log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/** The class name of an object, for the log. */
const char *ClassName(id object);

}  // namespace mtlinsp
