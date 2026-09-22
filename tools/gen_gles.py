#!/usr/bin/env python3
"""Generates the OpenGL ES capture library's entry points from the Khronos registry (gl.xml).

The OpenGL ES counterpart of gen_vulkan.py, for the plugin in src/plugins/gles (docs/PLUGINS.md).
From gl.xml it writes, for OpenGL ES 2.0 through 3.2 and the extensions listed below:

  gles_api.gen.h         the GL types, a function pointer type per command, GlesDispatch (every command's
                         real entry point, filled in by the platform layer), and the hand-written hooks'
                         declarations (Pre_ / Post_ / Late_, see below).
  gles_hooks.gen.cpp     one hook per command that is recorded or has a hand-written hook: it calls the
                         real entry point, and while a capture records, serializes the arguments -- enums
                         by name (per the parameter's group), objects as references ({"__id", "__class"},
                         per the parameter's class), arrays and strings by the registry's `len` -- and
                         hands the command to the capture (runtime.h). kHooks lists them by name.
  gles_enums.gen.cpp     enum names: one table per group the recorded commands use, one for everything.
  gles_constants.gen.h   #define GL_... for every enum of the registry, as the GL headers would have them.

The hand-written hooks, which track objects and passes, are in src/plugins/gles/src. A command named
in PRE_HOOKS has Pre_<name>(args) called before the real entry point, POST_HOOKS Post_<name>(args[, result])
after it and before the arguments are serialized (so a glGen* can register the names it made first),
LATE_HOOKS Late_<name>(args) after serializing (so a glDelete* is recorded with the ids it deletes).

Usage: gen_gles.py [--registry build/khronos/gl.xml] [--out src/plugins/gles/gen]
The registry is downloaded (at REGISTRY_COMMIT) when the file is missing.
"""
import argparse
import os
import re
import sys
import urllib.request
import xml.etree.ElementTree as ET

REGISTRY_COMMIT = "1cdd228e34966dd6b95bd203e9f84faba0f371a1"
REGISTRY_URL = f"https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/{REGISTRY_COMMIT}/xml/gl.xml"

# Extensions whose commands are hooked too, when the driver has them: what ES applications use.
EXTENSIONS = [
    "GL_KHR_debug", "GL_EXT_debug_marker", "GL_EXT_debug_label", "GL_KHR_blend_equation_advanced",
    "GL_OES_vertex_array_object", "GL_OES_mapbuffer", "GL_EXT_map_buffer_range", "GL_EXT_buffer_storage",
    "GL_EXT_discard_framebuffer", "GL_EXT_texture_storage", "GL_OES_texture_3D", "GL_OES_get_program_binary",
    "GL_EXT_disjoint_timer_query", "GL_EXT_occlusion_query_boolean",
    "GL_EXT_multisampled_render_to_texture", "GL_EXT_multisampled_render_to_texture2",
    "GL_ANGLE_framebuffer_blit", "GL_ANGLE_framebuffer_multisample", "GL_NV_framebuffer_blit",
    "GL_ANGLE_instanced_arrays", "GL_EXT_instanced_arrays", "GL_EXT_draw_instanced", "GL_NV_draw_instanced",
    "GL_EXT_base_instance", "GL_EXT_draw_elements_base_vertex", "GL_OES_draw_elements_base_vertex",
    "GL_EXT_multi_draw_arrays", "GL_EXT_multi_draw_indirect",
    "GL_OES_EGL_image", "GL_EXT_EGL_image_storage", "GL_EXT_copy_image", "GL_OES_copy_image",
    "GL_EXT_draw_buffers", "GL_EXT_draw_buffers_indexed", "GL_OES_draw_buffers_indexed",
    "GL_EXT_blend_func_extended", "GL_OES_sample_shading", "GL_EXT_separate_shader_objects",
    "GL_EXT_geometry_shader", "GL_EXT_tessellation_shader", "GL_EXT_primitive_bounding_box",
    "GL_EXT_texture_border_clamp", "GL_EXT_texture_buffer", "GL_EXT_robustness", "GL_KHR_robustness",
    "GL_EXT_shader_framebuffer_fetch_non_coherent", "GL_QCOM_tiled_rendering", "GL_EXT_clip_control",
    "GL_OVR_multiview", "GL_OVR_multiview_multisampled_render_to_texture", "GL_EXT_polygon_offset_clamp",
    "GL_OES_viewport_array", "GL_EXT_memory_object", "GL_EXT_semaphore",
]

