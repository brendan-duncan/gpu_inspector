// Which heap slots a DXIL shader takes (src/renderer/d3d12/heap_indices.ts), and the draw constants
// that decide them (src/renderer/d3d12/indexed_heap.ts, drawConstants).
//
// The first case is test/d3d12_triangle's cube_bindless.hlsl as dxc disassembles it: the index is
// the Frame cbuffer's `flags` (b1, root constants) shifted right by 8. The rest are the shapes an
// index can take that the analysis has to tell apart: a literal, one from a shader input (which no
// capture can settle), and one whose constant the capture does not hold.
//
//     cd src/app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "heapidx-"));
const load = async (name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", "renderer", "d3d12", `${name}.ts`)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return import(pathToFileURL(out).href);
};
const { heapAccesses } = await load("heap_indices");
const { drawConstants } = await load("indexed_heap");

// cube_bindless_ps.cso, the lines that matter, as dxinsp_shader.exe --disassemble prints them.
const CUBE_BINDLESS = `
define void @PSMain() {
  %Frame_cbuffer = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 1, i32 1, i32 0, i8 2 }, i32 1, i1 false), !dbg !118 ; line:44 col:20  ; CreateHandleFromBinding(bind,index,nonUniformIndex)
  %1 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %Frame_cbuffer, %dx.types.ResourceProperties { i32 13, i32 8 }), !dbg !119 ; line:43 col:23  ; AnnotateHandle(res,props)  resource: CBuffer
  %18 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %1, i32 0), !dbg !143 ; line:49 col:48  ; CBufferLoadLegacy(handle,regIndex)
  %19 = extractvalue %dx.types.CBufRet.i32 %18, 1, !dbg !143 ; line:49 col:48
  %20 = lshr i32 %19, 8, !dbg !144 ; line:49 col:54
  %21 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %20, i1 false, i1 false), !dbg !145 ; line:49 col:25  ; CreateHandleFromHeap(index,samplerHeap,nonUniformIndex)
  ret void
}`;

/** Root constants b1: time (a float) then flags, bit 0 swap and the slot from bit 8. */
const flags = (slot) => (register, space, byteOffset) => (register === 1 && space === 0 && byteOffset === 4 ? (slot << 8) | 1 : null);

test("an index from root constants resolves to the slot the draw's values give", () => {
  const [a] = heapAccesses(CUBE_BINDLESS, flags(10));
  assert.equal(a.slot, 10);
  assert.equal(a.samplers, false);
  assert.equal(a.nonUniform, false);
  assert.equal(a.line, 49);
  assert.equal(a.expression, "((b1 byte 4) >> 8) = 10");
});

test("a constant the capture does not hold leaves the slot unknown, saying which", () => {
  const [a] = heapAccesses(CUBE_BINDLESS, () => null);
  assert.equal(a.slot, null);
  assert.match(a.expression, /b1 byte 4.*does not hold/);
});

test("a literal index, a shader input's, and a non-uniform sampler index are told apart", () => {
  const dis = `
  %1 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 7, i1 false, i1 false) ; line:10 col:1
  %2 = call i32 @dx.op.loadInput.i32(i32 4, i32 3, i32 0, i8 0, i32 undef)
  %3 = add nuw i32 %2, 100
  %4 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %3, i1 true, i1 true) ; line:11 col:1`;
  const [literal, input] = heapAccesses(dis, () => null);
  assert.deepEqual([literal.slot, literal.expression, literal.samplers, literal.line], [7, "7", false, 10]);
  assert.equal(input.slot, null);
  assert.equal(input.expression, "a shader input");
  assert.equal(input.samplers, true);
  assert.equal(input.nonUniform, true);
});

test("a float constant converted to an integer, and a sum of two constants", () => {
  const dis = `
  %cb = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 0, i32 0, i32 2, i8 2 }, i32 0, i1 false)
  %h = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %cb, %dx.types.ResourceProperties { i32 13, i32 32 })
  %r = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %h, i32 1)
  %f = extractvalue %dx.types.CBufRet.f32 %r, 2
  %i = fptoui float %f to i32
  %s = add i32 %i, 3
  %x = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %s, i1 false, i1 false)`;
  const bits = new DataView(new ArrayBuffer(4));
  bits.setFloat32(0, 5.0, true);
  const [a] = heapAccesses(dis, (register, space, byteOffset) => (register === 0 && space === 2 && byteOffset === 24 ? bits.getUint32(0, true) : null));
  assert.equal(a.slot, 8);
  assert.equal(a.expression, "(uint((b0 space2 byte 24)) + 3) = 8");
});

test("drawConstants: root constants by register, written by the calls since the root signature was set", () => {
  const list = { __id: 11, __class: "ID3D12GraphicsCommandList" };
  const words = new Uint8Array(new Uint32Array([0x3f000000, (10 << 8) | 1]).buffer);
  const b64 = Buffer.from(words).toString("base64");
  const commands = [
    { method: "Reset", object: list },
    { method: "SetGraphicsRootSignature", object: list, args: { pRootSignature: { __id: 29 } } },
    { method: "SetGraphicsRoot32BitConstants", object: list, args: { RootParameterIndex: 1, Num32BitValuesToSet: 2, DestOffsetIn32BitValues: 0, pSrcData: { __bytes: 8, base64: b64 } } },
    // A later single constant overwrites the second value.
    { method: "SetGraphicsRoot32BitConstant", object: list, args: { RootParameterIndex: 1, SrcData: (12 << 8), DestOffsetIn32BitValues: 1 } },
    { method: "DrawIndexedInstanced", object: list },
  ].map((c, index) => ({ index, frame: 0, args: null, ...c }));
  const root = { args: { pDesc: { Desc_1_1: { pParameters: [
    { ParameterType: "D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE" },
    { ParameterType: "D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS", Constants: { ShaderRegister: 1, RegisterSpace: 0, Num32BitValues: 2 } },
  ] } } } };
  // A table's CBV at b0: its captured bytes start at buffer offset 256, the view at 512.
  const cbv = new Uint8Array(512);
  new DataView(cbv.buffer).setUint32(256 + 8, 42, true);
  const sets = [{ set: 0, descriptorSet: null, bindings: [{ binding: 0, type: "D3D12_DESCRIPTOR_RANGE_TYPE_CBV", register: 0, space: 0, descriptors: [{ buffer: { __id: 44 }, offset: 512, range: 256, data: 6 }] }] }];
  const read = drawConstants(commands, commands[4], (id) => (id === 29 ? root : null), false, sets, (id) => (id === 6 ? { bytes: cbv, offset: 256 } : null));
  assert.equal(read(1, 0, 0), 0x3f000000);
  assert.equal(read(1, 0, 4), 12 << 8);
  assert.equal(read(1, 0, 8), null, "past the parameter's values");
  assert.equal(read(0, 0, 8), 42);
  assert.equal(read(5, 0, 0), null);
  const [a] = heapAccesses(CUBE_BINDLESS, read);
  assert.equal(a.slot, 12);
});
