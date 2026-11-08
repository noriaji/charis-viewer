# llrhi — origin

| | |
|---|---|
| Source | Written for Charis Viewer: the render hardware interface between the viewer and its backends (the switches, the hooks, the passes' descriptions, the opt-in diagnostics) |
| Copyright | Charis Viewer contributors (each file's header) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/ in the sl-viewer repository (each file names its notes) |

None of these files is an official viewer file, and none holds code from
another project: a line by line comparison with the official viewer 26.5
(345ceae3db) found no shared runs of code longer than two lines. The headers
said "Copyright (C) 2026, Linden Research, Inc." until 2026-10-21; that was
the template of the official files, not Linden Lab's authorship.

The passes these files describe take their methods from elsewhere; where code
was taken, it is in `indra/llvulkan` and listed in `indra/llvulkan/ORIGIN.md`:

- `llrhigtao.*`: GTAO (the occlusion pass is a port of XeGTAO, Intel, MIT)
- `llrhissr.*`: screen space reflections after the official
  `project/graphics-care-package` (Linden Lab, LGPL 2.1); only the
  interface is here
- `llrhiglare.*`, `llrhivolumetric.*`, `llrhilook.*`: techniques from
  publications, see `indra/newview/plus/{glare,volumetric,look}/ORIGIN.md`
