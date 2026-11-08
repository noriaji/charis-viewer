# -*- cmake -*-
# Fast exit (research/276). Stage 1: the exit time log only (PlusExitTimeLog).
# Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/fastexit/exittime.cpp
    plus/fastexit/exitwatch.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/fastexit/exittime.h
    plus/fastexit/exitwatch.h
    )
