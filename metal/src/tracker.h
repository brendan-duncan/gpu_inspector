// Live object tracker: every Metal object the application creates, with the arguments of the call
// that created it (its "descriptor"), its parent and its label.
//
// The counterpart of layer/src/tracker.h, and it emits the same messages — AddObject,
// DeleteObjects, ObjectSetLabel — because the UI's object database does not care which API the
// objects came from. What differs is what identifies an object: Vulkan has typed handles, Metal
// has an Objective-C pointer, so the side table is keyed by pointer and the "type" is the
// protocol name the application sees (MTLBuffer, MTLRenderPipelineState) rather than the driver's
// private class.
//
// Ids are stable and never reused, so the UI can hold on to one after the object is gone.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#import <objc/runtime.h>

namespace mtlinsp {

/**
 * Registers an object under the protocol name the application knows it by, and streams AddObject.
 *
 * `cmd` is the selector that created it ("newBufferWithLength:options:"), which is the Metal
 * equivalent of the Vulkan layer's creating command; the UI shows it as the object's origin.
 * `argsJson` is the descriptor, already serialized, or empty.
 *
 * Returns the id, or the existing id when the object is already tracked — Metal hands the same
 * object back more than once (the device, most obviously).
 */
uint64_t TrackObject(id object, const char *type, const char *cmd, id parent,
                     const std::string &argsJson);

/** Id of an already-tracked object, or 0. */
uint64_t IdOf(id object);

/**
 * The tracked object with this id, if it is still alive, else nil.
 *
 * Held weakly. Retaining would keep every texture a game ever made resident for as long as the
 * inspector is attached, which for a real player is hundreds of megabytes of VRAM it cannot
 * reclaim — the tool would change what it is measuring. A weak reference reads nil once the
 * application lets go, which is also the honest answer to "show me this texture".
 */
id LiveObject(uint64_t id);

/**
 * Attaches named bytes to a tracked object, which the UI can ask for by index (`RequestBlob`).
 *
 * The counterpart of the Vulkan layer's shader blobs. A library's Metal Shading Language source
 * goes here when it was compiled from source, and its metallib bytes when it was loaded
 * precompiled — the two cases a real engine mixes.
 */
void AddBlob(id object, const char *name, const void *data, size_t size);

/** Answers `RequestBlob` with an `ObjectBlob` message and the bytes. */
void SendBlob(uint64_t objectId, uint32_t index);

/** Streams ObjectSetLabel when an object's label has changed since it was last seen. */
void TrackLabel(id object);

/** Sends AddObject for every live object, then streams events from there on. */
void SendSnapshot();
void OnDisconnect();

/** Starts the transport and wires the snapshot to it. Called once, on load. */
void StartTracking();

}  // namespace mtlinsp
