# -*- cmake -*-
# A settings change's shader reload: made in the background while the frames
# go on with the shaders in use, with a notice at the bottom of the world
# (research/250), or on several threads with the old buffers freed between its
# passes; the official mirror workaround's second reload skipped with Vulkan
# (fix-round107). Included by plus/plus.cmake. Called from the <plus> hooks in
# LLViewerShaderMgr::setShaders() and LLHeroProbeManager::update().

list(APPEND plus_SOURCE_FILES
    plus/shaderreload/reloadnotice.cpp
    plus/shaderreload/shaderreload.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/shaderreload/reloadnotice.h
    plus/shaderreload/shaderreload.h
    )
