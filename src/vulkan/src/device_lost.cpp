#include "device_lost.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "capture.h"
#include "layer.h"
#include "json_writer.h"
#include "transport.h"
#include "vk_commands.gen.h"

namespace vkinsp
{

namespace
{

/**
 * Marker slots in the breadcrumb buffer. Two per device is all the diagnosis needs: the last action
 * the GPU began, and the last one it finished. A ring of every action in flight would say more, but
 * these two already separate "this command hung" from "the hang came after it".
 */
constexpr uint32_t kSlotBegun = 0;
constexpr uint32_t kSlotEnded = 1;
constexpr VkDeviceSize kBufferSize = 2 * sizeof(uint32_t);

/**
 * What each ordinal was, so a marker read back after the loss can be named. Ordinals are handed out
 * in recording order and wrap around this ring; a hang in a frame with more actions than this
 * leaves the oldest unnamed, which the report says rather than guessing.
 */
constexpr uint32_t kRingSize = 1u << 16;

} // namespace

/** Per-device breadcrumb state, owned by DeviceData through a pointer so layer.h stays light. */
struct Breadcrumbs
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    volatile uint32_t* markers = nullptr;   // host-visible and coherent: the GPU writes, we read
    std::atomic<uint32_t> nextOrdinal{1};   // 0 means "nothing yet", so ordinals start at 1
    /** Per ordinal (modulo the ring): the command and the buffer it was recorded into. */
    struct Entry
    {
        uint32_t cmdId = 0;
        uint64_t commandBuffer = 0;
    };
    std::vector<Entry> ring;
    std::atomic<bool> reported{false};      // every later call fails the same way
    /** VKINSP_SIMULATE_DEVICE_LOST: report a loss after this many submissions (0: never). */
    uint32_t simulateAfter = 0;
    /** ":hung": also report the last action as unfinished, the shape of a real hang. */
    bool simulateHung = false;
    std::atomic<uint32_t> submissions{0};
};

