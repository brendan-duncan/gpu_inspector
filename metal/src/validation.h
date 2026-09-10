// Validation messages: what Metal says went wrong, as the UI's ValidationMessage.
//
// The counterpart of the Vulkan layer's debug-utils messenger (layer/src/validation.cpp), with
// the same message shape, dedupe by text and repeat counts, so the Inspect panel's message list,
// the object markers and the session bar's counter work unchanged. Metal has no messenger; it
// has three things that say the same kinds of things:
//
//   * A command buffer's `error` once it completes, and with encoder execution status enabled on
//     it, which encoder faulted. That is how a GPU fault or a bad resource reference surfaces.
//   * Metal's own validation layer (MTL_DEBUG_LAYER=1), which aborts on an error by default but
//     logs instead with MTL_DEBUG_LAYER_ERROR_MODE=nslog — through NSLog, which is interposed
//     here. The launch dialog's "Validation layer" sets those variables.
//   * Shader logging (`MTLLogContainer` on a completed command buffer), for a shader that logs.
#pragma once

#include <cstdint>
#include <string>

#import <objc/runtime.h>

namespace mtlinsp {

/**
 * Records a message and sends it, or its new count when the same text was seen before.
 * `severity` is "error", "warning" or "info"; `type` "validation", "performance" or "general";
 * `object` the Metal object it concerns, or nil.
 */
void ReportValidation(const char *severity, const char *type, const std::string &idName,
                      int64_t idNumber, const std::string &message, id object,
                      const char *objectClass);

/**
 * Asks to be told when a command buffer completes, to read its error and its shader logs. Only
 * while a client is connected: a completed handler per command buffer is cheap but not free.
 */
void WatchCommandBuffer(id commandBuffer);

/** Sends the repeat counts accumulated since the last flush. Called at every frame end. */
void FlushValidation();

/** Resends every message kept, for a client that just connected. */
void SendValidationSnapshot();

}  // namespace mtlinsp
