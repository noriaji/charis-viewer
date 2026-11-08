# -*- cmake -*-
# Safe mode (research/254 in the sl-viewer repository): --safe or Shift held at
# the start runs the viewer again with the experimental switches off
# (safemode_table.h), and a "starting" marker in the logs folder counts the
# starts that did not get through. Included by plus/plus.cmake. One <plus>
# hook: plus::safemode::relaunchIfAsked() first in wWinMain
# (llappviewerwin32.cpp); the rest registers itself (plus::FeatureRegistration).

list(APPEND plus_SOURCE_FILES
    plus/safemode/safemode.cpp
    plus/safemode/safemode_badge.cpp
    plus/safemode/safemode_launch.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/safemode/safemode.h
    plus/safemode/safemode_badge.h
    plus/safemode/safemode_files.h
    plus/safemode/safemode_table.h
    )
