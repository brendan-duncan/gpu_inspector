// The hand-written GL hooks (tools/gen_gles.py lists them): objects made, described and deleted,
// the bindings the capture follows, and the calls that begin, end or read into a pass.
#include "capture.h"
#include "formats.h"
#include "state.h"
#include "../gen/gles_constants.gen.h"

#include <algorithm>
#include <cstring>

namespace glesinsp
{

namespace
{

GLint GetInt(GLenum pname)
{
    GLint v = 0;
    if (g_gl.glGetIntegerv)
        g_gl.glGetIntegerv(pname, &v);
    return v;
}

/** The texture bound to `target` on the active unit (a cube face target names the cube map). */
GLuint BoundTexture(GLenum target)
{
    switch (target)
    {
        case GL_TEXTURE_2D: return (GLuint)GetInt(GL_TEXTURE_BINDING_2D);
        case GL_TEXTURE_3D: return (GLuint)GetInt(GL_TEXTURE_BINDING_3D);
        case GL_TEXTURE_2D_ARRAY: return (GLuint)GetInt(GL_TEXTURE_BINDING_2D_ARRAY);
        case GL_TEXTURE_CUBE_MAP_ARRAY: return (GLuint)GetInt(GL_TEXTURE_BINDING_CUBE_MAP_ARRAY);
        case GL_TEXTURE_2D_MULTISAMPLE: return (GLuint)GetInt(GL_TEXTURE_BINDING_2D_MULTISAMPLE);
        case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return (GLuint)GetInt(GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY);
        case GL_TEXTURE_EXTERNAL_OES: return (GLuint)GetInt(GL_TEXTURE_BINDING_EXTERNAL_OES);
        case GL_TEXTURE_BUFFER: return (GLuint)GetInt(GL_TEXTURE_BINDING_BUFFER);
        default:
            if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
                return (GLuint)GetInt(GL_TEXTURE_BINDING_CUBE_MAP);
            if (target == GL_TEXTURE_CUBE_MAP)
                return (GLuint)GetInt(GL_TEXTURE_BINDING_CUBE_MAP);
            return 0;
    }
}

GLuint BoundBuffer(GLenum target)
{
    switch (target)
    {
        case GL_ARRAY_BUFFER: return (GLuint)GetInt(GL_ARRAY_BUFFER_BINDING);
        case GL_ELEMENT_ARRAY_BUFFER: return (GLuint)GetInt(GL_ELEMENT_ARRAY_BUFFER_BINDING);
        case GL_COPY_READ_BUFFER: return (GLuint)GetInt(GL_COPY_READ_BUFFER_BINDING);
        case GL_COPY_WRITE_BUFFER: return (GLuint)GetInt(GL_COPY_WRITE_BUFFER_BINDING);
        case GL_PIXEL_PACK_BUFFER: return (GLuint)GetInt(GL_PIXEL_PACK_BUFFER_BINDING);
        case GL_PIXEL_UNPACK_BUFFER: return (GLuint)GetInt(GL_PIXEL_UNPACK_BUFFER_BINDING);
        case GL_UNIFORM_BUFFER: return (GLuint)GetInt(GL_UNIFORM_BUFFER_BINDING);
        case GL_TRANSFORM_FEEDBACK_BUFFER: return (GLuint)GetInt(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING);
        case GL_SHADER_STORAGE_BUFFER: return (GLuint)GetInt(GL_SHADER_STORAGE_BUFFER_BINDING);
        case GL_DRAW_INDIRECT_BUFFER: return (GLuint)GetInt(GL_DRAW_INDIRECT_BUFFER_BINDING);
        case GL_DISPATCH_INDIRECT_BUFFER: return (GLuint)GetInt(GL_DISPATCH_INDIRECT_BUFFER_BINDING);
        case GL_ATOMIC_COUNTER_BUFFER: return (GLuint)GetInt(GL_ATOMIC_COUNTER_BUFFER_BINDING);
        case GL_TEXTURE_BUFFER: return (GLuint)GetInt(GL_TEXTURE_BUFFER_BINDING);
        default: return 0;
    }
}

Object* BufferAt(Context* c, GLenum target)
{
    const GLuint name = BoundBuffer(target);
    return name ? ObjectOf(c, ObjType::Buffer, name) : nullptr;
}

Object* TextureAt(Context* c, GLenum target)
{
    const GLuint name = BoundTexture(target);
    if (!name)
        return nullptr;
    EnsureName(c, ObjType::Texture, name, "glBindTexture");
    return ObjectOf(c, ObjType::Texture, name);
}

bool IsCubeFace(GLenum target)
{
    return target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
}

void DescribeTexture(Object& t)
{
    Describe(t, "target", JsonEnum(t.target, EnumName_TextureTarget));
    Describe(t, "internalFormat", JsonEnum(t.internalFormat, EnumName_InternalFormat));
    Describe(t, "format", JsonString(FormatOf(t.internalFormat).vk));
    Describe(t, "width", JsonInt(t.width));
    Describe(t, "height", JsonInt(t.height));
    if (t.depth > 1 || t.target == GL_TEXTURE_3D || t.target == GL_TEXTURE_2D_ARRAY)
        Describe(t, "depth", JsonInt(t.depth));
    Describe(t, "levels", JsonInt(t.levels));
    if (t.samples > 1)
        Describe(t, "samples", JsonInt(t.samples));
}

/** A texture's level-0 shape from an upload or a storage call. */
void TextureShape(Context* c, GLenum target, GLint level, GLenum internalFormat, GLsizei w, GLsizei h, GLsizei d, GLint levels, GLsizei samples)
{
    Object* t = TextureAt(c, target);
    if (!t)
        return;
    ++t->generation;
    if (!t->target || t->target == target || IsCubeFace(target))
        t->target = IsCubeFace(target) ? GL_TEXTURE_CUBE_MAP : target;
    t->levels = std::max<GLint>(t->levels, std::max(levels, level + 1));
    if (level != 0)
    {
        if (!t->width)
        {
            // A texture whose level 0 was never given: its level 0 is what this level implies.
            t->internalFormat = internalFormat;
            t->width = w << level;
            t->height = h << level;
        }
        DescribeTexture(*t);
        return;
    }
    t->internalFormat = internalFormat;
    t->width = w;
    t->height = h;
    t->depth = d;
    t->samples = samples;
    t->compressed = FormatOf(internalFormat).compressed;
    if (!t->compressed)
        t->compressedLevel0.clear();
    DescribeTexture(*t);
}

/** Compressed level-0 data, kept per layer or face (nothing reads compressed texels back). */
void KeepCompressed(Context* c, Object& t, GLenum target, GLsizei layers, GLsizei imageSize, const void* data)
{
    if (!data || GetInt(GL_PIXEL_UNPACK_BUFFER_BINDING))
    {
        t.compressedLevel0.clear();   // uploaded from a buffer: not seen
        return;
    }
    (void)c;
    const bool face = IsCubeFace(target);
    const size_t count = face ? 6 : (size_t)std::max(1, layers);
    if (t.compressedLevel0.size() != count)
        t.compressedLevel0.assign(count, {});
    const size_t perLayer = (size_t)imageSize / std::max<size_t>(1, face ? 1 : count);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (face)
    {
        t.compressedLevel0[target - GL_TEXTURE_CUBE_MAP_POSITIVE_X].assign(bytes, bytes + imageSize);
    }
    else
    {
        for (size_t i = 0; i < count; ++i)
            t.compressedLevel0[i].assign(bytes + i * perLayer, bytes + (i + 1) * perLayer);
    }
    t.compressedLevel0Size = imageSize;
}

void RenderbufferShape(Context* c, GLenum internalFormat, GLsizei w, GLsizei h, GLsizei samples)
{
    const GLuint name = (GLuint)GetInt(GL_RENDERBUFFER_BINDING);
    Object* r = name ? ObjectOf(c, ObjType::Renderbuffer, name) : nullptr;
    if (!r)
        return;
    r->internalFormat = internalFormat;
    r->width = w;
    r->height = h;
    r->samples = samples;
    r->target = GL_RENDERBUFFER;
    Describe(*r, "internalFormat", JsonEnum(internalFormat, EnumName_InternalFormat));
    Describe(*r, "format", JsonString(FormatOf(internalFormat).vk));
    Describe(*r, "width", JsonInt(w));
    Describe(*r, "height", JsonInt(h));
    if (samples > 1)
        Describe(*r, "samples", JsonInt(samples));
}

GLuint FramebufferAt(Context* c, GLenum target)
{
    return target == GL_READ_FRAMEBUFFER ? c->readFramebuffer : c->drawFramebuffer;
}

void DescribeAttachments(Object& fb)
{
    JsonWriter w;
    w.BeginArray();
    for (const auto& [slot, a] : fb.attachments)
    {
        w.BeginObject();
        w.Key("attachment");
        w.Enum(EnumName_FramebufferAttachment(slot), slot);
        w.Key("object");
        w.Ref(a.id, TypeName(a.kind));
        w.Key("level");
        w.Int(a.level);
        if (a.layer)
        {
            w.Key("layer");
            w.Int(a.layer);
        }
        if (a.face)
        {
            w.Key("face");
            w.Enum(EnumName(a.face), a.face);
        }
        w.EndObject();
    }
    w.EndArray();
    Describe(fb, "attachments", w.str());
}

void Attach(Context* c, GLenum target, GLenum attachment, ObjType kind, GLuint name, GLint level, GLint layer, GLenum face)
{
    if (!c)
        return;
    const GLuint fbName = FramebufferAt(c, target);
    Object* fb = fbName ? ObjectOf(c, ObjType::Framebuffer, fbName) : nullptr;
    if (!fb)
        return;
    // A depth-stencil attachment is both; recording it once under its own name is what the call said.
    if (!name)
    {
        fb->attachments.erase(attachment);
    }
    else
    {
        Attachment a;
        a.kind = kind;
        a.name = name;
        a.id = EnsureName(c, kind, name, kind == ObjType::Texture ? "glBindTexture" : "glBindRenderbuffer");
        a.level = level;
        a.layer = layer;
        a.face = face;
        fb->attachments[attachment] = a;
    }
    DescribeAttachments(*fb);
}

void BufferShape(Context* c, GLenum target, GLsizeiptr size, const void* data, GLenum usage, bool storage)
{
    Object* b = BufferAt(c, target);
    if (!b)
        return;
    ++b->generation;
    b->size = size;
    if (!c->es3())
    {
        // ES 2 cannot map a buffer for reading: the library keeps the contents it sees written.
        if (data)
            b->shadow.assign((const uint8_t*)data, (const uint8_t*)data + size);
        else
            b->shadow.assign((size_t)size, 0);
    }
    Describe(*b, "size", JsonInt(size));
    Describe(*b, storage ? "flags" : "usage", storage ? JsonInt(usage) : JsonEnum(usage));
}

void Generated(ObjType kind, GLsizei n, const GLuint* names, const char* cmd)
{
    Context* c = Current();
    if (!c || !names)
        return;
    for (GLsizei i = 0; i < n; ++i)
        RegisterName(c, kind, names[i], cmd);
}

void Deleted(ObjType kind, GLsizei n, const GLuint* names)
{
    Context* c = Current();
    if (!c || !names)
        return;
    for (GLsizei i = 0; i < n; ++i)
        ForgetName(c, kind, names[i]);
}

/** A linked program's interface, asked of the driver: attributes, uniforms and uniform blocks. */
void Reflect(Context* c, Object& p)
{
    const GLuint program = p.name;
    GLint linked = 0, logLength = 0;
    g_gl.glGetProgramiv(program, GL_LINK_STATUS, &linked);
    g_gl.glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    std::string log;
    if (logLength > 1)
    {
        log.resize((size_t)logLength);
        GLsizei got = 0;
        g_gl.glGetProgramInfoLog(program, logLength, &got, log.data());
        log.resize((size_t)std::max(0, got));
    }
    p.linked = linked != 0;
    p.attributes.clear();
    p.uniforms.clear();
    p.blocks.clear();
    char name[512];
    if (p.linked)
    {
        GLint count = 0;
        g_gl.glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &count);
        for (GLint i = 0; i < count; ++i)
        {
            ProgramVariable v;
            GLsizei length = 0;
            g_gl.glGetActiveAttrib(program, (GLuint)i, sizeof(name), &length, &v.size, &v.type, name);
            v.name.assign(name, (size_t)std::max(0, length));
            v.location = g_gl.glGetAttribLocation(program, v.name.c_str());
            p.attributes.push_back(v);
        }
        std::sort(p.attributes.begin(), p.attributes.end(), [](const auto& a, const auto& b) { return a.location < b.location; });
        g_gl.glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
        for (GLint i = 0; i < count; ++i)
        {
            ProgramVariable v;
            GLsizei length = 0;
            g_gl.glGetActiveUniform(program, (GLuint)i, sizeof(name), &length, &v.size, &v.type, name);
            v.name.assign(name, (size_t)std::max(0, length));
            v.location = g_gl.glGetUniformLocation(program, v.name.c_str());
            if (c->es3() && g_gl.glGetActiveUniformsiv)
            {
                const GLuint index = (GLuint)i;
                GLint value = 0;
                g_gl.glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_BLOCK_INDEX, &value);
                v.blockIndex = value;
                g_gl.glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_OFFSET, &value);
                v.offset = value;
                g_gl.glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_ARRAY_STRIDE, &value);
                v.arrayStride = value;
                g_gl.glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_MATRIX_STRIDE, &value);
                v.matrixStride = value;
                g_gl.glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_IS_ROW_MAJOR, &value);
                v.rowMajor = value != 0;
            }
            // An array reports its first element ("lights[0]"): the array is the uniform.
            if (v.size > 1 && v.name.size() > 3 && v.name.compare(v.name.size() - 3, 3, "[0]") == 0)
                v.name.resize(v.name.size() - 3);
            p.uniforms.push_back(v);
        }
        if (c->es3())
        {
            g_gl.glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &count);
            for (GLint i = 0; i < count; ++i)
            {
                ProgramBlock b;
                b.index = (GLuint)i;
                GLsizei length = 0;
                g_gl.glGetActiveUniformBlockName(program, (GLuint)i, sizeof(name), &length, name);
                b.name.assign(name, (size_t)std::max(0, length));
                g_gl.glGetActiveUniformBlockiv(program, (GLuint)i, GL_UNIFORM_BLOCK_DATA_SIZE, &b.dataSize);
                for (const ProgramVariable& u : p.uniforms)
                    if (u.blockIndex == i)
                        b.members.push_back(u);
                p.blocks.push_back(std::move(b));
            }
        }
    }
    const auto type = [](GLenum t) {
        const char* glsl = GlslTypeName(t);
        return glsl ? JsonString(glsl) : JsonEnum(t);
    };
    Describe(p, "linked", JsonBool(p.linked));
    Describe(p, "infoLog", JsonString(log));
    JsonWriter w;
    w.BeginArray();
    for (const ProgramVariable& a : p.attributes)
    {
        w.BeginObject();
        w.Key("name");
        w.String(a.name);
        w.Key("location");
        w.Int(a.location);
        w.Key("type");
        w.Raw(type(a.type));
        if (a.size > 1)
        {
            w.Key("size");
            w.Int(a.size);
        }
        w.EndObject();
    }
    w.EndArray();
    Describe(p, "attributes", w.str());
    w.Reset();
    w.BeginArray();
    for (const ProgramVariable& u : p.uniforms)
    {
        if (u.blockIndex >= 0)
            continue;
        w.BeginObject();
        w.Key("name");
        w.String(u.name);
        w.Key("location");
        w.Int(u.location);
        w.Key("type");
        w.Raw(type(u.type));
        if (u.size > 1)
        {
            w.Key("size");
            w.Int(u.size);
        }
        w.EndObject();
    }
    w.EndArray();
    Describe(p, "uniforms", w.str());
    w.Reset();
    w.BeginArray();
    for (const ProgramBlock& b : p.blocks)
    {
        w.BeginObject();
        w.Key("name");
        w.String(b.name);
        w.Key("index");
        w.Uint(b.index);
        w.Key("dataSize");
        w.Int(b.dataSize);
        w.Key("members");
        w.BeginArray();
        for (const ProgramVariable& m : b.members)
        {
            w.BeginObject();
            // "Block.member" in some drivers, "member" in others: the member's own name is what reads.
            const size_t dot = m.name.find('.');
            w.Key("name");
            w.String(dot == std::string::npos ? m.name : m.name.substr(dot + 1));
            w.Key("type");
            w.Raw(type(m.type));
            w.Key("offset");
            w.Int(m.offset);
            if (m.size > 1)
            {
                w.Key("count");
                w.Int(m.size);
            }
            if (m.arrayStride)
            {
                w.Key("arrayStride");
                w.Int(m.arrayStride);
            }
            if (m.matrixStride)
            {
                w.Key("matrixStride");
                w.Int(m.matrixStride);
            }
            if (m.rowMajor)
            {
                w.Key("rowMajor");
                w.Boolean(true);
            }
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    Describe(p, "uniformBlocks", w.str());
}

void DescribeShaders(Context* c, Object& p)
{
    JsonWriter w;
    w.BeginArray();
    for (GLuint s : p.shaders)
        w.Ref(RefOf(ObjType::Shader, s), "GLShader");
    w.EndArray();
    Describe(p, "shaders", w.str());
    (void)c;
}

ObjType LabelType(GLenum identifier)
{
    switch (identifier)
    {
        case GL_BUFFER:
        case GL_BUFFER_OBJECT_EXT: return ObjType::Buffer;
        case GL_SHADER:
        case GL_SHADER_OBJECT_EXT: return ObjType::Shader;
        case GL_PROGRAM:
        case GL_PROGRAM_OBJECT_EXT: return ObjType::Program;
        case GL_VERTEX_ARRAY:
        case GL_VERTEX_ARRAY_OBJECT_EXT: return ObjType::VertexArray;
        case GL_QUERY:
        case GL_QUERY_OBJECT_EXT: return ObjType::Query;
        case GL_PROGRAM_PIPELINE:
        case GL_PROGRAM_PIPELINE_OBJECT_EXT: return ObjType::ProgramPipeline;
        case GL_TRANSFORM_FEEDBACK: return ObjType::TransformFeedback;
        case GL_SAMPLER: return ObjType::Sampler;
        case GL_TEXTURE: return ObjType::Texture;
        case GL_RENDERBUFFER: return ObjType::Renderbuffer;
        case GL_FRAMEBUFFER: return ObjType::Framebuffer;
        default: return ObjType::Count;
    }
}

void Label(GLenum identifier, GLuint name, GLsizei length, const GLchar* label)
{
    const ObjType kind = LabelType(identifier);
    if (kind == ObjType::Count)
        return;
    Object* o = ObjectOf(Current(), kind, name);
    if (!o)
        return;
    SetLabel(*o, !label ? std::string() : length < 0 ? std::string(label)
                                                     : std::string(label, (size_t)length));
}

void Draw(GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
    Context* c = Current();
    if (!c || !Recording())
        return;
    DrawParams p;
    p.mode = mode;
    p.first = first;
    p.count = count;
    p.instances = instances;
    BeforeDraw(c, p);
}

void DrawIndexed(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLint baseVertex)
{
    Context* c = Current();
    if (!c || !Recording())
        return;
    DrawParams p;
    p.mode = mode;
    p.count = count;
    p.indexed = true;
    p.indexType = type;
    p.indices = indices;
    p.instances = instances;
    p.baseVertex = baseVertex;
    BeforeDraw(c, p);
}

/**
 * An indirect draw: its counts are in the bound GL_DRAW_INDIRECT_BUFFER, which is read back for them
 * (DrawArraysIndirectCommand is count, instanceCount, first, baseInstance; the elements one has
 * firstIndex and baseVertex where first is).
 */
void DrawIndirect(GLenum mode, GLenum type, const void* indirect)
{
    Context* c = Current();
    if (!c || !Recording())
        return;
    DrawParams p;
    p.mode = mode;
    p.indirect = true;
    p.indirectOffset = indirect;
    p.indexed = type != 0;
    p.indexType = type;
    GLuint command[5] = {};
    const GLuint buffer = BoundBuffer(GL_DRAW_INDIRECT_BUFFER);
    if (buffer && g_gl.glMapBufferRange)
    {
        GLint copyRead = GetInt(GL_COPY_READ_BUFFER_BINDING);
        g_gl.glBindBuffer(GL_COPY_READ_BUFFER, buffer);
        const size_t bytes = type ? 20 : 16;
        if (const void* m = g_gl.glMapBufferRange(GL_COPY_READ_BUFFER, (GLintptr)(intptr_t)indirect, (GLsizeiptr)bytes, GL_MAP_READ_BIT))
        {
            memcpy(command, m, bytes);
            g_gl.glUnmapBuffer(GL_COPY_READ_BUFFER);
        }
        g_gl.glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)copyRead);
    }
    p.count = (GLsizei)command[0];
    p.instances = (GLsizei)command[1];
    if (type)
    {
        p.indices = (const void*)(uintptr_t)(command[2] * (type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2
                                                                                                                   : 1));
        p.baseVertex = (GLint)command[3];
    }
    else
    {
        p.first = (GLint)command[2];
    }
    BeforeDraw(c, p);
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Draws

