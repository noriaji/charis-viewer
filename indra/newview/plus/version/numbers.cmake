# -*- cmake -*-
# The Charis numbers for CMake: CHARIS_VERSION_MAJOR, CHARIS_VERSION_MINOR and
# CHARIS_VERSION_PATCH, read from version.h so that file stays the only place
# (research/123 H3, M1). The build number is Charis's own
# (VIEWER_VERSION_REVISION = CHARIS_BUILD_NUMBER of buildnumber.cmake; set by
# indra/cmake/BuildVersion.cmake before newview, so it is known here). Used by:
#   res/viewerRes.rc               the exe's version resource (FILEVERSION,
#                                  PRODUCTVERSION, FileVersion, ProductVersion)
#   cmake/BuildPackagesInfo.cmake  the first line of packages-info.txt (About >
#                                  Licenses), which includes this file itself:
#                                  it runs before plus/plus.cmake
# Included by version.cmake (and by plus.cmake's glob): once only.

include_guard(GLOBAL)

file(READ ${CMAKE_CURRENT_LIST_DIR}/version.h plus_version_header)
foreach(plus_version_part MAJOR MINOR PATCH)
    if (plus_version_header MATCHES "inline constexpr int ${plus_version_part} = ([0-9]+);")
        set(CHARIS_VERSION_${plus_version_part} ${CMAKE_MATCH_1})
    else ()
        message(FATAL_ERROR "plus/version/version.h: no ${plus_version_part} number")
    endif ()
endforeach()
unset(plus_version_header)

# CHARIS_VERSION_REVISION16: the fourth number of the exe's numeric version
# (FILEVERSION, PRODUCTVERSION in res/viewerRes.rc), which holds 16 bits
# (0..65535; rc would cut a larger one). Charis's build number (the commits
# since the branch from the official viewer, a few thousand) fits, so the
# numeric version shows it as it is. Should it ever pass 65535, the numeric
# version stays at 65535 (it never goes back down; the text versions,
# FileVersion, ProductVersion and packages-info.txt, keep the whole number).
# research/130 L5.
if (DEFINED VIEWER_VERSION_REVISION AND "${VIEWER_VERSION_REVISION}" MATCHES "^[0-9]+$")
    if (VIEWER_VERSION_REVISION GREATER 65535)
        set(CHARIS_VERSION_REVISION16 65535)
        message(WARNING "Build number ${VIEWER_VERSION_REVISION} does not fit the exe's numeric version; it shows 65535")
    else ()
        set(CHARIS_VERSION_REVISION16 ${VIEWER_VERSION_REVISION})
    endif ()
else ()
    set(CHARIS_VERSION_REVISION16 0)
endif ()

# A changed number in version.h re-runs configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/version.h)
message(STATUS "Charis Viewer version: ${CHARIS_VERSION_MAJOR}.${CHARIS_VERSION_MINOR}.${CHARIS_VERSION_PATCH}")
