// Measuring a shader stage by ablation from GPU Inspector's main process and the MCP server: the
// variants planned (renderer/vulkan/spirv_ablate.ts), each checked with spirv-val so no module the
// validator rejects reaches the driver, the replay asked to time them (vkinsp_replay --ablate), and
// the timings turned into what each part saved (renderer/shader_ablation.ts).
import type { ReplayAnalysis, ReplayRun } from "./replay.js";
import { assembleDxil, disassembleDxil, validateSpirv } from "./shader_tools.js";
import { planDxilAblation } from "../renderer/d3d12/dxil_ablate.js";
import { encodeAblationRequest, measuredAblation, parseAblationResult, type ShaderAblation } from "../renderer/shader_ablation.js";
import { planAblation } from "../renderer/vulkan/spirv_ablate.js";
import { analyzeSpirvCached } from "../renderer/vulkan/spirv_analysis.js";
import type { ShaderStage } from "../renderer/vulkan/spirv_reflect.js";

export interface StageAblationRequest {
  /** The draw or dispatch to time. */
  command: number;
  /** The pipeline bound at it, and the stage of it to measure. */
  pipeline: number;
  stage: ShaderStage;
  entryPoint: string;
  /**
   * The stage's SPIR-V as the capture holds it. For a D3D12 stage, the SPIR-V its HLSL compiles to
   * (compileHlslForDebugging), which is what the flame graph's functions and lines are numbered by.
   */
  spirv: Uint8Array;
  /** D3D12: the stage's DXIL container, which is what is measured. */
  dxil?: Uint8Array;
  /** Timed rounds (default 5). */
  rounds?: number;
  /** Draws per timed span; default from `drawMs`, else 8. */
  repeat?: number;
  /** What the replay timed the draw at, when known (Measure draws): sets the repeat so a span is about two milliseconds. */
  drawMs?: number | null;
  /** Functions and lines measured, the costliest by the model first, and textures, the most sampled first. */
  functions?: number;
  lines?: number;
  textures?: number;
}

/** Draws per timed span: enough for about two milliseconds of GPU time, between 4 and 64. */
export function ablationRepeat(drawMs: number | null | undefined): number {
  if (!drawMs || drawMs <= 0) return 8;
  return Math.min(64, Math.max(4, Math.ceil(2 / drawMs)));
}

/**
 * A D3D12 stage: the variants are of its DXIL (renderer/d3d12/dxil_ablate.ts), each assembled and
 * validated by dxc before the driver sees it, and `dxinsp_replay --ablate` times them. The parts
 * are named by the HLSL's functions and lines, and the flame graph's frames by the ids those have
 * in the SPIR-V the same HLSL compiles to, so the two are matched by name.
 */
async function measureDxilStageByAblation(run: (analysis: ReplayAnalysis) => Promise<ReplayRun>, req: StageAblationRequest): Promise<ShaderAblation> {
  const text = await disassembleDxil(req.dxil!);
  if (!text.ok) throw new Error(`The stage's DXIL could not be disassembled: ${text.text}`);
  const plan = planDxilAblation(text.text, req.stage, req.entryPoint, { functions: req.functions, lines: req.lines, textures: req.textures });

  // dxc wraps the entry point the source wrote: the SPIR-V entry point calls `src.<name>`, which
  // is the function the lines are in.
  const model = analyzeSpirvCached(req.spirv);
  const functionIds = new Map<string, number>();
  for (const f of model?.functions ?? []) {
    functionIds.set(f.name, f.id);
    if (f.name.startsWith("src.")) functionIds.set(f.name.slice(4), f.id);
  }

  const assembled = await Promise.all(plan.variants.map((v) => assembleDxil(v.text)));
  const kept = new Map<number, number>();   // index in the plan -> index among the variants that assembled
  const codes: Uint8Array[] = [];
  assembled.forEach((a, i) => {
    const { text: _text, edits: _edits, upstream: _upstream, ...part } = plan.variants[i];
    if (a.ok) {
      kept.set(i, codes.length);
      codes.push(a.container);
    } else {
      plan.skipped.push({ ...part, reason: `the variant does not assemble or validate: ${a.error}` });
    }
  });
  const variants = plan.variants.filter((_, i) => kept.has(i)).map((v) => {
    const { text: _text, ...part } = v;
    const functionId = part.functionName !== undefined ? functionIds.get(part.functionName) : undefined;
    return { ...part, ...(functionId !== undefined ? { functionId } : {}), spirv: new Uint8Array(0),
             upstream: v.upstream.filter((k) => kept.has(k)).map((k) => kept.get(k)!) };
  });
  if (!variants.length) {
    const reasons = plan.skipped.map((s) => `${s.name}: ${s.reason}`).slice(0, 8).join("; ");
    throw new Error(`Nothing in the ${req.stage} stage can be measured${reasons ? ` (${reasons})` : ""}.`);
  }
  const repeat = req.repeat ?? ablationRepeat(req.drawMs);
  const request = encodeAblationRequest([{ command: req.command, stage: req.stage, repeat, variants: variants.map((v, i) => ({ name: v.name, spirv: codes[i] })) }],
    Math.max(1, Math.min(32, req.rounds ?? 5)));
  const result = await run({ kind: "ablate", request });
  if (!result.data) throw new Error(`The replay could not time the variants: ${result.error ?? "no data"}`);
  const file = parseAblationResult(result.data);
  const target = file.targets.find((t) => t.command === req.command);
  if (!target) throw new Error("The replay did not answer for the command.");
  if (!target.baseline.measured) throw new Error(`The replay did not time the command: ${target.note ?? "no timings"}.`);
  const measured = measuredAblation(req.pipeline, req.stage, req.entryPoint, { variants, skipped: plan.skipped }, target, file.device);
  measured.repeat = repeat;
  return measured;
}

