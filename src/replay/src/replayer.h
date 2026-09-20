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

#include <memory>

#include "arena.h"
#include "decode.h"
#include "gpucap.h"
#include "vk_decode.gen.h"
#include "xfb_patch.h"

namespace vkreplay {

class Exporter;

/**
 * A pipeline's stage given other code for the whole replay (--replace): a shader edited in GPU
 * Inspector and run in the captured frame instead of in the application. What the frame's targets
 * hold with it is compared with what the capture read back, which is the edit's effect.
 */
struct ShaderReplacement {
    uint64_t pipeline = 0;
    /** "vertex", "fragment", "compute", ...: the stage's name in the pipeline's blobs ("fragment:main"). */
    std::string stage;
    std::vector<uint32_t> words;
};

struct ReplayOptions {
    /** Enable the Khronos validation layer and report its messages. */
    bool validation = false;
    std::vector<ShaderReplacement> replacements;
    /**
     * Export to C++ (exporter.h): write the frame as a standalone C++ project into this directory
     * while it is replayed. Set for Setup as well as the frame, since the objects are exported as
     * they are created.
     */
    std::string exportDir;
    /** Create the device with every feature an analysis may use, for a replay that serves many (--serve). */
    bool allFeatures = false;
    /** Read back the render targets the capture read back and compare them with its copies (TargetComparison). */
    bool compareTargets = true;
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
        /**
         * Break a draw that put several fragments on the pixel into one entry per fragment
         * (PixelFragment). It costs a second replay of the frame, so it is only run when the first
         * one found such a draw, and `fragments = false` turns it off altogether.
         */
        bool fragments = true;
    } history;
    /** Time and count every draw of the frame with timestamps and pipeline statistics (DrawResult). */
    bool drawStats = false;
    /** Draw the pixels of named draws on their own, for the overlays of a render target (OverlayResult). */
    struct {
        bool enabled = false;
        /** Command indices of the draws wanted. */
        std::vector<uint32_t> commands;
        /** Also trace each draw's wireframe (needs the fillModeNonSolid feature). */
        bool wireframe = true;
    } overlay;
    /** Capture what the vertex shader wrote for named draws, through transform feedback (MeshResult). */
    struct {
        bool enabled = false;
        std::vector<uint32_t> commands;
    } mesh;
    /**
     * Time draws or dispatches again with variants of one shader stage, each with a part of the shader
     * taken out (AblationResult, ablation.cpp). The variants' SPIR-V comes with the request
     * (src/app/src/renderer/vulkan/spirv_ablate.ts writes it).
     */
    struct AblationVariant {
        std::string name;
        std::vector<uint32_t> words;
    };
    struct AblationTarget {
        uint32_t command = 0;
        /** "vertex", "fragment", "compute"... as StageName writes them. */
        std::string stage;
        /** Times the draw is issued between one pair of timestamps, so a cheap draw takes long enough to time; results are per draw. */
        uint32_t repeat = 1;
        std::vector<AblationVariant> variants;
    };
    struct {
        bool enabled = false;
        /** Timed rounds per target, each issuing the draw once with every variant; one more round warms up first. */
        uint32_t rounds = 5;
        std::vector<AblationTarget> targets;
    } ablation;
    /**
     * Hardware counters per pass and per draw (HwCounterReport, hw_counters.cpp): the GPU vendor's own
     * counters, read through NVIDIA's Nsight Perf SDK or VK_KHR_performance_query. The frame is replayed
     * once per collection pass the counters need.
     */
    struct {
        bool enabled = false;
        /** The counters wanted, by the names `list` prints; empty for the backend's default set. */
        std::vector<std::string> names;
        /** Only list every counter the device offers, without replaying. */
        bool list = false;
        /**
         * Also measure each draw, not only each render pass. A range per draw needs a second nesting
         * level (so about twice the collection passes) and profiling thousands of ranges is slow,
         * so a frame with many draws takes far longer; off by default.
         */
        bool perDraw = false;
        /**
         * Which backend to use: "nvperf", "khr", or empty to pick whichever the device supports.
         * Naming one that the device cannot use still runs its path far enough to say why, which is
         * how the portable path is exercised on a machine whose driver does not offer it.
         */
        std::string backend;
    } counters;
};

/** What Export to C++ wrote (ReplayOptions::exportDir). */
struct ExportReport {
    bool requested = false;
    std::string directory;
    /** Why nothing, or not everything, was written. */
    std::string error;
    std::vector<std::string> files;
    size_t objects = 0;
    size_t commands = 0;
    size_t submissions = 0;
    /** Render targets the exported program reads back and compares. */
    size_t targets = 0;
    /** Commands left out of the source, as the replay left them out. */
    size_t leftOut = 0;
    uint64_t dataBytes = 0;
    /** What the source could not spell. */
    std::vector<std::string> notes;
};

/** One hardware counter: the name the backend knows it by, and what it measures. */
struct HwCounterInfo {
    std::string name;
    std::string description;
    /** The unit the backend groups it under (NvPerf: the hardware unit; KHR: the counter's category). */
    std::string category;
    /** "percent", "ns", "bytes", "count", "ratio", "cycles", "hertz", "watts", "volts", "amps", "kelvin", "bytes/s". */
    std::string unit;
    /** Whether a draw's query may carry it (a KHR counter with render pass scope cannot). */
    bool perDraw = true;
};

/** The counters of one range: a render pass (`pass`), or a draw or dispatch. */
struct HwCounterRange {
    bool pass = false;
    uint32_t command = 0;          // the draw, or the pass's begin
    uint32_t frame = 0;
    uint64_t commandBuffer = 0;
    uint32_t passIndex = 0;        // UINT32_MAX for a dispatch outside a pass
    /** One per HwCounterReport::counters; NaN where the counter was not collected for this range. */
    std::vector<double> values;
};

struct HwCounterReport {
    bool requested = false;
    /** "nvperf" or "khr"; empty when no backend could run. */
    std::string backend;
    /** NvPerf: the chip the counters are for ("AD103"). */
    std::string chip;
    /** Times the frame was replayed to collect everything. */
    uint32_t rounds = 0;
    std::vector<HwCounterInfo> counters;
    std::vector<HwCounterRange> passes;
    std::vector<HwCounterRange> draws;
    /** With ReplayOptions::counters.list: everything the device offers. */
    std::vector<HwCounterInfo> available;
    std::vector<std::string> notes;
};

