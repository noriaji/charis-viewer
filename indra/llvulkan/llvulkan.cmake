# -*- cmake -*-
# Vulkan backend, compiled into llrender alongside llrhi. Only built when the
# Vulkan SDK is present (VULKAN_SDK at configure time); otherwise LL_VULKAN=0
# and the viewer is plain GL.

if (DEFINED ENV{VULKAN_SDK} AND WINDOWS)
  file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" LL_VULKAN_SDK)
  message(STATUS "llvulkan: using Vulkan SDK at ${LL_VULKAN_SDK}")

  set(llvulkan_SOURCE_FILES
      ${CMAKE_CURRENT_LIST_DIR}/llvkattribpool.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkavatar.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkbenchmark.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkbindless.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkdevice.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkdrawlist.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkfullscreen.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_buffer.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_context.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_descriptors.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_draw.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_entry.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_glsl.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_glslbindless.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_glquery.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_hooks.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_linkbuild.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_oitshaders.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_occlusion.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_oit.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_packet.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_pipeline.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_pipeprewarm.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_program.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_readback.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_runs.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_scene.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_shaderobj.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_texcompact.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_texconvert.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_texref.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_texture.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkglare.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgpudriven.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgpulist.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkgtao.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkimmediate.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvklook.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkmirror.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvknative.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkpass.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkpassstate.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkqueues.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkatmosphere.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkrain.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkrecorder.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkrender.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkresource.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkscene.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkrtas.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkscenetable.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkshader.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkskinfill.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkskinsss.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkssgi.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkssr.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkswapchain.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvktexcompress.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvktaa.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvktracker.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkupscale.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkvolumetric.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkwetlook.cpp
      ${CMAKE_CURRENT_LIST_DIR}/llvkwindows.cpp
      )

  set(llvulkan_HEADER_FILES
      ${CMAKE_CURRENT_LIST_DIR}/llvkattribpool.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkavatar.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkbenchmark.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkbindless.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkdevice.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkdispatch.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkdrawlist.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkfullscreen.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkframetable.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_drawpath.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkgl_internal.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkglare.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkgpudriven.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkgpulist.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkgtao.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkimmediate.h
      ${CMAKE_CURRENT_LIST_DIR}/llvklook.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkmirror.h
      ${CMAKE_CURRENT_LIST_DIR}/llvknative.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkpass.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkpassstate.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkqueues.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkatmosphere.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkrain.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkrecorder.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkrender.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkresource.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkscene.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkrtas.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkscenetable.h
      ${CMAKE_CURRENT_LIST_DIR}/scene_cull.comp
      ${CMAKE_CURRENT_LIST_DIR}/llvktaa.h
      ${CMAKE_CURRENT_LIST_DIR}/llvktracker.h
      ${CMAKE_CURRENT_LIST_DIR}/llvktranslate.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkupscale.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkshader.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkskinfill.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkskinsss.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkssgi.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkssr.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkswapchain.h
      ${CMAKE_CURRENT_LIST_DIR}/llvktexcompress.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkbc7.glsl.inl
      ${CMAKE_CURRENT_LIST_DIR}/llvkvolumetric.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkwetlook.h
      ${CMAKE_CURRENT_LIST_DIR}/llvkwindows.h
      ${CMAKE_CURRENT_LIST_DIR}/thirdparty/vk_mem_alloc.h
      )

  target_sources(llrender PRIVATE ${llvulkan_SOURCE_FILES} ${llvulkan_HEADER_FILES})

  # research/197 stage 2: the retained scene's compute shader. scene_cull.comp
  # is written out whole as a C++ raw string (llvkscene.cpp builds it), and
  # the parts between its "scene shared" markers as C++ (llvkscenetable.cpp:
  # the CPU's run of the same functions, for verify). tools/selftest's
  # gen_scene.py cuts the same parts for sim-scene. Written at configure time
  # and again whenever scene_cull.comp changes (only when the text differs).
  set(llvulkan_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/llvulkan_generated)
  set(llvulkan_SCENE_COMP ${CMAKE_CURRENT_LIST_DIR}/scene_cull.comp)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${llvulkan_SCENE_COMP})
  set_source_files_properties(${llvulkan_SCENE_COMP} PROPERTIES HEADER_FILE_ONLY ON)
  file(READ ${llvulkan_SCENE_COMP} llvulkan_scene_glsl)
  string(REPLACE "\r" "" llvulkan_scene_glsl "${llvulkan_scene_glsl}")
  string(LENGTH "${llvulkan_scene_glsl}" llvulkan_scene_length)
  if (llvulkan_scene_length GREATER 16000)
    message(FATAL_ERROR "scene_cull.comp is longer than one MSVC string literal holds: split the raw string in llvulkan.cmake")
  endif ()
  function(llvulkan_write_if_changed path content)
    if (EXISTS "${path}")
      file(READ "${path}" old_content)
      string(REPLACE "\r" "" old_content "${old_content}")    # file(WRITE) ends lines with CR LF on Windows
      if (old_content STREQUAL content)
        return()
      endif ()
    endif ()
    file(WRITE "${path}" "${content}")
  endfunction()
  function(llvulkan_scene_section name begin_marker end_marker)
    string(FIND "${llvulkan_scene_glsl}" "\n${begin_marker}\n" begin_at)
    string(FIND "${llvulkan_scene_glsl}" "\n${end_marker}\n" end_at)
    if (begin_at EQUAL -1 OR end_at EQUAL -1 OR NOT end_at GREATER begin_at)
      message(FATAL_ERROR "scene_cull.comp: the marker lines '${begin_marker}' / '${end_marker}' are missing")
    endif ()
    string(LENGTH "\n${begin_marker}\n" marker_length)
    math(EXPR first "${begin_at} + ${marker_length}")
    math(EXPR length "${end_at} + 1 - ${first}")
    string(SUBSTRING "${llvulkan_scene_glsl}" ${first} ${length} section)
    llvulkan_write_if_changed("${llvulkan_GENERATED_DIR}/${name}" "${section}")
  endfunction()
  llvulkan_scene_section(llvkscene_cull_types.inl "// ==== scene shared types: begin ====" "// ==== scene shared types: end ====")
  llvulkan_scene_section(llvkscene_cull_funcs.inl "// ==== scene shared functions: begin ====" "// ==== scene shared functions: end ====")
  llvulkan_write_if_changed("${llvulkan_GENERATED_DIR}/llvkscene_cull_glsl.inl" "R\"GLSL(${llvulkan_scene_glsl})GLSL\"\n")
  target_include_directories(llrender PRIVATE ${llvulkan_GENERATED_DIR})
  target_include_directories(llrender PUBLIC
      ${CMAKE_CURRENT_LIST_DIR}
      ${CMAKE_CURRENT_LIST_DIR}/thirdparty
      ${LL_VULKAN_SDK}/Include
      )
  target_compile_definitions(llrender PUBLIC LL_VULKAN=1)

  # glslang (GLSL -> SPIR-V + reflection) from the SDK's static libraries.
  set(LL_VULKAN_LIBS
      vulkan-1
      glslang
      glslang-default-resource-limits
      MachineIndependent
      GenericCodeGen
      OSDependent
      SPIRV
      SPIRV-Tools-opt
      SPIRV-Tools
      )
  foreach (lib ${LL_VULKAN_LIBS})
    target_link_libraries(llrender ${LL_VULKAN_SDK}/Lib/${lib}.lib)
  endforeach ()

  # research/82: vulkan-1.dll is installed by the GPU driver. Delay loaded, so
  # the viewer still starts on a PC without a Vulkan driver (and stays on
  # OpenGL); LLVKDevice::createInstance() loads it before the first vk call.
  # INTERFACE: the option belongs on the executable's link (llrender is static).
  target_link_libraries(llrender delayimp)
  target_link_options(llrender INTERFACE "/DELAYLOAD:vulkan-1.dll")

  source_group("llvulkan" FILES ${llvulkan_SOURCE_FILES} ${llvulkan_HEADER_FILES})

  # research/91 8.4: the split llvkgl*.cpp files (and llvktracker.cpp, llvkdrawlist.cpp, llvkimmediate.cpp, llvkpass.cpp, llvkresource.cpp) read
  # each other's thread_local GL context (sState, sCurrentProgram, the units)
  # on every draw, and MSVC puts an on-demand initialization check before each
  # read of another file's thread_local. llrender is linked into the
  # executable, so the CRT's TLS callback initializes them as every thread
  # starts: the check is never needed. The shared precompiled header
  # (llprecompiled) is made without the option and a file must match it, so
  # these files are built without the PCH (as llvkdevice.cpp).
  set(llvulkan_GL_SOURCE_FILES ${llvulkan_SOURCE_FILES})
  list(FILTER llvulkan_GL_SOURCE_FILES INCLUDE REGEX "/(llvkgl(_[a-z0-9]+)?|llvktracker|llvkdrawlist|llvkimmediate|llvkpass|llvkresource)[.]cpp$")
  set_source_files_properties(${llvulkan_GL_SOURCE_FILES} PROPERTIES
      COMPILE_OPTIONS "/Zc:tlsGuards-"
      SKIP_PRECOMPILE_HEADERS ON)

  # The single-header VMA implementation is not built with the PCH.
  set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/llvkdevice.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
else ()
  message(STATUS "llvulkan: VULKAN_SDK not set, building without the Vulkan backend")
  target_compile_definitions(llrender PUBLIC LL_VULKAN=0)
endif ()
