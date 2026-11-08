# -*- cmake -*-
# Camera path: camera points the camera goes through in turn, smoothly or with a cut
# (research/150-camera-path.md). Included by plus/plus.cmake.
# The window is registered from the feature's init and its menu item is in menu_plus.xml. The keys
# (camera_path_add, camera_path_play) are registered in llviewerinput.cpp between <plus> marks.
# camerapathbench.cpp: measuring runs started by cue files (research/153).
# pathmath_test.cpp and shakemath_test.cpp are stand-alone tests, not built here.

list(APPEND plus_SOURCE_FILES
    plus/camerapath/camerapath.cpp
    plus/camerapath/camerapathbench.cpp
    plus/camerapath/camerapathfloater.cpp
    plus/camerapath/camerapathoverlay.cpp
    plus/camerapath/camerapathscript.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/camerapath/camerapath.h
    plus/camerapath/pathmath.h
    plus/camerapath/shakemath.h
    )
