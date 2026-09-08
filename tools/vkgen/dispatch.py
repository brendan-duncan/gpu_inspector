"""
Emits the layer's dispatch tables and forwarding entry points.

  vk_dispatch.gen.h    InstanceDispatch / DeviceDispatch function pointer tables + init functions
  vk_entry.gen.cpp     Forwarding entry points for every Vulkan command, plus the name lookup table
  vk_commands.gen.h    Command ids and names

Commands listed in MANUAL_COMMANDS are declared but not generated; they are implemented by hand.
Commands whose first parameter is a VkCommandBuffer get a recording hook after the downstream
call (see CommandRecorder in the layer) so frame capture can serialize their arguments.
"""
import os

from .registry import Command
from .serialize import Context

MANUAL_COMMANDS = {
    "vkGetInstanceProcAddr",
    "vkGetDeviceProcAddr",
    "vkCreateInstance",
    "vkDestroyInstance",
    "vkCreateDevice",
    "vkDestroyDevice",
    "vkEnumerateInstanceLayerProperties",
    "vkEnumerateInstanceExtensionProperties",
    "vkEnumerateDeviceLayerProperties",
    "vkEnumerateDeviceExtensionProperties",
    "vkEnumerateInstanceVersion",
    "vkQueuePresentKHR",
    # object labels
    "vkSetDebugUtilsObjectNameEXT",
    "vkDebugMarkerSetObjectNameEXT",
}

# Parent object for created handles, when it is not the dispatch object. (command -> (type, expr))
CREATE_PARENTS = {
    "vkAllocateCommandBuffers": ("VkCommandPool", "pAllocateInfo->commandPool"),
    "vkAllocateDescriptorSets": ("VkDescriptorPool", "pAllocateInfo->descriptorPool"),
    "vkGetSwapchainImagesKHR": ("VkSwapchainKHR", "swapchain"),
    "vkCreateImageView": ("VkImage", "pCreateInfo->image"),
    "vkCreateBufferView": ("VkBuffer", "pCreateInfo->buffer"),
}

# Commands that implicitly destroy all children of an object. (command -> (type, expr))
RESET_COMMANDS = {
    "vkResetDescriptorPool": ("VkDescriptorPool", "descriptorPool"),
}

# Hand-written post-call hooks (declared in hooks.h), called with the command's arguments after
# the downstream call and after object registration. Signature: void Hook_<cmd>(<params>).
EXTRA_HOOKS = {
    "vkCreateShaderModule",
    "vkCreateGraphicsPipelines",
    "vkCreateComputePipelines",
    "vkBindBufferMemory",
    "vkBindImageMemory",
    "vkBindBufferMemory2",
    "vkBindImageMemory2",
    "vkBindBufferMemory2KHR",
    "vkBindImageMemory2KHR",
    # physical device capabilities attached to the VkPhysicalDevice object
    "vkEnumeratePhysicalDevices",
    # resource registry
    "vkCreateImage",
    "vkCreateImageView",
    "vkCreateBuffer",
    "vkCreateFramebuffer",
    "vkCreateRenderPass",
    "vkCreateRenderPass2",
    "vkCreateRenderPass2KHR",
    "vkCreateSwapchainKHR",
    "vkGetSwapchainImagesKHR",
    # frame capture
    "vkEndCommandBuffer",
    "vkFreeCommandBuffers",
    "vkQueueSubmit",
    "vkQueueSubmit2",
    "vkQueueSubmit2KHR",
    "vkCmdBeginRenderPass",
    "vkCmdBeginRenderPass2",
    "vkCmdBeginRenderPass2KHR",
    "vkCmdBeginRendering",
    "vkCmdBeginRenderingKHR",
    "vkCmdEndRenderPass",
    "vkCmdEndRenderPass2",
    "vkCmdEndRenderPass2KHR",
    "vkCmdEndRendering",
    "vkCmdEndRenderingKHR",
    "vkCmdExecuteCommands",
    # image layout tracking / live readback
    "vkCmdPipelineBarrier",
    "vkCmdPipelineBarrier2",
    "vkCmdPipelineBarrier2KHR",
    "vkGetDeviceQueue",
    "vkGetDeviceQueue2",
    # descriptor set contents
    "vkCreateDescriptorSetLayout",
    "vkAllocateDescriptorSets",
    "vkUpdateDescriptorSets",
    "vkCreateDescriptorUpdateTemplate",
    "vkCreateDescriptorUpdateTemplateKHR",
    "vkUpdateDescriptorSetWithTemplate",
    "vkUpdateDescriptorSetWithTemplateKHR",
    # bound resources captured with the binding command
    "vkCmdBindDescriptorSets",
    "vkCmdBindDescriptorSets2",
    "vkCmdBindDescriptorSets2KHR",
    "vkCmdPushDescriptorSet",
    "vkCmdPushDescriptorSetKHR",
    "vkCmdPushDescriptorSet2",
    "vkCmdPushDescriptorSet2KHR",
    "vkCmdBindVertexBuffers",
    "vkCmdBindVertexBuffers2",
    "vkCmdBindVertexBuffers2EXT",
    "vkCmdBindIndexBuffer",
    "vkCmdBindIndexBuffer2",
    "vkCmdBindIndexBuffer2KHR",
    "vkCmdDrawIndirect",
    "vkCmdDrawIndexedIndirect",
    "vkCmdDispatchIndirect",
}

