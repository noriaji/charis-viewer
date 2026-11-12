# -*- cmake -*-
# Inventory window like a file explorer: a detail pane on the right (name, type, creator,
# owner, date, permissions, description, UUID of the selected item) that shows when the
# window is wide enough. Included by plus/plus.cmake.
# Wider still, three columns: the tab's folders, the objects in the chosen folder, the detail
# (invcolumns: what they mean; invlayout: where they are and when they come; research/318).
# Hooks: llpanelmaininventory.cpp (selection, draw, destructor, the gear menu's column),
# llavataractions.cpp (the center's selection for wear, share and the like), llui/llfolderview.{h,cpp}
# and llfolderviewitem.cpp (folders only), llinventoryfilter.{h,cpp} (plusCopyFrom). The window's XML
# (floater_my_inventory.xml) is the official one: the pane, the center and the splits are made in code.

list(APPEND plus_SOURCE_FILES
    plus/invexplorer/invcolumns.cpp
    plus/invexplorer/invexplorer.cpp
    plus/invexplorer/invlayout.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/invexplorer/invcolumns.h
    plus/invexplorer/invexplorer.h
    plus/invexplorer/invlayout.h
    )