/** One pipeline issued at an ablation target: the draw's times with it, one per round. */
struct AblationTiming {
    std::string name;
    bool measured = false;
    /** The median of the rounds, and every round. */
    double ms = 0;
    std::vector<double> samples;
    std::string note;
};

/**
 * A draw or dispatch timed with variants of one of its shader stages. Each round issues the draw with
 * the unchanged shader (the baseline) and with every variant, between a pair of timestamps, right
 * before the draw itself runs; the variant's cost is the baseline's time less its own.
 */
struct AblationResult {
    uint32_t command = 0;
    std::string stage;
    uint64_t pipeline = 0;
    uint32_t frame = 0;
    uint64_t commandBuffer = 0;
    uint32_t passIndex = 0;
    uint32_t rounds = 0;
    uint32_t repeat = 1;
    AblationTiming baseline;
    std::vector<AblationTiming> variants;
    std::string note;
};

/**
 * One draw of the replayed frame, measured where it was issued: a timestamp before and after it,
 * and a pipeline statistics query around it.
 *
 * The GPU pipelines draws, so the spans of consecutive draws overlap and their sum runs longer than
 * the pass they are in: a draw's time says what share of the pass it is, not what it costs on its
 * own. The counters are exact.
 */
struct DrawResult {
    uint32_t command = 0;          // index in the capture's command list
    uint32_t frame = 0;
    uint64_t commandBuffer = 0;
    /** The render pass it is in; UINT32_MAX for a dispatch outside one. */
    uint32_t passIndex = 0;
    bool timed = false;
    double durationMs = 0;
    bool counted = false;
    /** The samples of this draw that passed the depth and stencil tests (an occlusion query). */
    bool sampled = false;
    uint64_t samplesPassed = 0;
    uint64_t vertexInvocations = 0;
    uint64_t primitives = 0;
    uint64_t fragmentInvocations = 0;
    uint64_t computeInvocations = 0;
};

/**
 * One event of a pixel's history. "load" is a pass starting from what it loaded or cleared; "clear"
 * is vkCmdClearAttachments; "draw" is a draw, with what its fragments at the pixel met, measured
 * with occlusion queries on a one-pixel scissor, in samples.
 */
/**
 * One fragment of a draw at the pixel: which primitive it came from, and what its fragment shader
 * wrote for it. Several fragments of one draw land on a pixel whenever its geometry overlaps there,
 * and the draw's own entry can only report the one that won (PixelEvent::primitive).
 *
 * The value is the shader's output for that fragment, not the pixel after it: the fragments are
 * measured with the depth and stencil tests off and no blending, so a fragment the tests would have
 * killed still says what it computed -- which is what "why is this pixel not what that draw writes"
 * needs. Whether it passed is the draw's own counts, which measure exactly that.
 */
struct PixelFragment {
    /** The primitive it came from: the draw's nth triangle (or line, or point). */
    int64_t primitive = -1;
    /** The fragment shader's output, in the target's format; empty when it could not be read. */
    std::vector<uint8_t> value;
};

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
    /**
     * The fragment shader declares EarlyFragmentTests, so the depth and stencil tests ran before it
     * rather than after: `shaded` is measured with those tests on, the way the hardware runs them,
     * and a fragment missing from it was either killed by a test or discarded.
     */
    bool earlyTests = false;
    uint32_t testsMeasured = 0;    // bit per measurement below that was taken
    uint64_t covered = 0;          // the draw's primitives cover the pixel (no culling, no tests)
    uint64_t facing = 0;           // ... with the pipeline's culling
    uint64_t shaded = 0;           // ... and its fragment shader (discards count)
    uint64_t depthPassed = 0;      // ... and the depth test alone
    uint64_t stencilPassed = 0;    // ... and the stencil test alone
    uint64_t passed = 0;           // ... and every test: what the draw wrote
    /**
     * The primitive of the draw whose fragment won the pixel, or -1 when it was not measured and
     * -2 when it was measured and no fragment of the draw wrote the pixel. A primitive index is the
     * draw's own: the nth triangle (or line, or point) it assembled.
     */
    int64_t primitive = -1;
    std::vector<uint8_t> value;    // the pixel's texel after the event
    std::vector<uint8_t> depth;    // the pass's depth texel after the event, when it has depth
    /** A draw's fragments at the pixel, one entry each, in the order it rasterized them. */
    std::vector<PixelFragment> fragments;
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
    /** Shader modules an edit created, destroyed once the copy exists. */
    std::vector<VkShaderModule> temporary;

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

/**
 * Where one draw of the frame landed, for the overlays of a render target: the draw issued on its own
 * into a target of its pass's size, with the pass's depth evolved up to it (vk_overlay.cpp does the
 * same in RenderDoc). `mask` holds, per pixel: bit 0 the draw rasterized a fragment here, bit 1 one
 * of its fragments passed the depth and stencil tests, bit 2 a line of its wireframe crosses here.
 */
struct OverlayResult {
    uint32_t command = 0;
    uint64_t commandBuffer = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    std::string method;
    uint32_t width = 0;
    uint32_t height = 0;
    /** Fragments the draw rasterized, over every pixel (its own overdraw included). */
    uint64_t fragments = 0;
    uint64_t pixelsCovered = 0;
    uint64_t pixelsPassed = 0;
    uint64_t pixelsRejected = 0;
    /**
     * Pixels the stencil test alone rejected, and pixels where the draw's culling left nothing: a
     * back-facing fragment landed there and no front-facing one did.
     */
    uint64_t pixelsStencilRejected = 0;
    uint64_t pixelsBackFacing = 0;
    /** Whether the wireframe bit was drawn, and whether the depth-tested bit means anything. */
    bool wireframe = false;
    bool depthTested = false;
    /** The stencil test alone was replayed (the pass has a stencil aspect), and the cull-off run was. */
    bool stencilTested = false;
    bool backFaceTested = false;
    std::vector<uint8_t> mask;
    std::string note;
};