# Hand-written pre-call hooks: PreHook_<cmd>(<params by reference>). They run before the
# downstream call and may replace pointer arguments (e.g. to add usage flags to a create info).
PRE_HOOKS = {
    "vkCreateImage",
    "vkCreateBuffer",
    "vkCreateSwapchainKHR",
    "vkBeginCommandBuffer",
    "vkResetCommandBuffer",
    # live shader editing: bind the replacement pipeline instead of the original
    "vkCmdBindPipeline",
    # pass profiling: timestamp before the pass begins (must be outside the render pass)
    "vkCmdBeginRenderPass",
    "vkCmdBeginRenderPass2",
    "vkCmdBeginRenderPass2KHR",
    "vkCmdBeginRendering",
    "vkCmdBeginRenderingKHR",
    # CPU submit time
    "vkQueueSubmit",
    "vkQueueSubmit2",
    "vkQueueSubmit2KHR",
    # frame boundaries of applications that never present (OpenXR): see EndFrame in layer.cpp
    "vkWaitForFences",
    # compute pass timing: a run of dispatches is bracketed from the first dispatch to the next
    # barrier / event wait / render pass / label / secondary execution / end of the buffer
    "vkCmdDispatch",
    "vkCmdDispatchBase",
    "vkCmdDispatchBaseKHR",
    "vkCmdDispatchIndirect",
    "vkCmdPipelineBarrier",
    "vkCmdPipelineBarrier2",
    "vkCmdPipelineBarrier2KHR",
    "vkCmdWaitEvents",
    "vkCmdWaitEvents2",
    "vkCmdWaitEvents2KHR",
    "vkCmdExecuteCommands",
    "vkEndCommandBuffer",
    "vkCmdBeginDebugUtilsLabelEXT",
    "vkCmdEndDebugUtilsLabelEXT",
    "vkCmdDebugMarkerBeginEXT",
    "vkCmdDebugMarkerEndEXT",
}

SKIP_COMMANDS = {"vkNegotiateLoaderLayerInterfaceVersion"}

HEADER = "// GENERATED FILE - do not edit. Produced by tools/gen_vulkan.py from vk.xml.\n"


def short(name):
    return name[2:]


def guard(lines, protect, body):
    if protect:
        lines.append(f"#ifdef {protect}")
    lines.extend(body)
    if protect:
        lines.append("#endif")


