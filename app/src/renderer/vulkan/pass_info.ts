// Decoding a Vulkan render pass from the command that begins it: what it renders to, at what
// size, and with which load and store operations.
//
// Two commands begin a pass and they carry the attachments in completely different places.
// vkCmdBeginRenderPass names a VkRenderPass (the load/store ops and the subpasses that say which
// attachment is a color, depth or resolve target) and a VkFramebuffer (the views), so the
// attachment list has to be reassembled from three objects, or four with an imageless
// framebuffer. vkCmdBeginRendering carries everything inline. Both end up as one AttachmentUse
// list here.
//
// Split out of frame_analysis.ts so the render graph builder (vulkan/frame_resources.ts) reads
// the same attachments the frame rules do, rather than a second, subtly different decoding.
import { isObject, num, refId, str } from "./vulkan_object.js";
import type { ObjectLookup } from "./vulkan_object.js";
import type { ArgObject, ArgValue, CaptureCommand } from "../../shared/protocol.js";

export type AttachmentKind = "color" | "depth" | "resolve";

export interface AttachmentUse {
  kind: AttachmentKind;
  imageId: number | null;
  viewId: number | null;
  baseLayer: number;
  /** Array layers the view covers; 0 when the view says "all remaining". */
  layerCount: number;
  /** Mip level the view renders to, which is what makes a mip chain a chain and not a self-loop. */
  mipLevel: number;
  format: string;
  samples: number;
  loadOp: string;
  storeOp: string;
  stencilLoadOp: string;
  stencilStoreOp: string;
  /** VkImageUsageFlags of the image ("" when unknown: a swapchain image without a create info). */
  usage: string;
  /** A color attachment that has a resolve target in the same subpass. */
  resolved: boolean;
  /**
   * Dynamic rendering: the view the attachment resolves into, which is a target of the pass in
   * its own right but is not an entry of its own in `pRenderingInfo`. Null for a render pass
   * object, whose resolve targets are entries with kind "resolve".
   */
  resolveViewId: number | null;
  /** Index in the render pass' attachment list, or the color attachment index for dynamic rendering. */
  index: number;
}

/** What a pass begin command says about the pass, before its commands are walked. */
export interface PassAttachments {
  width: number;
  height: number;
  viewMask: number;
  attachments: AttachmentUse[];
}

/** "VK_ATTACHMENT_LOAD_OP_CLEAR" -> "CLEAR". */
export function loadStoreOp(v: ArgValue | undefined): string {
  return str(v).replace("VK_ATTACHMENT_LOAD_OP_", "").replace("VK_ATTACHMENT_STORE_OP_", "");
}

export function sampleCount(v: ArgValue | undefined): number {
  const m = /VK_SAMPLE_COUNT_(\d+)_BIT/.exec(str(v));
  return m ? Number(m[1]) : 1;
}

export function pNextChain(o: ArgObject | null | undefined): ArgObject[] {
  const chain = o?.pNext;
  return Array.isArray(chain) ? chain.filter(isObject) : [];
}

/** The VkImage a VkImageView was created from. */
export function imageOfView(db: ObjectLookup, viewId: number | null): number | null {
  const view = db.getObject(viewId);
  return view ? refId(view.descriptor?.image) : null;
}

/** The subresource range of a VkImageView: its base mip level, base layer and layer count. */
export function viewSubresource(db: ObjectLookup, viewId: number | null): { mipLevel: number; baseLayer: number; layerCount: number } {
  const view = db.getObject(viewId)?.descriptor ?? null;
  const range = isObject(view?.subresourceRange) ? view.subresourceRange : null;
  const layers = num(range?.layerCount);
  return {
    mipLevel: num(range?.baseMipLevel), baseLayer: num(range?.baseArrayLayer),
    // VK_REMAINING_ARRAY_LAYERS reads back as the raw 0xffffffff; report it as "all remaining" (0).
    layerCount: layers === 0xffffffff ? 0 : layers,
  };
}

