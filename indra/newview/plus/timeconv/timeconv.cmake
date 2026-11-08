# -*- cmake -*-
# Time converter (plus/timeconv, research/173): SL time, UTC and the time of places of choice,
# one moment on every row, edited from any field. Included by plus/plus.cmake. The window is
# registered from the feature's init and its menu item is in menu_plus.xml; the official hooks
# are two lines in LLStatusBar (a click on the SL clock opens it). timeconv_test.cpp is a
# stand-alone test of timeconvmath.h, not built here.

list(APPEND plus_SOURCE_FILES
    plus/timeconv/timeconvfloater.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/timeconv/timeconv.h
    plus/timeconv/timeconvmath.h
    )
