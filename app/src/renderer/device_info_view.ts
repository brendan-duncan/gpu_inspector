// Structured sections for the objects that describe the machine: what the physical device
// offers (properties and limits, memory, queue families, features, extensions, attached by the
// layer as an update at vkEnumeratePhysicalDevices), what the application enabled on its device
// (extensions, features across the pNext chain, queues), and what it asked of the instance.
// Everything here also works on capture files, where these questions matter most.
import { Checkbox } from "./widget/checkbox.js";
import { collapsible } from "./widget/collapsible.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { TextInput } from "./widget/text_input.js";
import { Widget } from "./widget/widget.js";
import { fmt, formatBytes, isObject, num, str, type VulkanObject } from "./vulkan/vulkan_object.js";
import type { ArgObject, ArgValue } from "../shared/protocol.js";

const VENDORS: Record<number, string> = {
  0x1002: "AMD", 0x1010: "Imagination", 0x106b: "Apple", 0x10de: "NVIDIA", 0x13b5: "ARM", 0x14e4: "Broadcom",
  0x1ae0: "Google", 0x5143: "Qualcomm", 0x8086: "Intel", 0x10001: "VIV", 0x10002: "VSI", 0x10003: "Kazan",
  0x10004: "Codeplay", 0x10005: "Mesa", 0x10006: "PoCL", 0x10007: "Mobileye",
};

/** "1.3.280" (with the variant when non-zero) from a packed Vulkan version. */
export function vulkanVersion(v: number): string {
  const variant = v >>> 29;
  const s = `${(v >>> 22) & 0x7f}.${(v >>> 12) & 0x3ff}.${v & 0xfff}`;
  return variant ? `${s} (variant ${variant})` : s;
}

/** A driver version in the vendor's own encoding, plus the raw value. */
function driverVersion(vendor: number, v: number): string {
  if (vendor === 0x10de) return `${v >>> 22}.${(v >>> 14) & 0xff}.${(v >>> 6) & 0xff}.${v & 0x3f}  (${v})`;
  if (vendor === 0x8086 && navigator.platform.startsWith("Win")) return `${v >>> 14}.${v & 0x3fff}  (${v})`;
  return `${vulkanVersion(v)}  (${v})`;
}

function hex(v: number): string {
  return `0x${v.toString(16).toUpperCase()}`;
}

/** Flag bits as names; "" when none are set (serialized as 0 or "0"). */
function flagsText(v: ArgValue | undefined): string {
  if (v === null || v === undefined || v === 0 || v === "0") return "";
  return fmt(v);
}

function valueText(v: ArgValue | undefined): string {
  if (v === null || v === undefined) return "";
  if (Array.isArray(v)) return v.map(valueText).join(", ");
  if (typeof v === "object") return Object.entries(v).map(([k, x]) => `${k}: ${valueText(x)}`).join(", ");
  if (typeof v === "string") return fmt(v);
  if (typeof v === "boolean") return v ? "true" : "false";
  return String(v);
}

function section(parent: Widget, label: string, collapsed = false): Div {
  const grp = new collapsible(parent, { label, collapsed });
  return new Div(grp.body, { class: "device-info" });
}

function row(parent: Widget, label: string, value: string): Div {
  const r = new Div(parent, { class: "draw-state-row" });
  new Span(r, { text: label, class: "draw-state-label device-info-label" });
  new Span(r, { text: value, class: "device-info-value" });
  return r;
}

/** Name / value rows with a filter box: the row is shown when its name or value matches. */
function filterableRows(parent: Widget, rows: [string, string][], placeholder: string): void {
  const bar = new Div(parent, { class: "inspector-filter-row" });
  new Span(bar, { text: "Filter", class: "inspector-filter-label-sm" });
  const input = new TextInput(bar, { placeholder, class: "inspector-filter-input-sm", style: "width: 200px;" });
  const count = new Span(bar, { text: `${rows.length}`, class: "text-muted font-sm" });
  const list = new Div(parent, { class: "draw-state" });
  const widgets = rows.map(([k, v]) => ({ k: k.toLowerCase(), v: v.toLowerCase(), widget: row(list, k, v) }));
  input.element.oninput = () => {
    const f = input.value.trim().toLowerCase();
    let shown = 0;
    for (const w of widgets) {
      const show = !f || w.k.includes(f) || w.v.includes(f);
      w.widget.style.display = show ? "" : "none";
      if (show) shown++;
    }
    count.text = f ? `${shown} of ${rows.length}` : `${rows.length}`;
  };
}

