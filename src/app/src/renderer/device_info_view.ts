// Structured sections for the objects that describe the machine: what the physical device
// offers (properties and limits, memory, queue families, features, extensions, attached by the
// layer as an update at vkEnumeratePhysicalDevices), what the application enabled on its device
// (extensions, features across the pNext chain, queues), and what it asked of the instance.
// Everything here also works on capture files, where these questions matter most.
import { heapPressure, memoryHeaps, usedHeaps, type MemoryDatabase } from "./memory_heaps.js";
import { HEAP_OCCUPANCY_LOW, heapOccupancy, metalMemory, type MetalMemory } from "./metal/metal_memory.js";
import { memoryTimeline, memoryVerdict } from "./memory_timeline.js";
import { Checkbox } from "./widget/checkbox.js";
import { collapsible } from "./widget/collapsible.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { TextInput } from "./widget/text_input.js";
import { Widget } from "./widget/widget.js";
import { fmt, formatBytes, isObject, num, str, type ObjectLookup, type VulkanObject } from "./vulkan/vulkan_object.js";
import { objectLink } from "./args_view.js";
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

/**
 * What the application has actually allocated from each heap, and what the driver says is resident
 * (renderer/memory_heaps.ts). The Memory section above is the device's own description — the same
 * for every application on this GPU; this one is about this run.
 */
function renderMemoryUse(container: Widget, db: MemoryDatabase | null): void {
  const m = db ? memoryHeaps(db) : null;
  const heaps = m && m.allocations ? m : null;
  // Metal has no heap table to break down; what it has is the size of every resource, which totals
  // by object kind instead (renderer/metal/metal_memory.ts).
  const metal = !heaps && db ? metalMemory(db) : null;
  const series = db?.memorySamples?.length ? db.memorySamples : null;
  if (!heaps && !metal && !series) return;
  const s = section(container, heaps
    ? `Memory Use (${heaps.allocations} allocations, ${formatBytes(heaps.totalBytes)})`
    : metal ? `Memory Use (${formatBytes(metal.totalBytes)})`
      : "Memory Use");
  if (heaps) renderHeapRows(s, heaps);
  if (metal) renderMetalRows(s, metal);
  renderMemoryOverTime(s, db);
}

/**
 * Metal's breakdown: by what is holding the memory rather than by which heap it came from, since
 * there are no heaps to enumerate. Resources inside a heap are named apart from the total, because
 * their bytes are the heap's reservation and counting both would count them twice.
 */
function renderMetalRows(s: Widget, m: MetalMemory): void {
  for (const g of m.groups) {
    row(s, g.label, `${formatBytes(g.bytes)} in ${g.count} object${g.count === 1 ? "" : "s"}, largest ${formatBytes(g.largestBytes)}`);
  }
  if (m.inHeaps.count) {
    row(s, "In heaps", `${m.inHeaps.count} resource${m.inHeaps.count === 1 ? "" : "s"} suballocated from the heaps above, using ${formatBytes(m.inHeaps.bytes)} of what they reserved`);
  }
  const occupancy = heapOccupancy(m);
  if (occupancy !== null && occupancy < HEAP_OCCUPANCY_LOW) {
    row(s, "Mostly empty", `The heaps report ${formatBytes(m.heapUsedBytes)} in use of ${formatBytes(m.heapReservedBytes)} reserved (${(100 * occupancy).toFixed(0)}%): the rest is memory this process has taken and is not using.`);
  }
  new Div(s, { text: "Totalled from the resources the inspector has seen created, which is not the whole story: the device's own figure in the series below includes what the driver allocated behind them. Metal reports no heap table and no residency separate from that figure.", class: "text-muted capture-note" });
}

/** The per-heap breakdown, for the backends that have one. */
function renderHeapRows(s: Widget, m: NonNullable<ReturnType<typeof memoryHeaps>>): void {
  for (const h of usedHeaps(m)) {
    const share = h.share === null ? "" : `  ${(100 * h.share).toFixed(h.share < 0.01 ? 2 : 1)}% of the heap`;
    row(s, `Heap ${h.index}${h.deviceLocal ? " (device local)" : ""}`,
        `${formatBytes(h.bytes)} in ${h.allocations} allocation${h.allocations === 1 ? "" : "s"}, largest ${formatBytes(h.largestBytes)}${share}`);
    // The driver's view counts every process, so it is normally larger than ours.
    if (h.budgetBytes !== undefined && h.usageBytes !== undefined) {
      row(s, "", `driver: ${formatBytes(h.usageBytes)} resident of ${formatBytes(h.budgetBytes)} available to this process`);
    }
    for (const t of h.types) {
      row(s, "", `type ${t.index} ${flagsText(t.propertyFlags) || "(no flags)"}: ${formatBytes(t.bytes)} in ${t.allocations}`);
    }
  }
  for (const h of heapPressure(m)) {
    row(s, "Nearly full", `Heap ${h.index} is close to its limit; an allocation failure here is a device-lost or an out-of-memory away.`);
  }
  if (!m.hasBudget) {
    // Both backends can fail to report it, for their own reasons, so the note names neither device.
    new Div(s, { text: "The driver did not report residency (Vulkan needs VK_EXT_memory_budget; D3D12 needs an adapter new enough for QueryVideoMemoryInfo), so how much is resident and how much it will allow are not known — only what this application asked for.", class: "text-muted capture-note" });
  }
}


