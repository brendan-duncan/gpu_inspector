// Export to C++: a capture's frame written as a standalone, compilable C++ project, mainly for
// driver bug reports (docs/REPLAY.md, "Export to C++").
//
// The exporter watches the replay engine do its work — the device it creates, every object with
// the create info it actually handed the driver (after the replay's own fixups: transfer usage,
// memory of each resource's own, stored attachments, modules from the capture's SPIR-V), every
// upload, every command it records, every submission, and every render target it reads back — and
// spells each as C++ with the emitters generated from vk.xml (gen/vk_emit.gen.h). So the exported
// program does what the replay did, and where the replay reproduces a driver's fault, so does the
// source. The project it writes:
//
//   CMakeLists.txt, README.md           builds with CMake and a C++20 compiler alone; the loader is opened at run time
//   vulkan_headers/                     the Vulkan headers the source is spelled with (this build's, embedded in the tool)
//   main.cpp                            instance, device, objects, contents, the frame, then the comparison
//   vk_support.h / .cpp                 memory, uploads, layout tracking, read-backs, PNG and the data file
//   vk_functions.h / .cpp               the Vulkan functions the source calls, loaded by name (no prototypes)
//   frame_handles.h / .cpp              one global per captured object, named by type and capture id (image_18)
//   frame_device.cpp                    CreateInstance / CreateDevice, from what the replay enabled
//   frame_objects*.cpp                  CreateObjects: every object in id order
//   frame_contents*.cpp                 UploadContents: sampled and frame-start image contents, initial layouts
//   frame_commands*.cpp                 Frame: each submission's buffer uploads, descriptor writes, recording, submit, read-backs
//   frame_destroy*.cpp                  DestroyObjects: in reverse order of creation
//   frame_data.bin                      SPIR-V, image and buffer contents, and the captured targets to compare with
//
// Functions are split into parts of a couple of thousand lines and the parts over files, so a
// frame of hundreds of thousands of commands still compiles in ordinary memory.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "decode.h"
#include "gpucap.h"
#include "source_writer.h"

namespace vkreplay {

struct ReplayReport;
struct ExportReport;

// The create infos a plain vkCreate*(device, &info, nullptr, &handle) is exported from.
#define VKINSP_EXPORT_CREATE_INFOS(X) \
    X(VkImageViewCreateInfo)          \
    X(VkBufferViewCreateInfo)         \
    X(VkSamplerCreateInfo)            \
    X(VkDescriptorSetLayoutCreateInfo) \
    X(VkPipelineLayoutCreateInfo)     \
    X(VkDescriptorPoolCreateInfo)     \
    X(VkCommandPoolCreateInfo)        \
    X(VkFenceCreateInfo)              \
    X(VkSemaphoreCreateInfo)          \
    X(VkEventCreateInfo)              \
    X(VkQueryPoolCreateInfo)          \
    X(VkRenderPassCreateInfo)         \
    X(VkRenderPassCreateInfo2)        \
    X(VkFramebufferCreateInfo)        \
    X(VkShaderModuleCreateInfo)       \
    X(VkAccelerationStructureCreateInfoKHR)

class Exporter {
public:
    /** A shader module the replay made from a pipeline's own payload, for the stage named. */
    struct StageModule {
        VkShaderModule module = VK_NULL_HANDLE;
        std::string stage;             // "vertex", "fragment"... (StageName)
        const void* code = nullptr;
        size_t size = 0;
    };

    Exporter(std::string directory, const CaptureFile& capture);
    ~Exporter();
    Exporter(const Exporter&) = delete;
    Exporter& operator=(const Exporter&) = delete;

    /** Creates the directory and opens the data file; false with `error` when it cannot. */
    bool Open(std::string& error);

    // ---- names: the variable each handle is spelled as
    void Name(const char* type, uint64_t handle, const std::string& name);
    void Forget(const char* type, uint64_t handle);

    // ---- the device (frame_device.cpp)
    void Instance(uint32_t apiVersion, const std::vector<const char*>& extensions);
    /** `capturedName` is the GPU the capture was taken on, `replayName` the one it was replayed on. */
    void Device(const std::string& capturedName, const std::string& replayName, const VkDeviceCreateInfo& info, uint32_t queueFamily);

