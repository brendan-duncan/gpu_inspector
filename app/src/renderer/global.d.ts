import type {
  AndroidDeviceList, AppConfig, LaunchConfig, LaunchResult, SessionInfo, SessionLogMessage, SessionMessages, SessionStatusMessage,
  CompileShaderResult, OpenFileOptions, SaveFileOptions, ShaderLanguage, ShaderTextMode, ShaderTextResult, ThemeName, UiRequest, UpdateStatus, StackFrame, ImplicitLayerStatus,
} from "../shared/protocol.js";

export interface InspectorApi {
  getConfig(): Promise<AppConfig>;
  /** Persists the theme and applies it to every window. */
  setTheme(theme: ThemeName): Promise<boolean>;
  onTheme(cb: (theme: ThemeName) => void): void;
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
  /** Third-party packages installed on an Android device. */
  androidPackages(serial: string): Promise<string[]>;
  /** Terminates the session's application; the session stays open. */
  kill(sessionId: number): Promise<boolean>;
  /** Terminates the session's application and launches it again with the same configuration. */
  restart(sessionId: number): Promise<LaunchResult>;
  /** Terminates the application and removes the session. */
  closeSession(sessionId: number): Promise<boolean>;
  openSessionWindow(sessionId: number): Promise<boolean>;
  /** Opens a capture in a window of its own: a file by path, or bytes (written to a temporary file). */
  openCaptureWindow(opts: { path?: string; data?: Uint8Array; name?: string }): Promise<boolean>;
  /**
   * Vulkan: replays a capture (its file bytes) on this machine's GPU with vkinsp_replay to measure
   * every pass's overdraw. `data` is the tool's --overdraw-data file (renderer/overdraw.ts parses it).
   */
  measureOverdraw(opts: { data: Uint8Array; name?: string }): Promise<{ data: Uint8Array | null; error?: string; output: string }>;
  /**
   * Vulkan: replays a capture (its file bytes) following one pixel of an image through the frame.
   * `data` is the tool's --pixel-data JSON (renderer/pixel_history.ts parses it).
   */
  pixelHistory(opts: { data: Uint8Array; name?: string; pixel: { image: number; x: number; y: number; mip?: number; layer?: number } }): Promise<{ data: Uint8Array | null; error?: string; output: string }>;
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
  shaderText(spirv: Uint8Array, mode: ShaderTextMode): Promise<ShaderTextResult>;
  /** Compiles shader source to SPIR-V with the Vulkan SDK's compilers (shader editor). */
  compileShader(source: string, language: ShaderLanguage, stage: string, entryPoint: string, spirvVersion: string): Promise<CompileShaderResult>;
}

declare global {
  interface Window {
    inspector: InspectorApi;
  }
}