/** Boolean members of a feature struct (VkPhysicalDeviceFeatures and the pNext feature structs). */
function featureRows(obj: ArgObject, source: string): [string, boolean][] {
  const out: [string, boolean][] = [];
  for (const [k, v] of Object.entries(obj)) {
    if (k === "sType" || k === "pNext") continue;
    if (typeof v === "boolean") out.push([source ? `${k}  (${source})` : k, v]);
    else if (typeof v === "number" && (v === 0 || v === 1)) out.push([source ? `${k}  (${source})` : k, v === 1]);
  }
  return out;
}

/** A feature list showing the enabled ones, with a checkbox to show the rest. */
function featureList(parent: Widget, rows: [string, boolean][], onWord: string): void {
  const enabled = rows.filter((r) => r[1]);
  const bar = new Div(parent, { class: "inspector-filter-row" });
  new Span(bar, { text: `${enabled.length} of ${rows.length} ${onWord}`, class: "text-muted font-sm" });
  const all = new Checkbox(bar, { label: "Show all", class: "inspector-filter-field" });
  const list = new Div(parent, { class: "draw-state" });
  const widgets = rows.map(([k, v]) => {
    const r = new Div(list, { class: `draw-state-row device-feature ${v ? "device-feature-on" : "device-feature-off"}` });
    new Span(r, { text: v ? "✓ " : "✗ ", class: "device-feature-mark" });
    new Span(r, { text: k });
    r.style.display = v ? "" : "none";
    return { on: v, widget: r };
  });
  all.input.onchange = () => {
    for (const w of widgets) w.widget.style.display = w.on || all.checked ? "" : "none";
  };
  if (!rows.length) new Div(list, { text: "None recorded.", class: "text-muted" });
}

/** The structs of a serialized pNext chain (an array of objects, or a single object). */
function pNextStructs(v: ArgValue | undefined): ArgObject[] {
  if (Array.isArray(v)) return v.filter(isObject);
  return isObject(v) ? [v] : [];
}

function shortSType(s: ArgValue | undefined): string {
  return str(s).replace(/^VK_STRUCTURE_TYPE_(PHYSICAL_DEVICE_)?/, "").replace(/_FEATURES(_\w+)?$/, "$1").toLowerCase();
}

// ---------------------------------------------------------------------------------------------

export function renderPhysicalDeviceSections(container: Widget, object: VulkanObject): void {
  const u = object.updates;
  const props = isObject(u.properties) ? u.properties : null;
  if (!props) {
    new Div(container, { text: "The layer did not report this device's capabilities (it was enumerated before the layer had them, or an older layer).", class: "text-muted capture-note" });
    return;
  }
  const vendor = num(props.vendorID);
  const p = section(container, "Properties");
  row(p, "Device", str(props.deviceName));
  row(p, "Type", fmt(props.deviceType));
  row(p, "Vendor", `${VENDORS[vendor] ?? "unknown"}  ${hex(vendor)}`);
  row(p, "Device ID", hex(num(props.deviceID)));
  row(p, "API version", vulkanVersion(num(props.apiVersion)));
  row(p, "Driver version", driverVersion(vendor, num(props.driverVersion)));
  if (Array.isArray(props.pipelineCacheUUID)) row(p, "Pipeline cache UUID", props.pipelineCacheUUID.map((b) => num(b).toString(16).padStart(2, "0")).join(""));
  if (isObject(props.sparseProperties)) {
    for (const [k, v] of Object.entries(props.sparseProperties)) row(p, k, valueText(v));
  }

  const limits = isObject(props.limits) ? props.limits : null;
  if (limits) {
    const rows = Object.entries(limits).map(([k, v]): [string, string] => [k, valueText(v)]).sort((a, b) => a[0].localeCompare(b[0]));
    filterableRows(section(container, `Limits (${rows.length})`, true), rows, "limit name or value...");
  }

  const mem = isObject(u.memoryProperties) ? u.memoryProperties : null;
  if (mem) {
    const heapCount = num(mem.memoryHeapCount);
    const typeCount = num(mem.memoryTypeCount);
    const heaps = Array.isArray(mem.memoryHeaps) ? mem.memoryHeaps.slice(0, heapCount) : [];
    const types = Array.isArray(mem.memoryTypes) ? mem.memoryTypes.slice(0, typeCount) : [];
    const m = section(container, `Memory (${heapCount} heaps, ${typeCount} types)`);
    for (let i = 0; i < heaps.length; i++) {
      const h = heaps[i];
      if (!isObject(h)) continue;
      row(m, `Heap ${i}`, `${formatBytes(num(h.size))}  ${flagsText(h.flags) || "host (not device local)"}`);
    }
    for (let i = 0; i < types.length; i++) {
      const t = types[i];
      if (!isObject(t)) continue;
      row(m, `Type ${i}`, `heap ${num(t.heapIndex)}  ${flagsText(t.propertyFlags) || "(no flags)"}`);
    }
  }

  const families = Array.isArray(u.queueFamilies) ? u.queueFamilies.filter(isObject) : [];
  if (families.length) {
    const q = section(container, `Queue Families (${families.length})`);
    families.forEach((f, i) => {
      const g = isObject(f.minImageTransferGranularity) ? f.minImageTransferGranularity : null;
      row(q, `Family ${i}`, `${num(f.queueCount)} queue${num(f.queueCount) === 1 ? "" : "s"}  ${flagsText(f.queueFlags) || "(no flags)"}  timestamp bits ${num(f.timestampValidBits)}${g ? `  transfer granularity ${num(g.width)}x${num(g.height)}x${num(g.depth)}` : ""}`);
    });
  }

  if (isObject(u.features)) {
    featureList(section(container, "Features", true), featureRows(u.features, ""), "supported");
  }

  const exts = Array.isArray(u.extensions) ? u.extensions.filter(isObject) : [];
  if (exts.length) {
    const rows = exts.map((e): [string, string] => [str(e.extensionName), `spec ${num(e.specVersion)}`]).sort((a, b) => a[0].localeCompare(b[0]));
    filterableRows(section(container, `Extensions (${rows.length})`, true), rows, "extension name...");
  }
}