# Hand-written hooks (src/plugins/gles/src/hooks_*.cpp).
GEN_FUNCS = ["glGenBuffers", "glGenTextures", "glGenFramebuffers", "glGenRenderbuffers", "glGenVertexArrays",
             "glGenVertexArraysOES", "glGenSamplers", "glGenQueries", "glGenQueriesEXT", "glGenTransformFeedbacks",
             "glGenProgramPipelines", "glGenProgramPipelinesEXT"]
DELETE_FUNCS = ["glDeleteBuffers", "glDeleteTextures", "glDeleteFramebuffers", "glDeleteRenderbuffers",
                "glDeleteVertexArrays", "glDeleteVertexArraysOES", "glDeleteSamplers", "glDeleteQueries",
                "glDeleteQueriesEXT", "glDeleteTransformFeedbacks", "glDeleteProgramPipelines",
                "glDeleteProgramPipelinesEXT", "glDeleteShader", "glDeleteProgram", "glDeleteSync"]
DRAW_FUNCS = [
    "glDrawArrays", "glDrawElements", "glDrawArraysInstanced", "glDrawElementsInstanced", "glDrawRangeElements",
    "glDrawArraysIndirect", "glDrawElementsIndirect", "glDrawElementsBaseVertex", "glDrawRangeElementsBaseVertex",
    "glDrawElementsInstancedBaseVertex", "glDrawArraysInstancedANGLE", "glDrawElementsInstancedANGLE",
    "glDrawArraysInstancedEXT", "glDrawElementsInstancedEXT", "glDrawArraysInstancedNV", "glDrawElementsInstancedNV",
    "glDrawArraysInstancedBaseInstanceEXT", "glDrawElementsInstancedBaseInstanceEXT",
    "glDrawElementsInstancedBaseVertexBaseInstanceEXT", "glDrawElementsBaseVertexEXT", "glDrawElementsBaseVertexOES",
    "glDrawRangeElementsBaseVertexEXT", "glDrawRangeElementsBaseVertexOES", "glDrawElementsInstancedBaseVertexEXT",
    "glDrawElementsInstancedBaseVertexOES", "glMultiDrawArraysEXT", "glMultiDrawElementsEXT",
    "glMultiDrawArraysIndirectEXT", "glMultiDrawElementsIndirectEXT",
]
PASS_FUNCS = ["glClear", "glClearBufferiv", "glClearBufferuiv", "glClearBufferfv", "glClearBufferfi",
              "glInvalidateFramebuffer", "glInvalidateSubFramebuffer", "glDiscardFramebufferEXT"]
