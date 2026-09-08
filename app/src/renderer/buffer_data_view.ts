// Shows the bytes of a captured buffer as typed values: structs as nested lists, arrays with
// offset/count paging when they are long, matrices one row per line, vertex attributes decoded
// by their VkFormat. Follows WebGPU Inspector's _showBufferDataType (capture_panel.js).
import { Div } from "./widget/div.js";
import { NumberInput } from "./widget/number_input.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import { float16ToFloat32 } from "./utils/float.js";
import { sizeOf, typeName, type ReflType, type ScalarType } from "./vulkan/spirv_reflect.js";
import { vertexFormat } from "./vulkan/vk_format.js";

export type Radix = 10 | 16 | 8 | 2;

/** Elements shown per page of a long array. */
const PAGE = 100;

function formatNumber(v: number, radix: Radix, isFloat: boolean): string {
  if (!Number.isFinite(v)) return String(v);
  if (isFloat) {
    if (radix === 10) return formatFloat(v);
    // Other radixes show the raw bits of a float.
    const buf = new DataView(new ArrayBuffer(4));
    buf.setFloat32(0, v, true);
    return prefix(radix) + buf.getUint32(0, true).toString(radix);
  }
  if (radix === 10) return String(v);
  if (v < 0) return "-" + prefix(radix) + (-v).toString(radix);
  return prefix(radix) + v.toString(radix);
}

function prefix(radix: Radix): string {
  return radix === 16 ? "0x" : radix === 8 ? "0o" : radix === 2 ? "0b" : "";
}

function formatFloat(v: number): string {
  if (v === 0) return Object.is(v, -0) ? "-0" : "0";
  const a = Math.abs(v);
  if (a >= 1e7 || a < 1e-5) return v.toExponential(5);
  return String(Number(v.toPrecision(7)));
}

function readScalar(view: DataView, offset: number, s: ScalarType): number {
  if (offset + s.size > view.byteLength) return NaN;
  switch (s.base) {
    case "float":
      return s.width === 16 ? float16ToFloat32(view.getUint16(offset, true)) : s.width === 64 ? view.getFloat64(offset, true) : view.getFloat32(offset, true);
    case "int":
      return s.width === 8 ? view.getInt8(offset) : s.width === 16 ? view.getInt16(offset, true) : s.width === 64 ? Number(view.getBigInt64(offset, true)) : view.getInt32(offset, true);
    case "uint":
    case "bool":
      return s.width === 8 ? view.getUint8(offset) : s.width === 16 ? view.getUint16(offset, true) : s.width === 64 ? Number(view.getBigUint64(offset, true)) : view.getUint32(offset, true);
  }
}

function scalarText(view: DataView, offset: number, s: ScalarType, radix: Radix): string {
  if (offset + s.size > view.byteLength) return "<out of range>";
  const v = readScalar(view, offset, s);
  if (s.base === "bool") return v ? "true" : "false";
  return formatNumber(v, radix, s.base === "float");
}

/** One-line text of a scalar, vector or vertex-format value; null for anything bigger. */
export function inlineValue(type: ReflType, view: DataView, offset: number, radix: Radix): string | null {
  switch (type.kind) {
    case "scalar":
      return scalarText(view, offset, type, radix);
    case "vector": {
      const parts: string[] = [];
      for (let i = 0; i < type.count; i++) parts.push(scalarText(view, offset + i * type.element.size, type.element, radix));
      return parts.join(", ");
    }
    case "format": {
      const f = vertexFormat(type.format);
      if (!f) return `<${typeName(type)}>`;
      if (offset + f.size > view.byteLength) return "<out of range>";
      return f.read(view, offset).map((v) => formatNumber(v, radix, !f.integer)).join(", ");
    }
    case "opaque":
      return `<${type.name}>`;
    default:
      return null;
  }
}

/**
 * Renders `type` read from `data` at `offset` into `parent` as list items (the caller supplies
 * a <ul> or any container). Large arrays get paging controls.
 */
export function renderTypedData(parent: Widget, type: ReflType, data: Uint8Array, offset = 0, radix: Radix = 10): void {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  renderInto(parent, type, view, offset, radix);
}

