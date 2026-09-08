// Live object database fed by the layer's messages.
//
// Adapted from WebGPU Inspector's devtools/object_database.js (MIT, Brendan Duncan): the same
// dependency graph and destroy-invalidation cascade, driven by generic {__id} references found
// in each object's serialized creation arguments instead of per-class knowledge.
import { Signal } from "../utils/signal.js";
import { VulkanObject, isHandleRef, objectMemoryBytes, type ObjectLookup } from "./vulkan_object.js";
import type { AddObjectMessage, ArgValue, LayerMessage, FrameStatsMessage } from "../../shared/protocol.js";

// Vulkan lets most objects be destroyed once the objects created from them exist (shader
// modules after pipelines, descriptor set layouts after pipeline layouts, ...). Only these
// relationships keep a live reference, so only they invalidate the dependent on destruction.
const HELD_REFERENCES: Record<string, Set<string>> = {
  VkImageView: new Set(["VkImage"]),
  VkBufferView: new Set(["VkBuffer"]),
  VkFramebuffer: new Set(["VkImageView"]),
  VkDescriptorSet: new Set(["VkImageView", "VkSampler", "VkBuffer", "VkBufferView"]),
  VkSwapchainKHR: new Set(["VkSurfaceKHR"]),
};

type ObjectSignal = Signal<(id: number, object: VulkanObject) => void>;

export class ObjectDatabase implements ObjectLookup {
  allObjects = new Map<number, VulkanObject>();        // live objects
  destroyedObjects = new Map<number, VulkanObject>();  // destroyed but still referenced by live objects
  objectsByType = new Map<string, Map<number, VulkanObject>>();
  objectsByHandle = new Map<string, VulkanObject>();   // "VkImage:0x..." -> most recent object
  frameIndex = 0;
  frameTimeMs = 0;
  inspectedObject: VulkanObject | null = null;
  /** Ids of the objects referenced by the most recent capture (for the object list filter). */
  capturedObjects = new Set<number>();
  /** Memory totals of the live objects (see objectMemoryBytes): allocations, buffers, images. */
  memory = { device: 0, allocations: 0, buffers: 0, images: 0 };
  private _snapshotRemaining = 0;

  readonly onReset = new Signal<() => void>();
  readonly onSnapshotBegin = new Signal<(count: number) => void>();
  readonly onAddObject = new Signal<(object: VulkanObject, inSnapshot: boolean) => void>();
  readonly onDeleteObject: ObjectSignal = new Signal();
  readonly onObjectLabelChanged = new Signal<(id: number, object: VulkanObject, label: string) => void>();
  readonly onObjectInvalidated = new Signal<(id: number, object: VulkanObject, reason: string) => void>();
  readonly onObjectUpdated: ObjectSignal = new Signal();
  readonly onFrameStats = new Signal<(msg: FrameStatsMessage) => void>();
  readonly onObjectBlob = new Signal<(id: number, index: number, data: Uint8Array | null) => void>();
  /** Messages not handled here (capture data) are forwarded to whoever listens. */
  readonly onOtherMessage = new Signal<(msg: LayerMessage) => void>();
  readonly onCapturedObjectsChanged = new Signal<() => void>();

  /** Records the objects a capture referenced (every {__id} in its commands). */
  setCapturedObjects(ids: Set<number>): void {
    this.capturedObjects = ids;
    this.onCapturedObjectsChanged.emit();
  }

  /** Every {__id} reference inside a value, recursively. */
  collectReferences(value: unknown, into: Set<number>): void {
    if (value === null || value === undefined || typeof value !== "object") return;
    if (Array.isArray(value)) {
      for (const v of value) this.collectReferences(v, into);
      return;
    }
    const rec = value as Record<string, unknown>;
    if (typeof rec.__id === "number") {
      into.add(rec.__id);
      return;
    }
    for (const key in rec) this.collectReferences(rec[key], into);
  }

  reset(): void {
    this.allObjects = new Map();
    this.destroyedObjects = new Map();
    this.objectsByType = new Map();
    this.objectsByHandle = new Map();
    this.frameIndex = 0;
    this.frameTimeMs = 0;
    this.inspectedObject = null;
    this.capturedObjects = new Set();
    this.memory = { device: 0, allocations: 0, buffers: 0, images: 0 };
    this._snapshotRemaining = 0;
  }

  private _accountMemory(o: VulkanObject, sign: 1 | -1): void {
    const bytes = objectMemoryBytes(o, this);
    if (o.type === "VkDeviceMemory") {
      this.memory.device += sign * bytes;
      this.memory.allocations += sign;
    } else if (o.type === "VkBuffer") {
      this.memory.buffers += sign * bytes;
    } else if (o.type === "VkImage") {
      this.memory.images += sign * bytes;
    }
  }

  getObject(id: number | undefined | null): VulkanObject | null {
    if (id === undefined || id === null) return null;
    return this.allObjects.get(id) ?? this.destroyedObjects.get(id) ?? null;
  }