PRE_HOOKS = set(DRAW_FUNCS + PASS_FUNCS + ["glPopDebugGroup", "glPopDebugGroupKHR", "glPopGroupMarkerEXT"] + [
    "glDispatchCompute", "glDispatchComputeIndirect", "glBindFramebuffer", "glBlitFramebuffer",
    "glBlitFramebufferANGLE", "glBlitFramebufferNV",
    "glFramebufferTexture2D", "glFramebufferTextureLayer", "glFramebufferRenderbuffer", "glFramebufferTexture",
    "glFramebufferTextureEXT", "glFramebufferTexture2DMultisampleEXT", "glFramebufferTextureMultiviewOVR",
    "glFramebufferTextureMultisampleMultiviewOVR",
])
POST_HOOKS = set(GEN_FUNCS + ["glPushDebugGroup", "glPushDebugGroupKHR", "glPushGroupMarkerEXT"] + [
    "glCreateShader", "glCreateProgram", "glCreateShaderProgramv", "glCreateShaderProgramvEXT", "glFenceSync",
    "glBindBuffer", "glBindBufferBase", "glBindBufferRange", "glBindTexture", "glBindFramebuffer", "glBindRenderbuffer",
    "glBindVertexArray", "glBindVertexArrayOES", "glBindSampler", "glBindTransformFeedback",
    "glBufferData", "glBufferSubData", "glBufferStorageEXT", "glUnmapBuffer", "glUnmapBufferOES",
    "glCopyBufferSubData", "glMapBufferRange", "glMapBufferRangeEXT", "glMapBufferOES",
    "glTexImage2D", "glTexImage3D", "glTexImage3DOES", "glTexStorage2D", "glTexStorage3D", "glTexStorage2DEXT",
    "glTexStorage3DEXT", "glTexStorage2DMultisample", "glTexStorage3DMultisample", "glCompressedTexImage2D",
    "glCompressedTexImage3D", "glCompressedTexImage3DOES", "glCompressedTexSubImage2D", "glCopyTexImage2D",
    "glEGLImageTargetTexture2DOES", "glTexBuffer", "glTexBufferEXT", "glTexBufferRange", "glTexBufferRangeEXT",
    "glRenderbufferStorage", "glRenderbufferStorageMultisample", "glRenderbufferStorageMultisampleEXT",
    "glRenderbufferStorageMultisampleANGLE", "glEGLImageTargetRenderbufferStorageOES",
    "glFramebufferTexture2D", "glFramebufferTextureLayer", "glFramebufferRenderbuffer", "glFramebufferTexture",
    "glFramebufferTextureEXT", "glFramebufferTexture2DMultisampleEXT", "glFramebufferTextureMultiviewOVR",
    "glFramebufferTextureMultisampleMultiviewOVR",
    "glShaderSource", "glCompileShader", "glAttachShader", "glDetachShader", "glLinkProgram", "glProgramBinary",
    "glProgramBinaryOES", "glObjectLabel", "glObjectLabelKHR", "glLabelObjectEXT",
])
LATE_HOOKS = set(DELETE_FUNCS)
# Replaced outright: the hook is Override_<name>(args), which calls the real entry point itself. glGetError,
# so an error the library's own GL work cleared is still the application's to read.
OVERRIDE_HOOKS = {"glGetError"}

# The object type each registry `class` names, as the capture library reports it (AddObject type).
CLASS_TYPES = {
    "buffer": "Buffer", "texture": "Texture", "framebuffer": "Framebuffer", "renderbuffer": "Renderbuffer",
    "program": "Program", "shader": "Shader", "vertex array": "VertexArray", "sampler": "Sampler",
    "query": "Query", "transform feedback": "TransformFeedback", "program pipeline": "ProgramPipeline", "sync": "Sync",
}

# Commands that ask rather than do: not recorded (a frame of them would bury the draws).
def is_query(name):
    return (name.startswith("glGet") or name.startswith("glIs") or name in (
        "glCheckFramebufferStatus", "glGetError", "glGetGraphicsResetStatus", "glGetGraphicsResetStatusEXT",
        "glGetGraphicsResetStatusKHR", "glCheckFramebufferStatusOES"))

INT_TYPES = {"GLint", "GLsizei", "GLint64", "GLintptr", "GLsizeiptr", "GLshort", "GLbyte", "GLfixed", "GLclampx",
             "GLint64EXT"}
UINT_TYPES = {"GLuint", "GLuint64", "GLushort", "GLubyte", "GLuint64EXT", "GLhalf"}
FLOAT_TYPES = {"GLfloat", "GLclampf", "GLdouble", "GLclampd"}
# Longest array written in full; longer ones are summarized (a big glUniform4fv).
MAX_ARRAY = 256
# The groups whose enums are bits (filled in by main), which are written as "A | B".
BITMASK_GROUPS = set()


def load_registry(path):
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print(f"downloading {REGISTRY_URL}")
        urllib.request.urlretrieve(REGISTRY_URL, path)
    return ET.parse(path).getroot()


class Param:
    def __init__(self, el):
        self.name = el.find("name").text
        self.ptype = el.find("ptype").text if el.find("ptype") is not None else None
        self.group = el.get("group")
        self.cls = el.get("class")
        self.len = el.get("len")
        text = "".join(el.itertext()).strip()
        self.decl = re.sub(r"\s+", " ", text)
        self.type_text = self.decl[: self.decl.rfind(self.name)].strip()
        self.pointer = "*" in self.type_text
        self.const = self.type_text.startswith("const") or " const" in self.type_text