/**
 * "Over time": the memory series drawn as a line, with what its shape means
 * (renderer/memory_timeline.ts). The rows above are an instant, and an instant cannot tell a leak
 * from a pool that happens to be full — only the direction can.
 */
function renderMemoryOverTime(container: Widget, db: MemoryDatabase | null): void {
  const t = db?.memorySamples ? memoryTimeline(db.memorySamples) : null;
  if (!t) return;
  row(container, "Over time", memoryVerdict(t));

  // Plotted against the sample's own frame number, so a stall does not stretch the line.
  const first = t.points[0].frame;
  const span = Math.max(1, t.points[t.points.length - 1].frame - first);
  // A flat series would otherwise divide by zero; give it a floor so the line sits mid-height.
  const low = t.minBytes;
  const height = Math.max(1, t.maxBytes - low);
  const points = t.points
    .map((p) => `${(((p.frame - first) / span) * 100).toFixed(2)},${(100 - ((p.allocated - low) / height) * 100).toFixed(2)}`)
    .join(" ");
  const line = new Div(container, { class: "memory-chart" });
  // preserveAspectRatio="none" lets the 0-100 space stretch to whatever width the panel has.
  line.element.innerHTML =
    `<svg viewBox="0 0 100 100" preserveAspectRatio="none" class="memory-chart-svg" aria-hidden="true">`
    + `<polyline points="${points}" class="memory-chart-line" vector-effect="non-scaling-stroke" /></svg>`;
  const scale = new Div(container, { class: "memory-chart-scale" });
  new Span(scale, { text: formatBytes(t.maxBytes) });
  new Span(scale, { text: `${t.points.length} samples, ${t.frames} frames` });
  new Span(scale, { text: formatBytes(t.minBytes) });
}

