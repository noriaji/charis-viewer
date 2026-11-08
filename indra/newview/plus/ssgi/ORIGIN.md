# Screen space indirect light (SSGI) — origin

| | |
|---|---|
| Source | Written for this viewer from a published method; no shader or code of anyone else's is taken |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/164-ssgi.md in the sl-viewer repository |

The method is that of O. Therrien, Y. Levesque and G. Gilet, "Screen Space
Indirect Lighting with Visibility Bitmask" (2023, arXiv 2301.11376): the
horizon search of GTAO (Jimenez et al., "Practical Realtime Strategies for
Accurate Indirect Occlusion", 2016) with each slice's hemisphere cut into 32
sectors, each sample covering the sectors between its front and a point a
fixed thickness behind it, and the sectors it covers first bringing its
light. The passes (`llvulkan/llvkssgi.cpp`) were written from that
description, not from the paper's or anyone's code.

What is this viewer's own: the sectors are of equal cosine weight (the sine
of the angle from the normal within the slice), so the share of covered
sectors is the cosine-weighted visibility GTAO's light map takes; two radii
over one march (the ambient occlusion's, PlusGtaoRadius, and the light's,
PlusSsgiRadius); the light is the last frame's lit opaque screen at half
size, held at a ceiling and moved by the camera's motion, and the slices are
weighted by the projected normal's length. The 4x4 Bayer pattern and the 4x4
depth-aware average follow plus/gtao (this viewer's own, see its ORIGIN.md);
the normal reconstruction from the depth (the nearer neighbour on each axis)
is a common technique, written here anew.
