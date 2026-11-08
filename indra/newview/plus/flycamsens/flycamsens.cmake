# -*- cmake -*-
# Sensitivity of the 3D mouse (flycam) on the number keys 1-9 (0: normal).
# Included by plus/plus.cmake.
# Hooks: llviewerinput.cpp (keyboard actions, default keys), llviewerjoystick.cpp (moveFlycam()).

list(APPEND plus_SOURCE_FILES
    plus/flycamsens/flycamsens.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/flycamsens/flycamsens.h
    )
