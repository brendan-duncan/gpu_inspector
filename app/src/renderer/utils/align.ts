// Ported from WebGPU Inspector (MIT): src/utils/align.js
export function alignTo(size: number, alignment: number): number {
  return (size + alignment - 1) & ~(alignment - 1);
}