void Pre_glDrawArrays(GLenum mode, GLint first, GLsizei count) { Draw(mode, first, count, 1); }
void Pre_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei n) { Draw(mode, first, count, n); }
void Pre_glDrawArraysInstancedANGLE(GLenum mode, GLint first, GLsizei count, GLsizei n) { Draw(mode, first, count, n); }
void Pre_glDrawArraysInstancedEXT(GLenum mode, GLint first, GLsizei count, GLsizei n) { Draw(mode, first, count, n); }
void Pre_glDrawArraysInstancedNV(GLenum mode, GLint first, GLsizei count, GLsizei n) { Draw(mode, first, count, n); }
void Pre_glDrawArraysInstancedBaseInstanceEXT(GLenum mode, GLint first, GLsizei count, GLsizei n, GLuint) { Draw(mode, first, count, n); }
void Pre_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) { DrawIndexed(mode, count, type, indices, 1, 0); }
void Pre_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint bv) { DrawIndexed(mode, count, type, indices, 1, bv); }
void Pre_glDrawElementsBaseVertexEXT(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint bv) { DrawIndexed(mode, count, type, indices, 1, bv); }
void Pre_glDrawElementsBaseVertexOES(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint bv) { DrawIndexed(mode, count, type, indices, 1, bv); }
void Pre_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n) { DrawIndexed(mode, count, type, indices, n, 0); }
void Pre_glDrawElementsInstancedANGLE(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n) { DrawIndexed(mode, count, type, indices, n, 0); }
void Pre_glDrawElementsInstancedEXT(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n) { DrawIndexed(mode, count, type, indices, n, 0); }
void Pre_glDrawElementsInstancedNV(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n) { DrawIndexed(mode, count, type, indices, n, 0); }
void Pre_glDrawElementsInstancedBaseInstanceEXT(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n, GLuint) { DrawIndexed(mode, count, type, indices, n, 0); }
void Pre_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n, GLint bv) { DrawIndexed(mode, count, type, indices, n, bv); }
void Pre_glDrawElementsInstancedBaseVertexEXT(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n, GLint bv) { DrawIndexed(mode, count, type, indices, n, bv); }
void Pre_glDrawElementsInstancedBaseVertexOES(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n, GLint bv) { DrawIndexed(mode, count, type, indices, n, bv); }
void Pre_glDrawElementsInstancedBaseVertexBaseInstanceEXT(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei n, GLint bv, GLuint) { DrawIndexed(mode, count, type, indices, n, bv); }
void Pre_glDrawRangeElements(GLenum mode, GLuint, GLuint, GLsizei count, GLenum type, const void* indices) { DrawIndexed(mode, count, type, indices, 1, 0); }
void Pre_glDrawRangeElementsBaseVertex(GLenum mode, GLuint, GLuint, GLsizei count, GLenum type, const void* indices, GLint bv) { DrawIndexed(mode, count, type, indices, 1, bv); }
void Pre_glDrawRangeElementsBaseVertexEXT(GLenum mode, GLuint, GLuint, GLsizei count, GLenum type, const void* indices, GLint bv) { DrawIndexed(mode, count, type, indices, 1, bv); }
void Pre_glDrawRangeElementsBaseVertexOES(GLenum mode, GLuint, GLuint, GLsizei count, GLenum type, const void* indices, GLint bv) { DrawIndexed(mode, count, type, indices, 1, bv); }
void Pre_glDrawArraysIndirect(GLenum mode, const void* indirect) { DrawIndirect(mode, 0, indirect); }
void Pre_glDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) { DrawIndirect(mode, type, indirect); }
// A multi-draw is recorded as one command: its state is the state of each of its draws, and its first
// draw's reads stand for the rest.
void Pre_glMultiDrawArraysEXT(GLenum mode, const GLint* first, const GLsizei* count, GLsizei n)
{
    if (n > 0 && first && count)
        Draw(mode, first[0], count[0], 1);
}
void Pre_glMultiDrawElementsEXT(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices, GLsizei n)
{
    if (n > 0 && count && indices)
        DrawIndexed(mode, count[0], type, indices[0], 1, 0);
}
void Pre_glMultiDrawArraysIndirectEXT(GLenum mode, const void* indirect, GLsizei, GLsizei) { DrawIndirect(mode, 0, indirect); }
void Pre_glMultiDrawElementsIndirectEXT(GLenum mode, GLenum type, const void* indirect, GLsizei, GLsizei) { DrawIndirect(mode, type, indirect); }

