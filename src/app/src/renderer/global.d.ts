import type {
  AndroidDeviceList, AppConfig, LaunchConfig, LaunchResult, SessionInfo, SessionLogMessage, SessionMessages, SessionStatusMessage,
  CompileShaderResult, DebugTranslationResult, OpenFileOptions, SaveFileOptions, ShaderLanguage, ShaderTextMode, ShaderTextResult, ThemeName, UiRequest, UpdateStatus, StackFrame, ImplicitLayerStatus, UserEnvironmentStatus,
} from "../shared/protocol.js";
import type { ShaderAblation, ShaderMeasureTarget } from "./shader_ablation.js";

export interface InspectorApi {
  getConfig(): Promise<AppConfig>;
  /** Persists the theme and applies it to every window. */
  setTheme(theme: ThemeName): Promise<boolean>;
  onTheme(cb: (theme: ThemeName) => void): void;
  /** Opens a page of the user documentation in the browser: "REPORTS.md#shader-debugger"; the index when left out. */
  openDocs(page?: string): Promise<boolean>;
  /** Self-update: progress arrives through onUpdate. Installed builds only (AppConfig.canUpdate). */
  checkForUpdates(): Promise<boolean>;
  downloadUpdate(): Promise<boolean>;
  /** Quits and installs a downloaded update. */
  installUpdate(): Promise<boolean>;
  onUpdate(cb: (status: UpdateStatus) => void): void;
  /** Launches an application in a new session, shown in the main window. */
  launch(config: LaunchConfig): Promise<LaunchResult>;
  /** Connects to an already running application in a new session, shown in the main window. */
  connect(port: number): Promise<LaunchResult>;
  /** Android devices reachable through adb (launch dialog). */
  androidDevices(): Promise<AndroidDeviceList>;
  /** The implicit registration of the capture layer for this user, and switching it. */
  implicitLayer(): Promise<ImplicitLayerStatus>;
  setImplicitLayer(on: boolean): Promise<ImplicitLayerStatus>;
  /** VKINSP_ENABLE and VKINSP_PORT for the user's account, and setting them (a port) or clearing them (null). */
  userEnvironment(): Promise<UserEnvironmentStatus>;
  setUserEnvironment(port: number | null): Promise<UserEnvironmentStatus>;
  /** Third-party packages installed on an Android device. */
  androidPackages(serial: string): Promise<string[]>;
  /** Terminates the session's application; the session stays open. */
  kill(sessionId: number): Promise<boolean>;
  /** Terminates the session's application and launches it again with the same configuration. */
  restart(sessionId: number): Promise<LaunchResult>;
  /** Terminates the application and removes the session. */
  closeSession(sessionId: number): Promise<boolean>;
  openSessionWindow(sessionId: number): Promise<boolean>;
  /**
   * Opens a capture in a window of its own: a file by path, or bytes (written to a temporary file).
   * `view` names a report for the new window to open on it (see CapturePanel's report tabs).
   */
  openCaptureWindow(opts: { path?: string; data?: Uint8Array; name?: string; view?: string }): Promise<boolean>;
  /** The application's stylesheets, for a report exported as a standalone HTML file (report_export.ts). */
  appStyles(): Promise<string>;
  /*
   * Vulkan replays, on this machine's GPU with vkinsp_replay (main/replay.ts). Each names the capture
   * by a key its view chose: the main process keeps a replay alive for it, and answers `needData`
   * when it does not have the capture yet, so the view sends its bytes (`data`) once. `data` in the
   * answer is the tool's data file for the analysis.
   */
  /** Every pass's overdraw (--overdraw-data; renderer/overdraw.ts parses it). */
  measureOverdraw(opts: { key: string; data?: Uint8Array; name?: string }): Promise<{ data: Uint8Array | null; error?: string; output: string; needData?: boolean }>;
  /** Every draw measured (--draw-data). */
  measureDraws(opts: { key: string; data?: Uint8Array; name?: string }): Promise<{ data: Uint8Array | null; error?: string; output: string; needData?: boolean }>;
  /** The GPU's own hardware counters per render pass, and per draw with `perDraw` (--counter-data). */
  measureHwCounters(opts: { key: string; data?: Uint8Array; name?: string; perDraw?: boolean }): Promise<{ data: Uint8Array | null; error?: string; output: string; needData?: boolean }>;
  /** Each named draw drawn on its own (--overlay-data). */
  drawOverlay(opts: { key: string; data?: Uint8Array; name?: string; commands: number[] }): Promise<{ data: Uint8Array | null; error?: string; output: string; needData?: boolean }>;
  /** What each named draw's vertex shader wrote (--mesh-data). */
  meshOutput(opts: { key: string; data?: Uint8Array; name?: string; commands: number[] }): Promise<{ data: Uint8Array | null; error?: string; output: string; needData?: boolean }>;
  /** One pixel of an image followed through the frame (--pixel-data; renderer/pixel_history.ts parses it). */
  pixelHistory(opts: { key: string; data?: Uint8Array; name?: string; pixel: { image: number; x: number; y: number; mip?: number; layer?: number } }): Promise<{ data: Uint8Array | null; error?: string; output: string; needData?: boolean }>;
  /** A shader stage measured by ablation at a draw (--ablate-data, main/shader_ablation_run.ts); `ablation` when it was. */
  measureShader(opts: { key: string; data?: Uint8Array; name?: string; stage: ShaderMeasureTarget & { drawMs?: number | null } }): Promise<{ ablation?: ShaderAblation; error?: string; needData?: boolean }>;
  /** Stops the replay kept for a capture key and removes its file. */
  releaseReplay(key: string): Promise<void>;
  /** Frames named by module and offset only, resolved on this machine with the unstripped libraries under the directories (empty: the last ones used). */
  symbolize(frames: StackFrame[], dirs: string[]): Promise<StackFrame[]>;
  /** The text of shader source files named by debug information, found under the roots (empty: the last ones used). */
  shaderSource(names: string[], roots: string[]): Promise<Record<string, string>>;
  /** From a capture window: the main window opens the file and this window closes. */
  openCaptureInMain(path: string): Promise<boolean>;
  onOpenCapture(cb: (path: string) => void): void;
  moveSessionToMain(sessionId: number): Promise<boolean>;
  /** Asks the layer to resend its live object snapshot. */
  refresh(sessionId: number): Promise<boolean>;
  send(sessionId: number, msg: UiRequest): Promise<boolean>;
  onSessionAdded(cb: (info: SessionInfo) => void): void;
  onSessionRemoved(cb: (sessionId: number) => void): void;
  onMessages(cb: (batch: SessionMessages) => void): void;
  onStatus(cb: (status: SessionStatusMessage) => void): void;
  onLog(cb: (line: SessionLogMessage) => void): void;

