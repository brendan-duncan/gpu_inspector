// GPU Inspector's MCP server: the capture tools over one capture store, and the live tools over
// the applications it launches. main.ts serves it on stdio; the tests call it directly.
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { CaptureStore } from "./capture_store.js";
import { commandTools } from "./command_tools.js";
import { debugTools } from "./debug_tools.js";
import { SessionManager } from "./live_session.js";
import { liveTools } from "./live_tools.js";
import { resourceTools } from "./resource_tools.js";
import { McpStdioServer } from "./stdio_server.js";
import { captureTools } from "./tools.js";

/**
 * The version reported to the client, read at run time from the plugin manifest beside the bundle
 * (`claude-plugin/.claude-plugin/plugin.json`).
 *
 * Deliberately not baked in at build time: the bundle is committed to the repository, so any value
 * the build embedded went stale the moment a version was bumped without rebuilding it, and CI
 * failed on a file nobody had touched. Nothing in the bundle depends on a version now, so it only
 * changes when the sources it is built from do.
 */
function serverVersion(): string {
  try {
    const manifest = fileURLToPath(new URL("../.claude-plugin/plugin.json", import.meta.url));
    const version = (JSON.parse(readFileSync(manifest, "utf8")) as { version?: unknown }).version;
    if (typeof version === "string" && version) return version;
  } catch {
    // Not running from an installed plugin (a test bundles these sources elsewhere).
  }
  return "dev";
}

/** Sent to the client at initialization, for models that meet the tools without the plugin's skill. */
const INSTRUCTIONS = [
  "These tools read GPU Inspector frame captures (.gpucap) of Vulkan, Metal and Direct3D 12 applications, with the analyses GPU Inspector runs, and drive running applications.",
  "Open a saved capture with open_capture (list_captures shows the files GPU Inspector saved recently), or launch_app an application (launch_android_app on Android) and capture_frames it; then start from get_capture_summary.",
  "An application that calls gpu_inspector_capture_named (include/gpu_inspector.h) from an assertion or a failed test gets its capture taken and saved by the session on its own: get_session_status lists them under appCaptures, and list_captures shows them open with the application's label.",
  "For performance: get_bottlenecks (needs profiled passes), get_frame_issues, get_render_graph, analyze_shaders, get_shader_flame_graph, get_live_frame_stats, and compare_captures to check a fix.",
  "To debug rendering: read_texture shows what a pass wrote; list_commands finds draws by pass, label or kind; get_command shows the state a draw read (pipeline, decoded uniforms, vertex and index buffers, render targets); read_vertices, read_buffer and get_shader go deeper; get_validation lists real errors. replace_shader tries a shader fix in the running application, and read_live_image looks at an image without capturing.",
  "Object references read Type#id \"name\": pass the id to get_object. Cite command indices, object ids and pass labels so the user can find them in GPU Inspector.",
].join(" ");

export function createServer(store = new CaptureStore(), sessions = new SessionManager()): McpStdioServer {
  // A capture the application asked for itself is opened as capture_frames' would be, so the
  // capture tools find it by id and list_captures shows it.
  sessions.onAppCaptureSaved = (session, file) => {
    try {
      const { capture } = store.open(file);
      session.appendLog(`the application's capture is open as ${capture.id}`);
    } catch (e) {
      session.appendLog(`the application's capture could not be opened: ${e instanceof Error ? e.message : String(e)}`);
    }
  };
  return new McpStdioServer({ name: "gpu-inspector", version: serverVersion() }, [
    ...captureTools(store), ...commandTools(store), ...resourceTools(store), ...debugTools(store), ...liveTools(sessions, store),
  ], INSTRUCTIONS);
}

export { CaptureStore, SessionManager };
