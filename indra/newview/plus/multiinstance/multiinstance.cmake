# -*- cmake -*-
# Charis viewers of one user on one GPU make room for the one in front (plus/multiinstance, research/337 stage 1-2):
# a 4 KB shared registry of numbers, a frame rate cap and a lower priority for a viewer while another is in front
# (PlusMultiBackgroundFps, PlusMultiBackgroundPriority), a share of the VRAM budget (PlusMultiShareVRAM, off by
# default); LLRHI_MULTI_INSTANCE=0 makes no registry. Included by plus/plus.cmake. Called from <plus> hooks in
# LLAppViewer::init(), doFrame() and cleanup() (llappviewer.cpp) and LLViewerTexture::updateClass()
# (llviewertexture.cpp); plus/fpslimit takes its cap.

list(APPEND plus_SOURCE_FILES
    plus/multiinstance/multiinstance.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/multiinstance/multiinstance.h
    )
