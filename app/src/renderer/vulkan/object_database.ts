// Live object database fed by the layer's messages.
//
// Adapted from WebGPU Inspector's devtools/object_database.js (MIT, Brendan Duncan): the same
// dependency graph and destroy-invalidation cascade, driven by generic {__id} references found
// in each object's serialized creation arguments instead of per-class knowledge.
import { Signal } from "../utils/signal.js";
import { VulkanObject, isHandleRef, objectMemoryBytes, type ObjectLookup } from "./vulkan_object.js";
import type { AddObjectMessage, ArgValue, LayerMessage, FrameStatsMessage, LeakReportMessage, StackFrame, ValidationMessage } from "../../shared/protocol.js";
import type { CaptureFileObject } from "../capture_file.js";

/** A validation message with its repeat count (see ValidationMessage in protocol.ts). */
export type ValidationEntry = ValidationMessage;

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
  /** CPU time per frame inside vkQueueSubmit, from the last FrameStats. */
  submitMs = 0;
  /** Display refresh interval while vsync is on (0 without), the present mode, and dropped frames. */
  refreshMs = 0;
  presentMode = "";
  droppedFrames = 0;       // in the last reporting interval
  droppedFramesTotal = 0;  // since the connection
  inspectedObject: VulkanObject | null = null;
  /** Ids of the objects referenced by the most recent capture (for the object list filter). */
  capturedObjects = new Set<number>();
  /** Memory totals of the live objects (see objectMemoryBytes): allocations, buffers, images. */
  memory = { device: 0, allocations: 0, buffers: 0, images: 0 };
  /** Binary payloads received (ObjectBlob) or loaded from a capture file, keyed "id:index". */
  blobData = new Map<string, Uint8Array>();
  /** Validation messages in arrival order, and by the objects they name. */
  validation: ValidationEntry[] = [];
  validationByKey = new Map<number, ValidationEntry>();
  validationByObject = new Map<number, ValidationEntry[]>();
  /** Messages by the command they fired on: "commandBuffer:slot" (see validationForCommand). */
  validationByCommand = new Map<string, ValidationEntry[]>();
  /** Unique messages the layer dropped after its cap. */
  validationDropped = 0;
  /** Leak reports (objects alive when their device or instance was destroyed), in arrival order. */
  leaks: LeakReportMessage[] = [];
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
  /** A validation message arrived (isNew) or its repeat count changed. */
  readonly onValidationMessage = new Signal<(entry: ValidationEntry, isNew: boolean) => void>();
  readonly onLeakReport = new Signal<(report: LeakReportMessage) => void>();
  /** Stack traces: creation stacks by object id, symbols by address, and whether the layer collects stacks. */
  stacks = new Map<number, StackFrame[]>();
  stacksAvailable: boolean | null = null;
  symbols = new Map<string, StackFrame>();
  readonly onStacktraces = new Signal<() => void>();
  readonly onSymbols = new Signal<() => void>();

  /** Leaked objects over every report. */
  get leakCount(): number {
    return this.leaks.reduce((n, r) => n + r.count, 0);
  }

  /** Validation errors and warnings by severity: [errors, warnings]. */
  get validationCounts(): [number, number] {
    let errors = 0;
    let warnings = 0;
    for (const v of this.validation) {
      if (v.severity === "error") errors++;
      else if (v.severity === "warning") warnings++;
    }
    return [errors, warnings];
  }

  /** Validation messages naming an object. */
  validationFor(id: number): ValidationEntry[] {
    return this.validationByObject.get(id) ?? [];
  }

  /** Validation messages that fired while a command was recorded: the command buffer's id and the command's slot. */
  validationForCommand(commandBufferId: number | undefined, slot: number | undefined): ValidationEntry[] {
    if (commandBufferId === undefined || slot === undefined) return [];
    return this.validationByCommand.get(`${commandBufferId}:${slot}`) ?? [];
  }

  private _indexByCommand(msg: ValidationEntry, add: boolean): void {
    if (!msg.command) return;
    const key = `${msg.command.commandBuffer}:${msg.command.slot}`;
    const list = this.validationByCommand.get(key) ?? [];
    if (add) {
      if (!list.includes(msg)) list.push(msg);
      this.validationByCommand.set(key, list);
    } else {
      const i = list.indexOf(msg);
      if (i >= 0) list.splice(i, 1);
    }
  }

  private _addValidation(msg: ValidationMessage): void {
    const existing = this.validationByKey.get(msg.key);
    if (existing) {
      existing.count = msg.count;
      // A resend carries a moved command reference (the recording a capture shows).
      if (msg.command && (existing.command?.commandBuffer !== msg.command.commandBuffer || existing.command?.slot !== msg.command.slot)) {
        this._indexByCommand(existing, false);
        existing.command = msg.command;
        this._indexByCommand(existing, true);
      }
      this.onValidationMessage.emit(existing, false);
      return;
    }
    this.validation.push(msg);
    this.validationByKey.set(msg.key, msg);
    this._indexByCommand(msg, true);
    for (const o of msg.objects ?? []) {
      if (!o.object || !isHandleRef(o.object)) continue;
      const list = this.validationByObject.get(o.object.__id) ?? [];
      if (!list.includes(msg)) list.push(msg);
      this.validationByObject.set(o.object.__id, list);
    }
    this.onValidationMessage.emit(msg, true);
  }

  /** Validation messages of a capture file. */
  loadValidation(entries: ValidationMessage[]): void {
    for (const e of entries) this._addValidation(e);
  }

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
    this.submitMs = 0;
    this.refreshMs = 0;
    this.presentMode = "";
    this.droppedFrames = 0;
    this.droppedFramesTotal = 0;
    this.inspectedObject = null;
    this.capturedObjects = new Set();
    this.memory = { device: 0, allocations: 0, buffers: 0, images: 0 };
    this.blobData = new Map();
    this.validation = [];
    this.validationByKey = new Map();
    this.validationByObject = new Map();
    this.validationByCommand = new Map();
    this.validationDropped = 0;
    this.stacks = new Map();
    this.stacksAvailable = null;
    this.symbols = new Map();
    this.leaks = [];
    this._snapshotRemaining = 0;
  }

  /**
   * Populates the database from a capture file's object graph (see capture_file.ts): the same
   * path as a live snapshot, then the objects destroyed before the save become ghosts without
   * the destroy cascade, so every link of the loaded capture still resolves.
   */
  loadObjects(objects: CaptureFileObject[], blobs: Map<string, Uint8Array>, stats: { frame: number; frameTimeMs: number; submitMs: number; refreshMs?: number }): void {
    this.reset();
    this._snapshotRemaining = objects.length;
    this.onReset.emit();
    this.onSnapshotBegin.emit(objects.length);
    for (const rec of objects) {
      this._addObject({
        action: "AddObject", id: rec.id, parent: rec.parent, type: rec.type, cmd: rec.cmd, index: rec.index, handle: rec.handle,
        label: rec.label, args: rec.args, blobs: rec.blobs.map((b) => ({ name: b.name, size: b.size })),
      });
      const o = this.allObjects.get(rec.id);
      if (!o) continue;
      o.updates = rec.updates ?? {};
      this._collectReferences(o.updates as ArgValue, (id) => {
        const dep = this.getObject(id);
        if (dep && dep !== o) {
          o.dependencies.add(dep);
          dep.dependents.add(o);
        }
      });
    }
    for (const rec of objects) {
      if (!rec.deleted) continue;
      const o = this.allObjects.get(rec.id);
      if (!o) continue;
      o.isDeleted = true;
      this._accountMemory(o, -1);
      this.allObjects.delete(o.id);
      this.objectsByType.get(o.type)?.delete(o.id);
      if (this.objectsByHandle.get(`${o.type}:${o.handle}`) === o) this.objectsByHandle.delete(`${o.type}:${o.handle}`);
      this.destroyedObjects.set(o.id, o);
      this.onDeleteObject.emit(o.id, o);
    }
    for (const [key, data] of blobs) this.blobData.set(key, data);
    this.frameIndex = stats.frame;
    this.frameTimeMs = stats.frameTimeMs;
    this.submitMs = stats.submitMs;
    this.refreshMs = stats.refreshMs ?? 0;
    this.onFrameStats.emit({ action: "FrameStats", frame: stats.frame, frameTimeMs: stats.frameTimeMs, submitMs: stats.submitMs, refreshMs: this.refreshMs });
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
        this.submitMs = msg.submitMs ?? 0;
        this.refreshMs = msg.refreshMs ?? 0;
        this.presentMode = msg.presentMode ?? "";
        this.droppedFrames = msg.dropped ?? 0;
        this.droppedFramesTotal = msg.droppedTotal ?? this.droppedFramesTotal + (msg.dropped ?? 0);
        this.onFrameStats.emit(msg);
        break;
      case "ObjectBlob":
        if (msg.__binary) this.blobData.set(`${msg.id}:${msg.index ?? 0}`, msg.__binary);
        this.onObjectBlob.emit(msg.id, msg.index ?? 0, msg.__binary ?? null);
        break;
      case "ValidationMessage":
        this._addValidation(msg);
        break;
      case "Stacktraces":
        this.stacksAvailable = msg.available;
        for (const s of msg.stacks ?? []) this.stacks.set(s.id, s.frames ?? []);
        this.onStacktraces.emit();
        break;
      case "Symbols":
        for (const f of msg.frames ?? []) if (f && f.address) this.symbols.set(f.address, f);
        this.onSymbols.emit();
        break;
      case "LeakReport":
        this.leaks.push(msg);
        this.onLeakReport.emit(msg);
        break;
      case "ValidationCount":
        for (const [key, count] of msg.counts ?? []) {
          const e = this.validationByKey.get(key);
          if (e && e.count !== count) {
            e.count = count;
            this.onValidationMessage.emit(e, false);
          }
        }
        if (msg.dropped !== undefined) this.validationDropped = msg.dropped;
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
