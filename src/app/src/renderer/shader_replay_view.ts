// What a shader edit did to the captured frame (renderer/shader_replay.ts): each render target
// the edit changed, as the capture read it back, as the replay left it, and where the two differ.
//
// Three pictures rather than two, because the two are usually the same picture to the eye: an edit
// that moves a highlight by a pixel changes a few hundred texels out of a million, and the way to
// see a few hundred texels is to be shown only them.
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import type { Widget } from "./widget/widget.js";
import { decodeImage } from "./vulkan/texture_decode.js";
import type { CapturedTexture } from "./capture_data.js";
import { changedTargets, replacementProblems, type ReplayedTarget, type ReplayedTargets } from "./shader_replay.js";

export interface ShaderReplayViewOptions {
  /** What was replaced, in words: "fragment stage of Cube pipeline". */
  edited: string[];
  /** The pipelines replaced, for telling a refused replacement from an effect. */
  pipelines: number[];
  /** The capture's own read-back of a target. */
  captured: (t: ReplayedTarget) => CapturedTexture | null;
  /** A name for an image. */
  imageName: (id: number) => string;
  onInspect?: (id: number) => void;
}

function canvasOf(pixels: Uint8ClampedArray<ArrayBuffer>, width: number, height: number): HTMLCanvasElement {
  const canvas = document.createElement("canvas");
  canvas.className = "shader-replay-canvas";
  canvas.width = width;
  canvas.height = height;
  canvas.getContext("2d")!.putImageData(new ImageData(pixels, width, height), 0, 0);
  return canvas;
}

/** Where two decoded images differ: the changed texels in the edit's colors over a dimmed copy of the original. */
function difference(before: Uint8ClampedArray, after: Uint8ClampedArray): Uint8ClampedArray<ArrayBuffer> {
  const out = new Uint8ClampedArray(before.length);
  for (let i = 0; i + 3 < before.length; i += 4) {
    const same = before[i] === after[i] && before[i + 1] === after[i + 1] && before[i + 2] === after[i + 2] && before[i + 3] === after[i + 3];
    if (same) {
      const gray = (before[i] + before[i + 1] + before[i + 2]) / 12;
      out[i] = out[i + 1] = out[i + 2] = gray;
    } else {
      out[i] = 255;
      out[i + 1] = 64;
      out[i + 2] = 160;
    }
    out[i + 3] = 255;
  }
  return out;
}

export function renderShaderReplay(container: Widget, result: ReplayedTargets, o: ShaderReplayViewOptions): void {
  container.html = "";
  const body = new Div(container, { style: "padding: 12px;" });
  new Div(body, { text: `Replayed with the edited ${o.edited.join(", ")} on ${result.device || "this GPU"}.`, class: "frame-bound-verdict" });

  const refused = replacementProblems(result, o.pipelines);
  if (refused.length) {
    new Div(body, {
      text: "The edited shader was not accepted, so its pipeline was left out of the replay and its draws drew nothing. What differs below is the draws missing, not what the edit computes.",
      class: "inspect_info_error", style: "margin: 6px 0;",
    });
    for (const p of refused) new Div(body, { text: p, class: "text-muted font-sm" });
  }

  const compared = result.targets.filter((t) => t.compared);
  const changed = changedTargets(result);
  if (!compared.length) {
    new Div(body, { text: "The capture holds no render targets to compare (it was taken with Render targets off).", class: "text-muted" });
  } else if (!changed.length) {
    new Div(body, { text: `All ${compared.length} render targets are exactly as the capture read them back: nothing the frame shows depends on the change.`, class: "text-muted" });
  }

  for (const t of changed) {
    const box = new Div(body, { class: "shader-replay-target" });
    const title = new Div(box, { class: "capture-texture-title" });
    const name = new Span(title, { text: o.imageName(t.image), class: o.onInspect ? "dependency_link" : "" });
    if (o.onInspect) name.element.onclick = () => o.onInspect?.(t.image);
    new Span(title, { text: `pass ${t.passIndex}, attachment ${t.attachment} (${t.aspect})`, class: "text-muted", style: "margin-left: 10px;" });
    const share = (100 * t.differingTexels) / Math.max(1, t.texels);
    new Div(box, {
      text: `${t.differingTexels.toLocaleString()} of ${t.texels.toLocaleString()} texels changed (${share < 0.1 ? share.toFixed(3) : share.toFixed(1)}%), by up to ${t.maxByteDelta} in a byte`,
      class: "text-muted font-sm",
    });
    const captured = o.captured(t);
    const before = captured?.data ? decodeImage(captured.info, captured.data) : null;
    const after = captured && t.pixels ? decodeImage(captured.info, t.pixels) : null;
    if (!before || !after) {
      new Div(box, { text: "The pixels cannot be shown: the capture's own copy of this target is not loaded, or its format is not one the viewer decodes.", class: "text-muted font-sm" });
      continue;
    }
    const row = new Div(box, { class: "shader-replay-images" });
    const cell = (label: string, pixels: Uint8ClampedArray<ArrayBuffer>): void => {
      const c = new Div(row);
      new Div(c, { text: label, class: "text-muted font-sm" });
      c.element.appendChild(canvasOf(pixels, t.width, t.height));
    };
    cell("As captured", before);
    cell("With the edit", after);
    cell("What changed", difference(before, after));
  }

  const same = compared.length - changed.length;
  if (changed.length && same > 0) new Div(body, { text: `${same} other render target${same === 1 ? " is" : "s are"} exactly as captured.`, class: "text-muted font-sm" });
  const others = result.problems.filter((p) => !refused.includes(p));
  if (others.length) {
    new Div(body, { text: `The replay left out ${others.length} thing${others.length === 1 ? "" : "s"} it could not rebuild, with or without the edit:`, class: "text-muted font-sm", style: "margin-top: 10px;" });
    for (const p of others.slice(0, 8)) new Div(body, { text: p, class: "text-muted font-sm" });
  }
}
