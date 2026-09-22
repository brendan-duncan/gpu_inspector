#include "capture.h"

#include "formats.h"
#include "../gen/gles_constants.gen.h"

#include <gpu_inspector/sdk/transport.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>

namespace glesinsp {

using gpuinsp::sdk::JsonValue;
using gpuinsp::sdk::Server;

namespace {

// ------------------------------------------------------------------------------------------------
// What a capture holds

struct Recorded {
    uint32_t frame = 0;
    uint64_t context = 0;
    std::string method;
    std::string args;
    std::string state;
    bool synthetic = false;
};

struct TextureCapture {
    uint64_t id = 0;
    uint32_t frame = 0;
    uint64_t context = 0;
    uint32_t passIndex = 0;
    uint32_t attachment = 0;
    std::string format;
    int width = 0, height = 0, layers = 1, mip = 0;
    /** "" for a pass attachment, "sampled" for a texture a draw read. */
    std::string kind;
    uint32_t capture = 0;
    std::string error;
    std::vector<uint8_t> data;
};

struct BufferCapture {
    uint32_t id = 0;
    uint64_t buffer = 0;
    uint32_t frame = 0;
    uint64_t context = 0;
    int64_t offset = 0;
    size_t original = 0;
    std::string error;
    std::vector<uint8_t> data;
};

std::recursive_mutex g_mutex;
std::atomic<bool> g_recording{false};
std::atomic<bool> g_armed{false};
CaptureOptions g_requested;
CaptureOptions g_options;
uint64_t g_swaps = 0;
uint64_t g_startSwap = 0;
uint32_t g_frame = 0;
std::vector<Recorded> g_commands;
std::vector<TextureCapture> g_textures;
std::vector<BufferCapture> g_buffers;
size_t g_bufferBytes = 0;
size_t g_imageBytes = 0;
uint32_t g_nextTextureCapture = 1;
std::map<std::tuple<uint64_t, uint32_t, int64_t, size_t>, uint32_t> g_bufferSeen;
std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_textureSeen;

/**
 * A pass's timing queries, resolved once the frame is done (PassTimes): two timestamps, or with
 * `elapsed` one elapsed-time query in `begin`.
 */
struct PassQueries {
    uint64_t context = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    GLuint begin = 0, end = 0;
    bool elapsed = false;
};
std::vector<PassQueries> g_passQueries;

thread_local std::string t_pendingState;

/**
 * Read-backs copied on the GPU during the frame and read once it is over (ResolveCopies): reading
 * them where they are met would make the CPU wait for the GPU in the middle of a pass, which is both
 * slow and, with pass timings on, measured as part of the pass.
 */
struct PendingBuffer {
    uint32_t id = 0;              // the BufferCapture it fills
    std::shared_ptr<ShareGroup> share;
    GLuint copy = 0;
    size_t size = 0;
};
struct PendingTexture {
    size_t index = 0;             // into g_textures
    std::shared_ptr<ShareGroup> share;
    GLuint copy = 0;
    GLenum internalFormat = 0;
    int width = 0, height = 0, layers = 1;
};
std::vector<PendingBuffer> g_pendingBuffers;
std::vector<PendingTexture> g_pendingTextures;

// Frame statistics: the swap intervals since the last report.
std::chrono::steady_clock::time_point g_lastSwap;
std::chrono::steady_clock::time_point g_lastReport;
double g_sumMs = 0, g_minMs = 0, g_maxMs = 0;
uint32_t g_intervals = 0;

// ------------------------------------------------------------------------------------------------
// The library's own GL work

/**
 * Around the library's own GL calls: an error the application has not read yet is set aside for its
 * next glGetError (Override_glGetError), and the errors of our own calls are drained afterwards.
 */
class GlWork {
public:
    explicit GlWork(Context* c) : _c(c) {
        const GLenum e = g_gl.glGetError ? g_gl.glGetError() : GL_NO_ERROR;
        if (e != GL_NO_ERROR && !_c->savedError) _c->savedError = e;
    }
    ~GlWork() {
        if (!g_gl.glGetError) return;
        for (int i = 0; i < 32 && g_gl.glGetError() != GL_NO_ERROR; ++i) {}
    }

private:
    Context* _c;
};

GLint GetInt(GLenum pname, GLint def = 0) {
    GLint v = def;
    if (g_gl.glGetIntegerv) g_gl.glGetIntegerv(pname, &v);
    return v;
}

bool IsOn(GLenum cap) {
    return g_gl.glIsEnabled && g_gl.glIsEnabled(cap) != 0;
}

/** The bindings a read-back moves, put back as the application had them. */
struct Bindings {
    GLint drawFb = 0, readFb = 0, readBuffer = GL_BACK;
    GLint packBuffer = 0, packAlign = 4, rowLength = 0, skipRows = 0, skipPixels = 0;
    GLint renderbuffer = 0, copyRead = 0, copyWrite = 0;
    bool scissor = false;
    bool es3 = false;

    explicit Bindings(Context* c) : es3(c->es3()) {
        if (es3) {
            drawFb = GetInt(GL_DRAW_FRAMEBUFFER_BINDING);
            readFb = GetInt(GL_READ_FRAMEBUFFER_BINDING);
            packBuffer = GetInt(GL_PIXEL_PACK_BUFFER_BINDING);
            rowLength = GetInt(GL_PACK_ROW_LENGTH);
            skipRows = GetInt(GL_PACK_SKIP_ROWS);
            skipPixels = GetInt(GL_PACK_SKIP_PIXELS);
            copyRead = GetInt(GL_COPY_READ_BUFFER_BINDING);
            copyWrite = GetInt(GL_COPY_WRITE_BUFFER_BINDING);
        } else {
            drawFb = readFb = GetInt(GL_FRAMEBUFFER_BINDING);
        }
        packAlign = GetInt(GL_PACK_ALIGNMENT, 4);
        renderbuffer = GetInt(GL_RENDERBUFFER_BINDING);
        scissor = IsOn(GL_SCISSOR_TEST);
        if (es3) {
            if (packBuffer) g_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            if (rowLength) g_gl.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            if (skipRows) g_gl.glPixelStorei(GL_PACK_SKIP_ROWS, 0);
            if (skipPixels) g_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        }
        if (packAlign != 1) g_gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    }

    ~Bindings() {
        if (es3) {
            g_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)drawFb);
            g_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readFb);
            if (packBuffer) g_gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)packBuffer);
            if (rowLength) g_gl.glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
            if (skipRows) g_gl.glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
            if (skipPixels) g_gl.glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
            if (g_gl.glBindBuffer) {
                g_gl.glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)copyRead);
                g_gl.glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)copyWrite);
            }
        } else {
            g_gl.glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)drawFb);
        }
        if (packAlign != 1) g_gl.glPixelStorei(GL_PACK_ALIGNMENT, packAlign);
        g_gl.glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)renderbuffer);
        if (scissor) g_gl.glEnable(GL_SCISSOR_TEST); else g_gl.glDisable(GL_SCISSOR_TEST);
    }

    /** Makes `fb` the framebuffer glReadPixels reads (ES 2 has one binding for both). */
    void BindRead(GLuint fb) const {
        g_gl.glBindFramebuffer(es3 ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER, fb);
    }
};

/** Reads `width` x `height` texels of the read framebuffer's current read buffer; rows top first when `flip`. */
bool ReadPixels(const ReadFormat& rf, int width, int height, bool flip, std::vector<uint8_t>& out) {
    const size_t row = (size_t)width * (size_t)rf.bytesPerTexel;
    out.assign(row * (size_t)height, 0);
    g_gl.glReadPixels(0, 0, width, height, rf.format, rf.type, out.data());
    if (g_gl.glGetError && g_gl.glGetError() != GL_NO_ERROR) return false;
    if (flip) {
        // GL's rows start at the bottom; the inspector shows the first row at the top.
        std::vector<uint8_t> tmp(row);
        for (int y = 0; y < height / 2; ++y) {
            uint8_t* a = out.data() + (size_t)y * row;
            uint8_t* b = out.data() + (size_t)(height - 1 - y) * row;
            memcpy(tmp.data(), a, row);
            memcpy(a, b, row);
            memcpy(b, tmp.data(), row);
        }
    }
    return true;
}

GLuint Scratch(Context* c, bool resolve) {
    GLuint& fb = resolve ? c->resolveFramebuffer : c->scratchFramebuffer;
    if (!fb) g_gl.glGenFramebuffers(1, &fb);
    return fb;
}

/**
 * Attaches `a` (a texture level or layer, or a renderbuffer) as color attachment 0 of the library's own
 * framebuffer, and reads it: through a resolve first when it is multisampled. `layer` picks a layer of
 * an array or 3D texture and `face` a cube face (a GL_TEXTURE_CUBE_MAP_* target).
 */
