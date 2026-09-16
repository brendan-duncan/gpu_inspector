// Direct3D 12's command classification. The Vulkan counterpart is ../vulkan/command_sets.ts,
// the Metal one ../metal/command_sets.ts, and the interface all three fill in is ../command_sets.ts.
//
// The names are the ID3D12GraphicsCommandList methods as the capture library records them
// (src/d3d12/README.md, "Frame capture"), with their parameters under their D3D12 names. D3D12 has no
// render pass unless the application uses BeginRenderPass, so the library treats every
// OMSetRenderTargets as the start of one and records a synthetic `EndRenderTargets` where the
// application made no call that would close it: PASS_BEGIN / PASS_END list both spellings.
import type { BoundIndexBuffer, BoundVertexBuffer, CommandSets } from "../command_sets.js";
import type { ArgObject, ArgValue, CaptureCommand } from "../../shared/protocol.js";
import { fmt, isObject, num, str } from "../vulkan/vulkan_object.js";

const DRAW = new Set(["DrawInstanced", "DrawIndexedInstanced", "DispatchMesh", "ExecuteIndirect"]);
const DISPATCH = new Set(["Dispatch", "DispatchGraph"]);
const TRACE = new Set(["DispatchRays"]);
const PASS_BEGIN = new Set(["OMSetRenderTargets", "BeginRenderPass"]);
const PASS_END = new Set(["EndRenderTargets", "EndRenderPass"]);
// The library restarts a list's pass numbering at Reset, as the Vulkan layer does at
// vkBeginCommandBuffer (src/d3d12/src/capture.cpp, CaptureManager::OnListReset).
const RECORD_BEGIN = new Set(["Reset"]);
const RECORD_END = new Set(["Close"]);
const SUBMIT = new Set(["ExecuteCommandLists", "Present", "Present1", "Signal", "Wait"]);
const BIND_DESCRIPTOR = new Set([
  "SetGraphicsRootDescriptorTable", "SetComputeRootDescriptorTable",
  "SetGraphicsRootConstantBufferView", "SetGraphicsRootShaderResourceView", "SetGraphicsRootUnorderedAccessView",
  "SetComputeRootConstantBufferView", "SetComputeRootShaderResourceView", "SetComputeRootUnorderedAccessView",
]);
const PUSH_CONSTANT = new Set(["SetGraphicsRoot32BitConstant", "SetGraphicsRoot32BitConstants", "SetComputeRoot32BitConstant", "SetComputeRoot32BitConstants"]);
const INDIRECT = new Set(["ExecuteIndirect"]);

/**
 * What closes a run of dispatches outside a render pass, the same rule the library brackets
 * compute passes with: a barrier, a render pass begin, an event, a bundle, the end of the list.
 */
const COMPUTE_PASS_END = new Set([
  "ResourceBarrier", "Barrier", "OMSetRenderTargets", "BeginRenderPass", "BeginEvent", "EndEvent", "SetMarker",
  "ExecuteBundle", "Close", "EndRenderTargets",
]);

/** The pipeline a SetPipelineState binds: the library writes it as `pipeline` (the UI's name) or under its own parameter name. */
export function d3d12PipelineOf(a: ArgObject | null | undefined): ArgValue | undefined {
  if (!a) return undefined;
  return a.pipeline ?? a.pPipelineState ?? a.pStateObject;
}

function bindPointOf(method: string): string {
  return DISPATCH.has(method) || TRACE.has(method) ? "compute" : "graphics";
}

function handleName(v: ArgValue | undefined, nameOf: (v: ArgValue | undefined) => string): string {
  return isObject(v) ? nameOf(v.resource) : "";
}

/** "[2] Constants" for a root parameter bind. */
function rootSlot(a: ArgObject, what: string): string {
  return `[${num(a.RootParameterIndex)}] ${what}`;
}

