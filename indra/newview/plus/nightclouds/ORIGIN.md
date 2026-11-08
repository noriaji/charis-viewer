# Night clouds — origin

| | |
|---|---|
| Source | Written for this viewer (no code taken from another viewer or engine) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/176-star-field.md section 10 in the sl-viewer repository |

Under a classic sky with RenderSkyAutoAdjustLegacy, the tone mapping's ACES curve crushes
the night clouds to black. cloudsF.glsl lifts the clouds (only their pixels) back towards the
brightness the same sky shows without the auto-adjust, undoing the curve with plus/skyscreen.

## Files

| File | What |
|---|---|
| plus/nightclouds/nightclouds.* | the setting (PlusNightCloudLift) and the night factor |
| lldrawpoolwlsky.cpp | `<plus>`: the cloud shader's uniforms |
| app_settings/shaders/class1/deferred/cloudsF.glsl | `<plus>`: the lift |
