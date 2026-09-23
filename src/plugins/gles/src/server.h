// The library's side of the connection: the SDK's server (gpu_inspector/sdk/transport.h), started
// with the first EGL context, and what the inspector's requests do.
#pragma once

namespace glesinsp
{

/** Listens for the inspector, on GLESINSP_PORT or the shared range. Only the first call does anything. */
void StartServer();

}  // namespace glesinsp