bool ReadAttachment(Context* c, const Bindings& b, const Object& o, ObjType kind, GLint level, GLint layer, GLenum face,
                    int width, int height, bool flip, TextureCapture& out) {
    GLenum internal = o.internalFormat;
    const FormatInfo f = FormatOf(internal);
    const ReadFormat rf = ReadFormatOf(f);
    if (f.compressed) { out.error = "compressed textures are not read back from the GPU"; return false; }
    if (f.depth || f.stencil) { out.error = "OpenGL ES cannot read depth or stencil back (glReadPixels reads color only)"; return false; }
    if (!rf.format) { out.error = std::string("this format cannot be read back in OpenGL ES (") + f.vk + ")"; return false; }
    if (width <= 0 || height <= 0) { out.error = "the attachment has no size"; return false; }
    if (!c->es3() && rf.format != GL_RGBA) { out.error = "OpenGL ES 2 reads back 8-bit color only"; return false; }

    const GLuint fb = Scratch(c, false);
    b.BindRead(fb);
    const GLenum target = c->es3() ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER;
    if (kind == ObjType::Renderbuffer) {
        g_gl.glFramebufferRenderbuffer(target, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, o.name);
    } else if (o.target == GL_TEXTURE_2D_ARRAY || o.target == GL_TEXTURE_3D || o.target == GL_TEXTURE_CUBE_MAP_ARRAY) {
        if (!g_gl.glFramebufferTextureLayer) { out.error = "no glFramebufferTextureLayer"; return false; }
        g_gl.glFramebufferTextureLayer(target, GL_COLOR_ATTACHMENT0, o.name, level, layer);
    } else {
        g_gl.glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, face ? face : (o.target ? o.target : GL_TEXTURE_2D), o.name, level);
    }
    if (c->es3()) g_gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
    const GLenum status = g_gl.glCheckFramebufferStatus(target);
    bool ok = false;
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        char buf[96];
        snprintf(buf, sizeof(buf), "the attachment cannot be read (framebuffer status 0x%X)", status);
        out.error = buf;
    } else if (o.samples > 1 && c->es3()) {
        // Multisampled: resolved into a renderbuffer of our own first, as glReadPixels cannot read samples.
        GLuint rb = 0;
        g_gl.glGenRenderbuffers(1, &rb);
        g_gl.glBindRenderbuffer(GL_RENDERBUFFER, rb);
        g_gl.glRenderbufferStorage(GL_RENDERBUFFER, internal, width, height);
        const GLuint resolve = Scratch(c, true);
        g_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve);
        g_gl.glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        g_gl.glDisable(GL_SCISSOR_TEST);
        g_gl.glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        g_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve);
        g_gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
        ok = ReadPixels(rf, width, height, flip, out.data);
        g_gl.glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
        g_gl.glDeleteRenderbuffers(1, &rb);
        if (!ok) out.error = "glReadPixels failed after resolving the samples";
    } else {
        ok = ReadPixels(rf, width, height, flip, out.data);
        if (!ok) out.error = "glReadPixels failed";
    }
    // Leave nothing attached, so the library's framebuffer keeps no texture alive.
    b.BindRead(fb);
    if (kind == ObjType::Renderbuffer) g_gl.glFramebufferRenderbuffer(target, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
    else g_gl.glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    if (ok) {
        out.format = rf.vk;
        out.width = width;
        out.height = height;
    }
    return ok;
}

// ------------------------------------------------------------------------------------------------
// Buffers

bool HasExtension(const char* name);

size_t IndexBytes(GLenum type) {
    return type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
}

/** Reads a range of a buffer object: mapped for reading in ES 3, from the library's copy in ES 2. */
bool ReadBufferRange(Context* c, const Object& o, int64_t offset, size_t size, std::vector<uint8_t>& out, std::string& error) {
    if (offset < 0 || (o.size && offset >= (int64_t)o.size)) { error = "the range is outside the buffer"; return false; }
    if (o.size && offset + (int64_t)size > (int64_t)o.size) size = (size_t)(o.size - offset);
    if (!size) { error = "the range is empty"; return false; }
    if (!c->es3() || !g_gl.glMapBufferRange) {
        if (o.shadow.size() < (size_t)offset + size) { error = "OpenGL ES 2 cannot read a buffer back, and its contents were not all seen"; return false; }
        out.assign(o.shadow.begin() + offset, o.shadow.begin() + offset + (int64_t)size);
        return true;
    }
    g_gl.glBindBuffer(GL_COPY_READ_BUFFER, o.name);
    GLint mapped = 0;
    g_gl.glGetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_MAPPED, &mapped);
    if (mapped) { error = "the application has the buffer mapped"; return false; }
    const void* p = g_gl.glMapBufferRange(GL_COPY_READ_BUFFER, (GLintptr)offset, (GLsizeiptr)size, GL_MAP_READ_BIT);
    if (!p) { error = "the buffer could not be mapped for reading"; return false; }
    out.assign((const uint8_t*)p, (const uint8_t*)p + size);
    g_gl.glUnmapBuffer(GL_COPY_READ_BUFFER);
    return true;
}

/**
 * A range of a buffer object copied into a buffer of the library's own, to be read once the frame is
 * over. False when the context cannot copy one (ES 2), and the range is read at once instead.
 */
bool CopyBufferRange(Context* c, const Object& o, int64_t offset, size_t& size, BufferCapture& b) {
    if (!c->es3() || !g_gl.glCopyBufferSubData || !g_gl.glMapBufferRange) return false;
    g_gl.glBindBuffer(GL_COPY_READ_BUFFER, o.name);
    GLint real = 0, mapped = 0;
    g_gl.glGetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &real);
    g_gl.glGetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_MAPPED, &mapped);
    if (mapped) { b.error = "the application has the buffer mapped"; return true; }
    if (offset < 0 || offset >= real) { b.error = "the range is outside the buffer"; return true; }
    size = std::min(size, (size_t)(real - offset));
    if (!size) { b.error = "the range is empty"; return true; }
    PendingBuffer pending;
    pending.id = b.id;
    pending.share = c->share;
    pending.size = size;
    g_gl.glGenBuffers(1, &pending.copy);
    g_gl.glBindBuffer(GL_COPY_WRITE_BUFFER, pending.copy);
    g_gl.glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)size, nullptr, GL_STREAM_READ);
    g_gl.glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, (GLintptr)offset, 0, (GLsizeiptr)size);
    g_pendingBuffers.push_back(std::move(pending));
    return true;
}

/**
 * A range of buffer `name` (or, with name 0, client memory at `client`) read back once per contents,
 * truncated to the capture's limit; its capture id, which the snapshot names it by.
 */
uint32_t CaptureBuffer(Context* c, GLuint name, int64_t offset, size_t size, const void* client = nullptr) {
    if (!g_options.captureBuffers || size == 0) return 0;
    Object* o = name ? ObjectOf(c, ObjType::Buffer, name) : nullptr;
    const uint64_t id = o ? o->id : 0;
    const size_t original = size;
    size = std::min(size, g_options.maxBufferSize);
    const auto key = std::make_tuple(id, o ? o->generation : 0u, name ? offset : (int64_t)(intptr_t)client, size);
    if (name) {
        auto it = g_bufferSeen.find(key);
        if (it != g_bufferSeen.end()) return it->second;
    }
    BufferCapture b;
    b.id = (uint32_t)g_buffers.size() + 1;
    b.buffer = id;
    b.frame = g_frame;
    b.context = c->id;
    b.offset = name ? offset : 0;
    b.original = original > size ? original : 0;
    if (g_bufferBytes + size > g_options.maxBufferTotal) {
        b.error = "the capture's buffer budget is spent";
    } else if (name && !o) {
        b.error = "a buffer the library does not know";
    } else if (!name) {
        if (!client) b.error = "no buffer bound and no client pointer";
        else b.data.assign((const uint8_t*)client, (const uint8_t*)client + size);
    } else if (!CopyBufferRange(c, *o, offset, size, b)) {
        ReadBufferRange(c, *o, offset, size, b.data, b.error);
    }
    g_bufferBytes += b.error.empty() ? size : 0;
    g_buffers.push_back(std::move(b));
    if (name) g_bufferSeen[key] = g_buffers.back().id;
    return g_buffers.back().id;
}

// ------------------------------------------------------------------------------------------------
// Commands and passes

void Append(Context* c, const char* method, std::string args, std::string state, bool synthetic) {
    Recorded r;
    r.frame = g_frame;
    r.context = c ? c->id : 0;
    r.method = method;
    r.args = std::move(args);
    r.state = std::move(state);
    r.synthetic = synthetic;
    g_commands.push_back(std::move(r));
}

/** The draw framebuffer's color attachments, as the pass's arguments name them. */
void WriteAttachments(Context* c, GLuint fb, gpuinsp::sdk::JsonWriter& w) {
    w.Key("attachments");
    w.BeginArray();
    if (Object* o = ObjectOf(c, ObjType::Framebuffer, fb)) {
        for (const auto& [slot, a] : o->attachments) {
            w.BeginObject();
            w.Key("attachment"); w.Enum(EnumName_FramebufferAttachment(slot), slot);
            w.Key("object"); w.Ref(a.id, TypeName(a.kind));
            w.Key("level"); w.Int(a.level);
            if (a.layer) { w.Key("layer"); w.Int(a.layer); }
            if (Object* t = FindObject(a.id)) {
                w.Key("format"); w.String(FormatOf(t->internalFormat).vk);
                w.Key("width"); w.Int(std::max(1, t->width >> a.level));
                w.Key("height"); w.Int(std::max(1, t->height >> a.level));
            }
            w.EndObject();
        }
    }
    w.EndArray();
}

void ReadPassAttachments(Context* c) {
    if (!g_options.captureTextures || c->passReadBack) return;
    c->passReadBack = true;
    GlWork work(c);
    Bindings b(c);
    if (c->passFramebuffer == 0) {
        // The surface: its back buffer, read before the swap makes it undefined.
        TextureCapture t;
        t.frame = g_frame;
        t.context = c->id;
        t.passIndex = c->passIndex;
        t.attachment = 0;
        const Drawable surface = CurrentDrawable(c);
        const int w = surface.width, h = surface.height;
        t.id = surface.id;
        const FormatInfo f = FormatOf(surface.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8);
        b.BindRead(0);
        GLint readBuffer = GL_BACK;
        if (c->es3()) {
            readBuffer = GetInt(GL_READ_BUFFER, GL_BACK);
            g_gl.glReadBuffer(GL_BACK);
        }
        const ReadFormat rf = ReadFormatOf(f);
        if (w > 0 && h > 0 && ReadPixels(rf, w, h, true, t.data)) {
            t.format = rf.vk;
            t.width = w;
            t.height = h;
        } else {
            t.error = "the surface could not be read back";
            t.width = w;
            t.height = h;
            t.format = rf.vk;
        }
        if (c->es3()) g_gl.glReadBuffer((GLenum)readBuffer);
        g_imageBytes += t.data.size();
        g_textures.push_back(std::move(t));
        return;
    }
    Object* fbo = ObjectOf(c, ObjType::Framebuffer, c->passFramebuffer);
    if (!fbo) return;
    uint32_t index = 0;
    for (const auto& [slot, a] : fbo->attachments) {
        if (slot < GL_COLOR_ATTACHMENT0 || slot > GL_COLOR_ATTACHMENT0 + 15) continue;
        Object* target = FindObject(a.id);
        TextureCapture t;
        t.id = a.id;
        t.frame = g_frame;
        t.context = c->id;
        t.passIndex = c->passIndex;
        t.attachment = slot - GL_COLOR_ATTACHMENT0;
        t.mip = a.level;
        ++index;
        if (!target) {
            t.error = "the attachment is not an object the library knows";
        } else if (g_imageBytes > g_options.maxImageTotal) {
            t.error = "the capture's image budget is spent";
        } else {
            const int w = std::max(1, target->width >> a.level);
            const int h = std::max(1, target->height >> a.level);
            t.width = w;
            t.height = h;
            t.format = FormatOf(target->internalFormat).vk;
            ReadAttachment(c, b, *target, a.kind, a.level, a.layer, a.face, w, h, true, t);
        }
        g_imageBytes += t.data.size();
        g_textures.push_back(std::move(t));
    }
    (void)index;
}

