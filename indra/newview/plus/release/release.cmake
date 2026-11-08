# -*- cmake -*-
# What the downloadable build (LL_RELEASE_FOR_DOWNLOAD, CMake option
# CHARIS_RELEASE_FOR_DOWNLOAD in 00-Common.cmake, research/138 section 8) does
# at startup that a developer build does not. Included by plus/plus.cmake.
# Hook: initConfiguration() (llappviewer.cpp).

list(APPEND plus_SOURCE_FILES
    plus/release/release.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/release/release.h
    )
