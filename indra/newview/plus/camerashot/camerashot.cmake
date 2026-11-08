# -*- cmake -*-
# Camera shots: a slow push-in / pull-out, a zoom and a dolly zoom, optionally locked
# on an avatar (camerashot). Included by plus/plus.cmake.
# No official file is changed: the window is registered from the feature's init and
# its menu item is in menu_plus.xml. shotmath_test.cpp is a stand-alone test, not built here.

list(APPEND plus_SOURCE_FILES
    plus/camerashot/camerashot.cpp
    plus/camerashot/camerashotfloater.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/camerashot/camerashot.h
    plus/camerashot/shotmath.h
    )
