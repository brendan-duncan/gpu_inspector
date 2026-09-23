// The scene the OpenGL ES test applications draw (test/android_gles_triangle, test/gles_linux): a spinning,
// textured cube drawn into a framebuffer of its own, then that framebuffer's texture onto the default
// framebuffer. What it uses is what the OpenGL ES plugin's capture has to handle: a vertex array with an
// index buffer, a uniform block, an immutable mipmapped texture, an ETC2 compressed texture (read back
// from the upload, as glReadPixels cannot), debug groups and object labels, and an invalidated depth
// attachment. Include it after the OpenGL ES 3.2 header, with GLES_SCENE_LOG defined as a printf.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace gles_scene
{

inline const char* kSceneVs = R"(#version 300 es
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(std140) uniform Transform { mat4 mvp; float tint; };
out vec2 vUv;
void main() { vUv = uv; gl_Position = mvp * vec4(position, 1.0); }
)";

inline const char* kSceneFs = R"(#version 300 es
precision mediump float;
uniform sampler2D checker;
uniform sampler2D detail;
layout(std140) uniform Transform { mat4 mvp; float tint; };
in vec2 vUv;
out vec4 color;
void main() { color = texture(checker, vUv) * mix(vec4(1.0), texture(detail, vUv * 4.0), tint); }
)";

inline const char* kBlitVs = R"(#version 300 es
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

inline const char* kBlitFs = R"(#version 300 es
precision mediump float;
uniform sampler2D image;
in vec2 vUv;
out vec4 color;
void main() { color = texture(image, vUv); }
)";

inline GLuint Compile(GLenum type, const char* source)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        GLES_SCENE_LOG("shader: %s", log);
    }
    return s;
}

inline GLuint Link(const char* vs, const char* fs, const char* label)
{
    GLuint p = glCreateProgram();
    GLuint v = Compile(GL_VERTEX_SHADER, vs), f = Compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glObjectLabel(GL_PROGRAM, p, -1, label);
    return p;
}

struct Mat4
{
    float m[16];
};

inline Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                r.m[c * 4 + row] += a.m[k * 4 + row] * b.m[c * 4 + k];
    return r;
}

inline Mat4 Mvp(float t, float aspect)
{
    const float f = 1.0f / std::tan(0.6f), n = 0.1f, fa = 10.0f;
    Mat4 proj{{f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (fa + n) / (n - fa), -1, 0, 0, 2 * fa * n / (n - fa), 0}};
    const float cy = std::cos(t), sy = std::sin(t), cx = std::cos(t * 0.7f), sx = std::sin(t * 0.7f);
    Mat4 ry{{cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1}};
    Mat4 rx{{1, 0, 0, 0, 0, cx, sx, 0, 0, -sx, cx, 0, 0, 0, 0, 1}};
    Mat4 view{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -3.2f, 1}};
    return Multiply(proj, Multiply(view, Multiply(rx, ry)));
}

struct Scene
{
    GLuint scene = 0, blit = 0, vao = 0, vbo = 0, ibo = 0, ubo = 0;
    GLuint checker = 0, detail = 0, target = 0, depth = 0, fbo = 0, blitVao = 0;
    static constexpr int kTarget = 256;

