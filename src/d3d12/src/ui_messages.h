// Incoming messages from the inspector UI (RequestBlob, RequestImage, Capture, ReplaceShader,
// ...), parsed and dispatched to the modules that answer them. See ui_messages.cpp.
#pragma once

#include <cstdint>

namespace dxinsp
{

/** Starts the transport with the snapshot, disconnect and message handlers wired. Called once, at initialization. */
void StartTracking();

/**
 * Asks the inspector to take a capture with the capture bar's options, which is what both the
 * application's own gpu_inspector_capture and the HUD's capture hotkey do. `frameCount` 0 leaves
 * the count to the bar; `label` may be null, and names the tab when it is not. `source` is who
 * asked, for the log. Returns false when nothing is attached to ask.
 */
bool RequestInspectorCapture(uint32_t frameCount, const char* label, const char* source);

}  // namespace dxinsp
