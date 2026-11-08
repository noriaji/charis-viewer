# -*- cmake -*-
# Fetch connections tuned to the line (plus/httpautotune): the HTTP connections for textures and
# mesh2 start at 24 and follow the line between 24 and 192 (time to the first byte, HTTP 429 / 503).
# PlusFetchAutoTune switches it. Included by plus/plus.cmake. Hooks: llappcorehttp.cpp
# (refreshSettings()), lltexturefetch.cpp (commonUpdate()), llmeshrepository.cpp (request water
# levels), llcorehttp httprequest.h/.cpp and _httplibcurl.h/.cpp (completion observer; a new
# connection limit set without waiting for the class to go idle).

list(APPEND plus_SOURCE_FILES
    plus/httpautotune/httpautotune.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/httpautotune/httpautotune.h
    )
