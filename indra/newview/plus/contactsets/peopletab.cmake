# -*- cmake -*-
# Contact sets in the People window (plus/contactsets/peopletab, research/333; branch cs-people):
# the Contact Sets tab, the sets line of the profile, the avatar picker's Contact Sets tab and the
# "Add to Set" of the menus. The data (LGGContactSets) is listed by contactsets.cmake (cs-core).
# Included by plus/plus.cmake (its glob takes every .cmake of the feature folders).

list(APPEND plus_SOURCE_FILES
    plus/contactsets/peopletab.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/contactsets/peopletab.h
    )
