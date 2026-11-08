# -*- cmake -*-
# Area search (plus/areasearch, from Firestorm's FSAreaSearch): lists the objects of the
# region by name, description, owner, group, creator and more (research/140). Included by
# plus/plus.cmake. The window is registered from the feature's init and its menu item is in
# menu_plus.xml; the one official hook is in LLViewerRegion::killInvisibleObjects().

list(APPEND plus_SOURCE_FILES
    plus/areasearch/areasearch.cpp
    plus/areasearch/areasearchfloater.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/areasearch/areasearch.h
    )
