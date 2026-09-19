#include "vk_support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

VkInstance instance = VK_NULL_HANDLE;
VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
VkDevice device = VK_NULL_HANDLE;
VkQueue queue = VK_NULL_HANDLE;

namespace {

struct ImageInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    uint32_t mips = 1;
    uint32_t layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkImageLayout> layouts;
};

struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

/** A read-back waiting for its submission, then compared. */
struct Readback {
    Staging staging;
    std::string name;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0;
    uint32_t height = 0;
    /** Layers read back one after another (a multiview target's views). */
    uint32_t layers = 1;
    const uint8_t* captured = nullptr;
    size_t capturedSize = 0;
    std::string note;
    // After the comparison:
    bool compared = false;
    uint64_t texels = 0;
    uint64_t differing = 0;
    uint32_t maxByteDelta = 0;
    std::vector<uint8_t> replayed;
};

void* library = nullptr;
VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
VkPhysicalDeviceMemoryProperties memoryProperties{};
VkPhysicalDeviceProperties deviceProperties{};
VkCommandPool utilityPool = VK_NULL_HANDLE;
uint32_t queueFamily = 0;
std::vector<VkDeviceMemory> memories;
std::vector<std::pair<uint32_t, uint32_t>> createdQueues;   // family, count
std::vector<uint8_t> dataBytes;
std::map<VkImage, ImageInfo> images;
std::vector<Readback> pending;
std::vector<Readback> results;
/** What a read-back made for itself, released once its submission has run. */
struct Transient {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView sourceView = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
};
std::vector<Transient> transients;
/** Render passes that resolve sample zero of a multisampled depth target, by format and sample count. */
std::map<std::pair<VkFormat, VkSampleCountFlagBits>, VkRenderPass> depthResolvePasses;
size_t validationErrors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const bool error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    if (error) ++validationErrors;
    std::fprintf(stderr, "validation %s: %s\n", error ? "error" : "warning", data && data->pMessage ? data->pMessage : "");
    return VK_FALSE;
}

VkImageAspectFlags FormatAspects(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT:
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

bool AllocateBound(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags want, VkDeviceMemory& memory, bool deviceAddress, bool track) {
    VkMemoryAllocateFlagsInfo addressFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    addressFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if (!(requirements.memoryTypeBits & (1u << i))) continue;
            const VkMemoryPropertyFlags flags = memoryProperties.memoryTypes[i].propertyFlags;
            if (pass == 0 && (flags & want) != want) continue;
            VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            if (deviceAddress) info.pNext = &addressFlags;
            info.allocationSize = requirements.size;
            info.memoryTypeIndex = i;
            if (vkAllocateMemory(device, &info, nullptr, &memory) == VK_SUCCESS) {
                if (track) memories.push_back(memory);
                return true;
            }
        }
    }
    return false;
}

