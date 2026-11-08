# Rainbow — origin

| | |
|---|---|
| Source | Written for this viewer (no code taken from another viewer or engine) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/255-rainbows.md in the sl-viewer repository (section 8: the review) |
| Desk check | research/255-rainbows-sim/ (rainbow_sim.py, results.md): the shader's constants come from it |

The rainbow worked out per pixel instead of the sky's rainbow texture: for 8 wavelengths, the primary and
the secondary bow from Airy's theory (the drops' size sets their width, the supernumeraries and the
fogbow), the CIE 1931 weights, geometric optics' brightening inside the primary, the sun's disc. Physics
from the literature (Descartes, Airy; water's refractive index by Quan & Fry 1995; the CIE functions as
fitted by Wyman, Sloan & Shirley 2013), the code our own.

## Files

| File | What |
|---|---|
| plus/rainbow/rainbow.* | the settings (PlusRainbow, PlusRainbowKind, PlusRainbowStrength), the kinds, the light's fade, the "no post" ceiling, the uniforms |
| app_settings/shaders/class1/deferred/skyRainbowF.glsl | the bow (new; linked into the sky's and the clouds' programs) |
| skyF.glsl, cloudsF.glsl | `<plus>`: add the bow to what they write |
| llviewershadermgr.cpp | `<plus>`: skyRainbowF.glsl in the sky's (both) and the clouds' (both) programs |
| lldrawpoolwlsky.cpp | `<plus>`: the uniforms; the official rainbow's moisture 0 while on |
