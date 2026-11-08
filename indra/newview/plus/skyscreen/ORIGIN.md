# Sky to screen — origin

| | |
|---|---|
| Source | Written for this viewer (no code taken from another viewer or engine) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/176-star-field.md section 9 in the sl-viewer repository |

What the scene does to the sky's colour before the screen (sky_hdr_scale, the exposure, the
tone curve and its mix, as LLSettingsVOSky::applySpecial and LLPipeline::tonemap set them),
handed to the sky's shaders that undo it: the star field (starsF.glsl) and the night clouds
(cloudsF.glsl). The inverse of ACES Hill's RRTAndODTFit (the official curve) is solved by
Newton's method in the shaders.

## Files

| File | What |
|---|---|
| plus/skyscreen/skyscreen.* | the values (`current`), the uniforms and the exposure map (`bind`) |
| lldrawpoolwlsky.cpp | `<plus>`: binds them for the stars and the clouds |
