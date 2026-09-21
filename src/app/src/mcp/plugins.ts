// Plugins in the MCP server (docs/PLUGINS.md): the same plugins the app finds (main/plugins.ts), their
// backend modules imported here so a capture of a plugin's API reads as it does in the app, and their
// capture libraries put into the applications launch_app starts.
import path from "node:path";
import { pathToFileURL } from "node:url";
import { findPlugins, pluginInfo, pluginLaunches, pluginSearchDirs, type Plugin, type PluginLaunch } from "../main/plugins.js";
import { activatePlugin } from "../renderer/plugin_host.js";
import { checkoutRoots, installedLayerDirs } from "./live_session.js";

let found: Plugin[] | null = null;

/** The plugins of the checkout this server is in and of the installed app (resources/plugins beside resources/layer). */
export function plugins(): Plugin[] {
  found ??= findPlugins(pluginSearchDirs(checkoutRoots(), installedLayerDirs().map((d) => path.join(path.dirname(d), "plugins"))));
  return found;
}

/** Imports and activates every usable plugin's backend; a plugin that fails is reported on stderr and skipped. */
export async function loadPluginBackends(): Promise<void> {
  for (const p of plugins()) {
    if (p.error || !p.backend) {
      if (p.error) process.stderr.write(`gpu-inspector MCP server: plugin ${p.manifest.id}: ${p.error}\n`);
      continue;
    }
    try {
      await activatePlugin(await import(pathToFileURL(p.backend).href), pluginInfo(p), "mcp");
    } catch (e) {
      process.stderr.write(`gpu-inspector MCP server: plugin ${p.manifest.id}: the backend did not load: ${(e as Error).message}\n`);
    }
  }
}

/** What the plugins put into an application launched on `port` (their libraries and settings). */
export function launchPlugins(port: number, recordAlways: boolean, stacktraces: boolean): PluginLaunch[] {
  return pluginLaunches(plugins(), { port, log: true, recordAlways, stacktraces });
}