/** The BeginRenderPass command's arguments: the framebuffer, its attachments, and what the pass did to them. */
std::string PassArgs(Context* c) {
    gpuinsp::sdk::JsonWriter w;
    w.BeginObject();
    w.Key("framebuffer"); w.Ref(c->passFramebuffer ? RefOf(ObjType::Framebuffer, c->passFramebuffer) : 0, "GLFramebuffer");
    if (c->passFramebuffer) {
        WriteAttachments(c, c->passFramebuffer, w);
    } else {
        const Drawable surface = CurrentDrawable(c);
        w.Key("surface"); w.Ref(surface.id, "GLSurface");
        w.Key("width"); w.Int(surface.width);
        w.Key("height"); w.Int(surface.height);
    }
    w.Key("passIndex"); w.Uint(c->passIndex);
    // Cleared before anything drew: what the pass started from does not matter (a load op of CLEAR).
    if (c->passCleared) {
        w.Key("cleared");
        w.BeginArray();
        if (c->passCleared & GL_COLOR_BUFFER_BIT) w.String("color");
        if (c->passCleared & GL_DEPTH_BUFFER_BIT) w.String("depth");
        if (c->passCleared & GL_STENCIL_BUFFER_BIT) w.String("stencil");
        w.EndArray();
    }
    // Invalidated: what the pass drew there is thrown away (a store op of DONT_CARE).
    if (!c->passInvalidated.empty()) {
        w.Key("invalidated");
        w.BeginArray();
        for (GLenum a : c->passInvalidated) w.Enum(EnumName_InvalidateFramebufferAttachment(a), a);
        w.EndArray();
    }
    w.Key("synthetic"); w.Boolean(true);
    w.EndObject();
    return w.str();
}

/**
 * Whether the current context has an extension: glGetStringi's list (OpenGL ES 3, and all a desktop
 * driver's ES context may answer), else the one string ES 2 has.
 */
bool HasExtension(const char* name) {
    GLint count = 0;
    if (g_gl.glGetStringi && g_gl.glGetIntegerv) {
        g_gl.glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; ++i) {
            const char* e = (const char*)g_gl.glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (e && strcmp(e, name) == 0) return true;
        }
        if (count > 0) return false;
    }
    const char* all = g_gl.glGetString ? (const char*)g_gl.glGetString(GL_EXTENSIONS) : nullptr;
    return all && strstr(all, name);
}

/**
 * The timer query entry points, by whichever name the driver has them: EXT_disjoint_timer_query's
 * suffixed ones (ANGLE), OpenGL ES 3's core ones for what it has, and the desktop names a desktop
 * driver's ES context answers to (NVIDIA's has no EXT spellings at all).
 */
struct TimerProcs {
    PFN_glGenQueries gen = nullptr;
    PFN_glDeleteQueries del = nullptr;
    PFN_glBeginQuery begin = nullptr;
    PFN_glEndQuery end = nullptr;
    PFN_glGetQueryiv queryiv = nullptr;
    PFN_glQueryCounterEXT counter = nullptr;
    PFN_glGetQueryObjectui64vEXT result64 = nullptr;
};
TimerProcs g_timer;

template <class T>
T First(T a, T b, Context* c, const char* name) {
    if (a) return a;
    if (b) return b;
    return (T)LookupProc(c, name);
}

/**
 * How the context can time a pass (Context::timestamps): EXT_disjoint_timer_query, with timestamps
 * when its counter has bits and with elapsed-time queries when it has none. Asked once per context.
 */
int TimingMode(Context* c) {
    if (c->timestamps >= 0) return c->timestamps;
    c->timestamps = 0;
    if (!HasExtension("GL_EXT_disjoint_timer_query")) {
        LogAlways("pass timings: the context has no EXT_disjoint_timer_query");
        return 0;
    }
    TimerProcs& t = g_timer;
    t.gen = First<PFN_glGenQueries>(g_gl.glGenQueriesEXT, g_gl.glGenQueries, c, "glGenQueriesEXT");
    t.del = First<PFN_glDeleteQueries>(g_gl.glDeleteQueriesEXT, g_gl.glDeleteQueries, c, "glDeleteQueriesEXT");
    t.begin = First<PFN_glBeginQuery>(g_gl.glBeginQueryEXT, g_gl.glBeginQuery, c, "glBeginQueryEXT");
    t.end = First<PFN_glEndQuery>(g_gl.glEndQueryEXT, g_gl.glEndQuery, c, "glEndQueryEXT");
    t.queryiv = First<PFN_glGetQueryiv>(g_gl.glGetQueryivEXT, g_gl.glGetQueryiv, c, "glGetQueryivEXT");
    t.counter = First<PFN_glQueryCounterEXT>(g_gl.glQueryCounterEXT, (PFN_glQueryCounterEXT)LookupProc(c, "glQueryCounter"), c, "glQueryCounterEXT");
    t.result64 = First<PFN_glGetQueryObjectui64vEXT>(g_gl.glGetQueryObjectui64vEXT, (PFN_glGetQueryObjectui64vEXT)LookupProc(c, "glGetQueryObjectui64v"), c, "glGetQueryObjectui64vEXT");
    if (!t.gen || !t.del || !t.result64) {
        LogAlways("pass timings: the driver hands out no query entry points");
        return 0;
    }
    GLint bits = 0;
    if (t.queryiv) t.queryiv(GL_TIMESTAMP_EXT, GL_QUERY_COUNTER_BITS_EXT, &bits);
    if (bits > 0 && t.counter) c->timestamps = 1;
    else if (t.begin && t.end) c->timestamps = 2;
    LogAlways("pass timings: %s", c->timestamps == 1 ? "timestamps" : c->timestamps == 2 ? "elapsed-time queries (no timestamp counter)" : "none");
    return c->timestamps;
}

/** Starts timing the pass beginning here: a timestamp, or an elapsed-time query begun; 0 when it cannot be timed. */
GLuint BeginPassTiming(Context* c) {
    if (!g_options.profilePasses) return 0;
    const int mode = TimingMode(c);
    if (!mode) return 0;
    GlWork work(c);
    // One elapsed-time query at a time: when the application has one running, the pass goes untimed.
    GLint active = 0;
    if (mode == 2 && g_timer.queryiv) g_timer.queryiv(GL_TIME_ELAPSED_EXT, GL_CURRENT_QUERY_EXT, &active);
    if (active) return 0;
    GLuint q = 0;
    g_timer.gen(1, &q);
    if (mode == 1) g_timer.counter(q, GL_TIMESTAMP_EXT);
    else g_timer.begin(GL_TIME_ELAPSED_EXT, q);
    return q;
}

void EndPass(Context* c) {
    if (!c->passOpen) return;
    // The end of the pass's own work, before the library reads its targets back.
    if (c->passBeginQuery) {
        PassQueries q;
        q.context = c->id;
        q.frame = g_frame;
        q.passIndex = c->passIndex;
        q.begin = c->passBeginQuery;
        q.elapsed = TimingMode(c) == 2;
        {
            GlWork work(c);
            if (q.elapsed) {
                g_timer.end(GL_TIME_ELAPSED_EXT);
            } else {
                g_timer.gen(1, &q.end);
                g_timer.counter(q.end, GL_TIMESTAMP_EXT);
            }
        }
        g_passQueries.push_back(q);
        c->passBeginQuery = 0;
    }
    ReadPassAttachments(c);
    // What the pass cleared and invalidated is known now: its BeginRenderPass says so.
    if ((c->passCleared || !c->passInvalidated.empty()) && c->passBeginCommand < g_commands.size()) {
        g_commands[c->passBeginCommand].args = PassArgs(c);
    }
    gpuinsp::sdk::JsonWriter w;
    w.BeginObject();
    w.Key("framebuffer"); w.Ref(c->passFramebuffer ? RefOf(ObjType::Framebuffer, c->passFramebuffer) : 0, "GLFramebuffer");
    w.Key("synthetic"); w.Boolean(true);
    w.EndObject();
    Append(c, "EndRenderPass", w.str(), "", true);
    c->passOpen = false;
    if (c->passFramebuffer == 0) c->surfacePassEnded = true;
}

void BeginPassIfNeeded(Context* c) {
    if (c->passOpen && c->passFramebuffer == c->drawFramebuffer) return;
    EndPass(c);
    c->passOpen = true;
    c->passReadBack = false;
    c->passFramebuffer = c->drawFramebuffer;
    c->passIndex = c->nextPassIndex++;
    c->passGroupDepth = c->groupDepth;
    c->passDrawn = false;
    c->passCleared = 0;
    c->passInvalidated.clear();
    c->passBeginCommand = g_commands.size();
    if (c->passFramebuffer == 0) c->surfacePassEnded = false;
    Append(c, "BeginRenderPass", PassArgs(c), "", true);
    c->passBeginQuery = BeginPassTiming(c);
}

// ------------------------------------------------------------------------------------------------
// Uniform types

struct UniformType {
    GLenum type;
    const char* glsl;
    int components;
    char base;         // 'f' float, 'i' int, 'u' uint, 'b' bool, 's' sampler
    GLenum textureBinding;   // samplers: the binding query of the target they sample
    GLenum target;
};

