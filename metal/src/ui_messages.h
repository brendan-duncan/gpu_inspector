// Incoming messages from the inspector UI. See ui_messages.mm.
#pragma once

namespace mtlinsp {

/** Installs the transport's message handler. Called once, on load, before the listener starts. */
void StartUiMessages();

}  // namespace mtlinsp
