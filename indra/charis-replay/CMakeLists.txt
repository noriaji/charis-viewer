# -*- cmake -*-
# research/271: charis-replay, the frame replay tool (development only, not
# shipped). P0: the viewer's renderer (llrender with llrhi and llvulkan, as
# the viewer links it) in an executable of its own, without newview. Added
# with EXCLUDE_FROM_ALL (indra/CMakeLists.txt): built only when asked for
# (cmake --build . --target charis-replay), never by the viewer's build.

project(charis-replay)

include(00-Common)
include(LLCommon)
include(LLImage)
include(LLWindow)

if (NOT (DEFINED ENV{VULKAN_SDK} AND WINDOWS))
  message(STATUS "charis-replay: needs Windows and the Vulkan SDK (VULKAN_SDK), not built")
  return()
endif ()
# Only open source parts (a project rule): llimage would bring Kakadu.
if (USE_KDU)
  message(STATUS "charis-replay: not built with USE_KDU (Kakadu is proprietary)")
  return()
endif ()

set(charis-replay_SOURCE_FILES
    charis_replay.cpp
    )

add_executable(charis-replay ${charis-replay_SOURCE_FILES})

# llrender carries llrhi and llvulkan (llrhi.cmake), and its INTERFACE link
# option (llvulkan.cmake) delay loads vulkan-1.dll as in the viewer.
# llwindow defines the window hooks the renderer sets (gLLRHIAttachWindow
# and the others).
target_link_libraries(charis-replay
    llrender
    llwindow
    llimage
    llfilesystem
    llmath
    llcommon
    shell32
    )

set_target_properties(charis-replay PROPERTIES FOLDER "Tools")