const UniformType kUniformTypes[] = {
    {GL_FLOAT, "float", 1, 'f', 0, 0}, {GL_FLOAT_VEC2, "vec2", 2, 'f', 0, 0}, {GL_FLOAT_VEC3, "vec3", 3, 'f', 0, 0},
    {GL_FLOAT_VEC4, "vec4", 4, 'f', 0, 0}, {GL_INT, "int", 1, 'i', 0, 0}, {GL_INT_VEC2, "ivec2", 2, 'i', 0, 0},
    {GL_INT_VEC3, "ivec3", 3, 'i', 0, 0}, {GL_INT_VEC4, "ivec4", 4, 'i', 0, 0}, {GL_UNSIGNED_INT, "uint", 1, 'u', 0, 0},
    {GL_UNSIGNED_INT_VEC2, "uvec2", 2, 'u', 0, 0}, {GL_UNSIGNED_INT_VEC3, "uvec3", 3, 'u', 0, 0},
    {GL_UNSIGNED_INT_VEC4, "uvec4", 4, 'u', 0, 0}, {GL_BOOL, "bool", 1, 'b', 0, 0}, {GL_BOOL_VEC2, "bvec2", 2, 'b', 0, 0},
    {GL_BOOL_VEC3, "bvec3", 3, 'b', 0, 0}, {GL_BOOL_VEC4, "bvec4", 4, 'b', 0, 0},
    {GL_FLOAT_MAT2, "mat2", 4, 'f', 0, 0}, {GL_FLOAT_MAT3, "mat3", 9, 'f', 0, 0}, {GL_FLOAT_MAT4, "mat4", 16, 'f', 0, 0},
    {GL_FLOAT_MAT2x3, "mat2x3", 6, 'f', 0, 0}, {GL_FLOAT_MAT2x4, "mat2x4", 8, 'f', 0, 0}, {GL_FLOAT_MAT3x2, "mat3x2", 6, 'f', 0, 0},
    {GL_FLOAT_MAT3x4, "mat3x4", 12, 'f', 0, 0}, {GL_FLOAT_MAT4x2, "mat4x2", 8, 'f', 0, 0}, {GL_FLOAT_MAT4x3, "mat4x3", 12, 'f', 0, 0},
    {GL_SAMPLER_2D, "sampler2D", 1, 's', GL_TEXTURE_BINDING_2D, GL_TEXTURE_2D},
    {GL_SAMPLER_2D_SHADOW, "sampler2DShadow", 1, 's', GL_TEXTURE_BINDING_2D, GL_TEXTURE_2D},
    {GL_INT_SAMPLER_2D, "isampler2D", 1, 's', GL_TEXTURE_BINDING_2D, GL_TEXTURE_2D},
    {GL_UNSIGNED_INT_SAMPLER_2D, "usampler2D", 1, 's', GL_TEXTURE_BINDING_2D, GL_TEXTURE_2D},
    {GL_SAMPLER_3D, "sampler3D", 1, 's', GL_TEXTURE_BINDING_3D, GL_TEXTURE_3D},
    {GL_INT_SAMPLER_3D, "isampler3D", 1, 's', GL_TEXTURE_BINDING_3D, GL_TEXTURE_3D},
    {GL_UNSIGNED_INT_SAMPLER_3D, "usampler3D", 1, 's', GL_TEXTURE_BINDING_3D, GL_TEXTURE_3D},
    {GL_SAMPLER_CUBE, "samplerCube", 1, 's', GL_TEXTURE_BINDING_CUBE_MAP, GL_TEXTURE_CUBE_MAP},
    {GL_SAMPLER_CUBE_SHADOW, "samplerCubeShadow", 1, 's', GL_TEXTURE_BINDING_CUBE_MAP, GL_TEXTURE_CUBE_MAP},
    {GL_INT_SAMPLER_CUBE, "isamplerCube", 1, 's', GL_TEXTURE_BINDING_CUBE_MAP, GL_TEXTURE_CUBE_MAP},
    {GL_UNSIGNED_INT_SAMPLER_CUBE, "usamplerCube", 1, 's', GL_TEXTURE_BINDING_CUBE_MAP, GL_TEXTURE_CUBE_MAP},
    {GL_SAMPLER_2D_ARRAY, "sampler2DArray", 1, 's', GL_TEXTURE_BINDING_2D_ARRAY, GL_TEXTURE_2D_ARRAY},
    {GL_SAMPLER_2D_ARRAY_SHADOW, "sampler2DArrayShadow", 1, 's', GL_TEXTURE_BINDING_2D_ARRAY, GL_TEXTURE_2D_ARRAY},
    {GL_INT_SAMPLER_2D_ARRAY, "isampler2DArray", 1, 's', GL_TEXTURE_BINDING_2D_ARRAY, GL_TEXTURE_2D_ARRAY},
    {GL_UNSIGNED_INT_SAMPLER_2D_ARRAY, "usampler2DArray", 1, 's', GL_TEXTURE_BINDING_2D_ARRAY, GL_TEXTURE_2D_ARRAY},
    {GL_SAMPLER_EXTERNAL_OES, "samplerExternalOES", 1, 's', GL_TEXTURE_BINDING_EXTERNAL_OES, GL_TEXTURE_EXTERNAL_OES},
    {GL_SAMPLER_2D_MULTISAMPLE, "sampler2DMS", 1, 's', GL_TEXTURE_BINDING_2D_MULTISAMPLE, GL_TEXTURE_2D_MULTISAMPLE},
    {GL_SAMPLER_CUBE_MAP_ARRAY, "samplerCubeArray", 1, 's', GL_TEXTURE_BINDING_CUBE_MAP_ARRAY, GL_TEXTURE_CUBE_MAP_ARRAY},
    {GL_SAMPLER_BUFFER, "samplerBuffer", 1, 's', GL_TEXTURE_BINDING_BUFFER, GL_TEXTURE_BUFFER},
};

const UniformType* UniformTypeOf(GLenum type) {
    for (const UniformType& t : kUniformTypes) if (t.type == type) return &t;
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
// Textures

/**
 * Whether the context copies between images (glCopyImageSubData: OpenGL ES 3.2, or EXT_copy_image or
 * OES_copy_image), and the entry point it does it with.
 */
PFN_glCopyImageSubData CopyImageProc(Context* c) {
    if (c->copyImage < 0) {
        c->copyImage = 0;
        if (c->es3() && ((c->major > 3 || c->minor >= 2) || HasExtension("GL_EXT_copy_image") || HasExtension("GL_OES_copy_image"))) {
            c->copyImage = g_gl.glCopyImageSubData || g_gl.glCopyImageSubDataEXT || g_gl.glCopyImageSubDataOES ? 1 : 0;
        }
    }
    if (!c->copyImage) return nullptr;
    return g_gl.glCopyImageSubData ? g_gl.glCopyImageSubData : g_gl.glCopyImageSubDataEXT ? (PFN_glCopyImageSubData)g_gl.glCopyImageSubDataEXT
         : (PFN_glCopyImageSubData)g_gl.glCopyImageSubDataOES;
}

/**
 * A texture's level 0 copied into a 2D array texture of the library's own, a layer per layer or cube
 * face, to be read once the frame is over. False when it cannot be (no copy-image, a format that
 * could not be read back from the copy either), and the texture is read at once instead.
 */
bool CopyTextureLevel(Context* c, const Object& tex, int layers, size_t index) {
    PFN_glCopyImageSubData copy = CopyImageProc(c);
    if (!copy || !ReadFormatOf(FormatOf(tex.internalFormat)).format || !g_gl.glTexStorage3D) return false;
    const GLint previous = GetInt(GL_TEXTURE_BINDING_2D_ARRAY);
    PendingTexture pending;
    pending.index = index;
    pending.share = c->share;
    pending.internalFormat = tex.internalFormat;
    pending.width = tex.width;
    pending.height = tex.height;
    pending.layers = layers;
    g_gl.glGenTextures(1, &pending.copy);
    g_gl.glBindTexture(GL_TEXTURE_2D_ARRAY, pending.copy);
    g_gl.glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, tex.internalFormat, tex.width, tex.height, layers);
    g_gl.glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)previous);
    // A cube map's faces are its layers to glCopyImageSubData, as an array's are.
    copy(tex.name, tex.target ? tex.target : GL_TEXTURE_2D, 0, 0, 0, 0, pending.copy, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, tex.width, tex.height, layers);
    if (g_gl.glGetError() != GL_NO_ERROR) {
        g_gl.glDeleteTextures(1, &pending.copy);
        return false;
    }
    g_pendingTextures.push_back(std::move(pending));
    return true;
}

/** A texture a draw samples, read back once per capture: all its layers (or cube faces) at its base level. */
uint32_t CaptureTexture(Context* c, const Bindings& b, Object& tex) {
    if (!g_options.captureImages) return 0;
    const auto key = std::make_pair(tex.id, tex.generation);
    auto it = g_textureSeen.find(key);
    if (it != g_textureSeen.end()) return it->second;
    TextureCapture t;
    t.id = tex.id;
    t.frame = g_frame;
    t.context = c->id;
    t.passIndex = c->passOpen ? c->passIndex : 0;
    t.kind = "sampled";
    t.capture = g_nextTextureCapture++;
    t.width = tex.width;
    t.height = tex.height;
    const FormatInfo f = FormatOf(tex.internalFormat);
    t.format = f.vk;
    const bool cube = tex.target == GL_TEXTURE_CUBE_MAP;
    const int layers = cube ? 6 : (tex.target == GL_TEXTURE_2D_ARRAY || tex.target == GL_TEXTURE_3D) ? std::max(1, tex.depth) : 1;
    t.layers = layers;
    if (g_imageBytes > g_options.maxImageTotal) {
        t.error = "the capture's image budget is spent";
    } else if (f.compressed) {
        // Nothing reads compressed texels back from the GPU: the level as the application uploaded it.
        if (!tex.compressedLevel0.empty() && tex.compressedLevel0.size() == (size_t)layers) {
            for (const auto& layer : tex.compressedLevel0) t.data.insert(t.data.end(), layer.begin(), layer.end());
        } else {
            t.error = "the texture's compressed data was not seen being uploaded";
        }
    } else if (tex.target == GL_TEXTURE_EXTERNAL_OES) {
        t.error = "an external texture (an EGLImage from a camera or video) cannot be attached to read it back";
    } else if (tex.width <= 0 || tex.height <= 0) {
        t.error = "the texture has no storage";
    } else if (CopyTextureLevel(c, tex, layers, g_textures.size())) {
        // Read once the frame is over; what it will take counts against the budget now.
        const ReadFormat rf = ReadFormatOf(f);
        t.format = rf.vk;
        g_imageBytes += (size_t)tex.width * (size_t)tex.height * (size_t)layers * (size_t)rf.bytesPerTexel;
    } else {
        for (int layer = 0; layer < layers && t.error.empty(); ++layer) {
            TextureCapture part;
            const GLenum face = cube ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer) : 0;
            // Rows top first, as a render target's are: a texture rendered to reads the same both ways.
            if (ReadAttachment(c, b, tex, ObjType::Texture, 0, layer, face, tex.width, tex.height, true, part)) {
                t.format = part.format;
                t.data.insert(t.data.end(), part.data.begin(), part.data.end());
            } else {
                t.error = part.error;
                t.data.clear();
            }
        }
    }
    g_imageBytes += t.data.size();
    const uint32_t id = t.capture;
    g_textures.push_back(std::move(t));
    g_textureSeen[key] = id;
    return id;
}

