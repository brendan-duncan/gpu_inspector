// The Metal half of the shader debugger's session setup (src/renderer/metal/shader_debug.ts and the
// rasterizer it shares with Vulkan in src/renderer/shader_debug_setup.ts): the state a Metal draw
// keeps on its encoder rather than in its pipeline, and the conventions that differ from Vulkan's.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "mtldbg-"));
const out = join(dir, "mtldbg.mjs");
buildSync({
  stdin: {
    contents: `export { rasterStateOf, coveringTriangle, coveredPixel, interpolate } from "./shader_debug_setup.js"; export { metalSampler } from "./metal/shader_debug.js"; export { emptyDrawState } from "./draw_state.js";`,
    resolveDir: join(here, "..", "src", "renderer"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { rasterStateOf, coveringTriangle, coveredPixel, metalSampler, emptyDrawState } = await import(pathToFileURL(out).href);

/** A DrawState as a Metal draw leaves it: the encoder's commands, not a pipeline's create info. */
function metalState({ viewport = { originX: 0, originY: 0, width: 800, height: 600, znear: 0, zfar: 1 }, cullMode, winding, depthCompare } = {}) {
  const state = emptyDrawState("compute");
  state.pipeline = { type: "MTLRenderPipelineState", descriptor: {} };
  state.viewports = viewport;
  state.cullMode = cullMode ?? null;
  state.frontFace = winding ?? null;
  state.depthStencil = depthCompare === undefined ? null : { descriptor: { depthCompareFunction: depthCompare } };
  return state;
}

/** A mesh output holding one triangle, as interpretedMeshOutput packs it. */
function triangle(clip) {
  const stride = 16;
  const data = new Uint8Array(3 * stride);
  const view = new DataView(data.buffer);
  clip.forEach((v, i) => v.forEach((x, k) => view.setFloat32(i * stride + k * 4, x, true)));
  return {
    command: 0, method: "drawPrimitives:vertexStart:vertexCount:", frame: 0, commandBuffer: 0, passIndex: 0,
    measured: true, topology: "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST", stride, vertices: 3, truncated: false,
    outputs: [{ name: "position", offset: 0, components: 4, base: "float", builtin: "Position" }], data,
  };
}

test("a Metal draw's rasterizer state comes off its encoder", () => {
  const raster = rasterStateOf(metalState({ cullMode: "MTLCullModeBack", winding: "MTLWindingCounterClockwise", depthCompare: 3 }));
  assert.deepEqual(raster.viewport, { x: 0, y: 0, width: 800, height: 600, minDepth: 0, maxDepth: 1 },
    "MTLViewport spells its origin and depth range differently from VkViewport");
  assert.equal(raster.cullBack, true);
  assert.equal(raster.cullFront, false);
  assert.equal(raster.ccwFront, true);
  assert.equal(raster.depthPrefers, "less", "MTLCompareFunctionLessEqual is recorded as the number 3");
  assert.equal(raster.yUp, true);
});

test("a Metal pass with no setViewport: uses the render target's size", () => {
  const state = metalState({ viewport: null });
  state.viewports = null;
  const whole = { x: 0, y: 0, width: 1280, height: 960, minDepth: 0, maxDepth: 1 };
  assert.deepEqual(rasterStateOf(state, whole).viewport, whole);
  assert.equal(rasterStateOf(state).viewport, null, "with nothing to fall back on, there is no viewport");
});

test("clip-space +Y is the top of a Metal render target and the bottom of a Vulkan one", () => {
  // A triangle filling the upper half of clip space (y from 0 to 1).
  const mesh = triangle([[-1, 0, 0, 1], [1, 0, 0, 1], [0, 1, 0, 1]]);
  const metal = rasterStateOf(metalState());
  const vulkan = { ...metal, yUp: false };
  // Metal: the top of the render target, which is a small window y.
  assert.ok(coveringTriangle(metal, mesh, 400, 100).hit, "Metal puts +Y at the top");
  assert.ok(!coveringTriangle(metal, mesh, 400, 500).hit, "and nothing at the bottom");
  // Vulkan: the same triangle lands in the lower half.
  assert.ok(coveringTriangle(vulkan, mesh, 400, 500).hit, "Vulkan puts +Y at the bottom");
  assert.ok(!coveringTriangle(vulkan, mesh, 400, 100).hit);
  // The pixel offered by default follows the same convention.
  const pixel = coveredPixel(metalState(), mesh, metal);
  assert.ok(pixel && pixel.y < 300, `a covered pixel is in the top half: ${JSON.stringify(pixel)}`);
});

test("culling and facing read the triangle's winding in framebuffer coordinates", () => {
  const mesh = triangle([[-1, -1, 0, 1], [1, -1, 0, 1], [0, 1, 0, 1]]);
  const raster = rasterStateOf(metalState({ winding: "MTLWindingClockwise" }));
  const hit = coveringTriangle(raster, mesh, 400, 300).hit;
  assert.ok(hit, "the triangle covers the middle of the viewport");
  const culled = coveringTriangle({ ...raster, cullBack: !hit.front, cullFront: hit.front }, mesh, 400, 300);
  assert.equal(culled.hit, null, "culling the face it has leaves nothing");
});

test("an MTLSamplerDescriptor's numeric enums become the interpreter's sampler", () => {
  const sampler = metalSampler({
    descriptor: {
      minFilter: 1, magFilter: 1, mipFilter: 2,
      sAddressMode: 2, tAddressMode: 3, rAddressMode: 0,
      borderColor: 1, normalizedCoordinates: true, lodMinClamp: 0, lodMaxClamp: 8, compareFunction: 1,
    },
  });
  assert.equal(sampler.minFilter, "linear");
  assert.equal(sampler.mipmapMode, "linear");
  assert.deepEqual(sampler.address, ["repeat", "mirror", "clamp"]);
  assert.deepEqual(sampler.border, [0, 0, 0, 1]);
  assert.equal(sampler.compareOp, "Less");
  assert.equal(sampler.maxLod, 8);
  assert.equal(sampler.unnormalized, false);

  const clamped = metalSampler({ descriptor: { minFilter: 0, magFilter: 0, compareFunction: 0 } }, { lodMinClamp: 1, lodMaxClamp: 3 });
  assert.equal(clamped.minFilter, "nearest");
  assert.equal(clamped.compareOp, null, "MTLCompareFunctionNever means the sampler does not compare");
  assert.equal(clamped.minLod, 1, "setFragmentSamplerState:lodMinClamp:... overrides the descriptor's clamps");
  assert.equal(clamped.maxLod, 3);
  assert.equal(metalSampler(null), null);
});