def emit_dispatch_header(cmds, out):
    lines = [HEADER, "#pragma once", "#include <vulkan/vulkan.h>", "", "namespace vkinsp {", ""]
    for level, struct_name in (("instance", "InstanceDispatch"), ("device", "DeviceDispatch")):
        lines.append(f"struct {struct_name} {{")
        for c in cmds:
            if c.level == level:
                guard(lines, c.protect, [f"    PFN_{c.name} {short(c.name)} = nullptr;"])
        lines.append("};")
        lines.append("")
    lines += [
        "void InitInstanceDispatch(VkInstance instance, PFN_vkGetInstanceProcAddr gpa, InstanceDispatch& d);",
        "void InitDeviceDispatch(VkDevice device, PFN_vkGetDeviceProcAddr gpa, DeviceDispatch& d);",
        "",
        "// Looks up the layer's own entry point for a command name; *level gets 0=global 1=instance 2=device.",
        "PFN_vkVoidFunction LookupEntryPoint(const char* name, int* level);",
        "",
        "} // namespace vkinsp",
        "",
    ]
    with open(os.path.join(out, "vk_dispatch.gen.h"), "w", newline="\n") as f:
        f.write("\n".join(lines))


def handle_types(reg):
    return sorted(n for n in reg.handles if n not in reg.handle_aliases)


def emit_commands_header(reg, cmds, out):
    lines = [HEADER, "#pragma once", "#include <cstdint>", "#include <vulkan/vulkan.h>", "", "namespace vkinsp {", ""]
    lines.append("// Every Vulkan handle type, in sorted name order.")
    lines.append("enum HandleType : uint16_t {")
    for i, h in enumerate(handle_types(reg)):
        lines.append(f"    HT_{h} = {i},")
    lines.append(f"    HT_Count = {len(handle_types(reg))}")
    lines.append("};")
    lines.append("")
    lines.append("extern const char* const kHandleTypeNames[];")
    lines.append("HandleType HandleTypeFromObjectType(VkObjectType t);")
    lines.append("")
    lines.append("enum class VkCmdId : uint16_t {")
    for i, c in enumerate(cmds):
        lines.append(f"    {short(c.name)} = {i},")
    lines.append(f"    Count = {len(cmds)}")
    lines.append("};")
    lines.append("")
    lines.append("extern const char* const kVkCommandNames[];")
    lines.append("")
    lines.append("} // namespace vkinsp")
    lines.append("")
    with open(os.path.join(out, "vk_commands.gen.h"), "w", newline="\n") as f:
        f.write("\n".join(lines))


def output_handles(reg, c):
    """Non-const pointer-to-handle parameters: the objects a command creates or retrieves."""
    return [p for p in c.params[1:] if p.ptr_depth == 1 and not p.is_const and reg.category(p.type) == "handle"]


def destroyed_handles(reg, c):
    """Handle parameters a vkDestroy*/vkFree* command destroys."""
    if not (c.name.startswith("vkDestroy") or c.name.startswith("vkFree")):
        return []
    handles = [p for p in c.params[1:] if reg.category(p.type) == "handle"]
    arrays = [p for p in handles if p.ptr_depth == 1 and p.len]
    if arrays:
        return arrays
    singles = [p for p in handles if p.ptr_depth == 0]
    if c.name.startswith("vkDestroy"):
        want = "Vk" + c.name[len("vkDestroy"):]
        want = reg.handle_aliases.get(want, want)
        singles = [p for p in singles if reg.handle_aliases.get(p.type, p.type) == want]
    return singles


