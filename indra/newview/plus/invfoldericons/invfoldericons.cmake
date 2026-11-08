# -*- cmake -*-
# Inventory folder icons: one colour and emblem per kind of system folder, as in
# Firestorm, drawn for Charis (make_icons.py; skins/default/textures/plus/inv_folder_*.png).
# One hook in LLViewerFolderType::lookupIconName(). Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/invfoldericons/invfoldericons.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/invfoldericons/invfoldericons.h
    )
