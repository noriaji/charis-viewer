# -*- cmake -*-
# The parts of the time-based passes (TAA, and the temporal upscalers later:
# research/373) that are the same for all of them: the projection's jitter, the
# last frame's camera, and when a history must be dropped; and the main view's
# motion vectors (motion.cpp: the G-buffer's fifth attachment). Included by
# plus/plus.cmake. Hooked in llviewerdisplay.cpp (display: the jitter's scope) and
# pipeline.cpp (the motion vectors); plus/taa and plus/upscale ask it.

list(APPEND plus_SOURCE_FILES
    plus/temporal/temporal.cpp
    plus/temporal/motion.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/temporal/temporal.h
    plus/temporal/motion.h
    )
