// frame_window.h on macOS: an NSWindow whose view is backed by a CAMetalLayer, which is what both
// Metal and Vulkan (MoltenVK, VK_EXT_metal_surface) present to. There is no application bundle and
// no run loop of AppKit's: the events are taken one by one, as the other platforms do.
#include "frame_window.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

@interface FrameWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) BOOL closed;
@end

@implementation FrameWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    self.closed = YES;
    return NO;   // the program closes it, once its loop has seen this
}
@end

struct FrameWindow {
    NSWindow* window = nil;
    CAMetalLayer* layer = nil;
    FrameWindowDelegate* delegate = nil;
    bool closed = false;
};

FrameWindow* OpenFrameWindow(const char* title, uint32_t width, uint32_t height) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        // A program started from a terminal is no application to AppKit until it says so.
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];

        // The frame's pixels one to one: the window is that many pixels, not that many points.
        const CGFloat scale = NSScreen.mainScreen ? NSScreen.mainScreen.backingScaleFactor : 1.0;
        const NSRect rect = NSMakeRect(0, 0, width / scale, height / scale);
        // Not resizable: the swap chain is made once, for this size.
        const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        NSWindow* nsWindow = [[NSWindow alloc] initWithContentRect:rect styleMask:style backing:NSBackingStoreBuffered defer:NO];
        if (!nsWindow) return nullptr;

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.contentsScale = scale;
        layer.drawableSize = CGSizeMake(width, height);
        layer.framebufferOnly = NO;   // the frame's output is copied into the drawable
        NSView* view = [[NSView alloc] initWithFrame:rect];
        view.wantsLayer = YES;
        view.layer = layer;
        nsWindow.contentView = view;

        auto* window = new FrameWindow;
        window->window = nsWindow;
        window->layer = layer;
        window->delegate = [[FrameWindowDelegate alloc] init];
        nsWindow.delegate = window->delegate;
        nsWindow.releasedWhenClosed = NO;
        nsWindow.title = [NSString stringWithUTF8String:title ? title : ""];
        [nsWindow center];
        [nsWindow makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        return window;
    }
}

bool PumpFrameWindow(FrameWindow* window) {
    if (!window) return false;
    @autoreleasepool {
        for (;;) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!event) break;
            if (event.type == NSEventTypeKeyDown && event.keyCode == 53) {   // Escape
                window->closed = true;
                continue;
            }
            [NSApp sendEvent:event];
        }
        if (window->delegate.closed) window->closed = true;
    }
    return !window->closed;
}

void SetFrameWindowTitle(FrameWindow* window, const char* title) {
    if (!window) return;
    @autoreleasepool {
        window->window.title = [NSString stringWithUTF8String:title ? title : ""];
    }
}

void CloseFrameWindow(FrameWindow* window) {
    if (!window) return;
    @autoreleasepool {
        window->window.delegate = nil;
        [window->window close];
    }
    delete window;
}

void* FrameWindowHandle(FrameWindow* window) { return window ? (__bridge void*)window->layer : nullptr; }
void* FrameWindowDisplay(FrameWindow*) { return nullptr; }
