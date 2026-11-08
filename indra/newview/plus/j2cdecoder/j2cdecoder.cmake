# -*- cmake -*-
# Switches and thread settings of the slj2k JPEG 2000 decoder (the decoder itself is in
# llimagej2coj/slj2k/). Included by plus/plus.cmake. Hook: llimagej2coj.cpp, CMakeLists.txt.

list(APPEND plus_SOURCE_FILES
    plus/j2cdecoder/j2cdecoder.cpp
    )
