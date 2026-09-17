// How a D3D12 shader's registers survive the trip through SPIR-V, for the shader debugger.
//
// There is no DXIL interpreter here, so a D3D12 stage is stepped as its HLSL compiled to SPIR-V by
// dxc (main/shader_tools.ts compileHlslForDebugging) in the same interpreter a Vulkan capture's
// modules run in. SPIR-V names a resource by a descriptor set and a binding; HLSL by a register
// class (b, t, s, u), a number and a space. dxc maps the space to the set and the register number
// to the binding, which would make t0 and b0 the same binding, so the compile shifts each class by
// a multiple of 65536 (-fvk-t-shift and friends) and the D3D12 side of the debugger
// (renderer/d3d12/shader_debug.ts) undoes the shift to find the register in the capture's binding
// snapshots. Both sides read this file, so the two cannot drift apart.

/** The bindings of each register class start here: b at 0, t at 65536, s at 131072, u at 196608. */
export const HLSL_BINDING_SHIFT = 0x10000;

export const HLSL_REGISTER_KINDS = ["b", "t", "s", "u"] as const;
export type HlslRegisterKind = (typeof HLSL_REGISTER_KINDS)[number];

export interface HlslRegister {
  kind: HlslRegisterKind;
  register: number;
  space: number;
}

/** The dxc options that shift each register class (the b class stays at 0). */
export const HLSL_SHIFT_ARGS: readonly string[] = HLSL_REGISTER_KINDS.flatMap((k, i) => (i ? [`-fvk-${k}-shift`, String(i * HLSL_BINDING_SHIFT), "all"] : []));

/** The register a translated module's descriptor set and binding stand for. */
export function hlslRegisterOf(set: number, binding: number): HlslRegister {
  const kind = HLSL_REGISTER_KINDS[Math.min(3, Math.floor(binding / HLSL_BINDING_SHIFT))];
  return { kind, register: binding % HLSL_BINDING_SHIFT, space: set };
}

/** "t3", or "t3 space 1" outside space 0: how HLSL spells the register. */
export function hlslBindingName(set: number, binding: number): string {
  const r = hlslRegisterOf(set, binding);
  return `${r.kind}${r.register}${r.space ? ` space ${r.space}` : ""}`;
}
