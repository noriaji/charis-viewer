# -*- cmake -*-
# "Show UI" button (plus/uishowbtn): with the UI hidden (Ctrl+Shift+U or Ctrl+Alt+F1), the mouse at
# the top edge of the window brings down a button that shows it again. Included by plus/plus.cmake.
# Drawn and clicked from two <plus> hooks (llviewerdisplay.cpp, llviewerwindow.cpp), not a view: with
# Ctrl+Alt+F1 the root view is not drawn and takes no click at all. Setting: PlusUiShowButton.

list(APPEND plus_SOURCE_FILES
    plus/uishowbtn/uishowbtn.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/uishowbtn/uishowbtn.h
    )
