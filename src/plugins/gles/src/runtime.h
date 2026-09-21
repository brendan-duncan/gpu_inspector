// What the generated hooks (gen/gles_hooks.gen.cpp) call: recording a command, naming objects and
// enums, writing arrays. The OpenGL ES capture library's own vocabulary, over the plugin SDK's JSON.
#pragma once

#include "../gen/gles_api.gen.h"
#include "../gen/gles_enums.gen.h"

#include <gpu_inspector/sdk/json.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace glesinsp {

using gpuinsp::sdk::JsonWriter;

/**
 * The kinds of object the library tracks. The first ones are shared between the contexts of a share
 * group (their names mean the same thing in each); the container objects from VertexArray on belong
 * to the one context that made them, as the GL specification has it.
 */
enum class ObjType : uint8_t {
    Buffer, Texture, Renderbuffer, Program, Shader, Sampler, Sync,
    VertexArray, Framebuffer, Query, TransformFeedback, ProgramPipeline,
    Count,
};

/** The type name an object of this kind is reported with ("GLTexture"). */
const char* TypeName(ObjType t);

/** Whether objects of this kind are shared across a share group, rather than per context. */
inline bool IsShared(ObjType t) { return t < ObjType::VertexArray; }

/** One call in flight: its arguments are written into `args` when a capture is recording it. */
struct Call {
    JsonWriter args;
    const char* name = nullptr;
    bool recording = false;
};

/** Whether this call is recorded (a capture records the current context); starts its arguments if so. */
bool BeginCall(Call& call, const char* name);
/** Hands the recorded call to the capture. */
void EndCall(Call& call);

/** The object id of a GL name in the current context (0 for the name 0, or a name never seen). */
uint64_t RefOf(ObjType type, GLuint name);
uint64_t SyncRef(GLsync sync);

void WriteRefs(JsonWriter& w, ObjType type, const char* className, const GLuint* names, size_t n);
void WriteEnums(JsonWriter& w, const char* (*nameOf)(GLenum), const GLenum* values, size_t n);
/** A GL string: null-terminated when `length` is negative. */
void WriteString(JsonWriter& w, const GLchar* s, GLsizei length);
/** glShaderSource's strings, joined as the compiler sees them. */
std::string JoinStrings(GLsizei count, const GLchar* const* strings, const GLint* lengths);

template <class T>
void WriteArray(JsonWriter& w, const T* values, size_t n, size_t max) {
    if (!values) { w.Null(); return; }
    if (n > max) { w.ArraySummary(n); return; }
    w.BeginArray();
    for (size_t i = 0; i < n; ++i) {
        if constexpr (std::is_floating_point_v<T>) w.Double((double)values[i]);
        else if constexpr (std::is_signed_v<T>) w.Int((int64_t)values[i]);
        else if constexpr (sizeof(T) == 1) w.Boolean(values[i] != 0);   // GLboolean
        else w.Uint((uint64_t)values[i]);
    }
    w.EndArray();
}

}  // namespace glesinsp
