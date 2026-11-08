# -*- cmake -*-
# Outfit backup: a copy of what the agent wears (and where the modifiable
# attachments sit) kept in the account's folder, offered back at login when
# the server comes back with an older outfit. Included by plus/plus.cmake.
# No hooks in official files.

list(APPEND plus_SOURCE_FILES
    plus/outfitbackup/outfitbackup.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/outfitbackup/outfitbackup.h
    )
