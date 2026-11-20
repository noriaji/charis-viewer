# -*- cmake -*-
# Pose sharing (plus/poseshare, research/374): your own pose shown to the Charis Viewers near you, and theirs on your
# screen, through the pose bridge (plus/lslbridge). poseshareproto has no viewer types; poseshare.cpp holds the
# settings and the clock, poseshare_send.cpp the sender, poseshare_receive.cpp the ledger of the people shown.
# Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/poseshare/poseshare.cpp
    plus/poseshare/poseshare_pushed.cpp
    plus/poseshare/poseshare_receive.cpp
    plus/poseshare/poseshare_send.cpp
    plus/poseshare/poseshareproto.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/poseshare/poseshare.h
    plus/poseshare/poseshareinternal.h
    plus/poseshare/poseshareproto.h
    )

# poseshare_test.cpp is a stand-alone test (cl /EHsc /std:c++17 poseshare_test.cpp poseshareproto.cpp), not built here.