/** Measures one stage; throws with the reason when nothing can be measured. */
export async function measureStageByAblation(run: (analysis: ReplayAnalysis) => Promise<ReplayRun>, req: StageAblationRequest): Promise<ShaderAblation> {
  if (req.dxil) return measureDxilStageByAblation(run, req);
  const analysis = analyzeSpirvCached(req.spirv);
  if (!analysis) throw new Error("The stage's SPIR-V could not be analyzed.");
  const plan = planAblation(req.spirv, req.stage, req.entryPoint, analysis, { functions: req.functions, lines: req.lines, textures: req.textures });
  const notes: string[] = [];
  // Variants the validator rejects are left out; a module that is invalid to begin with cannot say anything about its variants.
  const original = await validateSpirv(req.spirv);
  if (original === undefined) {
    notes.push("spirv-val was not found (Vulkan SDK), so the variants were not validated before the driver compiled them.");
  } else if (original !== null) {
    notes.push(`The captured module itself does not pass spirv-val (${original}), so its variants were not validated.`);
  } else {
    const checked = await Promise.all(plan.variants.map(async (v) => ({ v, verdict: await validateSpirv(v.spirv) })));
    const kept = new Map<number, number>();   // index in the plan -> index among the valid variants
    checked.forEach((c, i) => {
      if (!c.verdict) kept.set(i, kept.size);
      else {
        const { spirv: _spirv, edits: _edits, upstream: _upstream, ...part } = c.v;
        plan.skipped.push({ ...part, reason: `the variant does not validate: ${c.verdict}` });
      }
    });
    plan.variants = checked.filter((_, i) => kept.has(i))
      .map((c) => ({ ...c.v, upstream: c.v.upstream.filter((k) => kept.has(k)).map((k) => kept.get(k)!) }));
  }
  if (!plan.variants.length) {
    const reasons = plan.skipped.map((s) => `${s.name}: ${s.reason}`).slice(0, 8).join("; ");
    throw new Error(`Nothing in the ${req.stage} stage can be measured${reasons ? ` (${reasons})` : ""}.`);
  }
  const repeat = req.repeat ?? ablationRepeat(req.drawMs);
  const request = encodeAblationRequest([{ command: req.command, stage: req.stage, repeat, variants: plan.variants.map((v) => ({ name: v.name, spirv: v.spirv })) }],
    Math.max(1, Math.min(32, req.rounds ?? 5)));
  const result = await run({ kind: "ablate", request });
  if (!result.data) throw new Error(`The replay could not time the variants: ${result.error ?? "no data"}`);
  const file = parseAblationResult(result.data);
  const target = file.targets.find((t) => t.command === req.command);
  if (!target) throw new Error("The replay did not answer for the command.");
  if (!target.baseline.measured) throw new Error(`The replay did not time the command: ${target.note ?? "no timings"}.`);
  const measured = measuredAblation(req.pipeline, req.stage, req.entryPoint, plan, target, file.device);
  measured.repeat = repeat;
  if (notes.length) measured.notes = [...(measured.notes ?? []), ...notes];
  return measured;
}
