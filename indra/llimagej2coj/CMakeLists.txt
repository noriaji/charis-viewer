# -*- cmake -*-

project(llimagej2coj)

include(00-Common)
include(LLCommon)
include(LLImage)
include(OpenJPEG)

set(llimagej2coj_SOURCE_FILES
    llimagej2coj.cpp
    )

set(llimagej2coj_HEADER_FILES
    CMakeLists.txt

    llimagej2coj.h
    )

# <plus> slj2k: the Charis JPEG 2000 decoder tried before OpenJPEG (slj2k/, research/129 in the
# sl-viewer repository). It needs the exact floating point of OpenJPEG (/fp:precise) and skips the
# shared PCH. Only the vector kernels get /arch:AVX2; the entry, the CPU check, parsing and the
# thread pool are plain x64, so CPUs without AVX2 never run an AVX2 instruction (rules in
# slj2k/slj2k_avx2.h).
set(llimagej2coj_slj2k_SOURCE_FILES
    slj2k/plus_j2c.cpp
    slj2k/slj2k.cpp
    slj2k/slj2k_codestream.cpp
    )
set(llimagej2coj_slj2k_AVX2_SOURCE_FILES
    slj2k/slj2k_dwt.cpp
    slj2k/slj2k_output.cpp
    slj2k/slj2k_t1.cpp
    )
set(llimagej2coj_slj2k_HEADER_FILES
    slj2k/plus_j2c.h
    slj2k/slj2k.h
    slj2k/slj2k_avx2.h
    slj2k/slj2k_codestream.h
    slj2k/slj2k_internal.h
    slj2k/threadpool.h
    )
if (WINDOWS)
  set_source_files_properties(${llimagej2coj_slj2k_SOURCE_FILES}
      PROPERTIES COMPILE_OPTIONS "/fp:precise" SKIP_PRECOMPILE_HEADERS ON)
  set_source_files_properties(${llimagej2coj_slj2k_AVX2_SOURCE_FILES}
      PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/fp:precise" SKIP_PRECOMPILE_HEADERS ON)
endif (WINDOWS)
list(APPEND llimagej2coj_SOURCE_FILES ${llimagej2coj_slj2k_SOURCE_FILES} ${llimagej2coj_slj2k_AVX2_SOURCE_FILES}
     ${llimagej2coj_slj2k_HEADER_FILES})
# </plus>

list(APPEND llimagej2coj_SOURCE_FILES ${llimagej2coj_HEADER_FILES})

add_library (llimagej2coj ${llimagej2coj_SOURCE_FILES})

target_link_libraries( llimagej2coj
        llcommon
        llimage
        ll::openjpeg
    )

if (USE_PRECOMPILED_HEADERS)
  target_precompile_headers(llimagej2coj REUSE_FROM llprecompiled)
endif ()