    // ---- objects (frame_objects*.cpp), in the order the replay creates them
    // A plain vkCreate*(device, &info, nullptr, &handle), with the create info the replay handed the driver.
#define VKINSP_EXPORT_DECLARE(T) \
    void Create(const std::string& type, uint64_t id, uint64_t handle, const char* function, const T& info, const std::string& comment = "");
    VKINSP_EXPORT_CREATE_INFOS(VKINSP_EXPORT_DECLARE)
#undef VKINSP_EXPORT_DECLARE
    void CreateImage(uint64_t id, VkImage image, const VkImageCreateInfo& info, const std::string& comment);
    void CreateBuffer(uint64_t id, VkBuffer buffer, const VkBufferCreateInfo& info);
    /** One of the three create infos is set; `modules` are the stage modules the replay made for it, destroyed after. */
    void CreatePipeline(uint64_t id, VkPipeline pipeline, const std::string& function, const VkGraphicsPipelineCreateInfo* graphics,
                        const VkComputePipelineCreateInfo* compute, const VkRayTracingPipelineCreateInfoKHR* rayTracing,
                        const std::vector<StageModule>& modules);
    void CreateShaderObject(uint64_t id, VkShaderEXT shader, const VkShaderCreateInfoEXT& info);
    void AllocateCommandBuffer(uint64_t id, VkCommandBuffer cb, const VkCommandBufferAllocateInfo& info);
    void AllocateDescriptorSet(uint64_t id, VkDescriptorSet set, const VkDescriptorSetAllocateInfo& info);
    void Queue(uint64_t id, VkQueue queue, uint32_t family, uint32_t index);
    /** An object the replay left out on purpose, or could not create: a comment where it would be. */
    void Skipped(const std::string& type, uint64_t id, const std::string& why);

    // ---- contents (frame_contents*.cpp)
    void UploadImage(uint64_t id, VkImage image, bool initial, const std::vector<VkBufferImageCopy>& regions, const void* data, size_t size);
    /**
     * `restore`: into RestoreFrame, which puts the frame's images back so that it can run again (the
     * exported program shows it in a loop). There `current` is where the frame left each subresource,
     * which the program's layout tracking is told first, since the frame's own barriers pass it by.
     */
    void BeginInitialLayouts(bool restore = false);
    void InitialLayouts(uint64_t id, VkImage image, const std::vector<VkImageLayout>& targets, const std::vector<VkImageLayout>* current = nullptr);
    void EndInitialLayouts(bool restore = false);
    /** A command pool the frame's command buffers come from, reset before they are recorded again. */
    void RestorePool(VkCommandPool pool);
    /** What the frame leaves on screen, for the window: an image, the layout the frame leaves it in, its format and size. */
    void FrameOutput(uint64_t id, VkImage image, VkImageLayout layout, VkFormat format, VkExtent2D extent, const std::string& comment);

    // ---- the frame (frame_commands*.cpp)
    void BeginSubmission(uint32_t commandIndex, const std::string& method);
    void UploadBuffer(uint64_t bufferId, VkBuffer buffer, VkDeviceSize offset, const void* data, size_t size);
    void UpdateDescriptorSets(uint64_t setId, const std::vector<VkWriteDescriptorSet>& writes);
    void BeginCommandBuffer(uint64_t id, VkCommandBuffer cb, const VkCommandBufferBeginInfo& info, uint32_t first, uint32_t last, bool secondary);
    /** A captured vkCmd*, decoded again with `ctx` (the replay's handles) and spelled; left out where the replay left it out. */
    void Command(uint32_t index, const std::string& method, const JValue& args, DecodeContext& ctx);
    /** vkCmdBeginRendering as the replay issued it (every attachment stored). */
    void CmdBeginRendering(uint32_t index, const VkRenderingInfo& info);
    /** A push through an update template, pushed as the writes the replay built from the snapshot. */
    void PushDescriptors(uint32_t index, VkPipelineBindPoint bindPoint, VkPipelineLayout layout, uint32_t set,
                         const std::vector<VkWriteDescriptorSet>& writes, bool khr);
    void LeftOut(uint32_t index, const std::string& method, const std::string& why);
    /**
     * A command the replay issued but the source cannot spell yet (a ray tracing build or trace). What
     * it wrote is then missing from the exported frame, so an image only a shader writes is not
     * compared after it: it would still hold what was uploaded, and match the capture for no reason.
     */
    void NotExported(uint32_t index, const std::string& method, const std::string& why);
    void EndCommandBuffer(VkCommandBuffer cb);
    /**
     * A render target read back at the end of its pass (or a storage image at the end of its command
     * buffer) for comparison with the capture's copy, which goes into the data file.
     */
    void Readback(VkImage image, const std::string& name, VkImageAspectFlags aspect, uint32_t mip, uint32_t baseLayer, uint32_t layers,
                  VkExtent2D extent, VkImageLayout layout, VkSampleCountFlagBits samples, VkFormat format, const uint8_t* captured, size_t size,
                  bool shaderWritten = false);
    void Submit(VkQueue queue, const std::vector<VkCommandBuffer>& cbs);

