# -*- cmake -*-
# SLURL handler: which viewer opens secondlife:// links from the browser, and the
# buttons in Preferences > Setup that make it this one (HKCU only, on the press) or
# put back the one before; an optional notice at start (research/237-slurl-handler.md
# in the sl-viewer repository). Included by plus/plus.cmake. Official file changed:
# panel_preferences_setup.xml (en), the rows (panel_plus_slurl_handler.xml).

list(APPEND plus_SOURCE_FILES
    plus/slurlhandler/slurlhandler.cpp
    plus/slurlhandler/slurlregistry.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/slurlhandler/slurlregistry.h
    )
