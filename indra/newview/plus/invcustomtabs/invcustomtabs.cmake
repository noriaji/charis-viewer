# -*- cmake -*-
# Named filter tabs in the inventory window: the "+" tab after the official ones makes a new
# tab, and the search words and filters (types, dates, creator, search type) set in it stay with it;
# right click renames, clones, hides or closes it, and the arrow tab brings hidden ones
# back. Kept per account (FSInventoryCustomTabs). Ported from Firestorm (FIRE-35598,
# FIRE-36831; the idea comes from the Catznip viewer). research/314.
# Included by plus/plus.cmake.
# Hooks: llpanelmaininventory.{h,cpp}, llinventoryfunctions.cpp (reset_inventory_filter),
# llinventorypanel.cpp (openInventoryPanelAndSetSelection, doCreate), llinventoryfilter.cpp (isSinceLogoff),
# llui/lltabcontainer.{h,cpp}
# (five small functions: getTabContainedAtPoint, setTabPadding, setTabButtonVisible, moveTabPanel,
# setTabButtonWidth; and the current index in removeTabPanel),
# menu_inventory_custom_tab.xml, notifications.xml, strings_plus.xml, panel_main_inventory.xml.

list(APPEND plus_SOURCE_FILES
    plus/invcustomtabs/invcustomtabs.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/invcustomtabs/invcustomtabs.h
    )
