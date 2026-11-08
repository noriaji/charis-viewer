# -*- cmake -*-
# Plus: features ported into this viewer from other viewers.
# Included once from indra/newview/CMakeLists.txt (the <plus> hook), before
# add_executable(). Each feature folder plus/<feature>/ has a <feature>.cmake
# that appends its files to plus_SOURCE_FILES / plus_HEADER_FILES; they are
# picked up by the glob below, so adding a feature needs no edit here
# (re-run CMake configure after adding one).

set(plus_SOURCE_FILES
    plus/plus.cpp
    )

set(plus_HEADER_FILES
    plus/plus.h
    )

file(GLOB plus_FEATURE_CMAKE_FILES ${CMAKE_CURRENT_LIST_DIR}/*/*.cmake)
list(SORT plus_FEATURE_CMAKE_FILES)
foreach(plus_feature_cmake ${plus_FEATURE_CMAKE_FILES})
    include(${plus_feature_cmake})
endforeach()

# Plus sources live in subfolders of newview: give them the newview folder
# (for official headers) and plus/ (for plus.h) on the include path, without
# changing the include path of any official source file. Appended: a feature's .cmake may
# have given one of its files a folder of its own (localmesh.cmake: libxml2).
set_property(SOURCE ${plus_SOURCE_FILES}
    APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_LIST_DIR}")

source_group("Plus" FILES ${plus_SOURCE_FILES} ${plus_HEADER_FILES})

list(APPEND viewer_SOURCE_FILES ${plus_SOURCE_FILES})
list(APPEND viewer_HEADER_FILES ${plus_HEADER_FILES})
