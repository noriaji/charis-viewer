# -*- cmake -*-
# Group chat black and white lists, chosen in the group window. Included by plus/plus.cmake.
# Hooks: LLViewerChatterBoxInvitation::post() and LLIMMgr::addMessage() (llimview.cpp),
# the panel in panel_group_general.xml (en).

list(APPEND plus_SOURCE_FILES
    plus/groupwhitelist/groupwhitelist.cpp
    plus/groupwhitelist/groupwhitelistpanel.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/groupwhitelist/groupwhitelist.h
    )
