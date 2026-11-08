# -*- cmake -*-
# Pathfinding view without Havok: LLPathingLib implemented on the client from the region's
# own geometry (a navigation grid), instead of the Havok package Linden Lab does not publish.
# Included by plus/plus.cmake. Hook in an official file: llphysicsextensionsos/llpathinglib.{h,cpp}
# (LLPathingLib::setReplacement, marked <plus>). navgrid_test.cpp is a stand-alone test, not built here.

list(APPEND plus_SOURCE_FILES
    plus/pathing/pathing.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/pathing/pathing.h
    plus/pathing/navgrid.h
    )