  getObjectByHandle(type: string, handle: string): VulkanObject | null {
    return this.objectsByHandle.get(`${type}:${handle}`) ?? null;
  }

  getObjectsOfType(type: string): Map<number, VulkanObject> | null {
    return this.objectsByType.get(type) ?? null;
  }

  handleMessage(msg: LayerMessage): void {
    switch (msg.action) {
      case "Snapshot":
        this.reset();
        this._snapshotRemaining = msg.count;
        this.onReset.emit();
        this.onSnapshotBegin.emit(msg.count);
        break;
      case "AddObject":
        this._addObject(msg);
        break;
      case "DeleteObjects":
        for (const id of msg.ids) this._deleteObject(id);
        break;
      case "ObjectSetLabel": {
        const o = this.getObject(msg.id);
        if (o) {
          o.label = msg.label ?? "";
          this.onObjectLabelChanged.emit(o.id, o, o.label);
        }
        break;
      }
      case "FrameStats":
        this.frameIndex = msg.frame;
        this.frameTimeMs = msg.frameTimeMs;
        this.onFrameStats.emit(msg);
        break;
      case "ObjectBlob":
        this.onObjectBlob.emit(msg.id, msg.index ?? 0, msg.__binary ?? null);
        break;
      case "ObjectBlobs": {
        const o = this.getObject(msg.id);
        if (o) {
          o.blobs = msg.blobs ?? [];
          this.onObjectUpdated.emit(o.id, o);
        }
        break;
      }
      case "ObjectUpdate": {
        const o = this.getObject(msg.id);
        if (o) {
          for (const key in msg) {
            if (key !== "action" && key !== "id") o.updates[key] = msg[key] as ArgValue;
          }
          this._collectReferences(msg as unknown as ArgValue, (id) => {
            const dep = this.getObject(id);
            if (dep && dep !== o) {
              o.dependencies.add(dep);
              dep.dependents.add(o);
            }
          });
          this.onObjectUpdated.emit(o.id, o);
        }
        break;
      }
      default:
        this.onOtherMessage.emit(msg);
        break;
    }
  }

  private _addObject(msg: AddObjectMessage): void {
    const o = new VulkanObject(msg);
    this.allObjects.set(o.id, o);
    let map = this.objectsByType.get(o.type);
    if (!map) {
      map = new Map();
      this.objectsByType.set(o.type, map);
    }
    map.set(o.id, o);
    this.objectsByHandle.set(`${o.type}:${o.handle}`, o);
    this._accountMemory(o, 1);

    // Dependencies: every {__id} reference in the creation arguments.
    this._collectReferences(o.args, (id) => {
      if (id === o.id) return;
      const dep = this.getObject(id);
      if (dep) {
        o.dependencies.add(dep);
        dep.dependents.add(o);
      }
    });

    if (this._snapshotRemaining > 0) this._snapshotRemaining--;
    this.onAddObject.emit(o, this._snapshotRemaining > 0);
  }

  private _collectReferences(value: ArgValue | undefined, cb: (id: number) => void): void {
    if (value === null || value === undefined || typeof value !== "object") return;
    if (Array.isArray(value)) {
      for (const v of value) this._collectReferences(v, cb);
      return;
    }
    if (isHandleRef(value)) {
      cb(value.__id);
      return;
    }
    for (const key in value) this._collectReferences((value as Record<string, ArgValue>)[key], cb);
  }

  private _deleteObject(id: number): void {
    const o = this.allObjects.get(id);
    if (!o) return;
    o.isDeleted = true;
    this._accountMemory(o, -1);
    this.allObjects.delete(id);
    this.objectsByType.get(o.type)?.delete(id);
    if (this.objectsByHandle.get(`${o.type}:${o.handle}`) === o) this.objectsByHandle.delete(`${o.type}:${o.handle}`);

    // Drop this object from the dependency lists of things it referenced; release ghosts that
    // nothing references any more.
    for (const dep of o.dependencies) {
      dep.dependents.delete(o);
      if (dep.isDeleted && dep.dependents.size === 0) this.destroyedObjects.delete(dep.id);
    }
    o.dependencies.clear();

    // Live objects that still reference this one keep it around as a ghost so their links and
    // descriptors stay meaningful. Held references also mark the dependent unusable.
    let referenced = false;
    for (const dependent of o.dependents) {
      if (dependent.isDeleted) continue;
      referenced = true;
      const held = HELD_REFERENCES[dependent.type];
      if (held && held.has(o.type) && dependent.parentId !== id) {
        dependent.invalidReason = `references destroyed ${o.type} ${o.id}`;
        this.onObjectInvalidated.emit(dependent.id, dependent, dependent.invalidReason);
      }
    }
    if (referenced) this.destroyedObjects.set(id, o);
    else o.dependents.clear();

    if (this.inspectedObject === o) this.inspectedObject = null;
    this.onDeleteObject.emit(o.id, o);
  }
}
