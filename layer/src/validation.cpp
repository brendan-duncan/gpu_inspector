#include "validation.h"

#include "capture.h"
#include "command_recorder.h"
#include "json_writer.h"
#include "layer.h"
#include "tracker.h"
#include "transport.h"
#include "vk_commands.gen.h"

#include <cstring>

namespace vkinsp {

namespace {

// Unique messages kept per process; beyond this new ones are only counted.
constexpr size_t kMaxEntries = 2000;

VKAPI_ATTR VkBool32 VKAPI_CALL MessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT types,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                 void* /*userData*/) {
    ValidationLog::Get().OnMessage(severity, types, data);
    return VK_FALSE;
}

const char* SeverityName(VkDebugUtilsMessageSeverityFlagBitsEXT s) {
    if (s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) return "error";
    if (s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) return "warning";
    if (s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) return "info";
    return "verbose";
}

} // namespace

ValidationLog& ValidationLog::Get() {
    static ValidationLog log;
    return log;
}

bool ValidationLog::EnsureDebugUtils(PFN_vkGetInstanceProcAddr nextGipa, VkInstanceCreateInfo& info, std::vector<const char*>& names) {
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i) {
        if (strcmp(info.ppEnabledExtensionNames[i], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) return true;
    }
    // Only add it when the layers and loader below us offer it (the loader implements it itself,
    // so this is the normal case).
    auto enumerate = (PFN_vkEnumerateInstanceExtensionProperties)nextGipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!enumerate) return false;
    uint32_t count = 0;
    if (enumerate(nullptr, &count, nullptr) != VK_SUCCESS || count == 0) return false;
    std::vector<VkExtensionProperties> props(count);
    if (enumerate(nullptr, &count, props.data()) < VK_SUCCESS) return false;
    bool available = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(props[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) { available = true; break; }
    }
    if (!available) {
        Log("VK_EXT_debug_utils not available: validation messages will not be reported");
        return false;
    }
    names.assign(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);
    names.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    info.ppEnabledExtensionNames = names.data();
    info.enabledExtensionCount = (uint32_t)names.size();
    return true;
}

void ValidationLog::CreateMessenger(InstanceData* inst) {
    if (!inst) return;
    if (!inst->dispatch.CreateDebugUtilsMessengerEXT) {
        Log("vkCreateDebugUtilsMessengerEXT unavailable: validation messages will not be reported");
        return;
    }
    VkDebugUtilsMessengerCreateInfoEXT ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = MessengerCallback;
    VkResult r = inst->dispatch.CreateDebugUtilsMessengerEXT(inst->instance, &ci, nullptr, &inst->messenger);
    if (r != VK_SUCCESS) {
        inst->messenger = VK_NULL_HANDLE;
        Log("vkCreateDebugUtilsMessengerEXT failed (%d): validation messages will not be reported", (int)r);
    }
}

void ValidationLog::DestroyMessenger(InstanceData* inst) {
    if (!inst || inst->messenger == VK_NULL_HANDLE || !inst->dispatch.DestroyDebugUtilsMessengerEXT) return;
    inst->dispatch.DestroyDebugUtilsMessengerEXT(inst->instance, inst->messenger, nullptr);
    inst->messenger = VK_NULL_HANDLE;
}

ValidationLog::CommandRef ValidationLog::CurrentCommand(const VkDebugUtilsMessengerCallbackDataEXT* data) {
    CommandRef ref;
    if (!g_captureActive.load(std::memory_order_relaxed)) return ref;
    for (uint32_t i = 0; i < data->objectCount; ++i) {
        const VkDebugUtilsObjectNameInfoEXT& o = data->pObjects[i];
        if (o.objectType != VK_OBJECT_TYPE_COMMAND_BUFFER || !o.objectHandle) continue;
        // Only a tracked (live) handle is safe to dereference for its dispatch key.
        uint64_t id = Tracker::Get().Resolve(HT_VkCommandBuffer, o.objectHandle);
        if (!id) continue;
        VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)o.objectHandle;
        DeviceData* dev = GetDeviceData(cb);
        CommandRecorder* rec = dev ? CaptureManager::Get().RecorderFor(dev, cb) : nullptr;
        // The message fires inside the vkCmd call, before the layer's post-hook appends the
        // command, so the command in flight is at the current count.
        if (!rec || rec->ended()) continue;
        ref.cmdBufferId = id;
        ref.slot = (int64_t)rec->commandCount();
        break;
    }
    return ref;
}

