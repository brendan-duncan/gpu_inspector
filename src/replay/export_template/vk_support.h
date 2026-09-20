// Support for the exported frame: the Vulkan loader opened at run time, memory for every image and
// buffer, image layouts tracked per subresource, uploads from the data file, and the render targets
// read back and compared with what the capture holds. Nothing in here is specific to the frame;
// the frame_*.cpp files are.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vk_functions.h"

extern VkInstance instance;
extern VkPhysicalDevice physicalDevice;
extern VkDevice device;
/** The first queue of the family the frame's submissions used. */
extern VkQueue queue;

/** Prints what failed and exits with code 2. */
[[noreturn]] void Fail(const char* what, VkResult result);
[[noreturn]] void Fail(const std::string& message);
#define VK_CHECK(call) do { const VkResult vk_result_ = (call); if (vk_result_ < 0) Fail(#call, vk_result_); } while (0)

// The data file: SPIR-V, image and buffer contents, and the captured targets, by offset.
bool LoadData(const std::string& path);
const void* Data(uint64_t offset, uint64_t size);

// Instance and device
PFN_vkGetInstanceProcAddr LoadVulkanLoader();
bool ValidationLayerAvailable();
std::vector<const char*> AvailableInstanceExtensions(const char* const* wanted, size_t count);
std::vector<const char*> AvailableDeviceExtensions(VkPhysicalDevice gpu, const char* const* wanted, size_t count);
void CreateDebugMessenger();
/** The GPU with this name, else the first discrete one, else the first. */
VkPhysicalDevice SelectPhysicalDevice(const char* preferredName);
const char* DeviceName();
/** After vkCreateDevice: memory properties, the utility command pool, `queue`, and which queues exist. */
void InitDevice(uint32_t family, const VkDeviceQueueCreateInfo* queues, uint32_t queueCount);
/** The device's queue at family/index when it was created, else `queue`. */
VkQueue DeviceQueue(uint32_t family, uint32_t index);

// Memory: every image and buffer gets an allocation of its own, device local where it can be.
void BindImageMemory(VkImage image);
void BindBufferMemory(VkBuffer buffer, bool deviceAddress);
/** Tracks an image's subresource layouts, which the transitions below move. */
void RegisterImage(VkImage image, VkFormat format, VkExtent3D extent, uint32_t mips, uint32_t layers, VkSampleCountFlagBits samples);

// One-time command buffers, submitted to `queue` and waited for.
VkCommandBuffer BeginOneTime();
void EndOneTime(VkCommandBuffer cb);

// Layouts, one entry per subresource (mip * layers + layer); VK_IMAGE_LAYOUT_UNDEFINED leaves one where it is.
void TransitionSubresources(VkCommandBuffer cb, VkImage image, const VkImageLayout* targets, size_t count);
/** Says where an image's subresources are now: the frame's own barriers and passes move them without the tracking seeing it. */
void SetImageLayouts(VkImage image, const VkImageLayout* layouts, size_t count);
void TransitionAll(VkCommandBuffer cb, VkImage image, VkImageLayout layout);

// Contents
/** Moves every subresource to TRANSFER_DST_OPTIMAL and copies the regions from `data`. */
void UploadImage(VkImage image, const VkBufferImageCopy* regions, uint32_t regionCount, const void* data, size_t size);
void UploadBuffer(VkBuffer buffer, VkDeviceSize offset, const void* data, size_t size);

// The frame
/**
 * Copies a render target at this point of the command buffer, to compare with `captured` once the
 * submission has run. A multisampled target is resolved first, as the capture read it: colour with
 * vkCmdResolveImage, depth by sample zero. Its layout is put back after.
 */
void ReadbackImage(VkCommandBuffer cb, VkImage image, const char* name, VkImageAspectFlags aspect, uint32_t mip, uint32_t baseLayer,
                   uint32_t layers, VkExtent2D extent, VkImageLayout layout, VkSampleCountFlagBits samples, VkFormat format,
                   const void* captured, size_t capturedSize);
/** vkQueueSubmit of the command buffers, without semaphores or a fence, then vkQueueWaitIdle. */
void SubmitAndWait(VkQueue queue, const VkCommandBuffer* commandBuffers, uint32_t count);
/** Compares the submission's read-backs with the capture's copies. */
void CompleteReadbacks();
/** Prints every comparison and writes the images to `directory`; the process exit code: 0 all identical, 1 otherwise. */
int ReportResults(const std::string& directory, bool writeImages);
// The window (the default; --batch compares the targets instead). The frame runs again and again,
// and what it leaves on screen is blitted to a swapchain of the support's own and presented.
struct FrameOutputInfo {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   // where the frame leaves it
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
};
/** Before CreateInstance: whether the instance and the device are to have what a window needs. */
void WantWindow(bool wanted);
/** The surface extensions of this platform, those this Vulkan has, when a window is wanted. */
void AddWindowInstanceExtensions(std::vector<const char*>& extensions);
/** VK_KHR_swapchain, when a window is wanted and the device has it. */
void AddWindowDeviceExtensions(VkPhysicalDevice gpu, std::vector<const char*>& extensions);
/**
 * Opens a window of the output's size with a swapchain to show it in, and from then on the frame's
 * read-backs are not taken (ReadbackImage returns at once): they are --batch's. False, with the
 * reason printed, when there is nothing to show it on or the output cannot be blitted.
 */
bool OpenOutputWindow(const FrameOutputInfo& output, const char* title);
/** Whether a present waits for the display (the default); without, the frame runs as fast as it can. */
void SetOutputVsync(bool on);
/** Blits the output to the next swapchain image and presents; false once the window was closed. */
bool PresentOutput(const FrameOutputInfo& output);
/** Closes the window and prints how many frames it showed; the process exit code. */
int CloseOutputWindow();

/** Frees what the support allocated, then the device and the instance. */
void DestroySupport();
