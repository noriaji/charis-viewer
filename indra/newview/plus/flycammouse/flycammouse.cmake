# -*- cmake -*-
# The flycam on the keyboard and mouse: look with the mouse, wheel speed, Shift boost, gentle keys (research/219).
# Included by plus/plus.cmake.
# Hooks: llagentcamera.cpp (updateFlycam(), toggleFlycam()), llflycam.* (addLook()), llviewerwindow.cpp (right button,
# wheel, focus lost), llviewerinput.cpp (bindKeys, toggle_flycam), llviewerjoystick.cpp (toggleFlycam() with no device),
# llviewermenu.cpp (View.EnableJoystickFlycam), llagent.cpp (updateFlycam()'s frame time), llviewerwindow.cpp (letter keys).
# Settings tab: panel_plus_prefs_flycam_mouse.xml, included from panel_plus_prefs_game_control.xml (research/266;
# flycammouse.cpp's comment still names the old panel_plus_flycam_mouse_prefs.xml, which only the official panel
# includes until prefs-clean takes its plus mark back).

list(APPEND plus_SOURCE_FILES
    plus/flycammouse/flycamexit.cpp
    plus/flycammouse/flycammouse.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/flycammouse/flycammouse.h
    )
