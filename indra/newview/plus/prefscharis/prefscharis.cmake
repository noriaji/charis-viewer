# -*- cmake -*-
# Preferences > Charis (plus/prefscharis, research/263 in the sl-viewer repository): the panel class of
# panel_preferences_charis.xml (plus_panel_preference_charis, an LLPanelPreference). It shows the page of the
# sub-tab picked (PlusPrefsCharisTab) and reads the state of the few items driven by the features' registered
# functions (AO, safe mode next time, FPS position, DGC, the comparison in use); the rest are control_name and
# commit_callback in the XML. Included by plus/plus.cmake. The class name is set in
# floater_preferences.xml's Charis tab (inside its <!-- plus --> mark).

list(APPEND plus_SOURCE_FILES
    plus/prefscharis/prefscharis.cpp
    )