bool CreateStaging(VkDeviceSize size, Staging& staging) {
    staging = Staging{};
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(size, 1);
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device, &info, nullptr, &staging.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, staging.buffer, &req);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool ok = false;
    // Cached system memory first: it is read back on the CPU.
    for (int pass = 0; pass < 3 && !ok; ++pass) {
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount && !ok; ++i) {
            const VkMemoryPropertyFlags flags = memoryProperties.memoryTypes[i].propertyFlags;
            if (!(req.memoryTypeBits & (1u << i)) || (flags & want) != want) continue;
            if (pass == 0 && (!(flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) || (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) continue;
            if (pass == 1 && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = i;
            ok = vkAllocateMemory(device, &alloc, nullptr, &memory) == VK_SUCCESS;
        }
    }
    if (!ok) {
        vkDestroyBuffer(device, staging.buffer, nullptr);
        staging.buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(device, staging.buffer, memory, 0);
    vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &staging.mapped);
    staging.memory = memory;
    staging.size = size;
    return true;
}

void DestroyStaging(Staging& staging) {
    if (staging.memory) {
        vkUnmapMemory(device, staging.memory);
        vkFreeMemory(device, staging.memory, nullptr);
    }
    if (staging.buffer) vkDestroyBuffer(device, staging.buffer, nullptr);
    staging = Staging{};
}

void Barrier(VkCommandBuffer cb, VkImage image, const VkImageSubresourceRange& range, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void ReleaseTransients() {
    for (Transient& t : transients) {
        if (t.framebuffer) vkDestroyFramebuffer(device, t.framebuffer, nullptr);
        if (t.sourceView) vkDestroyImageView(device, t.sourceView, nullptr);
        if (t.view) vkDestroyImageView(device, t.view, nullptr);
        if (t.image) vkDestroyImage(device, t.image, nullptr);
        if (t.memory) vkFreeMemory(device, t.memory, nullptr);
    }
    transients.clear();
}

/**
 * Sample zero of one subresource of a multisampled depth image, in a single-sampled copy left in
 * TRANSFER_SRC_OPTIMAL: a subpass that resolves into it (VK_KHR_depth_stencil_resolve, core in 1.2),
 * which is how the capture read the target back. Null with `why` when it cannot be made.
 */
VkImage ResolveDepth(VkCommandBuffer cb, VkImage image, const ImageInfo& info, uint32_t mip, uint32_t baseLayer, VkImageLayout layout, std::string& why) {
    if (!vkCreateRenderPass2) {
        why = "resolving multisampled depth needs Vulkan 1.2 render passes";
        return VK_NULL_HANDLE;
    }
    const VkExtent2D extent{std::max(1u, info.extent.width >> mip), std::max(1u, info.extent.height >> mip)};
    const VkImageAspectFlags aspects = FormatAspects(info.format);
    VkRenderPass& rp = depthResolvePasses[{info.format, info.samples}];
    if (!rp) {
        VkAttachmentDescription2 attachments[2]{};
        for (VkAttachmentDescription2& a : attachments) {
            a.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            a.format = info.format;
            a.storeOp = a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        attachments[0].samples = info.samples;
        attachments[0].loadOp = attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const VkAttachmentReference2 source{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, aspects};
        const VkAttachmentReference2 target{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, aspects};
        VkSubpassDescriptionDepthStencilResolve resolve{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
        resolve.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        resolve.stencilResolveMode = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
        resolve.pDepthStencilResolveAttachment = &target;
        VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        subpass.pNext = &resolve;
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &source;
        VkRenderPassCreateInfo2 create{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        create.attachmentCount = 2;
        create.pAttachments = attachments;
        create.subpassCount = 1;
        create.pSubpasses = &subpass;
        if (vkCreateRenderPass2(device, &create, nullptr, &rp) != VK_SUCCESS) rp = VK_NULL_HANDLE;
    }
    if (!rp) {
        why = "the pass that resolves multisampled depth could not be created";
        return VK_NULL_HANDLE;
    }

    Transient t;
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = info.format;
    create.extent = {extent.width, extent.height, 1};
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkMemoryRequirements req{};
    bool ok = vkCreateImage(device, &create, nullptr, &t.image) == VK_SUCCESS;
    if (ok) {
        vkGetImageMemoryRequirements(device, t.image, &req);
        ok = AllocateBound(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.memory, false, false) && vkBindImageMemory(device, t.image, t.memory, 0) == VK_SUCCESS;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    if (ok) {
        view.image = t.image;
        view.subresourceRange = {aspects, 0, 1, 0, 1};
        ok = vkCreateImageView(device, &view, nullptr, &t.view) == VK_SUCCESS;
    }
    if (ok) {
        view.image = image;
        view.subresourceRange = {aspects, mip, 1, baseLayer, 1};
        ok = vkCreateImageView(device, &view, nullptr, &t.sourceView) == VK_SUCCESS;
    }
    if (ok) {
        const VkImageView views[2] = {t.sourceView, t.view};
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = rp;
        fb.attachmentCount = 2;
        fb.pAttachments = views;
        fb.width = extent.width;
        fb.height = extent.height;
        fb.layers = 1;
        ok = vkCreateFramebuffer(device, &fb, nullptr, &t.framebuffer) == VK_SUCCESS;
    }
    transients.push_back(t);   // released with the submission, whatever was made of it
    if (!ok) {
        why = "the multisampled depth target could not be resolved";
        return VK_NULL_HANDLE;
    }
    const VkImageSubresourceRange range{aspects, mip, 1, baseLayer, 1};
    Barrier(cb, image, range, layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = rp;
    begin.framebuffer = t.framebuffer;
    begin.renderArea = {{0, 0}, extent};
    vkCmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cb);
    Barrier(cb, image, range, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, layout);
    return t.image;
}

/** A single-sampled copy of one subresource of a multisampled colour image, in TRANSFER_SRC_OPTIMAL; null with `why` when it cannot be made. */
VkImage ResolveTarget(VkCommandBuffer cb, VkImage image, const ImageInfo& info, uint32_t mip, uint32_t baseLayer, VkImageLayout layout, std::string& why) {
    const VkExtent2D extent{std::max(1u, info.extent.width >> mip), std::max(1u, info.extent.height >> mip)};
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = info.format;
    create.extent = {extent.width, extent.height, 1};
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage resolved = VK_NULL_HANDLE;
    if (vkCreateImage(device, &create, nullptr, &resolved) != VK_SUCCESS) {
        why = "the resolve image could not be created";
        return VK_NULL_HANDLE;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, resolved, &req);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (!AllocateBound(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory, false, false) || vkBindImageMemory(device, resolved, memory, 0) != VK_SUCCESS) {
        vkDestroyImage(device, resolved, nullptr);
        why = "no memory for the resolve image";
        return VK_NULL_HANDLE;
    }
    Transient kept;
    kept.image = resolved;
    kept.memory = memory;
    transients.push_back(kept);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, baseLayer, 1};
    Barrier(cb, image, range, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Barrier(cb, resolved, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageResolve region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, baseLayer, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {extent.width, extent.height, 1};
    vkCmdResolveImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resolved, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Barrier(cb, image, range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
    Barrier(cb, resolved, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return resolved;
}

// ---- PNG output (stored deflate blocks: large files, no zlib)

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool WritePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(((size_t)width * 4 + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + (size_t)y * width * 4, rgba.begin() + ((size_t)y + 1) * width * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF));
        z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((uint8_t)(adler >> s));

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.write((const char*)signature, 8);
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        const uint32_t len = (uint32_t)data.size();
        const uint8_t be[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
        out.write((const char*)be, 4);
        out.write((const char*)body.data(), (std::streamsize)body.size());
        const uint32_t crc = Crc32(body.data(), body.size(), 0);
        const uint8_t cb[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
        out.write((const char*)cb, 4);
    };
    const std::vector<uint8_t> ihdr = {(uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
                                       (uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height,
                                       8, 6, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return true;
}

/** 8-bit RGBA and BGRA as they are, 32-bit float depth stretched to its range; false for other formats. */
bool ToRgba(const Readback& r, const uint8_t* bytes, size_t size, std::vector<uint8_t>& rgba) {
    const size_t texels = (size_t)r.width * r.height;
    rgba.assign(texels * 4, 255);
    const bool bgr = r.format == VK_FORMAT_B8G8R8A8_UNORM || r.format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool rgb = r.format == VK_FORMAT_R8G8B8A8_UNORM || r.format == VK_FORMAT_R8G8B8A8_SRGB;
    if ((bgr || rgb) && r.aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        if (size < texels * 4) return false;
        for (size_t i = 0; i < texels; ++i) {
            rgba[i * 4] = bytes[i * 4 + (bgr ? 2 : 0)];
            rgba[i * 4 + 1] = bytes[i * 4 + 1];
            rgba[i * 4 + 2] = bytes[i * 4 + (bgr ? 0 : 2)];
        }
        return true;
    }
    if ((r.format == VK_FORMAT_D32_SFLOAT || r.format == VK_FORMAT_D32_SFLOAT_S8_UINT) && r.aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
        if (size < texels * 4) return false;
        float lo = INFINITY, hi = -INFINITY;
        for (size_t i = 0; i < texels; ++i) {
            float v;
            std::memcpy(&v, &bytes[i * 4], 4);
            if (std::isfinite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
        }
        for (size_t i = 0; i < texels; ++i) {
            float v;
            std::memcpy(&v, &bytes[i * 4], 4);
            const uint8_t g = hi > lo ? (uint8_t)std::lround(255.0 * (v - lo) / (hi - lo)) : 0;
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g;
        }
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------------------------

void Fail(const char* what, VkResult result) {
    std::fprintf(stderr, "%s returned %d\n", what, (int)result);
    std::fflush(stderr);
    std::exit(2);
}

void Fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::fflush(stderr);
    std::exit(2);
}

bool LoadData(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    dataBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

const void* Data(uint64_t offset, uint64_t size) {
    if (offset + size > dataBytes.size()) Fail("the data file is shorter than the frame expects (offset " + std::to_string(offset) + ", " + std::to_string(size) + " bytes)");
    return dataBytes.data() + offset;
}

PFN_vkGetInstanceProcAddr LoadVulkanLoader() {
#ifdef _WIN32
    library = LoadLibraryA("vulkan-1.dll");
    return library ? (PFN_vkGetInstanceProcAddr)GetProcAddress(static_cast<HMODULE>(library), "vkGetInstanceProcAddr") : nullptr;
#else
    library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) library = dlopen("libvulkan.dylib", RTLD_NOW | RTLD_LOCAL);
    return library ? (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr") : nullptr;
#endif
}

bool ValidationLayerAvailable() {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& l : layers)
        if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) return true;
    std::fprintf(stderr, "the validation layer is not installed (Vulkan SDK); running without it\n");
    return false;
}

std::vector<const char*> AvailableInstanceExtensions(const char* const* wanted, size_t count) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> available(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, available.data());
    std::vector<const char*> out;
    for (size_t i = 0; i < count; ++i) {
        const bool has = std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, wanted[i]); });
        if (has) out.push_back(wanted[i]);
        else std::fprintf(stderr, "instance extension not available here: %s\n", wanted[i]);
    }
    return out;
}

std::vector<const char*> AvailableDeviceExtensions(VkPhysicalDevice gpu, const char* const* wanted, size_t count) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> available(n);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, available.data());
    std::vector<const char*> out;
    for (size_t i = 0; i < count; ++i) {
        const bool has = std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, wanted[i]); });
        if (has) out.push_back(wanted[i]);
        else std::fprintf(stderr, "device extension not available here: %s\n", wanted[i]);
    }
    return out;
}

void CreateDebugMessenger() {
    if (!vkCreateDebugUtilsMessengerEXT) return;
    VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    m.pfnUserCallback = DebugCallback;
    vkCreateDebugUtilsMessengerEXT(instance, &m, nullptr, &messenger);
}

VkPhysicalDevice SelectPhysicalDevice(const char* preferredName) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance, &count, physicals.data());
    if (physicals.empty()) Fail("no Vulkan device");
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties chosenProps{};
    for (VkPhysicalDevice p : physicals) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (preferredName && *preferredName && !std::strcmp(preferredName, props.deviceName)) { chosen = p; chosenProps = props; break; }
        if (!chosen || (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && chosenProps.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) {
            chosen = p;
            chosenProps = props;
        }
    }
    deviceProperties = chosenProps;
    if (preferredName && *preferredName && std::strcmp(preferredName, chosenProps.deviceName))
        std::fprintf(stderr, "the frame was captured on %s; running on %s\n", preferredName, chosenProps.deviceName);
    return chosen;
}

const char* DeviceName() { return deviceProperties.deviceName; }

void InitDevice(uint32_t family, const VkDeviceQueueCreateInfo* queues, uint32_t queueCount) {
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    queueFamily = family;
    for (uint32_t i = 0; i < queueCount; ++i) createdQueues.emplace_back(queues[i].queueFamilyIndex, queues[i].queueCount);
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = family;
    VK_CHECK(vkCreateCommandPool(device, &pool, nullptr, &utilityPool));
}

VkQueue DeviceQueue(uint32_t family, uint32_t index) {
    for (const auto& [f, count] : createdQueues) {
        if (f != family || index >= count) continue;
        VkQueue q = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, family, index, &q);
        if (q) return q;
    }
    return queue;
}

void BindImageMemory(VkImage image) {
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, image, &req);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (!AllocateBound(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory, false, true)) Fail("no memory for an image");
    VK_CHECK(vkBindImageMemory(device, image, memory, 0));
}

