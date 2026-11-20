# -*- cmake -*-
# Contact sets where they show (plus/contactsethooks, research/333 cs-color): the set colours of the name tags,
# chat, minimap, radar and friends lists, a set's online notices and busy answer. The data is plus/contactsets
# (LGGContactSets); the official files call cshooks.h only (marked <plus> contactsets). Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/contactsethooks/cshooks.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/contactsethooks/cshooks.h
    )
