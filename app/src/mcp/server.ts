// GPU Inspector's MCP server: the capture tools over one capture store, and the live tools over
// the applications it launches. main.ts serves it on stdio; the tests call it directly.
import { CaptureStore } from "./capture_store.js";
import { commandTools } from "./command_tools.js";
import { SessionManager } from "./live_session.js";
import { liveTools } from "./live_tools.js";
import { resourceTools } from "./resource_tools.js";
import { McpStdioServer } from "./stdio_server.js";
import { captureTools } from "./tools.js";

// Set by build.mjs from app/package.json; absent when a test bundles the sources.
declare const __GPU_INSPECTOR_VERSION__: string | undefined;

/** Sent to the client at initialization, for models that meet the tools without the plugin's skill. */
const INSTRUCTIONS = [
  "These tools read GPU Inspector frame captures (.gpucap) of Vulkan and Metal applications, with the analyses GPU Inspector runs, and drive running applications.",
  "Open a saved capture with open_capture (list_captures shows the files GPU Inspector saved recently), or launch_app an application (launch_android_app on Android) and capture_frames it; then start from get_capture_summary.",
  "For performance: get_bottlenecks (needs profiled passes), get_frame_issues, get_render_graph, analyze_shaders, get_shader_flame_graph, get_live_frame_stats, and compare_captures to check a fix.",
  "To debug rendering: read_texture shows what a pass wrote; list_commands finds draws by pass, label or kind; get_command shows the state a draw read (pipeline, decoded uniforms, vertex and index buffers, render targets); read_vertices, read_buffer and get_shader go deeper; get_validation lists real errors. replace_shader tries a shader fix in the running application, and read_live_image looks at an image without capturing.",
  "Object references read Type#id \"name\": pass the id to get_object. Cite command indices, object ids and pass labels so the user can find them in GPU Inspector.",
].join(" ");

export function createServer(store = new CaptureStore(), sessions = new SessionManager()): McpStdioServer {
  const version = typeof __GPU_INSPECTOR_VERSION__ === "string" ? __GPU_INSPECTOR_VERSION__ : "dev";
  return new McpStdioServer({ name: "gpu-inspector", version }, [
    ...captureTools(store), ...commandTools(store), ...resourceTools(store), ...liveTools(sessions, store),
  ], INSTRUCTIONS);
}

export { CaptureStore, SessionManager };
