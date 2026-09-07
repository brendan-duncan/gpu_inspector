import type {
  AppConfig, LaunchConfig, LaunchResult, SessionInfo, SessionLogMessage, SessionMessages, SessionStatusMessage,
  ShaderTextMode, ShaderTextResult, UiRequest,
} from "../shared/protocol.js";

export interface InspectorApi {
  getConfig(): Promise<AppConfig>;
  /** Launches an application in a new session, shown in the main window. */
  launch(config: LaunchConfig): Promise<LaunchResult>;
  /** Connects to an already running application in a new session, shown in the main window. */
  connect(port: number): Promise<LaunchResult>;
  /** Terminates the session's application; the session stays open. */
  kill(sessionId: number): Promise<boolean>;
  /** Terminates the session's application and launches it again with the same configuration. */
  restart(sessionId: number): Promise<LaunchResult>;
  /** Terminates the application and removes the session. */
  closeSession(sessionId: number): Promise<boolean>;
  openSessionWindow(sessionId: number): Promise<boolean>;
  moveSessionToMain(sessionId: number): Promise<boolean>;
  /** Asks the layer to resend its live object snapshot. */
  refresh(sessionId: number): Promise<boolean>;
  send(sessionId: number, msg: UiRequest): Promise<boolean>;
  onSessionAdded(cb: (info: SessionInfo) => void): void;
  onSessionRemoved(cb: (sessionId: number) => void): void;
  onMessages(cb: (batch: SessionMessages) => void): void;
  onStatus(cb: (status: SessionStatusMessage) => void): void;
  onLog(cb: (line: SessionLogMessage) => void): void;

  chooseFile(opts?: { title?: string; directory?: boolean }): Promise<string | null>;
  getRecents(): Promise<LaunchConfig[]>;
  removeRecent(index: number): Promise<LaunchConfig[]>;
  clearRecents(): Promise<LaunchConfig[]>;
  onRecents(cb: (recents: LaunchConfig[]) => void): void;
  shaderText(spirv: Uint8Array, mode: ShaderTextMode): Promise<ShaderTextResult>;
}

declare global {
  interface Window {
    inspector: InspectorApi;
  }
}
