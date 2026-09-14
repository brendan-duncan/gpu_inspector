// debug_shader: the shader debugger for an agent. One invocation of a draw's vertex or fragment
// shader, or of a dispatch's compute shader, run in the interpreter of whichever language it is in
// — SPIR-V for a Vulkan capture, Metal Shading Language for a Metal one — on the capture's inputs
// (renderer/shader_debug_setup.ts), with the values every source line computed in the order they
// ran, the first NaN or infinity, the outputs, and how they compare with what the GPU produced.
import { NO_REPLAY_TOOL, findReplayTool, replayServers } from "../main/replay.js";
import { findShaderSources } from "../main/shader_sources.js";
import { decompileForDebugging } from "../main/shader_tools.js";
import { drawState } from "../renderer/draw_state.js";
import { parseMeshFile, type MeshOutput } from "../renderer/mesh_output.js";
import { DebugController } from "../renderer/shader_debugger.js";
import { compareWithOriginal, coveredPixel, prepareDebugSession, sameValue, type DebugContext, type DebugTarget } from "../renderer/shader_debug_setup.js";
import { interpretedMeshOutput, metalRasterState } from "../renderer/metal/shader_debug.js";
import { nonFinite, Pointer, scalars } from "../renderer/debug/values.js";
import { SpirvProgram } from "../renderer/spirv/program.js";
import { sourceLineMap } from "../renderer/vulkan/spirv_debug.js";
import type { Capture, CaptureStore } from "./capture_store.js";
import { vertexInputs } from "./command_tools.js";
import { CAPTURE_PARAM, boolArg, enumArg, intArg, jsonResult, optionalInt, requireInt, schema, stringArg, tidy } from "./describe.js";
import { checkoutRoots, installedLayerDirs } from "./live_session.js";
import { searchPaths } from "./search_paths.js";
import type { ToolDefinition } from "./stdio_server.js";

const MAX_TRACE = 400;
const MAX_VALUES_PER_LINE = 24;

function meshOutputs(c: Capture): (command: number) => Promise<MeshOutput> {
  const cache = new Map<number, MeshOutput>();
  return async (command) => {
    const hit = cache.get(command);
    if (hit) return hit;
    const tool = findReplayTool(checkoutRoots(), installedLayerDirs());
    if (!tool) throw new Error(`a fragment's inputs come from replaying the draw's vertex shader, and ${NO_REPLAY_TOOL}`);
    const run = await replayServers.run(tool, c.path, { kind: "mesh", commands: [command] });
    if (!run.data) throw new Error(`the replay could not capture the draw's vertex outputs: ${run.error ?? "no data"}`);
    const m = parseMeshFile(run.data).draws.find((d) => d.command === command);
    if (!m) throw new Error("the replay did not reach the draw");
    cache.set(command, m);
    return m;
  };
}

