// The Metal replay engine: re-creates a Metal capture's objects on this machine's GPU and
// re-encodes its command buffers in commit order, the counterpart of the Vulkan replay
// (src/replay/src/replayer.h) and the Direct3D 12 one (src/d3d12/replay/src/dx_replayer.h).
// docs/REPLAY.md, "Metal".
//
// What a capture gives it:
//   * every object with the arguments it was created from, re-created in id order. A drawable's
//     texture becomes an ordinary render target, since there is no window here, and the
//     presentDrawable: that would have shown it is left out. A library takes its Metal Shading
//     Language, or its metallib bytes, from the blobs the capture keeps with it.
//   * the frame's commands in encoding order, each naming its command buffer and its encoder
//     (CaptureCommand.encoder), which is what the encoders are opened and closed from. A parallel
//     encoder's sub-encoder has no recorded endEncoding — its end is not the pass's — so it is
//     closed when the stream moves off it.
//   * the contents it read back: the textures a draw sampled, uploaded before the frame, and each
//     buffer range a command bound, written before the command buffer that binds it is committed.
//
// Every render target the capture read back at the end of a pass is read back here at the same
// point and compared byte for byte. A multisampled attachment is read through its resolve, which
// is what the capture read (QueueAttachmentCapture in src/metal/src/capture.mm).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#import <Metal/Metal.h>

#include "gpucap.h"
#include "json.h"

#include "mtl_decode.h"

namespace mtlreplay {

class MtlExporter;
using vkreplay::CaptureFile;
using vkreplay::JValue;

struct MtlReplayOptions {
    /** Turn Metal's API validation on for this process and report what it says. */
    bool validate = false;
    bool compareTargets = true;
    /** Keep both copies of every compared target in the report (--dump). */
    bool keepPixels = false;
    /** Print every object and command to stderr before it is replayed. */
    bool trace = false;
    /** Export to C++: write the frame as a standalone project into this directory while it replays. */
    std::string exportDir;
};

struct MtlTargetComparison {
    uint64_t texture = 0;
    uint64_t commandBuffer = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    uint32_t attachment = 0;
    std::string format;
    std::string aspect;
    uint32_t width = 0;
    uint32_t height = 0;
    bool compared = false;
    /** Nothing defined to compare: the capture's own read-back failed, or the pass discards the
     *  target. Not a failure of the replay. */
    bool undefined = false;
    std::string note;
    uint64_t differingTexels = 0;
    uint64_t texels = 0;
    uint32_t maxByteDelta = 0;
    std::vector<uint8_t> captured;
    std::vector<uint8_t> replayed;
};

struct MtlExportReport {
    std::string directory;
    std::string error;
    size_t objects = 0, commands = 0, submissions = 0, targets = 0, leftOut = 0;
    uint64_t dataBytes = 0;
    std::vector<std::string> files;
    std::vector<std::string> notes;
};

struct MtlReplayReport {
    std::string device;
    std::string capturedDevice;
    size_t objects = 0, commands = 0, submissions = 0;
    std::vector<std::string> problems;
    std::vector<MtlTargetComparison> comparisons;
    MtlExportReport exported;
};

class MtlReplayer {
public:
    MtlReplayer(const CaptureFile& capture, MtlReplayOptions options);
    ~MtlReplayer();
    MtlReplayer(const MtlReplayer&) = delete;
    MtlReplayer& operator=(const MtlReplayer&) = delete;

    /** Re-creates, replays and compares. False when the replay could not run at all. */
    bool Run(MtlReplayReport& report);

private:
    // ---- object graph
    void CreateObjects();
    void CreateObject(const JValue& object);
    id<MTLTexture> CreateTexture(const JValue& object, const Decoder& d, uint64_t captureId);
    id<MTLBuffer> CreateBuffer(const JValue& object, const Decoder& d, uint64_t captureId);
    id<MTLLibrary> CreateLibrary(const JValue& object, const Decoder& d, uint64_t captureId);
    id<MTLFunction> CreateFunction(const JValue& object, const Decoder& d, uint64_t captureId);
    id CreateRenderPipeline(const JValue& object, const Decoder& d, uint64_t captureId);
    id CreateComputePipeline(const JValue& object, const Decoder& d, uint64_t captureId);
    id CreateDepthStencil(const JValue& object, const Decoder& d, uint64_t captureId);
    id CreateSampler(const JValue& object, const Decoder& d, uint64_t captureId);
    id CreateHeap(const JValue& object, const Decoder& d, uint64_t captureId);
    id CreateAccelerationStructure(const JValue& object, const Decoder& d, uint64_t captureId);
    /** An intersection or visible function table, made from the pipeline it belongs to. */
    id CreateFunctionTable(const JValue& object, const Decoder& d, uint64_t captureId, bool intersection);
    /** A table made above, waiting for the objects its entries and buffers name. */
    struct PendingTable {
        id table = nil;
        uint64_t captureId = 0;
        id pipeline = nil;
        uint64_t pipelineId = 0;
        bool intersection = true;
        uint64_t functionCount = 0;
        std::string createdWith;
        std::string exportName;
    };
    /** Fills every table made, once every object exists; the tables are made before their buffers. */
    void FillFunctionTables();
    void FillFunctionTable(const PendingTable& p);

