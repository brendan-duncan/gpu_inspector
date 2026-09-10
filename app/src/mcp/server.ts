// GPU Inspector's MCP server: the capture tools over one capture store. main.ts serves it on
// stdio; the tests call it directly.
import { CaptureStore } from "./capture_store.js";
import { commandTools } from "./command_tools.js";
import { resourceTools } from "./resource_tools.js";
import { McpStdioServer } from "./stdio_server.js";
import { captureTools } from "./tools.js";

// Set by build.mjs from app/package.json; absent when a test bundles the sources.
declare const __GPU_INSPECTOR_VERSION__: string | undefined;

/** Sent to the client at initialization, for models that meet the tools without the plugin's skill. */
const INSTRUCTIONS = [
  "These tools read GPU Inspector frame captures (.gpucap) of Vulkan and Metal applications, with the analyses GPU Inspector runs.",
  "Open one with open_capture (list_captures shows the files GPU Inspector saved recently), then start from get_capture_summary.",
  "For performance: get_bottlenecks (needs a capture taken with Profile passes), get_frame_issues, get_render_graph, analyze_shaders, and compare_captures to check a fix.",
  "To debug rendering: read_texture shows what a pass wrote; list_commands finds draws by pass, label or kind; get_command shows the state a draw read (pipeline, decoded uniforms, vertex and index buffers, render targets); read_vertices, read_buffer and get_shader go deeper; get_validation lists real errors.",
  "Object references read Type#id \"name\": pass the id to get_object. Cite command indices, object ids and pass labels so the user can find them in GPU Inspector.",
].join(" ");

export function createServer(store = new CaptureStore()): McpStdioServer {
  const version = typeof __GPU_INSPECTOR_VERSION__ === "string" ? __GPU_INSPECTOR_VERSION__ : "dev";
  return new McpStdioServer({ name: "gpu-inspector", version }, [...captureTools(store), ...commandTools(store), ...resourceTools(store)], INSTRUCTIONS);
}

export { CaptureStore };
