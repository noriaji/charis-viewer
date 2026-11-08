# -*- cmake -*-
# Frame rate limit (plus/fpslimit): the main loop waits so a frame takes at least 1/limit seconds
# (PlusFpsLimit; 0 = no limit), and a button sets the limit to the refresh rate of the monitor the
# window is on. Included by plus/plus.cmake. Called from the <plus> hook in LLAppViewer::doFrame()
# (llappviewer.cpp); the setting row is in floater_preferences_graphics_advanced.xml.

list(APPEND plus_SOURCE_FILES
    plus/fpslimit/fpslimit.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/fpslimit/fpslimit.h
    )
