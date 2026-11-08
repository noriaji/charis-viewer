# Real night sky (real star positions) — origin

| | |
|---|---|
| Code | Written for this viewer (no code taken from another viewer, planetarium or library) |
| Code license | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), as the rest of the viewer |
| Design | research/177-real-sky-stars.md in the sl-viewer repository |
| State | stage 1: the catalogue and the position formulas with their self-test. Stages 2 to 4 (real-stars-2): drawn (PlusSkyStarsReal), the photo tools' rows, the constellation lines |

## Star catalogue (`app_settings/plus/starcat-bsc5.bin`)

| | |
|---|---|
| Source | The Bright Star Catalogue, 5th Revised Ed. (Preliminary Version), D. Hoffleit and W. H. Warren Jr., 1991. Yale University Observatory / NASA Astronomical Data Center |
| Copy used | CDS VizieR catalogue V/50, `catalog.gz` (https://cdsarc.cds.unistra.fr/ftp/V/50/catalog.gz), with its `ReadMe` (byte layout). Fetched 2026-10-24 |
| Source SHA-256 | `catalog.gz` 3dc44b1e90be8fbe5bcc7656032560f51275f985c7e3f783c9028e1838ec7bed, `ReadMe` 44fd9c73e2eecad0beb47bdfa3f01c60fd43f93d6964198e31fcd48732de5b33 |
| License | A work of the U.S. Government, public domain: the NASA HEASARC entry of the same catalogue (`bsc5p`) on data.gov (https://catalog.data.gov/dataset/bright-star-catalog) gives the license https://www.usa.gov/government-works (checked 2026-10-24) |
| Made by | `tools/starcat/make_starcat.py` in the sl-viewer repository (the source copy is kept in `tools/starcat/data/`). The output depends on the source bytes only |
| Output | 9,096 stars (the 14 entries without a position left out), 91,004 bytes, SHA-256 791514f0c04c64c8d5399c98e7ef5e2f79556c70a874dc21bae1c9743aef5894 |
| Kept | J2000 position (as a unit vector, int16, within 6 arc seconds), V magnitude, B-V colour (0.6 where the catalogue has none). Names, spectra, proper motions and the rest are not kept |

## Constellation lines (`app_settings/plus/starcat-lines.bin`, stage 4)

| | |
|---|---|
| Source | d3-celestial by Olaf Frohn, `data/constellations.lines.json` (https://github.com/ofrohn/d3-celestial): the lines of the 88 constellations, as J2000 coordinates |
| License | BSD 3-Clause, Copyright (c) 2015, Olaf Frohn (the repository's LICENSE, checked 2026-10-25; no separate terms for the data). The notice and the license's full text are in `plus/licenses-charis.txt` (shipped next to the viewer as `licenses-charis.txt`); the copy of the source and its LICENSE are kept in `tools/starcat/data/` of the sl-viewer repository |
| Made by | `tools/starcat/make_starlines.py` in the sl-viewer repository: each corner taken to the catalogue's star within 0.2 degrees (the brighter one first), written as pairs of star numbers of `starcat-bsc5.bin` |
| Copy used | `https://raw.githubusercontent.com/ofrohn/d3-celestial/master/data/constellations.lines.json`, fetched 2026-10-25: 27,136 bytes, SHA-256 294f66bef5d5cf50b1e17f16d2efa1d97a15131612c68dd935adef6e7373e13c (89 features: the 88 constellations, Serpens in two parts; 893 corners). `LICENSE` 1,480 bytes, SHA-256 a8c79239001ad4bea243d1796b85084e1fc39309c5c5a663930a1b46320254c6 |
| Output | 743 segments, 3,048 bytes, SHA-256 96808b2024e234089398389f64f64deaf2e31f1f96e9a3725abe2c561ba0a05f. Every corner found its star: median 2.5 arc seconds, 99 % within 10 arc seconds; 3 corners (Cap, Cha, Lyr: double stars) went to the brighter star of the pair, 3.5 to 6.4 arc minutes away (about 2 pixels at a 60 degree view) |

Not used for the lines: Stellarium's `constellationship.fab` (GPL-2.0+).

## Constellation pictures (`app_settings/plus/starart.bin`, `starart-tex.bin`, research/288)

| | |
|---|---|
| Pictures | Stellarium's "modern" sky culture, `skycultures/modern/illustrations/*.png` (https://github.com/Stellarium/stellarium), drawn by Johan Meuris (as Debian's copyright file of Stellarium names him). 85 pictures (3 of the 88 constellations have none). Fetched 2026-11-06 at Stellarium's commit 9dac07a412b1 (pinned) |
| Pictures' license | Free Art License 1.3 (https://artlibre.org/licence/lal/en/). Changed by Charis Viewer contributors, 2026: made grey (the few RGB ones by their luma), the smaller ones (128, 256) enlarged to 512 x 512, reduced pictures (mipmaps, 8 levels) added, compressed to BC4. The changed pictures are under the Free Art License 1.3 too. Each picture's own brightness is kept (its brightest 98 .. 215 of 255) |
| Placing | The three anchors of each picture in Stellarium's `skycultures/modern/index.json` (Stellarium's team, CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/), their stars' J2000 positions from the Hipparcos Catalogue (ESA 1997, CDS VizieR I/239). Changed: the anchors turned into the picture's four corners on the plane through the three anchor stars (as Stellarium places them). Then 7 pictures (Car, Aur, Crt, Dra, Hya, Mon, Aqr) refit so that the stars of index.json's constellation lines (679 stars, their positions from the same Hipparcos Catalogue) lie on the drawn figure: a small affine correction with the anchors held (research/291-starart-fit.md). On those 7 an anchor star is up to 3.7 pixels (0.24 degrees) from its anchor and the plane is tilted slightly, so they no longer go exactly through the three anchor stars. `starart.bin` is under CC BY-SA 4.0 too |
| Made by | `tools/starcat/make_starart.py` of the sl-viewer development repository (its master, 0558e1a and 049c364; the refit: bb16d89 and c52fff4, made with Python 3.10.11, numpy 2.2.6, Pillow 12.3.0; the sources it reads are kept beside it), once before the build: nothing is fetched, decoded or compressed while the viewer runs. The two files come with the viewer's branches star-art-data and star-art-fit (the refit) |
| Output | `starart.bin` 85 plates, 4,464 bytes, SHA-256 98c8d4cc9c36aca0d3af211a17f57028befd6fcdb6114073836c9f10f4c44ad5 (with the refit; without it, `--no-fit`, 118c7184d443a1ff…, and before the line stars were among the sources 868ad458ca7f…). `starart-tex.bin` 85 layers of 512 x 512 BC4_UNORM, 8 mips, 14,854,616 bytes; version 2 (a mip's layers together, uploaded a mip at a time), SHA-256 8f8ae196a9ba7d34a97592e1cdaca337ee907bb8b9ed528780abe3cb22eb72da (the first file, version 1 with each layer's mips together, is not read) |
| Notices | `plus/licenses-charis.txt` (shipped as `licenses-charis.txt`): the authors, the licenses, the originals' place and the changes. The pictures and the data come with no warranty |
| Shown | Over the real sky only (`PlusSkyStarsArt`, off by default), faint and bluish (the colour is the viewer's: the pictures are grey) |

Not used: HYG (CC BY-SA 4.0, a share-alike condition on derived data), Hipparcos / Tycho (not needed down to magnitude 6.5).

## Formulas

From J. Meeus, *Astronomical Algorithms*, 2nd ed. (1998), written here from the book's equations:
mean sidereal time (12.4), equatorial to horizontal coordinates (13), rigorous precession from J2000.0 (21.2–21.4),
the Sun's position at low accuracy (25). The self-test uses the book's worked examples 12.a, 12.b, 13.b, 21.b and 25.a.

## Files

| File | What |
|---|---|
| plus/realstars/realskymath.h | time to sidereal time, precession, the sky matrix (J2000 to east-north-up), the Sun |
| plus/realstars/realstarcat.h | the readers of `starcat-bsc5.bin` and `starcat-lines.bin` (from bytes in memory) |
| plus/realstars/realstarart.h | the readers of `starart.bin` and `starart-tex.bin` (from bytes in memory; the pictures' blocks are uploaded as they are) |
| plus/realstars/realskytime.h | the setting's UTC text and the photo tools' date box |
| plus/realstars/realstars.h / .cpp | the catalogue into the star field, the sky's turn for the place and the time (the viewer's side) |
| plus/realstars/realsky_test.cpp | stand-alone test (run by tools/selftest as `unit-realsky`) |
| app_settings/plus/starcat-bsc5.bin | the catalogue (above) |
| app_settings/plus/starcat-lines.bin | the constellation lines (above) |
| app_settings/plus/starart.bin, starart-tex.bin | the constellation pictures (above) |
