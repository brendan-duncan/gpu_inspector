# GPU Inspector plugin SDK

What a plugin (docs/PLUGINS.md) is built against: the C++ its capture library speaks the inspector's
protocol with, and the TypeScript types its backend module is written against.

```
include/gpu_inspector/sdk/
  transport.h   the server: listening, framing, the probe handshake, a sender thread
  json.h        JsonWriter (object references, bytes) and ParseJson
  config.h      settings from the environment, the launcher's settings block, Android properties
ts/
  index.ts      the types: Backend, CommandSets, DrawState, DetailSection, the protocol's messages
```

Both halves are header-only and dependency-free: a capture library adds `include/` to its include
path, and a backend module imports types from `ts/index.ts`, which leave nothing behind once the
module is bundled.

The built-in capture libraries predate the SDK and keep their own copies of the same code
(`src/vulkan/src/transport.cpp`, `json_writer.h` and `target_probe.h`, which `src/d3d12` and
`src/metal` reuse). The wire is the same, so the inspector cannot tell them apart.

The OpenGL ES plugin (`src/plugins/gles`) is the example: its library includes these headers and
nothing else of the inspector, and its backend is built from these types alone.
