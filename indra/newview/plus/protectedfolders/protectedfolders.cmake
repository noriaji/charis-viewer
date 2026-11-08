# -*- cmake -*-
# Protected folders (plus/protectedfolders, from Firestorm's "Protect folder"): inventory
# folders the user marked cannot be deleted or moved elsewhere (research/140). Included by
# plus/plus.cmake. The window is registered from the feature's init and its menu item is in
# menu_plus.xml; the official hooks are listed in protectedfolders.h (removal, moves, drag
# and drop in the tree and the gallery, the folder menu).

list(APPEND plus_SOURCE_FILES
    plus/protectedfolders/protectedfolders.cpp
    plus/protectedfolders/protectedfoldersfloater.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/protectedfolders/protectedfolders.h
    )