void Pre_glDispatchCompute(GLuint, GLuint, GLuint) { BeforeDispatch(Current(), false, 0); }
void Pre_glDispatchComputeIndirect(GLintptr indirect) { BeforeDispatch(Current(), true, indirect); }

// ------------------------------------------------------------------------------------------------
// Passes

namespace
{
/** glClearBuffer*'s buffer as the glClear bit it amounts to. */
GLbitfield ClearBit(GLenum buffer)
{
    return buffer == GL_COLOR ? GL_COLOR_BUFFER_BIT : buffer == GL_DEPTH ? GL_DEPTH_BUFFER_BIT
        : buffer == GL_STENCIL                                           ? GL_STENCIL_BUFFER_BIT
        : buffer == GL_DEPTH_STENCIL                                     ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT
                                                                         : 0;
}
}  // namespace

void Pre_glClear(GLbitfield mask) { BeforeFramebufferWrite(Current(), mask); }
void Pre_glClearBufferiv(GLenum buffer, GLint, const GLint*) { BeforeFramebufferWrite(Current(), ClearBit(buffer)); }
void Pre_glClearBufferuiv(GLenum buffer, GLint, const GLuint*) { BeforeFramebufferWrite(Current(), ClearBit(buffer)); }
void Pre_glClearBufferfv(GLenum buffer, GLint, const GLfloat*) { BeforeFramebufferWrite(Current(), ClearBit(buffer)); }
void Pre_glClearBufferfi(GLenum buffer, GLint, GLfloat, GLint) { BeforeFramebufferWrite(Current(), ClearBit(buffer)); }
void Pre_glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) { BeforeFramebufferWrite(Current(), 0); }
void Pre_glBlitFramebufferANGLE(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) { BeforeFramebufferWrite(Current(), 0); }
void Pre_glBlitFramebufferNV(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) { BeforeFramebufferWrite(Current(), 0); }
void Pre_glInvalidateFramebuffer(GLenum target, GLsizei n, const GLenum* attachments) { BeforeInvalidate(Current(), target, n, attachments); }
void Pre_glInvalidateSubFramebuffer(GLenum target, GLsizei n, const GLenum* attachments, GLint, GLint, GLsizei, GLsizei)
{
    // Part of the target only: the rest stays, so nothing is thrown away as far as the pass is concerned.
    BeforeInvalidate(Current(), target, 0, attachments);
    (void)n;
}
void Pre_glDiscardFramebufferEXT(GLenum target, GLsizei n, const GLenum* attachments) { BeforeInvalidate(Current(), target, n, attachments); }

