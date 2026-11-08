# Looks — origin

| | |
|---|---|
| Source | Written for this viewer (no code taken from another viewer or tool) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/102-community-looks.md in the sl-viewer repository (stage 1a) |

The LUT formats were read from their public descriptions:

- Adobe .cube: the Adobe "Cube LUT Specification" 1.0 (`TITLE`,
  `LUT_3D_SIZE`, `LUT_1D_SIZE`, `DOMAIN_MIN`, `DOMAIN_MAX`, rows with red
  changing fastest), and DaVinci Resolve's `LUT_3D_INPUT_RANGE` /
  `LUT_1D_INPUT_RANGE`.
- The PNG strip (ReShade's LUT.fx, Unreal, GShade, Alchemy): N tiles of N x N
  side by side; the tile is blue, x in a tile red, y green. The sampling (red
  and green at texel centres, two reads blended for blue) is the usual way to
  read such a strip; it was written from that description, not copied from
  LUT.fx (which carries no license text).

Alchemy's LUT support (alrenderutils.cpp) was not used; it keeps its LUTs in
3D textures, which this viewer's Vulkan translation layer does not take.

The shipped looks in `app_settings/plus/looks/` were generated for Charis by
`tools/looks/make_looks.py` (sl-viewer repository) from parameters chosen for
this viewer, and are released under CC0 1.0 (see `app_settings/plus/looks/LICENSE.txt`).
`autumn-street` ("Autumn Alley") is made by `tools/make-look-autumn.py` the
same way; its coefficients come from a few colour statistics (casts, the
saturation of hue bands, brightness percentiles) measured on a reference
photo, which is not shipped and no pixel of which is in the LUT.
No third-party LUT is shipped.

## Files

| File | What |
|---|---|
| plus/look/look.* | the look lists (shipped and the user's folder), look.xml, settings, the menu, loading the LUT, the hook's work |
| plus/look/lutfile.* | .cube and PNG strip parsing into one 3D table, the strip the GPU holds (standard C++ only) |
| llrhi/llrhilook.* | the pass's description and the backend's entry point, `LLRHI_LOOK` |
| llvulkan/llvklook.* | the native fullscreen pass (shader, uniforms) |
| llvulkan/llvknative.cpp | `LLVKLook::install()` next to the GTAO one |
| plus/presets/presets.* | `snapshotInProgress()` (for "only in snapshots") |
| pipeline.cpp | the `<plus>` hook in `LLPipeline::renderFinalize()` after anti-aliasing |
| app_settings/settings_plus.xml | `PlusLook`, `PlusLookStrength`, `PlusLookSnapshotOnly` |
| app_settings/plus/looks/ | the shipped looks (look.xml, lut.cube, lut.png) |
| skins/default/xui/{en,ja}/menu_plus.xml | the "Look" submenu (the strength and "only in snapshots" are in the photo tools, catalog.xml) |
