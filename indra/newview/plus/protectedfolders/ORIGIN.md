# Protected folders: origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `443cc2b4ebf9e00da5bb52f00a3e45be790d4a75` (2026-05-25, master) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`); the Firestorm copyright line is kept in every file taken from it |
| Author | Ansariel Hiller ("Protect folder": FSFloaterProtectedFolders and the checks in the inventory, 2020) |

The code was rewritten for this viewer, following Firestorm's behaviour; the parts
noted below are Firestorm's.

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| protectedfolders.h/.cpp | the "Protect folder" parts (FIRE-29342) of indra/newview/llinventorybridge.cpp (`LLFolderBridge::isItemMovable()`, `performAction()` "protect" / "unprotect", the folder menu), llinventoryfunctions.cpp (`get_is_category_removable()`) and llinventorymodel.h/.cpp (`mProtectedCategories`) | namespace `plus::protectedfolders`; the list is a set here, not a member of `LLInventoryModel` (official header untouched); deleting is refused in `get_is_category_removable()` as in Firestorm; moving is refused in `LLInventoryModel::changeCategoryParent()` (Firestorm refuses only the drag, in `isItemMovable()`), so moves from the gallery, cut and paste and the menu are covered too; a folder that holds a protected folder cannot be deleted either |
| protectedfoldersfloater.cpp | fsfloaterprotectedfolders.cpp (`FSFloaterProtectedFolders`) | namespace `plus::protectedfolders`; the name filter done here (the official scroll list has no `setFilterString()`); no RLVa check, no Ctrl+F key combo |
| protectedfolders.cmake | - | new (plus foundation) |
| skins/default/xui/{en,ja}/floater_plus_protected_folders.xml | floater_fs_protectedfolders.xml | control names kept |
| skins/default/xui/{en,ja}/menu_inventory.xml (two items) | menu_inventory.xml (Protect / Unprotect) | marked `<plus>` |
| app_settings/settings_plus.xml (`PlusProtectedFolders`) | settings_per_account.xml (`FSProtectedFolders`) | new name (a Firestorm list is not read) |

The official hooks (llinventoryfunctions.cpp, llinventorymodel.cpp,
llinventorybridge.cpp) are marked `<plus>` (plus/plus-hooks.sh lists the hooks).

## Not taken

- The `ProtectedFolder` string and the Protected Folders items of the View menu and
  the inventory gear menu (one item in the Charis menu instead).
- RLVa.