class Command:
    def __init__(self, el):
        proto = el.find("proto")
        self.name = proto.find("name").text
        text = "".join(proto.itertext()).strip()
        self.ret = re.sub(r"\s+", " ", text[: text.rfind(self.name)]).strip()
        self.ret_group = proto.get("group")
        self.ret_class = proto.get("class")
        self.params = [Param(p) for p in el.findall("param")]

    @property
    def void(self):
        return self.ret == "void"

    def sig(self):
        return ", ".join(p.decl for p in self.params) or "void"

    def args(self):
        return ", ".join(p.name for p in self.params)


def gles_commands(root):
    """Commands of OpenGL ES 2.0-3.2 and of EXTENSIONS, in registry order, and the enums they may name."""
    wanted = []
    enums = set()
    for f in root.findall("feature"):
        if f.get("api") != "gles2":
            continue
        for req in f.findall("require"):
            if req.get("profile") not in (None, "common"):
                continue
            wanted += [c.get("name") for c in req.findall("command")]
            enums |= {e.get("name") for e in req.findall("enum")}
    exts = {e.get("name"): e for e in root.find("extensions")}
    for name in EXTENSIONS:
        e = exts.get(name)
        if e is None:
            print(f"warning: {name} is not in the registry", file=sys.stderr)
            continue
        if "gles2" not in (e.get("supported") or "").split("|"):
            print(f"warning: {name} is not a GLES extension", file=sys.stderr)
            continue
        for req in e.findall("require"):
            if req.get("api") not in (None, "gles2"):
                continue
            wanted += [c.get("name") for c in req.findall("command")]
            enums |= {x.get("name") for x in req.findall("enum")}
    seen = set()
    order = []
    for n in wanted:
        if n not in seen:
            seen.add(n)
            order.append(n)
    cmds = {}
    for c in root.find("commands"):
        cmd = Command(c)
        cmds[cmd.name] = cmd
    return [cmds[n] for n in order], enums


def enum_tables(root, es_enums):
    """Every enum value's name (ES core names first), and each group's names."""
    values = {}   # name -> value
    groups = {}   # group -> [names]
    for block in root.findall("enums"):
        bitmask = block.get("type") == "bitmask"
        for e in block.findall("enum"):
            name = e.get("name")
            if e.get("api") not in (None, "gles2"):
                continue
            try:
                v = int(e.get("value"), 0)
            except (TypeError, ValueError):
                continue
            values[name] = v
            for g in (e.get("group") or "").split(","):
                if g:
                    groups.setdefault(g, []).append(name)
    return values, groups


SUFFIX = re.compile(r"_(OES|EXT|KHR|ARB|NV|ANGLE|QCOM|AMD|IMG|ARM|APPLE|INTEL|OVR|MESA|DMP|VIV|FJ|SGIX|SGIS|ATI|SUN|IBM|3DFX|HP|PGI|S3|WIN|REND|MESAX|GREMEDY|SGI|INGR|OML)$")


def best_name(names, es_enums):
    """The name to show for a value: an ES core name, then an unsuffixed one, then the first."""
    def rank(n):
        return (0 if n in es_enums and not SUFFIX.search(n) else 1 if n in es_enums else 2 if not SUFFIX.search(n) else 3, n)
    return sorted(names, key=rank)[0]


def c_ident(group):
    return re.sub(r"[^A-Za-z0-9_]", "_", group)


