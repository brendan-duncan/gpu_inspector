// The replay engine: re-creates a capture's objects on this machine's GPU and re-executes its
// command buffers in submission order.
//
// What a capture gives it today (docs/REPLAY.md, stage 1):
//   * every object the frame references, with its creation arguments: re-created in id order,
//     which is creation order. Swapchain images become ordinary images of the swapchain's format
//     and size; device memory is not re-created as such, every image and buffer gets memory of
//     its own; shader code comes from the SPIR-V payloads the capture keeps with modules and
//     pipelines; render passes store every attachment so the result of each pass can be read.
//   * the frame's commands, decoded by the generated recorders (gen/vk_decode.gen.cpp).
//   * the contents it read back: sampled textures are uploaded before the frame, each bound
//     buffer range before the submission of the command buffer that binds it, and every
//     descriptor set is written from the snapshot taken when it was bound.
//
// Every render target the capture read back at the end of a pass is read back by the replay at
// the same point and compared byte for byte: the measure of how faithful the replay is.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "arena.h"
#include "decode.h"
#include "gpucap.h"
#include "vk_decode.gen.h"

namespace vkreplay {

struct ReplayOptions {
    /** Enable the Khronos validation layer and report its messages. */
    bool validation = false;
    /** Keep the captured and replayed pixels of every compared target in the report (for --dump). */
    bool keepPixels = false;
    /** Print every object and command to stderr before it is replayed (to find what a driver crashes on). */
    bool trace = false;
};

/** A render target the capture read back at the end of a pass, and how the replay's copy compares. */
struct TargetComparison {
    uint64_t image = 0;
    uint64_t commandBuffer = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    uint32_t attachment = 0;
    std::string format;
    std::string aspect;
    uint32_t width = 0;
    uint32_t height = 0;
    bool compared = false;
    /** Why it was not compared, or what the comparison found. */
    std::string note;
    uint64_t bytes = 0;
    uint64_t differingTexels = 0;
    uint64_t texels = 0;
    uint32_t maxByteDelta = 0;
    /** With ReplayOptions::keepPixels: both copies' bytes, in the read-back layout. */
    std::vector<uint8_t> captured;
    std::vector<uint8_t> replayed;
};

struct ReplayReport {
    std::string device;
    size_t objectsCreated = 0;
    size_t objectsSkipped = 0;
    size_t commandsRecorded = 0;
    size_t submissions = 0;
    size_t texturesUploaded = 0;
    size_t bufferUploads = 0;
    std::vector<std::string> problems;
    std::vector<std::string> validation;
    std::vector<TargetComparison> targets;
};

class Replayer {
public:
    Replayer();
    ~Replayer();
    Replayer(const Replayer&) = delete;
    Replayer& operator=(const Replayer&) = delete;

    /** Replays the capture; false when it could not start (no Vulkan, no device). The report says what happened either way. */
    bool Run(const CaptureFile& capture, const ReplayOptions& options, ReplayReport& report);

private:
    struct ImageRecord {
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent3D extent{};
        uint32_t mips = 1;
        uint32_t layers = 1;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    struct ViewRecord {
        uint64_t image = 0;
        VkImageSubresourceRange range{};
    };
    struct BufferRecord {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };
    struct RenderPassRecord {
        std::vector<VkImageLayout> initialLayouts;
        std::vector<VkImageLayout> finalLayouts;
        std::vector<VkAttachmentLoadOp> loadOps;
    };
    struct Staging {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };
    struct PendingReadback {
        Staging staging;
        size_t target = 0;
        const JValue* texture = nullptr;
    };
    /** One command buffer's recording in the command list: its begin to its end. */
    struct CommandGroup {
        uint64_t commandBuffer = 0;
        uint32_t first = 0;
        uint32_t last = 0;
        bool used = false;
    };
    struct PassState {
        bool active = false;
        uint32_t index = 0;
        uint32_t frame = 0;
        uint64_t commandBuffer = 0;
        std::vector<uint64_t> views;
        std::vector<VkImageLayout> layouts;
        std::vector<uint64_t> resolveViews;
        std::vector<VkImageLayout> resolveLayouts;
    };
    struct Created {
        std::string type;
        uint64_t handle;
    };