// ------------------------------------------------------------------------------------------------
// The state at a draw

size_t AttribTypeBytes(GLenum type) {
    switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
        case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES: return 2;
        default: return 4;
    }
}

bool PackedAttrib(GLenum type) {
    return type == GL_INT_2_10_10_10_REV || type == GL_UNSIGNED_INT_2_10_10_10_REV;
}

void WriteInts(gpuinsp::sdk::JsonWriter& w, const GLint* v, int n) {
    w.BeginArray();
    for (int i = 0; i < n; ++i) w.Int(v[i]);
    w.EndArray();
}

void WriteFloats(gpuinsp::sdk::JsonWriter& w, const GLfloat* v, int n) {
    w.BeginArray();
    for (int i = 0; i < n; ++i) w.Double(v[i]);
    w.EndArray();
}

/**
 * An enum by name. Small values name a dozen things each (1 is GL_ONE, GL_LINES and GL_TRUE), so the
 * table of the value's group is what says which: EnumName, the table of every name, is the fallback.
 */
void WriteEnumValue(gpuinsp::sdk::JsonWriter& w, GLenum v, const char* (*nameOf)(GLenum) = EnumName) {
    w.Enum(nameOf(v), v);
}

/** Indices read from `data`: their lowest and highest, for the range of vertices the draw reads. */
bool IndexRange(const std::vector<uint8_t>& data, GLenum type, size_t count, uint32_t& lo, uint32_t& hi) {
    const size_t size = IndexBytes(type);
    const size_t n = std::min(count, data.size() / size);
    if (!n) return false;
    lo = UINT32_MAX;
    hi = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = 0;
        memcpy(&v, data.data() + i * size, size);
        // The fixed primitive restart index is not a vertex.
        if ((size == 1 && v == 0xFF) || (size == 2 && v == 0xFFFF) || (size == 4 && v == 0xFFFFFFFFu)) continue;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    return lo <= hi;
}

void WriteVertexInput(Context* c, Object* prog, const DrawParams& p, gpuinsp::sdk::JsonWriter& w) {
    const bool es3 = c->es3();
    if (es3 || g_gl.glBindVertexArrayOES) {
        w.Key("vertexArray");
        const GLint vao = GetInt(GL_VERTEX_ARRAY_BINDING);
        w.Ref(vao ? RefOf(ObjType::VertexArray, (GLuint)vao) : 0, "GLVertexArray");
    }
    // Which vertices the draw reads, for how much of each attribute's buffer to read back.
    int64_t firstVertex = p.first;
    int64_t lastVertex = (int64_t)p.first + std::max(0, p.count - 1);
    const GLint elementBuffer = GetInt(GL_ELEMENT_ARRAY_BUFFER_BINDING);
    if (p.indexed) {
        w.Key("elementBuffer"); w.Ref(elementBuffer ? RefOf(ObjType::Buffer, (GLuint)elementBuffer) : 0, "GLBuffer");
        w.Key("indexType"); WriteEnumValue(w, p.indexType, EnumName_DrawElementsType);
        const size_t bytes = (size_t)std::max(0, p.count) * IndexBytes(p.indexType);
        const int64_t offset = elementBuffer ? (int64_t)(intptr_t)p.indices : 0;
        w.Key("indexOffset"); w.Int(offset);
        const uint32_t id = CaptureBuffer(c, (GLuint)elementBuffer, offset, bytes, elementBuffer ? nullptr : p.indices);
        w.Key("indexData"); w.Uint(id);
        // Indices in client memory are read here and now; a buffer's are copied and read after the frame,
        // so each attribute's range then runs to its buffer's end (up to the capture's limit).
        uint32_t lo = 0, hi = 0;
        if (id && IndexRange(g_buffers[id - 1].data, p.indexType, (size_t)p.count, lo, hi)) {
            firstVertex = (int64_t)lo + p.baseVertex;
            lastVertex = (int64_t)hi + p.baseVertex;
        } else {
            firstVertex = 0;
            lastVertex = -1;   // unknown: each attribute's read-back is its buffer from the offset on
        }
    }
    w.Key("attributes");
    w.BeginArray();
    if (prog) {
        for (const ProgramVariable& a : prog->attributes) {
            if (a.location < 0 || a.name.rfind("gl_", 0) == 0) continue;
            const GLuint loc = (GLuint)a.location;
            GLint enabled = 0, buffer = 0, size = 4, type = GL_FLOAT, normalized = 0, stride = 0, integer = 0, divisor = 0;
            g_gl.glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
            w.BeginObject();
            w.Key("name"); w.String(a.name);
            w.Key("location"); w.Int(a.location);
            w.Key("type"); WriteEnumValue(w, a.type);
            w.Key("enabled"); w.Boolean(enabled != 0);
            if (!enabled) {
                // A disabled attribute reads the current generic value, the same for every vertex.
                GLfloat v[4] = {0, 0, 0, 1};
                g_gl.glGetVertexAttribfv(loc, GL_CURRENT_VERTEX_ATTRIB, v);
                w.Key("value"); WriteFloats(w, v, 4);
                w.EndObject();
                continue;
            }
            g_gl.glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
            g_gl.glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
            g_gl.glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
            g_gl.glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
            g_gl.glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
            if (es3) {
                g_gl.glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &integer);
                g_gl.glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &divisor);
            }
            void* pointer = nullptr;
            g_gl.glGetVertexAttribPointerv(loc, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
            const size_t element = PackedAttrib((GLenum)type) ? 4 : (size_t)size * AttribTypeBytes((GLenum)type);
            const size_t step = stride ? (size_t)stride : element;
            w.Key("buffer"); w.Ref(buffer ? RefOf(ObjType::Buffer, (GLuint)buffer) : 0, "GLBuffer");
            w.Key("size"); w.Int(size);
            w.Key("componentType"); WriteEnumValue(w, (GLenum)type, EnumName_VertexAttribPointerType);
            w.Key("normalized"); w.Boolean(normalized != 0);
            w.Key("integer"); w.Boolean(integer != 0);
            w.Key("stride"); w.Uint(step);
            w.Key("offset"); w.Int(buffer ? (int64_t)(intptr_t)pointer : 0);
            w.Key("divisor"); w.Int(divisor);
            // The read-back starts at the attribute's own offset, so vertex i is at i * stride in it.
            int64_t last = divisor ? (std::max(1, p.instances) - 1) / divisor : lastVertex;
            size_t bytes;
            if (last < 0) {
                Object* bo = buffer ? ObjectOf(c, ObjType::Buffer, (GLuint)buffer) : nullptr;
                bytes = bo && bo->size > (GLsizeiptr)(intptr_t)pointer ? (size_t)(bo->size - (GLsizeiptr)(intptr_t)pointer) : 0;
            } else {
                bytes = (size_t)last * step + element;
            }
            const uint32_t id = CaptureBuffer(c, (GLuint)buffer, buffer ? (int64_t)(intptr_t)pointer : 0, bytes, buffer ? nullptr : pointer);
            w.Key("data"); w.Uint(id);
            w.EndObject();
        }
    }
    w.EndArray();
    if (lastVertex >= 0) {
        w.Key("firstVertex"); w.Int(firstVertex);
        w.Key("lastVertex"); w.Int(lastVertex);
    }
}

