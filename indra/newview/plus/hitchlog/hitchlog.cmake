# -*- cmake -*-
# Hitch log: when a frame takes too long, a watchdog thread samples where the
# main thread is and writes the stacks to the log (research/274).
# Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/hitchlog/hitchlog.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/hitchlog/hitchlog.h
    )