export function renderDeviceSections(container: Widget, object: VulkanObject): void {
  const a = object.args ?? {};
  const ci = isObject(a.pCreateInfo) ? a.pCreateInfo : null;
  if (!ci) return;
  const exts = Array.isArray(ci.ppEnabledExtensionNames) ? ci.ppEnabledExtensionNames.map(str).filter(Boolean) : [];
  const e = section(container, `Enabled Extensions (${exts.length})`);
  if (exts.length) for (const x of exts.sort()) new Div(e, { text: x, class: "draw-state-row" });
  else new Div(e, { text: "None.", class: "text-muted" });

  const queues = Array.isArray(ci.pQueueCreateInfos) ? ci.pQueueCreateInfos.filter(isObject) : [];
  if (queues.length) {
    const q = section(container, `Queues (${queues.length})`);
    for (const qi of queues) {
      const prios = Array.isArray(qi.pQueuePriorities) ? qi.pQueuePriorities.map(valueText).join(", ") : "";
      row(q, `Family ${num(qi.queueFamilyIndex)}`, `${num(qi.queueCount)} queue${num(qi.queueCount) === 1 ? "" : "s"}${prios ? `  priorities ${prios}` : ""}${num(qi.flags) ? `  ${fmt(qi.flags)}` : ""}`);
    }
  }

  // Features: the core struct, and every *Features* struct chained through pNext.
  const rows: [string, boolean][] = [];
  if (isObject(ci.pEnabledFeatures)) rows.push(...featureRows(ci.pEnabledFeatures, ""));
  for (const s of pNextStructs(ci.pNext)) {
    const sType = str(s.sType);
    if (sType === "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2") {
      if (isObject(s.features)) rows.push(...featureRows(s.features, ""));
      continue;
    }
    if (/FEATURES/.test(sType)) rows.push(...featureRows(s, shortSType(sType)));
  }
  featureList(section(container, "Enabled Features", false), rows, "enabled");
}

export function renderInstanceSections(container: Widget, object: VulkanObject): void {
  const a = object.args ?? {};
  const ci = isObject(a.pCreateInfo) ? a.pCreateInfo : null;
  if (!ci) return;
  const app = isObject(ci.pApplicationInfo) ? ci.pApplicationInfo : null;
  const p = section(container, "Application");
  if (app) {
    row(p, "Application", `${str(app.pApplicationName) || "(unnamed)"}  version ${num(app.applicationVersion)}`);
    row(p, "Engine", `${str(app.pEngineName) || "(none)"}  version ${num(app.engineVersion)}`);
    row(p, "API version requested", vulkanVersion(num(app.apiVersion)));
  } else {
    new Div(p, { text: "No application info (Vulkan 1.0 defaults).", class: "text-muted" });
  }
  const layers = Array.isArray(ci.ppEnabledLayerNames) ? ci.ppEnabledLayerNames.map(str).filter(Boolean) : [];
  const l = section(container, `Enabled Layers (${layers.length})`);
  if (layers.length) for (const x of layers) new Div(l, { text: x, class: "draw-state-row" });
  else new Div(l, { text: "None requested by the application (the inspector's layer is enabled by the loader).", class: "text-muted" });
  const exts = Array.isArray(ci.ppEnabledExtensionNames) ? ci.ppEnabledExtensionNames.map(str).filter(Boolean) : [];
  const e = section(container, `Enabled Extensions (${exts.length})`);
  if (exts.length) for (const x of exts.sort()) new Div(e, { text: x, class: "draw-state-row" });
  else new Div(e, { text: "None.", class: "text-muted" });
}