    // ---- contents
    void UploadContents();
    /** Writes `size` bytes into a buffer, through a staging blit when it has no CPU mapping. */
    void WriteBuffer(id<MTLBuffer> buffer, uint64_t offset, const void* data, uint64_t size, const std::string& what);
    void UploadTexture(id<MTLTexture> texture, const JValue& info, const uint8_t* data, size_t size);

    // ---- the frame
    void ReplayCommands();
    void BeginCommandBuffer(uint64_t captureId);
    void EndEncoder();
    void Commit(bool last);
    /** One command against the open encoder or command buffer. */
    void IssueCommand(uint32_t index, const std::string& method, const Decoder& d, const JValue& command);
    bool RenderCommand(const std::string& m, const Decoder& d, uint32_t index);
    bool ComputeCommand(const std::string& m, const Decoder& d, uint32_t index);
    bool BlitCommand(const std::string& m, const Decoder& d, uint32_t index);
    /** A build, refit, copy or compacted-size write on an acceleration structure encoder. */
    bool AccelerationCommand(const std::string& m, const Decoder& d, uint32_t index);
    bool CommonCommand(const std::string& m, const Decoder& d, uint32_t index);
    bool CommandBufferCommand(const std::string& m, const Decoder& d, uint32_t index);
    /** Residency and fences, which the render, compute and blit encoders all have. */
    bool ResidencyCommand(const std::string& m, const Decoder& d, uint32_t index, const std::string& var);
    bool OpenEncoder(const std::string& m, const Decoder& d, uint32_t index, uint64_t encoderId);
    /** Whether this selector opens an encoder; every other command runs against the one already open. */
    static bool OpensEncoder(const std::string& m);

    /** The pass the capture recorded, with the store actions the read-back needs forced on. */
    MTLRenderPassDescriptor* BuildRenderPass(const Decoder& d);
    /** `id<MTLxEncoder> encoderN = nil;`, outside the block that builds the pass and assigns it. */
    void DeclareEncoder(const std::string& type, const std::string& name);
    /** Whether a pass descriptor's JSON names any color attachment. */
    bool HasColorAttachments(const Decoder& d) const;
    /** The exporter's name for an object, or "nil" when there is no export. */
    std::string ExportName(id object) const;
    void BindBufferContents(const Decoder& d, const JValue& command);
    /** One CaptureBuffers range written into its buffer; each is written once. */
    void UploadBufferRange(uint64_t dataId);

    // ---- read-back and comparison
    void QueuePassReadbacks();
    /**
     * Textures a compute pass of the frame wrote, read back after it and compared with what the
     * capture holds.
     *
     * A render pass's attachments are compared at the end of the pass, which is where the capture
     * read them. A storage texture a kernel wrote has no pass end to hang that on: the capture read
     * it back because a later pass *sampled* it, so the manifest calls it a sampled texture and the
     * replay would otherwise only upload it. That leaves a frame whose work is all in compute —
     * test/path_tracer/metal, where the traced image is the whole output — comparing nothing.
     *
     * Only textures the replay saw bound writable to a compute encoder: one the frame merely reads
     * was uploaded from the capture, so comparing it would be comparing the upload with itself.
     */
    void CompareWrittenTextures(MtlReplayReport& report);
    void CompleteReadbacks(MtlReplayReport& report);

    // ---- helpers
    void Problem(const std::string& message);
    void LeftOut(uint32_t index, const std::string& method, const std::string& why);
    /** The replay's object for a capture id, or nil. */
    id Object(uint64_t id) const;
    /** The capture's object record for an id, or null. */
    const JValue* Record(uint64_t id) const;
    std::string LabelOf(uint64_t id) const;

