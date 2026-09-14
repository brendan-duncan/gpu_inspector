# Third-party code and licenses

This project incorporates or adapts code from the following projects, MIT-licensed unless noted.
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

## MinHook
- Source: https://github.com/TsudaKageyu/minhook (the `third_party/minhook` submodule, built into `dxinsp_capture.dll`)
- License: BSD-2-Clause, Copyright (C) 2009-2017 Tsuda Kageyu. Its Hacker Disassembler Engine (`src/hde`) is Copyright (c) 2008-2009 Vyacheslav Patkov, under the same terms.
- Used for: hooking the `D3D12CreateDevice` and `CreateDXGIFactory*` entry points inline in the Direct3D 12 capture library (`d3d12/src/hook.cpp`).

The BSD-2-Clause license requires this notice to accompany binary distributions:

```
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
