# -*- cmake -*-
# Camera roll (tilting the camera) for the ordinary camera. Included by plus/plus.cmake.
# The official camera has no roll of its own except the flycam's. The hooks in llagentcamera.cpp
# call plus::camroll::apply() at the end of LLAgentCamera::updateCamera() and
# plus::camroll::reset() where the camera is reset. The photo tools (plus/photomode) set it.

list(APPEND plus_SOURCE_FILES
    plus/camroll/camroll.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/camroll/camroll.h
    )
