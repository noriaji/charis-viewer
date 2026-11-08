# -*- cmake -*-
# Hides the objects that stand on land of another owner than the parcel you are on
# (PlusHideOtherLandObjects), or on any other parcel (PlusHideOtherParcelObjects). For you only: nothing is sent and nothing changes for
# others. Included by plus/plus.cmake. Hook: one condition in
# LLVOVolume::lineSegmentIntersect() (llvovolume.cpp), so what is hidden is not clicked.

list(APPEND plus_SOURCE_FILES
    plus/otherland/otherland.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/otherland/otherland.h
    )
