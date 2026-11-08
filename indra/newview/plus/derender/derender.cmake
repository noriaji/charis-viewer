# -*- cmake -*-
# Derender: the selection hidden from the right-click menu until the next teleport (from
# Firestorm's temporary derender; research/251 in the sl-viewer repository). Included by
# plus/plus.cmake. Hooks: llviewerobjectlist.cpp (processObjectUpdate, processObjectUpdateFromCache,
# orphanize), menu_object.xml, menu_attachment_other.xml, menu_avatar_other.xml.

list(APPEND plus_SOURCE_FILES
    plus/derender/derender.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/derender/derender.h
    )