/**
 * What one draw's vertex shader wrote, for the mesh output view: every vertex the draw assembled
 * (an indexed draw's vertices in index order, strips and fans as lists, every instance), each a
 * record of `stride` bytes with the outputs at their offsets. Captured with transform feedback
 * (mesh.cpp, xfb_patch.cpp).
 */
struct MeshResult {
    uint32_t command = 0;
    uint64_t commandBuffer = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    std::string method;
    /** The pipeline's primitive topology, as vk.xml names it. */
    std::string topology;
    uint32_t stride = 0;
    uint32_t vertices = 0;
    /** The buffer filled up: the draw wrote more than was captured. */
    bool truncated = false;
    std::vector<XfbOutput> outputs;
    std::vector<uint8_t> data;
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
    /** Images put back as the frame first read them (the capture's "initial" contents). */
    size_t initialImagesUploaded = 0;
    size_t bufferUploads = 0;
    std::vector<std::string> problems;
    std::vector<std::string> validation;
    std::vector<TargetComparison> targets;
    std::vector<OverdrawResult> overdraw;
    PixelHistoryResult history;
    /** With ReplayOptions::drawStats: every draw of the frame, in command order. */
    std::vector<DrawResult> draws;
    /** Why the draws carry no timings or no counters (a device without them). */
    std::string drawStatsNote;
    /** With ReplayOptions::overlay: where each named draw landed, in frame order; draws the replay did not reach last. */
    std::vector<OverlayResult> overlays;
    /** With ReplayOptions::mesh: each named draw's vertex shader outputs, in the same order. */
    std::vector<MeshResult> meshes;
    /** With ReplayOptions::ablation: each target's timings, in frame order; targets the replay did not reach last. */
    std::vector<AblationResult> ablations;
    /** With ReplayOptions::counters: the vendor's counters per pass and per draw. */
    HwCounterReport counters;
    /** With ReplayOptions::exportDir: the project written. */
    ExportReport exported;
};

class Replayer {
public:
    Replayer();
    ~Replayer();
    Replayer(const Replayer&) = delete;
    Replayer& operator=(const Replayer&) = delete;

    /** Replays the capture; false when it could not start (no Vulkan, no device). The report says what happened either way. */
    bool Run(const CaptureFile& capture, const ReplayOptions& options, ReplayReport& report);

    /**
     * The two halves of Run, for a replay kept alive between analyses: Setup creates the device and
     * the capture's objects once; RunFrame replays the frame with an analysis, as often as asked.
     * Each frame after the first starts from the state the first did: command pools reset, images
     * back in their initial layouts with their contents cleared and the sampled textures uploaded
     * again, and every buffer range the frame binds uploaded again before its submission.
     * `options` of RunFrame pick the analysis; the device features are Setup's.
     */
    bool Setup(const CaptureFile& capture, const ReplayOptions& options, ReplayReport& report);
    void RunFrame(const ReplayOptions& options, ReplayReport& report);

private:
    struct ImageRecord {
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent3D extent{};
        uint32_t mips = 1;
        uint32_t layers = 1;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        /** STORAGE usage: a shader can write it, so what the frame left in it is worth comparing. */
        bool storage = false;
        /** Per subresource, mip * layers + layer: the layout outside the frame's own command buffers. */
        std::vector<VkImageLayout> layouts;
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
        /** Views a subpass renders at most (multiview): a query inside takes one index per view. */
        uint32_t views = 1;
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
    /** How a reissued draw is drawn: the counting copy, depth and stencil only, or its wireframe. */
    /**
     * How a draw is re-issued for an overlay or a measurement: counting its fragments, moving only
     * the depth and stencil the draw after it tests against, its edges as lines, its outputs through
     * transform feedback, its fragments that the stencil test alone kept, or the ones its own cull
     * mode would have thrown away.
     */
    enum class ReissueMode { Count, DepthOnly, Wireframe, Xfb, StencilOnly, BackFace };
    struct PendingOverdraw {
        Staging staging;
        size_t result = 0;
    };
    /** One draw's vertex shader outputs waiting for its submission: the feedback buffer and its counter. */
    struct PendingMesh {
        Staging buffer;
        Staging counter;
        size_t result = 0;
        uint64_t pipeline = 0;
        /** Vertices the draw's arguments say it assembles, three times over for strips and fans; 0 for indirect draws. */
        uint64_t estimate = 0;
    };
    /** One draw's overlay waiting for its submission: a staging buffer per variant drawn. */
    struct PendingOverlay {
        Staging rasterized;
        Staging passed;
        Staging wireframe;
        /** The stencil test on its own, and the faces the draw's cull mode threw away. */
        Staging stencilPassed;
        Staging backFacing;
        size_t result = 0;
    };
    /** A pass's pixel history waiting for its submission: the pixel after each event, and the queries of each draw. */
    struct PendingHistory {
        struct Entry {
            size_t event = 0;
            uint32_t slot = 0;
            int32_t queryBase = -1;
            uint32_t issued = 0;     // bit per query variant issued
            /** The slot the primitive id was written into, or none for an event without one. */
            int32_t idSlot = -1;
        };
        Staging staging;
        /** One pixel of the target's format, for a multisampled target whose samples are resolved into it. */
        TransientImage resolve;
        /** Where the primitive ids are read: one 32-bit slot per draw event. */
        Staging ids;
        uint32_t idSlots = 0;
        uint32_t nextId = 0;
        /** The pass-sized target the primitive id is drawn into, and the framebuffer over it. */
        TransientImage idTarget;
        /**
         * The depth and stencil the primitive-id pass tests against: a copy of the pass's own,
         * refreshed before each draw. It is the draw's to write, the way the draw writes the pass's,
         * so that the primitive left in the pixel is the one that really won it -- against the depth
         * the draw's earlier fragments put there, not only against the depth it started from.
         */
        TransientImage idDepthCopy;
        VkFramebuffer idFramebuffer = VK_NULL_HANDLE;
        /** The attachment whose copy holds the depth and stencil the id pass tests against, or none. */
        int idDepth = -1;
        VkQueryPool queries = VK_NULL_HANDLE;
        uint32_t queryCount = 0;
        uint32_t nextQuery = 0;
        /**
         * The fragment round (history.cpp): a colour target of the followed attachment's format and
         * a depth-stencil of the replay's own, whose stencil counts the draw's fragments so that one
         * of them at a time is let through.
         */
        TransientImage fragColor;
        TransientImage fragDepth;
        /** The followed attachment's format, which the fragment round's colour target takes. */
        VkFormat fragFormat = VK_FORMAT_UNDEFINED;
        VkFramebuffer fragColorFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer fragIdFramebuffer = VK_NULL_HANDLE;
        /** Where a fragment's value and primitive are read: one texel and one 32-bit slot each. */
        Staging fragValues;
        Staging fragIds;
        uint32_t fragSlots = 0;
        uint32_t nextFrag = 0;
        /** Per measured fragment: the event it belongs to, and the slot its value and id went into. */
        struct FragmentEntry {
            size_t event = 0;
            uint32_t index = 0;
            uint32_t slot = 0;
        };
        std::vector<FragmentEntry> fragmentEntries;
        uint32_t targetTexel = 0;
        uint32_t depthTexel = 0;
        uint32_t nextSlot = 0;
        uint32_t slots = 0;          // room the staging was made for
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
    /** A module from one of an object's SPIR-V payloads; `code` and `size`, when given, get the payload's bytes. */
    VkShaderModule ModuleFromBlob(const JValue& object, const std::string& blobName, const uint8_t** code = nullptr, size_t* size = nullptr);
    uint64_t CreatePipeline(const JValue& object, std::string_view cmd, uint32_t index, const JValue& args, size_t unresolvedBefore);
    /** A VkShaderEXT (VK_EXT_shader_object), from its payload's SPIR-V, made unlinked. */
    uint64_t CreateShaderObject(const JValue& object, uint32_t index, const JValue& args, size_t unresolvedBefore);