void Pre_glBindFramebuffer(GLenum target, GLuint framebuffer) { BeforeBindFramebuffer(Current(), target, framebuffer); }

void Post_glPushDebugGroup(GLenum, GLuint, GLsizei, const GLchar*) { AfterPushGroup(Current()); }
void Post_glPushDebugGroupKHR(GLenum, GLuint, GLsizei, const GLchar*) { AfterPushGroup(Current()); }
void Post_glPushGroupMarkerEXT(GLsizei, const GLchar*) { AfterPushGroup(Current()); }
void Pre_glPopDebugGroup() { BeforePopGroup(Current()); }
void Pre_glPopDebugGroupKHR() { BeforePopGroup(Current()); }
void Pre_glPopGroupMarkerEXT() { BeforePopGroup(Current()); }
void Post_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    Context* c = Current();
    if (!c)
        return;
    EnsureName(c, ObjType::Framebuffer, framebuffer, "glBindFramebuffer");
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
        c->drawFramebuffer = framebuffer;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
        c->readFramebuffer = framebuffer;
}

void Pre_glFramebufferTexture2D(GLenum target, GLenum, GLenum, GLuint, GLint) { BeforeAttachmentChange(Current(), target); }
void Pre_glFramebufferTexture2DMultisampleEXT(GLenum target, GLenum, GLenum, GLuint, GLint, GLsizei) { BeforeAttachmentChange(Current(), target); }
void Pre_glFramebufferTextureLayer(GLenum target, GLenum, GLuint, GLint, GLint) { BeforeAttachmentChange(Current(), target); }
void Pre_glFramebufferTexture(GLenum target, GLenum, GLuint, GLint) { BeforeAttachmentChange(Current(), target); }
void Pre_glFramebufferTextureEXT(GLenum target, GLenum, GLuint, GLint) { BeforeAttachmentChange(Current(), target); }
void Pre_glFramebufferRenderbuffer(GLenum target, GLenum, GLenum, GLuint) { BeforeAttachmentChange(Current(), target); }
void Pre_glFramebufferTextureMultiviewOVR(GLenum target, GLenum, GLuint, GLint, GLint, GLsizei) { BeforeAttachmentChange(Current(), target); }
void Pre_glFramebufferTextureMultisampleMultiviewOVR(GLenum target, GLenum, GLuint, GLint, GLsizei, GLint, GLsizei) { BeforeAttachmentChange(Current(), target); }

