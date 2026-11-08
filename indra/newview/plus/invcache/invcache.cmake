# -*- cmake -*-
# Inventory cache (plus/invcache, from Firestorm's "Clear Inventory Cache"): a button in
# Preferences > Advanced deletes the account's inventory cache at its next login, so the
# whole inventory is loaded from the server again (research/144). Included by
# plus/plus.cmake. Official file changed: panel_preferences_advanced.xml (en), the button
# (panel_plus_inv_cache.xml).

list(APPEND plus_SOURCE_FILES
    plus/invcache/invcache.cpp
    )
