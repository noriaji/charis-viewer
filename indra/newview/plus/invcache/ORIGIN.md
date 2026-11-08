# Inventory cache: origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `443cc2b4ebf9e00da5bb52f00a3e45be790d4a75` (2026-05-25, master) |
| License | GNU LGPL 2.1 only (the Firestorm parts are additions to Linden Lab files under `$LicenseInfo:...license=viewerlgpl$`) |
| Authors | Ansariel Hiller (the button, `FS:Ansariel`), Zi Ree (the marker file, `FS:Zi`, FIRE-22353, 872c0bff62, 2019-06-18) |

The code was rewritten for this viewer, following Firestorm's behaviour.

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| invcache.cpp `onClick` | llfloaterpreference.cpp `LLFloaterPreference::onClickInventoryClearCache()`, `callback_clear_inventory_cache()` | namespace `plus::invcache`; a commit callback of its own (`Plus.InvCache.Clear`) instead of a member of the official floater; before login the button says to log in first (Firestorm disables it until login, in `LLFloaterPreference::setPersonalInfo()`); the message after it is our own text (Firestorm shows `CacheWillClear`, which speaks of a restart) |
| invcache.cpp `onStartupState` | llinventorymodel.cpp, the start of `LLInventoryModel::loadSkeleton()` | done on entering `STATE_SEED_CAP_GRANTED` instead: this viewer starts reading the caches on worker threads (`loadInventoryCacheAsync()`) before `loadSkeleton()`, the agent's in that state; the library's owner is read from the login response, since `setLibraryOwnerID()` comes later |
| invcache.cmake | - | new (plus foundation) |
| skins/default/xui/{en,ja}/panel_plus_inv_cache.xml | the `ClearInventoryCache` button of panel_preferences_setup.xml | in Preferences > Advanced beside "Remembered Usernames" (Firestorm: Preferences > Network & Files > Directories) |
| skins/default/xui/{en,ja}/strings_plus.xml (`PlusInvCache*`) | notifications.xml `ConfirmClearInventoryCache` | our own texts, through `GenericAlertYesCancel` / `GenericAlert` |

The marker file name (`<agent id>_DELETE_INV_GZ` in the cache folder) is Firestorm's, so a
mark made in Firestorm on the same cache folder is honoured here and the other way round.

## Not taken

- The web browser cache button next to it in Firestorm (`ConfirmClearWebBrowserCache`).