/** Decodes the attachments of the pass `cmd` begins, or null when it is not a pass begin. */
export function decodePass(cmd: CaptureCommand, db: ObjectLookup): PassAttachments | null {
  const a = cmd.args;
  if (!a) return null;
  const pass: PassAttachments = { width: 0, height: 0, viewMask: 0, attachments: [] };
  if (cmd.method.startsWith("vkCmdBeginRendering")) {
    const info = isObject(a.pRenderingInfo) ? a.pRenderingInfo : null;
    if (!info) return pass;
    const extent = isObject(info.renderArea) && isObject(info.renderArea.extent) ? info.renderArea.extent : null;
    pass.width = num(extent?.width);
    pass.height = num(extent?.height);
    pass.viewMask = num(info.viewMask);
    const colors = Array.isArray(info.pColorAttachments) ? info.pColorAttachments.filter(isObject) : [];
    colors.forEach((c, i) => pass.attachments.push(dynamicAttachment(db, c, "color", i)));
    for (const key of ["pDepthAttachment", "pStencilAttachment"]) {
      const d = info[key];
      if (isObject(d) && refId(d.imageView) !== null) pass.attachments.push(dynamicAttachment(db, d, "depth", colors.length));
    }
    return pass;
  }
  const begin = isObject(a.pRenderPassBegin) ? a.pRenderPassBegin : null;
  if (!begin) return pass;
  const rp = db.getObject(refId(begin.renderPass))?.descriptor ?? null;
  const fb = db.getObject(refId(begin.framebuffer))?.descriptor ?? null;
  const extent = isObject(begin.renderArea) && isObject(begin.renderArea.extent) ? begin.renderArea.extent : null;
  pass.width = num(extent?.width) || num(fb?.width);
  pass.height = num(extent?.height) || num(fb?.height);
  if (!rp) return pass;
  // The view masks: VkRenderPassMultiviewCreateInfo (create info 1) or the subpasses' own (2).
  const subpasses = Array.isArray(rp.pSubpasses) ? rp.pSubpasses.filter(isObject) : [];
  const mv = pNextChain(rp).find((s) => str(s.sType) === "VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO");
  const masks = mv && Array.isArray(mv.pViewMasks) ? mv.pViewMasks.map(num) : subpasses.map((s) => num(s.viewMask));
  pass.viewMask = masks.reduce((m, v) => m | v, 0);
  // Attachment views: the framebuffer's, or VkRenderPassAttachmentBeginInfo for an imageless one.
  let views = Array.isArray(fb?.pAttachments) ? fb.pAttachments : [];
  const imageless = pNextChain(begin).find((s) => str(s.sType) === "VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO");
  if (imageless && Array.isArray(imageless.pAttachments)) views = imageless.pAttachments;
  const descs = Array.isArray(rp.pAttachments) ? rp.pAttachments : [];
  const kinds = new Map<number, AttachmentKind>();
  const resolvedColors = new Set<number>();
  for (const s of subpasses) {
    const refs = (key: string): number[] => (Array.isArray(s[key]) ? s[key] : []).filter(isObject).map((r) => num(r.attachment)).filter((i) => i < 0xffffffff);
    const colors = refs("pColorAttachments");
    const resolves = refs("pResolveAttachments");
    colors.forEach((c, i) => { kinds.set(c, "color"); if (resolves[i] !== undefined) resolvedColors.add(c); });
    for (const r of resolves) kinds.set(r, "resolve");
    const ds = isObject(s.pDepthStencilAttachment) ? num(s.pDepthStencilAttachment.attachment) : 0xffffffff;
    if (ds < 0xffffffff) kinds.set(ds, "depth");
  }
  descs.forEach((d, i) => {
    if (!isObject(d)) return;
    const kind = kinds.get(i);
    if (!kind) return;
    const viewId = refId(views[i]);
    pass.attachments.push({
      kind, viewId, imageId: imageOfView(db, viewId), ...viewSubresource(db, viewId),
      format: str(d.format), samples: sampleCount(d.samples),
      loadOp: loadStoreOp(d.loadOp), storeOp: loadStoreOp(d.storeOp),
      stencilLoadOp: loadStoreOp(d.stencilLoadOp), stencilStoreOp: loadStoreOp(d.stencilStoreOp),
      usage: "", resolved: resolvedColors.has(i), resolveViewId: null, index: i,
    });
  });
  return pass;
}

function dynamicAttachment(db: ObjectLookup, att: ArgObject, kind: AttachmentKind, index: number): AttachmentUse {
  const viewId = refId(att.imageView);
  const imageId = imageOfView(db, viewId);
  const view = db.getObject(viewId)?.descriptor ?? null;
  const image = db.getObject(imageId)?.descriptor ?? null;
  return {
    kind, viewId, imageId, ...viewSubresource(db, viewId),
    format: str(view?.format ?? image?.format), samples: sampleCount(image?.samples),
    loadOp: loadStoreOp(att.loadOp), storeOp: loadStoreOp(att.storeOp),
    stencilLoadOp: loadStoreOp(att.loadOp), stencilStoreOp: loadStoreOp(att.storeOp),
    usage: "", resolved: refId(att.resolveImageView) !== null, resolveViewId: refId(att.resolveImageView), index,
  };
}
