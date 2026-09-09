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
#include "swizzle.h"

#import <Metal/Metal.h>

namespace {

id<MTLDevice> Interposed_MTLCreateSystemDefaultDevice(void) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    mtlinsp::Log("MTLCreateSystemDefaultDevice -> %s (%s)", mtlinsp::ClassName(device),
                 device == nil ? "" : device.name.UTF8String);
    if (device != nil) mtlinsp::HookDeviceClass(device);
    return device;
}

NSArray<id<MTLDevice>> *Interposed_MTLCopyAllDevices(void) {
    NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
    mtlinsp::Log("MTLCopyAllDevices -> %lu device(s)", (unsigned long)devices.count);
    for (id<MTLDevice> device in devices) mtlinsp::HookDeviceClass(device);
    return devices;
}

struct Interpose {
    const void *replacement;
    const void *replacee;
};

__attribute__((used, section("__DATA,__interpose"))) const Interpose kInterposers[] = {
    {(const void *)&Interposed_MTLCreateSystemDefaultDevice,
     (const void *)&MTLCreateSystemDefaultDevice},
    {(const void *)&Interposed_MTLCopyAllDevices, (const void *)&MTLCopyAllDevices},
};

__attribute__((constructor)) void Loaded(void) {
    mtlinsp::Log("loaded into pid %d", getpid());
}

}  // namespace