void Post_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
    Attach(Current(), target, attachment, ObjType::Texture, texture, level, 0, IsCubeFace(textarget) ? textarget : 0);
}
void Post_glFramebufferTexture2DMultisampleEXT(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLsizei)
{
    Attach(Current(), target, attachment, ObjType::Texture, texture, level, 0, IsCubeFace(textarget) ? textarget : 0);
}
void Post_glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer)
{
    Attach(Current(), target, attachment, ObjType::Texture, texture, level, layer, 0);
}
void Post_glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level)
{
    Attach(Current(), target, attachment, ObjType::Texture, texture, level, 0, 0);
}
void Post_glFramebufferTextureEXT(GLenum target, GLenum attachment, GLuint texture, GLint level)
{
    Attach(Current(), target, attachment, ObjType::Texture, texture, level, 0, 0);
}
void Post_glFramebufferTextureMultiviewOVR(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint base, GLsizei)
{
    Attach(Current(), target, attachment, ObjType::Texture, texture, level, base, 0);
}
void Post_glFramebufferTextureMultisampleMultiviewOVR(GLenum target, GLenum attachment, GLuint texture, GLint level, GLsizei, GLint base, GLsizei)
{
    Attach(Current(), target, attachment, ObjType::Texture, texture, level, base, 0);
}
void Post_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum, GLuint renderbuffer)
{
    Attach(Current(), target, attachment, ObjType::Renderbuffer, renderbuffer, 0, 0, 0);
}

// ------------------------------------------------------------------------------------------------
// Objects made and deleted

