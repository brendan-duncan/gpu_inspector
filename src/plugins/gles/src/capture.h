// Frame capture for OpenGL ES.
//
// OpenGL ES has no command buffers and no passes, so the capture is the calls themselves, in the
// order each context made them between two eglSwapBuffers, with what the inspector needs added:
//
//   Passes. A pass begins at the first draw, clear or blit into a framebuffer and ends when the draw
//   framebuffer changes, its attachments change, or the surface is swapped. The library records a
//   BeginRenderPass before the command that began it and an EndRenderPass before the one that ended
//   it (neither is a GL call; `synthetic` says so), and reads the pass's color attachments back when
//   it ends -- or at a glInvalidateFramebuffer / glDiscardFramebufferEXT, which would throw them
//   away first. Passes are numbered per context from its last swap, which is where the inspector's
//   walk back through a context's commands stops (its command sets count eglSwapBuffers as a submit).
//
//   State. A draw's state is the context's, not something bound on a command, so every draw and
//   dispatch carries a snapshot of what was in effect, asked of the driver just before it ran
//   (`state`: the program, the vertex attributes with their buffers, the textures, the uniform blocks
//   and uniform values, the fixed-function state). The buffers and textures it names are read back
//   there and then, once per contents, and the snapshot refers to them by capture id.
//
// All of it runs on the thread that makes the calls, with the context current, and leaves the
// application's GL state as it found it (every binding the read-backs move is put back).
#pragma once

#include "state.h"

#include <gpu_inspector/sdk/json.h>

#include <string>

namespace glesinsp
{

struct CaptureOptions
{
    uint32_t frameCount = 1;
    /** The swap count to start at; -1 for the next frame. */
    int64_t atFrame = -1;
    bool captureTextures = true;   // pass attachments
    bool captureBuffers = true;    // vertex, index, uniform buffers
    bool captureImages = true;     // sampled textures
    bool profilePasses = true;     // timestamps around every pass (EXT_disjoint_timer_query)
    size_t maxBufferSize = 64 * 1024;
    size_t maxBufferTotal = 64 * 1024 * 1024;
    size_t maxImageTotal = 256 * 1024 * 1024;
};

/** Arms a capture (the inspector's Capture request). */
void RequestCapture(const gpuinsp::sdk::JsonValue& msg);
/** Whether a capture is recording right now. */
bool Recording();

/** eglSwapBuffers, before the real swap: the frame's last pass ends and the swap is recorded. */
void BeforeSwap(Context* c, EGLDisplay display, EGLSurface surface, const char* method);
/** After it: frame statistics, and the capture starting or finishing on the frame boundary. */
void AfterSwap(Context* c);
/**
 * After glFlush or glFinish. An application that never swaps -- a browser, whose WebGL draws into
 * surfaces its compositor presents -- has no other frame boundary: once 60 flushes pass without a
 * swap, each flush ends a frame. GLESINSP_FRAME_BOUNDARY=flush makes them do so from the start,
 * =swap never.
 */
void AfterFlush(Context* c);

/** What a draw call draws, for the snapshot's read-backs (how many vertices and indices it reads). */
struct DrawParams
{
    GLenum mode = 0;
    GLint first = 0;
    GLsizei count = 0;
    GLsizei instances = 1;
    /** Indexed draws: the index type and the offset (or client pointer) of the first index. */
    bool indexed = false;
    GLenum indexType = 0;
    const void* indices = nullptr;
    GLint baseVertex = 0;
    /** Indirect draws: the offset of the command in the bound GL_DRAW_INDIRECT_BUFFER. */
    bool indirect = false;
    const void* indirectOffset = nullptr;
};

void BeforeDraw(Context* c, const DrawParams& p);
void BeforeDispatch(Context* c, bool indirect, intptr_t indirectOffset);
/** A clear (`clearMask`, the buffers it clears) or a blit (0) into the draw framebuffer: begins its pass. */
void BeforeFramebufferWrite(Context* c, GLbitfield clearMask);
/** glInvalidateFramebuffer / glDiscardFramebufferEXT: the pass's attachments are read before they go. */
void BeforeInvalidate(Context* c, GLenum target, GLsizei count, const GLenum* attachments);
/** glBindFramebuffer: a change of draw framebuffer ends the open pass. */
void BeforeBindFramebuffer(Context* c, GLenum target, GLuint framebuffer);
/** A debug group closes: a pass that began inside it ends first. */
void BeforePopGroup(Context* c);
void AfterPushGroup(Context* c);
/** An attachment of the framebuffer bound to `target` is about to change: a pass drawing to it ends. */
void BeforeAttachmentChange(Context* c, GLenum target);

/** Records a command that is no GL call of the application's (a pass boundary, a swap). */
void RecordCommand(Context* c, const char* method, const std::string& argsJson, bool synthetic);

/** The inspector went away: a capture in progress is abandoned. */
void OnDisconnect();

/** An error the library's own GL calls found pending and took from the application, returned by its next glGetError. */
GLenum TakeSavedError();

/** A uniform or attribute type's GLSL name ("vec4", "sampler2D"), or null for one the library does not know. */
const char* GlslTypeName(GLenum type);

}  // namespace glesinsp
