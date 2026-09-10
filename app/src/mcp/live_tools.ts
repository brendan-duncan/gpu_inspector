// The MCP server's live tools: launching an application with GPU Inspector's capture library in it
// (or attaching to one already running), watching its frame statistics and log, reading its live
// objects, images and descriptor sets, capturing frames into .gpucap files that the capture tools
// then read, and replacing a pipeline's shader while the application runs.
import { listPackages } from "../main/android.js";
import { compileShader } from "../main/shader_tools.js";
import { fetchBlob } from "../renderer/capture_file.js";
import { REFRESH_SOURCE_NOTE } from "../renderer/capture_statistics.js";
import { pipelineStages } from "../renderer/shader_cache.js";
import { requestStacks } from "../renderer/stack_requests.js";
import { reflectSpirv } from "../renderer/vulkan/spirv_reflect.js";
import type { ObjectDatabase } from "../renderer/vulkan/object_database.js";
import { imageOfView } from "../renderer/vulkan/pass_info.js";
import { decodeTexels, isFormatSupported } from "../renderer/vulkan/texture_decode.js";
import { isObject, num, refId, str } from "../renderer/vulkan/vulkan_object.js";
import type { CaptureDescriptor, CaptureDescriptorBinding, ShaderLanguage, StackFrame } from "../shared/protocol.js";
import type { CaptureStore } from "./capture_store.js";
import { objectDetail } from "./command_tools.js";
import {
  PAGE_PARAMS, boolArg, clip, compact, enumArg, intArg, jsonResult, numberArg, optionalInt, page, refText, regexArg, requireInt, requireString, round,
  schema, stackLines, stringArg,
} from "./describe.js";
import { capturesDir, type LiveSession, type SessionManager } from "./live_session.js";
import { IMAGE_PARAMS, texelAnswer } from "./resource_tools.js";
import { symbolizeOnHost } from "./search_paths.js";
import type { ToolDefinition } from "./stdio_server.js";
import { captureSummary } from "./tools.js";

const SESSION_PARAM = { type: "string", description: "A live session's id (\"app-1\"). Defaults to the session started most recently." };
const LANGUAGES = ["glsl", "hlsl", "spirv-asm"] as const;

const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

/** The GPU the application renders with, as the capture library reported it. */
function deviceName(db: ObjectDatabase): string | undefined {
  for (const o of db.objectsByType.get("VkPhysicalDevice")?.values() ?? []) {
    const summary = o.summary(db).trim();
    if (summary) return summary;
  }
  const metal = db.objectsByType.get("MTLDevice")?.values().next().value;
  return metal ? metal.name : undefined;
}

function sessionStatus(s: LiveSession): Record<string, unknown> {
  const db = s.database;
  const byType = [...db.objectsByType].filter(([, objects]) => objects.size).sort((a, b) => b[1].size - a[1].size).slice(0, 12);
  const last = s.frameStats.at(-1)?.msg;
  const [errors, warnings] = db.validationCounts;
  return {
    session: s.id, name: s.name, state: s.state, detail: s.detail || undefined, launched: s.launched, pid: s.pid ?? undefined, port: s.port,
    exitCode: s.exitCode ?? undefined, api: s.api ?? undefined, device: deviceName(db), uptimeSeconds: round((Date.now() - s.startedAt) / 1000),
    frame: last ? {
      index: last.frame, frameMs: round(last.frameTimeMs), fps: last.frameTimeMs > 0 ? round(1000 / last.frameTimeMs) : undefined,
      submitMs: round(last.submitMs), refreshMs: round(last.refreshMs) || undefined, presentMode: last.presentMode || undefined,
      droppedTotal: last.droppedTotal || undefined,
    } : undefined,
    objects: { live: db.allObjects.size, byType: Object.fromEntries(byType.map(([type, objects]) => [type, objects.size])) },
    memoryBytes: {
      deviceMemory: db.memory.device || undefined, buffers: db.memory.buffers || undefined, images: db.memory.images || undefined,
      reportedByDriver: db.memory.reported || undefined,
    },
    validation: { errors, warnings, total: db.validation.length },
    leakedObjects: db.leakCount || undefined,
    recentLog: s.log.slice(-15),
    note: s.state === "connected" && !last ? "Connected, but no frame has been reported yet: the application may not be rendering." : undefined,
  };
}

/** The temporary source file the compilers were given (shader_tools.ts), however a compiler writes its path. */
const TEMP_SOURCE = /\S*vkinsp_\d+_\d+_\d+\.(glsl|hlsl|spvasm)/g;

/** A compiler's output with the temporary file's path read as "source"; glslangValidator prints nothing else on success. */
function compilerLog(log: string): string {
  return log.split(/\r?\n/).filter((line) => line.trim().replace(TEMP_SOURCE, "") !== "").join("\n").replace(TEMP_SOURCE, "source").trim();
}