    bool LoadVulkan();
    bool CreateInstance();
    bool CreateDevice();
    void CreateObjects();
    void CreateObject(const JValue& object);
    uint64_t CreateImage(uint64_t id, const VkImageCreateInfo& info);
    uint64_t CreateBuffer(uint64_t id, const VkBufferCreateInfo& info);
    VkShaderModule ModuleFromBlob(const JValue& object, const std::string& blobName);
    uint64_t CreatePipeline(const JValue& object, std::string_view cmd, uint32_t index, const JValue& args, size_t unresolvedBefore);

    void ComputeInitialLayouts();
    void UploadSampledTextures();
    void TransitionToInitialLayouts();
    void ReplayCommands();
    void BuildGroups();
    void RecordGroup(CommandGroup& group, std::vector<PendingReadback>& readbacks);
    void RecordSecondaries(size_t executeIndex, const JValue& execute);
    void ApplyBufferData(const CommandGroup& group);
    void ApplyDescriptorSnapshot(const JValue* descriptors);
    void BeginPass(const JValue& command, uint32_t index, uint64_t commandBuffer);
    void BeginDynamicPass(const JValue& command, uint32_t index, uint64_t commandBuffer, VkCommandBuffer cb);
    /** Copies the pass's captured targets for comparison; with a reason, only reports them as not compared. */
    void InjectReadbacks(VkCommandBuffer cb, const PassState& pass, std::vector<PendingReadback>& readbacks, const char* skipReason = nullptr);
    void CompareReadbacks(std::vector<PendingReadback>& readbacks);

    bool CreateStaging(VkDeviceSize size, Staging& staging);
    void DestroyStaging(Staging& staging);
    bool AllocateBound(VkMemoryRequirements requirements, VkMemoryPropertyFlags want, VkDeviceMemory& memory);
    bool RunOneTime(const std::function<void(VkCommandBuffer)>& record);
    void UploadToBuffer(VkBuffer buffer, VkDeviceSize offset, const uint8_t* data, size_t size);
    void Transition(VkCommandBuffer cb, const ImageRecord& image, VkImageLayout from, VkImageLayout to);
    void Problem(std::string message);
    uint64_t Handle(uint64_t id) const;
    void Track(const std::string& type, uint64_t handle);
    void DestroyAll();

    const CaptureFile* _capture = nullptr;
    ReplayOptions _options;
    ReplayReport* _report = nullptr;

    void* _library = nullptr;
    VkFunctions _fns{};
    VkInstance _instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT _messenger = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;
    VkDevice _device = VK_NULL_HANDLE;
    uint32_t _queueFamily = 0;
    VkQueue _queue = VK_NULL_HANDLE;
    VkCommandPool _utilityPool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties _memoryProperties{};
    bool _hasSwapchainExtension = false;

    Arena _arena;
    DecodeContext _ctx{_arena};
    std::unordered_map<uint64_t, uint64_t> _handles;    // tracker id -> replay handle
    std::unordered_set<uint64_t> _skipped;              // ids left out on purpose
    std::unordered_map<uint64_t, ImageRecord> _images;
    std::unordered_map<uint64_t, ViewRecord> _views;
    std::unordered_map<uint64_t, BufferRecord> _buffers;
    std::unordered_map<uint64_t, RenderPassRecord> _renderPasses;
    std::unordered_map<uint64_t, std::vector<uint64_t>> _framebufferViews;
    std::unordered_map<uint64_t, VkImageLayout> _initialLayouts;
    std::unordered_map<uint64_t, std::string> _descriptorContents;  // set id -> contents last written
    std::unordered_map<uint64_t, const JValue*> _bufferData;        // capture data id -> buffers entry
    std::vector<CommandGroup> _groups;
    std::vector<Created> _created;
    std::vector<VkDeviceMemory> _memories;
};

} // namespace vkreplay
