# -*- cmake -*-
# Menu shortcut bar (plus/shortcutbar, research/225): a row under the navigation bar with text-only
# buttons that run items of the menu bar; items are added with a right click on the menu item.
# Included by plus/plus.cmake. Official files: main_view.xml (the row), llmenugl.cpp/.h (one hook).
# Setting: PlusShortcutBarShow. The list is plus_shortcut_bar.xml in the user settings folder.

list(APPEND plus_SOURCE_FILES
    plus/shortcutbar/shortcutbar.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/shortcutbar/shortcutbar.h
    )