  chooseFile(opts?: OpenFileOptions): Promise<string | null>;
  /** Writes `data` to the file the user picks (or to opts.path); returns the path, null when cancelled or failed. */
  saveFile(opts: SaveFileOptions, data: Uint8Array): Promise<string | null>;
  /** Reads a whole file; null when it cannot be read. */
  readFile(path: string): Promise<Uint8Array | null>;
  /** Recent capture files (saved or opened), most recent first; changes reach every window. */
  addRecentCapture(path: string): Promise<string[]>;
  removeRecentCapture(index: number): Promise<string[]>;
  onRecentCaptures(cb: (list: string[]) => void): void;
  /** The filesystem path of a File dropped onto the window. */
  pathForFile(file: File): string;
  getRecents(): Promise<LaunchConfig[]>;
  removeRecent(index: number): Promise<LaunchConfig[]>;
  clearRecents(): Promise<LaunchConfig[]>;
  onRecents(cb: (recents: LaunchConfig[]) => void): void;
  /**
   * A shader payload as text: SPIR-V through spirv-dis / spirv-cross; a D3D12 pipeline's DXBC/DXIL
   * container through dxinsp_shader.exe ("dis" its disassembly, "hlsl" its HLSL source, other
   * modes not available). `pdbDirs` (the session's symbol directories) are searched for the PDB a
   * shader built with dxc -Zs kept its source in.
   */
  shaderText(spirv: Uint8Array, mode: ShaderTextMode, pdbDirs?: string[]): Promise<ShaderTextResult>;
  /** Compiles shader source to SPIR-V with the Vulkan SDK's compilers (shader editor). */
  compileShader(source: string, language: ShaderLanguage, stage: string, entryPoint: string, spirvVersion: string): Promise<CompileShaderResult>;
  /**
   * D3D12 shader editor: HLSL compiled to DXIL with dxc for the stage (`spirv` in the result holds
   * the bytecode). `shaderModel` is the profile suffix ("6_0", the default; the pipeline's own
   * reflection names its target). #include is resolved against the session's source roots, as for
   * compileShader.
   */
  compileDxil(source: string, stage: string, entryPoint: string, shaderModel?: string): Promise<CompileShaderResult>;
  /** A SPIR-V module decompiled to GLSL and recompiled with line information, for the shader debugger. */
  decompileForDebugging(spirv: Uint8Array, stage: string, entryPoint: string): Promise<DebugTranslationResult>;
  /**
   * A D3D12 stage's HLSL (embedded, or in a PDB under `pdbDirs` and the saved symbol directories)
   * compiled to SPIR-V with line information, for the shader debugger; `target` is the stage's
   * profile from its reflection ("ps_6_0"). The result's `source` is the main file's text.
   */
  compileHlslForDebugging(bytecode: Uint8Array, stage: string, entryPoint: string, target?: string, pdbDirs?: string[]): Promise<DebugTranslationResult>;
}

declare global {
  interface Window {
    inspector: InspectorApi;
  }
}
