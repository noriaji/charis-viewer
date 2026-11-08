# Tonemap types 2..4 — origin

No C++ here. The code is a marked block (`// <plus>` ... `// </plus>`) in the
official `app_settings/shaders/class1/deferred/tonemapUtilF.glsl`, plus two
`default:` lines in its two `switch (tonemap_type)`, three combo items in
`floater_preferences_graphics_advanced.xml` (en, ja) and the comment of
`RenderTonemapType` in `settings.xml`.

| | |
|---|---|
| Source | Black Dragon Viewer (NiranV), https://github.com/NiranV/Black-Dragon-Viewer, master `b2ca434` (5.6.3, 2026-07-01), `tonemapUtilF.glsl` |
| License | GNU LGPL 2.1 only (the file carries the Linden Lab `viewerlgpl` header) |
| Author | NiranV Dean (Black Dragon); the formulas are public |
| Design | research/116-hable-filmic-design.md in the sl-viewer repository |

Values match Black Dragon's `RenderTonemapType` (2 = Reinhard, 3 = Hable /
Uncharted 2, 4 = Hejl / Burgess-Dawson), so the same setting gives the same look.

Formulas:
- Reinhard: Reinhard et al. 2002, extended form with a white point, on luminance.
- Hable: John Hable, "Filmic Tonemapping Operators" (2010), constants A..F, W as in Uncharted 2, exposure bias 2.
- Hejl: Jim Hejl and Richard Burgess-Dawson (2010), 6.2 / 0.5 / 1.7 / 0.06, then `pow(., 2.2)`.

Differences from Black Dragon (on purpose):
- Black Dragon applies types 2..4 to the raw colour (auto exposure and
  `RenderExposure` do not act). Here they act on the exposed colour, like types 0 and 1.
- Reinhard returns the colour unchanged for luminance 0 (Black Dragon divides by zero).
- Hable clamps negative input to 0.
