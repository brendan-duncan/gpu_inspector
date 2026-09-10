// The compressed-texture decoders against reference vectors (tools/texture_vectors.py):
// every texel of every vector within the vector's tolerance, in 8-bit units.
//
//     cd app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const vectors = JSON.parse(readFileSync(join(here, "vectors", "texture_decode.json"), "utf8")).vectors;

// The decoder is TypeScript with .js import specifiers: bundle it the way the app is built.
const out = join(mkdtempSync(join(tmpdir(), "texdec-")), "texture_decode.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "renderer", "vulkan", "texture_decode.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { decodeTexels } = await import(pathToFileURL(out).href);

function bytes(b64) {
  return new Uint8Array(Buffer.from(b64, "base64"));
}

for (const v of vectors) {
  test(`${v.format} ${v.width}x${v.height}`, () => {
    const data = bytes(v.data);
    const expected = bytes(v.expected);
    const tex = decodeTexels({ format: v.format, aspect: "color", width: v.width, height: v.height }, data);
    assert.ok(tex, "format decodes");
    const n = v.width * v.height;
    let worst = 0;
    let worstAt = "";
    // SNORM references store -1..1 as 0..255; float references clamp to 0..1 like UNORM.
    const signed = /SNORM/.test(v.format);
    // Punchthrough alpha: the specification makes a transparent texel black; the reference
    // keeps its colour. Only the alpha is compared there.
    const punchthrough = /R8G8B8A1/.test(v.format);
    for (let i = 0; i < n; i++) {
      for (let c = 0; c < 4; c++) {
        if (punchthrough && c < 3 && expected[i * 4 + 3] === 0) continue;
        let value = tex.values[i * 4 + c];
        if (c >= tex.channels) value = c === 3 ? 1 : 0;
        // The references store signed and float data as 8-bit unsigned: -1..1 or 0..1 clamped.
        let got;
        if (signed && c < tex.channels) got = Math.round((Math.max(-1, Math.min(1, value)) * 0.5 + 0.5) * 255);
        else got = Math.round(Math.max(0, Math.min(1, value)) * 255);
        const diff = Math.abs(got - expected[i * 4 + c]);
        if (diff > worst) { worst = diff; worstAt = `texel (${i % v.width}, ${Math.floor(i / v.width)}) channel ${c}: got ${got}, expected ${expected[i * 4 + c]}`; }
      }
    }
    assert.ok(worst <= v.tolerance, `worst difference ${worst} > ${v.tolerance} at ${worstAt}`);
  });
}
