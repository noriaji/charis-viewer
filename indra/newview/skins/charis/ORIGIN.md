# The Charis skin (skins/charis)

Everything in this folder but this note is made by `indra/newview/plus/photoscreen/psui/make_ui_tokens.py`
from `tokens.txt` next to it (research/239 (12), branch ui-skin). Do not edit the made files: change
`tokens.txt` and run `python -I make_ui_tokens.py`; `tools/predeliver.py` (u) fails while they differ.

- `colors.xml`: the tokens' colours as `Ps<Name>` (the names the XUI windows use) and the official colour
  names the Charis look changes. Read after `skins/default/colors.xml` (it replaces the names it has).
- `textures/textures.xml` and `textures/charis/*.png`: the official image names drawn again in the photo
  screen's style (windows, toasts, inspectors, tooltips, push buttons, scroll bars, the title bar's buttons).

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
