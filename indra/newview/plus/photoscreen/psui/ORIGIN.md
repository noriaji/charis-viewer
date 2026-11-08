# Origin of the photo screen's icons

`make_ps_images.py` draws `skins/default/textures/plus/ps_icon_*.png` (40x40, white, coloured when drawn)
from SVG path data on Lucide's 24 unit grid with Lucide's round 2 unit lines.

- Lucide, https://lucide.dev, ISC license (portions from Feather by Cole Bemis, MIT). The license text is in
  `plus/licenses-charis.txt` (section 1).
- The path data was written into the script from Lucide's published icons (not fetched by a build step).
  Before a release, compare the icons with lucide.dev once more (the shapes, not the pixels).

| ps_icon_ | made from |
|---|---|
| scene | Lucide `image` |
| avatar | Lucide `user`, filled (the body closed and filled) |
| environment | Lucide `droplet` and `leaf`, smaller and put together |
| lighting | Lucide `lightbulb`, smaller, with rays like Lucide `sun` |
| pose | drawn for Charis Viewer in Lucide's style (a runner; Lucide has none) |
| camera | Lucide `camera`, filled, the lens cut out and filled again |
| effects | Lucide `sparkles` |
| presets | Lucide `file-text` |
| clapperboard | Lucide `clapperboard` |
| sliders | Lucide `sliders-horizontal` |
| user | Lucide `user` |
| aperture | Lucide `aperture` |
| sun | Lucide `sun` |
| palette | Lucide `palette` |
| output | Lucide `share` |
| film | Lucide `film` |
| resolution | Lucide `scan` with a square inside |
| ratio | Lucide `ratio` |
| grid | Lucide `grid-3x3` |
| focus | Lucide `focus` |
| frame | Lucide `scan` with a dot |
| maximize | Lucide `maximize-2` |
| scan_face | Lucide `scan-face` |
| flycam | Lucide `move-3d` |
| chevron_down, chevron_up | Lucide `chevron-down`, `chevron-up` (thinner lines) |
| more | Lucide `ellipsis` |
| plus | Lucide `plus` (thinner lines) |
| settings | Lucide `settings`, filled |
| check | Lucide `check` (thicker lines: it is shown small) |
| close | Lucide `x` |
| star, star_filled | the five-pointed star of Feather / Lucide `star` (sharp corners; outline and filled) |
| water | Lucide `droplet` |
| quality | Lucide `gauge` |
| message | Lucide `message-circle` |
| send | Lucide `send` |
| chevron_left, chevron_right | Lucide `chevron-left`, `chevron-right` (thinner lines) |
| rotate_ccw, rotate_cw | Lucide `rotate-ccw`, `rotate-cw` |
| minus | Lucide `minus` (thinner lines) |

The same script draws the photo screen's toolbar button `photoscreen_18.png` (18x18, white, filled like
`phototools_18.png`, on an 18 unit grid so its edges fall on pixels):

| file | made from |
|---|---|
| photoscreen_18.png | the corners of Lucide `scan` with a small filled Lucide `camera` inside, redrawn at 18 px |

The masks (`ps_round.png`, `ps_circle.png`, `ps_cut.png`, `ps_shadow.png`) are made by the same script for
Charis Viewer.
