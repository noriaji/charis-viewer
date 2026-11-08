# -*- cmake -*-
# Charis Viewer apart from the official viewer: own user folders (with a
# one-time copy of the official settings), cache, window class, taskbar id and
# channel (research/114-independence.md in the sl-viewer repository).
# Included by plus/plus.cmake. Hooks: llappviewer.cpp/.h, llappviewerwin32.cpp,
# llvelopack.cpp, lldir.cpp, lldir_win32.cpp (plus/plus-hooks.sh lists them).

list(APPEND plus_SOURCE_FILES
    plus/independence/independence.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/independence/independence.h
    )