    void ComputeInitialLayouts();
    /** Sampled images, then what images held when the frame first read them. */
    void UploadImageContents();
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
    /**
     * Adds to a copy what the graphics pipeline libraries a create info links hold (libraries linked from
     * libraries included): their stages, with code from `linked`'s payloads or the library's own, and the
     * state each library's flags say it holds. False when the create info links none, or one is missing.
     */
    bool MergeLibraries(const JValue& linked, const JValue& info, PipelineCopy& p, std::vector<VkShaderModule>& temporary, int depth = 0);
    /**
     * A state member of a captured graphics pipeline's create info ("pInputAssemblyState"), from the pipeline or,
     * for one linked from libraries, the library holding `part` (VK_GRAPHICS_PIPELINE_LIBRARY_*: "VERTEX_INPUT_INTERFACE",
     * "PRE_RASTERIZATION_SHADERS", "FRAGMENT_SHADER_BIT", "FRAGMENT_OUTPUT_INTERFACE"); null when none sets it.
     */
    const JValue* PipelineState(uint64_t pipelineId, std::string_view member, std::string_view part) const;
    /** Whether a captured graphics pipeline, or a library it links, declares a dynamic state ("VK_DYNAMIC_STATE_SCISSOR"). */
    bool PipelineDynamic(uint64_t pipelineId, std::string_view state) const;
    /** The fragment shader that writes 1.0 (overdraw counts, and coverage without discards). */
    VkShaderModule CountModule();
    /** The fragment shader of the back-face overlay (util.h, kBackFaceFragmentSpirv). */
    VkShaderModule BackFaceModule();
    // --- The fragment round (history.cpp) -------------------------------------------------------
    /**
     * A render pass of the replay's own for measuring one fragment: a colour attachment of `format`
     * cleared to nothing, and a depth-stencil whose stencil counts the draw's fragments.
     */
    VkRenderPass HistoryFragmentRenderPass(VkFormat format);
    /**
     * A copy of the draw that lets exactly the fragment whose index is the stencil reference write:
     * every fragment increments the stencil, and only the one that finds its own index passes.
     * `idPass` replaces the fragment shader with the one writing gl_PrimitiveID.
     */
    VkPipeline HistoryFragmentPipeline(uint64_t pipelineId, VkFormat format, bool idPass);
    /** The images and staging the fragment round reads through; false when they could not be made. */
    bool PrepareHistoryFragments(PendingHistory& pending, const PassState& pass, uint32_t slots);
    /** A depth-stencil format with a stencil aspect this device supports, for the fragment counter. */
    VkFormat HistoryFragmentDepthFormat();
    VkFormat _historyFragmentDepthFormat = VK_FORMAT_UNDEFINED;
    /** Whether any draw the first replay measured put more than one fragment on the pixel. */
    bool HistoryHasMultipleFragments() const;
    /** Whether the frame is being replayed again to break its draws into fragments. */
    bool _historyFragmentRound = false;
    /** The event each draw of the fragment round belongs to, keyed as the events were recorded. */
    std::map<std::tuple<uint32_t, uint64_t, uint32_t, uint32_t>, size_t> _historyEventIndex;
    std::map<std::pair<uint64_t, VkFormat>, VkPipeline> _historyFragmentPipelines;
    std::map<std::pair<uint64_t, VkFormat>, VkPipeline> _historyFragmentIdPipelines;
    std::map<VkFormat, VkRenderPass> _historyFragmentRenderPasses;

    /** The fragment shader of the primitive-id pass (util.h, kPrimitiveIdFragmentSpirv). */
    VkShaderModule PrimitiveIdModule();

