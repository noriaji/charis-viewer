# -*- cmake -*-
# Windows that touch move together, snapping to the near windows only (plus/floatersnap, research/387). Included by
# plus/plus.cmake. Official files: llui/lldraghandle.cpp (four hooks) and llui/llfloater.cpp (two: canSnapTo, reshape), all marked <plus> (9 marks); the
# settings (PlusFloaterGroupMove, PlusFloaterSnapNearOnly, PlusFloaterSnapGap) are in settings_plus.xml, their switches in
# panel_plus_prefs_general.xml. floatersnap_test.cpp is a stand-alone test of floatersnapmath.h, not built here.

list(APPEND plus_SOURCE_FILES
    plus/floatersnap/floatersnap.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/floatersnap/floatersnap.h
    plus/floatersnap/floatersnapmath.h
    )
