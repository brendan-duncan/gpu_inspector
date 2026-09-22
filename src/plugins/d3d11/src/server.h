// The library's side of the connection: the SDK's server (gpu_inspector/sdk/transport.h), started
// with the first swap chain, and what the inspector's requests do.
#pragma once

namespace d3d11insp {

/** Listens for the inspector, on D3D11INSP_PORT or the shared range. Only the first call does anything. */
void StartServer();

}  // namespace d3d11insp