def write_api_header(out, cmds, recorded, hooked):
    lines = []
    w = lines.append
    w("// Generated by tools/gen_gles.py from the Khronos registry (gl.xml). Do not edit.")
    w("#pragma once")
    w("")
    w("#include <cstddef>")
    w("#include <cstdint>")
    w("")
    w("#if defined(_WIN32) && !defined(_WIN32_WCE)")
    w("#define GL_APIENTRY __stdcall")
    w("#else")
    w("#define GL_APIENTRY")
    w("#endif")
    w("")
    w("typedef unsigned int GLenum;")
    w("typedef unsigned char GLboolean;")
    w("typedef unsigned int GLbitfield;")
    w("typedef void GLvoid;")
    w("typedef int8_t GLbyte;")
    w("typedef uint8_t GLubyte;")
    w("typedef int16_t GLshort;")
    w("typedef uint16_t GLushort;")
    w("typedef int GLint;")
    w("typedef unsigned int GLuint;")
    w("typedef int32_t GLclampx;")
    w("typedef int GLsizei;")
    w("typedef float GLfloat;")
    w("typedef float GLclampf;")
    w("typedef double GLdouble;")
    w("typedef double GLclampd;")
    w("typedef char GLchar;")
    w("typedef uint16_t GLhalf;")
    w("typedef int32_t GLfixed;")
    w("typedef intptr_t GLintptr;")
    w("typedef intptr_t GLsizeiptr;")
    w("typedef int64_t GLint64;")
    w("typedef uint64_t GLuint64;")
    w("typedef int64_t GLint64EXT;")
    w("typedef uint64_t GLuint64EXT;")
    w("typedef struct __GLsync* GLsync;")
    w("typedef void* GLeglImageOES;")
    w("typedef void* GLeglClientBufferEXT;")
    w("typedef void (GL_APIENTRY* GLDEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam);")
    w("typedef GLDEBUGPROC GLDEBUGPROCKHR;")
    w("")
    for c in cmds:
        w(f"typedef {c.ret} (GL_APIENTRY* PFN_{c.name})({c.sig()});")
    w("")
    w("namespace glesinsp {")
    w("")
    w("/** The real entry points: the driver's own, or the trampolines past our hooks. Null where the driver has none. */")
    w("struct GlesDispatch {")
    for c in cmds:
        w(f"    PFN_{c.name} {c.name};")
    w("};")
    w("")
    w("extern GlesDispatch g_gl;")
    w("")
    w("/** Every command's name, in GlesDispatch's order: g_gl's members are this many pointers. */")
    w(f"constexpr size_t kCommandCount = {len(cmds)};")
    w("extern const char* const kCommandNames[kCommandCount];")
    w("")
    w("/** A hooked command: its name, our replacement, and the dispatch slot its real entry point goes in. */")
    w("struct HookEntry {")
    w("    const char* name;")
    w("    void* hook;")
    w("    void** real;")
    w("};")
    w(f"constexpr size_t kHookCount = {len(hooked)};")
    w("extern const HookEntry kHooks[kHookCount];")
    w("")
    w("// Hand-written hooks (src/plugins/gles/src): see tools/gen_gles.py.")
    by_name = {c.name: c for c in cmds}
    for name in sorted(PRE_HOOKS & set(by_name)):
        c = by_name[name]
        w(f"void Pre_{name}({c.sig()});")
    for name in sorted(POST_HOOKS & set(by_name)):
        c = by_name[name]
        extra = "" if c.void else (", " if c.params else "") + f"{c.ret} result"
        w(f"void Post_{name}({c.sig() if c.params else ''}{extra});".replace("(void, ", "("))
    for name in sorted(LATE_HOOKS & set(by_name)):
        c = by_name[name]
        w(f"void Late_{name}({c.sig()});")
    for name in sorted(OVERRIDE_HOOKS & set(by_name)):
        c = by_name[name]
        w(f"{c.ret} Override_{name}({c.sig() if c.params else ''});")
    w("")
    w("}  // namespace glesinsp")
    open(os.path.join(out, "gles_api.gen.h"), "w", newline="\n").write("\n".join(lines) + "\n")


def len_expr(p, params):
    """A C++ expression for the element count of an array parameter, or None when the registry's is not one."""
    if not p.len:
        return None
    if p.len.startswith("COMPSIZE"):
        return None
    names = {q.name for q in params}
    expr = p.len.replace(" ", "")
    tokens = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", expr)
    if any(t not in names for t in tokens):
        return None
    return f"(size_t)({expr})" if tokens else expr


