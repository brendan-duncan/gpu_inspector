// Frame Issues rules over sampling state, for both APIs.
//
// These need no GPU counters: what a texture is and how a draw samples it is in the descriptors
// the capture already carries. They predict what a counter would otherwise have to measure — a
// large texture with no mip chain is what a texture read cache limiter reports after the fact.
//
//   unmipped-texture   a large content texture sampled with no mip chain
//
// The two APIs say the same things differently: Metal binds a texture to a stage with its own
// command and describes it with `mipmapLevelCount` and a usage string, Vulkan binds an image view
// through a descriptor set and describes the image with `mipLevels` and usage flags. Only the
// reading differs, so the rule itself is written once.
import { isObject, num, refId, str } from "./vulkan/vulkan_object.js";
import type { FrameFinding } from "./vulkan/frame_analysis.js";
import type { ObjectLookup } from "./vulkan/vulkan_object.js";
import type { ArgObject, CaptureCommand } from "../shared/protocol.js";
import type { CaptureData } from "./capture_data.js";

export const SAMPLING_RULES = ["unmipped-texture"];

/**
 * A texture this big, sampled with one mip level, thrashes the texture cache as soon as it is
 * drawn smaller than itself. Below it the whole texture fits in cache and the mip chain saves
 * little, so the rule stays quiet.
 */
const LARGE_TEXTURE_PIXELS = 1024 * 1024;

/** Metal: binding a texture to a shader stage. It will be sampled, not written. */
const METAL_TEXTURE_BINDS = new Set([
  "setFragmentTexture:atIndex:", "setFragmentTextures:withRange:",
  "setVertexTexture:atIndex:", "setVertexTextures:withRange:",
  "setTexture:atIndex:", "setTextures:withRange:",
]);

/** Vulkan descriptor types a shader samples from. */
function samplesFrom(type: string): boolean {
  return type.includes("SAMPLED_IMAGE") || type.includes("COMBINED_IMAGE_SAMPLER");
}

/** Texture or image id -> the first command that bound it for sampling. */
function collectSampled(data: CaptureData): Map<number, CaptureCommand> {
  const out = new Map<number, CaptureCommand>();
  const note = (id: number | null, cmd: CaptureCommand): void => {
    if (id !== null && !out.has(id)) out.set(id, cmd);
  };
  for (const cmd of data.commands) {
    if (cmd.descriptors) {
      for (const set of cmd.descriptors.sets) {
        for (const b of set.bindings) {
          if (!samplesFrom(b.type)) continue;
          for (const d of b.descriptors) note(refId(d?.imageView), cmd);
        }
      }
      continue;
    }
    const a = cmd.args;
    if (!a || !METAL_TEXTURE_BINDS.has(cmd.method)) continue;
    note(refId(a.texture), cmd);
    if (Array.isArray(a.textures)) for (const t of a.textures) note(refId(t), cmd);
  }
  return out;
}

/** The image behind what was bound: a Vulkan image view names one, a Metal texture is one. */
function imageOf(id: number, db: ObjectLookup): ArgObject | null {
  const object = db.getObject(id);
  const d = object?.descriptor;
  if (!d) return null;
  if (d.image !== undefined) {
    const image = db.getObject(refId(d.image));
    return image?.descriptor ?? null;
  }
  return d;
}

/** Pixels of the largest mip, and how many levels it has. */
function sizeOf(d: ArgObject): { pixels: number; levels: number } {
  const extent = isObject(d.extent) ? d.extent : null;
  const width = num(extent?.width) || num(d.width);
  const height = num(extent?.height) || num(d.height);
  return { pixels: width * height, levels: num(d.mipLevels) || num(d.mipmapLevelCount) || 1 };
}

/** Whether the image is something the frame renders into, in either API's spelling. */
function isRenderTarget(d: ArgObject): boolean {
  const usage = str(d.usage);
  return usage.includes("RenderTarget") || usage.includes("ATTACHMENT_BIT");
}

export function analyzeSampling(data: CaptureData, db: ObjectLookup): { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> } {
  const findings: FrameFinding[] = [];
  const byCommand = new Map<number, FrameFinding[]>();
  const commands: number[] = [];
  let first: number | null = null;
  let count = 0;
  for (const [id, cmd] of collectSampled(data)) {
    const d = imageOf(id, db);
    if (!d) continue;
    const { pixels, levels } = sizeOf(d);
    // A render target sampled by a later pass is normally read at its own size, where a mip
    // chain would not help and could not be generated for free anyway.
    if (levels > 1 || pixels < LARGE_TEXTURE_PIXELS || isRenderTarget(d)) continue;
    if (first === null) first = cmd.index;
    count++;
    if (commands.length < 64) commands.push(cmd.index);
  }
  if (count) {
    const megapixels = LARGE_TEXTURE_PIXELS / (1024 * 1024);
    const f: FrameFinding = {
      rule: "unmipped-texture", severity: "medium", confidence: "medium",
      message: `${count} texture${count === 1 ? " is" : "s are"} sampled with a single mip level at ${megapixels} megapixel or more. Drawn smaller than itself, such a texture reads scattered texels and misses the cache on most of them; a mip chain costs a third more memory and reads one texel per sample.`,
      commandIndex: first ?? undefined, count,
    };
    findings.push(f);
    for (const index of commands) {
      const list = byCommand.get(index);
      if (list) list.push(f); else byCommand.set(index, [f]);
    }
  }
  return { findings, byCommand };
}
