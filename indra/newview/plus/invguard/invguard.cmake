# -*- cmake -*-
# Inventory guards (plus/invguard): one registry that the official inventory entries ask
# whether a folder may be deleted, moved or dragged; protected folders are a guard
# (research/248 1.3). Included by plus/plus.cmake. The hooks are listed in invguard.h.

list(APPEND plus_SOURCE_FILES
    plus/invguard/invguard.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/invguard/invguard.h
    )
