# -*- cmake -*-
# The flycam's number keys: 1-9 its sensitivity (3D mouse), 0 levels the camera.
# Included by plus/plus.cmake.
# Hooks: llviewerinput.cpp (keyboard actions, default keys), llviewerjoystick.cpp (moveFlycam()),
#        llagentcamera.cpp/.h (levelFlycam), llflycam.cpp/.h (startLevel, cancelReset).
# Called from: plus/photomode/phototoolsfloater.cpp (the digits while its window has the focus),
#        plus/flycammouse/flycamexit.cpp (the how-to's lines).

list(APPEND plus_SOURCE_FILES
    plus/flycamsens/flycamsens.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/flycamsens/flycamsens.h
    )