export function debugTools(store: CaptureStore): ToolDefinition[] {
  return [
    {
      name: "debug_shader",
      description: "Runs one shader invocation of a capture in GPU Inspector's own interpreter, the way RenderDoc's shader debugger " +
        "does, for \"why is this pixel black / this vertex in the wrong place / this value NaN\": a Vulkan capture's SPIR-V or a Metal " +
        "capture's Metal Shading Language. A draw's vertex (its attributes decoded from the captured buffers), a draw's fragment at a " +
        "pixel, or a dispatch's compute invocation, on the resources the command had bound. A Vulkan fragment's inputs are rasterized " +
        "from the replayed vertex shader outputs, so that needs vkinsp_replay; a Metal fragment's come from running the draw's own vertex " +
        "shader in the interpreter, so it needs nothing. Gives the outputs, the render target's pixel or the replay's vertex outputs to " +
        "compare with, the values every source line computed in execution order (SPIR-V instructions when the shader has no line " +
        "information), the first NaN or infinity, and what the interpreter could not do faithfully. `line` keeps only that line's values. " +
        "A Metal library the application loaded precompiled has no source, and says so.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        command: { type: "integer", minimum: 0, description: "The draw or dispatch command's index." },
        stage: { type: "string", enum: ["vertex", "fragment", "compute"], description: "Default: compute for a dispatch, fragment for a draw." },
        vertex: { type: "integer", minimum: 0, description: "Vertex: the vertex, in the order the draw read them (an indexed draw's index order). Default 0." },
        instance: { type: "integer", minimum: 0, description: "Vertex: the instance. Default 0." },
        x: { type: "integer", minimum: 0, description: "Fragment: the pixel's column. Default: a pixel the draw covers." },
        y: { type: "integer", minimum: 0, description: "Fragment: the pixel's row." },
        invocation: { type: "array", items: { type: "integer", minimum: 0 }, minItems: 3, maxItems: 3, description: "Compute: gl_GlobalInvocationID (Metal: thread_position_in_grid). Default [0, 0, 0]." },
        line: { type: "integer", minimum: 1, description: "Only the values of this source line (each time it ran)." },
        trace: { type: "boolean", description: "Include the line-by-line values (default true)." },
        decompiled: { type: "boolean", description: "Vulkan: step GLSL that spirv-cross decompiles from the SPIR-V and glslang compiles back " +
          "with line information, for a shader built without debug information (lines instead of instructions). It is not the module the " +
          "GPU ran, so the original runs too and `original` says whether they agree. Needs the Vulkan SDK's spirv-cross and glslangValidator. Default false." },
      }, ["command"]),
      readOnly: true,
      handler: async (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const command = requireInt(args, "command");
        const cmd = c.data.commands[command];
        if (!cmd) throw new Error(`The capture has no command ${command}.`);
        const isDispatch = c.data.sets.DISPATCH.has(cmd.method);
        if (!isDispatch && !c.data.sets.DRAW.has(cmd.method)) throw new Error(`Command ${command} (${cmd.method}) is neither a draw nor a dispatch.`);
        const stage = enumArg(args, "stage", ["vertex", "fragment", "compute"] as const, isDispatch ? "compute" : "fragment");
        const state = drawState(c.data, c.db, cmd);
        const inputNames = new Map<number, string>();
        for (const v of vertexInputs(c, state)) if (v.location !== undefined && v.name) inputNames.set(v.location, v.name);
        // A Metal capture needs no replay: its fragment inputs come from interpreting the draw's
        // own vertex shader, so the replay is only wired up for a Vulkan one.
        const metal = c.data.api === "metal";
        const decompiled = !metal && boolArg(args, "decompiled", false);
        const ctx: DebugContext = {
          data: c.data, db: c.db, inputNames,
          meshOutput: metal ? undefined : meshOutputs(c),
          translate: decompiled ? async (bytes, source) => {
            const r = await decompileForDebugging(bytes, source.stage, source.entryPoint);
            if (!r.ok || !r.spirv) throw new Error(`the SPIR-V could not be decompiled for debugging: ${r.log.trim() || `${r.tool} failed`}`);
            return r.spirv;
          } : undefined,
        };

        let target: DebugTarget;
        if (stage === "compute") {
          const inv = Array.isArray(args.invocation) ? args.invocation.map((v) => Math.max(0, Math.floor(Number(v) || 0))) : [0, 0, 0];
          target = { stage, command, invocation: [inv[0] ?? 0, inv[1] ?? 0, inv[2] ?? 0] };
        } else if (stage === "vertex") {
          target = { stage, command, vertex: intArg(args, "vertex", 0, 0), instance: intArg(args, "instance", 0, 0) };
        } else {
          let x = optionalInt(args, "x"), y = optionalInt(args, "y");
          if (x === undefined || y === undefined) {
            let mesh: MeshOutput;
            try {
              // Metal has no replay: the vertex shader is interpreted to find a covered pixel.
              mesh = metal ? await interpretedMeshOutput(ctx, cmd, state) : await ctx.meshOutput!(command);
            } catch (e) {
              return jsonResult({ capture: c.id, command, stage, note: `Cannot debug the fragment: ${(e as Error).message}` });
            }
            const pixel = mesh.measured ? coveredPixel(state, mesh, metal ? metalRasterState(ctx, cmd, state) : undefined) : null;
            if (!pixel) return jsonResult({ capture: c.id, command, stage, note: `No pixel to debug: ${mesh.measured ? "no triangle of the draw is visible in its viewport" : mesh.note ?? "the vertex outputs were not captured"}. Give x and y.` });
            x = pixel.x;
            y = pixel.y;
          }
          target = { stage, command, x, y };
        }

        let session;
        try {
          session = await prepareDebugSession(ctx, target);
        } catch (e) {
          return jsonResult({ capture: c.id, command, stage, note: `Cannot debug: ${(e as Error).message}` });
        }
        const program = session.program;
        // SPIR-V with line information but no text: the source roots may have the files. MSL is
        // the capture's own text, so there is nothing to look for.
        if (program instanceof SpirvProgram) {
          const missing = program.files.filter((f) => f.text === null && f.name);
          if (missing.length) {
            const texts = findShaderSources(missing.map((f) => f.name), searchPaths("sourceRoots").dirs);
            for (const f of missing) if (typeof texts[f.name] === "string") f.text = texts[f.name];
          }
        }
        const maps = program.files.map((f) => (f.text == null ? null : sourceLineMap(f.text)));
        const sourceOf = (file: number, line: number): string | undefined => {
          const map = maps[file];
          const phys = map?.physicalOf.get(line);
          return map && phys !== undefined ? map.lines[phys].trim() : undefined;
        };

        const ctl = new DebugController(session);
        const onlyLine = optionalInt(args, "line");
        const wantTrace = boolArg(args, "trace", true);
        const trace: (Record<string, unknown> | string)[] = [];
        let traceTruncated = false;
        let firstNonFinite: Record<string, unknown> | undefined;
        while (!ctl.finished) {
          ctl.advance(ctl.mode === "source" ? "into" : "instruction", 1_000_000);
          const last = ctl.lastLine;
          if (!last.results.length) continue;
          const inst = last.results[last.results.length - 1].inst;
          const loc = ctl.location(inst);
          for (const r of last.results) {
            if (!firstNonFinite && nonFinite(r.value)) {
              const l = ctl.location(r.inst);
              firstNonFinite = { line: l?.line, instruction: r.inst.index, name: program.nameOf(r.id), value: program.valueText(program.resultType(r), r.value), source: l ? sourceOf(l.file, l.line) : undefined };
            }
          }
          if (!wantTrace || (onlyLine !== undefined && loc?.line !== onlyLine)) continue;
          if (trace.length >= MAX_TRACE) {
            traceTruncated = true;
            continue;
          }
          // Pointers (variables, access chains) are where values go, not values.
          const values = last.results.filter((r) => !(r.value instanceof Pointer));
          if (!values.length) continue;
          const named = values.filter((r) => !program.resultTemporary(r));
          const shown = (named.length ? named : values).slice(-MAX_VALUES_PER_LINE);
          const texts = shown.map((r) => `${program.nameOf(r.id)} = ${program.valueText(program.resultType(r), r.value)}`);
          if (ctl.mode === "instruction") {
            // Without lines, an entry per instruction: "ordinal: name = value" (get_shader's disassembly has the instructions).
            trace.push(`${inst.index}: ${texts.join("; ")}`);
          } else {
            trace.push({
              line: loc?.line,
              file: program.files.length > 1 && loc ? program.files[loc.file]?.name : undefined,
              source: loc ? sourceOf(loc.file, loc.line) : undefined,
              values: texts,
              depth: ctl.invocation.depth > 1 ? ctl.invocation.depth : undefined,
            });
          }
          // A partial pass over the same line by "into" (a loop) is fine: each entry is one visit.
          ctl.lastLine.results.length = 0;
        }

        const inv = ctl.invocation;
        let original: Record<string, unknown> | undefined;
        if (session.original) {
          try {
            const run = session.original();
            run.run();
            const c = compareWithOriginal(inv, run.invocation, stage);
            original = {
              matches: c.matches, status: c.status.original, error: c.status.error,
              differences: c.values.filter((v) => !v.matches).map((v) => {
                if ((v.translated?.length ?? 0) <= 16 && (v.original?.length ?? 0) <= 16) return { name: v.label, translated: v.translated?.map(tidy), original: v.original?.map(tidy) };
                // A buffer: just where it first differs.
                const at = (v.original ?? []).findIndex((x, i) => !sameValue(x, v.translated?.[i]));
                const i = at < 0 ? Math.min(v.original?.length ?? 0, v.translated?.length ?? 0) : at;
                return { name: v.label, firstDifference: i, translated: v.translated?.[i], original: v.original?.[i] };
              }),
            };
            if (!c.matches) original.note = "The translation does not compute what the original does: debug without `decompiled`.";
          } catch (e) {
            original = { error: `the original could not be run to compare with: ${(e as Error).message}` };
          }
        }
        const outputs = inv.outputs().map((o) => ({ name: o.name, location: o.location, builtin: o.builtin, type: program.typeName(o.type), value: program.valueText(o.type, o.value, 64) }));
        let compare: Record<string, unknown> | undefined;
        if (inv.status === "returned" && session.targetPixel) {
          compare = {
            renderTargetAfterPass: session.targetPixel.value.map(tidy), format: session.targetPixel.format,
            note: "The pixel after the whole pass: blending and later draws come between. get_pixel_history has the value after this draw.",
          };
        } else if (inv.status === "returned" && session.replayedOutputs) {
          const outs = inv.outputs();
          compare = {
            replayedVertexOutputs: session.replayedOutputs.map((r) => {
              const mine = r.builtin === "Position"
                ? outs.find((o) => o.builtin === 0)?.value ?? (outs.find((o) => Array.isArray(o.value) && Array.isArray(o.value[0]))?.value as unknown[] | undefined)?.[0]
                : outs.find((o) => o.location === r.location)?.value;
              const values = scalars(mine as never);
              const diff = values.length ? Math.max(...r.value.map((x, i) => Math.abs(x - (values[i] ?? NaN)) / Math.max(1, Math.abs(x)))) : NaN;
              return { name: r.name, gpu: r.value.map(tidy), matches: diff < 1e-4 };
            }),
          };
        }
        return jsonResult({
          capture: c.id, command, method: cmd.method, stage, entryPoint: session.stage.entryPoint,
          invocation: session.description, notes: session.notes.length ? session.notes : undefined,
          status: inv.status, error: inv.error || undefined, instructions: inv.steps,
          steppedBy: decompiled ? "source line of GLSL decompiled from the SPIR-V (spirv-cross, recompiled by glslang)"
            : ctl.mode === "source" ? `source line (${program.languageName})` : "SPIR-V instruction (the shader has no line information)",
          outputs, compare, original, firstNonFinite,
          warnings: inv.warnings.size ? [...inv.warnings] : undefined,
          trace: wantTrace ? trace : undefined, traceTruncated: traceTruncated || undefined,
        });
      },
    },
  ];
}
