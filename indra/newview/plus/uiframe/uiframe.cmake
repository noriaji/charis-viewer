# -*- cmake -*-
# UI frames: while the user works the UI over a slow world, most frames draw
# only the UI over the last world image (research/86-ui-thread.md E1 in the
# sl-viewer repository). Included by plus/plus.cmake. Called from the <plus>
# hooks in LLAppViewer::doFrame() (llappviewer.cpp) and render_ui()
# (llviewerdisplay.cpp).

list(APPEND plus_SOURCE_FILES
    plus/uiframe/uiframe.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/uiframe/uiframe.h
    )
