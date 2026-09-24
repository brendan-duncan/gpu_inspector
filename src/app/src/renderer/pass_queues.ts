// Which queue each timed pass ran on, named for the Timeline's lanes (timeline_tracks.ts).
import type { CaptureCommand, PassTiming } from "../shared/protocol.js";
import { commandBufferQueues, type PassQueue } from "./timeline_tracks.js";
import type { ObjectLookup } from "./vulkan/vulkan_object.js";

/**
 * A lookup from a pass timing to its queue, or undefined where the capture does not say. Built once
 * per capture: the command stream is walked a single time, and each queue's label is made when it is
 * first asked for.
 *
 * A queue is named as the object list names it — the application's own label where it set one
 * ("Async compute"), else its type and id — with what kind of queue it is beside it, since two
 * queues of one kind is the case that most needs telling apart. The kind is left out where the
 * name already says it ("Direct queue", not "Direct queue (direct)"): a lane's label has a narrow
 * column to fit in.
 */
export function passQueueResolver(commands: readonly CaptureCommand[], db: ObjectLookup): (t: PassTiming) => PassQueue | undefined {
  const byBuffer = commandBufferQueues(commands, (commandBuffer) => {
    // Metal: a command buffer's parent is the queue that made it (src/metal/src/capture.mm).
    const parent = db.getObject(db.getObject(commandBuffer)?.parentId);
    return parent && parent.type.endsWith("Queue") ? parent.id : null;
  });
  const labels = new Map<number, PassQueue>();
  return (t) => {
    const id = byBuffer.get(`${t.frame}:${t.commandBuffer}`);
    if (id === undefined) return undefined;
    let queue = labels.get(id);
    if (!queue) {
      const o = db.getObject(id);
      const kind = o?.summary(db) ?? "";
      const name = o?.name ?? `Queue ${id}`;
      const says = !kind || name.toLowerCase().includes(kind.toLowerCase());
      queue = { id, label: says ? name : `${name} (${kind})` };
      labels.set(id, queue);
    }
    return queue;
  };
}