void Post_glGenBuffers(GLsizei n, GLuint* names) { Generated(ObjType::Buffer, n, names, "glGenBuffers"); }
void Post_glGenTextures(GLsizei n, GLuint* names) { Generated(ObjType::Texture, n, names, "glGenTextures"); }
void Post_glGenFramebuffers(GLsizei n, GLuint* names) { Generated(ObjType::Framebuffer, n, names, "glGenFramebuffers"); }
void Post_glGenRenderbuffers(GLsizei n, GLuint* names) { Generated(ObjType::Renderbuffer, n, names, "glGenRenderbuffers"); }
void Post_glGenVertexArrays(GLsizei n, GLuint* names) { Generated(ObjType::VertexArray, n, names, "glGenVertexArrays"); }
void Post_glGenVertexArraysOES(GLsizei n, GLuint* names) { Generated(ObjType::VertexArray, n, names, "glGenVertexArraysOES"); }
void Post_glGenSamplers(GLsizei n, GLuint* names) { Generated(ObjType::Sampler, n, names, "glGenSamplers"); }
void Post_glGenQueries(GLsizei n, GLuint* names) { Generated(ObjType::Query, n, names, "glGenQueries"); }
void Post_glGenQueriesEXT(GLsizei n, GLuint* names) { Generated(ObjType::Query, n, names, "glGenQueriesEXT"); }
void Post_glGenTransformFeedbacks(GLsizei n, GLuint* names) { Generated(ObjType::TransformFeedback, n, names, "glGenTransformFeedbacks"); }
void Post_glGenProgramPipelines(GLsizei n, GLuint* names) { Generated(ObjType::ProgramPipeline, n, names, "glGenProgramPipelines"); }
void Post_glGenProgramPipelinesEXT(GLsizei n, GLuint* names) { Generated(ObjType::ProgramPipeline, n, names, "glGenProgramPipelinesEXT"); }

void Late_glDeleteBuffers(GLsizei n, const GLuint* names) { Deleted(ObjType::Buffer, n, names); }
void Late_glDeleteTextures(GLsizei n, const GLuint* names) { Deleted(ObjType::Texture, n, names); }
void Late_glDeleteFramebuffers(GLsizei n, const GLuint* names) { Deleted(ObjType::Framebuffer, n, names); }
void Late_glDeleteRenderbuffers(GLsizei n, const GLuint* names) { Deleted(ObjType::Renderbuffer, n, names); }
void Late_glDeleteVertexArrays(GLsizei n, const GLuint* names) { Deleted(ObjType::VertexArray, n, names); }
void Late_glDeleteVertexArraysOES(GLsizei n, const GLuint* names) { Deleted(ObjType::VertexArray, n, names); }
void Late_glDeleteSamplers(GLsizei n, const GLuint* names) { Deleted(ObjType::Sampler, n, names); }
void Late_glDeleteQueries(GLsizei n, const GLuint* names) { Deleted(ObjType::Query, n, names); }
void Late_glDeleteQueriesEXT(GLsizei n, const GLuint* names) { Deleted(ObjType::Query, n, names); }
void Late_glDeleteTransformFeedbacks(GLsizei n, const GLuint* names) { Deleted(ObjType::TransformFeedback, n, names); }
void Late_glDeleteProgramPipelines(GLsizei n, const GLuint* names) { Deleted(ObjType::ProgramPipeline, n, names); }
void Late_glDeleteProgramPipelinesEXT(GLsizei n, const GLuint* names) { Deleted(ObjType::ProgramPipeline, n, names); }
void Late_glDeleteShader(GLuint shader) { Deleted(ObjType::Shader, 1, &shader); }
void Late_glFlush() { AfterFlush(Current()); }
void Late_glFinish() { AfterFlush(Current()); }
void Late_glDeleteProgram(GLuint program) { Deleted(ObjType::Program, 1, &program); }
void Late_glDeleteSync(GLsync sync)
{
    uint64_t id = 0;
    {
        std::lock_guard lock(State().mutex);
        auto it = State().syncs.find(sync);
        if (it == State().syncs.end())
            return;
        id = it->second;
        State().syncs.erase(it);
    }
    Forget(id);
}

void Post_glFenceSync(GLenum condition, GLbitfield, GLsync result)
{
    Context* c = Current();
    if (!c || !result)
        return;
    Object& o = NewObject("GLSync", ObjType::Sync, c->id, "glFenceSync");
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)result);
    o.handle = buf;
    o.args.push_back({"condition", JsonEnum(condition)});
    {
        std::lock_guard lock(State().mutex);
        State().syncs[result] = o.id;
    }
    Announce(o);
}

// ------------------------------------------------------------------------------------------------
// Bindings

void Post_glBindBuffer(GLenum, GLuint buffer) { EnsureName(Current(), ObjType::Buffer, buffer, "glBindBuffer"); }
void Post_glBindBufferBase(GLenum, GLuint, GLuint buffer) { EnsureName(Current(), ObjType::Buffer, buffer, "glBindBufferBase"); }
void Post_glBindBufferRange(GLenum, GLuint, GLuint buffer, GLintptr, GLsizeiptr) { EnsureName(Current(), ObjType::Buffer, buffer, "glBindBufferRange"); }
void Post_glBindRenderbuffer(GLenum, GLuint renderbuffer) { EnsureName(Current(), ObjType::Renderbuffer, renderbuffer, "glBindRenderbuffer"); }
void Post_glBindVertexArray(GLuint array) { EnsureName(Current(), ObjType::VertexArray, array, "glBindVertexArray"); }
void Post_glBindVertexArrayOES(GLuint array) { EnsureName(Current(), ObjType::VertexArray, array, "glBindVertexArrayOES"); }
void Post_glBindSampler(GLuint, GLuint sampler) { EnsureName(Current(), ObjType::Sampler, sampler, "glBindSampler"); }
void Post_glBindTransformFeedback(GLenum, GLuint id) { EnsureName(Current(), ObjType::TransformFeedback, id, "glBindTransformFeedback"); }
void Post_glBindTexture(GLenum target, GLuint texture)
{
    Context* c = Current();
    if (!c || !texture)
        return;
    EnsureName(c, ObjType::Texture, texture, "glBindTexture");
    // A texture takes its target at its first bind.
    if (Object* t = ObjectOf(c, ObjType::Texture, texture); t && !t->target)
    {
        t->target = target;
        Describe(*t, "target", JsonEnum(target, EnumName_TextureTarget));
    }
}

// ------------------------------------------------------------------------------------------------
// Buffers