function summarize(cmd: CaptureCommand, nameOf: (v: ArgValue | undefined) => string): string {
  const a = cmd.args;
  const m = cmd.method;
  if (!a) return "";
  switch (m) {
    case "DrawInstanced":
      return `${num(a.VertexCountPerInstance)} verts x${num(a.InstanceCount)}`;
    case "DrawIndexedInstanced":
      return `${num(a.IndexCountPerInstance)} idx x${num(a.InstanceCount)}`;
    case "DispatchMesh":
      return `${num(a.ThreadGroupCountX)}x${num(a.ThreadGroupCountY)}x${num(a.ThreadGroupCountZ)} mesh groups`;
    case "Dispatch":
      return `${num(a.ThreadGroupCountX)}x${num(a.ThreadGroupCountY)}x${num(a.ThreadGroupCountZ)} groups`;
    case "ExecuteIndirect":
      return `${nameOf(a.pArgumentBuffer)} x${num(a.MaxCommandCount)}${a.pCountBuffer ? " (counted)" : ""}`;
    case "DispatchRays": {
      const d = isObject(a.pDesc) ? a.pDesc : null;
      return d ? `${num(d.Width)}x${num(d.Height)}x${num(d.Depth)} rays` : "";
    }
    case "SetPipelineState":
    case "SetPipelineState1":
      return nameOf(d3d12PipelineOf(a));
    case "SetGraphicsRootSignature":
    case "SetComputeRootSignature":
      return nameOf(a.pRootSignature);
    case "SetGraphicsRootDescriptorTable":
    case "SetComputeRootDescriptorTable": {
      const h = isObject(a.BaseDescriptor) ? a.BaseDescriptor : null;
      return rootSlot(a, h ? `${nameOf(h.heap)}${h.index !== undefined ? ` +${num(h.index)}` : ""}` : "table");
    }
    case "SetGraphicsRootConstantBufferView":
    case "SetGraphicsRootShaderResourceView":
    case "SetGraphicsRootUnorderedAccessView":
    case "SetComputeRootConstantBufferView":
    case "SetComputeRootShaderResourceView":
    case "SetComputeRootUnorderedAccessView": {
      const loc = isObject(a.BufferLocation) ? a.BufferLocation : null;
      const buffer = loc ? nameOf(loc.buffer) : "";
      return rootSlot(a, buffer ? `${buffer}${num(loc?.offset) ? ` +${num(loc?.offset)}` : ""}` : str(loc?.address) || "(none)");
    }
    case "SetGraphicsRoot32BitConstant":
    case "SetGraphicsRoot32BitConstants":
    case "SetComputeRoot32BitConstant":
    case "SetComputeRoot32BitConstants":
      return rootSlot(a, `${a.size !== undefined ? num(a.size) : (num(a.Num32BitValuesToSet) || 1) * 4} bytes`);
    case "OMSetRenderTargets": {
      const targets = Array.isArray(a.pRenderTargetDescriptors) ? a.pRenderTargetDescriptors : [];
      const first = targets.find((t) => isObject(t) && t.resource);
      const name = handleName(first, nameOf);
      const count = num(a.NumRenderTargetDescriptors) || targets.length;
      const more = count > 1 ? ` +${count - 1}` : "";
      const depth = isObject(a.pDepthStencilDescriptor) ? " + depth" : "";
      return `${name || `${count} target${count === 1 ? "" : "s"}`}${more}${depth}`;
    }
    case "BeginRenderPass": {
      const targets = Array.isArray(a.pRenderTargets) ? a.pRenderTargets : [];
      const first = targets.find(isObject);
      const handle = first ? (isObject(first.cpuDescriptor) ? first.cpuDescriptor : first) : undefined;
      const name = handleName(handle, nameOf);
      const count = targets.length;
      const more = count > 1 ? ` +${count - 1}` : "";
      const depth = isObject(a.pDepthStencil) ? " + depth" : "";
      return `${name || `${count} target${count === 1 ? "" : "s"}`}${more}${depth}`;
    }
    case "ClearRenderTargetView":
      return handleName(a.RenderTargetView, nameOf);
    case "ClearDepthStencilView":
      return `${handleName(a.DepthStencilView, nameOf)} ${fmt(a.ClearFlags)}`.trim();
    case "ClearUnorderedAccessViewUint":
    case "ClearUnorderedAccessViewFloat":
      return nameOf(a.pResource);
    case "ResourceBarrier": {
      const n = num(a.NumBarriers) || (Array.isArray(a.pBarriers) ? a.pBarriers.length : 0);
      return `${n} barrier${n === 1 ? "" : "s"}`;
    }
    case "Barrier": {
      const groups = Array.isArray(a.pBarrierGroups) ? a.pBarrierGroups : [];
      const n = groups.reduce((sum: number, g) => sum + (isObject(g) ? num(g.NumBarriers) : 0), 0) || num(a.NumBarrierGroups);
      return `${n} barrier${n === 1 ? "" : "s"}`;
    }
    case "ExecuteCommandLists": {
      const n = num(a.NumCommandLists) || (Array.isArray(a.ppCommandLists) ? a.ppCommandLists.length : 0);
      return `${n} list${n === 1 ? "" : "s"}`;
    }
    case "ExecuteBundle":
      return nameOf(a.pCommandList);
    case "BeginEvent":
    case "SetMarker":
      return typeof a.label === "string" && a.label ? `"${a.label}"` : "";
    case "IASetVertexBuffers": {
      const views = Array.isArray(a.pViews) ? a.pViews : [];
      return `slot ${num(a.StartSlot)} +${num(a.NumViews) || views.length}`;
    }
    case "IASetIndexBuffer": {
      const v = isObject(a.pView) ? a.pView : null;
      const loc = v && isObject(v.BufferLocation) ? v.BufferLocation : null;
      return v ? `${nameOf(loc?.buffer) || str(loc?.address)} ${fmt(v.Format)}` : "(none)";
    }
    case "IASetPrimitiveTopology":
      return fmt(a.PrimitiveTopology);
    case "RSSetViewports": {
      const v = Array.isArray(a.pViewports) && isObject(a.pViewports[0]) ? a.pViewports[0] : null;
      return v ? `${num(v.Width)}x${num(v.Height)}` : "";
    }
    case "RSSetScissorRects": {
      const r = Array.isArray(a.pRects) && isObject(a.pRects[0]) ? a.pRects[0] : null;
      return r ? `${num(r.right) - num(r.left)}x${num(r.bottom) - num(r.top)} at ${num(r.left)},${num(r.top)}` : "";
    }
    case "CopyResource":
      return `${nameOf(a.pSrcResource)} -> ${nameOf(a.pDstResource)}`;
    case "CopyBufferRegion":
      return `${nameOf(a.pSrcBuffer)} -> ${nameOf(a.pDstBuffer)}  ${num(a.NumBytes)} bytes`;
    case "CopyTextureRegion": {
      const src = isObject(a.pSrc) ? nameOf(a.pSrc.pResource) : "";
      const dst = isObject(a.pDst) ? nameOf(a.pDst.pResource) : "";
      return `${src} -> ${dst}`;
    }
    case "ResolveSubresource":
      return `${nameOf(a.pSrcResource)} -> ${nameOf(a.pDstResource)}`;
    case "Present":
    case "Present1":
      return num(a.SyncInterval) ? `vsync ${num(a.SyncInterval)}` : "no vsync";
    case "Signal":
    case "Wait":
      return `${nameOf(a.pFence)} ${num(a.Value)}`;
    default:
      break;
  }
  // Everything else: a few scalars, enums and references.
  const parts: string[] = [];
  for (const [key, value] of Object.entries(a)) {
    if (parts.length >= 4) break;
    if (value === null || value === undefined) continue;
    if (typeof value === "number" || typeof value === "boolean") parts.push(`${key} ${value}`);
    else if (typeof value === "string") parts.push(/^(D3D12?_|DXGI_)/.test(value) ? fmt(value) : `${key} ${value}`);
    else if (isObject(value) && typeof value.__id === "number") { const n = nameOf(value); if (n) parts.push(n); }
  }
  return parts.join(", ");
}

