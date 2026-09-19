#include "exporter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "replayer.h"
#include "util.h"
#include "vk_emit.gen.h"

namespace vkreplay {

// The exported project's hand-written files (src/replay/export_template, embedded by tools/embed_files.py).
struct EmbeddedFile { const char* name; const char* const* pieces; size_t count; };
extern const EmbeddedFile kExportTemplates[];
extern const size_t kExportTemplatesCount;

namespace {

/**
 * Lines in one generated function, and in one generated file: bounds on what the compiler is given
 * at once. VKINSP_EXPORT_PART_LINES and VKINSP_EXPORT_FILE_LINES override them, which is how the
 * splitting is tested on a frame far smaller than the ones it is for.
 */
size_t LimitFromEnvironment(const char* name, size_t fallback) {
    const char* text = std::getenv(name);
    const long long value = text ? std::atoll(text) : 0;
    return value > 0 ? (size_t)value : fallback;
}
const size_t kPartLines = LimitFromEnvironment("VKINSP_EXPORT_PART_LINES", 2000);
const size_t kFileLines = LimitFromEnvironment("VKINSP_EXPORT_FILE_LINES", 24000);

/** Vulkan functions the hand-written support and main call, whatever the frame uses. */
const char* const kSupportFunctions[] = {
    "vkEnumerateInstanceExtensionProperties", "vkEnumerateInstanceLayerProperties", "vkCreateInstance", "vkDestroyInstance",
    "vkEnumeratePhysicalDevices", "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceMemoryProperties",
    "vkEnumerateDeviceExtensionProperties", "vkCreateDevice", "vkDestroyDevice", "vkGetDeviceQueue", "vkDeviceWaitIdle",
    "vkQueueSubmit", "vkQueueWaitIdle", "vkCreateCommandPool", "vkDestroyCommandPool", "vkAllocateCommandBuffers",
    "vkFreeCommandBuffers", "vkBeginCommandBuffer", "vkEndCommandBuffer", "vkCreateBuffer", "vkDestroyBuffer",
    "vkGetBufferMemoryRequirements", "vkGetImageMemoryRequirements", "vkAllocateMemory", "vkFreeMemory", "vkBindBufferMemory",
    "vkBindImageMemory", "vkMapMemory", "vkUnmapMemory", "vkCmdCopyBuffer", "vkCmdCopyBufferToImage", "vkCmdCopyImageToBuffer",
    "vkCmdPipelineBarrier", "vkCmdResolveImage", "vkCreateImage", "vkDestroyImage", "vkCreateDebugUtilsMessengerEXT",
    "vkDestroyDebugUtilsMessengerEXT",
    // resolving a multisampled depth target to read it back
    "vkCreateRenderPass2", "vkDestroyRenderPass", "vkCreateImageView", "vkDestroyImageView", "vkCreateFramebuffer",
    "vkDestroyFramebuffer", "vkCmdBeginRenderPass", "vkCmdEndRenderPass",
};

/** How an object of each type is destroyed; types missing here go with their pool or are not the frame's to destroy. */
const char* DestroyFunction(const std::string& type) {
    static const std::map<std::string, const char*> kDestroy = {
        {"VkImage", "vkDestroyImage"}, {"VkBuffer", "vkDestroyBuffer"}, {"VkImageView", "vkDestroyImageView"},
        {"VkBufferView", "vkDestroyBufferView"}, {"VkSampler", "vkDestroySampler"},
        {"VkDescriptorSetLayout", "vkDestroyDescriptorSetLayout"}, {"VkPipelineLayout", "vkDestroyPipelineLayout"},
        {"VkDescriptorPool", "vkDestroyDescriptorPool"}, {"VkCommandPool", "vkDestroyCommandPool"}, {"VkFence", "vkDestroyFence"},
        {"VkSemaphore", "vkDestroySemaphore"}, {"VkEvent", "vkDestroyEvent"}, {"VkQueryPool", "vkDestroyQueryPool"},
        {"VkRenderPass", "vkDestroyRenderPass"}, {"VkFramebuffer", "vkDestroyFramebuffer"},
        {"VkShaderModule", "vkDestroyShaderModule"}, {"VkPipeline", "vkDestroyPipeline"}, {"VkShaderEXT", "vkDestroyShaderEXT"},
        {"VkAccelerationStructureKHR", "vkDestroyAccelerationStructureKHR"},
    };
    auto it = kDestroy.find(type);
    return it == kDestroy.end() ? nullptr : it->second;
}

std::string Template(const char* name) {
    for (size_t i = 0; i < kExportTemplatesCount; ++i) {
        if (std::strcmp(kExportTemplates[i].name, name) != 0) continue;
        std::string out;
        for (size_t p = 0; p < kExportTemplates[i].count; ++p) out += kExportTemplates[i].pieces[p];
        return out;
    }
    return std::string();
}

std::string Hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
    return buf;
}

uint64_t HashBytes(const void* data, size_t size) {
    // FNV-1a, 64 bits: only to find contents already in the data file.
    uint64_t h = 1469598103934665603ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::string Trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \n");
    const size_t b = s.find_last_not_of(" \n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string ApiVersion(uint32_t v) {
    return "VK_MAKE_API_VERSION(0, " + std::to_string(VK_API_VERSION_MAJOR(v)) + ", " + std::to_string(VK_API_VERSION_MINOR(v)) + ", 0)";
}

} // namespace

Exporter::Exporter(std::string directory, const CaptureFile& capture) : _dir(std::move(directory)), _capture(capture) {
    for (Section* s : {&_objectsSection, &_contentsSection, &_frameSection, &_destroySection}) Configure(s->writer);
}

Exporter::~Exporter() {
    if (_data) std::fclose(_data);
}

void Exporter::Configure(SourceWriter& w) {
    w.handle = [this](const char* type, uint64_t handle) { return HandleName(type, handle); };
    w.data = [this](const void* data, size_t size) { return DataExpr(data, size); };
    w.indent = 1;
}

bool Exporter::Open(std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(_dir, ec);
    if (ec) {
        error = "could not create " + _dir + ": " + ec.message();
        return false;
    }
    const std::string path = _dir + "/frame_data.bin";
    _data = std::fopen(path.c_str(), "wb");
    if (!_data) {
        error = "could not write " + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Names and data

void Exporter::Name(const char* type, uint64_t handle, const std::string& name) {
    if (handle) _names[{type, handle}] = name;
}

void Exporter::Forget(const char* type, uint64_t handle) { _names.erase({type, handle}); }

std::string Exporter::HandleName(const char* type, uint64_t handle) const {
    auto it = _names.find({type, handle});
    return it == _names.end() ? std::string() : it->second;
}

std::string Exporter::VariableName(const std::string& type, uint64_t id) {
    std::string stem = type.rfind("Vk", 0) == 0 ? type.substr(2) : type;
    for (const char* suffix : {"KHR", "EXT", "NV"}) {
        const size_t n = std::strlen(suffix);
        if (stem.size() > n && stem.compare(stem.size() - n, n, suffix) == 0) stem.resize(stem.size() - n);
    }
    if (!stem.empty()) stem[0] = (char)std::tolower((unsigned char)stem[0]);
    return stem + "_" + std::to_string(id);
}

std::string Exporter::Declare(const std::string& type, uint64_t id, uint64_t handle) {
    const std::string name = VariableName(type, id);
    _handles.emplace_back(type, name);
    _created.emplace_back(type, name);
    Name(type.c_str(), handle, name);
    return name;
}

std::string Exporter::DataExpr(const void* data, size_t size) {
    if (!data || !size || !_data) return "nullptr";
    const uint64_t hash = HashBytes(data, size);
    auto& known = _blobs[hash];
    for (const Blob& b : known)
        if (b.size == size) return "Data(" + Hex(b.offset) + ", " + std::to_string(size) + ")";
    // Every blob starts on 16 bytes, so SPIR-V words and anything else read in place are aligned.
    static const char zeros[16] = {};
    const uint64_t padded = (_dataSize + 15) & ~15ull;
    if (padded != _dataSize) std::fwrite(zeros, 1, (size_t)(padded - _dataSize), _data);
    std::fwrite(data, 1, size, _data);
    known.push_back({padded, size});
    _dataSize = padded + size;
    return "Data(" + Hex(padded) + ", " + std::to_string(size) + ")";
}

// ---------------------------------------------------------------------------------------------
// Sections

void Exporter::MaybeSplit(Section& s) {
    if (s.writer.Lines() >= kPartLines) SplitNow(s);
}

void Exporter::SplitNow(Section& s) {
    if (s.writer.text.empty()) return;
    s.parts.push_back(std::move(s.writer.text));
    s.writer.ResetPart();
}

void Exporter::FrameStatement(const std::string& label, const std::function<void(SourceWriter&)>& body) {
    SourceWriter& w = _frameSection.writer;
    SourceWriter scratch;
    Configure(scratch);
    scratch.cb = w.cb;
    scratch.indent = w.indent + 1;
    body(scratch);
    if (scratch.Statements() == 1) {
        // One statement: on its own line, the label after it.
        const std::string statement = Trimmed(scratch.text);
        w.used.insert(scratch.used.begin(), scratch.used.end());
        for (const std::string& n : scratch.notes) w.Note(n);
        w.Line(statement + (label.empty() || statement.rfind("//", 0) == 0 ? "" : "   // " + label));
    } else if (scratch.Statements()) {
        // Locals were declared ahead of the statement: a block of their own.
        w.Line("{" + (label.empty() ? std::string() : "   // " + label));
        w.Append(scratch);
        w.Line("}");
    }
    MaybeSplit(_frameSection);
}

// ---------------------------------------------------------------------------------------------
// The device

void Exporter::Instance(uint32_t apiVersion, const std::vector<const char*>& extensions) {
    std::string wanted;
    for (const char* e : extensions) wanted += (wanted.empty() ? "" : ", ") + SourceWriter::String(e);
    std::string s;
    s += "void CreateInstance(bool validate) {\n";
    if (extensions.empty()) {
        s += "    std::vector<const char*> extensions;\n";
    } else {
        s += "    const char* const wanted[] = {" + wanted + "};\n";
        s += "    std::vector<const char*> extensions = AvailableInstanceExtensions(wanted, " + std::to_string(extensions.size()) + ");\n";
    }
    s += "    const char* const layers[] = {\"VK_LAYER_KHRONOS_validation\"};\n";
    s += "    const bool layer = validate && ValidationLayerAvailable();\n";
    s += "    const VkApplicationInfo applicationInfo = {\n";
    s += "        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,\n";
    s += "        .pNext = nullptr,\n";
    s += "        .pApplicationName = \"GPU Inspector exported frame\",\n";
    s += "        .applicationVersion = 0,\n";
    s += "        .pEngineName = nullptr,\n";
    s += "        .engineVersion = 0,\n";
    s += "        .apiVersion = " + ApiVersion(apiVersion) + ",\n";
    s += "    };\n";
    s += "    const VkInstanceCreateInfo info = {\n";
    s += "        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,\n";
    s += "        .pNext = nullptr,\n";
    s += "        .flags = 0,\n";
    s += "        .pApplicationInfo = &applicationInfo,\n";
    s += "        .enabledLayerCount = layer ? 1u : 0u,\n";
    s += "        .ppEnabledLayerNames = layers,\n";
    s += "        .enabledExtensionCount = (uint32_t)extensions.size(),\n";
    s += "        .ppEnabledExtensionNames = extensions.data(),\n";
    s += "    };\n";
    s += "    VK_CHECK(vkCreateInstance(&info, nullptr, &instance));\n";
    s += "    LoadInstanceFunctions(instance);\n";
    s += "    if (layer) CreateDebugMessenger();\n";
    s += "}\n";
    _instanceSource = s;
}

void Exporter::Device(const std::string& capturedName, const std::string& replayName, const VkDeviceCreateInfo& info, uint32_t queueFamily) {
    _capturedDevice = capturedName;
    _replayDevice = replayName;
    SourceWriter w;
    Configure(w);
    std::string wanted;
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
        wanted += (wanted.empty() ? "" : ", ") + SourceWriter::String(info.ppEnabledExtensionNames[i]);
    if (!capturedName.empty() && capturedName != replayName) w.Comment("Captured on " + capturedName + "; this source was exported from a replay on " + replayName + ".");
    w.Line("physicalDevice = SelectPhysicalDevice(" + SourceWriter::String((capturedName.empty() ? replayName : capturedName).c_str()) + ");");
    if (info.enabledExtensionCount) {
        w.Comment("The extensions the frame's device had, less the ones missing here.");
        w.Line("const char* const wanted[] = {" + wanted + "};");
        w.Line("std::vector<const char*> extensions = AvailableDeviceExtensions(physicalDevice, wanted, " + std::to_string(info.enabledExtensionCount) + ");");
    } else {
        w.Line("std::vector<const char*> extensions;");
    }
    const std::string next = EmitPNext(w, info.pNext);
    const std::string features = info.pEnabledFeatures ? "&" + EmitLocal(w, "VkPhysicalDeviceFeatures", "features", Emit(w, *info.pEnabledFeatures, w.indent))
                                                       : std::string("nullptr");
    const std::string queues = EmitStructArray(w, "queues", "VkDeviceQueueCreateInfo", info.pQueueCreateInfos, (size_t)info.queueCreateInfoCount,
                                               [&](const VkDeviceQueueCreateInfo& e) { return Emit(w, e, w.indent + 1); });
    w.Line("const VkDeviceCreateInfo info = {");
    w.Line("    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,");
    w.Line("    .pNext = " + next + ",");
    w.Line("    .flags = 0,");
    w.Line("    .queueCreateInfoCount = " + std::to_string(info.queueCreateInfoCount) + ",");
    w.Line("    .pQueueCreateInfos = " + queues + ",");
    w.Line("    .enabledLayerCount = 0,");
    w.Line("    .ppEnabledLayerNames = nullptr,");
    w.Line("    .enabledExtensionCount = (uint32_t)extensions.size(),");
    w.Line("    .ppEnabledExtensionNames = extensions.data(),");
    w.Line("    .pEnabledFeatures = " + features + ",");
    w.Line("};");
    w.Line("VK_CHECK(vkCreateDevice(physicalDevice, &info, nullptr, &device));");
    w.Line("LoadDeviceFunctions(device);");
    w.Comment("Memory properties, the utility command pool, and `queue`: the first queue of the family the frame submits to.");
    w.Line("InitDevice(" + std::to_string(queueFamily) + ", " + queues + ", " + std::to_string(info.queueCreateInfoCount) + ");");
    _used.insert(w.used.begin(), w.used.end());
    for (const std::string& n : w.notes) _notes.push_back(n);
    _deviceSource = "void CreateDevice() {\n" + w.text + "}\n";
}

// ---------------------------------------------------------------------------------------------
// Objects

template <typename Info>
void Exporter::CreateFrom(const std::string& type, uint64_t id, uint64_t handle, const char* function, const char* infoType, const Info& info,
                          const std::string& comment) {
    const std::string name = Declare(type, id, handle);
    SourceWriter& w = _objectsSection.writer;
    w.ResetLocals();
    w.Line("{   // " + type + " " + std::to_string(id) + " (" + function + ")" + (comment.empty() ? "" : ": " + comment));
    ++w.indent;
    const std::string expr = Emit(w, info, w.indent);
    w.Line("const " + std::string(infoType) + " info = " + expr + ";");
    w.Use(function);
    w.Line("VK_CHECK(" + std::string(function) + "(device, &info, nullptr, &" + name + "));");
    --w.indent;
    w.Line("}");
    ++_objects;
    MaybeSplit(_objectsSection);
}

#define VKINSP_EXPORT_DEFINE(T)                                                                                                    \
    void Exporter::Create(const std::string& type, uint64_t id, uint64_t handle, const char* function, const T& info,              \
                          const std::string& comment) {                                                                           \
        CreateFrom(type, id, handle, function, #T, info, comment);                                                                 \
    }
VKINSP_EXPORT_CREATE_INFOS(VKINSP_EXPORT_DEFINE)
#undef VKINSP_EXPORT_DEFINE

void Exporter::CreateImage(uint64_t id, VkImage image, const VkImageCreateInfo& info, const std::string& comment) {
    const std::string name = Declare("VkImage", id, (uint64_t)image);
    SourceWriter& w = _objectsSection.writer;
    w.ResetLocals();
    w.Line("{   // VkImage " + std::to_string(id) + (comment.empty() ? std::string(" (vkCreateImage)") : ": " + comment));
    ++w.indent;
    w.Line("const VkImageCreateInfo info = " + Emit(w, info, w.indent) + ";");
    w.Use("vkCreateImage");
    w.Line("VK_CHECK(vkCreateImage(device, &info, nullptr, &" + name + "));");
    w.Line("BindImageMemory(" + name + ");   // memory of its own, device local");
    w.Line("RegisterImage(" + name + ", info.format, info.extent, info.mipLevels, info.arrayLayers, info.samples);");
    --w.indent;
    w.Line("}");
    ++_objects;
    MaybeSplit(_objectsSection);
}

void Exporter::CreateBuffer(uint64_t id, VkBuffer buffer, const VkBufferCreateInfo& info) {
    const std::string name = Declare("VkBuffer", id, (uint64_t)buffer);
    SourceWriter& w = _objectsSection.writer;
    w.ResetLocals();
    w.Line("{   // VkBuffer " + std::to_string(id) + " (vkCreateBuffer)");
    ++w.indent;
    w.Line("const VkBufferCreateInfo info = " + Emit(w, info, w.indent) + ";");
    w.Use("vkCreateBuffer");
    w.Line("VK_CHECK(vkCreateBuffer(device, &info, nullptr, &" + name + "));");
    const bool deviceAddress = (info.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
    w.Line("BindBufferMemory(" + name + ", " + (deviceAddress ? "true" : "false") + ");   // memory of its own, device local" +
           (deviceAddress ? ", with a device address" : ""));
    --w.indent;
    w.Line("}");
    ++_objects;
    MaybeSplit(_objectsSection);
}

void Exporter::CreatePipeline(uint64_t id, VkPipeline pipeline, const std::string& function, const VkGraphicsPipelineCreateInfo* graphics,
                              const VkComputePipelineCreateInfo* compute, const VkRayTracingPipelineCreateInfoKHR* rayTracing,
                              const std::vector<StageModule>& modules) {
    const std::string name = Declare("VkPipeline", id, (uint64_t)pipeline);
    SourceWriter& w = _objectsSection.writer;
    w.ResetLocals();
    w.Line("{   // VkPipeline " + std::to_string(id) + " (" + function + ")");
    ++w.indent;
    // The stages' modules, from the SPIR-V the capture keeps with the pipeline: they live for this block only.
    std::vector<std::string> moduleNames;
    for (const StageModule& m : modules) {
        std::string stem = m.stage;
        std::replace(stem.begin(), stem.end(), ':', '_');
        for (char& c : stem)
            if (!std::isalnum((unsigned char)c)) c = '_';
        const std::string moduleName = w.Local((stem + "Module").c_str());
        moduleNames.push_back(moduleName);
        w.Line("VkShaderModule " + moduleName + " = VK_NULL_HANDLE;");
        w.Line("{");
        ++w.indent;
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = m.size / 4 * 4;
        info.pCode = static_cast<const uint32_t*>(m.code);
        w.Line("const VkShaderModuleCreateInfo info = " + Emit(w, info, w.indent) + ";");
        w.Use("vkCreateShaderModule");
        w.Line("VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &" + moduleName + "));");
        --w.indent;
        w.Line("}");
        Name("VkShaderModule", (uint64_t)m.module, moduleName);
    }
    std::string type;
    std::string expr;
    if (graphics) { type = "VkGraphicsPipelineCreateInfo"; expr = Emit(w, *graphics, w.indent); }
    else if (compute) { type = "VkComputePipelineCreateInfo"; expr = Emit(w, *compute, w.indent); }
    else if (rayTracing) { type = "VkRayTracingPipelineCreateInfoKHR"; expr = Emit(w, *rayTracing, w.indent); }
    w.Line("const " + type + " info = " + expr + ";");
    w.Use(function.c_str());
    if (rayTracing) w.Line("VK_CHECK(" + function + "(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &" + name + "));");
    else w.Line("VK_CHECK(" + function + "(device, VK_NULL_HANDLE, 1, &info, nullptr, &" + name + "));");
    for (size_t i = 0; i < modules.size(); ++i) {
        w.Use("vkDestroyShaderModule");
        w.Line("vkDestroyShaderModule(device, " + moduleNames[i] + ", nullptr);");
        Forget("VkShaderModule", (uint64_t)modules[i].module);
    }
    --w.indent;
    w.Line("}");
    ++_objects;
    MaybeSplit(_objectsSection);
}

void Exporter::CreateShaderObject(uint64_t id, VkShaderEXT shader, const VkShaderCreateInfoEXT& info) {
    const std::string name = Declare("VkShaderEXT", id, (uint64_t)shader);
    SourceWriter& w = _objectsSection.writer;
    w.ResetLocals();
    w.Line("{   // VkShaderEXT " + std::to_string(id) + " (vkCreateShadersEXT), made unlinked");
    ++w.indent;
    w.Line("const VkShaderCreateInfoEXT info = " + Emit(w, info, w.indent) + ";");
    w.Use("vkCreateShadersEXT");
    w.Line("VK_CHECK(vkCreateShadersEXT(device, 1, &info, nullptr, &" + name + "));");
    --w.indent;
    w.Line("}");
    ++_objects;
    MaybeSplit(_objectsSection);
}

void Exporter::AllocateCommandBuffer(uint64_t id, VkCommandBuffer cb, const VkCommandBufferAllocateInfo& info) {
    const std::string name = VariableName("VkCommandBuffer", id);
    _handles.emplace_back("VkCommandBuffer", name);   // freed with its pool
    Name("VkCommandBuffer", (uint64_t)(uintptr_t)cb, name);
    SourceWriter& w = _objectsSection.writer;
    w.ResetLocals();
    w.Line("{   // VkCommandBuffer " + std::to_string(id) + " (vkAllocateCommandBuffers)");
    ++w.indent;
    w.Line("const VkCommandBufferAllocateInfo info = " + Emit(w, info, w.indent) + ";");
    w.Use("vkAllocateCommandBuffers");
    w.Line("VK_CHECK(vkAllocateCommandBuffers(device, &info, &" + name + "));");
    --w.indent;
    w.Line("}");
    ++_objects;
    MaybeSplit(_objectsSection);
}

void Exporter::AllocateDescriptorSet(uint64_t id, VkDescriptorSet set, const VkDescriptorSetAllocateInfo& info) {
    const std::string name = VariableName("VkDescriptorSet", id);
    _handles.emplace_back("VkDescriptorSet", name);   // freed with its pool
    Name("VkDescriptorSet", (uint64_t)set, name);
    SourceWriter& w = _objectsSection.writer;
    w.ResetLocals();
    w.Line("{   // VkDescriptorSet " + std::to_string(id) + " (vkAllocateDescriptorSets)");
    ++w.indent;
    w.Line("const VkDescriptorSetAllocateInfo info = " + Emit(w, info, w.indent) + ";");
    w.Use("vkAllocateDescriptorSets");
    w.Line("VK_CHECK(vkAllocateDescriptorSets(device, &info, &" + name + "));");
    --w.indent;
    w.Line("}");
    ++_objects;
    MaybeSplit(_objectsSection);
}

void Exporter::Queue(uint64_t id, VkQueue queue, uint32_t family, uint32_t index) {
    const std::string name = VariableName("VkQueue", id);
    _handles.emplace_back("VkQueue", name);
    Name("VkQueue", (uint64_t)(uintptr_t)queue, name);
    SourceWriter& w = _objectsSection.writer;
    w.Line(name + " = DeviceQueue(" + std::to_string(family) + ", " + std::to_string(index) + ");   // VkQueue " + std::to_string(id));
    ++_objects;
}

void Exporter::Skipped(const std::string& type, uint64_t id, const std::string& why) {
    _objectsSection.writer.Comment(type + " " + std::to_string(id) + ": " + why);
}

// ---------------------------------------------------------------------------------------------
// Contents

void Exporter::UploadImage(uint64_t id, VkImage image, bool initial, const std::vector<VkBufferImageCopy>& regions, const void* data, size_t size) {
    SourceWriter& w = _contentsSection.writer;
    const std::string name = w.Handle("VkImage", (uint64_t)image);
    w.ResetLocals();
    w.Line("{   // image " + std::to_string(id) + ": " + (initial ? "what it held when the frame first read it" : "its contents as the frame sampled them"));
    ++w.indent;
    const std::string list = EmitStructArray(w, "regions", "VkBufferImageCopy", regions.data(), regions.size(),
                                             [&](const VkBufferImageCopy& e) { return Emit(w, e, w.indent + 1); });
    w.Line("UploadImage(" + name + ", " + list + ", " + std::to_string(regions.size()) + ", " + DataExpr(data, size) + ", " + std::to_string(size) + ");");
    --w.indent;
    w.Line("}");
    MaybeSplit(_contentsSection);
}

void Exporter::BeginInitialLayouts() {
    SourceWriter& w = _contentsSection.writer;
    w.Comment("Every subresource in the first layout the frame expects it in (per mip, per layer; UNDEFINED leaves one alone).");
    w.Line("{");
    ++w.indent;
    w.Line("VkCommandBuffer cb = BeginOneTime();");
}

void Exporter::InitialLayouts(uint64_t id, VkImage image, const std::vector<VkImageLayout>& targets) {
    SourceWriter& w = _contentsSection.writer;
    if (std::all_of(targets.begin(), targets.end(), [](VkImageLayout l) { return l == VK_IMAGE_LAYOUT_UNDEFINED; })) return;
    const std::string name = w.Handle("VkImage", (uint64_t)image);
    const bool uniform = std::all_of(targets.begin(), targets.end(), [&](VkImageLayout l) { return l == targets[0]; });
    if (uniform) {
        w.Line("TransitionAll(cb, " + name + ", " + w.Enum(kEnum_VkImageLayout, kEnumCount_VkImageLayout, targets[0], "VkImageLayout") + ");");
        return;
    }
    std::string items;
    for (size_t i = 0; i < targets.size(); ++i)
        items += (i ? ", " : "") + w.Enum(kEnum_VkImageLayout, kEnumCount_VkImageLayout, targets[i], "VkImageLayout");
    const std::string list = EmitArrayLocal(w, "VkImageLayout", "layouts", items);
    w.Line("TransitionSubresources(cb, " + name + ", " + list + ", " + std::to_string(targets.size()) + ");   // image " + std::to_string(id));
}

void Exporter::EndInitialLayouts() {
    SourceWriter& w = _contentsSection.writer;
    w.Line("EndOneTime(cb);");
    --w.indent;
    w.Line("}");
    MaybeSplit(_contentsSection);
}

// ---------------------------------------------------------------------------------------------
// The frame

void Exporter::BeginSubmission(uint32_t commandIndex, const std::string& method) {
    SourceWriter& w = _frameSection.writer;
    w.Blank();
    w.Comment("---- The submission at command " + std::to_string(commandIndex) + " (" + method + ") ----");
}

void Exporter::UploadBuffer(uint64_t bufferId, VkBuffer buffer, VkDeviceSize offset, const void* data, size_t size) {
    SourceWriter& w = _frameSection.writer;
    w.Line("UploadBuffer(" + w.Handle("VkBuffer", (uint64_t)buffer) + ", " + std::to_string(offset) + ", " + DataExpr(data, size) + ", " +
           std::to_string(size) + ");   // buffer " + std::to_string(bufferId) + ": what the frame reads from it, as captured");
    MaybeSplit(_frameSection);
}

void Exporter::UpdateDescriptorSets(uint64_t setId, const std::vector<VkWriteDescriptorSet>& writes) {
    if (writes.empty()) return;
    FrameStatement("descriptor set " + std::to_string(setId) + ", as it was when bound", [&](SourceWriter& w) {
        const std::string list = EmitStructArray(w, "writes", "VkWriteDescriptorSet", writes.data(), writes.size(),
                                                 [&](const VkWriteDescriptorSet& e) { return Emit(w, e, w.indent + 1); });
        w.Use("vkUpdateDescriptorSets");
        w.Line("vkUpdateDescriptorSets(device, " + std::to_string(writes.size()) + ", " + list + ", 0, nullptr);");
    });
}

void Exporter::BeginCommandBuffer(uint64_t id, VkCommandBuffer cb, const VkCommandBufferBeginInfo& info, uint32_t first, uint32_t last, bool secondary) {
    SourceWriter& w = _frameSection.writer;
    const std::string name = w.Handle("VkCommandBuffer", (uint64_t)(uintptr_t)cb);
    _cbStack.push_back(name);
    w.cb = name;
    w.Comment(std::string(secondary ? "secondary command buffer " : "command buffer ") + std::to_string(id) +
              (secondary ? "" : ": commands " + std::to_string(first) + " to " + std::to_string(last)));
    if (!secondary) {
        w.Use("vkResetCommandBuffer");
        w.Line("vkResetCommandBuffer(" + name + ", 0);");
    }
    FrameStatement("", [&](SourceWriter& s) {
        const std::string beginInfo = EmitLocal(s, "VkCommandBufferBeginInfo", "beginInfo", Emit(s, info, s.indent));
        s.Use("vkBeginCommandBuffer");
        s.Line("VK_CHECK(vkBeginCommandBuffer(" + name + ", &" + beginInfo + "));");
    });
}

void Exporter::EndCommandBuffer(VkCommandBuffer cb) {
    SourceWriter& w = _frameSection.writer;
    w.Use("vkEndCommandBuffer");
    w.Line("VK_CHECK(vkEndCommandBuffer(" + w.Handle("VkCommandBuffer", (uint64_t)(uintptr_t)cb) + "));");
    if (!_cbStack.empty()) _cbStack.pop_back();
    w.cb = _cbStack.empty() ? "cb" : _cbStack.back();
    MaybeSplit(_frameSection);
}

void Exporter::Command(uint32_t index, const std::string& method, const JValue& args, DecodeContext& ctx) {
    const ExportFn fn = FindExportCommand(method);
    if (!fn) {
        LeftOut(index, method, "the replay does not record it");
        return;
    }
    bool leftOut = false;
    FrameStatement("[" + std::to_string(index) + "]", [&](SourceWriter& w) {
        fn(ctx, args, w);
        // The generated emitter leaves a command out with a comment, as the replay's recorder does.
        leftOut = w.Statements() == 1 && Trimmed(w.text).rfind("// left out", 0) == 0;
        if (leftOut) w.text = std::string((size_t)w.indent * 4, ' ') + "// [" + std::to_string(index) + "] " + method + ": " + Trimmed(w.text).substr(3) + "\n";
    });
    if (leftOut) ++_leftOut;
    else ++_commands;
}

void Exporter::CmdBeginRendering(uint32_t index, const VkRenderingInfo& info) {
    FrameStatement("[" + std::to_string(index) + "] every attachment stored, so the pass's result can be read", [&](SourceWriter& w) {
        const std::string renderingInfo = EmitLocal(w, "VkRenderingInfo", "renderingInfo", Emit(w, info, w.indent));
        w.Use("vkCmdBeginRendering");
        w.Line("vkCmdBeginRendering(" + w.cb + ", &" + renderingInfo + ");");
    });
    ++_commands;
}

void Exporter::PushDescriptors(uint32_t index, VkPipelineBindPoint bindPoint, VkPipelineLayout layout, uint32_t set,
                               const std::vector<VkWriteDescriptorSet>& writes, bool khr) {
    const char* function = khr ? "vkCmdPushDescriptorSetKHR" : "vkCmdPushDescriptorSet";
    FrameStatement("[" + std::to_string(index) + "] pushed through an update template in the capture; here as plain writes of what it pushed",
                   [&](SourceWriter& w) {
        const std::string list = EmitStructArray(w, "writes", "VkWriteDescriptorSet", writes.data(), writes.size(),
                                                 [&](const VkWriteDescriptorSet& e) { return Emit(w, e, w.indent + 1); });
        w.Use(function);
        w.Line(std::string(function) + "(" + w.cb + ", " + w.Enum(kEnum_VkPipelineBindPoint, kEnumCount_VkPipelineBindPoint, bindPoint, "VkPipelineBindPoint") +
               ", " + w.Handle("VkPipelineLayout", (uint64_t)layout) + ", " + std::to_string(set) + ", " + std::to_string(writes.size()) + ", " + list + ");");
    });
    ++_commands;
}

void Exporter::LeftOut(uint32_t index, const std::string& method, const std::string& why) {
    _frameSection.writer.Comment("[" + std::to_string(index) + "] " + method + ": left out: " + why);
    ++_leftOut;
}

void Exporter::NotExported(uint32_t index, const std::string& method, const std::string& why) {
    LeftOut(index, method, why);
    ++_notExported;
}

void Exporter::Readback(VkImage image, const std::string& name, VkImageAspectFlags aspect, uint32_t mip, uint32_t baseLayer, uint32_t layers,
                        VkExtent2D extent, VkImageLayout layout, VkSampleCountFlagBits samples, VkFormat format, const uint8_t* captured, size_t size,
                        bool shaderWritten) {
    SourceWriter& w = _frameSection.writer;
    if (shaderWritten && _notExported) {
        w.Comment(name + " is not compared: the replay compares it, but commands that write it are left out of this source, so here it");
        w.Comment("would still hold the contents uploaded for it and match the capture for no reason.");
        _notes.push_back(name + " is not compared by the exported program: commands that may write it are left out of the source");
        return;
    }
    w.Comment("Read back here to compare with the capture's copy, taken at the same point of the frame.");
    w.Line("ReadbackImage(" + w.cb + ", " + w.Handle("VkImage", (uint64_t)image) + ", " + SourceWriter::String(name.c_str()) + ", " +
           w.Flags(kEnum_VkImageAspectFlagBits, kEnumCount_VkImageAspectFlagBits, aspect, "VkImageAspectFlags") + ", " + std::to_string(mip) + ", " +
           std::to_string(baseLayer) + ", " + std::to_string(layers) + ", {" + std::to_string(extent.width) + ", " + std::to_string(extent.height) + "}, " +
           w.Enum(kEnum_VkImageLayout, kEnumCount_VkImageLayout, layout, "VkImageLayout") + ", " +
           w.Enum(kEnum_VkSampleCountFlagBits, kEnumCount_VkSampleCountFlagBits, samples, "VkSampleCountFlagBits") + ", " +
           w.Enum(kEnum_VkFormat, kEnumCount_VkFormat, format, "VkFormat") + ", " + DataExpr(captured, size) + ", " + std::to_string(size) + ");");
    ++_targets;
    MaybeSplit(_frameSection);
}

void Exporter::Submit(VkQueue queue, const std::vector<VkCommandBuffer>& cbs) {
    FrameStatement("without the application's semaphores and fence, and waited for", [&](SourceWriter& w) {
        std::string items;
        for (size_t i = 0; i < cbs.size(); ++i) items += (i ? ", " : "") + w.Handle("VkCommandBuffer", (uint64_t)(uintptr_t)cbs[i]);
        const std::string list = EmitArrayLocal(w, "VkCommandBuffer", "commandBuffers", items);
        w.Line("SubmitAndWait(" + w.Handle("VkQueue", (uint64_t)(uintptr_t)queue) + ", " + list + ", " + std::to_string(cbs.size()) + ");");
    });
    _frameSection.writer.Line("CompleteReadbacks();");
    ++_submissions;
    // A submission is a good place for a part to end.
    if (_frameSection.writer.Lines() >= kPartLines / 2) SplitNow(_frameSection);
}

// ---------------------------------------------------------------------------------------------
// Files

bool Exporter::WriteText(const std::string& name, const std::string& text, std::string& error) {
    const std::string path = _dir + "/" + name;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (out) out.write(text.data(), (std::streamsize)text.size());
    if (!out) {
        error = "could not write " + path;
        return false;
    }
    return true;
}

bool Exporter::WriteSection(Section& s, std::vector<std::string>& files, std::string& error) {
    SplitNow(s);
    _used.insert(s.writer.used.begin(), s.writer.used.end());
    for (const std::string& n : s.writer.notes) _notes.push_back(n);
    const std::string header = "// Generated by GPU Inspector's Export to C++ (vkinsp_replay --export). See README.md.\n"
                               "#include \"frame_handles.h\"\n#include \"vk_support.h\"\n\n";
    // The parts, distributed over files; the function that calls them all goes in the first.
    std::vector<std::string> fileTexts(1, header);
    std::vector<size_t> fileLines(1, 0);
    std::string declarations;
    std::string calls;
    for (size_t i = 0; i < s.parts.size(); ++i) {
        const std::string partName = s.function + "_" + std::to_string(i + 1);
        const size_t lines = (size_t)std::count(s.parts[i].begin(), s.parts[i].end(), '\n');
        if (fileLines.back() && fileLines.back() + lines > kFileLines) {
            fileTexts.push_back(header);
            fileLines.push_back(0);
        }
        fileTexts.back() += "void " + partName + "() {\n" + s.parts[i] + "}\n\n";
        fileLines.back() += lines + 3;
        declarations += "void " + partName + "();\n";
        calls += "    " + partName + "();\n";
    }
    fileTexts[0].insert(header.size(), declarations + (declarations.empty() ? "" : "\n") + "void " + s.function + "() {\n" + calls + "}\n\n");
    for (size_t f = 0; f < fileTexts.size(); ++f) {
        const std::string name = s.file + (f ? "_" + std::to_string(f + 1) : "") + ".cpp";
        if (!WriteText(name, fileTexts[f], error)) return false;
        files.push_back(name);
    }
    return true;
}

std::string Exporter::FunctionsHeader(const std::set<std::string>& functions) const {
    std::string s = "// The Vulkan functions this frame calls, loaded by name from the loader opened at run time\n"
                    "// (vk_support.cpp, LoadVulkanLoader): the program links against no Vulkan library.\n"
                    "#pragma once\n\n#ifndef VK_NO_PROTOTYPES\n#define VK_NO_PROTOTYPES\n#endif\n#include <vulkan/vulkan.h>\n\n";
    for (const std::string& f : functions) s += "extern PFN_" + f + " " + f + ";\n";
    s += "\nvoid LoadGlobalFunctions(PFN_vkGetInstanceProcAddr getInstanceProcAddr);\n"
         "void LoadInstanceFunctions(VkInstance instance);\n"
         "void LoadDeviceFunctions(VkDevice device);\n";
    return s;
}

std::string Exporter::FunctionsSource(const std::set<std::string>& functions) const {
    std::string s = "#include \"vk_functions.h\"\n\n";
    for (const std::string& f : functions) s += "PFN_" + f + " " + f + " = nullptr;\n";
    std::string global, inst, dev;
    for (const std::string& f : functions) {
        if (f == "vkGetInstanceProcAddr" || f == "vkGetDeviceProcAddr") continue;
        const int level = VulkanFunctionLevel(f);
        std::string& into = level == 0 ? global : level == 1 ? inst : dev;
        const char* getter = level == 0 ? "vkGetInstanceProcAddr(nullptr, \"" : level == 1 ? "vkGetInstanceProcAddr(instance, \"" : "vkGetDeviceProcAddr(device, \"";
        into += "    " + f + " = (PFN_" + f + ")" + getter + f + "\");\n";
    }
    s += "\nvoid LoadGlobalFunctions(PFN_vkGetInstanceProcAddr getInstanceProcAddr) {\n    vkGetInstanceProcAddr = getInstanceProcAddr;\n" + global + "}\n";
    s += "\nvoid LoadInstanceFunctions(VkInstance instance) {\n"
         "    vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(instance, \"vkGetDeviceProcAddr\");\n" + inst + "}\n";
    s += "\nvoid LoadDeviceFunctions(VkDevice device) {\n" + dev + "}\n";
    return s;
}

std::string Exporter::HandlesHeader() const {
    std::string s = "// One variable per object of the capture, named by its type and its id in the capture (the id\n"
                    "// GPU Inspector shows), and the functions the frame is made of.\n"
                    "#pragma once\n\n#include \"vk_support.h\"\n\n";
    for (const auto& [type, name] : _handles) s += "extern " + type + " " + name + ";\n";
    s += "\nvoid CreateInstance(bool validate);\nvoid CreateDevice();\nvoid CreateObjects();\nvoid UploadContents();\nvoid Frame();\nvoid DestroyObjects();\n";
    return s;
}

std::string Exporter::HandlesSource() const {
    std::string s = "#include \"frame_handles.h\"\n\n";
    for (const auto& [type, name] : _handles) s += type + " " + name + " = VK_NULL_HANDLE;\n";
    return s;
}

std::string Exporter::CMake(const std::vector<std::string>& sources) const {
    std::string list;
    for (const std::string& f : sources) list += "    " + f + "\n";
    return "cmake_minimum_required(VERSION 3.20)\n"
           "project(frame CXX)\n\n"
           "# A frame exported from a GPU Inspector capture (README.md). Designated initializers need C++20.\n"
           "set(CMAKE_CXX_STANDARD 20)\n"
           "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n"
           "# No Vulkan SDK is needed: the headers the source was written against are in vulkan_headers, and the\n"
           "# loader is opened at run time. VULKAN_HEADERS names another include directory to build against instead.\n"
           "set(VULKAN_HEADERS \"${CMAKE_CURRENT_SOURCE_DIR}/vulkan_headers\" CACHE PATH \"Include directory with vulkan/vulkan.h\")\n"
           "set(FRAME_VULKAN_INCLUDE \"${VULKAN_HEADERS}\")\n\n"
           "add_executable(frame\n" + list + ")\n"
           "target_include_directories(frame PRIVATE \"${FRAME_VULKAN_INCLUDE}\")\n"
           "target_compile_definitions(frame PRIVATE VK_NO_PROTOTYPES)\n"
           "if(MSVC)\n"
           "    target_compile_definitions(frame PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)\n"
           "    target_compile_options(frame PRIVATE /W3 /bigobj)\n"
           "else()\n"
           "    target_compile_options(frame PRIVATE -Wall -Wno-unused-variable -Wno-missing-field-initializers)\n"
           "    target_link_libraries(frame PRIVATE ${CMAKE_DL_LIBS})\n"
           "endif()\n\n"
           "# The data file beside the executable, where it looks for it.\n"
           "add_custom_command(TARGET frame POST_BUILD\n"
           "    COMMAND ${CMAKE_COMMAND} -E copy_if_different \"${CMAKE_CURRENT_SOURCE_DIR}/frame_data.bin\" \"$<TARGET_FILE_DIR:frame>/frame_data.bin\")\n";
}

std::string Exporter::Readme(const ReplayReport& report, const ExportReport& summary) const {
    auto text = [&](const char* key) { return Str(_capture.Manifest().Get(key)); };
    std::string s = "# An exported frame\n\n";
    s += "This project re-creates one captured frame of a Vulkan application and runs it again: the frame's\n"
         "objects, what its images and buffers held, and every command of its command buffers, as plain\n"
         "C++ with no dependency but the Vulkan headers. It was written by GPU Inspector's Export to C++\n"
         "(`vkinsp_replay --export`) from a capture, for reproducing a problem outside the application.\n\n";
    s += "| | |\n|---|---|\n";
    if (!text("application").empty()) s += "| Application | " + text("application") + " |\n";
    if (!text("savedAt").empty()) s += "| Captured | " + text("savedAt") + " |\n";
    if (!_capturedDevice.empty()) s += "| Captured on | " + _capturedDevice + " |\n";
    s += "| Exported from a replay on | " + _replayDevice + " |\n";
    s += "| Vulkan headers (in `vulkan_headers`) | " + std::to_string(VK_HEADER_VERSION) + " |\n";
    s += "| Objects | " + std::to_string(summary.objects) + " |\n";
    s += "| Commands | " + std::to_string(summary.commands) + " in " + std::to_string(summary.submissions) + " submission" + (summary.submissions == 1 ? "" : "s") + " |\n";
    s += "| Render targets compared | " + std::to_string(summary.targets) + " |\n\n";
    s += "## Build and run\n\n```\ncmake -B build\ncmake --build build --config Release\nbuild/Release/frame        # or build/frame\n```\n\n"
         "It needs CMake and a C++20 compiler, and nothing else: the Vulkan headers the source was written\n"
         "against are in `vulkan_headers` (Khronos' Vulkan-Headers, under their own license), and the Vulkan\n"
         "loader is opened at run time. `-DVULKAN_HEADERS=<include directory>` builds against other headers.\n\n"
         "- `--validate` enables the Khronos validation layer and prints its messages.\n"
         "- `--out <directory>` is where the render targets are written (default `out`), `--no-images` writes none.\n"
         "- `--data <file>` names `frame_data.bin` when it is not beside the executable.\n\n"
         "The program prints each render target the capture read back, compared byte for byte with the copy\n"
         "the capture holds, and writes both as PNG where the format allows. It exits with 0 when every target\n"
         "is identical, 1 when some differ or could not be compared, and 2 when a Vulkan call failed (the\n"
         "call is printed).\n\n";
    s += "## What is in it\n\n"
         "| File | |\n|---|---|\n"
         "| `frame_device.cpp` | The instance and the device, with the extensions and features the frame's device had. |\n"
         "| `frame_objects*.cpp` | `CreateObjects`: every object the frame uses, in the order it was created. |\n"
         "| `frame_contents*.cpp` | `UploadContents`: what the frame's images held, and the layouts it expects them in. |\n"
         "| `frame_commands*.cpp` | `Frame`: each submission's buffer contents, descriptor writes, command buffers and submit. |\n"
         "| `frame_destroy*.cpp` | `DestroyObjects`. |\n"
         "| `frame_handles.h` | One variable per object, named by type and capture id: `image_18` is image 18 in GPU Inspector. |\n"
         "| `frame_data.bin` | SPIR-V, image and buffer contents, and the captured render targets. |\n"
         "| `vk_support.*`, `vk_functions.*`, `main.cpp` | Not specific to the frame: memory, uploads, read-backs, loading Vulkan. |\n"
         "| `vulkan_headers/` | The Vulkan headers the source is spelled with. |\n\n"
         "A number in brackets after a command (`// [17]`) is its index in the capture's command list.\n\n";
    s += "## How it differs from the application\n\n"
         "The source is what GPU Inspector's replay did with the capture, which is the application's frame with\n"
         "these differences:\n\n"
         "- Every image and buffer has a memory allocation of its own, and transfer usage added, so their\n"
         "  contents can be uploaded and read back. External memory is dropped.\n"
         "- Swapchain images are ordinary images of the swapchain's format and size; there is no surface or window.\n"
         "- Pipelines take their shaders from the SPIR-V the capture kept, as shader modules of their own, and are\n"
         "  created one at a time without a pipeline cache or a base pipeline.\n"
         "- Render passes store every attachment, so each pass's result can be read.\n"
         "- Descriptor sets are written from what they held when they were bound, and buffers hold the ranges the\n"
         "  frame read; nothing else of the application's memory is in the capture.\n"
         "- Submissions carry no semaphores or fences; each is waited for before the next.\n\n";
    if (summary.leftOut || !report.problems.empty() || !summary.notes.empty()) {
        s += "## What the replay left out or reported\n\n";
        if (summary.leftOut) s += "- " + std::to_string(summary.leftOut) + " command(s) are left out, each with a comment where it would be in `frame_commands*.cpp`.\n";
        std::map<std::string, int> grouped;
        for (const std::string& p : report.problems) ++grouped[p];
        for (const std::string& n : summary.notes) ++grouped[n];
        size_t shown = 0;
        for (const auto& [p, n] : grouped) {
            if (++shown > 60) {
                s += "- ... " + std::to_string(grouped.size() - 60) + " more\n";
                break;
            }
            s += "- " + p + (n > 1 ? " (x" + std::to_string(n) + ")" : "") + "\n";
        }
        s += "\n";
    }
    if (!report.targets.empty()) {
        s += "## The replay's own result\n\nWhat the replay this was exported from got on " + _replayDevice + ", for comparison with a run of this program:\n\n";
        for (const TargetComparison& t : report.targets) {
            s += "- image " + std::to_string(t.image) + " (" + t.format + " " + std::to_string(t.width) + "x" + std::to_string(t.height) + " " + t.aspect + "): ";
            if (!t.compared) s += "not compared: " + t.note + "\n";
            else if (!t.differingTexels) s += "identical to the capture\n";
            else s += std::to_string(t.differingTexels) + " of " + std::to_string(t.texels) + " texels differ\n";
        }
        s += "\n";
    }
    return s;
}

bool Exporter::Finish(const ReplayReport& report, ExportReport& out) {
    out.requested = true;
    out.directory = _dir;
    if (_data) {
        std::fclose(_data);
        _data = nullptr;
    }
    // DestroyObjects, in reverse order of creation.
    for (auto it = _created.rbegin(); it != _created.rend(); ++it) {
        const char* destroy = DestroyFunction(it->first);
        if (!destroy) continue;
        _destroySection.writer.Use(destroy);
        _destroySection.writer.Line(std::string(destroy) + "(device, " + it->second + ", nullptr);");
        MaybeSplit(_destroySection);
    }

    std::vector<std::string> sources = {"main.cpp", "vk_support.cpp", "vk_functions.cpp", "frame_handles.cpp", "frame_device.cpp"};
    for (Section* s : {&_objectsSection, &_contentsSection, &_frameSection, &_destroySection})
        if (!WriteSection(*s, sources, out.error)) return false;

    std::set<std::string> functions(_used);
    for (const char* f : kSupportFunctions) functions.insert(f);
    functions.insert("vkGetInstanceProcAddr");
    functions.insert("vkGetDeviceProcAddr");
    for (auto it = functions.begin(); it != functions.end();) {
        if (VulkanFunctionLevel(*it) < 0) {
            _notes.push_back(*it + " is not a function of the Vulkan registry this was built with, and is not loaded");
            it = functions.erase(it);
        } else {
            ++it;
        }
    }

    const std::string deviceSource = "// The instance and the device the frame runs on, as the replay created them.\n"
                                     "#include \"frame_handles.h\"\n#include \"vk_support.h\"\n\n" + _instanceSource + "\n" + _deviceSource;
    out.objects = _objects;
    out.commands = _commands;
    out.submissions = _submissions;
    out.targets = _targets;
    out.leftOut = _leftOut;
    out.dataBytes = _dataSize;
    std::sort(_notes.begin(), _notes.end());
    _notes.erase(std::unique(_notes.begin(), _notes.end()), _notes.end());
    out.notes = _notes;

    if (!WriteText("vk_functions.h", FunctionsHeader(functions), out.error) || !WriteText("vk_functions.cpp", FunctionsSource(functions), out.error) ||
        !WriteText("frame_handles.h", HandlesHeader(), out.error) || !WriteText("frame_handles.cpp", HandlesSource(), out.error) ||
        !WriteText("frame_device.cpp", deviceSource, out.error) || !WriteText("CMakeLists.txt", CMake(sources), out.error) ||
        !WriteText("README.md", Readme(report, out), out.error))
        return false;
    for (const char* name : {"main.cpp", "vk_support.h", "vk_support.cpp"}) {
        if (Template(name).empty()) {
            out.error = std::string("the exporter was built without its template ") + name;
            return false;
        }
    }
    out.files = sources;
    for (const char* name : {"vk_support.h", "vk_functions.h", "frame_handles.h", "CMakeLists.txt", "README.md", "frame_data.bin"}) out.files.push_back(name);
    // The support code and main, and the Vulkan headers the source is spelled with (vulkan_headers/...).
    for (size_t i = 0; i < kExportTemplatesCount; ++i) {
        const std::string name = kExportTemplates[i].name;
        if (!WriteText(name, Template(name.c_str()), out.error)) return false;
        if (name.rfind("vulkan_headers/", 0) == 0) out.files.push_back(name);
    }
    return true;
}

} // namespace vkreplay
