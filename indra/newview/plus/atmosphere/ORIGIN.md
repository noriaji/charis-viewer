# Atmosphere effects — origin

| | |
|---|---|
| Source | Written for this viewer (no code taken from another viewer) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/148-atmosphere-effects.md in the sl-viewer repository |

The techniques are textbook ones:

- exponential height fog with the optical depth integrated in closed form along
  the view ray (Quilez, "Better fog", 2010; Wenzel, "Real-time Atmospheric
  Effects in Games", GDC 2006)
- Henyey-Greenstein phase function (Henyey & Greenstein 1941)
- fireflies: world-fixed cells walked along the view ray with a 2D digital
  differential analyser (Amanatides & Woo, "A Fast Voxel Traversal Algorithm
  for Ray Tracing", 1987), one point of light per cell on a Lissajous path
- hash: the integer hash used by plus/rain (hash-prospector, see
  llvulkan/ORIGIN.md)