void BindBufferMemory(VkBuffer buffer, bool deviceAddress) {
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (!AllocateBound(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory, deviceAddress, true)) Fail("no memory for a buffer");
    VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0));
}

void RegisterImage(VkImage image, VkFormat format, VkExtent3D extent, uint32_t mips, uint32_t layers, VkSampleCountFlagBits samples) {
    ImageInfo info;
    info.format = format;
    info.extent = extent;
    info.mips = std::max(1u, mips);
    info.layers = std::max(1u, layers);
    info.samples = samples;
    info.layouts.assign((size_t)info.mips * info.layers, VK_IMAGE_LAYOUT_UNDEFINED);
    images[image] = info;
}

VkCommandBuffer BeginOneTime() {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = utilityPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &alloc, &cb));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));
    return cb;
}

void EndOneTime(VkCommandBuffer cb) {
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, utilityPool, 1, &cb);
}

void TransitionSubresources(VkCommandBuffer cb, VkImage image, const VkImageLayout* targets, size_t count) {
    auto it = images.find(image);
    if (it == images.end()) Fail("TransitionSubresources: an image that was not registered");
    ImageInfo& info = it->second;
    // One barrier per run of layers in a mip that share their current and target layouts.
    std::vector<VkImageMemoryBarrier> barriers;
    for (uint32_t m = 0; m < info.mips; ++m) {
        for (uint32_t l = 0; l < info.layers;) {
            const size_t i = (size_t)m * info.layers + l;
            const VkImageLayout from = info.layouts[i];
            const VkImageLayout to = i < count ? targets[i] : VK_IMAGE_LAYOUT_UNDEFINED;
            uint32_t end = l + 1;
            while (end < info.layers && info.layouts[i + end - l] == from && (i + end - l < count ? targets[i + end - l] : VK_IMAGE_LAYOUT_UNDEFINED) == to) ++end;
            if (to != VK_IMAGE_LAYOUT_UNDEFINED && to != from) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.oldLayout = from;
                b.newLayout = to;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.subresourceRange = {FormatAspects(info.format), m, 1, l, end - l};
                b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                barriers.push_back(b);
                for (uint32_t k = l; k < end; ++k) info.layouts[(size_t)m * info.layers + k] = to;
            }
            l = end;
        }
    }
    if (!barriers.empty())
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                             (uint32_t)barriers.size(), barriers.data());
}

