# -*- cmake -*-
# Inventory window like a file explorer: a detail pane on the right (name, type, creator,
# owner, date, permissions, description, UUID of the selected item) that shows when the
# window is wide enough. Included by plus/plus.cmake.
# Hooks: llpanelmaininventory.cpp (selection, draw), floater_my_inventory.xml (the pane).

list(APPEND plus_SOURCE_FILES
    plus/invexplorer/invexplorer.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/invexplorer/invexplorer.h
    )
