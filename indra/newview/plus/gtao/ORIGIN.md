# GTAO and contact shadows — origin

| | |
|---|---|
| Source | Written for this viewer; the occlusion pass is a GLSL port of XeGTAO's main pass (MIT, below) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/92-gtao.md in the sl-viewer repository |

The occlusion pass (`OCCLUSION_FRAGMENT` in `llvulkan/llvkgtao.cpp`) follows
the main pass of **XeGTAO** (Intel, GitHub GameTechDev/XeGTAO, MIT license,
see below), rewritten as a GLSL fragment
shader: the slice and horizon loop, the falloff, the projected normal and the
visible-arc integral, and the constants named after XeGTAO's defaults
(falloff share 0.615, sample distribution power 2, "pixel too close" 1.3, the
0.05 blend of the projected normal's length). Not taken: its depth MIP chain
(the screen radius is capped instead), its compute shaders, its edge-packed
denoiser and its Hilbert/R2 noise (a 4x4 ordered pattern and a 4x4 average
here). XeGTAO's license (MIT, as its LICENSE file gives it):

> Copyright (C) 2016-2021, Intel Corporation
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

The method: Jimenez, Wu, Pesce, Jarabo, "Practical Realtime Strategies for
Accurate Indirect Occlusion" (SIGGRAPH 2016 course).

Contact shadows (`CONTACT_FRAGMENT`) are the common screen-space technique (a
short ray towards the light tested against the depth buffer), written here
from scratch; the ray offsets use interleaved gradient noise (Jimenez,
"Next Generation Post Processing in Call of Duty: Advanced Warfare", 2014).

No other viewer's code was read for this (Black Dragon has no GTAO or contact
shadows; research/89).

## Files

| File | What |
|---|---|
| plus/gtao/gtao.* | settings, menu callbacks, what the passes read from LLPipeline, the reduced targets |
| llrhi/llrhigtao.* | the passes' description and the backend's entry points, `LLRHI_GTAO` |
| llvulkan/llvkgtao.* | the five native passes (shaders, uniforms) |
| llvulkan/llvkfullscreen.* | fullscreen fragment passes on GL targets (shared with volumetric light) |
| llvulkan/llvknative.cpp | one line: `LLVKGtao::install()` |
| pipeline.cpp | the `<plus>` hook in `LLPipeline::renderDeferredLighting()` |
| app_settings/settings_plus.xml | `PlusGtao`, `PlusGtaoQuality`, `PlusGtaoStrength`, `PlusGtaoRadius`, `PlusContactShadows`, `PlusContactShadowLength` |
| app_settings/plus/phototools/catalog.xml | GTAO, its quality, contact shadows (photo tools); the Charis menu items were removed (2026-10-21, menu-trim) |
