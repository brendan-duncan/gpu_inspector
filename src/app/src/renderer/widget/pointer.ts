// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector

/**
 * A snapshot of a PointerEvent, tracked by Widget while pointers are down.
 */
export class Pointer {
  event: PointerEvent;
  pageX: number;
  pageY: number;
  clientX: number;
  clientY: number;
  id: number;
  type: string;
  buttons: number;

  constructor(event: PointerEvent) {
    this.event = event;
    this.pageX = event.pageX;
    this.pageY = event.pageY;
    this.clientX = event.clientX;
    this.clientY = event.clientY;
    this.id = event.pointerId;
    this.type = event.pointerType;
    this.buttons = event.buttons ?? -1;
  }

  getCoalesced(): Pointer[] {
    return this.event.getCoalescedEvents().map((p) => new Pointer(p));
  }
}
