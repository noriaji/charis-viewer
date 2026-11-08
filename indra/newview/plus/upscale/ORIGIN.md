# plus/upscale — origin

| | |
|---|---|
| Source | Written for Charis Viewer (research/165-upscale.md in the sl-viewer repository) |
| Copyright | Charis Viewer contributors, except the FSR 1 shaders (below) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |

## Files

| File | What |
|---|---|
| plus/upscale/upscale.* | the settings, the drawn size of the main view, snapshots and the photo mode (photo screen or photo tools) at the full size, the passes' order in renderFinalize |
| llrhi/llrhiupscale.* | the pass's description and the backend's entry point, `LLRHI_UPSCALE`, `upscale.toggle` |
| llvulkan/llvkupscale.cpp | the native fullscreen passes (EASU, RCAS, a bilinear stretch) |
| llvulkan/llvknative.cpp | `LLVKUpscale::install()` next to the look's |

## AMD FidelityFX Super Resolution 1

The EASU and RCAS fragment shaders in `llvulkan/llvkupscale.cpp` are a GLSL
port of `ffx_fsr1.h` (FsrEasuCon, FsrEasuF, FsrRcasCon, FsrRcasF with
FSR_RCAS_DENOISE) of FidelityFX Super Resolution 1.0,
https://github.com/GPUOpen-Effects/FidelityFX-FSR. The approximate
reciprocals of `ffx_a.h` are plain divisions with a small floor; the
16-bit paths are left out.

```
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

AMD, FidelityFX and FidelityFX Super Resolution are trademarks of Advanced
Micro Devices, Inc.