    // Pixel history (history.cpp)
    VkPipeline HistoryPipeline(uint64_t pipelineId, int variant);
    /** Whether a captured pipeline's fragment shader asks for the depth and stencil tests before it. */
    bool HistoryEarlyFragmentTests(uint64_t pipelineId);
    /**
     * The pixel history's primitive-id pass: which primitive of a draw wrote the pixel. The draw is
     * issued again with its fragment shader replaced by one writing gl_PrimitiveID into a target of
     * the replay's own, tested against the depth and stencil the event starts from, so what remains
     * in the pixel is the primitive of the fragment that won it.
     */
    VkRenderPass HistoryIdRenderPass(VkFormat depthFormat);
    VkPipeline HistoryIdPipeline(uint64_t pipelineId, VkFormat depthFormat);
    VkRenderPass HistoryRenderPass(uint64_t renderPassId);
    ScissorInfo PipelineScissor(uint64_t pipelineId);
    void PrepareHistory(VkCommandBuffer cb, PassState& pass, std::vector<PendingHistory>& histories);
    /**
     * What a command outside a render pass does to the image the pixel history follows. A clear, a
     * copy, a blit or a resolve says in its own arguments which image it writes, where, and in what
     * layout, so it is known before the command runs; a dispatch or a trace writes through a
     * descriptor, which no argument names, so those are watched instead — the pixel is read before
     * and after, and the event is kept only when it changed.
     */
    struct DirectWrite {
        bool writes = false;
        const char* kind = "";                              // the event's kind: "clear", "copy", "blit", "resolve", "compute"
        std::string detail;                                 // what it was, for the event
        VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;     // the layout the image is in around the command
    };
    DirectWrite HistoryDirectWrite(const std::string& method, const JValue& args);
    /** The pixel read straight from the followed image, into the group's direct-write staging. */
    void HistoryDirectPixel(VkCommandBuffer cb, const CommandGroup& group, const DirectWrite& write,
                            std::vector<PendingHistory>& histories, uint32_t index, const std::string& method, uint32_t frame);
    /**
     * Keeps whether each bound descriptor set holds the followed image as a storage image, per
     * pipeline bind point, so a dispatch or a trace that could have written the pixel is known.
     */
    void NoteHistoryBindings(const JValue* descriptors);
    void RecordHistory(VkCommandBuffer cb, const CommandGroup& group, PassState& pass, uint32_t endIndex, std::vector<PendingHistory>& histories);
    uint32_t CopyHistoryPixel(VkCommandBuffer cb, const PassState& pass, PendingHistory& pending, VkImageLayout layout);
    void CompleteHistory(std::vector<PendingHistory>& histories);

    // Overdraw
    TransientImage CreateTransientImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                                        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void ReleaseTransients();
    VkRenderPass OverdrawRenderPass(VkFormat depthFormat);
    VkPipeline OverdrawPipeline(uint64_t pipelineId, bool depthTested, VkFormat depthFormat, ReissueMode mode = ReissueMode::Count);
    void PrepareOverdraw(VkCommandBuffer cb, PassState& pass);
    void RecordOverdraw(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex, std::vector<PendingOverdraw>& pending);
    void ReissueCommand(VkCommandBuffer cb, uint32_t index, bool depthTested, VkFormat depthFormat, bool insidePass);
    /**
     * Issues a pass's state and commands again, secondaries inline, into a render pass and framebuffer of the
     * replay's own; or, with no render pass, into dynamic rendering to `colour` (which shader objects need).
     */
    void ReissuePass(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex, bool depthTested,
                     VkFormat depthFormat, VkRenderPass renderPass, VkFramebuffer framebuffer, VkImageView colour = VK_NULL_HANDLE);
    /** Whether a draw's vertex stage is a shader object (vkCmdBindShadersEXT) rather than a pipeline's. */
    bool DrawUsesShaderObjects(const CommandGroup& group, uint32_t target) const;
    void CompleteOverdraw(std::vector<PendingOverdraw>& pending);

    // Draw-call overlays (overlay.cpp): one draw of a pass issued on its own into a mask.
    /** Whether one of `commands` is in the pass beginning at `beginIndex` (its secondaries included). */
    bool PassHoldsAny(uint32_t beginIndex, const std::vector<uint32_t>& commands) const;
    void RecordOverlay(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex);
    /** Draws one variant of an overlay into a count target and stages it; false when the draw could not be drawn. */
    bool DrawOverlayVariant(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex, uint32_t target,
                            ReissueMode mode, bool depthTested, Staging& out);
    /** Reads the submission's overlays back into their masks; `submitted` false drops them. */
    void CompleteOverlay(bool submitted);

    // Mesh output (mesh.cpp): one draw issued again with its vertex shader writing transform feedback.
    void RecordMesh(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex);
    /** At the draw a mesh is for, once its pipeline copy is bound: the feedback buffers; false without them. */
    bool PrepareMeshBuffers();
    void CompleteMesh(bool submitted);
    /** The topology a pipeline's create info names, and whether it is dynamic. */
    std::string PipelineTopology(uint64_t pipelineId, bool& dynamic) const;
    /** A vertex shader object's copy that writes transform feedback (its layout in _xfbLayouts); null when it cannot be made. */
    VkShaderEXT FeedbackShader(uint64_t shaderId);
    bool PrepareDrawStats();
    void ResetDrawQueries(VkCommandBuffer cb);
    void DestroyDrawStats();
    /** Starts this draw's queries; the slot it took, or -1 when there is no room left. */
    /** Begins a draw's queries; the index to end them with, -1 when the pools are full. */
    int BeginDrawQuery(VkCommandBuffer cb, uint32_t command, uint32_t frame, uint64_t commandBuffer, uint32_t passIndex);
    void EndDrawQuery(VkCommandBuffer cb, int pending);
    /** Reads the submission's results; `submitted` false drops them (a submission that never ran). */
    void CompleteDrawStats(bool submitted);
    void Barrier(VkCommandBuffer cb, VkImage image, const VkImageSubresourceRange& range, VkImageLayout from, VkImageLayout to);

    /** What a command buffer (or a secondary) has bound so far, which an ablation issues its draw again with. */
    struct StreamState {
        uint64_t graphicsPipeline = 0;
        uint64_t computePipeline = 0;
        /** The commands that set the depth write enable and the stencil write mask, restored after an ablation changes them. */
        std::vector<uint32_t> depthWriteCommands;
        std::vector<uint32_t> stencilWriteCommands;
    };
    static void NoteStreamCommand(StreamState& stream, const std::string& method, const JValue& args, uint32_t index);

    // Ablation (ablation.cpp): a draw issued again with variants of a shader stage, timed.
    bool PrepareAblation();
    /** Resets the query ranges of the targets a command buffer holds (resets are not allowed inside a pass). */
    void ResetAblationQueries(VkCommandBuffer cb, const CommandGroup& group);
    /** At a draw or dispatch the request names: every variant issued and timed, then the command buffer's state put back. */
    void IssueAblation(VkCommandBuffer cb, uint32_t index, const std::string& method, const JValue& args, uint32_t frame, uint64_t commandBuffer,
                       uint32_t passIndex, const StreamState& stream);
    VkPipeline AblationPipeline(uint64_t pipelineId, size_t target, int variant, bool compute);
    void CompleteAblation(bool submitted);
    void DestroyAblation();