void Post_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
    if (Context* c = Current())
        BufferShape(c, target, size, data, usage, false);
}
void Post_glBufferStorageEXT(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags)
{
    if (Context* c = Current())
        BufferShape(c, target, size, data, flags, true);
}
void Post_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
    Context* c = Current();
    if (!c)
        return;
    Object* b = BufferAt(c, target);
    if (!b)
        return;
    ++b->generation;
    if (!c->es3() && data && b->shadow.size() >= (size_t)(offset + size))
        memcpy(b->shadow.data() + offset, data, (size_t)size);
}
void Post_glCopyBufferSubData(GLenum, GLenum writeTarget, GLintptr, GLintptr, GLsizeiptr)
{
    if (Context* c = Current())
    {
        if (Object* b = BufferAt(c, writeTarget))
            ++b->generation;
    }
}
void Post_glMapBufferRange(GLenum target, GLintptr, GLsizeiptr, GLbitfield, void*)
{
    if (Context* c = Current())
    {
        if (Object* b = BufferAt(c, target))
            ++b->generation;
    }
}
void Post_glMapBufferRangeEXT(GLenum target, GLintptr, GLsizeiptr, GLbitfield, void*)
{
    if (Context* c = Current())
    {
        if (Object* b = BufferAt(c, target))
        {
            ++b->generation;
            if (!c->es3())
                b->shadow.clear();   // written through a mapping the library cannot see
        }
    }
}
void Post_glMapBufferOES(GLenum target, GLenum, void*)
{
    if (Context* c = Current())
    {
        if (Object* b = BufferAt(c, target))
        {
            ++b->generation;
            if (!c->es3())
                b->shadow.clear();
        }
    }
}
void Post_glUnmapBuffer(GLenum target, GLboolean)
{
    if (Context* c = Current())
    {
        if (Object* b = BufferAt(c, target))
            ++b->generation;
    }
}
void Post_glUnmapBufferOES(GLenum target, GLboolean) { Post_glUnmapBuffer(target, GL_TRUE); }

// ------------------------------------------------------------------------------------------------
// Textures and renderbuffers

void Post_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type, const void*)
{
    if (Context* c = Current())
        TextureShape(c, target, level, SizedFormat((GLenum)internalformat, format, type), w, h, 1, 0, 0);
}
void Post_glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLsizei d, GLint, GLenum format, GLenum type, const void*)
{
    if (Context* c = Current())
        TextureShape(c, target, level, SizedFormat((GLenum)internalformat, format, type), w, h, d, 0, 0);
}
void Post_glTexImage3DOES(GLenum target, GLint level, GLenum internalformat, GLsizei w, GLsizei h, GLsizei d, GLint, GLenum format, GLenum type, const void*)
{
    if (Context* c = Current())
        TextureShape(c, target, level, SizedFormat(internalformat, format, type), w, h, d, 0, 0);
}
void Post_glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint, GLint, GLsizei w, GLsizei h, GLint)
{
    if (Context* c = Current())
        TextureShape(c, target, level, SizedFormat(internalformat, internalformat, GL_UNSIGNED_BYTE), w, h, 1, 0, 0);
}
void Post_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei w, GLsizei h)
{
    if (Context* c = Current())
        TextureShape(c, target, 0, internalformat, w, h, 1, levels, 0);
}
void Post_glTexStorage2DEXT(GLenum target, GLsizei levels, GLenum internalformat, GLsizei w, GLsizei h)
{
    if (Context* c = Current())
        TextureShape(c, target, 0, internalformat, w, h, 1, levels, 0);
}
void Post_glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei w, GLsizei h, GLsizei d)
{
    if (Context* c = Current())
        TextureShape(c, target, 0, internalformat, w, h, d, levels, 0);
}
void Post_glTexStorage3DEXT(GLenum target, GLsizei levels, GLenum internalformat, GLsizei w, GLsizei h, GLsizei d)
{
    if (Context* c = Current())
        TextureShape(c, target, 0, internalformat, w, h, d, levels, 0);
}
void Post_glTexStorage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei w, GLsizei h, GLboolean)
{
    if (Context* c = Current())
        TextureShape(c, target, 0, internalformat, w, h, 1, 1, samples);
}
void Post_glTexStorage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei w, GLsizei h, GLsizei d, GLboolean)
{
    if (Context* c = Current())
        TextureShape(c, target, 0, internalformat, w, h, d, 1, samples);
}
void Post_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei w, GLsizei h, GLint, GLsizei imageSize, const void* data)
{
    Context* c = Current();
    if (!c)
        return;
    TextureShape(c, target, level, internalformat, w, h, 1, 0, 0);
    if (level == 0)
    {
        if (Object* t = TextureAt(c, target))
            KeepCompressed(c, *t, target, 1, imageSize, data);
    }
}
void Post_glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei w, GLsizei h, GLsizei d, GLint, GLsizei imageSize, const void* data)
{
    Context* c = Current();
    if (!c)
        return;
    TextureShape(c, target, level, internalformat, w, h, d, 0, 0);
    if (level == 0)
    {
        if (Object* t = TextureAt(c, target))
            KeepCompressed(c, *t, target, d, imageSize, data);
    }
}
void Post_glCompressedTexImage3DOES(GLenum target, GLint level, GLenum internalformat, GLsizei w, GLsizei h, GLsizei d, GLint border, GLsizei imageSize, const void* data)
{
    Post_glCompressedTexImage3D(target, level, internalformat, w, h, d, border, imageSize, data);
}
void Post_glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum, GLsizei imageSize, const void* data)
{
    Context* c = Current();
    if (!c || level != 0)
        return;
    Object* t = TextureAt(c, target);
    if (!t)
        return;
    ++t->generation;
    // A texture made with glTexStorage gets its compressed contents this way: whole-level updates are kept.
    if (x == 0 && y == 0 && w == t->width && h == t->height)
        KeepCompressed(c, *t, target, 1, imageSize, data);
}
void Post_glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES)
{
    if (Context* c = Current())
    {
        if (Object* t = TextureAt(c, target))
        {
            ++t->generation;
            Describe(*t, "eglImage", JsonBool(true));
        }
    }
}
void Post_glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer)
{
    if (Context* c = Current())
    {
        if (Object* t = TextureAt(c, target))
        {
            t->internalFormat = internalformat;
            Describe(*t, "buffer", JsonRef(RefOf(ObjType::Buffer, buffer), "GLBuffer"));
            Describe(*t, "internalFormat", JsonEnum(internalformat));
        }
    }
}
void Post_glTexBufferEXT(GLenum target, GLenum internalformat, GLuint buffer) { Post_glTexBuffer(target, internalformat, buffer); }
void Post_glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr, GLsizeiptr) { Post_glTexBuffer(target, internalformat, buffer); }
void Post_glTexBufferRangeEXT(GLenum target, GLenum internalformat, GLuint buffer, GLintptr, GLsizeiptr) { Post_glTexBuffer(target, internalformat, buffer); }

