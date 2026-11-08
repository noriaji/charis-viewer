# -*- cmake -*-
# Update check: once a day at start, read a small JSON file from our own site
# (PlusUpdateCheckURL) and say so when it names a newer Charis Viewer
# (research/123-update-check.md in the sl-viewer repository). Included by
# plus/plus.cmake. Official files: the check box in panel_preferences_setup.xml (en),
# and HttpOptions::setMaxReplyBytes() in llcorehttp (research/130 M2).
# updateversion_test.cpp is a stand-alone test, not built here.

list(APPEND plus_SOURCE_FILES
    plus/updatecheck/updatecheck.cpp
    plus/updatecheck/updateversion.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/updatecheck/updateversion.h
    )
