// The OpenGL ES plugin's backend: how the inspector reads a capture of the OpenGL ES capture library
// (../src). Loaded by the app and the MCP server from plugin.json's `backend` (docs/PLUGINS.md), and
// written against the plugin SDK's types only (src/sdk/ts): what it runs with comes from the host.
//
// What the library records (../src/capture.h): every call the application made between two
// eglSwapBuffers, per context; synthetic BeginRenderPass / EndRenderPass around what drew into one
// framebuffer; and on every draw and dispatch a `state` snapshot of what was bound, with the capture
// ids of the buffers and textures it read back. Everything here reads those.
import type {
  ArgObject, ArgValue, Backend, BoundIndexBuffer, BoundVertexBuffer, CaptureCommand, CommandSets, DetailContext,
  DetailSection, DetailValue, DrawState, InspectorObject, ObjectLookup, PluginHost, RawAccess, RawResource,
  ResourceSource,
} from "../../../sdk/ts/index.js";

const DRAW = new Set([
  "glDrawArrays", "glDrawElements", "glDrawArraysInstanced", "glDrawElementsInstanced", "glDrawRangeElements",
  "glDrawArraysIndirect", "glDrawElementsIndirect", "glDrawElementsBaseVertex", "glDrawRangeElementsBaseVertex",
  "glDrawElementsInstancedBaseVertex", "glDrawArraysInstancedANGLE", "glDrawElementsInstancedANGLE",
  "glDrawArraysInstancedEXT", "glDrawElementsInstancedEXT", "glDrawArraysInstancedNV", "glDrawElementsInstancedNV",
  "glDrawArraysInstancedBaseInstanceEXT", "glDrawElementsInstancedBaseInstanceEXT",
  "glDrawElementsInstancedBaseVertexBaseInstanceEXT", "glDrawElementsBaseVertexEXT", "glDrawElementsBaseVertexOES",
  "glDrawRangeElementsBaseVertexEXT", "glDrawRangeElementsBaseVertexOES", "glDrawElementsInstancedBaseVertexEXT",
  "glDrawElementsInstancedBaseVertexOES", "glMultiDrawArraysEXT", "glMultiDrawElementsEXT",
  "glMultiDrawArraysIndirectEXT", "glMultiDrawElementsIndirectEXT",
]);
const DISPATCH = new Set(["glDispatchCompute", "glDispatchComputeIndirect"]);
// The library brackets what drew into one framebuffer with these; OpenGL ES has no pass of its own.
const PASS_BEGIN = new Set(["BeginRenderPass"]);
const PASS_END = new Set(["EndRenderPass"]);
const LABEL_BEGIN = new Set(["glPushDebugGroup", "glPushDebugGroupKHR", "glPushGroupMarkerEXT"]);
const LABEL_END = new Set(["glPopDebugGroup", "glPopDebugGroupKHR", "glPopGroupMarkerEXT"]);
// A swap ends a context's frame, and the library numbers each context's passes from its last one.
const SUBMIT = new Set(["eglSwapBuffers", "eglSwapBuffersWithDamageKHR", "eglSwapBuffersWithDamageEXT"]);
const BIND_PIPELINE = new Set(["glUseProgram", "glBindProgramPipeline", "glBindProgramPipelineEXT"]);
const INDIRECT = new Set(["glDrawArraysIndirect", "glDrawElementsIndirect", "glDispatchComputeIndirect",
  "glMultiDrawArraysIndirectEXT", "glMultiDrawElementsIndirectEXT"]);
const NONE: ReadonlySet<string> = new Set();

const TOPOLOGY: Record<string, string> = {
  GL_POINTS: "VK_PRIMITIVE_TOPOLOGY_POINT_LIST",
  GL_LINES: "VK_PRIMITIVE_TOPOLOGY_LINE_LIST",
  GL_LINE_STRIP: "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP",
  GL_LINE_LOOP: "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP",
  GL_TRIANGLES: "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST",
  GL_TRIANGLE_STRIP: "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP",
  GL_TRIANGLE_FAN: "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN",
  GL_PATCHES: "VK_PRIMITIVE_TOPOLOGY_PATCH_LIST",
};

