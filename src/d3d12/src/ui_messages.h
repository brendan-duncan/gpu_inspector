// Incoming messages from the inspector UI (RequestBlob, RequestImage, Capture, ReplaceShader,
// ...), parsed and dispatched to the modules that answer them. See ui_messages.cpp.
#pragma once

namespace dxinsp
{

/** Starts the transport with the snapshot, disconnect and message handlers wired. Called once, at initialization. */
void StartTracking();

}  // namespace dxinsp
