// Exposes a small, explicit API to the renderer (see renderer/global.d.ts for the typed surface).
import { contextBridge, ipcRenderer, webUtils } from "electron";

contextBridge.exposeInMainWorld("inspector", {
  getConfig: () => ipcRenderer.invoke("inspector:getConfig"),
  setTheme: (theme: string) => ipcRenderer.invoke("inspector:setTheme", theme),
  onTheme: (cb: (theme: string) => void) => ipcRenderer.on("inspector:theme", (_e, t) => cb(t)),
  // Self-update (installed builds)
  checkForUpdates: () => ipcRenderer.invoke("inspector:checkForUpdates"),
  downloadUpdate: () => ipcRenderer.invoke("inspector:downloadUpdate"),
  installUpdate: () => ipcRenderer.invoke("inspector:installUpdate"),
  onUpdate: (cb: (status: unknown) => void) => ipcRenderer.on("inspector:update", (_e, s) => cb(s)),
  // Sessions
  launch: (config: unknown) => ipcRenderer.invoke("inspector:launch", config),
  connect: (port: number) => ipcRenderer.invoke("inspector:connect", port),
  androidDevices: () => ipcRenderer.invoke("inspector:androidDevices"),
  androidPackages: (serial: string) => ipcRenderer.invoke("inspector:androidPackages", serial),
  kill: (sessionId: number) => ipcRenderer.invoke("inspector:kill", sessionId),
  restart: (sessionId: number) => ipcRenderer.invoke("inspector:restart", sessionId),
  closeSession: (sessionId: number) => ipcRenderer.invoke("inspector:closeSession", sessionId),
  openSessionWindow: (sessionId: number) => ipcRenderer.invoke("inspector:openSessionWindow", sessionId),
  moveSessionToMain: (sessionId: number) => ipcRenderer.invoke("inspector:moveSessionToMain", sessionId),
  openCaptureWindow: (opts: unknown) => ipcRenderer.invoke("inspector:openCaptureWindow", opts),
  openCaptureInMain: (path: string) => ipcRenderer.invoke("inspector:openCaptureInMain", path),
  onOpenCapture: (cb: (path: string) => void) => ipcRenderer.on("inspector:openCapture", (_e, p) => cb(p)),
  refresh: (sessionId: number) => ipcRenderer.invoke("inspector:refresh", sessionId),
  send: (sessionId: number, msg: unknown) => ipcRenderer.invoke("inspector:send", sessionId, msg),
  onSessionAdded: (cb: (info: unknown) => void) => ipcRenderer.on("inspector:sessionAdded", (_e, info) => cb(info)),
  onSessionRemoved: (cb: (sessionId: number) => void) => ipcRenderer.on("inspector:sessionRemoved", (_e, id) => cb(id)),
  onMessages: (cb: (batch: unknown) => void) => ipcRenderer.on("inspector:messages", (_e, batch) => cb(batch)),
  onStatus: (cb: (s: unknown) => void) => ipcRenderer.on("inspector:status", (_e, s) => cb(s)),
  onLog: (cb: (line: unknown) => void) => ipcRenderer.on("inspector:log", (_e, line) => cb(line)),
  // Recents, files, tools
  chooseFile: (opts: unknown) => ipcRenderer.invoke("inspector:chooseFile", opts),
  saveFile: (opts: unknown, data: Uint8Array) => ipcRenderer.invoke("inspector:saveFile", opts, data),
  readFile: (path: string) => ipcRenderer.invoke("inspector:readFile", path),
  addRecentCapture: (path: string) => ipcRenderer.invoke("inspector:addRecentCapture", path),
  removeRecentCapture: (index: number) => ipcRenderer.invoke("inspector:removeRecentCapture", index),
  onRecentCaptures: (cb: (list: string[]) => void) => ipcRenderer.on("inspector:recentCaptures", (_e, l) => cb(l)),
  pathForFile: (file: File) => webUtils.getPathForFile(file),
  getRecents: () => ipcRenderer.invoke("inspector:getRecents"),
  removeRecent: (index: number) => ipcRenderer.invoke("inspector:removeRecent", index),
  clearRecents: () => ipcRenderer.invoke("inspector:clearRecents"),
  onRecents: (cb: (recents: unknown[]) => void) => ipcRenderer.on("inspector:recents", (_e, r) => cb(r)),
  shaderText: (spirv: Uint8Array, mode: string) => ipcRenderer.invoke("inspector:shaderText", spirv, mode),
  compileShader: (source: string, language: string, stage: string, entryPoint: string, spirvVersion: string) =>
    ipcRenderer.invoke("inspector:compileShader", source, language, stage, entryPoint, spirvVersion),
});
