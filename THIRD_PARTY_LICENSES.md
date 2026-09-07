# Third-party code and licenses

This project incorporates or adapts code from the following MIT-licensed projects.
Files that contain adapted code carry a header comment naming their origin.

## WebGPU Inspector
- Source: https://github.com/brendan-duncan/webgpu_inspector
- License: MIT, Copyright (c) 2023 Brendan Duncan
- Used for: UI widget library, texture/buffer viewers, capture data model and panel design.

## RenderDoc
- Source: https://github.com/baldurk/renderdoc
- License: MIT, Copyright (c) 2019-2025 Baldur Karlsson
- Used for: Vulkan layer structure, format tables, SPIR-V tooling (planned).

## GFXReconstruct
- Source: https://github.com/LunarG/gfxreconstruct
- License: MIT, Copyright (c) 2018-2020 LunarG, Inc. (its vendored Vulkan-Utility-Libraries headers are Apache-2.0)
- Used for: reference for vk.xml code generation, struct encoding, object state tracking and GPU readback.

## Vulkan-Headers / vk.xml (Khronos)
- License: Apache-2.0 OR MIT
- Used for: code generation of the Vulkan dispatch tables and serializers from `vk.xml`.
