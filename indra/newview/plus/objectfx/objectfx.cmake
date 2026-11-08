# -*- cmake -*-
# Object appear / disappear effects, the UI (plus/objectfx): the Preferences > Charis button that opens
# Advanced Graphics on the group, and the callbacks of the group's Color3 colour swatch. Included by
# plus/plus.cmake. No official file is changed: the group is in floater_preferences_graphics_advanced.xml
# (Charis tab), the menu item in menu_plus.xml; the settings belong to the effects (plus/materialize,
# plus/derez).

list(APPEND plus_SOURCE_FILES
    plus/objectfx/objectfx.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/objectfx/objectfx_palette.h
    )