export const D3D12_SETS: CommandSets = {
  DRAW,
  DISPATCH,
  TRACE,
  PASS_BEGIN,
  PASS_END,
  RECORD_BEGIN,
  RECORD_END,
  LABEL_BEGIN: new Set(["BeginEvent"]),
  LABEL_END: new Set(["EndEvent"]),
  SUBMIT,
  BIND_DESCRIPTOR,
  BIND_VERTEX: new Set(["IASetVertexBuffers"]),
  BIND_INDEX: new Set(["IASetIndexBuffer"]),
  PUSH_CONSTANT,
  INDIRECT,
  COMPUTE_PASS_END,
  // PASS_BEGIN is render-only: a compute pass is a run of dispatches the walk brackets itself.
  bindPointOf,
  BIND_PIPELINE: new Set(["SetPipelineState", "SetPipelineState1"]),
  // The library records the bind point beside the pipeline (a compute pipeline state binds compute).
  pipelineBindPointOf(_method: string, args: ArgObject | null): string {
    return args?.bindPoint === "compute" ? "compute" : "graphics";
  },
  graphicsBindPoint: "graphics",

  vertexBuffersOf(cmd: CaptureCommand): BoundVertexBuffer[] {
    const a = cmd.args;
    if (!a || !Array.isArray(a.pViews)) return [];
    const first = num(a.StartSlot);
    const out: BoundVertexBuffer[] = [];
    a.pViews.forEach((v, i) => {
      if (!isObject(v)) return;
      const loc = isObject(v.BufferLocation) ? v.BufferLocation : null;
      out.push({
        cmd,
        binding: first + i,
        buffer: loc?.buffer ?? null,
        offset: num(loc?.offset),
        size: v.SizeInBytes !== undefined ? num(v.SizeInBytes) : null,
        stride: v.StrideInBytes !== undefined ? num(v.StrideInBytes) : null,
        dataId: cmd.bufferData?.[i] ?? 0,
      });
    });
    return out;
  },

  indexBufferOf(cmd: CaptureCommand): BoundIndexBuffer | null {
    if (cmd.method !== "IASetIndexBuffer") return null;
    const a = cmd.args;
    const v = a && isObject(a.pView) ? a.pView : null;
    if (!v) return null;
    const loc = isObject(v.BufferLocation) ? v.BufferLocation : null;
    return {
      cmd,
      buffer: loc?.buffer ?? null,
      offset: num(loc?.offset),
      // The DXGI name; the index decoders read the width out of it (R16_UINT / R32_UINT).
      indexType: str(v.Format) || "DXGI_FORMAT_R16_UINT",
      dataId: cmd.bufferData?.[0] ?? 0,
    };
  },

  summarize,
};
