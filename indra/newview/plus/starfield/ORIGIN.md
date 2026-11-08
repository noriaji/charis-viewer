# Star field and Milky Way — origin

| | |
|---|---|
| Source | Written for this viewer (no code taken from another viewer or engine) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/176-star-field.md in the sl-viewer repository |

Textbook techniques, written here from their descriptions:

- apparent magnitude: a star of magnitude m has the flux 10^(-0.4 m); the number of stars grows by about 10^0.5 per magnitude
- a star's colour from its surface temperature: Tanner Helland's fit of a black body's colour (2012, "How to Convert Temperature (K) to RGB")
- xorshift32 (George Marsaglia, 2003) for the fixed seed; value noise for the Milky Way's clumps
- stars as Gaussian points a pixel or so wide in clip space (energy kept when the star is narrower than a pixel)

No star catalogue and no photograph of the sky is used: the stars are random, the Milky Way a procedural band.

## Files

| File | What |
|---|---|
| plus/starfield/starfield.* | the settings (PlusSkyStars, PlusSkyStarGain), the catalogue (stars, Milky Way patches) |
| llvowlsky.cpp / .h | `<plus>`: builds the vertex buffer from the catalogue, follows the mode |
| lldrawpoolwlsky.cpp | `<plus>`: the uniforms of the star program, the Milky Way's second draw |
| app_settings/shaders/class1/deferred/starsV.glsl, starsF.glsl | `<plus>`: the new look (`star_field`); the official one stays when it is 0 |
| plus/starfield/skymilky.* | the baked Milky Way's picture (PlusSkyStarsMilkyWayHQ, research/224): one background thread runs `math::bake()`, the main thread uploads the result as a float texture (the way `gEXRImage` is made) |
| app_settings/shaders/class1/deferred/skyMilkyV.glsl, skyMilkyF.glsl | the program that draws that picture on a box round the camera (`gDeferredStarMilkyProgram`) |
