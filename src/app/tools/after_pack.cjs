// electron-builder afterPack hook: drops the parts of Electron the app never loads, then hands
// off to the ad-hoc signing fallback in adhoc_sign.cjs. electron-builder takes one afterPack, so
// the two live behind this one entry point.
//
// Pruning has to happen here rather than after packaging, because afterPack runs before
// electron-builder signs: on macOS the signature then seals the pruned tree, instead of sealing
// files that are later taken out from under it.
const fs = require("node:fs");
const path = require("node:path");
const adhocSign = require("./adhoc_sign.cjs").default;

// DirectXShaderCompiler, which Dawn uses to compile WGSL to HLSL for WebGPU. The app has no
// WebGPU in it: the one thing its renderer draws with the GPU is the mesh preview
// (src/renderer/mesh_preview.ts), and that is WebGL2, which goes through ANGLE's D3D11 backend
// and the separate, older d3dcompiler_47.dll -- which is why that one is not in this list.
//
// dxcompiler.dll itself is kept, but moved: the Direct3D 12 capture library and dxinsp_shader.exe
// need a dxcompiler.dll for DXIL reflection, disassembly and the shader editor's assembler and
// validator, and look beside themselves first (src/d3d12/src/shader_reflect.cpp), so Electron's
// copy goes into resources/layer rather than a user without the Vulkan or Windows SDK going
// without shader text. It is a full DXC build (the shader-edit test passes against it, validator
// included), so dxil.dll, Microsoft's separate validator, is not needed and is removed: 1.5MB.
const WINDOWS_UNUSED = ["dxil.dll"];
const WINDOWS_MOVED = [{ name: "dxcompiler.dll", to: path.join("resources", "layer") }];

function relocate(appOutDir, moves) {
  for (const { name, to } of moves) {
    const file = path.join(appOutDir, name);
    if (!fs.existsSync(file)) {
      // A later Electron may stop shipping it; the library then looks in the SDKs as before.
      console.log(`  • not present, nothing to move  file=${name}`);
      continue;
    }
    const dir = path.join(appOutDir, to);
    fs.mkdirSync(dir, { recursive: true });
    fs.renameSync(file, path.join(dir, name));
    console.log(`  • moved Electron file beside the capture library  file=${name} to=${to}`);
  }
}

function prune(appOutDir, names) {
  for (const name of names) {
    const file = path.join(appOutDir, name);
    let size;
    try {
      size = fs.statSync(file).size;
    } catch {
      // Not an error: a later Electron may stop shipping it, or ship it under another name. The
      // point of the hook is that the file is absent, and it already is.
      console.log(`  • not present, nothing to remove  file=${name}`);
      continue;
    }
    fs.rmSync(file);
    console.log(`  • removed unused Electron file  file=${name} size=${(size / 1024 / 1024).toFixed(1)}MB`);
  }
}

exports.default = async function afterPack(context) {
  if (context.electronPlatformName === "win32") {
    relocate(context.appOutDir, WINDOWS_MOVED);
    prune(context.appOutDir, WINDOWS_UNUSED);
  }
  await adhocSign(context);
};
