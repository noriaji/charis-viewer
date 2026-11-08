# -*- cmake -*-
# Charis's own place for bug reports and release notes instead of Linden Lab's
# (research/123 H1, M3). Included by plus/plus.cmake.
# Hooks: handle_report_bug() (llviewermenu.cpp), getViewerInfo() and
# getViewerInfoString() (llappviewer.cpp), the "Report Bug" items of
# menu_viewer.xml and menu_login.xml (en).

list(APPEND plus_SOURCE_FILES
    plus/support/support.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/support/support.h
    )
