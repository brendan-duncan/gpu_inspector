// What the library knows about the application's GL: its contexts and share groups, every object
// with the description the inspector shows (AddObject's args), and the per-context bindings the
// capture needs to know without asking the driver (which framebuffer draws, which pass is open).
//
// Everything here is guarded by one lock, State().mutex: GL applications do nearly all their work on
// one thread, and the ones that do not still serialize on their contexts.
#pragma once

#include "egl_api.h"
#include "runtime.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace glesinsp {

// ------------------------------------------------------------------------------------------------
// Logging (GLESINSP_LOG=1, GLESINSP_LOG_FILE=<path>)

bool LogEnabled();
void Log(const char* fmt, ...);
/** Logged whatever GLESINSP_LOG says: what the session's log should always show. */
void LogAlways(const char* fmt, ...);

// ------------------------------------------------------------------------------------------------
// Objects

/** A framebuffer attachment: what is attached where. */
struct Attachment {
    ObjType kind = ObjType::Texture;   // Texture or Renderbuffer
    GLuint name = 0;
    uint64_t id = 0;
    GLint level = 0;
    GLint layer = 0;
    /** A cube face target (GL_TEXTURE_CUBE_MAP_POSITIVE_X...), or 0. */
    GLenum face = 0;
};

/** One active uniform, attribute or block member of a linked program. */
struct ProgramVariable {
    std::string name;
    GLint location = -1;
    GLenum type = 0;
    GLint size = 1;
    GLint blockIndex = -1;
    GLint offset = -1;
    GLint arrayStride = 0;
    GLint matrixStride = 0;
    bool rowMajor = false;
};

struct ProgramBlock {
    std::string name;
    GLuint index = 0;
    GLint dataSize = 0;
    std::vector<ProgramVariable> members;
};

struct Object {
    uint64_t id = 0;
    ObjType kind = ObjType::Count;   // Count for the EGL objects (contexts, surfaces)
    std::string type;                // "GLTexture"
    uint64_t parent = 0;
    std::string cmd;                 // the call that made it
    std::string handle;              // the GL name, "0x..." for EGL handles
    std::string label;
    GLuint name = 0;
    /** What describes it, key -> JSON value, in the order first set: AddObject's args. */
    std::vector<std::pair<std::string, std::string>> args;

    // Buffers
    GLsizeiptr size = 0;
    /** Bumped by every write the library sees, so a read-back of an unchanged buffer is reused. */
    uint32_t generation = 0;
    /** ES 2 contexts, which cannot map a buffer for reading: the contents as the application wrote them. */
    std::vector<uint8_t> shadow;

    // Textures and renderbuffers
    GLenum target = 0;
    GLenum internalFormat = 0;
    GLint width = 0, height = 0, depth = 0;
    GLint levels = 0;
    GLint samples = 0;
    bool compressed = false;
    /** Compressed textures: level 0 as uploaded, per layer/face, since nothing reads compressed texels back. */
    std::vector<std::vector<uint8_t>> compressedLevel0;
    GLsizei compressedLevel0Size = 0;

    // Framebuffers
    std::map<GLenum, Attachment> attachments;

    // Shaders and programs
    GLenum shaderType = 0;
    std::string source;
    std::vector<GLuint> shaders;
    bool linked = false;
    std::vector<ProgramVariable> attributes;
    std::vector<ProgramVariable> uniforms;
    std::vector<ProgramBlock> blocks;
};

// ------------------------------------------------------------------------------------------------
// Contexts

struct ShareGroup {
    std::unordered_map<GLuint, uint64_t> names[(int)ObjType::Count];
};

struct Context {
    EGLContext handle = nullptr;
    EGLDisplay display = nullptr;
    uint64_t id = 0;
    std::shared_ptr<ShareGroup> share;
    /** Per-context names: vertex arrays, framebuffers, queries, transform feedbacks, pipelines. */
    std::unordered_map<GLuint, uint64_t> names[(int)ObjType::Count];
    int major = 2, minor = 0;
    /**
     * Made through WGL (an OpenGL ES profile context of the desktop driver) rather than EGL:
     * `handle` is its HGLRC, and `drawSurface` the HDC it was last made current on.
     */
    bool wgl = false;
    EGLSurface drawSurface = nullptr;
    /** GL_DRAW_FRAMEBUFFER (GL_FRAMEBUFFER in ES 2) and GL_READ_FRAMEBUFFER, as the application bound them. */
    GLuint drawFramebuffer = 0;
    GLuint readFramebuffer = 0;

