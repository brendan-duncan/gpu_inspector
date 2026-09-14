// Message names exchanged with the capture layer. Follows WebGPU Inspector's actions.js.

// Layer -> UI
export const Actions = {
  Snapshot: "Snapshot",
  AddObject: "AddObject",
  DeleteObjects: "DeleteObjects",
  ObjectSetLabel: "ObjectSetLabel",
  FrameStats: "FrameStats",
  ObjectBlob: "ObjectBlob",
  ObjectBlobs: "ObjectBlobs",
  ObjectUpdate: "ObjectUpdate",
  Pong: "Pong",
  CaptureFrameResults: "CaptureFrameResults",
  CaptureFrameCommands: "CaptureFrameCommands",
  CaptureBuffers: "CaptureBuffers",
  CaptureBufferData: "CaptureBufferData",
  CaptureTextureFrames: "CaptureTextureFrames",
  CaptureTextureData: "CaptureTextureData",
} as const;

// UI -> Layer
export const PanelActions = {
  Ping: "Ping",
  Capture: "Capture",
  Settings: "Settings",
  RequestBlob: "RequestBlob",
  RequestTexture: "RequestTexture",
} as const;
