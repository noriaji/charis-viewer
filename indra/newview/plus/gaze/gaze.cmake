# -*- cmake -*-
# Camera gaze: avatars look at the camera, eyes first, the head only as much as
# needed (camera-gaze, 2026-10-17). Included by plus/plus.cmake. Applied through the <plus> hook in
# LLVOAvatar::updateCharacter() (llvoavatar.cpp).

list(APPEND plus_SOURCE_FILES
    plus/gaze/gaze.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/gaze/gaze.h
    )