def emit_entry_cpp(reg, cmds, out):
    lines = [HEADER]
    lines += [
        '#include "vk_dispatch.gen.h"',
        '#include "vk_commands.gen.h"',
        '#include "vk_serialize.gen.h"',
        '#include "layer.h"',
        '#include "command_recorder.h"',
        '#include "tracker.h"',
        '#include "hooks.h"',
        "#include <cstring>",
        "",
        "namespace vkinsp {",
        "",
        "const char* const kVkCommandNames[] = {",
    ]
    for c in cmds:
        lines.append(f'    "{c.name}",')
    lines.append("};")
    lines.append("")
    lines.append("const char* const kHandleTypeNames[] = {")
    for h in handle_types(reg):
        lines.append(f'    "{h}",')
    lines.append("};")
    lines.append("")
    lines.append("HandleType HandleTypeFromObjectType(VkObjectType t) {")
    lines.append("    switch ((int)t) {")
    for h in handle_types(reg):
        ot = reg.handle_objtype.get(h)
        if ot:
            guard(lines, reg.type_protect.get(h, "VKINSP_UNUSED_TYPE"), [f"    case {ot}: return HT_{h};"])
    lines.append("    default: return HT_Count;")
    lines.append("    }")
    lines.append("}")
    lines.append("")

    lines.append("// Hand-written entry points (see layer/src).")
    for c in cmds:
        if c.name in MANUAL_COMMANDS:
            params = ", ".join(p.decl for p in c.params)
            guard(lines, c.protect, [f"VKAPI_ATTR {c.ret} VKAPI_CALL layer_{c.name}({params});"])
    lines.append("")

    lines.append("// Generated forwarders.")
    for c in cmds:
        if c.name in MANUAL_COMMANDS or c.level == "global":
            continue
        params = ", ".join(p.decl for p in c.params)
        args = ", ".join(p.name for p in c.params)
        first = c.dispatch_param
        getter = "GetInstanceData" if c.level == "instance" else "GetDeviceData"
        body = [f"static VKAPI_ATTR {c.ret} VKAPI_CALL layer_{c.name}({params}) {{"]
        body.append(f"    auto* vkinsp_dev = {getter}({first.name});")
        call = f"vkinsp_dev->dispatch.{short(c.name)}({args})"
        ctx = Context(c.params, "")
        creates = output_handles(reg, c)
        destroys = destroyed_handles(reg, c)
        reset = RESET_COMMANDS.get(c.name)

        if c.name in PRE_HOOKS:
            body.append(f"    PreHook_{c.name}({args});")

        # Destroy hooks run before the downstream call, while the handle is still valid.
        for p in destroys:
            real = reg.handle_aliases.get(p.type, p.type)
            if p.len:
                n = ctx.count(p.len[0])
                body.append(f"    if ({p.name}) for (size_t i = 0; i < (size_t)({n}); ++i) Tracker::Get().OnDestroy(HT_{real}, (uint64_t)(uintptr_t){p.name}[i]);")
            else:
                body.append(f"    Tracker::Get().OnDestroy(HT_{real}, (uint64_t)(uintptr_t){p.name});")
        if reset:
            body.append(f"    Tracker::Get().OnDestroyChildren(HT_{reset[0]}, (uint64_t)(uintptr_t){reset[1]});")

        has_result = c.ret != "void"
        if has_result:
            body.append(f"    {c.ret} result = {call};")
        else:
            body.append(f"    {call};")

        if first.type == "VkCommandBuffer":
            body.append(f"    if (CommandRecorder* rec = vkinsp_dev->RecorderFor({first.name})) {{")
            body.append(f"        JsonWriter& w = rec->Begin(VkCmdId::{short(c.name)});")
            body.append(f"        ArgsToJson_{c.name}(w, {args});")
            body.append(f"        rec->End(w, {'(int64_t)result' if has_result else '0'});")
            body.append("    }")

        if creates:
            cond = "    if ((int)result >= 0) {" if c.ret == "VkResult" else "    {"
            body.append(cond)
            body.append("        Tracker& t = Tracker::Get();")
            body.append("        JsonWriter& w = t.BeginArgs();")
            body.append(f"        ArgsToJson_{c.name}(w, {args});")
            parent = CREATE_PARENTS.get(c.name, (first.type, first.name))
            pexpr = f"HT_{parent[0]}, (uint64_t)(uintptr_t)({parent[1]})"
            for p in creates:
                real = reg.handle_aliases.get(p.type, p.type)
                if p.len:
                    n = ctx.count(p.len[0])
                    body.append(f"        if ({p.name}) for (size_t i = 0; i < (size_t)({n}); ++i) if ({p.name}[i]) t.OnCreate(HT_{real}, (uint64_t)(uintptr_t){p.name}[i], {pexpr}, VkCmdId::{short(c.name)}, (uint32_t)i, w.str());")
                else:
                    body.append(f"        if ({p.name} && *{p.name}) t.OnCreate(HT_{real}, (uint64_t)(uintptr_t)*{p.name}, {pexpr}, VkCmdId::{short(c.name)}, 0, w.str());")
            body.append("        t.EndArgs();")
            body.append("    }")

        if c.name in EXTRA_HOOKS:
            body.append(f"    Hook_{c.name}({args});")

        if has_result:
            body.append("    return result;")
        body.append("}")
        guard(lines, c.protect, body)
    lines.append("")

    for level, struct_name, handle, fn in (
        ("instance", "InstanceDispatch", "VkInstance instance", "InitInstanceDispatch"),
        ("device", "DeviceDispatch", "VkDevice device", "InitDeviceDispatch"),
    ):
        gpa_t = "PFN_vkGetInstanceProcAddr" if level == "instance" else "PFN_vkGetDeviceProcAddr"
        hname = handle.split()[1]
        lines.append(f"void {fn}({handle}, {gpa_t} gpa, {struct_name}& d) {{")
        for c in cmds:
            if c.level == level:
                guard(lines, c.protect, [f'    d.{short(c.name)} = (PFN_{c.name})gpa({hname}, "{c.name}");'])
        lines.append("}")
        lines.append("")

    lines.append("namespace {")
    lines.append("struct EntryPoint { const char* name; PFN_vkVoidFunction fn; int level; };")
    lines.append("const EntryPoint kEntryPoints[] = {")
    level_id = {"global": 0, "instance": 1, "device": 2}
    for c in cmds:
        if c.level == "global" and c.name not in MANUAL_COMMANDS:
            continue
        guard(lines, c.protect, [f'    {{ "{c.name}", (PFN_vkVoidFunction)layer_{c.name}, {level_id[c.level]} }},'])
    lines.append("};")
    lines.append("} // namespace")
    lines.append("")
    lines += [
        "PFN_vkVoidFunction LookupEntryPoint(const char* name, int* level) {",
        "    size_t lo = 0, hi = sizeof(kEntryPoints) / sizeof(kEntryPoints[0]);",
        "    while (lo < hi) {",
        "        size_t mid = (lo + hi) / 2;",
        "        int cmp = strcmp(kEntryPoints[mid].name, name);",
        "        if (cmp == 0) { if (level) *level = kEntryPoints[mid].level; return kEntryPoints[mid].fn; }",
        "        if (cmp < 0) lo = mid + 1; else hi = mid;",
        "    }",
        "    return nullptr;",
        "}",
        "",
        "} // namespace vkinsp",
        "",
    ]
    with open(os.path.join(out, "vk_entry.gen.cpp"), "w", newline="\n") as f:
        f.write("\n".join(lines))


