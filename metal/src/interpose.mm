// Where the library gets into the process, and where the class tree is entered.
//
// Metal has no loader and no layer mechanism, so there is nothing to register with: the library
// is loaded by DYLD_INSERT_LIBRARIES and takes the only chokepoints the API has, the C functions
// that hand out a device. Everything else is an Objective-C protocol method reached from one of
// those devices (see swizzle.h).
//
// dyld interposing rather than symbol interposition by name: a __DATA,__interpose section is
// applied by dyld to every image in the process, including Metal.framework in the shared cache,
// and only when this library is inserted.
//
// Four entry points hand out devices. An application that takes its device from somewhere else —
// a CAMetalLayer's preferredDevice, an MTKView — is caught at the layer's nextDrawable instead
// (hooks_device.mm), which every device that draws to a window passes through.
#include "hooks.h"
#include "swizzle.h"
#include "tracker.h"

#import <CoreGraphics/CGDirectDisplayMetal.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>

namespace {

id<MTLDevice> Interposed_MTLCreateSystemDefaultDevice(void) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    mtlinsp::TrackDeviceObject(device, "MTLCreateSystemDefaultDevice");
    return device;
}

NSArray<id<MTLDevice>> *Interposed_MTLCopyAllDevices(void) {
    NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
    mtlinsp::Log("MTLCopyAllDevices -> %lu device(s)", (unsigned long)devices.count);
    for (id<MTLDevice> device in devices) mtlinsp::TrackDeviceObject(device, "MTLCopyAllDevices");
    return devices;
}

// The observer parameter's exact spelling (a pointer to an id, with nullability qualifiers) has
// varied between SDKs, and an interposer has to match the replacee's type exactly, so the
// signature is taken from the declaration rather than written out.
template <typename F>
struct ObserverInterposer;
template <typename R, typename... A>
struct ObserverInterposer<R (*)(A...)> {
    static R Call(A... args) {
        R devices = MTLCopyAllDevicesWithObserver(args...);
        mtlinsp::Log("MTLCopyAllDevicesWithObserver -> %lu device(s)", (unsigned long)devices.count);
        for (id<MTLDevice> device in devices) {
            mtlinsp::TrackDeviceObject(device, "MTLCopyAllDevicesWithObserver");
        }
        return devices;
    }
};
constexpr auto Interposed_MTLCopyAllDevicesWithObserver =
    &ObserverInterposer<decltype(&MTLCopyAllDevicesWithObserver)>::Call;

id<MTLDevice> Interposed_CGDirectDisplayCopyCurrentMetalDevice(CGDirectDisplayID display) {
    id<MTLDevice> device = CGDirectDisplayCopyCurrentMetalDevice(display);
    mtlinsp::TrackDeviceObject(device, "CGDirectDisplayCopyCurrentMetalDevice");
    return device;
}

struct Interpose {
    const void *replacement;
    const void *replacee;
};

__attribute__((used, section("__DATA,__interpose"))) const Interpose kInterposers[] = {
    {(const void *)&Interposed_MTLCreateSystemDefaultDevice,
     (const void *)&MTLCreateSystemDefaultDevice},
    {(const void *)&Interposed_MTLCopyAllDevices, (const void *)&MTLCopyAllDevices},
    {(const void *)Interposed_MTLCopyAllDevicesWithObserver,
     (const void *)&MTLCopyAllDevicesWithObserver},
    {(const void *)&Interposed_CGDirectDisplayCopyCurrentMetalDevice,
     (const void *)&CGDirectDisplayCopyCurrentMetalDevice},
};

__attribute__((constructor)) void Loaded(void) {
    mtlinsp::Log("loaded into pid %d", getpid());
    // The listener comes up before the application has a device, so the UI can be waiting when
    // the process starts or attach later and get a snapshot either way.
    mtlinsp::StartTracking();
    mtlinsp::HookDrawableSource();
}

}  // namespace