    // The pass open on this context (capture.cpp).
    bool passOpen = false;
    GLuint passFramebuffer = 0;
    uint32_t passIndex = 0;
    /** The pass's attachments were read back already (before an invalidate threw them away). */
    bool passReadBack = false;
    /** How many passes this context began since its last swap: the next pass's index. */
    uint32_t nextPassIndex = 0;
    /** Debug groups open (glPushDebugGroup), and how many were open when the pass began. */
    int groupDepth = 0;
    int passGroupDepth = 0;
    /**
     * What the open pass did to its attachments that its BeginRenderPass should say, which is only known
     * later: the index of that command in the capture, the buffers cleared before its first draw (the
     * pass's "load op"), and the attachments invalidated (its "store op").
     */
    size_t passBeginCommand = 0;
    bool passDrawn = false;
    GLbitfield passCleared = 0;
    std::vector<GLenum> passInvalidated;

    /** Framebuffers of the library's own, for reading attachments and textures back (capture.cpp). */
    GLuint scratchFramebuffer = 0;
    GLuint resolveFramebuffer = 0;
    /** An error the application had not read yet when the library's own calls began (glGetError returns it). */
    GLenum savedError = 0;
    /**
     * How the context times passes (EXT_disjoint_timer_query): -1 not asked yet, 0 it cannot,
     * 1 timestamps, 2 elapsed-time queries only (ANGLE on Direct3D 11 has no timestamp counter).
     */
    int timestamps = -1;
    /** The timestamp query written where the open pass began. */
    GLuint passBeginQuery = 0;

    bool es3() const { return major >= 3; }
};

struct LibraryState {
    std::recursive_mutex mutex;
    std::unordered_map<uint64_t, Object> objects;
    std::unordered_map<EGLContext, std::unique_ptr<Context>> contexts;
    std::unordered_map<EGLSurface, uint64_t> surfaces;
    std::unordered_map<GLsync, uint64_t> syncs;
    uint64_t nextId = 1;
};

LibraryState& State();

/** The context current on this thread; one first seen here is taken on (made elsewhere, or before we were in). */
Context* Current();

/** What the default framebuffer draws into: the surface's object id, its size, and whether it is sRGB. */
struct Drawable {
    uint64_t id = 0;
    int width = 0, height = 0;
    bool srgb = false;
};
/** The current context's drawable, from EGL or WGL (hooks_egl.cpp, hooks_wgl.cpp). */
Drawable CurrentDrawable(Context* c);
Drawable EglDrawable(Context* c);
Drawable WglDrawable(Context* c);
/** An entry point by name from the context's own window system (eglGetProcAddress, wglGetProcAddress); null when it has none. */
void* LookupProc(Context* c, const char* name);
void* EglLookupProc(const char* name);
void* WglLookupProc(const char* name);
/** The surface object registered for an EGLSurface or an HDC; 0 when there is none. */
uint64_t SurfaceId(void* surface);
/** The context this thread has current, as far as the library knows, without taking one on. */
Context* CurrentKnown();
Context* ContextOf(EGLContext handle);
void SetCurrent(Context* c);

/** A new object (not yet announced). */
Object& NewObject(const char* type, ObjType kind, uint64_t parent, const char* cmd);
Object* FindObject(uint64_t id);
/** The object a GL name names in context `c` (its share group for shared kinds); null when unknown. */
Object* ObjectOf(Context* c, ObjType kind, GLuint name);
/** Registers a name the application made (glGen*, glCreate*), announcing it. */
uint64_t RegisterName(Context* c, ObjType kind, GLuint name, const char* cmd);
/** A name bound without being generated first, which ES 2 allows for buffers and textures. */
uint64_t EnsureName(Context* c, ObjType kind, GLuint name, const char* cmd);
void ForgetName(Context* c, ObjType kind, GLuint name);
/** Announces a new object to the inspector (AddObject), if one is connected. */
void Announce(const Object& o);
void Forget(uint64_t id);

/** Sets one field of an object's description and tells the inspector (ObjectUpdate). */
void Describe(Object& o, const std::string& key, const std::string& json);
void SetLabel(Object& o, const std::string& label);

/** Everything, for a newly connected inspector: Snapshot, then AddObject for every object. */
void SendSnapshot();

// ------------------------------------------------------------------------------------------------
// JSON helpers for descriptions

std::string JsonString(const std::string& s);
std::string JsonEnum(GLenum v);
/** An enum named by its group's table (gen/gles_enums.gen.h), the value itself when it has no name there. */
std::string JsonEnum(GLenum v, const char* (*nameOf)(GLenum));
std::string JsonRef(uint64_t id, const char* className);
inline std::string JsonInt(int64_t v) { return std::to_string(v); }
inline std::string JsonBool(bool v) { return v ? "true" : "false"; }

}  // namespace glesinsp
