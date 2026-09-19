// Export to C++: what vkinsp_replay --export wrote (its --export-data summary, WriteExportData in
// src/replay/src/main.cpp), and how the capture bar says it. The export itself is the replay tool's:
// the frame is replayed on this machine's GPU and written, as it replays, as a standalone C++ project
// (src/replay/src/exporter.h, docs/REPLAY.md).

/** The summary of one export. */
export interface ExportCppSummary {
  device: string;
  directory: string;
  ok: boolean;
  error: string;
  objects: number;
  commands: number;
  submissions: number;
  /** Render targets the exported program reads back and compares with the capture's copies. */
  targets: number;
  /** Commands the source leaves out, as the replay left them out. */
  leftOut: number;
  dataBytes: number;
  files: string[];
  notes: string[];
  problems: string[];
}

export function parseExportSummary(data: Uint8Array): ExportCppSummary {
  const root = JSON.parse(new TextDecoder().decode(data)) as Record<string, unknown>;
  if (root.format !== "gpu-inspector-export-cpp") throw new Error("not an export summary");
  const n = (v: unknown): number => (typeof v === "number" && Number.isFinite(v) ? v : 0);
  const s = (v: unknown): string => (typeof v === "string" ? v : "");
  const list = (v: unknown): string[] => (Array.isArray(v) ? v.filter((x): x is string => typeof x === "string") : []);
  return {
    device: s(root.device), directory: s(root.directory), ok: root.ok === true, error: s(root.error),
    objects: n(root.objects), commands: n(root.commands), submissions: n(root.submissions), targets: n(root.targets),
    leftOut: n(root.leftOut), dataBytes: n(root.dataBytes), files: list(root.files), notes: list(root.notes), problems: list(root.problems),
  };
}

/** One line for the status bar. */
export function exportSummaryText(e: ExportCppSummary): string {
  if (!e.ok) return `export to C++ failed: ${e.error || "the replay wrote no project"}`;
  const parts = [
    `${e.objects} objects`,
    `${e.commands} commands in ${e.submissions} submission${e.submissions === 1 ? "" : "s"}`,
    `${(e.dataBytes / (1024 * 1024)).toFixed(1)} MB of data`,
  ];
  const caveats = [
    e.leftOut ? `${e.leftOut} command${e.leftOut === 1 ? "" : "s"} left out` : "",
    e.problems.length ? `${e.problems.length} replay problem${e.problems.length === 1 ? "" : "s"}, listed in its README` : "",
  ].filter(Boolean);
  return `exported C++ project to ${e.directory} (${parts.join(", ")})${caveats.length ? `; ${caveats.join("; ")}` : ""}`;
}

/** A folder name for a capture's project, from the capture's label. */
export function exportFolderName(label: string): string {
  const stem = label.replace(/\.gpucap$/i, "").replace(/[^\w.-]+/g, "_").replace(/^_+|_+$/g, "");
  return `${stem || "frame"}_cpp`;
}