/** The stage flag of a pipeline's stage by the stage's name ("fragment"). */
function stageOf(s: LiveSession, pipelineId: number, stage: string): { flag: string; entryPoint: string; source: ReturnType<typeof pipelineStages>[number] } {
  const pipeline = s.database.getObject(pipelineId);
  if (!pipeline || pipeline.type !== "VkPipeline") throw new Error(`${refText(s.database, pipelineId) ?? `Object ${pipelineId}`} is not a live VkPipeline of ${s.id}.`);
  const stages = pipelineStages(pipeline, s.database);
  const source = stages.find((x) => x.stage === stage.toLowerCase());
  if (!source) throw new Error(`${refText(s.database, pipelineId)} has no ${stage} stage with code (it has: ${stages.map((x) => x.stage).join(", ") || "none"}).`);
  return { flag: source.stageFlag, entryPoint: source.entryPoint, source };
}

/** A descriptor a live set holds, as get_live_descriptor_set lists it. */
function liveDescriptor(db: ObjectDatabase, d: CaptureDescriptor): Record<string, unknown> {
  if (d.buffer !== undefined) return { buffer: refText(db, d.buffer), offset: d.offset, range: d.range };
  if (d.imageView !== undefined || d.sampler !== undefined) {
    return {
      imageView: refText(db, d.imageView), image: refText(db, imageOfView(db, refId(d.imageView ?? undefined))), layout: d.imageLayout,
      sampler: refText(db, d.sampler), immutableSampler: d.immutable || undefined,
    };
  }
  return compact(d, db) as Record<string, unknown>;
}

/** A note when every render pass that draws into the image discards it: its contents between frames are then undefined. */
function discardNote(db: ObjectDatabase, imageId: number): string | undefined {
  const views = new Set<number>();
  for (const v of db.objectsByType.get("VkImageView")?.values() ?? []) if (refId(v.descriptor?.image) === imageId) views.add(v.id);
  let stored = 0;
  let discarded = 0;
  for (const fb of db.objectsByType.get("VkFramebuffer")?.values() ?? []) {
    const d = fb.descriptor;
    const attachments = d && Array.isArray(d.pAttachments) ? d.pAttachments : [];
    const pass = db.getObject(refId(d?.renderPass));
    const descriptions = pass?.descriptor && Array.isArray(pass.descriptor.pAttachments) ? pass.descriptor.pAttachments : [];
    attachments.forEach((a, i) => {
      const view = refId(a);
      const description = descriptions[i];
      if (view === null || !views.has(view) || !isObject(description)) return;
      if (str(description.storeOp).endsWith("DONT_CARE")) discarded++; else stored++;
    });
  }
  return discarded && !stored
    ? "Every render pass that draws into this image discards it (storeOp DONT_CARE), so what it holds between frames is undefined: capture_frames reads attachments at the end of each pass instead."
    : undefined;
}

function requireConnected(s: LiveSession): void {
  if (!s.connected) throw new Error(`${s.id} is not connected (${s.state}${s.detail ? `: ${s.detail}` : ""}).`);
}