def ref_decl(p):
    """'const VkImageCreateInfo* pCreateInfo' -> 'const VkImageCreateInfo*& pCreateInfo'."""
    i = p.decl.rfind(" " + p.name)
    return p.decl[:i] + "& " + p.name


def emit_hooks_header(cmds, out):
    lines = [HEADER, "#pragma once", "#include <vulkan/vulkan.h>", "", "namespace vkinsp {", "",
             "// Post-call hooks implemented in layer/src/hooks.cpp."]
    for c in cmds:
        if c.name in EXTRA_HOOKS:
            params = ", ".join(p.decl for p in c.params)
            guard(lines, c.protect, [f"void Hook_{c.name}({params});"])
    lines.append("")
    lines.append("// Pre-call hooks (all parameters by reference so pointers can be substituted).")
    for c in cmds:
        if c.name in PRE_HOOKS:
            params = ", ".join(ref_decl(p) for p in c.params)
            guard(lines, c.protect, [f"void PreHook_{c.name}({params});"])
    lines += ["", "} // namespace vkinsp", ""]
    with open(os.path.join(out, "vk_hooks.gen.h"), "w", newline="\n") as f:
        f.write("\n".join(lines))


def emit(reg, out):
    cmds = [c for c in reg.commands if c.name not in SKIP_COMMANDS]
    emit_hooks_header(cmds, out)
    emit_dispatch_header(cmds, out)
    emit_commands_header(reg, cmds, out)
    emit_entry_cpp(reg, cmds, out)
    return cmds
