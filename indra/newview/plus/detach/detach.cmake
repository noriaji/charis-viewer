# -*- cmake -*-
# Detached floaters: a floater in a window of its own on any monitor
# (research/87-detached-floaters.md in the sl-viewer repository, steps S0, S1).
# Included by plus/plus.cmake. Called from the <plus> hooks in render_ui()
# (llviewerdisplay.cpp) and LLViewerWindow (llviewerwindow.cpp); the hooks in
# llwindowwin32.cpp, llfloater.cpp and llui.cpp are set from here. The windows are
# presented by the Vulkan backend (llvulkan/llvkwindows.cpp).

list(APPEND plus_SOURCE_FILES
    plus/detach/detach.cpp
    plus/detach/detachplaces.cpp
    plus/detach/detachview.cpp
    plus/detach/detachwin32.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/detach/detach.h
    plus/detach/detachplaces.h
    plus/detach/detachview.h
    plus/detach/detachwin32.h
    )
