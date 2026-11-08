# -*- cmake -*-
# llrhi is compiled into llrender (it needs llrender's GL function pointers,
# and llrender calls back into it). Included from the end of
# llrender/CMakeLists.txt so upstream only carries a single line.

set(llrhi_SOURCE_FILES
    ${CMAKE_CURRENT_LIST_DIR}/llrhi.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiatmosphere.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiavatar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhidrawlist.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiframe.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiglare.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiglstats.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhigtao.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiimmediate.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhilook.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiocclusion.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhipass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhipassstate.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiprofiler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhirain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiresource.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiscene.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhishaderdump.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiskinfill.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiskinsss.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhissgi.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhissr.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhitaa.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiuploadtrace.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiupscale.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhivbcensus.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhivkmirror.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhivolumetric.cpp
    ${CMAKE_CURRENT_LIST_DIR}/llrhiwetlook.cpp
    ${CMAKE_CURRENT_LIST_DIR}/replay/llrhireplaycapture.cpp
    )

set(llrhi_HEADER_FILES
    ${CMAKE_CURRENT_LIST_DIR}/llrhi.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiatmosphere.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiavatar.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhidrawlist.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiframe.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhigl.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiglare.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiglstats.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhigtao.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhihash.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiimmediate.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhilook.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiocclusion.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhipass.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhipassstate.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiprofiler.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhirain.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiresource.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiscene.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhishaderdump.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiskinfill.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiskinsss.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhissgi.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhissr.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhitaa.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiuploadtrace.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiupscale.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhivbcensus.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhivkmirror.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhivolumetric.h
    ${CMAKE_CURRENT_LIST_DIR}/llrhiwetlook.h
    ${CMAKE_CURRENT_LIST_DIR}/replay/llrhireplaycapture.h
    ${CMAKE_CURRENT_LIST_DIR}/replay/llrhireplayformat.h
    )

target_sources(llrender PRIVATE ${llrhi_SOURCE_FILES} ${llrhi_HEADER_FILES})
target_include_directories(llrender PUBLIC ${CMAKE_CURRENT_LIST_DIR})
source_group("llrhi" FILES ${llrhi_SOURCE_FILES} ${llrhi_HEADER_FILES})

include(${CMAKE_CURRENT_LIST_DIR}/../llvulkan/llvulkan.cmake)
