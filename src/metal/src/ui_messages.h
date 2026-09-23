// Incoming messages from the inspector UI. See ui_messages.mm.
#pragma once

#include <cstdint>

namespace mtlinsp
{

/** Installs the transport's message handler. Called once, on load, before the listener starts. */
void StartUiMessages();

/**
 * Asks the inspector to take a capture with the capture bar's options, which is what both the
 * application's own gpu_inspector_capture and the HUD's capture hotkey do. `frameCount` 0 leaves
 * the count to the bar; `label` may be null, and names the tab when it is not. `source` is who
 * asked, for the log. Returns false when nothing is attached to ask.
 */
bool RequestInspectorCapture(uint32_t frameCount, const char* label, const char* source);

}  // namespace mtlinsp