def serialize_param(p, c, lines, groups, enum_groups):
    """The C++ that writes one argument into `w` (while recording)."""
    w = lines.append
    key = p.name
    w(f'        jw.Key("{key}");')
    base = p.ptype
    n = len_expr(p, c.params)
    if not p.pointer:
        if p.cls and base == "GLuint":
            w(f'        jw.Ref(RefOf(ObjType::{CLASS_TYPES[p.cls]}, {p.name}), "GL{CLASS_TYPES[p.cls]}");')
        elif p.cls == "sync" or base == "GLsync":
            w(f'        jw.Ref(SyncRef({p.name}), "GLSync");')
        elif base == "GLenum":
            fn = f"EnumName_{c_ident(p.group)}" if p.group in enum_groups else "EnumName"
            w(f"        jw.Enum({fn}({p.name}), (int64_t){p.name});")
        elif base == "GLbitfield":
            if p.group in enum_groups and p.group in BITMASK_GROUPS:
                w(f"        jw.String(BitfieldName_{c_ident(p.group)}({p.name}));")
            else:
                w(f"        jw.Uint({p.name});")
        elif base == "GLboolean":
            w(f"        jw.Boolean({p.name} != 0);")
        elif base in INT_TYPES:
            w(f"        jw.Int((int64_t){p.name});")
        elif base in UINT_TYPES:
            w(f"        jw.Uint((uint64_t){p.name});")
        elif base in FLOAT_TYPES:
            w(f"        jw.Double((double){p.name});")
        else:
            w(f"        jw.Pointer((const void*)(uintptr_t){p.name});")
        return
    # Pointers.
    if base == "GLchar" and "*const*" in p.decl.replace(" ", "") and c.name.startswith("glShaderSource"):
        w(f"        jw.String(JoinStrings({c.params[1].name}, {p.name}, {c.params[3].name}));")
        return
    if base == "GLchar" and p.const and (p.len is None or p.len.startswith("COMPSIZE(") and p.len != "COMPSIZE(buf,length)"
                                         or p.len == "length" and any(q.name == "length" and not q.pointer for q in c.params)):
        if p.len == "length" and any(q.name == "length" and not q.pointer for q in c.params):
            w(f"        WriteString(jw, {p.name}, length);")
        else:
            w(f"        WriteString(jw, {p.name}, -1);")
        return
    if base is None and "void" in p.type_text and p.const:
        # Data: bytes when the registry says how many.
        if n and not p.len.startswith("count"):
            w(f"        jw.Bytes({p.name}, {n});")
        else:
            w(f"        jw.Pointer({p.name});")
        return
    if not p.const:
        # Output pointers: glGen*'s names (registered by the Post hook first), else only the address.
        if p.cls and base == "GLuint" and n:
            w(f"        WriteRefs(jw, ObjType::{CLASS_TYPES[p.cls]}, \"GL{CLASS_TYPES[p.cls]}\", {p.name}, {n});")
        else:
            w(f"        jw.Pointer({p.name});")
        return
    if n and "*const*" not in p.decl.replace(" ", ""):
        if p.cls and base == "GLuint":
            w(f"        WriteRefs(jw, ObjType::{CLASS_TYPES[p.cls]}, \"GL{CLASS_TYPES[p.cls]}\", {p.name}, {n});")
        elif base == "GLenum":
            fn = f"EnumName_{c_ident(p.group)}" if p.group in enum_groups else "EnumName"
            w(f"        WriteEnums(jw, {fn}, {p.name}, {n});")
        elif base in FLOAT_TYPES or base in INT_TYPES or base in UINT_TYPES or base == "GLboolean":
            w(f"        WriteArray(jw, {p.name}, {n}, {MAX_ARRAY});")
        else:
            w(f"        jw.Pointer({p.name});")
        return
    w(f"        jw.Pointer({p.name});")