const INDEX_BYTES: Record<string, number> = { GL_UNSIGNED_BYTE: 1, GL_UNSIGNED_SHORT: 2, GL_UNSIGNED_INT: 4 };

const INDEX_TYPES: Record<string, string> = {
  GL_UNSIGNED_BYTE: "VK_INDEX_TYPE_UINT8_EXT",
  GL_UNSIGNED_SHORT: "VK_INDEX_TYPE_UINT16",
  GL_UNSIGNED_INT: "VK_INDEX_TYPE_UINT32",
};

/** A vertex attribute's format as the inspector's decoders name it: its GL type, size and how it is read. */
function vertexFormat(type: string, size: number, normalized: boolean, integer: boolean): string {
  const n = Math.max(1, Math.min(4, size));
  const channels = ["R", "G", "B", "A"].slice(0, n);
  const packed = (bits: number, kind: string): string => `VK_FORMAT_${channels.map((c) => `${c}${bits}`).join("")}_${kind}`;
  const kind8 = (signed: boolean): string => (integer ? (signed ? "SINT" : "UINT") : normalized ? (signed ? "SNORM" : "UNORM") : (signed ? "SSCALED" : "USCALED"));
  switch (type) {
    case "GL_FLOAT": return packed(32, "SFLOAT");
    case "GL_HALF_FLOAT": case "GL_HALF_FLOAT_OES": return packed(16, "SFLOAT");
    case "GL_BYTE": return packed(8, kind8(true));
    case "GL_UNSIGNED_BYTE": return packed(8, kind8(false));
    case "GL_SHORT": return packed(16, kind8(true));
    case "GL_UNSIGNED_SHORT": return packed(16, kind8(false));
    case "GL_INT": case "GL_FIXED": return packed(32, "SINT");
    case "GL_UNSIGNED_INT": return packed(32, "UINT");
    case "GL_INT_2_10_10_10_REV": return normalized ? "VK_FORMAT_A2B10G10R10_SNORM_PACK32" : "VK_FORMAT_A2B10G10R10_SINT_PACK32";
    case "GL_UNSIGNED_INT_2_10_10_10_REV": return normalized ? "VK_FORMAT_A2B10G10R10_UNORM_PACK32" : "VK_FORMAT_A2B10G10R10_UINT_PACK32";
    default: return "VK_FORMAT_UNDEFINED";
  }
}

/** "GL_COLOR_BUFFER_BIT" -> "COLOR_BUFFER_BIT", for compact summaries. */
function short(v: ArgValue | undefined): string {
  return typeof v === "string" ? v.replace(/^GL_/, "") : v === null || v === undefined ? "" : String(v);
}

