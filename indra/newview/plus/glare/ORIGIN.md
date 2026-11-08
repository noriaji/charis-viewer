# Glare, lens flare and cross filter — origin

| | |
|---|---|
| Source | Written for this viewer (no code taken from another viewer or engine) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/108-glare.md and research/119-cross-filter-idea.md in the sl-viewer repository |

The techniques are textbook ones, written here from their descriptions:

- the 13-tap downsample and tent upsample chain for bloom, with the Karis
  average on the first level (Jorge Jimenez, "Next Generation Post Processing
  in Call of Duty: Advanced Warfare", SIGGRAPH 2014)
- a soft (quadratic knee) threshold on the exposed brightness
- lens flare ghosts on the line from the light through the centre of the
  screen, with chromatic aberration and a halo (John Chapman, "Pseudo Lens
  Flare", 2013), drawn here analytically from the sun's place on screen
- the cross filter's rays as streaks of a few passes, the taps of each pass
  n^k texels apart so the passes together fall off exponentially (Masaki
  Kawase, "Frame Buffer Postprocessing Effects in DOUBLE-S.T.E.A.L
  (Wreckless)", GDC 2003); the lights picked by a ring test (brighter than
  everything around them) written here for this viewer

## Files

| File | What |
|---|---|
| plus/glare/glare.* | settings, presets and snapshot boost, the targets (levels, the sun's visibility, the cross filter's), the sun's place, the rays' passes, the passes' order, menu callbacks |
| llrhi/llrhiglare.* | the passes' description and the backend's entry points, `LLRHI_GLARE` |
| llvulkan/llvkglare.* | the native passes (shaders, uniforms) |
| llvulkan/llvknative.cpp | `LLVKGlare::install()` |
| pipeline.cpp | the `<plus>` hook in `LLPipeline::renderFinalize()` (after `generateExposure`) |
| app_settings/settings_plus.xml | `PlusGlare`, `PlusLensFlare`, `PlusCrossFilter`, `PlusCrossFilterRays`, `PlusCrossFilterAngle` |
| app_settings/plus/presets/graphic/*.xml | `PlusGlare` (light 0, standard 1, beautiful 2) |
| app_settings/plus/phototools/catalog.xml | the lens flare's and the cross filter's rows on the photo tools' Light & Color tab (catalog tab "color") |
| plus/photomode (photo tools) | the glare slider ("param:glare") and the lens flare row (catalog.xml, tab "color", the Light & Color tab); the Charis menu items were removed (2026-10-21, menu-trim) |