void TransitionAll(VkCommandBuffer cb, VkImage image, VkImageLayout layout) {
    auto it = images.find(image);
    if (it == images.end()) Fail("TransitionAll: an image that was not registered");
    const std::vector<VkImageLayout> targets(it->second.layouts.size(), layout);
    TransitionSubresources(cb, image, targets.data(), targets.size());
}

void UploadImage(VkImage image, const VkBufferImageCopy* regions, uint32_t regionCount, const void* data, size_t size) {
    Staging staging;
    if (!CreateStaging(size, staging)) Fail("no staging memory for an image upload");
    std::memcpy(staging.mapped, data, size);
    VkCommandBuffer cb = BeginOneTime();
    TransitionAll(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cb, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regionCount, regions);
    EndOneTime(cb);
    DestroyStaging(staging);
}

void UploadBuffer(VkBuffer buffer, VkDeviceSize offset, const void* data, size_t size) {
    Staging staging;
    if (!CreateStaging(size, staging)) Fail("no staging memory for a buffer upload");
    std::memcpy(staging.mapped, data, size);
    VkCommandBuffer cb = BeginOneTime();
    const VkBufferCopy copy{0, offset, size};
    vkCmdCopyBuffer(cb, staging.buffer, buffer, 1, &copy);
    EndOneTime(cb);
    DestroyStaging(staging);
}

