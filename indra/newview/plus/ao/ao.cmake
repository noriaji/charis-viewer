# -*- cmake -*-
# Animation Overrider, ported from Firestorm (see SOURCE.md; research/ao-port.md
# in the sl-viewer repository). Included by plus/plus.cmake. The motion hooks are
# in LLVOAvatar::startMotion()/stopMotion() (llvoavatar.cpp, <plus>).

list(APPEND plus_SOURCE_FILES
    plus/ao/ao.cpp
    plus/ao/aoengine.cpp
    plus/ao/aofloater.cpp
    plus/ao/aoset.cpp
    plus/ao/aostore.cpp
    plus/ao/aotransfer.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/ao/ao.h
    plus/ao/aoengine.h
    plus/ao/aofloater.h
    plus/ao/aoset.h
    plus/ao/aostore.h
    plus/ao/aotransfer.h
    )

# Feature-local headers (aoengine.h etc.) are found next to the sources.