void Post_glRenderbufferStorage(GLenum, GLenum internalformat, GLsizei w, GLsizei h)
{
    if (Context* c = Current())
        RenderbufferShape(c, internalformat, w, h, 0);
}
void Post_glRenderbufferStorageMultisample(GLenum, GLsizei samples, GLenum internalformat, GLsizei w, GLsizei h)
{
    if (Context* c = Current())
        RenderbufferShape(c, internalformat, w, h, samples);
}
void Post_glRenderbufferStorageMultisampleEXT(GLenum, GLsizei samples, GLenum internalformat, GLsizei w, GLsizei h)
{
    if (Context* c = Current())
        RenderbufferShape(c, internalformat, w, h, samples);
}
void Post_glRenderbufferStorageMultisampleANGLE(GLenum, GLsizei samples, GLenum internalformat, GLsizei w, GLsizei h)
{
    if (Context* c = Current())
        RenderbufferShape(c, internalformat, w, h, samples);
}
void Post_glEGLImageTargetRenderbufferStorageOES(GLenum, GLeglImageOES) {}

// ------------------------------------------------------------------------------------------------
// Shaders and programs

void Post_glCreateShader(GLenum type, GLuint result)
{
    Context* c = Current();
    if (!c || !result)
        return;
    RegisterName(c, ObjType::Shader, result, "glCreateShader");
    if (Object* s = ObjectOf(c, ObjType::Shader, result))
    {
        s->shaderType = type;
        Describe(*s, "type", JsonEnum(type, EnumName_ShaderType));
    }
}

void Post_glCreateProgram(GLuint result)
{
    if (Context* c = Current())
        RegisterName(c, ObjType::Program, result, "glCreateProgram");
}

static void ShaderProgram(GLenum type, GLsizei count, const GLchar* const* strings, GLuint result, const char* cmd)
{
    Context* c = Current();
    if (!c || !result)
        return;
    RegisterName(c, ObjType::Program, result, cmd);
    if (Object* p = ObjectOf(c, ObjType::Program, result))
    {
        Describe(*p, "separable", JsonBool(true));
        Describe(*p, "stage", JsonEnum(type));
        Describe(*p, "source", JsonString(JoinStrings(count, strings, nullptr)));
        Reflect(c, *p);
    }
}
void Post_glCreateShaderProgramv(GLenum type, GLsizei count, const GLchar* const* strings, GLuint result)
{
    ShaderProgram(type, count, strings, result, "glCreateShaderProgramv");
}
void Post_glCreateShaderProgramvEXT(GLenum type, GLsizei count, const GLchar* const* strings, GLuint result)
{
    ShaderProgram(type, count, strings, result, "glCreateShaderProgramvEXT");
}

void Post_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length)
{
    Context* c = Current();
    Object* s = ObjectOf(c, ObjType::Shader, shader);
    if (!s)
        return;
    s->source = JoinStrings(count, string, length);
    Describe(*s, "source", JsonString(s->source));
}

void Post_glCompileShader(GLuint shader)
{
    Context* c = Current();
    Object* s = ObjectOf(c, ObjType::Shader, shader);
    if (!s)
        return;
    GLint compiled = 0, logLength = 0;
    g_gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    g_gl.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::string log;
    if (logLength > 1)
    {
        log.resize((size_t)logLength);
        GLsizei got = 0;
        g_gl.glGetShaderInfoLog(shader, logLength, &got, log.data());
        log.resize((size_t)std::max(0, got));
    }
    Describe(*s, "compiled", JsonBool(compiled != 0));
    Describe(*s, "infoLog", JsonString(log));
}

void Post_glAttachShader(GLuint program, GLuint shader)
{
    Context* c = Current();
    Object* p = ObjectOf(c, ObjType::Program, program);
    if (!p)
        return;
    if (std::find(p->shaders.begin(), p->shaders.end(), shader) == p->shaders.end())
        p->shaders.push_back(shader);
    DescribeShaders(c, *p);
}

void Post_glDetachShader(GLuint program, GLuint shader)
{
    Context* c = Current();
    Object* p = ObjectOf(c, ObjType::Program, program);
    if (!p)
        return;
    p->shaders.erase(std::remove(p->shaders.begin(), p->shaders.end(), shader), p->shaders.end());
    DescribeShaders(c, *p);
}

void Post_glLinkProgram(GLuint program)
{
    Context* c = Current();
    Object* p = ObjectOf(c, ObjType::Program, program);
    if (!p)
        return;
    // What was linked, kept with the program: shaders are routinely detached and deleted right after.
    JsonWriter w;
    w.BeginArray();
    for (GLuint s : p->shaders)
    {
        Object* shader = ObjectOf(c, ObjType::Shader, s);
        w.BeginObject();
        w.Key("shader");
        w.Ref(shader ? shader->id : 0, "GLShader");
        w.Key("type");
        w.Raw(shader ? JsonEnum(shader->shaderType, EnumName_ShaderType) : "null");
        w.Key("source");
        w.String(shader ? shader->source : std::string());
        w.EndObject();
    }
    w.EndArray();
    Describe(*p, "stages", w.str());
    Reflect(c, *p);
}

void Post_glProgramBinary(GLuint program, GLenum, const void*, GLsizei)
{
    Context* c = Current();
    if (Object* p = ObjectOf(c, ObjType::Program, program))
    {
        Describe(*p, "fromBinary", JsonBool(true));
        Reflect(c, *p);
    }
}
void Post_glProgramBinaryOES(GLuint program, GLenum format, const void* binary, GLint length)
{
    Post_glProgramBinary(program, format, binary, (GLsizei)length);
}

// ------------------------------------------------------------------------------------------------
// Labels, errors

void Post_glObjectLabel(GLenum identifier, GLuint name, GLsizei length, const GLchar* label) { Label(identifier, name, length, label); }
void Post_glObjectLabelKHR(GLenum identifier, GLuint name, GLsizei length, const GLchar* label) { Label(identifier, name, length, label); }
void Post_glLabelObjectEXT(GLenum type, GLuint object, GLsizei length, const GLchar* label) { Label(type, object, length, label); }

GLenum Override_glGetError()
{
    if (const GLenum saved = TakeSavedError())
        return saved;
    return g_gl.glGetError ? g_gl.glGetError() : GL_NO_ERROR;
}

}  // namespace glesinsp
