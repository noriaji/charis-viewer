# The Charis skin (skins/charis)

Everything in this folder but this note is made by `indra/newview/plus/photoscreen/psui/make_ui_tokens.py`
from `tokens.txt` next to it (research/239 (12), branch ui-skin). Do not edit the made files: change
`tokens.txt` and run `python -I make_ui_tokens.py`; `tools/predeliver.py` (u) fails while they differ.

- `colors.xml`: the tokens' colours as `Ps<Name>` (the names the XUI windows use) and the official colour
  names the Charis look changes. Read after `skins/default/colors.xml` (it replaces the names it has).
- `textures/textures.xml` and `textures/charis/*.png`: the official image names drawn again in the photo
  screen's style (windows, toasts, inspectors, tooltips, push buttons, scroll bars, the title bar's buttons),
  and the official small parts (`old_parts` in make_ui_tokens.py, branch ui-old-recolor): drop-downs, text and
  search fields, check boxes, radio buttons, spinner and scroll bar arrows, sliders, tabs, list rows, accordion
  headers and arrows, the inventory's "Received items" bar, segmented buttons, the floaters' bottom-bar buttons
  and bottom tabs, a docked floater's tongue, an inspector's back, the badges, and the location bar's parcel
  icons. Those keep the official image's size, 9 slices and preload (read from
  `skins/default/textures/textures.xml`), so the official windows keep their layout; only the colours change.
  `make_ui_tokens.py --check` fails when the official skin gets an image of those families that is not drawn
  again (or named as left on purpose), when an image the drawing takes the size of changes size, and when a 9
  sliced image (any in this folder) has a line in its stretched band or right at the band's edge (the drawing
  filters across that seam, so a border there is smeared over a few pixels as the band stretches;
  `check_stretched_bands`, with the drawings meant to be there named in `STRETCH_DRAWN`).
  Two kinds keep other 9 slices than the official ones (the image and where it is drawn do not change):
  `Toolbar_*` keep at least 2 px at every side (Left keeps the official 5 at the left and 4 at the bottom, Right
  5 at the right, Middle_Flash 5,23,29,4; the official keep 1 px at some: the border alone), and `ListItem_Select`
  keeps 3 px at the left (the official 2 are the accent's bar; one more is a line of the fill beside it).

## Images made from the official ones

- `shape_*.png` (`Flyout_Left/Right/Pointer`, `Inspector_Hover`, `Badge_Background`): the official image's
  opacity (its outline) with a token's colour; nothing else of the official image.
- `icon_parcel_forsale_light.png`, `icon_pathfinding_dirty.png`, `icon_pathfinding_disabled.png`: the official
  icons with their greys turned over (dark to light) for the dark field; their coloured marks are the
  official ones.
- `Parcel_VoiceNo/FlyNo/PushNo/BuildNo/SeeAVsOff_Light` are pointed at the official `icons/Parcel_*_Dark.png`
  (no image in this folder).

The skin is used through the setting `SkinCurrent` (default `charis`; lldir.cpp `setSkinFolder` lays
`skins/<SkinCurrent>` over `skins/default`). `viewer_manifest.py` copies it like every skin folder
(`*/textures/*/*.png`, `*/textures/textures.xml`, `*/*.xml`); this note is not copied.

## Icons

The title bar's icons (`textures/charis/title_*.png`, 16 px, line icons) are drawn from Lucide's path data
(https://lucide.dev, ISC license; the license text is in `plus/licenses-charis.txt`, section 1) with the
rasterizer of `make_ps_images.py`:

| title_ | Lucide |
|---|---|
| close | `x` |
| minimize | `minus` |
| restore | `copy` |
| dock | `arrow-down-to-line` |
| help | `circle-help` |

The other line art of the small parts is drawn the same way: the check mark (`check_*.png`) is Lucide's
`check`, the magnifier of the search fields (`search.png`, the official name `Search`, 13 px) Lucide's `search`;
the chevrons are plain lines.
