// Support for the exported frame: the device, the data file, uploads, the render targets read back
// and compared with what the capture holds, and the PNGs. Nothing in here is specific to the frame;
// the frame_*.mm files are.
#pragma once

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstddef>
#include <cstdint>
#include <string>

extern id<MTLDevice> device;
extern id<MTLCommandQueue> queue;

/** Prints what failed and exits with code 2. */
[[noreturn]] void Fail(const char* what, NSError* error);
[[noreturn]] void Fail(const std::string& message);

/** The system default device and a queue on it. */
void CreateDevice(void);
const char* DeviceName(void);

// The data file: shader bytes, texture and buffer contents, and the captured targets, by offset.
bool LoadData(const std::string& path);
const void* Data(uint64_t offset, uint64_t size);

/** A `.metal` file in `shaders`, beside the executable, as the source to compile. */
NSString* LoadShader(NSString* path);
/** A library from metallib bytes in the data file. */
id<MTLLibrary> LoadLibrary(const void* bytes, uint64_t size, NSError** error);

/** Writes one mip of a texture through a staging buffer, which a private texture needs. */
void UploadTexture(id<MTLTexture> texture, const void* data, uint64_t size, uint64_t rowBytes,
    uint64_t width, uint64_t height, uint64_t slice, uint64_t level);
/** Writes a buffer's range; a private buffer goes through a staging blit. */
void UploadBuffer(id<MTLBuffer> buffer, uint64_t offset, const void* data, uint64_t size);

/**
 * Copies one subresource of a render target into a staging buffer at this point of the command
 * buffer, to compare with `captured` once the buffer has run. `options` is
 * MTLBlitOptionDepthFromDepthStencil for a depth aspect, MTLBlitOptionNone otherwise.
 */
void ReadbackTexture(id<MTLCommandBuffer> commands, id<MTLTexture> texture, const char* name,
    uint64_t slice, uint64_t level, uint64_t width, uint64_t height, uint64_t rowBytes,
    uint64_t imageBytes, MTLBlitOption options, const void* captured, uint64_t capturedSize);

/** Prints every comparison and writes the images to `directory`; the process exit code. */
int ReportResults(const std::string& directory, bool writeImages);

// The window (the default; --batch compares the targets instead). The frame runs again and again,
// and what it leaves on screen is copied to a drawable of the window's layer and presented.
/**
 * Opens a window of the output's size, and from then on the frame's read-backs are not taken
 * (ReadbackTexture returns at once): they are --batch's. False, with the reason printed, when there
 * is nothing to show it on or the output is not something a layer's drawable can hold.
 */
bool OpenOutputWindow(id<MTLTexture> output, const char* title);
/** Whether a present waits for the display (the default); without, the frame runs as fast as it can. */
void SetOutputVsync(bool on);
/** Copies the output to the layer's next drawable and presents; false once the window was closed. */
bool PresentOutput(id<MTLTexture> output);
/** Closes the window and prints how many frames it showed; the process exit code. */
int CloseOutputWindow(void);
