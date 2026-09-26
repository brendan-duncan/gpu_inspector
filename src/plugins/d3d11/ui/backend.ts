// The Direct3D 11 plugin's backend: how the inspector reads a capture of the Direct3D 11 capture
// library (../src). Loaded by the app and the MCP server from plugin.json's `backend`
// (docs/PLUGINS.md), and written against the plugin SDK's types only (src/sdk/ts): what it runs
// with comes from the host.
//
// What the library records (../src/capture.h): every call the application made between two
// Presents, per device context; synthetic BeginRenderPass / EndRenderPass around what drew into
// one set of render targets; and on every draw and dispatch a `state` snapshot of what was bound,
// with the capture ids of the buffers and textures it read back. Everything here reads those.
import type {
  ArgObject, ArgValue, Backend, BoundIndexBuffer, BoundStageBuffer, BoundStageSampler, BoundStageTexture, BoundVertexBuffer,
  CaptureCommand, CommandSets, DetailContext, DetailSection, DetailValue, DrawState, InspectorObject, ObjectLookup, PluginHost,
  RawAccess, RawResource, ResourceSource,
} from "../../../sdk/ts/index.js";

const DRAW = new Set(["Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced", "DrawAuto", "DrawIndexedInstancedIndirect", "DrawInstancedIndirect"]);
const DISPATCH = new Set(["Dispatch", "DispatchIndirect"]);
// The library brackets what drew into one set of targets with these; Direct3D 11 has no pass of its own.
const PASS_BEGIN = new Set(["BeginRenderPass"]);
const PASS_END = new Set(["EndRenderPass"]);
const LABEL_BEGIN = new Set(["BeginEvent"]);
const LABEL_END = new Set(["EndEvent"]);
// A present ends a context's frame; the library numbers each context's passes per frame.
const SUBMIT = new Set(["Present", "Present1"]);
const BIND_PIPELINE = new Set(["VSSetShader", "PSSetShader", "GSSetShader", "HSSetShader", "DSSetShader", "CSSetShader"]);
const BIND_VERTEX = new Set(["IASetVertexBuffers"]);
const BIND_INDEX = new Set(["IASetIndexBuffer"]);
const INDIRECT = new Set(["DrawIndexedInstancedIndirect", "DrawInstancedIndirect", "DispatchIndirect"]);
// What closes a run of dispatches outside a render pass: the same list the library ends its compute passes on.
const COMPUTE_PASS_END = new Set([
  "OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews", "BeginRenderPass", "EndRenderPass", "BeginEvent", "EndEvent",
  "SetMarker", "ExecuteCommandList", "FinishCommandList", "Present", "Present1", "ClearState", "Flush", "Flush1",
]);
const NONE: ReadonlySet<string> = new Set();

const STAGES = ["vertex", "tess_control", "tess_eval", "geometry", "fragment", "compute"];
const STAGE_LABELS: Record<string, string> = {
  vertex: "Vertex", tess_control: "Hull", tess_eval: "Domain", geometry: "Geometry", fragment: "Pixel", compute: "Compute",
};

const TOPOLOGY: Record<string, string> = {
  D3D_PRIMITIVE_TOPOLOGY_POINTLIST: "VK_PRIMITIVE_TOPOLOGY_POINT_LIST",
  D3D_PRIMITIVE_TOPOLOGY_LINELIST: "VK_PRIMITIVE_TOPOLOGY_LINE_LIST",
  D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP",
  D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST",
  D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP",
  D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: "VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY",
  D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY",
  D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY",
  D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY",
};

const INDEX_TYPES: Record<string, string> = {
  DXGI_FORMAT_R16_UINT: "VK_INDEX_TYPE_UINT16",
  DXGI_FORMAT_R32_UINT: "VK_INDEX_TYPE_UINT32",
};

const CULL: Record<string, string> = {
  D3D11_CULL_NONE: "VK_CULL_MODE_NONE", D3D11_CULL_FRONT: "VK_CULL_MODE_FRONT_BIT", D3D11_CULL_BACK: "VK_CULL_MODE_BACK_BIT",
};

