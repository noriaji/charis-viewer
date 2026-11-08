# llvulkan — origin

| | |
|---|---|
| Source | Written for Charis Viewer: the Vulkan backend (the GL translation layer and the native passes) |
| Copyright | Charis Viewer contributors (each file's header), except the parts listed below |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/ in the sl-viewer repository (each file names its notes) |

None of these files is an official viewer file, and none was copied from
another viewer: a line by line comparison with the official viewer 26.5
(345ceae3db) found no shared runs of code longer than two lines. The
headers said "Copyright (C) 2026, Linden Research, Inc." until 2026-10-21;
that was the template of the official files, not Linden Lab's authorship.

## Code from other projects

| File | What | From | License |
|---|---|---|---|
| `thirdparty/vk_mem_alloc.h` | Vulkan Memory Allocator 3.2.1, unchanged | AMD, https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | MIT, `thirdparty/VMA-LICENSE.txt` (Copyright (c) 2017-2025 Advanced Micro Devices, Inc.) |
| `llvkgtao.cpp` (`OCCLUSION_FRAGMENT`) | the GTAO occlusion pass: a GLSL port of XeGTAO's main pass | Intel, https://github.com/GameTechDev/XeGTAO | MIT (Copyright (C) 2016-2021, Intel Corporation); the notice is in the file's header and in `newview/plus/gtao/ORIGIN.md` |
| `llvkupscale.cpp` (`UPSCALE_FRAGMENT`, EASU and RCAS) | FidelityFX Super Resolution 1.0: a GLSL port of `ffx_fsr1.h`'s `FsrEasuCon`/`FsrEasuF` and `FsrRcasCon`/`FsrRcasF` (with `FSR_RCAS_DENOISE`), written for these passes' bindings (research/165); the reciprocal approximations of `ffx_a.h` are plain divisions with a floor | AMD, https://github.com/GPUOpen-Effects/FidelityFX-FSR | MIT (Copyright (c) 2021 Advanced Micro Devices, Inc.); the full notice is in the file's header and in `newview/plus/upscale/ORIGIN.md` |
| `llvkssr.cpp` (the shaders) | the screen space reflection passes, rewritten for these passes' bindings from the official `project/graphics-care-package` branch (2c79cbae84): `screenSpaceReflUtil.glsl`, `screenSpaceReflTraceF.glsl`, `screenSpaceReflResolveF.glsl`, `hiZReduceF.glsl`, `ssrDownsampleF.glsl` (research/129) | Linden Lab, https://github.com/secondlife/viewer | GNU LGPL 2.1 only, Copyright (C) 2007-2026, Linden Research, Inc. (the header says so) |

The graphics-care-package's Hi-Z trace says in its comments that it follows
Godot Engine's `screen_space_reflection.glsl` (MIT license; "matches Godot
exactly" in places). The branch's files carry only Linden Lab's notice.
`llvkssr.cpp` keeps that trace (`hiZTrace`). Whether Godot's notice is owed
has not been settled (see the open points of the fix-headers handoff).

## Written after the official viewer's shaders

To give the same results as the official GL path, some native passes are
written to match official shaders (Linden Lab, LGPL 2.1, the same license);
no runs of their code were copied:

- `llvknative.cpp`: the sun shadow passes (`shadowV.glsl`,
  `shadowAlphaMaskV.glsl`/`F.glsl`, `treeShadowV.glsl`/`F.glsl`,
  `objectSkinV.glsl`'s skinning)
- `llvkskinfill.cpp`, `llvkwetlook.cpp`: `globalF.glsl`'s `decodeNormal` /
  `encodeNormal`, `shadowUtil.glsl`'s shadow compare
- `llvkbenchmark.cpp`: the GPU benchmark (`benchmarkV.glsl`, `benchmarkF.glsl`)
- `llvkgl_*`: the GL behaviour the viewer relies on, measured against GL drivers

## Techniques from publications (no code taken)

- `llvkglare.cpp`: 13-tap downsample and Karis average (Jimenez, "Next
  Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH
  2014), lens flare ghosts and halo (Chapman, "Pseudo Lens Flare", 2013),
  streaks (Kawase, GDC 2003); see `newview/plus/glare/ORIGIN.md`
- `llvkgtao.cpp`: contact shadows (screen-space ray against the depth
  buffer), interleaved gradient noise (Jimenez 2014); the GTAO method
  (Jimenez, Wu, Pesce, Jarabo, SIGGRAPH 2016)
- `llvkvolumetric.cpp`: shadow-map ray marching, Henyey-Greenstein phase,
  interleaved gradient noise; see `newview/plus/volumetric/ORIGIN.md`
- `llvkwetlook.cpp`: darkening and saturation of wet porous surfaces
  (Lagarde, "Water drop 3b - Physically based wet surfaces", 2013)
- `llvkbc7.glsl.inl`: a BC7 encoder written for this viewer; the partition
  and anchor tables and the weights are those of the BC7 format
  specification
- `llvklook.cpp`: LUT sampling and screen effects; see
  `newview/plus/look/ORIGIN.md`

Short integer hashes, as commonly published: `lowbias32` by Chris Wellons
(hash-prospector) in `llvkrain.cpp` and `llvkwetlook.cpp`, `pcg2d`
(Jarzynski and Olano, "Hash Functions for GPU Rendering", JCGT 2020) in
`llvkssr.cpp`, the PCG output function (M. O'Neill) in `llvklook.cpp`.
