# Inventory folder icons: origin

| | |
|---|---|
| Idea | Firestorm (Phoenix Firestorm Project): one texture name per folder type in `llviewerfoldertype.cpp` (`<FS:Ansariel> Use individual icons for different folder types`), shown with the old viewer's folder images (`skins/firestorm/textures/textures.xml` -> `legacy/inv_folder_*.tga`) |
| Code | Charis' own (a hook in `LLViewerFolderType::lookupIconName()` in place of Firestorm's changed table); GNU LGPL 2.1 only |
| Icons | Charis' own drawing, made by `make_icons.py` in this folder; Copyright Charis Viewer contributors, GNU LGPL 2.1 only like the rest of the viewer |

No image of Firestorm or of the Second Life viewer is copied. Firestorm's
`legacy/inv_folder_*.tga` are a mix of Linden Lab's old viewer artwork (CC BY-SA
3.0, doc/LICENSE-logos.txt) and icons added by Firestorm contributors whose
author and licence are not written down, so they were not taken.

## Remaking the icons

    python -I make_icons.py ../../skins/default/textures/plus --preview <preview.png>

writes `inv_folder_<key>_closed.png` and `_open.png` (16 x 16) for every key of
`FOLDERS`; `invfoldericons.cpp` (`TYPE_ICONS`) maps the folder types to the keys.
