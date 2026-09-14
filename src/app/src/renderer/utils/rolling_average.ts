// Ported from WebGPU Inspector (MIT): src/utils/rolling_average.js
export class RollingAverage {
  windowSize: number;
  buffer: number[];
  sum: number;

  constructor(windowSize: number) {
    this.windowSize = windowSize;
    this.buffer = [];
    this.sum = 0;
  }

  add(frameTime: number): void {
    this.buffer.push(frameTime);
    if (this.buffer.length > this.windowSize) {
      // buffer.length > windowSize >= 1 here, so shift() always yields a number.
      this.sum -= this.buffer.shift() as number;
    }
    this.sum += frameTime;
  }

  get average(): number {
    if (this.buffer.length === 0) {
      return 0;
    }
    return this.sum / this.buffer.length;
  }
}