export function liveTools(sessions: SessionManager, store: CaptureStore): ToolDefinition[] {
  return [
    {
      name: "launch_app",
      description: "Launch an application with GPU Inspector's capture library in it (the Vulkan layer; on macOS the Metal " +
        "library) and connect to it, so its frames can be captured and its frame statistics watched while it runs. Returns " +
        "the session's status: the device, live objects, the frame rate once frames arrive, and the recent log (the layer's " +
        "own output is in it, which is where to look when it does not connect). The application runs until stop_app or " +
        "until this server exits.",
      inputSchema: schema({
        exe: { type: "string", description: "The executable (on macOS an .app bundle works too)." },
        args: { type: "string", description: "Command line arguments, quoted as in a shell." },
        cwd: { type: "string", description: "Working directory (default the executable's directory)." },
        env: { type: "object", additionalProperties: { type: "string" }, description: "Extra environment variables." },
        validation: { type: "boolean", description: "Also enable the Khronos validation layer (Vulkan SDK) or Metal's validation, so validation messages reach the captures (default false)." },
        syncValidation: { type: "boolean", description: "With validation: synchronization validation too (default false)." },
        stacktraces: { type: "boolean", description: "Record a stack at every object creation (default true)." },
        recordAlways: { type: "boolean", description: "Record every command buffer as it is built, so buffers recorded once and reused appear in captures (default false; costs CPU time)." },
        port: { type: "integer", minimum: 1, maximum: 65535, description: "Port for the capture library (default 47531, or the next free one)." },
        layerDir: { type: "string", description: "The directory holding VK_LAYER_INSPECTOR_capture.json, when neither a GPU Inspector checkout nor an installed GPU Inspector provides it." },
        waitSeconds: { type: "number", minimum: 1, maximum: 600, description: "How long to wait for the capture library to connect (default 60)." },
      }, ["exe"]),
      handler: async (args) => {
        const env = args.env && typeof args.env === "object" ? Object.fromEntries(Object.entries(args.env as Record<string, unknown>).map(([k, v]) => [k, String(v)])) : undefined;
        const s = await sessions.launch({
          exe: requireString(args, "exe"), args: stringArg(args, "args"), cwd: stringArg(args, "cwd"), env,
          validation: boolArg(args, "validation", false), syncValidation: boolArg(args, "syncValidation", false),
          stacktraces: boolArg(args, "stacktraces", true), recordAlways: boolArg(args, "recordAlways", false),
          port: optionalInt(args, "port"), layerDir: stringArg(args, "layerDir"),
        }, (numberArg(args, "waitSeconds") ?? 60) * 1000);
        const result = jsonResult({
          ...sessionStatus(s),
          problem: s.connected ? undefined : "The capture library did not connect. recentLog (and get_session_log) has the application's and the layer's output: a crash, an application that does not use Vulkan, or a layer the loader did not load.",
        });
        if (!s.connected) result.isError = true;
        return result;
      },
    },
    {
      name: "attach_app",
      description: "Connect to an application whose capture library already listens on a port: one started with GPU " +
        "Inspector's implicit layer (VKINSP_ENABLE=1 and VKINSP_PORT), by hand, or by GPU Inspector itself. The capture " +
        "library serves one client at a time, so attaching disconnects GPU Inspector from that application if it was connected.",
      inputSchema: schema({
        port: { type: "integer", minimum: 1, maximum: 65535, description: "The port (default 47531)." },
        waitSeconds: { type: "number", minimum: 1, maximum: 600, description: "How long to keep trying (default 10)." },
      }),
      handler: async (args) => {
        const s = await sessions.attach(intArg(args, "port", 47531, 1, 65535), (numberArg(args, "waitSeconds") ?? 10) * 1000);
        return jsonResult(sessionStatus(s));
      },
    },
    {
      name: "list_android_devices",
      description: "The Android devices adb sees (serial, state, model, Android API level, ABI) and whether GPU Inspector's " +
        "Android layer is available; with `device`, also the third-party packages installed on that device, for " +
        "launch_android_app.",
      inputSchema: schema({ device: { type: "string", description: "A device's serial: also list the packages installed on it." } }),
      readOnly: true,
      handler: async (args) => {
        const { adb, devices, layer } = await sessions.androidDevices();
        const device = stringArg(args, "device");
        return jsonResult({
          adb: adb ?? "not found: install the Android SDK platform-tools, or set ANDROID_HOME or INSPECTOR_ADB",
          devices: devices.map((d) => ({ serial: d.serial, state: d.state, model: d.model || undefined, sdk: d.sdk || undefined, abi: d.abi || undefined })),
          layer: layer ?? "not found: build it with tools/build_android.py (it needs the Android NDK), install GPU Inspector, or set INSPECTOR_ANDROID_LAYER_DIR",
          packages: adb && device ? await listPackages(adb, device) : undefined,
          note: devices.some((d) => d.state === "unauthorized") ? "An unauthorized device is waiting for its USB debugging prompt to be accepted." : undefined,
        });
      },
    },
    {
      name: "launch_android_app",
      description: "Launch an Android application with GPU Inspector's Vulkan layer and connect to it, for the same live tools " +
        "as launch_app (get_live_frame_stats, capture_frames, read_live_image, replace_shader...). The layer is installed " +
        "on the device (the layer package on Android 10+, else copied into the application's data), enabled for the " +
        "package through Android's GPU debug layer settings, and reached through an adb port forward. The application must " +
        "be debuggable (a development build) unless the device is rooted. Logcat's layer output and crashes go to " +
        "get_session_log; stop_app ends the application and turns the debug layer settings off.",
      inputSchema: schema({
        package: { type: "string", description: "The package name (list_android_devices lists the installed ones)." },
        device: { type: "string", description: "The device's serial (default the only connected device)." },
        activity: { type: "string", description: "The activity to start (default the package's launcher activity)." },
        stacktraces: { type: "boolean", description: "Record a stack at every object creation (default true)." },
        recordAlways: { type: "boolean", description: "Record every command buffer as it is built, for applications that reuse command buffers recorded once (default false)." },
        port: { type: "integer", minimum: 1, maximum: 65535, description: "Host port for the forward (default 47531, or the next free one)." },
        waitSeconds: { type: "number", minimum: 1, maximum: 600, description: "How long to wait for the layer to connect once the application has started (default 60)." },
      }, ["package"]),
      handler: async (args) => {
        const s = await sessions.launchAndroid({
          package: requireString(args, "package"), device: stringArg(args, "device"), activity: stringArg(args, "activity"),
          stacktraces: boolArg(args, "stacktraces", true), recordAlways: boolArg(args, "recordAlways", false), port: optionalInt(args, "port"),
        }, (numberArg(args, "waitSeconds") ?? 60) * 1000);
        const result = jsonResult({
          ...sessionStatus(s),
          problem: s.connected ? undefined : s.state === "error"
            ? `The launch failed on the device: ${s.detail}`
            : "The layer did not connect. recentLog (and get_session_log) has logcat's layer output and crashes: an application that is not debuggable or does not use Vulkan, or a device that is asleep.",
        });
        if (!s.connected) result.isError = true;
        return result;
      },
    },
    {
      name: "list_sessions",
      description: "List the live sessions this server has launched or attached to, with their state.",
      inputSchema: schema({}),
      readOnly: true,
      handler: () => jsonResult({
        sessions: sessions.list().map((s) => ({
          session: s.id, name: s.name, state: s.state, pid: s.pid ?? undefined, port: s.port, api: s.api ?? undefined,
          frame: s.frameStats.at(-1)?.msg.frame,
        })),
        capturesDirectory: capturesDir(),
      }),
    },
    {
      name: "get_session_status",
      description: "A live session's state: whether it is connected, the process, the device, the last frame report (frame " +
        "time, submit time, refresh period, dropped frames), live objects by type, memory, validation counts, and the recent log.",
      inputSchema: schema({ session: SESSION_PARAM }),
      readOnly: true,
      handler: (args) => jsonResult(sessionStatus(sessions.get(stringArg(args, "session")))),
    },
    {
      name: "get_live_frame_stats",
      description: "Watch a running application's frame reports for a few seconds, without capturing: average, shortest and " +
        "longest frame time, frame rate, CPU time inside queue submission, the display refresh period and where it came " +
        "from, dropped frames, and a verdict on whether the frame meets the refresh or is bound by submission. GPU time is not " +
        "measured live: capture_frames with profilePasses measures each pass.",
      inputSchema: schema({
        session: SESSION_PARAM,
        seconds: { type: "number", minimum: 0.3, maximum: 30, description: "How long to watch (default 2)." },
      }),
      readOnly: true,
      handler: async (args) => {
        const s = sessions.get(stringArg(args, "session"));
        if (!s.connected) throw new Error(`${s.id} is not connected (${s.state}).`);
        const seconds = Math.min(30, Math.max(0.3, numberArg(args, "seconds") ?? 2));
        const since = Date.now();
        await sleep(seconds * 1000);
        const reports = s.frameStats.filter((f) => f.at >= since).map((f) => f.msg);
        if (!reports.length) {
          return jsonResult({ session: s.id, seconds, note: "No frame reports arrived: the application is not presenting frames (minimized, paused, loading, or not rendering)." });
        }
        let frames = 0;
        let weightedMs = 0;
        let weightedSubmit = 0;
        let min = Infinity;
        let max = 0;
        let dropped = 0;
        for (const m of reports) {
          const n = Math.max(1, m.frames ?? 1);
          frames += n;
          weightedMs += m.frameTimeMs * n;
          weightedSubmit += (m.submitMs ?? 0) * n;
          min = Math.min(min, m.minMs ?? m.frameTimeMs);
          max = Math.max(max, m.maxMs ?? m.frameTimeMs);
          dropped += m.dropped ?? 0;
        }
        const last = reports[reports.length - 1];
        const frameMs = weightedMs / frames;
        const submitMs = weightedSubmit / frames;
        const refresh = last.refreshMs ?? 0;
        let verdict: string;
        if (refresh > 0 && frameMs <= refresh * 1.1) verdict = "Meeting the display refresh: the frame waits for vsync, so there is headroom.";
        else if (frameMs > 0 && submitMs / frameMs > 0.8) verdict = "Bound by submission: most of each frame is spent inside queue submit on the CPU.";
        else verdict = `${refresh > 0 ? "Missing the display refresh" : "Vsync is off"}, and submission is not what takes the time: the GPU, presentation, or the application's own CPU work. capture_frames with profilePasses measures the GPU passes.`;
        return jsonResult({
          session: s.id, seconds, frames, frame: last.frame,
          frameMs: round(frameMs), fps: round(1000 / frameMs), shortestMs: round(min), longestMs: round(max), submitMs: round(submitMs),
          refreshMs: refresh > 0 ? round(refresh) : undefined,
          refreshSource: last.refreshSource ? REFRESH_SOURCE_NOTE[last.refreshSource] ?? last.refreshSource : undefined,
          presentMode: last.presentMode || undefined, frameBoundary: last.frameBoundary || undefined,
          droppedFrames: dropped || undefined, droppedTotal: last.droppedTotal || undefined,
          driverAllocatedBytes: last.allocatedBytes, verdict,
        });
      },
    },
    {
      name: "capture_frames",
      description: "Capture frames of a running application: every command of the frame with its bound state, the render " +
        "targets read back at the end of each pass, bound buffers and images, and (profilePasses) GPU timestamps and counters " +
        "per pass. The capture is saved as a .gpucap file and opened, and its summary returned: the capture tools " +
        "(get_bottlenecks, get_frame_issues, get_command, read_texture...) work on it by the returned capture id. Capture " +
        "while the application shows what is slow or wrong.",
      inputSchema: schema({
        session: SESSION_PARAM,
        frames: { type: "integer", minimum: 1, maximum: 16, description: "Frames to capture (default 1)." },
        atFrame: { type: "integer", minimum: 0, description: "Capture that frame (the capture library's present counter) instead of the next one." },
        delaySeconds: { type: "number", minimum: 0, maximum: 600, description: "Wait this long before requesting the capture." },
        profilePasses: { type: "boolean", description: "GPU timestamps and counters around every pass (default true)." },
        renderTargets: { type: "boolean", description: "Read back every pass's attachments (default true)." },
        buffers: { type: "boolean", description: "Read back bound buffer ranges (default true)." },
        images: { type: "boolean", description: "Read back images bound through descriptor sets (default true)." },
        stacktraces: { type: "boolean", description: "Record the stack of every command (default false; costs CPU time in the application while capturing)." },
        maxBufferKB: { type: "integer", minimum: 1, description: "Bytes read back per bound buffer range, in KB (default 128)." },
        recordAlways: { type: "boolean", description: "Switch recording of every command buffer on (or off) first, for applications that reuse command buffers recorded before the capture." },
        timeoutSeconds: { type: "number", minimum: 5, maximum: 3600, description: "How long to wait for the capture (default 60)." },
        saveAs: { type: "string", description: `Where to save the .gpucap (default a new file in ${capturesDir()}).` },
      }),
      handler: async (args) => {
        const s = sessions.get(stringArg(args, "session"));
        const delay = numberArg(args, "delaySeconds");
        if (delay) await sleep(delay * 1000);
        if (args.recordAlways !== undefined) await s.send({ action: "Settings", recordAlways: boolArg(args, "recordAlways", false) });
        const result = await s.capture({
          frames: intArg(args, "frames", 1, 1, 16), atFrame: optionalInt(args, "atFrame"),
          profilePasses: boolArg(args, "profilePasses", true), renderTargets: boolArg(args, "renderTargets", true),
          buffers: boolArg(args, "buffers", true), images: boolArg(args, "images", true), stacktraces: boolArg(args, "stacktraces", false),
          maxBufferBytes: intArg(args, "maxBufferKB", 128, 1) * 1024, timeoutMs: (numberArg(args, "timeoutSeconds") ?? 60) * 1000,
        });
        const file = await s.saveCapture(result.data, stringArg(args, "saveAs"));
        const { capture } = store.open(file);
        const notes: string[] = [];
        if (result.completion === "quiet") notes.push("This capture library does not mark the end of a capture (it was built before that message existed), so the capture was taken as complete once its stream went quiet.");
        if (!result.data.commands.length) notes.push("The capture has no commands. An application that records its command buffers once and resubmits them needs recordAlways: true.");
        return jsonResult({
          session: s.id, file, megabytes: round(capture.fileBytes / 1048576), secondsToCapture: round(result.elapsedMs / 1000),
          captureNotes: notes.length ? notes : undefined, ...captureSummary(capture),
        });
      },
    },
    {
      name: "list_live_objects",
      description: "List a running application's live objects (images, buffers, pipelines, descriptor sets, Metal textures...) " +
        "as the capture library tracks them now, with a one-line summary each; without a type filter it also counts them by " +
        "type. Object ids are the same in the session's captures.",
      inputSchema: schema({
        session: SESSION_PARAM,
        type: { type: "string", description: "Only this type: \"VkImage\", \"VkDescriptorSet\", \"MTLTexture\" (the Vk prefix may be left out)." },
        name: { type: "string", description: "Regular expression on the object's name or label." },
        ...PAGE_PARAMS,
      }),
      readOnly: true,
      handler: (args) => {
        const s = sessions.get(stringArg(args, "session"));
        const db = s.database;
        const type = stringArg(args, "type")?.toLowerCase();
        const name = regexArg(args, "name");
        const all = [...db.allObjects.values()].sort((a, b) => a.id - b.id);
        const list = all.filter((o) => (!type || o.type.toLowerCase() === type || o.shortType.toLowerCase() === type) && (!name || name.test(o.name) || name.test(o.label)));
        const types: Record<string, number> = {};
        if (!type) for (const o of all) types[o.type] = (types[o.type] ?? 0) + 1;
        const p = page(list, args, 100, 500);
        return jsonResult({
          session: s.id, state: s.state, types: type ? undefined : types, total: p.total, offset: p.offset, nextOffset: p.nextOffset,
          objects: p.items.map((o) => ({
            id: o.id, type: o.type, name: o.name !== `${o.shortType} ${o.id}` ? o.name : undefined,
            summary: o.summary(db) || undefined, destroyed: o.isDeleted || undefined,
          })),
        });
      },
    },
    {
      name: "get_live_object",
      description: "One live object of a running application in full: the call that created it with its arguments, later " +
        "updates (memory bindings, descriptor contents read so far), its owner, what it depends on and what depends on it, its " +
        "payloads, the validation messages naming it, and its creation stack, fetched from the capture library (launched with " +
        "stack traces, the default). The stack is where a leaked or misconfigured object came from.",
      inputSchema: schema({
        session: SESSION_PARAM,
        id: { type: "integer", minimum: 0, description: "The object's id: the number after # in a reference like VkImage#12." },
        stack: { type: "boolean", description: "Fetch the creation stack (default true)." },
      }, ["id"]),
      readOnly: true,
      handler: async (args) => {
        const s = sessions.get(stringArg(args, "session"));
        const db = s.database;
        const id = requireInt(args, "id");
        const o = db.getObject(id);
        if (!o) throw new Error(`No live object ${id} in ${s.id} (list_live_objects lists them).`);
        let stack: StackFrame[] | undefined;
        let stackNote: string | undefined;
        if (boolArg(args, "stack", true)) {
          stack = db.stacks.get(id) ?? (s.connected ? (await requestStacks(s, [id]))?.get(id) : undefined);
          if (!stack?.length) {
            stackNote = db.stacksAvailable === false ? "The capture library records no stacks: the application was launched without stack traces."
              : s.connected ? "No creation stack was recorded for this object." : "Not connected, so the creation stack cannot be fetched.";
          }
        }
        const validation = db.validationFor(id);
        const contents = ["VkImage", "VkImageView", "MTLTexture"].includes(o.type) ? "read_live_image shows its current contents."
          : o.type === "VkDescriptorSet" ? "get_live_descriptor_set reads what it binds now." : undefined;
        return jsonResult({
          session: s.id, ...objectDetail(db, o),
          validation: validation.length ? validation.slice(0, 20).map((v) => ({
            severity: v.severity, id: v.idName ?? undefined, count: v.count > 1 ? v.count : undefined, frame: v.frame, message: clip(v.message, 600),
          })) : undefined,
          creationStack: stack?.length ? stackLines(await symbolizeOnHost(stack)) : undefined, stackNote, contents,
        });
      },
    },
    {
      name: "read_live_image",
      description: "Look at an image of a running application as it is now, without capturing: the capture library copies one " +
        "mip level and array layer at the application's next frame (a VkImageView reads its image at the view's first mip " +
        "and layer). Returns what read_texture returns: the PNG, per-channel minimum, maximum and mean, the share of zero " +
        "texels, NaN and infinity counts, a 3x3 grid of texel values and exact values at requested texels. Quicker than a " +
        "capture for checking a target after replace_shader. An image the library cannot copy from (transient attachments, " +
        "an unknown layout) comes back with the reason; an attachment every render pass discards (storeOp DONT_CARE) holds " +
        "undefined contents between frames, which the answer notes.",
      inputSchema: schema({
        session: SESSION_PARAM,
        object: { type: "integer", minimum: 1, description: "The VkImage, VkImageView or MTLTexture object id (list_live_objects lists them)." },
        mip: { type: "integer", minimum: 0, description: "Mip level (default 0, or the view's first)." },
        layer: { type: "integer", minimum: 0, description: "Array layer, or the slice of a 3D image (default 0, or the view's first layer)." },
        ...IMAGE_PARAMS,
        timeoutSeconds: { type: "number", minimum: 1, maximum: 120, description: "How long to wait for the application's next frame (default 15)." },
      }, ["object"]),
      readOnly: true,
      handler: async (args) => {
        const s = sessions.get(stringArg(args, "session"));
        requireConnected(s);
        const db = s.database;
        const requested = requireInt(args, "object");
        let o = db.getObject(requested);
        if (!o) throw new Error(`No live object ${requested} in ${s.id} (list_live_objects lists them).`);
        let mip = optionalInt(args, "mip");
        let layer = optionalInt(args, "layer");
        if (o.type === "VkImageView") {
          const view = o.descriptor;
          const range = view && isObject(view.subresourceRange) ? view.subresourceRange : null;
          mip ??= num(range?.baseMipLevel);
          layer ??= num(range?.baseArrayLayer);
          const image = db.getObject(refId(view?.image));
          if (!image) throw new Error(`${refText(db, o.id)} names no live image.`);
          o = image;
        }
        if (o.type !== "VkImage" && o.type !== "MTLTexture") {
          throw new Error(`${refText(db, o.id)} is not an image: read_live_image takes a VkImage, a VkImageView or an MTLTexture.`);
        }
        // A 3D image's read-back holds every depth slice; `layer` picks one of them.
        const d = o.descriptor;
        const is3D = /3D/.test(`${str(d?.imageType)}${str(d?.textureType)}`);
        const timeoutMs = (numberArg(args, "timeoutSeconds") ?? 15) * 1000;
        const msg = await s.readImage(o.id, mip ?? 0, is3D ? 0 : layer ?? 0, timeoutMs);
        if (!msg) {
          throw new Error(`${refText(db, o.id)} was not read back within ${timeoutMs / 1000} s: the capture library copies images at the application's next frame, so it may not be rendering.`);
        }
        const slices = Math.max(1, msg.depth || 1);
        const slice = slices > 1 ? Math.min(layer ?? 0, slices - 1) : undefined;
        const head = {
          session: s.id, image: refText(db, o.id), format: msg.format || undefined, aspect: msg.aspect, mip: msg.mip,
          layer: slices > 1 ? undefined : msg.layer, slice, slices: slices > 1 ? slices : undefined,
          note: o.type === "VkImage" ? discardNote(db, o.id) : undefined,
        };
        if (msg.error) {
          const failed = jsonResult({ ...head, error: msg.error });
          failed.isError = true;
          return failed;
        }
        if (!msg.__binary) return jsonResult({ ...head, note: "The answer carried no pixel data." });
        if (!isFormatSupported(msg)) return jsonResult({ ...head, note: `Decoding ${msg.format} is not supported.` });
        const tex = decodeTexels(msg, msg.__binary, slice ?? 0);
        if (!tex) return jsonResult({ ...head, note: "The pixel data is shorter than the image's size says." });
        return texelAnswer(tex, args, head, msg.aspect);
      },
    },
    {
      name: "get_live_descriptor_set",
      description: "What a running application's descriptor set binds now (Vulkan): each binding's type and stages, and each " +
        "descriptor's buffer with offset and range, image view with its image and layout, or sampler. A capture shows the sets " +
        "bound at each draw with their buffer contents; this reads a set between captures.",
      inputSchema: schema({
        session: SESSION_PARAM,
        set: { type: "integer", minimum: 1, description: "The VkDescriptorSet's object id." },
      }, ["set"]),
      readOnly: true,
      handler: async (args) => {
        const s = sessions.get(stringArg(args, "session"));
        requireConnected(s);
        const db = s.database;
        const id = requireInt(args, "set");
        const o = db.getObject(id);
        if (!o || o.type !== "VkDescriptorSet") throw new Error(`${o ? refText(db, id) : `Object ${id}`} is not a live VkDescriptorSet of ${s.id}.`);
        if (!(await s.readDescriptorSet(id))) throw new Error("The layer did not answer within 10 s.");
        const u = o.updates;
        if (u.tracked === false) {
          return jsonResult({ session: s.id, set: refText(db, id), note: "The layer has no record of this set's contents: it was freed, or its pool was reset." });
        }
        const bindings = Array.isArray(u.bindings) ? (u.bindings as unknown as CaptureDescriptorBinding[]) : [];
        return jsonResult({
          session: s.id, set: refText(db, id), layout: refText(db, u.layout),
          bindings: bindings.map((b) => {
            const shown = b.descriptors.slice(0, 32);
            return {
              binding: b.binding, type: b.type, stages: b.stages,
              descriptors: shown.map((x) => (x ? liveDescriptor(db, x) : "not written")),
              more: b.descriptors.length > shown.length ? b.descriptors.length - shown.length : undefined,
            };
          }),
          note: bindings.length ? undefined : "The set has no bindings.",
        });
      },
    },
    {
      name: "replace_shader",
      description: "Replace one stage of a running Vulkan pipeline: the source (GLSL, HLSL or SPIR-V assembly) is compiled " +
        "with the Vulkan SDK's compilers for the stage's entry point and SPIR-V version, and the layer rebuilds the pipeline " +
        "with it, binding the replacement wherever the application binds the original. get_shader with view \"glsl\" on a " +
        "capture gives editable source for a pipeline; capture again (and compare_captures) to see the effect; restore_shader " +
        "undoes it. Command buffers recorded before the edit keep the original until the application records them again.",
      inputSchema: schema({
        session: SESSION_PARAM,
        pipeline: { type: "integer", minimum: 1, description: "The VkPipeline's object id (the same in the live session and its captures)." },
        stage: { type: "string", description: "The stage to replace: vertex, fragment, compute, geometry, tess_control, tess_eval, mesh, task, ..." },
        source: { type: "string", description: "The complete new source of the stage." },
        language: { type: "string", enum: LANGUAGES, description: "The source's language (default glsl)." },
        entryPoint: { type: "string", description: "The entry point in the source (default the stage's own)." },
      }, ["pipeline", "stage", "source"]),
      handler: async (args) => {
        const s = sessions.get(stringArg(args, "session"));
        if (s.api === "metal") throw new Error("Shader replacement is Vulkan only.");
        if (!s.connected) throw new Error(`${s.id} is not connected (${s.state}).`);
        const pipelineId = requireInt(args, "pipeline");
        const stageName = requireString(args, "stage").toLowerCase();
        const { flag, entryPoint, source } = stageOf(s, pipelineId, stageName);
        // The stage's current SPIR-V says which SPIR-V version the application's driver was given.
        const original = await fetchBlob(s, source.object, source.blobIndex);
        const version = (original && reflectSpirv(original)?.version) || "";
        const language = enumArg(args, "language", LANGUAGES, "glsl") as ShaderLanguage;
        const compiled = await compileShader(requireString(args, "source"), language, stageName, stringArg(args, "entryPoint") ?? entryPoint, version);
        if (!compiled.ok || !compiled.spirv) {
          const failed = jsonResult({ ok: false, failedAt: "compile", compiler: compiled.tool, log: clip(compilerLog(compiled.log), 12000) });
          failed.isError = true;
          return failed;
        }
        const reply = await s.replaceShader(pipelineId, flag, stageName, compiled.spirv);
        const result = jsonResult({
          ok: reply?.ok ?? false, pipeline: refText(s.database, pipelineId), stage: stageName, spirvVersion: version || undefined,
          replacement: reply?.replacement ? refText(s.database, reply.replacement) ?? `VkPipeline#${reply.replacement}` : undefined,
          error: reply ? reply.error : "The layer did not answer within 15 s.", layerNote: reply?.note,
          compilerLog: compilerLog(compiled.log) ? clip(compilerLog(compiled.log), 4000) : undefined,
        });
        if (!reply?.ok) result.isError = true;
        return result;
      },
    },
    {
      name: "restore_shader",
      description: "Undo replace_shader: the pipeline binds its original code again, for one stage or every replaced stage.",
      inputSchema: schema({
        session: SESSION_PARAM,
        pipeline: { type: "integer", minimum: 1, description: "The VkPipeline's object id." },
        stage: { type: "string", description: "The stage to restore (default every replaced stage)." },
      }, ["pipeline"]),
      handler: async (args) => {
        const s = sessions.get(stringArg(args, "session"));
        if (!s.connected) throw new Error(`${s.id} is not connected (${s.state}).`);
        const pipelineId = requireInt(args, "pipeline");
        const stage = stringArg(args, "stage");
        const flag = stage ? stageOf(s, pipelineId, stage).flag : undefined;
        const reply = await s.restoreShader(pipelineId, flag);
        return jsonResult({ ok: reply?.ok ?? false, pipeline: refText(s.database, pipelineId), stage, error: reply ? reply.error : "The layer did not answer within 15 s." });
      },
    },
    {
      name: "get_session_log",
      description: "A live session's log: the application's standard output and error, the capture library's own log, " +
        "connection changes, validation messages and shader edits, most recent last.",
      inputSchema: schema({
        session: SESSION_PARAM,
        lines: { type: "integer", minimum: 1, maximum: 2000, description: "How many of the most recent lines (default 100)." },
        match: { type: "string", description: "Regular expression: only lines that match." },
      }),
      readOnly: true,
      handler: (args) => {
        const s = sessions.get(stringArg(args, "session"));
        const match = regexArg(args, "match");
        const lines = (match ? s.log.filter((l) => match.test(l)) : s.log).slice(-intArg(args, "lines", 100, 1, 2000));
        return jsonResult({ session: s.id, state: s.state, lines });
      },
    },
    {
      name: "stop_app",
      description: "End a live session: a launched application is terminated (with the processes it started), an attached one is disconnected.",
      inputSchema: schema({ session: SESSION_PARAM }),
      handler: async (args) => {
        const s = sessions.get(stringArg(args, "session"));
        await s.stop();
        return jsonResult({ session: s.id, state: s.state, detail: s.detail || undefined, exitCode: s.exitCode ?? undefined });
      },
    },
  ];
}
