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

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <tuple>
#include <utility>
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
    /** Measure every render pass's overdraw (OverdrawResult). */
    bool overdraw = false;
    /** Follow one pixel of one image through the frame (PixelHistoryResult). */
    struct {
        bool enabled = false;
        uint64_t image = 0;
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t mip = 0;
        uint32_t layer = 0;
    } history;
};

/**
 * One event of a pixel's history. "load" is a pass starting from what it loaded or cleared; "clear"
 * is vkCmdClearAttachments; "draw" is a draw, with what its fragments at the pixel met, measured
 * with occlusion queries on a one-pixel scissor, in samples.
 */
struct PixelEvent {
    std::string kind;
    uint32_t command = 0;          // the command's index in the capture (the pass's begin for "load")
    std::string method;
    std::string detail;            // "load": the attachment's load op
    uint64_t commandBuffer = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    uint64_t pipeline = 0;
    bool scissored = false;        // the pixel is outside the draw's scissor
    uint32_t testsMeasured = 0;    // bit per measurement below that was taken
    uint64_t covered = 0;          // the draw's primitives cover the pixel (no culling, no tests)
    uint64_t facing = 0;           // ... with the pipeline's culling
    uint64_t shaded = 0;           // ... and its fragment shader (discards count)
    uint64_t depthPassed = 0;      // ... and the depth test alone
    uint64_t stencilPassed = 0;    // ... and the stencil test alone
    uint64_t passed = 0;           // ... and every test: what the draw wrote
    std::vector<uint8_t> value;    // the pixel's texel after the event
    std::vector<uint8_t> depth;    // the pass's depth texel after the event, when it has depth
};

struct PixelHistoryResult {
    bool requested = false;
    uint64_t image = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t mip = 0;
    uint32_t layer = 0;
    std::string format;
    std::string depthFormat;
    std::vector<PixelEvent> events;
    std::vector<std::string> notes;
};

/** A copy of a captured graphics pipeline being made: its create info and the state it points at, for an edit to change. */
struct PipelineCopy {
    VkGraphicsPipelineCreateInfo info{};
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    VkPipelineMultisampleStateCreateInfo multisample{};
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    VkPipelineColorBlendStateCreateInfo blend{};
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
    std::vector<VkDynamicState> dynamic;
    bool hasRasterization = false;
    bool hasMultisample = false;
    bool hasDepthStencil = false;
    bool hasBlend = false;
    bool hasDynamic = false;

    /** Replaces the fragment stage (adds one to a pipeline without). */
    void ReplaceFragment(VkShaderModule module);
    void RemoveDynamic(std::initializer_list<VkDynamicState> states);
    void AddDynamic(VkDynamicState state);
    bool HasDynamic(VkDynamicState state) const;
};

/**
 * The overdraw of one render pass: how many fragments landed on each pixel when the pass's draws
 * were replayed with a fragment shader that counts, into a target of the pass's size.
 */