void PlanBreadcrumbs(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info, BreadcrumbSetup& setup)
{
    if (!ConfigFlag("VKINSP_BREADCRUMBS"))
        return;
    if (!inst || !inst->dispatch.EnumerateDeviceExtensionProperties)
        return;
    setup.wanted = true;
    uint32_t count = 0;
    inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    if (count)
        inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, available.data());
    const bool has = std::any_of(available.begin(), available.end(), [](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, VK_AMD_BUFFER_MARKER_EXTENSION_NAME) == 0;
    });
    if (!has)
    {
        Log("device-lost breadcrumbs: this device has no %s, so a hang cannot be traced to a command",
            VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
        return;
    }
    setup.extensionNames.assign(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);
    const bool already = std::any_of(setup.extensionNames.begin(), setup.extensionNames.end(), [](const char* e) {
        return std::strcmp(e, VK_AMD_BUFFER_MARKER_EXTENSION_NAME) == 0;
    });
    if (!already)
        setup.extensionNames.push_back(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
    info.ppEnabledExtensionNames = setup.extensionNames.data();
    info.enabledExtensionCount = (uint32_t)setup.extensionNames.size();
    setup.added = true;
}

void CreateBreadcrumbs(DeviceData* dev, const BreadcrumbSetup& setup)
{
    if (!setup.added || !dev->dispatch.CmdWriteBufferMarkerAMD)
    {
        if (setup.wanted && !setup.added)
            Log("device-lost breadcrumbs: not enabled on this device");
        return;
    }
    auto crumbs = std::make_unique<Breadcrumbs>();

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = kBufferSize;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dev->dispatch.CreateBuffer(dev->device, &bi, nullptr, &crumbs->buffer) != VK_SUCCESS)
    {
        Log("device-lost breadcrumbs: could not create the marker buffer");
        return;
    }
    VkMemoryRequirements req{};
    dev->dispatch.GetBufferMemoryRequirements(dev->device, crumbs->buffer, &req);
    // Host visible and coherent, and ideally not device-local: the point is to read it after the
    // device has gone, so it must live where a lost device cannot take it with it.
    uint32_t typeIndex = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < dev->memoryProperties.memoryTypeCount; ++i)
    {
        if (!(req.memoryTypeBits & (1u << i)))
            continue;
        if ((dev->memoryProperties.memoryTypes[i].propertyFlags & want) != want)
            continue;
        typeIndex = i;
        if (!(dev->memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            break;
    }
    if (typeIndex == UINT32_MAX)
    {
        Log("device-lost breadcrumbs: no host-visible memory for the marker buffer");
        dev->dispatch.DestroyBuffer(dev->device, crumbs->buffer, nullptr);
        return;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = typeIndex;
    if (dev->dispatch.AllocateMemory(dev->device, &ai, nullptr, &crumbs->memory) != VK_SUCCESS ||
        dev->dispatch.BindBufferMemory(dev->device, crumbs->buffer, crumbs->memory, 0) != VK_SUCCESS)
    {
        Log("device-lost breadcrumbs: could not allocate the marker buffer");
        if (crumbs->memory)
            dev->dispatch.FreeMemory(dev->device, crumbs->memory, nullptr);
        dev->dispatch.DestroyBuffer(dev->device, crumbs->buffer, nullptr);
        return;
    }
    void* mapped = nullptr;
    if (dev->dispatch.MapMemory(dev->device, crumbs->memory, 0, kBufferSize, 0, &mapped) != VK_SUCCESS)
    {
        Log("device-lost breadcrumbs: could not map the marker buffer");
        dev->dispatch.FreeMemory(dev->device, crumbs->memory, nullptr);
        dev->dispatch.DestroyBuffer(dev->device, crumbs->buffer, nullptr);
        return;
    }
    // Left mapped for the life of the device: after a loss there is no unmapping to be done, and the
    // markers have to be readable exactly then.
    std::memset(mapped, 0, kBufferSize);
    crumbs->markers = static_cast<volatile uint32_t*>(mapped);
    crumbs->ring.resize(kRingSize);
    if (const std::string v = ConfigValue("VKINSP_SIMULATE_DEVICE_LOST"); !v.empty())
    {
        crumbs->simulateAfter = (uint32_t)strtoul(v.c_str(), nullptr, 10);
        crumbs->simulateHung = v.find(":hung") != std::string::npos;
        Log("device-lost breadcrumbs: simulating a device loss after %u submissions%s", crumbs->simulateAfter,
            crumbs->simulateHung ? ", with the last action reported unfinished" : "");
    }
    dev->breadcrumbs = crumbs.release();
    Log("device-lost breadcrumbs: on (%s)", VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
}

void DestroyBreadcrumbs(DeviceData* dev)
{
    Breadcrumbs* crumbs = dev->breadcrumbs;
    if (!crumbs)
        return;
    dev->breadcrumbs = nullptr;
    // The memory stays mapped; freeing it unmaps it.
    if (crumbs->memory)
        dev->dispatch.FreeMemory(dev->device, crumbs->memory, nullptr);
    if (crumbs->buffer)
        dev->dispatch.DestroyBuffer(dev->device, crumbs->buffer, nullptr);
    delete crumbs;
}

bool BreadcrumbsEnabled(const DeviceData* dev) { return dev && dev->breadcrumbs != nullptr; }

uint32_t BeginBreadcrumb(DeviceData* dev, VkCommandBuffer cb, uint32_t cmdId)
{
    Breadcrumbs* crumbs = dev ? dev->breadcrumbs : nullptr;
    if (!crumbs)
        return 0;
    const uint32_t ordinal = crumbs->nextOrdinal.fetch_add(1);
    Breadcrumbs::Entry& e = crumbs->ring[ordinal % kRingSize];
    e.cmdId = cmdId;
    e.commandBuffer = (uint64_t)(uintptr_t)cb;
    // Top of pipe: written as the command is reached, before any of its work runs.
    dev->dispatch.CmdWriteBufferMarkerAMD(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, crumbs->buffer,
        kSlotBegun * sizeof(uint32_t), ordinal);
    return ordinal;
}

void EndBreadcrumb(DeviceData* dev, VkCommandBuffer cb, uint32_t ordinal)
{
    Breadcrumbs* crumbs = dev ? dev->breadcrumbs : nullptr;
    if (!crumbs || !ordinal)
        return;
    // Bottom of pipe: written only once the command's work has completed.
    dev->dispatch.CmdWriteBufferMarkerAMD(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, crumbs->buffer,
        kSlotEnded * sizeof(uint32_t), ordinal);
}

bool SimulateDeviceLost(DeviceData* dev, bool* pretendIncomplete)
{
    Breadcrumbs* crumbs = dev ? dev->breadcrumbs : nullptr;
    if (!crumbs || !crumbs->simulateAfter)
        return false;
    if (crumbs->submissions.fetch_add(1) + 1 != crumbs->simulateAfter)
        return false;
    if (pretendIncomplete)
        *pretendIncomplete = crumbs->simulateHung;
    return true;
}

namespace
{

/** "vkCmdDraw in command buffer 0x...", or what is known of it. */
std::string DescribeOrdinal(Breadcrumbs* crumbs, uint32_t ordinal, uint32_t newest)
{
    if (!ordinal)
        return "nothing (the GPU had not reached a recorded action)";
    // The ring only holds the most recent kRingSize ordinals; anything older has been overwritten.
    if (newest >= ordinal && newest - ordinal >= kRingSize)
    {
        return "action #" + std::to_string(ordinal) + " (too long ago to name: more than " +
            std::to_string(kRingSize) + " actions were recorded after it)";
    }
    const Breadcrumbs::Entry& e = crumbs->ring[ordinal % kRingSize];
    // kVkCommandNames already carries the "vk" prefix.
    std::string name = e.cmdId < (uint32_t)VkCmdId::Count ? kVkCommandNames[e.cmdId] : "an action";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)e.commandBuffer);
    return name + " (action #" + std::to_string(ordinal) + " in command buffer " + buf + ")";
}

} // namespace

void OnDeviceLost(DeviceData* dev, const char* call, bool pretendIncomplete)
{
    Breadcrumbs* crumbs = dev ? dev->breadcrumbs : nullptr;
    // Without breadcrumbs there is still something worth saying: which call reported it, and how to
    // get the rest next time.
    static std::atomic<bool> reportedWithout{false};
    if (!crumbs)
    {
        if (reportedWithout.exchange(true))
            return;
        Log("VK_ERROR_DEVICE_LOST from %s. The GPU stopped responding; the command that hung is not "
            "known because device-lost breadcrumbs were off. Run again with VKINSP_BREADCRUMBS=1 to "
            "have the GPU record how far it got.",
            call);
        JsonWriter w;
        w.BeginObject();
        w.Key("action");
        w.String("DeviceLost");
        w.Key("call");
        w.String(call);
        w.Key("breadcrumbs");
        w.Boolean(false);
        w.Key("message");
        w.String("The GPU stopped responding. Run with device-lost breadcrumbs on to learn which command it was executing.");
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
        return;
    }
    if (crumbs->reported.exchange(true))
        return;

    const uint32_t begun = crumbs->markers[kSlotBegun];
    // `pretendIncomplete` is the simulation reaching the branch a real hang takes: the GPU is idle
    // when the simulation fires, so without it the markers always read as "everything finished".
    const uint32_t ended = pretendIncomplete && begun ? begun - 1 : crumbs->markers[kSlotEnded];
    const uint32_t newest = crumbs->nextOrdinal.load();
    const std::string begunText = DescribeOrdinal(crumbs, begun, newest);
    const std::string endedText = DescribeOrdinal(crumbs, ended, newest);
    const bool hung = begun != 0 && begun != ended;

    std::string message;
    if (hung)
    {
        message = "The GPU stopped responding while running " + begunText +
            ". It last finished " + endedText + ".";
    }
    else if (begun)
    {
        message = "The GPU stopped responding after finishing " + begunText +
            ", so the hang is in what came next rather than in that command.";
    }
    else
    {
        message = "The GPU stopped responding before reaching any recorded action.";
    }
    Log("VK_ERROR_DEVICE_LOST from %s. %s", call, message.c_str());

    JsonWriter w;
    w.BeginObject();
    w.Key("action");
    w.String("DeviceLost");
    w.Key("call");
    w.String(call);
    w.Key("breadcrumbs");
    w.Boolean(true);
    w.Key("lastBegun");
    w.Uint(begun);
    w.Key("lastCompleted");
    w.Uint(ended);
    w.Key("hungCommand");
    w.String(hung ? begunText : std::string());
    w.Key("lastCompletedCommand");
    w.String(endedText);
    w.Key("message");
    w.String(message);
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

} // namespace vkinsp