void WriteProgramResources(Context* c, const Bindings& b, Object* prog, gpuinsp::sdk::JsonWriter& w) {
    if (!prog) return;
    const GLuint program = prog->name;
    // Textures, through the sampler uniforms: which unit each one reads, and what is bound there.
    w.Key("textures");
    w.BeginArray();
    const GLint activeTexture = GetInt(GL_ACTIVE_TEXTURE, GL_TEXTURE0);
    for (const ProgramVariable& u : prog->uniforms) {
        const UniformType* t = UniformTypeOf(u.type);
        if (!t || t->base != 's' || u.location < 0) continue;
        for (GLint e = 0; e < std::min<GLint>(u.size, 16); ++e) {
            GLint unit = 0;
            g_gl.glGetUniformiv(program, u.location + e, &unit);
            g_gl.glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
            const GLint name = GetInt(t->textureBinding);
            const GLint sampler = c->es3() ? GetInt(GL_SAMPLER_BINDING) : 0;
            w.BeginObject();
            w.Key("uniform"); w.String(u.size > 1 ? u.name + "[" + std::to_string(e) + "]" : u.name);
            w.Key("type"); w.String(t->glsl);
            w.Key("unit"); w.Int(unit);
            w.Key("target"); WriteEnumValue(w, t->target, EnumName_TextureTarget);
            w.Key("texture"); w.Ref(name ? RefOf(ObjType::Texture, (GLuint)name) : 0, "GLTexture");
            w.Key("sampler"); w.Ref(sampler ? RefOf(ObjType::Sampler, (GLuint)sampler) : 0, "GLSampler");
            if (Object* tex = name ? ObjectOf(c, ObjType::Texture, (GLuint)name) : nullptr) {
                g_gl.glActiveTexture((GLenum)activeTexture);
                w.Key("capture"); w.Uint(CaptureTexture(c, b, *tex));
            }
            w.EndObject();
        }
    }
    g_gl.glActiveTexture((GLenum)activeTexture);
    w.EndArray();

    // Uniform blocks: the buffer range bound at each block's binding point.
    w.Key("uniformBlocks");
    w.BeginArray();
    for (const ProgramBlock& block : prog->blocks) {
        GLint binding = 0;
        g_gl.glGetActiveUniformBlockiv(program, block.index, GL_UNIFORM_BLOCK_BINDING, &binding);
        GLint buffer = 0;
        GLint64 start = 0, size = 0;
        if (g_gl.glGetIntegeri_v) g_gl.glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, (GLuint)binding, &buffer);
        if (g_gl.glGetInteger64i_v) {
            g_gl.glGetInteger64i_v(GL_UNIFORM_BUFFER_START, (GLuint)binding, &start);
            g_gl.glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, (GLuint)binding, &size);
        }
        w.BeginObject();
        w.Key("name"); w.String(block.name);
        w.Key("index"); w.Uint(block.index);
        w.Key("binding"); w.Int(binding);
        w.Key("dataSize"); w.Int(block.dataSize);
        w.Key("buffer"); w.Ref(buffer ? RefOf(ObjType::Buffer, (GLuint)buffer) : 0, "GLBuffer");
        w.Key("offset"); w.Int(start);
        w.Key("size"); w.Int(size);
        if (buffer) {
            const size_t bytes = size > 0 ? (size_t)std::min<GLint64>(size, block.dataSize ? block.dataSize : size) : (size_t)block.dataSize;
            w.Key("data"); w.Uint(CaptureBuffer(c, (GLuint)buffer, start, bytes));
        }
        w.EndObject();
    }
    w.EndArray();

    // The default block's uniforms, as they are set now.
    w.Key("uniforms");
    w.BeginArray();
    for (const ProgramVariable& u : prog->uniforms) {
        if (u.location < 0 || u.blockIndex >= 0) continue;
        const UniformType* t = UniformTypeOf(u.type);
        if (!t) continue;
        w.BeginObject();
        w.Key("name"); w.String(u.name);
        w.Key("type"); w.String(t->glsl);
        w.Key("location"); w.Int(u.location);
        if (u.size > 1) { w.Key("size"); w.Int(u.size); }
        w.Key("value");
        w.BeginArray();
        for (GLint e = 0; e < std::min<GLint>(u.size, 16); ++e) {
            if (t->base == 'f') {
                GLfloat v[16] = {};
                g_gl.glGetUniformfv(program, u.location + e, v);
                for (int i = 0; i < t->components; ++i) w.Double(v[i]);
            } else if (t->base == 'u' && g_gl.glGetUniformuiv) {
                GLuint v[4] = {};
                g_gl.glGetUniformuiv(program, u.location + e, v);
                for (int i = 0; i < t->components; ++i) w.Uint(v[i]);
            } else {
                GLint v[4] = {};
                g_gl.glGetUniformiv(program, u.location + e, v);
                for (int i = 0; i < t->components; ++i) {
                    if (t->base == 'b') w.Boolean(v[i] != 0); else w.Int(v[i]);
                }
            }
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
}

void WriteFixedFunction(Context* c, gpuinsp::sdk::JsonWriter& w) {
    GLint v[4] = {};
    GLfloat f[4] = {};
    GLboolean mask[4] = {};
    w.Key("raster");
    w.BeginObject();
    g_gl.glGetIntegerv(GL_VIEWPORT, v);
    w.Key("viewport"); WriteInts(w, v, 4);
    w.Key("scissorTest"); w.Boolean(IsOn(GL_SCISSOR_TEST));
    g_gl.glGetIntegerv(GL_SCISSOR_BOX, v);
    w.Key("scissor"); WriteInts(w, v, 4);
    w.Key("cullFace"); w.Boolean(IsOn(GL_CULL_FACE));
    w.Key("cullMode"); WriteEnumValue(w, (GLenum)GetInt(GL_CULL_FACE_MODE, GL_BACK), EnumName_TriangleFace);
    w.Key("frontFace"); WriteEnumValue(w, (GLenum)GetInt(GL_FRONT_FACE, GL_CCW), EnumName_FrontFaceDirection);
    w.Key("polygonOffsetFill"); w.Boolean(IsOn(GL_POLYGON_OFFSET_FILL));
    g_gl.glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &f[0]);
    g_gl.glGetFloatv(GL_POLYGON_OFFSET_UNITS, &f[1]);
    w.Key("polygonOffset"); WriteFloats(w, f, 2);
    g_gl.glGetFloatv(GL_LINE_WIDTH, &f[0]);
    w.Key("lineWidth"); w.Double(f[0]);
    if (c->es3()) {
        w.Key("rasterizerDiscard"); w.Boolean(IsOn(GL_RASTERIZER_DISCARD));
        w.Key("primitiveRestart"); w.Boolean(IsOn(GL_PRIMITIVE_RESTART_FIXED_INDEX));
    }
    w.Key("dither"); w.Boolean(IsOn(GL_DITHER));
    w.EndObject();

    w.Key("depth");
    w.BeginObject();
    w.Key("test"); w.Boolean(IsOn(GL_DEPTH_TEST));
    w.Key("func"); WriteEnumValue(w, (GLenum)GetInt(GL_DEPTH_FUNC, GL_LESS), EnumName_DepthFunction);
    g_gl.glGetBooleanv(GL_DEPTH_WRITEMASK, mask);
    w.Key("write"); w.Boolean(mask[0] != 0);
    g_gl.glGetFloatv(GL_DEPTH_RANGE, f);
    w.Key("range"); WriteFloats(w, f, 2);
    w.EndObject();

    w.Key("stencil");
    w.BeginObject();
    w.Key("test"); w.Boolean(IsOn(GL_STENCIL_TEST));
    const struct { const char* key; GLenum func, ref, valueMask, fail, zfail, zpass, writeMask; } faces[] = {
        {"front", GL_STENCIL_FUNC, GL_STENCIL_REF, GL_STENCIL_VALUE_MASK, GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS, GL_STENCIL_WRITEMASK},
        {"back", GL_STENCIL_BACK_FUNC, GL_STENCIL_BACK_REF, GL_STENCIL_BACK_VALUE_MASK, GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_PASS_DEPTH_FAIL, GL_STENCIL_BACK_PASS_DEPTH_PASS, GL_STENCIL_BACK_WRITEMASK},
    };
    for (const auto& face : faces) {
        w.Key(face.key);
        w.BeginObject();
        w.Key("func"); WriteEnumValue(w, (GLenum)GetInt(face.func), EnumName_StencilFunction);
        w.Key("ref"); w.Int(GetInt(face.ref));
        w.Key("valueMask"); w.Uint((GLuint)GetInt(face.valueMask));
        w.Key("fail"); WriteEnumValue(w, (GLenum)GetInt(face.fail), EnumName_StencilOp);
        w.Key("depthFail"); WriteEnumValue(w, (GLenum)GetInt(face.zfail), EnumName_StencilOp);
        w.Key("pass"); WriteEnumValue(w, (GLenum)GetInt(face.zpass), EnumName_StencilOp);
        w.Key("writeMask"); w.Uint((GLuint)GetInt(face.writeMask));
        w.EndObject();
    }
    w.EndObject();

    w.Key("blend");
    w.BeginObject();
    w.Key("enabled"); w.Boolean(IsOn(GL_BLEND));
    w.Key("equationRgb"); WriteEnumValue(w, (GLenum)GetInt(GL_BLEND_EQUATION_RGB, GL_FUNC_ADD), EnumName_BlendEquationModeEXT);
    w.Key("equationAlpha"); WriteEnumValue(w, (GLenum)GetInt(GL_BLEND_EQUATION_ALPHA, GL_FUNC_ADD), EnumName_BlendEquationModeEXT);
    w.Key("srcRgb"); WriteEnumValue(w, (GLenum)GetInt(GL_BLEND_SRC_RGB, GL_ONE), EnumName_BlendingFactor);
    w.Key("dstRgb"); WriteEnumValue(w, (GLenum)GetInt(GL_BLEND_DST_RGB, GL_ZERO), EnumName_BlendingFactor);
    w.Key("srcAlpha"); WriteEnumValue(w, (GLenum)GetInt(GL_BLEND_SRC_ALPHA, GL_ONE), EnumName_BlendingFactor);
    w.Key("dstAlpha"); WriteEnumValue(w, (GLenum)GetInt(GL_BLEND_DST_ALPHA, GL_ZERO), EnumName_BlendingFactor);
    g_gl.glGetFloatv(GL_BLEND_COLOR, f);
    w.Key("color"); WriteFloats(w, f, 4);
    g_gl.glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    w.Key("colorMask");
    w.BeginArray();
    for (int i = 0; i < 4; ++i) w.Boolean(mask[i] != 0);
    w.EndArray();
    w.EndObject();
}

std::string Snapshot(Context* c, const DrawParams* draw) {
    GlWork work(c);
    Bindings b(c);
    gpuinsp::sdk::JsonWriter w;
    w.BeginObject();
    const GLint program = GetInt(GL_CURRENT_PROGRAM);
    Object* prog = program ? ObjectOf(c, ObjType::Program, (GLuint)program) : nullptr;
    w.Key("program"); w.Ref(prog ? prog->id : 0, "GLProgram");
    if (draw) {
        w.Key("framebuffer"); w.Ref(c->drawFramebuffer ? RefOf(ObjType::Framebuffer, c->drawFramebuffer) : 0, "GLFramebuffer");
        w.Key("passIndex"); w.Uint(c->passIndex);
        w.Key("mode"); WriteEnumValue(w, draw->mode, EnumName_PrimitiveType);
        // What the draw draws, indirect draws' counts read from their buffer: the mesh view's input.
        w.Key("draw");
        w.BeginObject();
        w.Key("indexed"); w.Boolean(draw->indexed);
        w.Key("count"); w.Int(draw->count);
        w.Key("first"); w.Int(draw->first);
        w.Key("instances"); w.Int(draw->instances);
        w.Key("baseVertex"); w.Int(draw->baseVertex);
        w.Key("indirect"); w.Boolean(draw->indirect);
        w.EndObject();
        WriteVertexInput(c, prog, *draw, w);
        WriteProgramResources(c, b, prog, w);
        WriteFixedFunction(c, w);
    } else {
        WriteProgramResources(c, b, prog, w);
    }
    w.EndObject();
    return w.str();
}