/** "D3D11_CULL_BACK" -> "CULL_BACK", "DXGI_FORMAT_R8G8B8A8_UNORM" -> "R8G8B8A8_UNORM": compact summaries. */
function short(v: ArgValue | undefined): string {
  return typeof v === "string" ? v.replace(/^(D3D11_|D3D_|DXGI_FORMAT_|VK_FORMAT_)/, "") : v === null || v === undefined ? "" : String(v);
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
  const desc = (o: InspectorObject | null | undefined): ArgObject => {
    const d = described(o);
    return isObject(d.pDesc) ? d.pDesc : {};
  };
  /** The stages a snapshot names, in pipeline order, with their bindings. */
  const stagesOf = (s: ArgObject): [string, ArgObject][] => {
    const st = isObject(s.stages) ? s.stages : {};
    return STAGES.filter((k) => isObject(st[k])).map((k) => [k, st[k] as ArgObject]);
  };

  const summarize = (cmd: CaptureCommand, nameOf: (v: ArgValue | undefined) => string): string | undefined => {
    const a = cmd.args ?? {};
    const s = stateOf(cmd);
    const draw = s && isObject(s.draw) ? s.draw : null;
    if (DRAW.has(cmd.method)) {
      if (!draw) return undefined;
      if (draw.indirect) return "indirect";
      if (draw.auto) return "stream output";
      const instances = num(draw.instances) > 1 ? ` x${num(draw.instances)}` : "";
      return `${num(draw.count)} ${draw.indexed ? "idx" : "verts"}${instances}`;
    }
    const first = (v: ArgValue | undefined): string => (Array.isArray(v) && v.length ? (v.length > 1 ? `${nameOf(v[0])} +${v.length - 1}` : nameOf(v[0])) : "none");
    switch (cmd.method) {
      case "Dispatch":
        return `${num(a.ThreadGroupCountX)}x${num(a.ThreadGroupCountY)}x${num(a.ThreadGroupCountZ)} groups`;
      case "BeginRenderPass": {
        const targets = list(a.attachments);
        const color = targets.filter((t) => t.aspect === "color");
        const depth = targets.some((t) => t.aspect === "depth");
        return `${color.map((t) => nameOf(t.resource)).join(", ") || "no color"}${depth ? " + depth" : ""}`;
      }
      case "OMSetRenderTargets":
      case "OMSetRenderTargetsAndUnorderedAccessViews":
        return `${first(a.ppRenderTargetViews)}${a.pDepthStencilView ? ` + ${nameOf(a.pDepthStencilView)}` : ""}`;
      case "VSSetShader": case "PSSetShader": case "GSSetShader": case "HSSetShader": case "DSSetShader": case "CSSetShader":
        return a.pShader ? nameOf(a.pShader) : "none";
      case "IASetVertexBuffers":
        return `slot ${num(a.StartSlot)}: ${first(a.ppVertexBuffers)}`;
      case "IASetIndexBuffer":
        return `${a.pIndexBuffer ? nameOf(a.pIndexBuffer) : "none"} ${short(a.Format)}`;
      case "IASetInputLayout":
        return a.pInputLayout ? nameOf(a.pInputLayout) : "none";
      case "IASetPrimitiveTopology":
        return short(a.Topology).replace(/^PRIMITIVE_TOPOLOGY_/, "").toLowerCase();
      case "VSSetConstantBuffers": case "PSSetConstantBuffers": case "GSSetConstantBuffers": case "HSSetConstantBuffers":
      case "DSSetConstantBuffers": case "CSSetConstantBuffers":
      case "VSSetConstantBuffers1": case "PSSetConstantBuffers1": case "GSSetConstantBuffers1": case "HSSetConstantBuffers1":
      case "DSSetConstantBuffers1": case "CSSetConstantBuffers1":
        return `b${num(a.StartSlot)}: ${first(a.ppConstantBuffers)}`;
      case "VSSetShaderResources": case "PSSetShaderResources": case "GSSetShaderResources": case "HSSetShaderResources":
      case "DSSetShaderResources": case "CSSetShaderResources":
        return `t${num(a.StartSlot)}: ${first(a.ppShaderResourceViews)}`;
      case "VSSetSamplers": case "PSSetSamplers": case "GSSetSamplers": case "HSSetSamplers": case "DSSetSamplers": case "CSSetSamplers":
        return `s${num(a.StartSlot)}: ${first(a.ppSamplers)}`;
      case "CSSetUnorderedAccessViews":
        return `u${num(a.StartSlot)}: ${first(a.ppUnorderedAccessViews)}`;
      case "ClearRenderTargetView":
        return nameOf(a.pRenderTargetView);
      case "ClearDepthStencilView":
        return `${nameOf(a.pDepthStencilView)} ${str(a.ClearFlags).replace(/D3D11_CLEAR_/g, "").toLowerCase()}`;
      case "Map":
        return `${nameOf(a.pResource)} ${short(a.MapType).replace(/^MAP_/, "").toLowerCase()}`;
      case "Unmap": case "UpdateSubresource": case "UpdateSubresource1": case "DiscardResource":
        return nameOf(a.pResource ?? a.pDstResource);
      case "CopyResource": case "CopySubresourceRegion": case "CopySubresourceRegion1": case "ResolveSubresource":
        return `${nameOf(a.pSrcResource)} -> ${nameOf(a.pDstResource)}`;
      case "RSSetViewports": {
        const v = list(a.pViewports)[0];
        return v ? `${num(v.TopLeftX)},${num(v.TopLeftY)} ${num(v.Width)}x${num(v.Height)}${list(a.pViewports).length > 1 ? ` +${list(a.pViewports).length - 1}` : ""}` : "none";
      }
      case "RSSetState": return a.pRasterizerState ? nameOf(a.pRasterizerState) : "default";
      case "OMSetBlendState": return a.pBlendState ? nameOf(a.pBlendState) : "default";
      case "OMSetDepthStencilState": return a.pDepthStencilState ? `${nameOf(a.pDepthStencilState)} ref ${num(a.StencilRef)}` : "default";
      case "ExecuteCommandList": return nameOf(a.pCommandList);
      case "Present": case "Present1": return `${nameOf(a.swapChain)} sync ${num(a.SyncInterval)}`;
      case "DiscardView": case "DiscardView1": return nameOf(a.pResourceView);
      case "BeginEvent": case "SetMarker": return str(a.Name);
      default:
        return undefined;
    }
  };

  const labelOf = (cmd: CaptureCommand): string | undefined => {
    const text = cmd.args?.Name;
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

  /** "Render Pass 1: Scene color + depth", "Render Pass 2: back buffer 0": the targets are what name a D3D11 pass. */
  const passLabel = (cmd: CaptureCommand, passIndex: number, nameOf: (v: ArgValue | undefined) => string): string | undefined => {
    const targets = list(cmd.args?.attachments);
    const color = targets.filter((t) => t.aspect === "color");
    const depth = targets.some((t) => t.aspect === "depth") ? " + depth" : "";
    const name = color.length ? color.map((t) => nameOf(t.resource)).join(", ") : "no color target";
    return `Render Pass ${passIndex}: ${name}${depth}`;
  };

  const vertexBuffersOf = (cmd: CaptureCommand): BoundVertexBuffer[] => {
    if (cmd.method !== "IASetVertexBuffers") return [];
    const a = cmd.args ?? {};
    const buffers = Array.isArray(a.ppVertexBuffers) ? a.ppVertexBuffers : [];
    const strides = Array.isArray(a.pStrides) ? a.pStrides : [];
    const offsets = Array.isArray(a.pOffsets) ? a.pOffsets : [];
    return buffers.map((b, i) => ({ cmd, binding: num(a.StartSlot) + i, buffer: b, offset: num(offsets[i]), size: null, stride: num(strides[i]), dataId: 0 }));
  };

  const indexBufferOf = (cmd: CaptureCommand): BoundIndexBuffer | null => {
    if (cmd.method !== "IASetIndexBuffer") return null;
    const a = cmd.args ?? {};
    return { cmd, buffer: a.pIndexBuffer ?? null, offset: num(a.Offset), indexType: INDEX_TYPES[str(a.Format)] ?? str(a.Format), dataId: 0 };
  };

  const sets: CommandSets = {
    ...host.emptySets,
    DRAW, DISPATCH, PASS_BEGIN, PASS_END, LABEL_BEGIN, LABEL_END, SUBMIT, BIND_PIPELINE, BIND_VERTEX, BIND_INDEX, INDIRECT,
    COMPUTE_PASS_END,
    RECORD_BEGIN: NONE,
    RECORD_END: NONE,
    bindPointOf: (method) => (DISPATCH.has(method) || method === "CSSetShader" ? "compute" : "graphics"),
    pipelineBindPointOf: (method) => (method === "CSSetShader" ? "compute" : "graphics"),
    graphicsBindPoint: "graphics",
    vertexBuffersOf,
    indexBufferOf,
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
    if (!compute) {
      const elements = list(s.attributes);
      const strides = new Map<number, number>();
      for (const vb of list(s.vertexBuffers)) {
        const slot = num(vb.slot);
        strides.set(slot, num(vb.stride));
        vertexBuffers.set(slot, { cmd, binding: slot, buffer: vb.buffer ?? null, offset: num(vb.offset), size: null, stride: num(vb.stride), dataId: num(vb.data) });
      }
      // A binding per slot the layout reads, in vkCmdSetVertexInputEXT's shape; the stride is the bound buffer's.
      for (const slot of new Set(elements.map((e) => num(e.slot)))) {
        const perInstance = elements.some((e) => num(e.slot) === slot && e.perInstance);
        bindings.push({ binding: slot, stride: strides.get(slot) ?? 0, inputRate: perInstance ? "VK_VERTEX_INPUT_RATE_INSTANCE" : "VK_VERTEX_INPUT_RATE_VERTEX" });
      }
      for (const e of elements) attributes.push({ location: num(e.location), binding: num(e.slot), format: str(e.format), offset: num(e.offset) });
    }
    let indexBuffer: BoundIndexBuffer | null = null;
    const ib = isObject(s.indexBuffer) ? s.indexBuffer : null;
    if (ib) indexBuffer = { cmd, buffer: ib.buffer ?? null, offset: num(ib.offset), indexType: INDEX_TYPES[str(ib.format)] ?? str(ib.format), dataId: num(ib.data) };
    const shaders: InspectorObject[] = [];
    const stageBuffers = new Map<string, BoundStageBuffer>();
    const stageTextures = new Map<string, BoundStageTexture>();
    const stageSamplers = new Map<string, BoundStageSampler>();
    for (const [stage, b] of stagesOf(s)) {
      const shader = db.getObject(refId(b.shader));
      if (shader) shaders.push(shader);
      for (const cb of list(b.constantBuffers)) stageBuffers.set(`${stage}:${num(cb.slot)}`, { cmd, stage, index: num(cb.slot), buffer: cb.buffer ?? null, offset: num(cb.offset), dataId: num(cb.data), inline: false });
      for (const r of list(b.resources)) {
        if (r.capture !== undefined) stageTextures.set(`${stage}:${num(r.slot)}`, { cmd, stage, index: num(r.slot), texture: r.resource ?? null, dataId: num(r.capture) });
      }
      for (const sm of list(b.samplers)) stageSamplers.set(`${stage}:${num(sm.slot)}`, { cmd, stage, index: num(sm.slot), sampler: sm.sampler ?? null });
    }
    const raster = db.getObject(refId(s.rasterizerState));
    const rd = desc(raster);
    const viewports = list(s.viewports).map((v) => ({ x: num(v.TopLeftX), y: num(v.TopLeftY), width: num(v.Width), height: num(v.Height), minDepth: num(v.MinDepth), maxDepth: num(v.MaxDepth) }));
    const scissors = list(s.scissors).map((r) => ({ offset: { x: num(r.left), y: num(r.top) }, extent: { width: num(r.right) - num(r.left), height: num(r.bottom) - num(r.top) } }));
    const cullMode = raster ? CULL[str(rd.CullMode)] ?? null : "VK_CULL_MODE_BACK_BIT";
    const frontFace = raster ? (rd.FrontCounterClockwise ? "VK_FRONT_FACE_COUNTER_CLOCKWISE" : "VK_FRONT_FACE_CLOCKWISE") : "VK_FRONT_FACE_CLOCKWISE";
    const dss = isObject(s.depthStencilState) ? db.getObject(refId(s.depthStencilState.state)) : null;
    return {
      bindPoint: compute ? "compute" : "graphics",
      pipelineCmd: null,
      pipeline: null,
      shaders,
      shadersCmd: null,
      dynamic: { cullMode, frontFace, topology: TOPOLOGY[str(s.topology)] ?? null, depthTest: null, depthCompare: null, patchControlPoints: null },
      sets: new Map(),
      vertexBuffers,
      stageBuffers,
      rayBindings: new Map(),
      stageTextures,
      stageSamplers,
      indexBuffer,
      vertexInput: compute ? null : { vertexBindingDescriptionCount: bindings.length, pVertexBindingDescriptions: bindings, vertexAttributeDescriptionCount: attributes.length, pVertexAttributeDescriptions: attributes },
      viewports: viewports.length ? viewports : null,
      scissors: rd.ScissorEnable && scissors.length ? scissors : null,
      pushConstants: [],
      cullMode,
      frontFace,
      depthStencil: dss,
    };
  };

  /** The input layout's semantic names by location, for the mesh view's columns. */
  const vertexInputNames = (cmd: CaptureCommand): Map<number, string> | null => {
    const s = stateOf(cmd);
    if (!s) return null;
    return new Map(list(s.attributes).map((e) => [num(e.location), str(e.name)]));
  };

  /** A shader's row: the object, its target and its bytecode size. */
  const shaderRows = (db: ObjectLookup, s: ArgObject): [string, DetailValue][] => {
    const rows: [string, DetailValue][] = [];
    for (const [stage, b] of stagesOf(s)) {
      const shader = db.getObject(refId(b.shader));
      const d = described(shader);
      rows.push([`${STAGE_LABELS[stage] ?? stage} shader`, shader ? { object: shader.id } : "none"]);
      if (shader && d.target) rows.push([`${STAGE_LABELS[stage] ?? stage} target`, `${str(d.target)}, ${formatBytes(num(d.BytecodeLength))}`]);
    }
    return rows;
  };

  const commandDetails = (cmd: CaptureCommand, ctx: DetailContext): DetailSection[] => {
    const a = cmd.args ?? {};
    if (cmd.method === "BeginRenderPass" || cmd.method === "EndRenderPass") {
      const attachments = list(a.attachments);
      const cleared = (Array.isArray(a.cleared) ? a.cleared : []).map((v) => String(v));
      const discarded = (Array.isArray(a.discarded) ? a.discarded : []).map((v) => String(v));
      const what = (t: ArgObject): string => {
        const key = t.aspect === "depth" ? "depth" : String(num(t.attachment));
        const parts: string[] = [];
        if (cleared.includes(key) || (t.aspect === "depth" && cleared.includes("stencil"))) parts.push("cleared");
        if (discarded.includes(key)) parts.push("discarded");
        return parts.join(", ");
      };
      return [{
        title: "Render Pass",
        note: "Direct3D 11 has no render passes: the capture library marks one wherever the render targets change, the state is cleared, a command list is executed or the swap chain is presented.",
        ...(attachments.length ? {
          table: {
            columns: ["Attachment", "View", "Resource", "Format", "Size", "Mip", "Load / store"],
            rows: attachments.map((t) => [t.aspect === "depth" ? "depth" : num(t.attachment), ref(t.view), ref(t.resource), short(t.dxgiFormat ?? t.format),
              t.width !== undefined ? `${num(t.width)}x${num(t.height)}${num(t.samples) > 1 ? ` ${num(t.samples)}x` : ""}` : "", num(t.mip), what(t)]),
          },
        } : { rows: [["Targets", "none"]] as [string, DetailValue][] }),
      }];
    }
    if (BIND_PIPELINE.has(cmd.method)) {
      const shader = ctx.db.getObject(refId(a.pShader));
      const d = described(shader);
      return [{ title: "Shader", rows: [["Shader", shader ? { object: shader.id } : "none"], ["Target", str(d.target)], ["Bytecode", formatBytes(num(d.BytecodeLength))]] }];
    }
    const s = stateOf(cmd);
    if (!s) return [];
    const sections: DetailSection[] = [{ title: "Shaders", rows: shaderRows(ctx.db, s) }];

    if (DRAW.has(cmd.method)) {
      const layout = ctx.db.getObject(refId(s.inputLayout));
      const elements = list(s.attributes);
      const buffers = new Map(list(s.vertexBuffers).map((vb) => [num(vb.slot), vb]));
      sections.push({
        title: "Vertex Input",
        rows: [["Input layout", layout ? { object: layout.id } : "none"], ["Topology", short(s.topology).replace(/^PRIMITIVE_TOPOLOGY_/, "")]],
        table: {
          columns: ["Location", "Semantic", "Slot", "Buffer", "Format", "Offset", "Stride", "Rate", "Contents"],
          rows: elements.map((e) => {
            const vb = buffers.get(num(e.slot));
            return [num(e.location), str(e.name), num(e.slot), vb?.buffer ? ref(vb.buffer) : "none", short(e.format), num(e.offset), vb ? num(vb.stride) : "",
              e.perInstance ? `instance / ${num(e.stepRate)}` : "vertex", vb && num(vb.data) ? { buffer: num(vb.data) } : "not captured"];
          }),
        },
      });
      const ib = isObject(s.indexBuffer) ? s.indexBuffer : null;
      if (ib) {
        sections.push({
          title: "Index Buffer",
          rows: [["Buffer", ref(ib.buffer)], ["Format", short(ib.format)], ["Offset", num(ib.offset)], ["Contents", num(ib.data) ? { buffer: num(ib.data) } : "not captured"]],
        });
      }
    }

    for (const [stage, b] of stagesOf(s)) {
      const label = STAGE_LABELS[stage] ?? stage;
      const shader = ctx.db.getObject(refId(b.shader));
      const reflection = isObject(described(shader).reflection) ? (described(shader).reflection as ArgObject)[stage] : null;
      const resources = list(isObject(reflection) ? reflection.resources : undefined);
      const nameOf = (kind: string, register: number): string => str(resources.find((r) => str(r.kind) === kind && num(r.register) <= register && register < num(r.register) + Math.max(1, num(r.count)))?.name);
      const layoutOf = (register: number): { members: { name: string; type: string; offset: number }[]; blockName: string; blockSize: number } | null => {
        const r = resources.find((x) => str(x.kind) === "cbuffer" && num(x.register) === register);
        const t = r && isObject(r.type) ? r.type : null;
        if (!t) return null;
        return {
          blockName: str(r!.name), blockSize: num(t.size),
          members: list(t.members).map((m) => ({ name: str(m.name), type: isObject(m.type) ? typeName(m.type) : "", offset: num(m.offset) })),
        };
      };
      const cbs = list(b.constantBuffers);
      if (cbs.length) {
        sections.push({
          title: `${label} Constant Buffers`,
          table: {
            columns: ["Slot", "Name", "Buffer", "Offset", "Size", "Contents"],
            rows: cbs.map((cb) => {
              const layout = layoutOf(num(cb.slot));
              return [`b${num(cb.slot)}`, nameOf("cbuffer", num(cb.slot)), ref(cb.buffer), num(cb.offset), formatBytes(num(cb.size)),
                num(cb.data) ? { buffer: num(cb.data), ...(layout ?? {}) } : "not captured"];
            }),
          },
        });
      }
      const srvs = list(b.resources);
      if (srvs.length) {
        sections.push({
          title: `${label} Shader Resources`,
          table: {
            columns: ["Slot", "Name", "View", "Resource", "Contents"],
            rows: srvs.map((r) => [`t${num(r.slot)}`, nameOf("srv", num(r.slot)), ref(r.view), ref(r.resource),
              r.capture !== undefined ? (num(r.capture) ? { texture: num(r.capture) } : "not captured") : num(r.data) ? { buffer: num(r.data) } : "not captured"]),
          },
        });
      }
      const samplers = list(b.samplers);
      if (samplers.length) {
        sections.push({
          title: `${label} Samplers`,
          table: { columns: ["Slot", "Name", "Sampler"], rows: samplers.map((sm) => [`s${num(sm.slot)}`, nameOf("sampler", num(sm.slot)), ref(sm.sampler)]) },
        });
      }
    }
    const uavs = list(s.uavs);
    if (uavs.length) {
      sections.push({
        title: "Unordered Access Views",
        table: {
          columns: ["Slot", "View", "Resource", "Contents"],
          rows: uavs.map((u) => [`u${num(u.slot)}`, ref(u.view), ref(u.resource),
            u.capture !== undefined ? (num(u.capture) ? { texture: num(u.capture) } : "not captured") : num(u.data) ? { buffer: num(u.data) } : "not captured"]),
        },
      });
    }
    if (isObject(s.indirectArgs)) {
      sections.push({ title: "Indirect Arguments", rows: [["Buffer", ref(s.indirectArgs.buffer)], ["Offset", num(s.indirectArgs.offset)], ["Contents", num(s.indirectArgs.data) ? { buffer: num(s.indirectArgs.data) } : "not captured"]] });
    }

    if (DRAW.has(cmd.method)) {
      const raster = ctx.db.getObject(refId(s.rasterizerState));
      const rd = desc(raster);
      const blend = isObject(s.blend) ? s.blend : {};
      const blendState = ctx.db.getObject(refId(blend.state));
      const bd = desc(blendState);
      const dsState = isObject(s.depthStencilState) ? s.depthStencilState : {};
      const dss = ctx.db.getObject(refId(dsState.state));
      const dd = desc(dss);
      const viewport = (v: ArgObject): string => `${num(v.TopLeftX)},${num(v.TopLeftY)} ${num(v.Width)}x${num(v.Height)} depth ${num(v.MinDepth)}..${num(v.MaxDepth)}`;
      const rect = (r: ArgObject): string => `${num(r.left)},${num(r.top)} ${num(r.right) - num(r.left)}x${num(r.bottom) - num(r.top)}`;
      sections.push({
        title: "Rasterizer",
        rows: [
          ["State", raster ? { object: raster.id } : "default"],
          ["Fill", raster ? short(rd.FillMode) : "SOLID"],
          ["Cull", raster ? `${short(rd.CullMode)} (front ${rd.FrontCounterClockwise ? "counter-clockwise" : "clockwise"})` : "CULL_BACK (front clockwise)"],
          ["Viewports", list(s.viewports).map(viewport).join("; ") || "none"],
          ["Scissor", rd.ScissorEnable ? list(s.scissors).map(rect).join("; ") || "none set" : "off"],
          ["Depth bias", raster ? `${num(rd.DepthBias)} (slope ${num(rd.SlopeScaledDepthBias)}, clamp ${num(rd.DepthBiasClamp)})` : "0"],
        ],
      });
      const face = (f: ArgValue | undefined): string => (isObject(f) ? `${short(f.StencilFunc)}, fail ${short(f.StencilFailOp)}, depth fail ${short(f.StencilDepthFailOp)}, pass ${short(f.StencilPassOp)}` : "");
      sections.push({
        title: "Depth and Stencil",
        rows: [
          ["State", dss ? { object: dss.id } : "default"],
          ["Depth test", dss ? (dd.DepthEnable ? short(dd.DepthFunc) : "off") : "LESS"],
          ["Depth write", dss ? (str(dd.DepthWriteMask).endsWith("ALL") ? "on" : "off") : "on"],
          ["Stencil", dss && dd.StencilEnable ? `ref ${num(dsState.stencilRef)}, read 0x${num(dd.StencilReadMask).toString(16)}, write 0x${num(dd.StencilWriteMask).toString(16)}; front: ${face(dd.FrontFace)}; back: ${face(dd.BackFace)}` : "off"],
        ],
      });
      const targets = list(bd.RenderTarget);
      const t0 = targets[0] ?? {};
      sections.push({
        title: "Blend",
        rows: [
          ["State", blendState ? { object: blendState.id } : "default"],
          ["Blend", blendState ? (t0.BlendEnable ? `${short(t0.SrcBlend)} * src ${short(t0.BlendOp)} ${short(t0.DestBlend)} * dst (alpha: ${short(t0.SrcBlendAlpha)}, ${short(t0.BlendOpAlpha)}, ${short(t0.DestBlendAlpha)})` : "off") : "off"],
          ["Write mask", blendState ? str(t0.RenderTargetWriteMask).replace(/D3D11_COLOR_WRITE_ENABLE_/g, "") : "ALL"],
          ["Blend factor", Array.isArray(blend.factor) ? blend.factor.map((v) => num(v)).join(", ") : ""],
          ["Sample mask", `0x${num(blend.sampleMask).toString(16)}`],
          ...(bd.AlphaToCoverageEnable ? [["Alpha to coverage", "on"] as [string, DetailValue]] : []),
        ],
      });
      const rts = list(s.renderTargets);
      const ds = isObject(s.depthStencil) ? s.depthStencil : null;
      sections.push({
        title: "Render Targets",
        table: {
          columns: ["Slot", "View", "Resource"],
          rows: [...rts.map((r) => [num(r.slot), ref(r.view), ref(r.resource)] as DetailValue[]), ...(ds ? [["depth", ref(ds.view), ref(ds.resource)] as DetailValue[]] : [])],
        },
      });
    }
    return sections;
  };

  /** A reflection type's name, in HLSL terms, for a constant buffer's member rows. */
  const typeName = (t: ArgObject): string => {
    switch (str(t.kind)) {
      case "scalar": return str(t.base);
      case "vector": return `${isObject(t.element) ? typeName(t.element) : ""}${num(t.count)}`;
      case "matrix": return `${isObject(t.element) ? typeName(t.element) : ""}${num(t.rows)}x${num(t.columns)}`;
      case "array": return `${isObject(t.element) ? typeName(t.element) : ""}[${num(t.count)}]`;
      case "struct": return str(t.name);
      default: return str(t.name);
    }
  };

  const objectSummary = (o: InspectorObject): string | undefined => {
    const d = described(o);
    const p = isObject(d.pDesc) ? d.pDesc : {};
    switch (o.type) {
      case "ID3D11Device":
        return `${short(d.featureLevel).replace(/^FEATURE_LEVEL_/, "feature level ").replace("_", ".")}${isObject(d.adapter) ? `, ${str(d.adapter.Description)}` : ""}`;
      case "ID3D11DeviceContext":
        return str(d.type);
      case "IDXGISwapChain": {
        const bd = isObject(p.BufferDesc) ? p.BufferDesc : {};
        return `${short(bd.Format)} ${num(bd.Width)}x${num(bd.Height)}, ${num(p.BufferCount)} buffers`;
      }
      case "ID3D11Buffer":
        return `${formatBytes(num(p.ByteWidth))} ${str(p.BindFlags).replace(/D3D11_BIND_/g, "").toLowerCase()} ${short(p.Usage).replace(/^USAGE_/, "").toLowerCase()}${num(p.StructureByteStride) ? `, stride ${num(p.StructureByteStride)}` : ""}`;
      case "ID3D11Texture1D":
      case "ID3D11Texture2D":
      case "ID3D11Texture3D": {
        const size = o.type === "ID3D11Texture1D" ? `${num(p.Width)}` : o.type === "ID3D11Texture3D" ? `${num(p.Width)}x${num(p.Height)}x${num(p.Depth)}` : `${num(p.Width)}x${num(p.Height)}`;
        const layers = num(p.ArraySize) > 1 ? `x${num(p.ArraySize)}` : "";
        const mips = num(p.MipLevels) !== 1 ? ` ${num(p.MipLevels) || "all"} mips` : "";
        const samples = isObject(p.SampleDesc) && num(p.SampleDesc.Count) > 1 ? ` ${num(p.SampleDesc.Count)}x MSAA` : "";
        return `${short(p.Format)} ${size}${layers}${mips}${samples}${d.swapChain ? " (back buffer)" : ""}`;
      }
      case "ID3D11ShaderResourceView":
      case "ID3D11RenderTargetView":
      case "ID3D11DepthStencilView":
      case "ID3D11UnorderedAccessView":
        return `${short(p.Format)} ${short(p.ViewDimension).replace(/^(SRV|RTV|DSV|UAV)_DIMENSION_/, "").toLowerCase()}`;
      case "ID3D11VertexShader": case "ID3D11PixelShader": case "ID3D11GeometryShader":
      case "ID3D11HullShader": case "ID3D11DomainShader": case "ID3D11ComputeShader":
        return `${str(d.target) || str(d.stage)}, ${formatBytes(num(d.BytecodeLength))}`;
      case "ID3D11InputLayout":
        return list(d.elements).map((e) => str(e.name)).join(", ");
      case "ID3D11SamplerState":
        return `${short(p.Filter).replace(/^FILTER_/, "").toLowerCase()} ${short(p.AddressU).replace(/^TEXTURE_ADDRESS_/, "").toLowerCase()}`;
      case "ID3D11RasterizerState":
        return `${short(p.FillMode).replace(/^FILL_/, "").toLowerCase()}, ${short(p.CullMode).replace(/^CULL_/, "cull ").toLowerCase()}${p.ScissorEnable ? ", scissor" : ""}`;
      case "ID3D11BlendState": {
        const t0 = list(p.RenderTarget)[0];
        return t0 && t0.BlendEnable ? `${short(t0.SrcBlend)} ${short(t0.BlendOp)} ${short(t0.DestBlend)}`.toLowerCase() : "no blending";
      }
      case "ID3D11DepthStencilState":
        return `depth ${p.DepthEnable ? short(p.DepthFunc).toLowerCase() : "off"}${p.StencilEnable ? ", stencil" : ""}`;
      case "ID3D11Query": case "ID3D11Predicate":
        return short(p.Query).replace(/^QUERY_/, "").toLowerCase();
      case "ID3D11CommandList":
        return "command list";
      default:
        return undefined;
    }
  };

  const objectBytes = (o: InspectorObject): number => {
    const d = described(o);
    const p = isObject(d.pDesc) ? d.pDesc : {};
    if (o.type === "ID3D11Buffer") return num(p.ByteWidth);
    if (o.type === "ID3D11Texture2D" || o.type === "ID3D11Texture3D" || o.type === "ID3D11Texture1D") {
      // An estimate from the texel size the format name carries (block formats count 1 byte per texel, near enough).
      const bits = [...str(p.Format).replace(/^DXGI_FORMAT_/, "").matchAll(/[RGBAXD](\d+)/g)].reduce((sum, m) => sum + Number(m[1]), 0);
      const texel = /BC[1-7]/.test(str(p.Format)) ? 1 : Math.max(1, Math.ceil(bits / 8));
      const samples = isObject(p.SampleDesc) ? Math.max(1, num(p.SampleDesc.Count)) : 1;
      const mips = num(p.MipLevels) || 1;
      let bytes = 0;
      for (let m = 0; m < mips; ++m) bytes += Math.max(1, num(p.Width) >> m) * Math.max(1, (num(p.Height) || 1) >> m) * Math.max(1, (num(p.Depth) || 1) >> m);
      return bytes * texel * samples * Math.max(1, num(p.ArraySize) || 1);
    }
    return 0;
  };

  /** What each pass writes and each draw reads, for the render graph. */
  const resourceSource = (db: ObjectLookup): ResourceSource => {
    const resourceOf = (id: number, level: number, layer: number, presented = false): RawResource | null => {
      const o = db.getObject(id);
      if (!o) return null;
      const image = o.type.startsWith("ID3D11Texture");
      return {
        key: image ? `image:${id}:m${level}:l${layer}` : `buffer:${id}`,
        objectId: id,
        type: image ? "image" : "buffer",
        label: o.name,
        detail: objectSummary(o) ?? "",
        bytes: objectBytes(o),
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
        const cleared = (Array.isArray(a.cleared) ? a.cleared : []).map((v) => String(v));
        const discarded = (Array.isArray(a.discarded) ? a.discarded : []).map((v) => String(v));
        for (const t of list(a.attachments)) {
          const depth = t.aspect === "depth";
          const key = depth ? "depth" : String(num(t.attachment));
          const resource = db.getObject(refId(t.resource));
          const presented = !!described(resource).swapChain;
          access(accesses, refId(t.resource) ?? 0, "write", depth ? "depth attachment" : "color attachment", num(t.mip), num(t.firstSlice), presented,
            { discards: cleared.includes(key), dropped: discarded.includes(key) });
        }
        const first = list(a.attachments).find((t) => t.aspect === "color") ?? list(a.attachments)[0];
        const target = first ? db.getObject(refId(first.resource)) : null;
        return { kind: "render", label: target ? target.name : "no targets", accesses };
      },
      actionAccesses: (cmd) => {
        const s = stateOf(cmd);
        const accesses: RawAccess[] = [];
        if (!s) return { accesses, unresolved: 0 };
        for (const [, b] of stagesOf(s)) {
          for (const r of list(b.resources)) access(accesses, refId(r.resource) ?? 0, "read", r.capture !== undefined ? "sampled" : "storage");
          for (const cb of list(b.constantBuffers)) access(accesses, refId(cb.buffer) ?? 0, "read", "uniform");
        }
        for (const u of list(s.uavs)) access(accesses, refId(u.resource) ?? 0, "write", "storage");
        for (const vb of list(s.vertexBuffers)) access(accesses, refId(vb.buffer) ?? 0, "read", "vertex");
        if (isObject(s.indexBuffer)) access(accesses, refId(s.indexBuffer.buffer) ?? 0, "read", "index");
        return { accesses, unresolved: 0 };
      },
      transferAccesses: (cmd) => {
        const a = cmd.args ?? {};
        const accesses: RawAccess[] = [];
        switch (cmd.method) {
          case "CopyResource": case "CopySubresourceRegion": case "CopySubresourceRegion1": case "ResolveSubresource":
            access(accesses, refId(a.pSrcResource) ?? 0, "read", "copy src");
            access(accesses, refId(a.pDstResource) ?? 0, "write", "copy dst", 0, 0, false, { discards: cmd.method === "CopyResource" });
            return { label: cmd.method, accesses };
          case "UpdateSubresource": case "UpdateSubresource1":
            access(accesses, refId(a.pDstResource) ?? 0, "write", "upload", 0, 0, false, { discards: !a.pDstBox });
            return { label: cmd.method, accesses };
          // A render target or depth clear is the pass's own (BeginRenderPass says what was cleared);
          // a UAV clear is a write of its own.
          case "ClearUnorderedAccessViewUint": case "ClearUnorderedAccessViewFloat": {
            const view = db.getObject(refId(a.pUnorderedAccessView));
            access(accesses, refId(described(view).pResource) ?? 0, "write", "clear", 0, 0, false, { discards: true });
            return { label: cmd.method, accesses };
          }
          default:
            return null;
        }
      },
      computePassLabel: (ordinal) => `Compute ${ordinal}`,
    };
  };

  return {
    id: "d3d11",
    displayName: "Direct3D 11",
    objectTypePrefixes: ["ID3D11", "IDXGISwapChain"],
    sets,
    replay: { draws: false, shaders: false, hwCounters: false, overdraw: false, pixelHistory: false, drawOverlay: false, exportCpp: false, edits: false },
    live: { overdraw: false, pixelHistory: false, drawOverlay: false },
    submitCall: "Present",
    advice: {
      discard: "DiscardView (ID3D11DeviceContext1) on the view once the pass is done with it",
      subpass: "Direct3D 11 has no subpasses: drawing both into one set of render targets, the second reading the first through a shader resource view, is what saves the round trip. ",
      transient: "a target discarded (DiscardView) as soon as its pass ends and never read",
    },
    drawState,
    vertexInputNames,
    commandDetails,
    objectSummary,
    objectBytes,
    resourceSource,
  };
}
