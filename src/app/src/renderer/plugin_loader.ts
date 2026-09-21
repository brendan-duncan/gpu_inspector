// The renderer's half of plugins: every plugin the main process found (main/plugins.ts) has its
// backend module imported through the gpuinsp-plugin: scheme and activated (plugin_host.ts), before
// the window is built, so the first capture already reads with the plugin's backend.
import { activatePlugin } from "./plugin_host.js";
import type { PluginInfo } from "../shared/protocol.js";

/** Each plugin as it came out of loading: its error, when its backend could not be activated. */
export const loadedPlugins: PluginInfo[] = [];

export async function loadPlugins(): Promise<void> {
  let infos: PluginInfo[] = [];
  try {
    infos = await window.inspector.plugins();
  } catch (e) {
    console.error(`the plugin list could not be read: ${(e as Error).message}`);
    return;
  }
  for (const info of infos) {
    const entry = { ...info };
    loadedPlugins.push(entry);
    if (info.error || !info.backendUrl) {
      if (info.error) console.warn(`plugin ${info.id}: ${info.error}`);
      continue;
    }
    try {
      const mod: unknown = await import(/* webpackIgnore: true */ info.backendUrl);
      await activatePlugin(mod, info, "renderer");
      console.log(`plugin ${info.id} ${info.version}: the ${info.api} backend is registered`);
    } catch (e) {
      entry.error = `the backend did not load: ${(e as Error).message}`;
      console.error(`plugin ${info.id}: ${entry.error}`);
    }
  }
}