void ReadbackImage(VkCommandBuffer cb, VkImage image, const char* name, VkImageAspectFlags aspect, uint32_t mip, uint32_t baseLayer,
                   uint32_t layers, VkExtent2D extent, VkImageLayout layout, VkSampleCountFlagBits samples, VkFormat format,
                   const void* captured, size_t capturedSize) {
    Readback r;
    r.name = name;
    r.format = format;
    r.aspect = aspect;
    r.width = extent.width;
    r.height = extent.height;
    r.layers = std::max(1u, layers);
    r.captured = static_cast<const uint8_t*>(captured);
    r.capturedSize = capturedSize;
    auto skip = [&](std::string why) {
        r.note = std::move(why);
        results.push_back(r);
    };
    if (samples != VK_SAMPLE_COUNT_1_BIT && layers > 1) { skip("not compared: a multisampled layered target"); return; }
    if (!CreateStaging(capturedSize, r.staging)) { skip("not compared: no staging memory"); return; }
    VkBufferImageCopy copy{};
    copy.imageExtent = {std::max(1u, extent.width), std::max(1u, extent.height), 1};
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
        auto it = images.find(image);
        if (it == images.end()) { DestroyStaging(r.staging); skip("not compared: the image was not registered"); return; }
        std::string why;
        const VkImage resolved = aspect == VK_IMAGE_ASPECT_COLOR_BIT ? ResolveTarget(cb, image, it->second, mip, baseLayer, layout, why)
                                                                      : ResolveDepth(cb, image, it->second, mip, baseLayer, layout, why);
        if (!resolved) { DestroyStaging(r.staging); skip("not compared: " + why); return; }
        copy.imageSubresource = {aspect, 0, 0, 1};
        vkCmdCopyImageToBuffer(cb, resolved, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.staging.buffer, 1, &copy);
    } else {
        const VkImageSubresourceRange range{FormatAspects(format), mip, 1, baseLayer, layers};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = range;
        b.oldLayout = layout;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        copy.imageSubresource = {aspect, mip, baseLayer, layers};
        vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.staging.buffer, 1, &copy);
        std::swap(b.oldLayout, b.newLayout);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    pending.push_back(std::move(r));
}