    /** Writes every file and fills `out`; false, with out.error, when one could not be written. */
    bool Finish(const ReplayReport& report, ExportReport& out);

private:
    /** One generated function, written as parts over one or more files. */
    struct Section {
        std::string file;        // "frame_objects"
        std::string function;    // "CreateObjects"
        SourceWriter writer;
        std::vector<std::string> parts;
    };
    struct Blob {
        uint64_t offset;
        uint64_t size;
    };

    void Configure(SourceWriter& w);
    /** Ends the section's current part when it has grown enough; every statement the exporter writes is self-contained. */
    void MaybeSplit(Section& s);
    void SplitNow(Section& s);
    std::string DataExpr(const void* data, size_t size);
    std::string HandleName(const char* type, uint64_t handle) const;
    static std::string VariableName(const std::string& type, uint64_t id);
    /** Names an object's variable, declares it in frame_handles.h and lists it for destruction. */
    std::string Declare(const std::string& type, uint64_t id, uint64_t handle);
    /** A statement (or a block of them, when `body` declares locals) of the frame, with a trailing label. */
    void FrameStatement(const std::string& label, const std::function<void(SourceWriter&)>& body);
    template <typename Info>
    void CreateFrom(const std::string& type, uint64_t id, uint64_t handle, const char* function, const char* infoType, const Info& info,
                    const std::string& comment);
    bool WriteText(const std::string& name, const std::string& text, std::string& error);
    bool WriteSection(Section& s, std::vector<std::string>& files, std::string& error);
    std::string FunctionsHeader(const std::set<std::string>& functions) const;
    std::string FunctionsSource(const std::set<std::string>& functions) const;
    std::string HandlesHeader() const;
    std::string HandlesSource() const;
    std::string CMake(const std::vector<std::string>& sources) const;
    std::string Readme(const ReplayReport& report, const ExportReport& summary) const;

    std::string _dir;
    const CaptureFile& _capture;
    FILE* _data = nullptr;
    uint64_t _dataSize = 0;
    std::unordered_map<uint64_t, std::vector<Blob>> _blobs;   // by content hash, so identical contents are stored once
    std::map<std::pair<std::string, uint64_t>, std::string> _names;
    std::vector<std::pair<std::string, std::string>> _created;  // (type, variable) in creation order
    std::vector<std::pair<std::string, std::string>> _handles;  // (type, variable) to declare
    std::vector<std::string> _notes;
    size_t _leftOut = 0;
    /** Commands of the frame so far that the replay issued and the source does not hold (NotExported). */
    size_t _notExported = 0;
    size_t _objects = 0;
    size_t _commands = 0;
    size_t _submissions = 0;
    size_t _targets = 0;
    std::string _deviceSource;
    std::string _instanceSource;
    std::string _capturedDevice;
    std::string _replayDevice;
    std::vector<std::string> _cbStack;   // the command buffer variables being recorded, innermost last
    Section _objectsSection{"frame_objects", "CreateObjects", {}, {}};
    Section _contentsSection{"frame_contents", "UploadContents", {}, {}};
    Section _frameSection{"frame_commands", "Frame", {}, {}};
    Section _destroySection{"frame_destroy", "DestroyObjects", {}, {}};
    Section _restoreSection{"frame_restore", "RestoreFrame", {}, {}};
    std::string _outputSource;
    std::set<std::string> _used;
};

} // namespace vkreplay