export function activate(host: PluginHost): Backend {
  const { isObject, num, str, refId, formatBytes } = host.util;

  /** An object's description: its arguments, with the updates the library sent since folded in. */
  const described = (o: InspectorObject | null | undefined): ArgObject => ({ ...(o?.args ?? {}), ...(o?.updates ?? {}) });
  const stateOf = (cmd: CaptureCommand): ArgObject | null => {
    const s = (cmd as CaptureCommand & { state?: ArgValue }).state;
    return isObject(s) ? s : null;
  };
  const list = (v: ArgValue | undefined): ArgObject[] => (Array.isArray(v) ? v.filter(isObject) : []);
  const ref = (v: ArgValue | undefined): DetailValue => {
    const id = refId(v);
    return id ? { object: id } : null;
  };
  const vkShort = (f: ArgValue | undefined): string => str(f).replace(/^VK_FORMAT_/, "");

  const summarize = (cmd: CaptureCommand, nameOf: (v: ArgValue | undefined) => string): string | undefined => {
    const a = cmd.args ?? {};
    const s = stateOf(cmd);
    const draw = s && isObject(s.draw) ? s.draw : null;
    if (DRAW.has(cmd.method)) {
      if (!draw) return undefined;
      const instances = num(draw.instances) > 1 ? ` x${num(draw.instances)}` : "";
      return `${num(draw.count)} ${draw.indexed ? "idx" : "verts"}${instances}${draw.indirect ? " (indirect)" : ""}`;
    }
    switch (cmd.method) {
      case "glDispatchCompute":
        return `${num(a.num_groups_x)}x${num(a.num_groups_y)}x${num(a.num_groups_z)} groups`;
      case "BeginRenderPass": {
        if (a.framebuffer) return nameOf(a.framebuffer);
        return `default framebuffer ${num(a.width)}x${num(a.height)}`;
      }
      case "glUseProgram":
        return a.program ? nameOf(a.program) : "none";
      case "glBindFramebuffer":
        return `${short(a.target)} ${a.framebuffer ? nameOf(a.framebuffer) : "default"}`;
      case "glBindTexture":
        return `${short(a.target)} ${a.texture ? nameOf(a.texture) : "none"}`;
      case "glBindBuffer":
      case "glBindBufferBase":
      case "glBindBufferRange":
        return `${short(a.target)}${a.index !== undefined ? `[${num(a.index)}]` : ""} ${a.buffer ? nameOf(a.buffer) : "none"}`;
      case "glBindVertexArray":
      case "glBindVertexArrayOES":
        return a.array ? nameOf(a.array) : "none";
      case "glBufferData":
      case "glBufferSubData":
        return `${short(a.target)} ${formatBytes(num(a.size))}`;
      case "glClear":
        return str(a.mask).replace(/GL_|_BUFFER_BIT/g, "").toLowerCase();
      case "glViewport":
      case "glScissor":
        return `${num(a.x)},${num(a.y)} ${num(a.width)}x${num(a.height)}`;
      case "glEnable":
      case "glDisable":
        return short(a.cap);
      case "glActiveTexture":
        return short(a.texture);
      case "glInvalidateFramebuffer":
      case "glDiscardFramebufferEXT":
        return (Array.isArray(a.attachments) ? a.attachments : []).map((x) => short(x).replace(/_ATTACHMENT/, "")).join(", ").toLowerCase();
      default:
        return undefined;
    }
  };

  const labelOf = (cmd: CaptureCommand): string | undefined => {
    const a = cmd.args ?? {};
    const text = a.message ?? a.marker;
    return typeof text === "string" && text ? text : undefined;
  };

  const drawArgsOf = (cmd: CaptureCommand) => {
    const s = stateOf(cmd);
    const d = s && isObject(s.draw) ? s.draw : null;
    if (!d) return null;
    // The library read the index data back from the draw's first index on: index 0 of the capture.
    return d.indexed
      ? { indexed: true, indexCount: num(d.count), firstIndex: 0, vertexOffset: num(d.baseVertex), instanceCount: num(d.instances) }
      : { indexed: false, vertexCount: num(d.count), firstVertex: num(d.first), instanceCount: num(d.instances) };
  };

  /** "Render Pass 1: offscreen", "Render Pass 2: default framebuffer": the framebuffer is what names a GL pass. */
  const passLabel = (cmd: CaptureCommand, passIndex: number, nameOf: (v: ArgValue | undefined) => string): string | undefined => {
    const a = cmd.args ?? {};
    const colors = list(a.attachments).filter((x) => /COLOR/.test(str(x.attachment))).length;
    const depth = list(a.attachments).some((x) => /DEPTH/.test(str(x.attachment))) ? " + depth" : "";
    if (a.framebuffer) return `Render Pass ${passIndex}: ${nameOf(a.framebuffer) || `${colors} color attachment${colors === 1 ? "" : "s"}`}${depth}`;
    return `Render Pass ${passIndex}: default framebuffer`;
  };

  const sets: CommandSets = {
    ...host.emptySets,
    DRAW, DISPATCH, PASS_BEGIN, PASS_END, LABEL_BEGIN, LABEL_END, SUBMIT, BIND_PIPELINE, INDIRECT,
    COMPUTE_PASS_END: NONE,
    bindPointOf: (method) => (DISPATCH.has(method) ? "compute" : "graphics"),
    pipelineBindPointOf: () => "graphics",
    graphicsBindPoint: "graphics",
    vertexBuffersOf: () => [],
    indexBufferOf: () => null,
    summarize,
    labelOf,
    drawArgsOf,
    passLabel,
  };

  /** The state at a draw, from the snapshot the library attached to it, in the shape the mesh view reads. */
  const drawState = (_data: unknown, db: ObjectLookup, cmd: CaptureCommand): DrawState | null => {
    const s = stateOf(cmd);
    if (!s) return null;
    const compute = DISPATCH.has(cmd.method);
    const vertexBuffers = new Map<number, BoundVertexBuffer>();
    const bindings: ArgObject[] = [];
    const attributes: ArgObject[] = [];
    for (const a of list(s.attributes)) {
      if (!a.enabled) continue;
      const location = num(a.location);
      const stride = num(a.stride);
      vertexBuffers.set(location, {
        cmd, binding: location, buffer: a.buffer ?? null, offset: num(a.offset), size: null, stride, dataId: num(a.data),
      });
      // Each GL attribute reads its own buffer range: a binding of its own, with the attribute at its start.
      bindings.push({ binding: location, stride, inputRate: num(a.divisor) ? "VK_VERTEX_INPUT_RATE_INSTANCE" : "VK_VERTEX_INPUT_RATE_VERTEX" });
      attributes.push({ location, binding: location, format: vertexFormat(str(a.componentType), num(a.size), !!a.normalized, !!a.integer), offset: 0 });
    }
    let indexBuffer: BoundIndexBuffer | null = null;
    if (s.indexType !== undefined) {
      indexBuffer = { cmd, buffer: s.elementBuffer ?? null, offset: num(s.indexOffset), indexType: INDEX_TYPES[str(s.indexType)] ?? str(s.indexType), dataId: num(s.indexData) };
    }
    const raster = isObject(s.raster) ? s.raster : {};
    return {
      bindPoint: compute ? "compute" : "graphics",
      pipelineCmd: null,
      pipeline: db.getObject(refId(s.program)),
      shaders: [],
      shadersCmd: null,
      dynamic: { cullMode: null, frontFace: null, topology: TOPOLOGY[str(s.mode)] ?? null, depthTest: null, depthCompare: null, patchControlPoints: null },
      sets: new Map(),
      vertexBuffers,
      stageBuffers: new Map(),
      rayBindings: new Map(),
      stageTextures: new Map(),
      stageSamplers: new Map(),
      indexBuffer,
      vertexInput: compute ? null : { vertexBindingDescriptionCount: bindings.length, pVertexBindingDescriptions: bindings, vertexAttributeDescriptionCount: attributes.length, pVertexAttributeDescriptions: attributes },
      viewports: Array.isArray(raster.viewport) ? [{ x: num(raster.viewport[0]), y: num(raster.viewport[1]), width: num(raster.viewport[2]), height: num(raster.viewport[3]), minDepth: 0, maxDepth: 1 }] : null,
      scissors: null,
      pushConstants: [],
      cullMode: null,
      frontFace: null,
      depthStencil: null,
    };
  };

  /** The attributes' names, as the program's reflection gave them, by location. */
  const vertexInputNames = (cmd: CaptureCommand): Map<number, string> | null => {
    const s = stateOf(cmd);
    if (!s) return null;
    return new Map(list(s.attributes).map((a) => [num(a.location), str(a.name)]));
  };

  /** A program's stages: the shaders it was linked from, with their sources as they were at the link. */
  const programSections = (program: InspectorObject | null): DetailSection[] => {
    if (!program) return [{ title: "Program", note: "No program is in use." }];
    const d = described(program);
    const out: DetailSection[] = [{
      title: "Program",
      rows: [
        ["Program", { object: program.id }],
        ["Linked", d.linked === undefined ? null : d.linked ? "yes" : "no"],
        ...(str(d.infoLog).trim() ? [["Info log", str(d.infoLog).trim()] as [string, DetailValue]] : []),
      ],
    }];
    for (const stage of list(d.stages)) {
      const kind = str(stage.type).replace(/^GL_/, "").replace(/_SHADER$/, "").toLowerCase();
      out.push({ title: `${kind.charAt(0).toUpperCase()}${kind.slice(1)} Shader`, collapsed: true, code: { text: str(stage.source), language: "glsl" } });
    }
    if (d.source) out.push({ title: "Shader", collapsed: true, code: { text: str(d.source), language: "glsl" } });
    return out;
  };

  /**
   * The range of vertices an indexed draw reads. The library knows it only for indices in client
   * memory; a buffer's indices are read after the frame, so the range is found here in them.
   */
  const verticesRead = (s: ArgObject, data: DetailContext["data"]): string => {
    if (Number(s.lastVertex) >= 0) return `${num(s.firstVertex)} to ${num(s.lastVertex)}`;
    const bytes = data.buffers.get(num(s.indexData))?.data;
    const size = INDEX_BYTES[str(s.indexType)];
    const d = isObject(s.draw) ? s.draw : null;
    if (!bytes || !size || !d) return "unknown";
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const count = Math.min(num(d.count), Math.floor(bytes.byteLength / size));
    let lo = Infinity, hi = -1;
    for (let i = 0; i < count; ++i) {
      const v = size === 1 ? view.getUint8(i) : size === 2 ? view.getUint16(i * 2, true) : view.getUint32(i * 4, true);
      // The primitive restart index (all ones) ends a strip rather than naming a vertex.
      if (v === 2 ** (size * 8) - 1) continue;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (hi < 0) return "none";
    const base = num(d.baseVertex);
    return `${lo + base} to ${hi + base}`;
  };

  const commandDetails = (cmd: CaptureCommand, ctx: DetailContext): DetailSection[] => {
    const a = cmd.args ?? {};
    if (cmd.method === "BeginRenderPass" || cmd.method === "EndRenderPass") {
      const rows: [string, DetailValue][] = [["Framebuffer", a.framebuffer ? ref(a.framebuffer) : "the default framebuffer (the surface)"]];
      if (a.surface) rows.push(["Surface", ref(a.surface)]);
      if (a.width !== undefined) rows.push(["Size", `${num(a.width)}x${num(a.height)}`]);
      const attachments = list(a.attachments);
      return [{
        title: "Render Pass",
        note: "OpenGL ES has no render passes: the capture library marks one wherever the draw framebuffer changes, its attachments change or the surface is swapped.",
        rows,
        ...(attachments.length ? {
          table: {
            columns: ["Attachment", "Object", "Format", "Size", "Level"],
            rows: attachments.map((x) => [short(x.attachment), ref(x.object), vkShort(x.format), x.width !== undefined ? `${num(x.width)}x${num(x.height)}` : "", num(x.level)]),
          },
        } : {}),
      }];
    }
    if (BIND_PIPELINE.has(cmd.method)) return programSections(ctx.db.getObject(refId(a.program)));
    const s = stateOf(cmd);
    if (!s) return [];
    const sections: DetailSection[] = [...programSections(ctx.db.getObject(refId(s.program)))];

    if (DRAW.has(cmd.method)) {
      const attrs = list(s.attributes);
      sections.push({
        title: "Vertex Input",
        ...(s.vertexArray !== undefined ? { rows: [["Vertex array", s.vertexArray ? ref(s.vertexArray) : "none (client-side arrays)"]] as [string, DetailValue][] } : {}),
        table: {
          columns: ["Location", "Name", "Buffer", "Format", "Offset", "Stride", "Divisor", "Contents"],
          rows: attrs.map((x) => x.enabled
            ? [num(x.location), str(x.name), x.buffer ? ref(x.buffer) : "client memory",
              vkShort(vertexFormat(str(x.componentType), num(x.size), !!x.normalized, !!x.integer)),
              num(x.offset), num(x.stride), num(x.divisor), num(x.data) ? { buffer: num(x.data) } : "not captured"]
            : [num(x.location), str(x.name), "disabled", str(x.type).replace(/^GL_/, ""), "", "", "",
              Array.isArray(x.value) ? `constant (${x.value.map((v) => num(v)).join(", ")})` : "constant"]),
        },
      });
      if (s.indexType !== undefined) {
        sections.push({
          title: "Index Buffer",
          rows: [
            ["Buffer", s.elementBuffer ? ref(s.elementBuffer) : "client memory"],
            ["Type", short(s.indexType)],
            ["Offset", num(s.indexOffset)],
            ["Contents", num(s.indexData) ? { buffer: num(s.indexData) } : "not captured"],
            ["Vertices read", verticesRead(s, ctx.data)],
          ],
        });
      }
    }

    const textures = list(s.textures);
    if (textures.length) {
      sections.push({
        title: "Textures",
        table: {
          columns: ["Unit", "Uniform", "Type", "Texture", "Sampler", "Contents"],
          rows: textures.map((t) => [num(t.unit), str(t.uniform), str(t.type), t.texture ? ref(t.texture) : "none",
            t.sampler ? ref(t.sampler) : "texture's own", num(t.capture) ? { texture: num(t.capture) } : "not captured"]),
        },
      });
    }

    const program = ctx.db.getObject(refId(s.program));
    const blockLayouts = new Map(list(described(program).uniformBlocks).map((b) => [str(b.name), b]));
    const blocks = list(s.uniformBlocks);
    if (blocks.length) {
      sections.push({
        title: "Uniform Blocks",
        table: {
          columns: ["Block", "Binding", "Buffer", "Offset", "Size", "Contents"],
          rows: blocks.map((b) => {
            const layout = blockLayouts.get(str(b.name));
            const members = list(layout?.members).map((m) => ({
              name: str(m.name), type: str(m.type), offset: num(m.offset),
              ...(m.count !== undefined ? { count: num(m.count) } : {}),
              ...(m.arrayStride !== undefined ? { arrayStride: num(m.arrayStride) } : {}),
              ...(m.matrixStride !== undefined ? { matrixStride: num(m.matrixStride) } : {}),
              ...(m.rowMajor ? { rowMajor: true } : {}),
            }));
            return [str(b.name), num(b.binding), b.buffer ? ref(b.buffer) : "none", num(b.offset),
              num(b.size) ? formatBytes(num(b.size)) : `whole (${num(b.dataSize)} bytes used)`,
              num(b.data) ? { buffer: num(b.data), members, blockName: str(b.name), blockSize: num(b.dataSize) } : "not captured"];
          }),
        },
      });
    }

    const uniforms = list(s.uniforms).filter((u) => !/sampler/.test(str(u.type)));
    if (uniforms.length) {
      sections.push({
        title: "Uniforms",
        table: {
          columns: ["Name", "Type", "Location", "Value"],
          rows: uniforms.map((u) => [str(u.name), str(u.type), num(u.location),
            (Array.isArray(u.value) ? u.value : []).map((v) => (typeof v === "number" ? Number(v.toFixed(4)) : String(v))).join(", ")]),
        },
      });
    }

    if (DRAW.has(cmd.method)) {
      const r = isObject(s.raster) ? s.raster : {};
      const d = isObject(s.depth) ? s.depth : {};
      const st = isObject(s.stencil) ? s.stencil : {};
      const b = isObject(s.blend) ? s.blend : {};
      const box = (v: ArgValue | undefined): string => (Array.isArray(v) ? v.map((x) => num(x)).join(", ") : "");
      sections.push({
        title: "Rasterizer",
        rows: [
          ["Primitive", short(s.mode)],
          ["Framebuffer", s.framebuffer ? ref(s.framebuffer) : "default"],
          ["Viewport", box(r.viewport)],
          ["Scissor", r.scissorTest ? box(r.scissor) : "off"],
          ["Cull", r.cullFace ? `${short(r.cullMode)} (front ${short(r.frontFace)})` : "off"],
          ["Polygon offset", r.polygonOffsetFill ? box(r.polygonOffset) : "off"],
          ...(r.rasterizerDiscard ? [["Rasterizer discard", "on"] as [string, DetailValue]] : []),
        ],
      });
      const stencilFace = (f: ArgValue | undefined): string => {
        if (!isObject(f)) return "";
        return `${short(f.func)} ref ${num(f.ref)} mask 0x${num(f.valueMask).toString(16)}, fail ${short(f.fail)}, depth fail ${short(f.depthFail)}, pass ${short(f.pass)}`;
      };
      sections.push({
        title: "Depth and Stencil",
        rows: [
          ["Depth test", d.test ? short(d.func) : "off"],
          ["Depth write", d.write ? "on" : "off"],
          ["Depth range", box(d.range)],
          ["Stencil", st.test ? `front: ${stencilFace(st.front)}; back: ${stencilFace(st.back)}` : "off"],
        ],
      });
      sections.push({
        title: "Blend",
        rows: [
          ["Blend", b.enabled ? `${short(b.srcRgb)} * src ${short(b.equationRgb)} ${short(b.dstRgb)} * dst (alpha: ${short(b.srcAlpha)}, ${short(b.equationAlpha)}, ${short(b.dstAlpha)})` : "off"],
          ["Color mask", Array.isArray(b.colorMask) ? b.colorMask.map((m, i) => (m ? "RGBA"[i] : "-")).join("") : ""],
          ["Blend color", box(b.color)],
        ],
      });
    }
    return sections;
  };

  const objectSummary = (o: InspectorObject): string | undefined => {
    const d = described(o);
    switch (o.type) {
      case "GLContext":
        return `OpenGL ES ${num(d.clientVersion)}.${num(d.minorVersion)}`;
      case "GLSurface":
        return `${str(d.kind)} ${num(d.width)}x${num(d.height)}`;
      case "GLBuffer":
        return d.size !== undefined ? `${formatBytes(num(d.size))} ${short(d.usage)}` : "";
      case "GLTexture":
      case "GLRenderbuffer": {
        if (d.width === undefined) return "no storage";
        const layers = num(d.depth) > 1 ? `x${num(d.depth)}` : "";
        const levels = num(d.levels) > 1 ? ` ${num(d.levels)} mips` : "";
        const samples = num(d.samples) > 1 ? ` ${num(d.samples)}x MSAA` : "";
        return `${vkShort(d.format) || short(d.internalFormat)} ${num(d.width)}x${num(d.height)}${layers}${levels}${samples}`;
      }
      case "GLShader":
        return `${short(d.type).replace(/_SHADER$/, "").toLowerCase()} shader${d.compiled === false ? " (failed to compile)" : ""}`;
      case "GLProgram": {
        if (d.linked === false) return "failed to link";
        const stages = list(d.stages).map((s) => short(s.type).replace(/_SHADER$/, "").toLowerCase());
        return stages.length ? stages.join(" + ") : d.separable ? `separable ${short(d.stage).replace(/_SHADER$/, "").toLowerCase()}` : "";
      }
      case "GLFramebuffer": {
        const attachments = list(d.attachments);
        return attachments.length ? attachments.map((a) => short(a.attachment).replace(/_ATTACHMENT/, "").toLowerCase()).join(" + ") : "incomplete";
      }
      default:
        return undefined;
    }
  };

  const objectBytes = (o: InspectorObject): number => {
    const d = described(o);
    if (o.type === "GLBuffer") return num(d.size);
    return 0;
  };

  /** What each pass writes and each draw reads, for the render graph. */
  const resourceSource = (db: ObjectLookup): ResourceSource => {
    const resourceOf = (id: number, level: number, layer: number, presented = false): RawResource | null => {
      const o = db.getObject(id);
      if (!o) return null;
      const d = described(o);
      const image = o.type === "GLTexture" || o.type === "GLRenderbuffer" || o.type === "GLSurface";
      return {
        key: image ? `image:${id}:m${level}:l${layer}` : `buffer:${id}`,
        objectId: id,
        type: image ? "image" : "buffer",
        label: o.name,
        detail: objectSummary(o) ?? "",
        bytes: image ? num(d.width) * num(d.height) * 4 : num(d.size),
        presented,
      };
    };
    const access = (list_: RawAccess[], id: number, mode: RawAccess["mode"], usage: string, level = 0, layer = 0, presented = false,
                    extra: Partial<RawAccess> = {}): void => {
      const r = id ? resourceOf(id, level, layer, presented) : null;
      if (r) list_.push({ resource: r, mode, usage, ...extra });
    };
    return {
      observe: () => {},
      passAccesses: (cmd) => {
        const a = cmd.args ?? {};
        const accesses: RawAccess[] = [];
        // The library says which buffers the pass cleared before drawing (so it needs nothing from
        // before) and which attachments it invalidated (so nothing it drew there survives).
        const cleared = new Set((Array.isArray(a.cleared) ? a.cleared : []).map((v) => str(v)));
        const invalidated = new Set((Array.isArray(a.invalidated) ? a.invalidated : []).map((v) => str(v)));
        const aspectOf = (attachment: string): string => (/DEPTH_STENCIL/.test(attachment) ? "depth" : /DEPTH/.test(attachment) ? "depth" : /STENCIL/.test(attachment) ? "stencil" : "color");
        if (a.framebuffer) {
          for (const x of list(a.attachments)) {
            const name = str(x.attachment);
            const aspect = aspectOf(name);
            const usage = aspect === "color" ? "color attachment" : "depth attachment";
            const dropped = invalidated.has(name) || (aspect !== "color" && invalidated.has("GL_DEPTH_STENCIL_ATTACHMENT"));
            access(accesses, refId(x.object) ?? 0, "write", usage, num(x.level), num(x.layer), false,
              { discards: cleared.has(aspect), dropped });
          }
        } else if (a.surface) {
          access(accesses, refId(a.surface) ?? 0, "write", "color attachment", 0, 0, true,
            { discards: cleared.has("color"), dropped: invalidated.has("GL_COLOR") });
        }
        const fb = a.framebuffer ? db.getObject(refId(a.framebuffer)) : null;
        return { kind: "render", label: fb ? fb.name : "default framebuffer", accesses };
      },
      actionAccesses: (cmd) => {
        const s = stateOf(cmd);
        const accesses: RawAccess[] = [];
        if (!s) return { accesses, unresolved: 0 };
        for (const t of list(s.textures)) access(accesses, refId(t.texture) ?? 0, "read", "sampled");
        for (const b of list(s.uniformBlocks)) access(accesses, refId(b.buffer) ?? 0, "read", "uniform");
        for (const x of list(s.attributes)) if (x.enabled) access(accesses, refId(x.buffer) ?? 0, "read", "vertex");
        if (s.elementBuffer) access(accesses, refId(s.elementBuffer) ?? 0, "read", "index");
        return { accesses, unresolved: 0 };
      },
      transferAccesses: () => null,
      computePassLabel: (ordinal) => `Compute ${ordinal}`,
    };
  };

  return {
    id: "gles",
    displayName: "OpenGL ES",
    objectTypePrefixes: ["GL"],
    sets,
    replay: { draws: false, shaders: false, hwCounters: false, overdraw: false, pixelHistory: false, drawOverlay: false, exportCpp: false, edits: false },
    live: { overdraw: false, pixelHistory: false, drawOverlay: false },
    submitCall: "eglSwapBuffers",
    advice: {
      discard: "glInvalidateFramebuffer on the attachment before the framebuffer is unbound",
      subpass: "On a tiled GPU the two passes cost a store and a load of the target; drawing both into one framebuffer, the second reading the first with framebuffer fetch (EXT_shader_framebuffer_fetch), is what saves them. ",
      transient: "a renderbuffer invalidated at the end of the pass (glInvalidateFramebuffer) and never read, or one pass drawing both with framebuffer fetch",
    },
    drawState,
    vertexInputNames,
    commandDetails,
    objectSummary,
    objectBytes,
    resourceSource,
  };
}