void SubmitAndWait(VkQueue q, const VkCommandBuffer* commandBuffers, uint32_t count) {
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = count;
    info.pCommandBuffers = commandBuffers;
    VK_CHECK(vkQueueSubmit(q, 1, &info, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(q));
}

void CompleteReadbacks() {
    for (Readback& r : pending) {
        const auto* replayed = static_cast<const uint8_t*>(r.staging.mapped);
        const size_t size = std::min<size_t>(r.capturedSize, (size_t)r.staging.size);
        // A 24-bit depth aspect is copied as 32 bits whose top byte is undefined.
        const bool d24 = r.aspect == VK_IMAGE_ASPECT_DEPTH_BIT && (r.format == VK_FORMAT_D24_UNORM_S8_UINT || r.format == VK_FORMAT_X8_D24_UNORM_PACK32);
        // The texel size from the capture's own copy: every layer's texels, one layer after another.
        const size_t texels = (size_t)r.width * r.height * r.layers;
        const uint32_t texel = texels ? std::max<uint32_t>(1, (uint32_t)(r.capturedSize / texels)) : 1;
        const uint32_t compared = d24 ? 3 : texel;
        r.compared = true;
        r.texels = size / texel;
        for (size_t t = 0; t + texel <= size; t += texel) {
            bool differs = false;
            for (uint32_t k = 0; k < compared; ++k) {
                const uint32_t delta = (uint32_t)std::abs((int)r.captured[t + k] - (int)replayed[t + k]);
                if (delta) {
                    differs = true;
                    r.maxByteDelta = std::max(r.maxByteDelta, delta);
                }
            }
            if (differs) ++r.differing;
        }
        r.replayed.assign(replayed, replayed + size);
        DestroyStaging(r.staging);
        results.push_back(std::move(r));
    }
    pending.clear();
    ReleaseTransients();
}

int ReportResults(const std::string& directory, bool writeImages) {
    size_t identical = 0, differing = 0, skipped = 0;
    if (writeImages && !results.empty()) std::filesystem::create_directories(directory);
    std::printf("render targets: %zu\n", results.size());
    for (const Readback& r : results) {
        if (r.layers > 1) std::printf("  %s (%ux%u, %u layers): ", r.name.c_str(), r.width, r.height, r.layers);
        else std::printf("  %s (%ux%u): ", r.name.c_str(), r.width, r.height);
        if (!r.compared) {
            ++skipped;
            std::printf("%s\n", r.note.c_str());
            continue;
        }
        if (r.differing == 0) {
            ++identical;
            std::printf("identical to the capture (%llu texels)\n", (unsigned long long)r.texels);
        } else {
            ++differing;
            std::printf("%llu of %llu texels differ from the capture, largest byte difference %u\n", (unsigned long long)r.differing,
                        (unsigned long long)r.texels, r.maxByteDelta);
        }
        if (!writeImages) continue;
        const std::string base = directory + "/" + r.name;
        std::ofstream raw(base + "_replayed.raw", std::ios::binary);
        raw.write((const char*)r.replayed.data(), (std::streamsize)r.replayed.size());
        // The PNGs show the first layer; the .raw file holds every one.
        std::vector<uint8_t> a, b;
        if (ToRgba(r, r.captured, r.capturedSize, a) && ToRgba(r, r.replayed.data(), r.replayed.size(), b)) {
            WritePng(base + "_captured.png", r.width, r.height, a);
            WritePng(base + "_replayed.png", r.width, r.height, b);
            if (r.differing) {
                std::vector<uint8_t> diff(a.size(), 255);
                const size_t texel = r.texels ? r.replayed.size() / r.texels : 4;
                const size_t shown = (size_t)r.width * r.height;
                for (size_t i = 0; i < shown && i * 4 < diff.size() && (i + 1) * texel <= r.replayed.size(); ++i) {
                    uint32_t delta = 0;
                    for (size_t k = 0; k < texel; ++k) delta = std::max<uint32_t>(delta, (uint32_t)std::abs((int)r.captured[i * texel + k] - (int)r.replayed[i * texel + k]));
                    diff[i * 4] = (uint8_t)std::min<uint32_t>(255, delta ? 64 + delta * 4 : 0);
                    diff[i * 4 + 1] = diff[i * 4 + 2] = 0;
                }
                WritePng(base + "_diff.png", r.width, r.height, diff);
            }
        }
    }
    if (writeImages && !results.empty()) std::printf("wrote the targets to %s/\n", directory.c_str());
    if (validationErrors) std::printf("validation errors: %zu\n", validationErrors);
    return differing == 0 && skipped == 0 ? 0 : 1;
}

void DestroySupport() {
    if (device) {
        vkDeviceWaitIdle(device);
        for (Readback& r : pending) DestroyStaging(r.staging);
        pending.clear();
        ReleaseTransients();
        for (auto& [key, pass] : depthResolvePasses)
            if (pass) vkDestroyRenderPass(device, pass, nullptr);
        depthResolvePasses.clear();
        for (VkDeviceMemory m : memories) vkFreeMemory(device, m, nullptr);
        memories.clear();
        if (utilityPool) vkDestroyCommandPool(device, utilityPool, nullptr);
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (messenger && vkDestroyDebugUtilsMessengerEXT) vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
}
