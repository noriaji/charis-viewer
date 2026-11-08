# -*- cmake -*-
# Group titles (plus/grouptitles, from Firestorm's FSFloaterGroupTitles and FSGroupTitleRegionMgr): the window
# of every group title, and Auto Region (the title given to a region becomes the active one there). Included
# by plus/plus.cmake. The window is registered from the feature's init; it opens from the People window's
# Groups tab (panel_people.xml, marked <plus>) and the toolbar button plus_group_titles (commands_plus.xml).

list(APPEND plus_SOURCE_FILES
    plus/grouptitles/grouptitles.cpp
    plus/grouptitles/grouptitlesfloater.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/grouptitles/grouptitles.h
    )
