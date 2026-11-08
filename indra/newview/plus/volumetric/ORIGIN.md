# Volumetric light — origin

| | |
|---|---|
| Source | Written for this viewer (no code taken from another viewer) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/84-volumetric-lighting.md in the sl-viewer repository |

Black Dragon (which has volumetric lighting) is not in `refs/` and was not
read for this. The techniques are textbook ones:

- ray marching the shadow maps along each view ray at reduced resolution
  (e.g. Tóth & Umenhoffer, "Real-time Volumetric Lighting in Participating
  Media", Eurographics 2009)
- Henyey-Greenstein phase function (Henyey & Greenstein 1941)
- interleaved gradient noise for the sample offsets (Jimenez, "Next
  Generation Post Processing in Call of Duty: Advanced Warfare", 2014)
- depth-guided (joint bilateral) upsampling

## Files

| File | What |
|---|---|
| plus/volumetric/volumetric.* | settings, what the passes read from LLPipeline, the scatter target |
| llrhi/llrhivolumetric.* | the passes' description and the backend's entry points, `LLRHI_VOLUMETRIC` |
| llvulkan/llvkvolumetric.* | the two native passes (shaders, uniforms) |
| llvulkan/llvkfullscreen.* | fullscreen fragment passes on GL targets: pipelines, sets, draw (shared with GTAO) |
| llvulkan/llvkgl.cpp, llvktracker.h | `LLVKTracker::beginPass` may sample the target's own depth (read-only), `LLVKTracker::endPass` |
| pipeline.cpp | the `<plus>` hook in `LLPipeline::renderFinalize()` |
| app_settings/settings_plus.xml | `PlusVolumetric`, `PlusVolumetricQuality`, `PlusVolumetricStrength`, `PlusVolumetricSamples` |
| app_settings/plus/phototools/catalog.xml | light shafts, their density and quality (photo tools); the Charis menu items were removed (2026-10-21, menu-trim) |