function renderInto(ui: Widget, type: ReflType, view: DataView, offset: number, radix: Radix): void {
  const inline = inlineValue(type, view, offset, radix);
  if (inline !== null) {
    new Widget("li", ui, { text: inline, class: "buffer-value" });
    return;
  }
  switch (type.kind) {
    case "matrix": {
      // Column-major: element (col c, row r) at c * stride + r * elemSize; row-major swaps them.
      const e = type.element;
      for (let r = 0; r < type.rows; r++) {
        const cells: string[] = [];
        for (let c = 0; c < type.columns; c++) {
          const at = type.rowMajor ? r * type.stride + c * e.size : c * type.stride + r * e.size;
          cells.push(scalarText(view, offset + at, e, radix));
        }
        new Widget("li", ui, { text: cells.join("  "), class: "buffer-value buffer-matrix-row" });
      }
      return;
    }
    case "struct": {
      const list = new Widget("ul", ui, { class: "buffer-struct" });
      // Members entirely past the captured bytes are summarized in one line: a Vulkan binding may
      // cover only the part of a block the shader reads (Unity binds constant buffers that way).
      const beyond: string[] = [];
      for (const m of type.members) {
        const memberOffset = offset + m.offset;
        if (memberOffset >= view.byteLength) {
          beyond.push(m.name);
          continue;
        }
        const one = inlineValue(m.type, view, memberOffset, radix);
        if (one !== null) {
          const li = new Widget("li", list);
          new Span(li, { text: `${m.name}: `, class: "buffer-member" });
          new Span(li, { text: one, class: "buffer-value" });
          new Span(li, { text: `  ${typeName(m.type)}`, class: "buffer-type" });
        } else {
          const li = new Widget("li", list);
          new Span(li, { text: `${m.name}: `, class: "buffer-member" });
          new Span(li, { text: typeName(m.type), class: "buffer-type" });
          new Span(li, { text: `  @${m.offset}`, class: "buffer-offset" });
          const sub = new Widget("ul", li);
          renderInto(sub, m.type, view, memberOffset, radix);
        }
      }
      if (beyond.length) {
        const names = beyond.length > 6 ? `${beyond.slice(0, 6).join(", ")}, ...` : beyond.join(", ");
        new Widget("li", list, { text: `${beyond.length} member${beyond.length === 1 ? "" : "s"} past the ${view.byteLength - offset} bound bytes (not read by this shader): ${names}`, class: "text-muted buffer-beyond" });
      }
      return;
    }
    case "array": {
      renderArray(ui, type.element, type.count, type.stride, view, offset, radix);
      return;
    }
    default:
      return;
  }
}

function renderArray(ui: Widget, element: ReflType, count: number, stride: number, view: DataView, offset: number, radix: Radix): void {
  if (stride <= 0) stride = Math.max(1, sizeOf(element));
  if (count === 0) {
    // Runtime-sized: as many elements as the captured bytes hold.
    count = Math.max(0, Math.floor((view.byteLength - offset) / stride));
  }
  const container = new Div(ui, { class: "buffer-array" });
  const list = new Widget("ul", container);
  let start = 0;
  let pageSize = PAGE;

  const fill = (): void => {
    list.html = "";
    const end = Math.min(count, start + pageSize);
    for (let i = start; i < end; i++) {
      const at = offset + i * stride;
      const one = inlineValue(element, view, at, radix);
      if (one !== null) {
        const li = new Widget("li", list);
        new Span(li, { text: `[${i}]: `, class: "buffer-member" });
        new Span(li, { text: one, class: "buffer-value" });
      } else {
        const li = new Widget("li", list);
        new Span(li, { text: `[${i}]: `, class: "buffer-member" });
        new Span(li, { text: typeName(element), class: "buffer-type" });
        const sub = new Widget("ul", li);
        renderInto(sub, element, view, at, radix);
      }
    }
    if (count === 0) new Widget("li", list, { text: "(empty)", class: "text-muted" });
  };

  if (count > PAGE) {
    const filter = new Div(null, { class: "buffer-array-filter" });
    container.insertBefore(filter, list);
    new Span(filter, { text: "Offset:" });
    new NumberInput(filter, { value: 0, min: 0, max: count, precision: 0, step: 1, tooltip: "First element to display",
      onChange: (v: string) => {
        start = Math.max(0, Math.min(count - 1, Math.floor(Number(v)) || 0));
        fill();
      } });
    new Span(filter, { text: "Count:" });
    new NumberInput(filter, { value: PAGE, min: 1, max: 1000, precision: 0, step: 1, tooltip: "Number of elements to display",
      onChange: (v: string) => {
        pageSize = Math.max(1, Math.min(1000, Math.floor(Number(v)) || PAGE));
        fill();
      } });
    new Span(filter, { text: `/ ${count}` });
  }
  fill();
}

/** Renders a flat list of index values with the same paging as arrays. */
export function renderIndexData(parent: Widget, view: DataView, indexType: string, firstIndex: number, indexCount: number): void {
  const bytes = indexType.includes("UINT32") ? 4 : indexType.includes("UINT8") ? 1 : 2;
  const available = Math.floor(view.byteLength / bytes);
  const element: ScalarType = { kind: "scalar", base: "uint", width: bytes * 8, size: bytes };
  const start = Math.min(firstIndex, available);
  const count = indexCount > 0 ? Math.min(indexCount, available - start) : available - start;
  const note = new Div(parent, { class: "text-muted font-sm" });
  note.text = `${available} indices captured${indexCount > 0 ? `, draw uses [${firstIndex}..${firstIndex + indexCount - 1}]` : ""}`;
  renderArray(parent, element, Math.max(0, count), bytes, view, start * bytes, 10);
}