    struct Readback {
        MtlTargetComparison comparison;
        id<MTLBuffer> staging = nil;
        uint64_t rowBytes = 0;
        const uint8_t* captured = nullptr;
        size_t capturedSize = 0;
    };
    struct OpenPass {
        uint64_t encoderId = 0;
        uint32_t passIndex = 0;
        bool parallel = false;
        /** A parallel encoder's sub-encoder: it shares the pass, and its end is not the pass's. */
        uint64_t parentId = 0;
        MTLRenderPassDescriptor* renderPass = nil;
        std::string variable;
    };

    const CaptureFile& _capture;
    MtlReplayOptions _options;
    MtlExporter* _x = nullptr;
    DecodeEnv _env;

    id<MTLDevice> _device = nil;
    id<MTLCommandQueue> _queue = nil;

    std::unordered_map<uint64_t, id> _objects;
    std::unordered_map<uint64_t, const JValue*> _records;
    /** Textures the capture says are a drawable's: re-created as ordinary render targets. */
    std::unordered_set<uint64_t> _drawables;
    /** The drawable's texture the capture made last: what the exported program shows in its window. */
    uint64_t _lastDrawable = 0;
    /** CaptureBuffers id -> its manifest entry, for the contents a bind names. */
    std::unordered_map<uint64_t, const JValue*> _bufferData;
    /** CaptureTextureFrames `capture` id -> its manifest entry, for a sampled texture's contents. */
    std::unordered_map<uint64_t, const JValue*> _textureData;
    /** Buffer ranges already written, so a uniform bound at every draw is uploaded once. */
    std::unordered_set<uint64_t> _uploadedBuffers;
    std::unordered_set<uint64_t> _uploadedTextures;
    /** (commandBuffer, passIndex) -> the manifest's attachment entries to compare. */
    std::map<std::pair<uint64_t, uint32_t>, std::vector<const JValue*>> _targets;

    // The command buffer being replayed, and what is open on it.
    uint64_t _commandBufferId = 0;
    id<MTLCommandBuffer> _commandBuffer = nil;
    std::string _commandBufferVar;
    uint32_t _passCounter = 0;
    id _encoder = nil;
    OpenPass _pass;
    /** The parallel encoder a sub-encoder came from, still open under it. */
    id _parallel = nil;
    OpenPass _parallelPass;
    /**
     * Scratch buffers the acceleration structure builds are given, held until the replay is done
     * with them: a build's scratch is the *driver's* size for a descriptor, so the replay makes its
     * own rather than trusting the application's, which a different driver may have sized smaller.
     */
    std::vector<id<MTLBuffer>> _scratch;
    /**
     * Each compute pipeline's linked functions by name, for filling a function table made from it.
     * A table entry names its function and nothing else, so the name is the only key there is.
     */
    std::unordered_map<uint64_t, std::vector<std::pair<std::string, id>>> _linkedFunctions;
    std::vector<PendingTable> _pendingTables;
    /**
     * Textures a compute pass of the frame wrote, and whether what they hold can be reproduced
     * (CompareWrittenTextures).
     *
     * A kernel that *reads* the texture it writes accumulates into it, so the frame continues from
     * contents nothing captured — the capture holds them only as they were after the frame. Neither
     * that texture nor anything else the same kernel wrote can be reproduced: the path tracer's
     * tonemapped output is the accumulation tonemapped, and it differs for the same reason the
     * accumulation does. So the verdict is taken per encoder, once it ends and every binding of it
     * is known.
     */
    std::unordered_map<uint64_t, bool> _writtenTextures;   // id -> reproducible
    /** Written textures bound to the open compute encoder, and whether any of them was read_write. */
    std::vector<uint64_t> _encoderWrites;
    bool _encoderAccumulates = false;
    /** The compute pipeline last bound, whose reflection says how each texture slot is accessed. */
    uint64_t _boundComputePipeline = 0;
    /** "readOnly" / "writeOnly" / "readWrite" for a slot of a compute pipeline's reflection, or "". */
    std::string ComputeTextureAccess(uint64_t pipelineId, uint64_t slot) const;
    std::vector<id<MTLCommandBuffer>> _committed;
    std::vector<Readback> _readbacks;

    size_t _objectCount = 0, _commandCount = 0, _submissionCount = 0, _leftOut = 0;
};

} // namespace mtlreplay