// ------------------------------------------------------------------------------------------------
// Reading the copies

/**
 * The read-backs copied during the frame, read now that it is over (the GPU has long finished most of
 * them) and the copies deleted. A copy is an object of the context that made it, readable from any
 * context of its share group: one made in another group is reported rather than read.
 */
void ResolveCopies(Context* c) {
    if (g_pendingBuffers.empty() && g_pendingTextures.empty()) return;
    if (!c) {
        for (auto& p : g_pendingBuffers) g_buffers[p.id - 1].error = "no context was current when the capture finished";
        for (auto& p : g_pendingTextures) g_textures[p.index].error = "no context was current when the capture finished";
        g_pendingBuffers.clear();
        g_pendingTextures.clear();
        return;
    }
    GlWork work(c);
    Bindings b(c);
    for (const PendingBuffer& p : g_pendingBuffers) {
        BufferCapture& out = g_buffers[p.id - 1];
        if (p.share != c->share) {
            out.error = "copied on a context that shares nothing with the one the capture finished on";
            continue;
        }
        g_gl.glBindBuffer(GL_COPY_READ_BUFFER, p.copy);
        if (const void* m = g_gl.glMapBufferRange(GL_COPY_READ_BUFFER, 0, (GLsizeiptr)p.size, GL_MAP_READ_BIT)) {
            out.data.assign((const uint8_t*)m, (const uint8_t*)m + p.size);
            g_gl.glUnmapBuffer(GL_COPY_READ_BUFFER);
        } else {
            out.error = "the copy could not be mapped for reading";
        }
        g_gl.glDeleteBuffers(1, &p.copy);
    }
    for (const PendingTexture& p : g_pendingTextures) {
        TextureCapture& out = g_textures[p.index];
        if (p.share != c->share) {
            out.error = "copied on a context that shares nothing with the one the capture finished on";
            continue;
        }
        Object copy;
        copy.name = p.copy;
        copy.target = GL_TEXTURE_2D_ARRAY;
        copy.internalFormat = p.internalFormat;
        copy.width = p.width;
        copy.height = p.height;
        copy.depth = p.layers;
        for (int layer = 0; layer < p.layers && out.error.empty(); ++layer) {
            TextureCapture part;
            if (ReadAttachment(c, b, copy, ObjType::Texture, 0, layer, 0, p.width, p.height, true, part)) {
                out.format = part.format;
                out.data.insert(out.data.end(), part.data.begin(), part.data.end());
            } else {
                out.error = part.error;
                out.data.clear();
            }
        }
        g_gl.glDeleteTextures(1, &p.copy);
    }
    g_pendingBuffers.clear();
    g_pendingTextures.clear();
}

// ------------------------------------------------------------------------------------------------
// Sending the capture

/**
 * The passes' GPU times, from the queries written around them, as CapturePassTimings. Reading a
 * result waits for the GPU to reach it, which after the frame's swap is soon. Only the queries of
 * the context current here can be read; a query taken while the GPU was disjoint (a power state
 * change, a context switch) is no good, and then none are sent. With elapsed-time queries there are
 * durations but no starts: the passes are placed end to end, which is how the GPU runs one
 * context's work anyway.
 */
