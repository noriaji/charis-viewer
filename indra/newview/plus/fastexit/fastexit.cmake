# -*- cmake -*-
# Fast exit (research/276). Stage 1: the exit time log (PlusExitTimeLog). Stage 2: the fast exit itself (fastexit.cpp, PlusFastExit).
# Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/fastexit/exittime.cpp
    plus/fastexit/exitwatch.cpp
    plus/fastexit/fastexit.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/fastexit/exittime.h
    plus/fastexit/exitwatch.h
    plus/fastexit/fastexit.h
    )