    // Hardware counters (hw_counters.cpp): the vendor's counters around every pass and every draw,
    // collected over as many replays of the frame as they need.
    /** Sets the backend up for the frame; false (with the report's notes saying why) when none can run. */
    bool PrepareCounters();
    /** The KHR path of PrepareCounters: a performance query pool over the frame's draws. */
    bool PrepareKhrCounters(uint32_t draws);
    /** Collection passes the configured counters need, so the round loop knows where to stop. */
    uint32_t CounterRounds() const;
    /** The limiter metrics collected when the request names none (NvPerf's spelling). */
    std::vector<std::string> DefaultCounterNames() const;
    /** Lists what the device offers into the report (ReplayOptions::counters.list). */
    void ListCounters();
    /** Starts a collection round: false when the backend failed (the report says why). */
    bool BeginCounterRound();
    /** Ends the round after its submissions completed; true when another round is needed. */
    bool EndCounterRound();
    void BeginCounterPass(VkCommandBuffer cb, const PassState& pass);
    void EndCounterPass(VkCommandBuffer cb);
    int BeginCounterDraw(VkCommandBuffer cb, uint32_t command, uint32_t frame, uint64_t commandBuffer, uint32_t passIndex);
    void EndCounterDraw(VkCommandBuffer cb, int range);
    /** Turns what the rounds collected into the report. */
    void CompleteCounters();
    void DestroyCounters();

    /** `frame`, `commandBuffer` and `passIndex` are the primary's, for the draws measured inside. */
    void RecordSecondaries(size_t executeIndex, const JValue& execute, uint32_t frame, uint64_t commandBuffer, uint32_t passIndex);
    void ApplyBufferData(const CommandGroup& group);
    void ApplyDescriptorSnapshot(const JValue* descriptors);
    /** One snapshot set as writes (to `handle`, or to a pushed set when null), with the storage they point into. */
    struct DescriptorWrites {
        std::vector<VkWriteDescriptorSet> writes;
        std::vector<std::unique_ptr<std::vector<VkDescriptorBufferInfo>>> buffers;
        std::vector<std::unique_ptr<std::vector<VkDescriptorImageInfo>>> images;
        std::vector<std::unique_ptr<std::vector<VkBufferView>>> views;
        std::vector<std::unique_ptr<std::vector<VkAccelerationStructureKHR>>> structures;
        std::vector<std::unique_ptr<VkWriteDescriptorSetAccelerationStructureKHR>> structureWrites;
        std::string key;   // the contents, to skip rewriting a set with what it holds
    };
    void BuildDescriptorWrites(const JValue& set, VkDescriptorSet handle, DescriptorWrites& out);
    /** Issues a captured command: a push through an update template is pushed from its snapshot. */
    /**
     * `index` is the command's index in the capture when it is issued as part of the frame, which is what
     * Export to C++ writes; an analysis issuing a command again (overdraw, pixel history) passes none.
     */
    void IssueCommand(ReplayFn fn, const JValue& command, const JValue& args, VkCommandBuffer cb, uint32_t index = UINT32_MAX);
    void BeginPass(const JValue& command, uint32_t index, uint64_t commandBuffer);
    void BeginDynamicPass(const JValue& command, uint32_t index, uint64_t commandBuffer, VkCommandBuffer cb);
    /** Copies the pass's captured targets for comparison; with a reason, only reports them as not compared. */
    void InjectReadbacks(VkCommandBuffer cb, const PassState& pass, std::vector<PendingReadback>& readbacks, const char* skipReason = nullptr);
    /**
     * Reads back the images a shader may have written, at the end of a command buffer's recording.
     * Render targets are compared at their pass's end; an image a trace or a dispatch wrote is not
     * a target of any pass, so without this nothing the replay computes into one is ever checked.
     */
    void InjectStorageReadbacks(VkCommandBuffer cb, const CommandGroup& group, std::vector<PendingReadback>& readbacks);
    void CompareReadbacks(std::vector<PendingReadback>& readbacks);

