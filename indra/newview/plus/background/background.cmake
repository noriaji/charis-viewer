# -*- cmake -*-
# A window that lost focus but is still on screen keeps working as in front (plus/background,
# PlusBackgroundKeepVisible; research/303). Included by plus/plus.cmake. Called from <plus> hooks in
# LLAppViewer::doFrame() (the yield), LLViewerTexture::updateClass() (the discard bias) and
# LLViewerMedia (the priority of media).

list(APPEND plus_SOURCE_FILES
    plus/background/background.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/background/background.h
    )