struct OverdrawResult {
    uint64_t commandBuffer = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    /** true: the fragments that passed the pass's depth and stencil tests, in draw order, from the depth the pass started with; false: every rasterized fragment. */
    bool depthTested = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t fragments = 0;
    uint64_t coveredPixels = 0;
    uint32_t maxCount = 0;
    /** Draws issued with a counting pipeline, and draws left out because their pipeline could not be copied. */
    uint32_t draws = 0;
    uint32_t skippedDraws = 0;
    /** The fragment shader invocations the capture's pipeline statistics measured for the pass; -1 without. */
    int64_t capturedFragments = -1;
    /** Pixels by count: 1, 2, 3, 4, 5-8, 9-16, 17-32, 33 and more. */
    std::array<uint64_t, 8> histogram{};
    /** The count of every pixel, row by row. */
    std::vector<uint16_t> counts;
    std::string note;
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
    std::vector<OverdrawResult> overdraw;
    PixelHistoryResult history;
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
        std::vector<VkFormat> formats;
        /** The first subpass's depth/stencil attachment, -1 without. */
        int depthAttachment = -1;
    };
    struct TransientImage {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
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
        // Overdraw: where the pass begins, its size, and the depth it starts from.
        uint32_t beginIndex = 0;
        VkExtent2D extent{};
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        uint64_t depthImage = 0;
        VkImageSubresourceRange depthRange{};
        VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        VkImageLayout depthLayoutBefore = VK_IMAGE_LAYOUT_UNDEFINED;
        VkClearDepthStencilValue depthClear{1.0f, 0};
        TransientImage overdrawDepth;
        bool overdraw = false;
        // Pixel history: what each attachment starts from, and the copies the pass is replayed into.
        uint64_t renderPass = 0;                 // 0 for dynamic rendering
        std::vector<VkFormat> formats;
        std::vector<VkAttachmentLoadOp> loadOps;
        std::vector<VkImageLayout> startLayouts;
        std::vector<VkClearValue> clearValues;
        std::vector<int> dynamicColorSlots;      // dynamic rendering: the attachment of each color slot, -1 for none
        int dynamicDepth = -1;
        int dynamicStencil = -1;
        std::vector<TransientImage> shadows;
        int historyAttachment = -1;
        int historyDepthAttachment = -1;
        bool history = false;
        size_t historyPending = 0;
    };
    struct PendingOverdraw {
        Staging staging;
        size_t result = 0;
    };
    /** A pass's pixel history waiting for its submission: the pixel after each event, and the queries of each draw. */
    struct PendingHistory {
        struct Entry {
            size_t event = 0;
            uint32_t slot = 0;
            int32_t queryBase = -1;
            uint32_t issued = 0;     // bit per query variant issued
        };
        Staging staging;
        VkQueryPool queries = VK_NULL_HANDLE;
        uint32_t queryCount = 0;
        uint32_t nextQuery = 0;
        uint32_t targetTexel = 0;
        uint32_t depthTexel = 0;
        uint32_t nextSlot = 0;
        std::vector<Entry> entries;
    };
    /** Whether a captured pipeline's scissor is dynamic, and its static scissor otherwise. */
    struct ScissorInfo {
        bool dynamic = true;
        bool withCount = false;
        bool hasRect = false;
        VkRect2D rect{};
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
    void RecordGroup(CommandGroup& group, std::vector<PendingReadback>& readbacks, std::vector<PendingOverdraw>& overdraws,
                     std::vector<PendingHistory>& histories);
    /** Decodes a command's arguments and says whether every handle in them resolves, reporting nothing. */
    bool ArgsResolve(const std::string& method, const JValue& args);

    // Pipeline copies (pipeline_copy.cpp)
    /** Copies a captured graphics pipeline through `edit` (false: no copy); null, with a problem naming `purpose`, when it cannot. */
    VkPipeline CopyGraphicsPipeline(uint64_t pipelineId, const std::string& purpose, const std::function<bool(PipelineCopy&)>& edit);
    /** The fragment shader that writes 1.0 (overdraw counts, and coverage without discards). */
    VkShaderModule CountModule();

    // Pixel history (history.cpp)
    VkPipeline HistoryPipeline(uint64_t pipelineId, int variant);
    VkRenderPass HistoryRenderPass(uint64_t renderPassId);
    ScissorInfo PipelineScissor(uint64_t pipelineId);
    void PrepareHistory(VkCommandBuffer cb, PassState& pass, std::vector<PendingHistory>& histories);
    void RecordHistory(VkCommandBuffer cb, const CommandGroup& group, PassState& pass, uint32_t endIndex, std::vector<PendingHistory>& histories);
    uint32_t CopyHistoryPixel(VkCommandBuffer cb, const PassState& pass, PendingHistory& pending, VkImageLayout layout);
    void CompleteHistory(std::vector<PendingHistory>& histories);

    // Overdraw
    TransientImage CreateTransientImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                                        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void ReleaseTransients();
    VkRenderPass OverdrawRenderPass(VkFormat depthFormat);
    VkPipeline OverdrawPipeline(uint64_t pipelineId, bool depthTested, VkFormat depthFormat);
    void PrepareOverdraw(VkCommandBuffer cb, PassState& pass);
    void RecordOverdraw(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex, std::vector<PendingOverdraw>& pending);
    void ReissueCommand(VkCommandBuffer cb, uint32_t index, bool depthTested, VkFormat depthFormat, bool insidePass);
    void CompleteOverdraw(std::vector<PendingOverdraw>& pending);
    void Barrier(VkCommandBuffer cb, VkImage image, const VkImageSubresourceRange& range, VkImageLayout from, VkImageLayout to);
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
    /** Allocates memory for requirements, preferring `want`; tracked memory is freed with the device, untracked is the caller's. */
    bool AllocateBound(VkMemoryRequirements requirements, VkMemoryPropertyFlags want, VkDeviceMemory& memory, bool track = true);
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

    // Overdraw
    std::unordered_map<uint64_t, VkExtent2D> _framebufferExtents;
    VkShaderModule _countModule = VK_NULL_HANDLE;
    std::map<std::tuple<uint64_t, bool, VkFormat>, VkPipeline> _overdrawPipelines;
    std::map<VkFormat, VkRenderPass> _overdrawRenderPasses;
    std::vector<TransientImage> _transientImages;
    std::vector<VkFramebuffer> _transientFramebuffers;
    /** Whether a counting pipeline is bound in the overdraw pass being recorded (draws are left out otherwise). */
    bool _overdrawDrawable = false;
    uint32_t _overdrawDraws = 0;
    uint32_t _overdrawSkippedDraws = 0;

    // Pixel history
    std::map<std::pair<uint64_t, int>, VkPipeline> _historyPipelines;
    std::map<uint64_t, VkRenderPass> _historyRenderPasses;
    std::map<uint64_t, ScissorInfo> _pipelineScissors;
    size_t _historyPasses = 0;
};

} // namespace vkreplay
