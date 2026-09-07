// Reassembles a frame capture streamed by the layer: command batches, then render target
// descriptors, then their pixel data. Follows WebGPU Inspector's capture_data.js.
import { Signal } from "./utils/signal.js";
import type { CaptureCommand, CaptureTextureInfo, LayerMessage } from "../shared/protocol.js";

export interface CapturedTexture {
  info: CaptureTextureInfo;
  data: Uint8Array | null;
  canvas: HTMLCanvasElement | null;   // set by the panel while waiting for data
}

export class CaptureData {
  frame = 0;
  commands: CaptureCommand[] = [];
  textures: CapturedTexture[] = [];
  private _expectedCommands = 0;

  readonly onCaptureStatus = new Signal<(text: string) => void>();
  readonly onCommandsComplete = new Signal<() => void>();
  readonly onTextureLoaded = new Signal<(texture: CapturedTexture) => void>();
  readonly onTexturesAnnounced = new Signal<() => void>();

  reset(): void {
    this.frame = 0;
    this.commands = [];
    this.textures = [];
    this._expectedCommands = 0;
  }

  texturesForPass(commandBufferId: number, passIndex: number): CapturedTexture[] {
    return this.textures.filter((t) => t.info.commandBuffer === commandBufferId && t.info.passIndex === passIndex)
      .sort((a, b) => a.info.attachment - b.info.attachment);
  }

  handleMessage(msg: LayerMessage): void {
    switch (msg.action) {
      case "CaptureFrameResults":
        this.reset();
        this.frame = msg.frame;
        this._expectedCommands = msg.count;
        this.onCaptureStatus.emit(`receiving ${msg.count} commands...`);
        if (msg.count === 0) this.onCommandsComplete.emit();
        break;
      case "CaptureFrameCommands":
        for (const c of msg.commands) this.commands[c.index] = c;
        if (this.commands.length >= this._expectedCommands) {
          this.onCaptureStatus.emit(`${this.commands.length} commands`);
          this.onCommandsComplete.emit();
        }
        break;
      case "CaptureTextureFrames":
        this.textures = msg.textures.map((info) => ({ info, data: null, canvas: null }));
        this.onTexturesAnnounced.emit();
        break;
      case "CaptureTextureData": {
        const tex = this.textures.find((t) => t.info.commandBuffer === msg.commandBuffer && t.info.passIndex === msg.passIndex && t.info.attachment === msg.attachment);
        if (tex) {
          tex.data = msg.__binary ?? null;
          this.onTextureLoaded.emit(tex);
        }
        break;
      }
      default:
        break;
    }
  }
}
