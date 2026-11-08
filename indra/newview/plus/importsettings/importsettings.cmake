# -*- cmake -*-
# Import settings: brings the settings of another viewer on this computer over
# (the official viewer, Firestorm and its kin, other Charis Viewer installs),
# offered at the first start and from Preferences > Setup (research/143).
# Included by plus/plus.cmake. Official file changed: panel_preferences_setup.xml (en),
# the button (panel_plus_import_settings.xml).

list(APPEND plus_SOURCE_FILES
    plus/importsettings/importer.cpp
    plus/importsettings/importsettings.cpp
    plus/importsettings/sources.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/importsettings/importsettings.h
    )