def write_hooks(out, cmds, recorded, hooked, groups, enum_groups):
    lines = []
    w = lines.append
    w("// Generated by tools/gen_gles.py from the Khronos registry (gl.xml). Do not edit.")
    w('#include "gles_api.gen.h"')
    w('#include "../src/runtime.h"')
    w("")
    w("namespace glesinsp {")
    w("")
    w("GlesDispatch g_gl{};")
    w("")
    w("const char* const kCommandNames[kCommandCount] = {")
    for c in cmds:
        w(f'    "{c.name}",')
    w("};")
    w("")
    w("namespace {")
    w("")
    for c in hooked:
        rec = c.name in recorded
        w(f"{c.ret} GL_APIENTRY Hook_{c.name}({c.sig()}) {{")
        if c.name in OVERRIDE_HOOKS:
            w(f"    return Override_{c.name}({c.args()});")
            w("}")
            w("")
            continue
        if rec:
            w("    Call call;")
            w(f'    const bool recording = BeginCall(call, "{c.name}");')
        if c.name in PRE_HOOKS:
            w(f"    Pre_{c.name}({c.args()});")
        if c.void:
            w(f"    g_gl.{c.name}({c.args()});")
        else:
            w(f"    {c.ret} result = g_gl.{c.name}({c.args()});")
        if c.name in POST_HOOKS:
            extra = "" if c.void else (", " if c.params else "") + "result"
            w(f"    Post_{c.name}({c.args()}{extra});")
        if rec:
            w("    if (recording) {")
            w("        JsonWriter& jw = call.args;")
            if not c.params and c.void:
                w("        (void)jw;")
            for p in c.params:
                serialize_param(p, c, lines, groups, enum_groups)
            if not c.void:
                w('        jw.Key("result");')
                if c.ret == "GLuint" and c.ret_class:
                    w(f'        jw.Ref(RefOf(ObjType::{CLASS_TYPES[c.ret_class]}, result), "GL{CLASS_TYPES[c.ret_class]}");')
                elif c.ret == "GLsync":
                    w('        jw.Ref(SyncRef(result), "GLSync");')
                elif c.ret == "GLenum":
                    w("        jw.Enum(EnumName(result), (int64_t)result);")
                elif c.ret == "GLboolean":
                    w("        jw.Boolean(result != 0);")
                elif c.ret in INT_TYPES:
                    w("        jw.Int((int64_t)result);")
                elif c.ret in UINT_TYPES:
                    w("        jw.Uint((uint64_t)result);")
                else:
                    w("        jw.Pointer((const void*)(uintptr_t)result);")
            w("        EndCall(call);")
            w("    }")
        if c.name in LATE_HOOKS:
            w(f"    Late_{c.name}({c.args()});")
        if not c.void:
            w("    return result;")
        w("}")
        w("")
    w("}  // namespace")
    w("")
    w("const HookEntry kHooks[kHookCount] = {")
    for c in hooked:
        w(f'    {{"{c.name}", (void*)&Hook_{c.name}, (void**)&g_gl.{c.name}}},')
    w("};")
    w("")
    w("}  // namespace glesinsp")
    w("")
    # Linux: the library is preloaded, and an application linked against libGLESv2 (or libGL) calls
    # whichever library exports a name first, so every hook is exported under the command's own name
    # (src/plugins/gles/src/platform_linux.cpp).
    w("#if defined(__linux__) && !defined(__ANDROID__)")
    w('extern "C" {')
    w("")
    for c in hooked:
        ret = "" if c.void else "return "
        w(f'__attribute__((visibility("default"))) {c.ret} {c.name}({c.sig()}) {{ {ret}glesinsp::Hook_{c.name}({c.args()}); }}')
    w("")
    w('}  // extern "C"')
    w("#endif")
    open(os.path.join(out, "gles_hooks.gen.cpp"), "w", newline="\n").write("\n".join(lines) + "\n")


