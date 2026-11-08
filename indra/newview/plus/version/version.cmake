# -*- cmake -*-
# Charis Viewer's own version number (0.1.0.<build>, shown as
# "Charis Viewer 0.1.0 alpha 1 (rev N) based on Second Life Release 26.5.0"),
# apart from the official number of VIEWER_VERSION.txt. The number and the
# stage are set in plus/version/version.h. Included by plus/plus.cmake.
# Hooks: llversioninfo.cpp, llappviewer.cpp, llvvmquery.cpp (plus/plus-hooks.sh lists them).

list(APPEND plus_SOURCE_FILES
    plus/version/version.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/version/version.h
    )

# CHARIS_VERSION_MAJOR, _MINOR, _PATCH for CMake (numbers.cmake, also included
# early by cmake/BuildPackagesInfo.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/numbers.cmake)