export function renderPhysicalDeviceSections(container: Widget, object: VulkanObject, db: MemoryDatabase | null = null): void {
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

  renderMemoryUse(container, db);

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

// ---------------------------------------------------------------------------------------------
// D3D12: the device's feature level and CheckFeatureSupport answers (src/d3d12/README.md, "Device
// sections"), and the adapter's DXGI_ADAPTER_DESC.

/** "D3D_FEATURE_LEVEL_12_1" -> "12.1"; "D3D_SHADER_MODEL_6_6" -> "6.6". */
function dottedLevel(v: ArgValue | undefined): string {
  const s = str(v);
  const m = /_(\d+)_(\d+)$/.exec(s);
  return m ? `${m[1]}.${m[2]}` : fmt(s);
}

/** The rows of one CheckFeatureSupport struct: every member, nested ones flattened with a dot. */
function optionRows(obj: ArgObject, prefix = ""): [string, string][] {
  const out: [string, string][] = [];
  for (const [k, v] of Object.entries(obj)) {
    if (isObject(v)) out.push(...optionRows(v, `${prefix}${k}.`));
    else out.push([`${prefix}${k}`, valueText(v)]);
  }
  return out;
}

export function renderD3D12DeviceSections(container: Widget, object: VulkanObject, db: ObjectLookup | null = null): void {
  const a = object.args ?? {};
  const u = object.updates;
  const features = isObject(u.features) ? u.features : null;
  const p = section(container, "Device");
  const adapter = db?.getObject(object.parentId) ?? null;
  if (adapter) {
    const r = row(p, "Adapter", "");
    objectLink(r, adapter, () => undefined);
    const desc = isObject(adapter.updates.Desc) ? adapter.updates.Desc : isObject(adapter.args?.Desc) ? adapter.args.Desc : null;
    if (desc) new Span(r, { text: `  ${str(desc.Description)}`, class: "text-muted" });
  }
  const requested = a.MinimumFeatureLevel ?? a.featureLevel;
  if (requested !== undefined) row(p, "Feature level requested", dottedLevel(requested));
  const levels = features && isObject(features.FEATURE_LEVELS) ? features.FEATURE_LEVELS : null;
  if (levels && levels.MaxSupportedFeatureLevel !== undefined) row(p, "Feature level supported", dottedLevel(levels.MaxSupportedFeatureLevel));
  const sm = features && isObject(features.SHADER_MODEL) ? features.SHADER_MODEL : null;
  if (sm && sm.HighestShaderModel !== undefined) row(p, "Shader model", dottedLevel(sm.HighestShaderModel));
  const rootSig = features && isObject(features.ROOT_SIGNATURE) ? features.ROOT_SIGNATURE : null;
  if (rootSig && rootSig.HighestVersion !== undefined) row(p, "Root signature", fmt(rootSig.HighestVersion).replace(/^ROOT_SIGNATURE_VERSION_/, "").replace("_", "."));
  const arch = features && isObject(features.ARCHITECTURE1) ? features.ARCHITECTURE1 : features && isObject(features.ARCHITECTURE) ? features.ARCHITECTURE : null;
  if (arch) {
    const parts = [arch.TileBasedRenderer ? "tile-based" : "immediate-mode", arch.UMA ? (arch.CacheCoherentUMA ? "cache-coherent UMA" : "UMA") : "discrete memory", arch.IsolatedMMU ? "isolated MMU" : ""].filter(Boolean);
    row(p, "Architecture", parts.join(", "));
  }
  if (!features) {
    new Div(container, { text: "The library did not report this device's features (CheckFeatureSupport answers arrive as an update after creation).", class: "text-muted capture-note" });
    return;
  }
  // One filterable table per option struct (D3D12_OPTIONS, D3D12_OPTIONS1, ...), in the order the library sent them.
  for (const [name, value] of Object.entries(features)) {
    if (!isObject(value)) continue;
    const rows = optionRows(value);
    if (!rows.length) continue;
    filterableRows(section(container, `${name} (${rows.length})`, true), rows, "option name or value...");
  }
}

export function renderDxgiAdapterSections(container: Widget, object: VulkanObject, db: MemoryDatabase | null = null): void {
  const desc = isObject(object.updates.Desc) ? object.updates.Desc : isObject(object.args?.Desc) ? object.args.Desc : isObject(object.args?.pDesc) ? object.args.pDesc : null;
  if (!desc) {
    new Div(container, { text: "The library did not report this adapter's description.", class: "text-muted capture-note" });
    return;
  }
  const vendor = num(desc.VendorId);
  const p = section(container, "Adapter");
  row(p, "Description", str(desc.Description));
  row(p, "Vendor", `${VENDORS[vendor] ?? "unknown"}  ${hex(vendor)}`);
  row(p, "Device ID", hex(num(desc.DeviceId)));
  if (desc.SubSysId !== undefined) row(p, "Subsystem ID", hex(num(desc.SubSysId)));
  if (desc.Revision !== undefined) row(p, "Revision", String(num(desc.Revision)));
  if (desc.DedicatedVideoMemory !== undefined) row(p, "Dedicated video memory", formatBytes(num(desc.DedicatedVideoMemory)));
  if (desc.DedicatedSystemMemory !== undefined) row(p, "Dedicated system memory", formatBytes(num(desc.DedicatedSystemMemory)));
  if (desc.SharedSystemMemory !== undefined) row(p, "Shared system memory", formatBytes(num(desc.SharedSystemMemory)));
  if (desc.Flags !== undefined && flagsText(desc.Flags)) row(p, "Flags", flagsText(desc.Flags));
  if (desc.GraphicsPreemptionGranularity !== undefined) row(p, "Graphics preemption", fmt(desc.GraphicsPreemptionGranularity).replace(/^GRAPHICS_PREEMPTION_/, "").toLowerCase().replace(/_/g, " "));
  if (desc.ComputePreemptionGranularity !== undefined) row(p, "Compute preemption", fmt(desc.ComputePreemptionGranularity).replace(/^COMPUTE_PREEMPTION_/, "").toLowerCase().replace(/_/g, " "));
  const luid = isObject(desc.AdapterLuid) ? desc.AdapterLuid : null;
  if (luid) row(p, "LUID", `${hex(num(luid.HighPart))}:${hex(num(luid.LowPart))}`);

  // The adapter is where D3D12 reports its memory segments, as Vulkan does on the physical
  // device (src/d3d12/src/cpu_timeline.h).
  renderMemoryUse(container, db);
}

/**
 * What a Metal device reports about memory. Metal has no heap table to enumerate and no separate
 * residency figure — `currentAllocatedSize` is both — so this is the series and nothing else
 * (src/metal/src/cpu_timeline.h).
 */
export function renderMetalDeviceSections(container: Widget, db: MemoryDatabase | null = null): void {
  renderMemoryUse(container, db);
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
