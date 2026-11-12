# -*- cmake -*-
# Poser, ported from Firestorm (see ORIGIN.md). Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/poser/fsfloaterposer.cpp
    plus/poser/fsjointpose.cpp
    plus/poser/fsmanipikjoint.cpp
    plus/poser/fsmaniprotatejoint.cpp
    plus/poser/fsmaniptranslatejoint.cpp
    plus/poser/fsposeranimator.cpp
    plus/poser/fsposestate.cpp
    plus/poser/fsposingmotion.cpp
    plus/poser/fstoolcomppose.cpp
    plus/poser/fsvirtualtrackpad.cpp
    plus/poser/handcurl.cpp
    plus/poser/poser.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/poser/fsfloaterposer.h
    plus/poser/fsjointpose.h
    plus/poser/fsmanipikjoint.h
    plus/poser/fsmaniprotatejoint.h
    plus/poser/fsmaniptranslatejoint.h
    plus/poser/fsposeranimator.h
    plus/poser/fsposestate.h
    plus/poser/fsposingmotion.h
    plus/poser/fstoolcomppose.h
    plus/poser/fsvirtualtrackpad.h
    plus/poser/handcurl.h
    )

# handcurl_test.cpp is a stand-alone test (cl /EHsc /std:c++17 handcurl_test.cpp handcurl.cpp), not built here.
# Feature-local headers (fsposingmotion.h etc.) are found next to the sources.
