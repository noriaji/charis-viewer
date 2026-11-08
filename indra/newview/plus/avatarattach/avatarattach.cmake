# -*- cmake -*-
# The attachments of an avatar (plus/avatarattach): "Attachments..." in the context menu of
# an avatar (menu_avatar_other/self.xml, menu_attachment_other/self.xml) opens a window that
# lists what the avatar wears. Included by plus/plus.cmake. The window and the menu callback
# are registered from the feature's init; the one official hook is in
# LLSelectMgr::processObjectProperties() (the names and creators of the attached objects).

list(APPEND plus_SOURCE_FILES
    plus/avatarattach/avatarattach.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/avatarattach/avatarattach.h
    )