std::string PassTimes(Context* c) {
    gpuinsp::sdk::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CapturePassTimings");
    w.Key("timestampPeriodNs"); w.Double(1.0);
    std::vector<std::pair<const PassQueries*, std::pair<GLuint64, GLuint64>>> times;
    if (c && !g_passQueries.empty()) {
        GlWork work(c);
        GLint disjoint = 0;
        g_gl.glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        GLuint64 cursor = 0;
        for (const PassQueries& q : g_passQueries) {
            if (q.context != c->id || !q.begin || (!q.elapsed && !q.end)) continue;
            GLuint64 begin = 0, end = 0;
            if (q.elapsed) {
                GLuint64 ns = 0;
                g_timer.result64(q.begin, GL_QUERY_RESULT_EXT, &ns);
                begin = cursor;
                end = cursor + ns;
                cursor = end;
            } else {
                g_timer.result64(q.begin, GL_QUERY_RESULT_EXT, &begin);
                g_timer.result64(q.end, GL_QUERY_RESULT_EXT, &end);
            }
            if (end >= begin) times.push_back({&q, {begin, end}});
        }
        g_gl.glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        if (disjoint) {
            LogAlways("the GPU was disjoint while the capture was timed (EXT_disjoint_timer_query): no pass timings");
            times.clear();
        }
        for (const PassQueries& q : g_passQueries) {
            if (q.context != c->id) continue;
            if (q.begin) g_timer.del(1, &q.begin);
            if (q.end) g_timer.del(1, &q.end);
        }
    }
    g_passQueries.clear();
    GLuint64 origin = UINT64_MAX;
    for (const auto& t : times) origin = std::min(origin, t.second.first);
    w.Key("count"); w.Uint(times.size());
    w.Key("passes");
    w.BeginArray();
    for (const auto& [q, t] : times) {
        w.BeginObject();
        w.Key("frame"); w.Uint(q->frame);
        w.Key("commandBuffer"); w.Uint(q->context);
        w.Key("passIndex"); w.Uint(q->passIndex);
        w.Key("kind"); w.String("render");
        w.Key("startMs"); w.Double((double)(t.first - origin) / 1e6);
        w.Key("durationMs"); w.Double((double)(t.second - t.first) / 1e6);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return w.str();
}

void SendCapture(Context* c) {
    Server& server = Server::Get();
    const uint32_t frames = g_frame;
    {
        gpuinsp::sdk::JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureFrameResults");
        w.Key("frame"); w.Uint(g_startSwap);
        w.Key("frames"); w.Uint(std::max<uint32_t>(1, frames));
        w.Key("count"); w.Uint(g_commands.size());
        constexpr size_t kBatch = 1000;
        w.Key("batches"); w.Uint((g_commands.size() + kBatch - 1) / kBatch);
        w.Key("api"); w.String("gles");
        w.EndObject();
        server.SendJson(w.str());
        for (size_t start = 0, batch = 0; start < g_commands.size(); start += kBatch, ++batch) {
            gpuinsp::sdk::JsonWriter out;
            out.BeginObject();
            out.Key("action"); out.String("CaptureFrameCommands");
            out.Key("frame"); out.Uint(g_startSwap);
            out.Key("index"); out.Uint(batch);
            out.Key("commands");
            out.BeginArray();
            for (size_t i = start; i < std::min(g_commands.size(), start + kBatch); ++i) {
                const Recorded& r = g_commands[i];
                out.BeginObject();
                out.Key("index"); out.Uint(i);
                out.Key("frame"); out.Uint(r.frame);
                out.Key("method"); out.String(r.method);
                out.Key("object"); out.Ref(r.context, "GLContext");
                out.Key("args"); out.Raw(r.args.empty() ? std::string("{}") : r.args);
                if (!r.state.empty()) { out.Key("state"); out.Raw(r.state); }
                out.EndObject();
            }
            out.EndArray();
            out.EndObject();
            server.SendJson(out.str());
        }
    }
    {
        gpuinsp::sdk::JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureTextureFrames");
        w.Key("count"); w.Uint(g_textures.size());
        w.Key("textures");
        w.BeginArray();
        for (const TextureCapture& t : g_textures) {
            w.BeginObject();
            w.Key("id"); w.Uint(t.id);
            w.Key("frame"); w.Uint(t.frame);
            w.Key("commandBuffer"); w.Uint(t.context);
            w.Key("passIndex"); w.Uint(t.passIndex);
            w.Key("attachment"); w.Uint(t.attachment);
            w.Key("format"); w.String(t.format.empty() ? "VK_FORMAT_UNDEFINED" : t.format);
            w.Key("aspect"); w.String("color");
            w.Key("width"); w.Int(t.width);
            w.Key("height"); w.Int(t.height);
            w.Key("depth"); w.Int(1);
            w.Key("layers"); w.Int(t.layers);
            w.Key("mip"); w.Int(t.mip);
            w.Key("size"); w.Uint(t.data.size());
            if (!t.error.empty()) { w.Key("error"); w.String(t.error); }
            if (!t.kind.empty()) {
                w.Key("kind"); w.String(t.kind);
                w.Key("capture"); w.Uint(t.capture);
                w.Key("baseLayer"); w.Uint(0);
            }
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        server.SendJson(w.str());
        for (const TextureCapture& t : g_textures) {
            if (t.data.empty()) continue;
            gpuinsp::sdk::JsonWriter h;
            h.BeginObject();
            h.Key("action"); h.String("CaptureTextureData");
            h.Key("id"); h.Uint(t.id);
            h.Key("frame"); h.Uint(t.frame);
            h.Key("commandBuffer"); h.Uint(t.context);
            h.Key("passIndex"); h.Uint(t.passIndex);
            h.Key("attachment"); h.Uint(t.attachment);
            h.Key("aspect"); h.String("color");
            if (t.capture) { h.Key("capture"); h.Uint(t.capture); }
            h.Key("size"); h.Uint(t.data.size());
            h.EndObject();
            server.SendBinary(h.str(), t.data.data(), t.data.size());
        }
    }
    {
        gpuinsp::sdk::JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureBuffers");
        w.Key("count"); w.Uint(g_buffers.size());
        w.Key("buffers");
        w.BeginArray();
        for (const BufferCapture& b : g_buffers) {
            w.BeginObject();
            w.Key("id"); w.Uint(b.id);
            w.Key("buffer"); w.Uint(b.buffer);
            w.Key("frame"); w.Uint(b.frame);
            w.Key("commandBuffer"); w.Uint(b.context);
            w.Key("offset"); w.Int(b.offset);
            w.Key("size"); w.Uint(b.data.size());
            if (b.original) { w.Key("originalSize"); w.Uint(b.original); }
            if (!b.error.empty()) { w.Key("error"); w.String(b.error); }
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        server.SendJson(w.str());
        for (const BufferCapture& b : g_buffers) {
            if (!b.error.empty() || b.data.empty()) continue;
            server.SendBinary("{\"action\":\"CaptureBufferData\",\"id\":" + std::to_string(b.id) + ",\"size\":" + std::to_string(b.data.size()) + "}",
                              b.data.data(), b.data.size());
        }
    }
    // Sent even when there are no timings: a capture that asked for them is then told there are none.
    if (g_options.profilePasses) server.SendJson(PassTimes(c));
    server.SendJson("{\"action\":\"CaptureComplete\",\"frame\":" + std::to_string(g_startSwap) + ",\"frames\":" + std::to_string(std::max<uint32_t>(1, frames)) + "}");
    LogAlways("capture of %u frame(s) sent: %zu commands, %zu images (%zu MB), %zu buffers (%zu KB)", frames, g_commands.size(),
              g_textures.size(), g_imageBytes >> 20, g_buffers.size(), g_bufferBytes >> 10);
}

void Reset() {
    g_commands.clear();
    g_textures.clear();
    g_buffers.clear();
    g_bufferBytes = 0;
    g_imageBytes = 0;
    g_nextTextureCapture = 1;
    g_bufferSeen.clear();
    g_textureSeen.clear();
    g_passQueries.clear();
    g_pendingBuffers.clear();
    g_pendingTextures.clear();
    g_frame = 0;
}

void FrameStats() {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastSwap.time_since_epoch().count()) {
        const double ms = std::chrono::duration<double, std::milli>(now - g_lastSwap).count();
        g_sumMs += ms;
        g_minMs = g_intervals ? std::min(g_minMs, ms) : ms;
        g_maxMs = g_intervals ? std::max(g_maxMs, ms) : ms;
        ++g_intervals;
    }
    g_lastSwap = now;
    if (!g_lastReport.time_since_epoch().count()) g_lastReport = now;
    if (now - g_lastReport < std::chrono::milliseconds(100) || !g_intervals) return;
    if (Server::Get().Connected()) {
        gpuinsp::sdk::JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("FrameStats");
        w.Key("frame"); w.Uint(g_swaps);
        w.Key("frameTimeMs"); w.Double(g_sumMs / g_intervals);
        w.Key("minMs"); w.Double(g_minMs);
        w.Key("maxMs"); w.Double(g_maxMs);
        w.Key("frames"); w.Uint(g_intervals);
        w.Key("frameBoundary"); w.String("present");
        w.EndObject();
        Server::Get().SendJson(w.str());
    }
    g_lastReport = now;
    g_sumMs = 0;
    g_intervals = 0;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Public

bool Recording() {
    return g_recording.load(std::memory_order_relaxed);
}

void RequestCapture(const JsonValue& msg) {
    std::lock_guard lock(g_mutex);
    CaptureOptions o;
    o.frameCount = (uint32_t)std::max(1.0, msg.GetNumber("frameCount", 1));
    if (const JsonValue* at = msg.Get("atFrame"); at && at->kind == JsonValue::Number) o.atFrame = (int64_t)at->num;
    o.captureTextures = msg.GetBool("captureTextures", true);
    o.captureBuffers = msg.GetBool("captureBuffers", true);
    o.captureImages = msg.GetBool("captureImages", true);
    o.profilePasses = msg.GetBool("profilePasses", false);
    if (msg.Get("maxBufferSize")) o.maxBufferSize = (size_t)msg.GetNumber("maxBufferSize");
    if (msg.Get("maxBufferTotal")) o.maxBufferTotal = (size_t)msg.GetNumber("maxBufferTotal");
    if (msg.Get("maxImageTotal")) o.maxImageTotal = (size_t)msg.GetNumber("maxImageTotal");
    g_requested = o;
    g_armed = true;
    Log("capture armed: %u frame(s)%s", o.frameCount, o.atFrame >= 0 ? " at a given frame" : "");
}

bool BeginCall(Call& call, const char* name) {
    if (!Recording()) return false;
    call.name = name;
    call.recording = true;
    call.args.BeginObject();
    return true;
}

void EndCall(Call& call) {
    call.args.EndObject();
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    Context* c = Current();
    if (!c) return;
    Append(c, call.name, std::move(call.args.str()), std::move(t_pendingState), false);
    t_pendingState.clear();
}

void RecordCommand(Context* c, const char* method, const std::string& argsJson, bool synthetic) {
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    Append(c, method, argsJson, "", synthetic);
}

void BeforeDraw(Context* c, const DrawParams& p) {
    if (!Recording() || !c) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    BeginPassIfNeeded(c);
    c->passDrawn = true;
    t_pendingState = Snapshot(c, &p);
}

void BeforeDispatch(Context* c, bool indirect, intptr_t indirectOffset) {
    if (!Recording() || !c) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    (void)indirect;
    (void)indirectOffset;
    t_pendingState = Snapshot(c, nullptr);
}

void BeforeFramebufferWrite(Context* c, GLbitfield clearMask) {
    if (!Recording() || !c) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    BeginPassIfNeeded(c);
    // A clear that comes before any draw decides what the pass starts from; one after is drawing.
    // A clear with the scissor test on clears part of the target, which keeps the rest.
    if (clearMask && !c->passDrawn && !(g_gl.glIsEnabled && g_gl.glIsEnabled(GL_SCISSOR_TEST))) c->passCleared |= clearMask;
    else if (!clearMask) c->passDrawn = true;
}

void BeforeInvalidate(Context* c, GLenum target, GLsizei count, const GLenum* attachments) {
    if (!Recording() || !c) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    // Invalidating the read framebuffer alone leaves what the pass drew alone.
    if (target == GL_READ_FRAMEBUFFER) return;
    if (!c->passOpen || c->passFramebuffer != c->drawFramebuffer) return;
    ReadPassAttachments(c);
    for (GLsizei i = 0; attachments && i < count; ++i) {
        if (std::find(c->passInvalidated.begin(), c->passInvalidated.end(), attachments[i]) == c->passInvalidated.end()) c->passInvalidated.push_back(attachments[i]);
    }
}

void BeforeBindFramebuffer(Context* c, GLenum target, GLuint framebuffer) {
    if (!c) return;
    const bool draw = target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER;
    if (draw && Recording() && c->passOpen && framebuffer != c->passFramebuffer) {
        std::lock_guard lock(g_mutex);
        if (Recording()) EndPass(c);
    }
}

void BeforePopGroup(Context* c) {
    if (!c) return;
    // A pass begun inside the group ends with it, so the command tree nests the one in the other.
    if (Recording() && c->passOpen && c->passGroupDepth >= c->groupDepth) {
        std::lock_guard lock(g_mutex);
        if (Recording()) EndPass(c);
    }
    if (c->groupDepth > 0) --c->groupDepth;
}

void AfterPushGroup(Context* c) {
    if (c) ++c->groupDepth;
}

void BeforeAttachmentChange(Context* c, GLenum target) {
    if (!Recording() || !c) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    const GLuint fb = target == GL_READ_FRAMEBUFFER ? c->readFramebuffer : c->drawFramebuffer;
    if (c->passOpen && fb == c->passFramebuffer && fb != 0) EndPass(c);
}

void BeforeSwap(Context* c, EGLDisplay display, EGLSurface surface, const char* method) {
    if (!c || !Recording()) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    // A swap with nothing drawn since the last still shows the surface as it is: a pass of its own
    // reads it, unless the frame's last pass on the surface ended (with a debug group, say) and has.
    if ((!c->passOpen || c->passFramebuffer != 0) && !c->surfacePassEnded) {
        const GLuint saved = c->drawFramebuffer;
        c->drawFramebuffer = 0;
        BeginPassIfNeeded(c);
        c->drawFramebuffer = saved;
    }
    EndPass(c);
    gpuinsp::sdk::JsonWriter w;
    w.BeginObject();
    if (display) { w.Key("dpy"); w.Pointer(display); }
    w.Key(c->wgl ? "hdc" : c->glx ? "drawable" : "surface"); w.Ref(SurfaceId(surface), "GLSurface");
    w.EndObject();
    Append(c, method, w.str(), "", false);
}

void AfterSwap(Context* c) {
    std::lock_guard lock(g_mutex);
    ++g_swaps;
    FrameStats();
    if (c) {
        c->nextPassIndex = 0;
        c->surfacePassEnded = false;
    }
    if (Recording()) {
        ++g_frame;
        if (g_frame >= g_options.frameCount) {
            g_recording = false;
            ResolveCopies(c);
            SendCapture(c);
            Reset();
        }
        return;
    }
    if (g_armed && (g_requested.atFrame < 0 || (int64_t)g_swaps >= g_requested.atFrame)) {
        if (!Server::Get().Connected()) {
            g_armed = false;
            return;
        }
        g_armed = false;
        g_options = g_requested;
        Reset();
        g_startSwap = g_swaps;
        // Every context starts its pass count afresh with the capture.
        std::lock_guard slock(State().mutex);
        for (auto& [handle, ctx] : State().contexts) {
            ctx->nextPassIndex = 0;
            ctx->passOpen = false;
        }
        g_recording = true;
        Log("capture started at frame %llu", (unsigned long long)g_swaps);
    }
}

void OnDisconnect() {
    std::lock_guard lock(g_mutex);
    g_armed = false;
    if (g_recording) {
        g_recording = false;
        Reset();
        Log("the inspector went away mid-capture; the capture is dropped");
    }
}

const char* GlslTypeName(GLenum type) {
    const UniformType* t = UniformTypeOf(type);
    return t ? t->glsl : nullptr;
}

GLenum TakeSavedError() {
    Context* c = Current();
    if (!c || !c->savedError) return 0;
    const GLenum e = c->savedError;
    c->savedError = 0;
    return e;
}

}  // namespace glesinsp