    void Create()
    {
        GLES_SCENE_LOG("%s, %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
        scene = Link(kSceneVs, kSceneFs, "Scene");
        blit = Link(kBlitVs, kBlitFs, "Blit");
        glUniformBlockBinding(scene, glGetUniformBlockIndex(scene, "Transform"), 0);
        glUseProgram(scene);
        glUniform1i(glGetUniformLocation(scene, "checker"), 0);
        glUniform1i(glGetUniformLocation(scene, "detail"), 1);
        glUseProgram(blit);
        glUniform1i(glGetUniformLocation(blit, "image"), 0);

        // A cube: 24 vertices (position, uv), 36 indices.
        std::vector<float> v;
        std::vector<uint16_t> idx;
        const int faces[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
        for (int face = 0; face < 6; ++face)
        {
            const float s = (face & 1) ? -1.0f : 1.0f;
            const int* a = faces[face];
            const uint16_t base = (uint16_t)(v.size() / 5);
            for (int corner = 0; corner < 4; ++corner)
            {
                const float u = (corner & 1) ? 1.0f : 0.0f, w = (corner & 2) ? 1.0f : 0.0f;
                float p[3];
                p[a[0]] = s;
                p[a[1]] = (u * 2 - 1) * s;
                p[a[2]] = w * 2 - 1;
                v.insert(v.end(), {p[0], p[1], p[2], u, w});
            }
            idx.insert(idx.end(), {base, (uint16_t)(base + 1), (uint16_t)(base + 2), (uint16_t)(base + 2), (uint16_t)(base + 1), (uint16_t)(base + 3)});
        }
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(float)), v.data(), GL_STATIC_DRAW);
        glObjectLabel(GL_BUFFER, vbo, -1, "Cube vertices");
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(uint16_t)), idx.data(), GL_STATIC_DRAW);
        glObjectLabel(GL_BUFFER, ibo, -1, "Cube indices");
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void*)12);
        glBindVertexArray(0);
        glGenVertexArrays(1, &blitVao);

        glGenBuffers(1, &ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferData(GL_UNIFORM_BUFFER, 80, nullptr, GL_DYNAMIC_DRAW);
        glObjectLabel(GL_BUFFER, ubo, -1, "Transform");

        // A 64x64 checker board, immutable and mipmapped.
        std::vector<uint8_t> px(64 * 64 * 4);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
            {
                const bool on = ((x / 8) + (y / 8)) & 1;
                uint8_t* p = &px[(y * 64 + x) * 4];
                p[0] = on ? 240 : 40;
                p[1] = on ? 190 : 60;
                p[2] = on ? 40 : 200;
                p[3] = 255;
            }
        glGenTextures(1, &checker);
        glBindTexture(GL_TEXTURE_2D, checker);
        glTexStorage2D(GL_TEXTURE_2D, 7, GL_RGBA8, 64, 64);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glObjectLabel(GL_TEXTURE, checker, -1, "Checker");

        // A 16x16 ETC2 texture: 4x4 blocks of flat color (individual mode, table 0, all indices 0).
        uint8_t blocks[16 * 8] = {};
        for (int b = 0; b < 16; ++b)
        {
            const uint8_t r = (b & 1) ? 0xF : 0x6, g = (b & 2) ? 0xE : 0x5, bl = (b & 4) ? 0xD : 0x7;
            uint8_t* p = &blocks[b * 8];
            p[0] = (uint8_t)(r << 4 | r);
            p[1] = (uint8_t)(g << 4 | g);
            p[2] = (uint8_t)(bl << 4 | bl);
        }
        glGenTextures(1, &detail);
        glBindTexture(GL_TEXTURE_2D, detail);
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB8_ETC2, 16, 16, 0, sizeof(blocks), blocks);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glObjectLabel(GL_TEXTURE, detail, -1, "Detail (ETC2)");

        // The offscreen target the cube is drawn into.
        glGenTextures(1, &target);
        glBindTexture(GL_TEXTURE_2D, target);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kTarget, kTarget);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glObjectLabel(GL_TEXTURE, target, -1, "Offscreen color");
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kTarget, kTarget);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        glObjectLabel(GL_FRAMEBUFFER, fbo, -1, "Offscreen");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /** One frame at time `t`, into a default framebuffer of `w` x `h`; the caller swaps. */
    void Render(float t, int w, int h)
    {

        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 1, -1, "Offscreen cube");
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, kTarget, kTarget);
        glClearColor(0.08f, 0.1f, 0.18f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        struct
        {
            Mat4 mvp;
            float tint;
            float pad[3];
        } block{Mvp(t, 1.0f), 0.6f, {}};
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(block), &block);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
        glUseProgram(scene);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, checker);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, detail);
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
        glBindVertexArray(0);
        const GLenum discard = GL_DEPTH_ATTACHMENT;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &discard);
        glPopDebugGroup();

        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 2, -1, "Present");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        const int side = w < h ? w : h;
        glViewport((w - side) / 2, (h - side) / 2, side, side);
        glUseProgram(blit);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, target);
        glBindVertexArray(blitVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glPopDebugGroup();
    }
};

}  // namespace gles_scene