void ValidationLog::OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                              const VkDebugUtilsMessengerCallbackDataEXT* data) {
    if (!data || !data->pMessage) return;
    // The message text names the handles involved, so the same mistake on another object is a
    // message of its own; the same mistake on the same object every frame is one message counted.
    std::string dedupe = std::to_string(data->messageIdNumber) + "|" + data->pMessage;
    CommandRef cmd = CurrentCommand(data);

    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _keys.find(dedupe);
    if (it != _keys.end()) {
        Entry& e = _entries[it->second - 1];
        e.count++;
        e.dirty = true;
        // A repeat while a capture records the command buffer: point the message at this
        // recording, which is the one the capture will show.
        if (cmd.cmdBufferId && (cmd.cmdBufferId != e.cmdBufferId || cmd.slot != e.cmdSlot)) {
            e.cmdBufferId = cmd.cmdBufferId;
            e.cmdSlot = cmd.slot;
            e.resend = true;
        }
        _anyDirty = true;
        return;
    }
    if (_entries.size() >= kMaxEntries) {
        _dropped++;
        _droppedDirty = true;
        _anyDirty = true;
        return;
    }
    Entry e;
    e.key = _entries.size() + 1;
    e.severity = SeverityName(severity);
    if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) e.types.push_back("validation");
    if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) e.types.push_back("performance");
    if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) e.types.push_back("general");
    if (data->pMessageIdName) e.idName = data->pMessageIdName;
    e.idNumber = data->messageIdNumber;
    e.message = data->pMessage;
    e.frame = _frame.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < data->objectCount; ++i) {
        const VkDebugUtilsObjectNameInfoEXT& o = data->pObjects[i];
        if (!o.objectHandle) continue;
        HandleType ht = HandleTypeFromObjectType(o.objectType);
        ObjectRef ref;
        ref.handleType = ht < HT_Count ? (int)ht : -1;
        ref.className = ht < HT_Count ? kHandleTypeNames[ht] : "VkObject";
        ref.handle = o.objectHandle;
        if (o.pObjectName) ref.name = o.pObjectName;
        e.objects.push_back(std::move(ref));
    }
    for (uint32_t i = 0; i < data->queueLabelCount; ++i)
        if (data->pQueueLabels[i].pLabelName) e.queueLabels.push_back(data->pQueueLabels[i].pLabelName);
    for (uint32_t i = 0; i < data->cmdBufLabelCount; ++i)
        if (data->pCmdBufLabels[i].pLabelName) e.cmdBufLabels.push_back(data->pCmdBufLabels[i].pLabelName);
    e.count = 1;
    e.cmdBufferId = cmd.cmdBufferId;
    e.cmdSlot = cmd.slot;
    _keys.emplace(std::move(dedupe), e.key);
    _entries.push_back(std::move(e));
    const Entry& added = _entries.back();
    if (LogEnabled()) Log("validation %s: %s", added.severity.c_str(), added.message.c_str());
    if (Transport::Get().Connected()) Transport::Get().SendJson(MessageJson(added));
}

std::string ValidationLog::MessageJson(const Entry& e) const {
    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("action"); w.String("ValidationMessage");
    w.Key("key"); w.Uint(e.key);
    w.Key("severity"); w.String(e.severity);
    w.Key("types"); w.BeginArray();
    for (auto& t : e.types) w.String(t);
    w.EndArray();
    w.Key("idName"); w.String(e.idName);
    w.Key("idNumber"); w.Int(e.idNumber);
    w.Key("message"); w.String(e.message);
    w.Key("frame"); w.Uint(e.frame);
    w.Key("count"); w.Uint(e.count);
    w.Key("objects"); w.BeginArray();
    for (auto& o : e.objects) {
        w.BeginObject();
        w.Key("object");
        if (o.handleType >= 0) w.Handle(o.handleType, o.className.c_str(), o.handle);
        else w.Null();
        w.Key("class"); w.String(o.className);
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)o.handle);
        w.Key("handle"); w.String(buf);
        if (!o.name.empty()) { w.Key("name"); w.String(o.name); }
        w.EndObject();
    }
    w.EndArray();
    if (!e.queueLabels.empty()) {
        w.Key("queueLabels"); w.BeginArray();
        for (auto& l : e.queueLabels) w.String(l);
        w.EndArray();
    }
    if (!e.cmdBufLabels.empty()) {
        w.Key("cmdBufLabels"); w.BeginArray();
        for (auto& l : e.cmdBufLabels) w.String(l);
        w.EndArray();
    }
    if (e.cmdBufferId && e.cmdSlot >= 0) {
        w.Key("command"); w.BeginObject();
        w.Key("commandBuffer"); w.Uint(e.cmdBufferId);
        w.Key("slot"); w.Uint((uint64_t)e.cmdSlot);
        w.EndObject();
    }
    w.EndObject();
    return std::move(w.str());
}

void ValidationLog::Flush() {
    if (!_anyDirty || !Transport::Get().Connected()) return;
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_anyDirty) return;
    // Messages whose command reference moved go out in full; the rest as counts.
    for (Entry& e : _entries) {
        if (!e.resend) continue;
        Transport::Get().SendJson(MessageJson(e));
        e.resend = false;
        e.dirty = false;
    }
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ValidationCount");
    w.Key("counts"); w.BeginArray();
    for (Entry& e : _entries) {
        if (!e.dirty) continue;
        w.BeginArray(); w.Uint(e.key); w.Uint(e.count); w.EndArray();
        e.dirty = false;
    }
    w.EndArray();
    if (_droppedDirty) {
        w.Key("dropped"); w.Uint(_dropped);
        _droppedDirty = false;
    }
    w.EndObject();
    _anyDirty = false;
    Transport::Get().SendJson(std::move(w.str()));
}

void ValidationLog::SendSnapshot() {
    std::lock_guard<std::mutex> lock(_mutex);
    for (Entry& e : _entries) {
        Transport::Get().SendJson(MessageJson(e));
        e.dirty = false;
        e.resend = false;
    }
    if (_dropped) {
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("ValidationCount");
        w.Key("counts"); w.BeginArray(); w.EndArray();
        w.Key("dropped"); w.Uint(_dropped);
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
        _droppedDirty = false;
    }
    _anyDirty = false;
}

} // namespace vkinsp
