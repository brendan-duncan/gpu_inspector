// What a plugin's backend module is given, and how it is activated (docs/PLUGINS.md).
//
// A backend module is an ES module exporting `activate(host)`, which returns the Backend (or several)
// for the plugin's API. The host is everything the module gets from the app: the plugin contract's
// version and the helpers every backend needs to read serialized arguments, so a module can be built
// on its own, against the SDK's types (src/sdk/ts), without bundling any of the app.
//
// DOM-free: the renderer (plugin_loader.ts) and the MCP server (src/mcp/main.ts) both activate plugins.
import { EMPTY_SETS, registerBackend, registeredBackends, type Backend } from "./backend.js";
import { fmt, formatBytes, isHandleRef, isObject, num, refId, str } from "./vulkan/vulkan_object.js";
import { PLUGIN_SDK_VERSION, type PluginInfo } from "../shared/protocol.js";

export interface PluginHost {
  readonly sdkVersion: number;
  /** "renderer" in the app's window, "mcp" in the MCP server (which has no DOM). */
  readonly context: "renderer" | "mcp";
  /** A classification with nothing in it, for a backend to spread its own sets over. */
  readonly emptySets: typeof EMPTY_SETS;
  /** Reading serialized arguments (shared/protocol.ts ArgValue). */
  readonly util: {
    isObject: typeof isObject;
    isHandleRef: typeof isHandleRef;
    num: typeof num;
    str: typeof str;
    refId: typeof refId;
    /** A Vulkan enum or flag name shortened for display: "VK_FORMAT_R8G8B8A8_UNORM" -> "R8G8B8A8_UNORM". */
    fmt: typeof fmt;
    formatBytes: typeof formatBytes;
  };
}

export function pluginHost(context: PluginHost["context"]): PluginHost {
  return {
    sdkVersion: PLUGIN_SDK_VERSION,
    context,
    emptySets: EMPTY_SETS,
    util: { isObject, isHandleRef, num, str, refId, fmt, formatBytes },
  };
}

/** What a backend module exports. */
export interface BackendModule {
  activate(host: PluginHost): Backend | Backend[] | Promise<Backend | Backend[]>;
}

/**
 * Registers what a plugin's module activates to. The backend of the plugin's `api` has to be among
 * them; each is marked with the plugin it came from.
 */
export async function activatePlugin(mod: unknown, info: PluginInfo, context: PluginHost["context"]): Promise<Backend[]> {
  const m = mod as Partial<BackendModule> | null;
  if (!m || typeof m.activate !== "function") throw new Error("the backend module exports no activate function");
  const got = await m.activate(pluginHost(context));
  const backends = Array.isArray(got) ? got : [got];
  if (!backends.some((b) => b && b.id === info.api)) throw new Error(`activate returned no backend for "${info.api}", the api plugin.json names`);
  for (const b of backends) {
    registerBackend(Object.setPrototypeOf({ plugin: { id: info.id, version: info.version, dir: info.dir } }, b) as Backend);
  }
  return registeredBackends().filter((b) => backends.some((x) => x.id === b.id));
}