def write_enums(out, values, groups, used_groups, bitmask_groups, es_enums):
    lines = []
    w = lines.append
    w("// Generated by tools/gen_gles.py from the Khronos registry (gl.xml). Do not edit.")
    w('#include "gles_api.gen.h"')
    w('#include "../src/runtime.h"')
    w("")
    w("namespace glesinsp {")
    w("")
    # Everything ES knows, one name per value.
    by_value = {}
    for name, v in values.items():
        if v > 0xFFFFFFFF or v < 0:
            continue
        by_value.setdefault(v, []).append(name)
    w("const char* EnumName(GLenum value) {")
    w("    switch (value) {")
    for v in sorted(by_value):
        names = [n for n in by_value[v] if n in es_enums] or by_value[v]
        # Tiny values are shared by booleans, points and bits; only an ES name is worth guessing there.
        if v <= 0x10 and not any(n in es_enums for n in by_value[v]):
            continue
        w(f'        case 0x{v:X}u: return "{best_name(names, es_enums)}";')
    w("        default: return nullptr;")
    w("    }")
    w("}")
    w("")
    for g in sorted(used_groups):
        names = groups.get(g, [])
        ident = c_ident(g)
        if g in bitmask_groups:
            bits = {}
            for n in names:
                v = values.get(n)
                if v is None or v == 0 or v & (v - 1) or v > 0xFFFFFFFF:
                    continue
                bits.setdefault(v, []).append(n)
            w(f"std::string BitfieldName_{ident}(GLbitfield value) {{")
            w("    std::string out;")
            w("    GLbitfield rest = value;")
            for v in sorted(bits):
                w(f'    if (value & 0x{v:X}u) {{ if (!out.empty()) out += " | "; out += "{best_name(bits[v], es_enums)}"; rest &= ~0x{v:X}u; }}')
            w("    if (rest || out.empty()) { if (!out.empty()) out += \" | \"; char buf[16]; snprintf(buf, sizeof(buf), \"0x%X\", rest); out += buf; }")
            w("    return out;")
            w("}")
        else:
            vals = {}
            for n in names:
                v = values.get(n)
                if v is None or v > 0xFFFFFFFF:
                    continue
                vals.setdefault(v, []).append(n)
            w(f"const char* EnumName_{ident}(GLenum value) {{")
            w("    switch (value) {")
            for v in sorted(vals):
                w(f'        case 0x{v:X}u: return "{best_name(vals[v], es_enums)}";')
            w("        default: return EnumName(value);")
            w("    }")
            w("}")
        w("")
    w("}  // namespace glesinsp")
    open(os.path.join(out, "gles_enums.gen.cpp"), "w", newline="\n").write("\n".join(lines) + "\n")

    decl = ["// Generated by tools/gen_gles.py from the Khronos registry (gl.xml). Do not edit.", "#pragma once", "",
            '#include "gles_api.gen.h"', "", "#include <string>", "", "namespace glesinsp {", "",
            "/** A GLenum's name, or null when ES has none for it. */",
            "const char* EnumName(GLenum value);"]
    for g in sorted(used_groups):
        ident = c_ident(g)
        if g in bitmask_groups:
            decl.append(f"std::string BitfieldName_{ident}(GLbitfield value);")
        else:
            decl.append(f"const char* EnumName_{ident}(GLenum value);")
    decl += ["", "}  // namespace glesinsp"]
    open(os.path.join(out, "gles_enums.gen.h"), "w", newline="\n").write("\n".join(decl) + "\n")


def write_constants(out, values, es_enums):
    """#define GL_... for every enum the registry has (outside other APIs'), as the headers would."""
    lines = ["// Generated by tools/gen_gles.py from the Khronos registry (gl.xml). Do not edit.", "#pragma once", ""]
    for name in sorted(values):
        v = values[name]
        if not name.startswith("GL_"):
            continue
        lines.append(f"#define {name} 0x{v:X}" + ("ull" if v > 0xFFFFFFFF else "u"))
    open(os.path.join(out, "gles_constants.gen.h"), "w", newline="\n").write("\n".join(lines) + "\n")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root_dir = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--registry", default=os.path.join(root_dir, "build", "khronos", "gl.xml"))
    ap.add_argument("--out", default=os.path.join(root_dir, "src", "plugins", "gles", "gen"))
    a = ap.parse_args()
    root = load_registry(a.registry)
    cmds, es_enums = gles_commands(root)
    values, groups = enum_tables(root, es_enums)
    bitmask_groups = BITMASK_GROUPS
    for block in root.findall("enums"):
        if block.get("type") == "bitmask":
            for e in block.findall("enum"):
                for g in (e.get("group") or "").split(","):
                    if g:
                        bitmask_groups.add(g)
    recorded = {c.name for c in cmds if not is_query(c.name)}
    hooked = [c for c in cmds if c.name in recorded or c.name in PRE_HOOKS or c.name in POST_HOOKS or c.name in LATE_HOOKS
              or c.name in OVERRIDE_HOOKS]
    used_groups = set()
    for c in hooked:
        if c.name not in recorded:
            continue
        for p in c.params:
            if p.group and p.group in groups and p.ptype in ("GLenum", "GLbitfield"):
                used_groups.add(p.group)
    enum_groups = used_groups
    os.makedirs(a.out, exist_ok=True)
    write_constants(a.out, values, es_enums)
    write_api_header(a.out, cmds, recorded, hooked)
    write_hooks(a.out, cmds, recorded, hooked, groups, enum_groups)
    write_enums(a.out, values, groups, used_groups, bitmask_groups, es_enums)
    print(f"{len(cmds)} commands, {len(hooked)} hooked ({len(recorded)} recorded), {len(used_groups)} enum groups -> {a.out}")


if __name__ == "__main__":
    main()