    bool CreateStaging(VkDeviceSize size, Staging& staging, VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    void DestroyStaging(Staging& staging);
    /** Allocates memory for requirements, preferring `want`; tracked memory is freed with the device, untracked is the caller's. */
    bool AllocateBound(VkMemoryRequirements requirements, VkMemoryPropertyFlags want, VkDeviceMemory& memory, bool track = true,
                       bool deviceAddress = false);
    bool RunOneTime(const std::function<void(VkCommandBuffer)>& record);
    void UploadToBuffer(VkBuffer buffer, VkDeviceSize offset, const uint8_t* data, size_t size);
    /** Moves each subresource of an image to its target (UNDEFINED: left where it is), from the layouts the record holds. */
    void TransitionSubresources(VkCommandBuffer cb, ImageRecord& image, const std::vector<VkImageLayout>& targets);
    void TransitionAll(VkCommandBuffer cb, ImageRecord& image, VkImageLayout to);
    /** A multisampled target resolved into a single-sampled copy the size of its mip, which the caller copies from; TRANSFER_SRC_OPTIMAL. */
    VkImage ResolveTarget(VkCommandBuffer cb, const ImageRecord& image, VkImageAspectFlags aspect, uint32_t mip, uint32_t baseLayer,
                          VkImageLayout layout, std::string& why);
    void Problem(std::string message);
    uint64_t Handle(uint64_t id) const;

    /** A device address the capture recorded, as one in this process (see the definition). */
    VkDeviceAddress RemapAddress(uint64_t bufferId, uint64_t offset);
    /** Working space for acceleration structure builds, grown as they need it. */
    /**
     * A stretch of build scratch for one build command, as a device address in `address`.
     *
     * Scratch holds no input, so the replay makes its own rather than carrying the capture's — but
     * it cannot hand every build the same memory from offset 0. Two builds recorded into one
     * submission may run with nothing ordering them, which is legal when the application gave each
     * its own scratch, and sharing would introduce a hazard the frame never had. So each
     * reservation takes a fresh stretch, reset only when the next submission starts recording.
     */
    bool ReserveScratch(VkDeviceSize size, VkDeviceAddress& address);
    /** Replays one vkCmdBuildAccelerationStructuresKHR with its addresses remapped. */
    void BuildAccelerationStructures(const JValue& command, const JValue& args, VkCommandBuffer cb);
    /** Replays one vkCmdTraceRaysKHR, rebuilding its binding table with this driver's handles. */
    void TraceRays(const JValue& command, const JValue& args, VkCommandBuffer cb);
    bool EnsureBindingTable(VkDeviceSize size);
    /**
     * Rewrites the bottom-level references in a top level's instance buffer to this process's.
     * Every instance names its bottom level by the *captured* device address, which names nothing
     * here, so a top level built from the buffer as captured references structures that do not
     * exist. Returns false when the instances could not be patched, which leaves the build out.
     */
    bool PatchInstanceReferences(uint64_t bufferId, uint64_t offset, uint32_t captureId, uint32_t instances);
    /** This process's address for the structure the captured address named, or 0. */
    VkDeviceAddress RemapStructureAddress(uint64_t capturedAddress);
    void Track(const std::string& type, uint64_t handle);
    void DestroyAll();
    /** Puts the frame's state back where the first frame found it (RunFrame, after the first). */
    void ResetFrameState();

    const CaptureFile* _capture = nullptr;
    ReplayOptions _options;
    ReplayReport* _report = nullptr;

    void* _library = nullptr;
    VkFunctions _fns{};
    /** Build scratch, handed out a stretch at a time within a submission (ReserveScratch). */
    VkBuffer _scratch = VK_NULL_HANDLE;
    VkDeviceMemory _scratchMemory = VK_NULL_HANDLE;
    VkDeviceSize _scratchSize = 0;
    /** How much of it this submission has given out; reset when the next one starts recording. */
    VkDeviceSize _scratchUsed = 0;
    /** Buffers outgrown mid-submission, kept alive because recorded builds hold their addresses. */
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> _retiredScratch;
    /** The replay's own shader binding table, written with this driver's group handles. */
    VkBuffer _bindingTable = VK_NULL_HANDLE;
    VkDeviceMemory _bindingTableMemory = VK_NULL_HANDLE;
    uint8_t* _bindingTableMapped = nullptr;
    VkDeviceSize _bindingTableSize = 0;
    /** The pipeline the last vkCmdBindPipeline bound at the ray tracing bind point. */
    uint64_t _boundRayTracingPipeline = 0;
    /** Captured acceleration structure device address -> its object id (built on first use). */
    std::unordered_map<uint64_t, uint64_t> _structureAddresses;
    bool _structureAddressesBuilt = false;
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
    /** Export to C++: the project being written while the capture replays, and the context its commands are decoded with. */
    std::unique_ptr<Exporter> _exporter;
    DecodeContext _exportCtx{_arena};
    /** What the exported source says about the image CreateImage is making (a swapchain's). */
    std::string _exportComment;
    std::unordered_map<uint64_t, uint64_t> _handles;    // tracker id -> replay handle
    std::unordered_set<uint64_t> _skipped;              // ids left out on purpose
    std::unordered_map<uint64_t, ImageRecord> _images;
    std::unordered_map<uint64_t, ViewRecord> _views;
    std::unordered_map<uint64_t, BufferRecord> _buffers;
    std::unordered_map<uint64_t, RenderPassRecord> _renderPasses;
    std::unordered_map<uint64_t, std::vector<uint64_t>> _framebufferViews;
    // What the exported program shows in its window: the swapchain image the frame wrote last, which
    // is what it presented, else its last colour target (a renderer that never presents).
    std::unordered_set<uint64_t> _swapchainImages;
    uint64_t _lastSwapchainWrite = 0;
    uint64_t _lastColorTarget = 0;
    void NoteImageWrite(uint64_t image, bool colorTarget);
    /** Export to C++: what the window shows, and RestoreFrame (command pools reset, images back in their first layouts). */
    void ExportFrameEnd();

    /** Per image, per subresource as in ImageRecord::layouts: the first layout the frame expects it in (UNDEFINED: none). */
    std::unordered_map<uint64_t, std::vector<VkImageLayout>> _initialLayouts;
    /**
     * The same walk's other end: the layout the frame leaves each subresource in (UNDEFINED: it names
     * none). Export to C++ runs the frame in a loop, and RestoreFrame moves each image from here back
     * to its initial layout.
     */
    std::unordered_map<uint64_t, std::vector<VkImageLayout>> _finalLayouts;
    /** Render passes resolving a multisampled depth target (sample zero), by format and sample count. */
    std::map<std::pair<VkFormat, VkSampleCountFlagBits>, VkRenderPass> _depthResolvePasses;
    std::unordered_map<uint64_t, std::string> _descriptorContents;  // set id -> contents last written
    std::unordered_map<uint64_t, const JValue*> _bufferData;        // capture data id -> buffers entry
    std::vector<CommandGroup> _groups;
    std::vector<Created> _created;
    std::vector<VkDeviceMemory> _memories;

    // Overdraw
    std::unordered_map<uint64_t, VkExtent2D> _framebufferExtents;
    VkShaderModule _countModule = VK_NULL_HANDLE;
    VkShaderModule _backFaceModule = VK_NULL_HANDLE;
    std::map<std::tuple<uint64_t, bool, VkFormat, ReissueMode>, VkPipeline> _overdrawPipelines;
    std::map<VkFormat, VkRenderPass> _overdrawRenderPasses;
    std::vector<TransientImage> _transientImages;
    std::vector<VkFramebuffer> _transientFramebuffers;
    /** Whether a counting pipeline is bound in the overdraw pass being recorded (draws are left out otherwise). */
    bool _overdrawDrawable = false;
    uint32_t _overdrawDraws = 0;
    uint32_t _overdrawSkippedDraws = 0;

    // Draw-call overlays: while one is being recorded, the draw it is for and how the rest are drawn.
    /** The command index of the draw the overlay is for; UINT32_MAX when no overlay is being recorded. */
    uint32_t _overlayTarget = UINT32_MAX;
    /** Leave every other draw out (the variants that do not need the pass's depth). */
    bool _overlayOnlyTarget = false;
    /** How the target draw itself is drawn. */
    ReissueMode _overlayTargetMode = ReissueMode::Count;
    /** The pipeline the reissued commands last bound, which the target draw needs a copy of. */
    uint64_t _overlayPipeline = 0;
    /**
     * VK_EXT_shader_object: the vertex shader object the reissued commands last bound in place of a
     * pipeline, whether a tessellation or geometry shader object is bound beside it, and the topology
     * the dynamic state last set (and had at the target draw, once it is issued).
     */
    uint64_t _overlayVertexShader = 0;
    bool _overlayShaderGeometry = false;
    std::string _overlayTopology;
    std::string _overlayDrawnTopology;
    /** Set once the target draw has been issued: nothing after it is. */
    bool _overlayIssued = false;
    /**
     * Whether the target draw was issued with its pipeline copy, and which captured pipeline that
     * copied: kept apart from the per-secondary state, which the pass's later secondaries reset.
     */
    bool _overlayDrawn = false;
    uint64_t _overlayDrawnPipeline = 0;
    bool _wireframeAvailable = false;
    /** The submission's overlays, waiting for it to complete. */
    std::vector<PendingOverlay> _pendingOverlays;

    // Setup, kept for every frame run after it
    bool _setupDone = false;
    bool _frameRun = false;
    /** What Setup reported (device, objects, its problems), copied into each frame's report. */
    ReplayReport _setupReport;
    /** Setup's options: the device features, validation and tracing stay as they were created. */
    ReplayOptions _setupOptions;

    // Mesh output
    bool _xfbAvailable = false;
    /** The edited vertex shader of each pipeline copied for feedback: its record layout, or why it could not be edited. */
    std::map<uint64_t, XfbPatch> _xfbLayouts;
    /** Vertex shader objects edited for feedback, by captured shader object; null where the edit failed. */
    std::map<uint64_t, VkShaderEXT> _xfbShaders;
    /** The mesh being recorded, whose buffers the target draw binds. */
    PendingMesh* _meshTarget = nullptr;
    std::vector<PendingMesh> _pendingMeshes;

    // Per-draw timing and counters: a timestamp pair and a statistics query per draw, reset at the
    // start of each submission's recording and read once the submission has completed.
    VkQueryPool _drawTimestamps = VK_NULL_HANDLE;
    VkQueryPool _drawStatistics = VK_NULL_HANDLE;
    VkQueryPool _drawOcclusion = VK_NULL_HANDLE;
    uint32_t _drawQueryCapacity = 0;
    uint32_t _drawSlot = 0;
    /** Nanoseconds per timestamp tick; 0 where the queue cannot write timestamps. */
    double _timestampPeriod = 0;
    bool _drawCountersAvailable = false;
    bool _drawSamplesAvailable = false;
    /** The submission's draws, in slot order, waiting for its results. */
    std::vector<DrawResult> _pendingDraws;
    /** Each pending draw's first query and how many it takes: one per view in a multiview pass. */
    std::vector<std::pair<uint32_t, uint32_t>> _pendingDrawSlots;
    /** Views the pass being recorded renders (1 outside multiview). */
    uint32_t _passViews = 1;
    /** Queries the capture's own commands have open: a statistics query cannot nest inside one. */
    uint32_t _appQueryDepth = 0;

    // Ablation
    VkQueryPool _ablationPool = VK_NULL_HANDLE;
    /** Per target of the request: its first query, and the command index it is at. */
    std::vector<uint32_t> _ablationBase;
    std::unordered_map<uint32_t, size_t> _ablationTargets;
    /** Copies by captured pipeline, target and variant (-1 the baseline), kept between frames. */
    std::map<std::tuple<uint64_t, size_t, int>, VkPipeline> _ablationPipelines;
    /** Whether a captured pipeline sets its depth write enable and stencil write mask dynamically. */
    std::unordered_map<uint64_t, std::pair<bool, bool>> _ablationDynamic;
    /** The submission's ablations: the report's entry, and which pipeline of each round was issued. */
    struct PendingAblation {
        size_t result = 0;
        uint32_t base = 0;
        std::vector<bool> issued;
    };
    std::vector<PendingAblation> _pendingAblations;

    // Hardware counters
    /** The backend's state while counters are collected (hw_counters.cpp owns the type). */
    struct HwCounterState* _hw = nullptr;
    /** The device has VK_KHR_performance_query with performanceCounterQueryPools enabled. */
    bool _perfQueryAvailable = false;
    /** NVIDIA's Nsight Perf SDK loaded, and its extensions enabled on the instance and device. */
    bool _nvperfReady = false;
    std::string _nvperfNote;
    /** Chained into each of the frame's submissions during a collection round (the KHR pass index). */
    const void* _submitNext = nullptr;
    /** Rounds are only counters': the report's other results come from the first replay. */
    uint32_t _hwRound = 0;

    // Pixel history
    std::map<std::pair<uint64_t, int>, VkPipeline> _historyPipelines;
    std::map<uint64_t, VkRenderPass> _historyRenderPasses;
    std::map<uint64_t, ScissorInfo> _pipelineScissors;
    size_t _historyPasses = 0;
    /** Writes to the followed image outside a render pass, for the note when nothing touched it. */
    size_t _historyWrites = 0;
    /** The command buffer's direct-write pending record (an index into the group's histories), or none yet. */
    int _historyDirect = -1;
    /** Per pipeline bind point, per bound set index: it holds the followed image as a storage image. */
    std::map<std::string, std::map<uint32_t, bool>> _historyStorageBinds;
    /** Whether a pipeline's fragment shader declares EarlyFragmentTests, read from its SPIR-V once. */
    std::map<uint64_t, bool> _historyEarlyTests;
    /** The primitive-id render passes by depth format, and the pipelines that draw into them. */
    std::map<VkFormat, VkRenderPass> _historyIdRenderPasses;
    std::map<std::pair<uint64_t, VkFormat>, VkPipeline> _historyIdPipelines;
    VkShaderModule _primitiveIdModule = VK_NULL_HANDLE;
    /** The geometryShader feature is enabled, without which gl_PrimitiveID cannot be read. */
    bool _primitiveIdAvailable = false;
};

} // namespace vkreplay
