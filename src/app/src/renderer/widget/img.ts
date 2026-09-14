// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Widget, WidgetOptions } from './widget.js';

export interface ImgOptions extends WidgetOptions {
  src?: string;
}

export class Img extends Widget<HTMLImageElement> {
  constructor(parent?: Widget | HTMLElement | ImgOptions | null, options?: ImgOptions) {
    super('img', parent, options);
  }

  get src(): string {
    return this.element.src;
  }

  set src(v: string) {
    this.element.src = v;
  }

  override configure(options: ImgOptions): void {
    if (!options) {
      return;
    }
    super.configure(options);
    if (options.src !== undefined) {
      this.element.src = options.src;
    }
  }
}
