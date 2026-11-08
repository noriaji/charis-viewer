# Group titles (Auto Region): origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `443cc2b4ebf9e00da5bb52f00a3e45be790d4a75` (2026-05-25, master; the files last changed in `2d2791d1b5`, 2026-04-06) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`); the Firestorm copyright lines are kept in every file taken from it |
| Authors | Ansariel Hiller (FSFloaterGroupTitles, 2012); The Phoenix Firestorm Project, Inc. (FSGroupTitleRegionMgr, "FS:PP", 2026) |

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| grouptitles.h/.cpp | indra/newview/fsgrouptitleregionmgr.h/.cpp (`FSGroupTitleRegionMgr`) | namespace `plus::grouptitles`, free functions over one state (no singleton); the file's name and format kept (`region_group_titles.xml` per account, a copied file works in both viewers); what is read is checked (names sanitized as typed ones, keys that are no name dropped, at most 1000, a role without a group none, the XML's nesting checked first; a file that does not parse is kept as `.bad`, not overwritten); saved through a temporary file; names cut at a UTF-8 character, not inside one; the timers are idle callbacks with a generation (no `LLEventTimer` to delete); a region whose name is not known yet is looked at again every 2 s (15 times at most) besides Firestorm's one retry 5 s after the file is read; as in Firestorm, Auto Region sends no title the cached titles have selected already (the window's button always sends); titles given to regions of groups the agent is not in get a row of their own in the window, so they can be cleared; the stale temporary files of a crashed save are removed (plus::removeStaleTemps()); the activation shared with the window (`activateTitle()`); the file is read when the login reaches `STATE_INVENTORY_SEND2` (the "StartupState" event pump, no hook in llstartup.cpp); RLVa's `canChangeActiveGroup()` is `plus::rlv::canChangeActiveGroup()` (@setgroup is not in Charis yet: true until it comes) |
| grouptitlesfloater.cpp | indra/newview/fsfloatergrouptitles.h/.cpp (`FSFloaterGroupTitles`) | an `LLFloater` in the feature's namespace; the list is built again from the group manager's titles on the next idle (once per frame whatever came in) instead of row by row per reply, so the selection is kept and the official "new group" event is enough (Firestorm added "update grouptitle list" to llagent.cpp); the filter is done here (title or group name; the official list has no filter) |
| grouptitles.cmake | - | new (plus foundation) |
| skins/default/xui/{en,ja}/floater_plus_group_titles.xml | floater_fs_group_titles.xml | the psui look (charis-ui-style): the same names, buttons and strings; the official list and filter in glass boxes |
| skins/default/textures/plus/group_titles_16.png (`Plus_GroupTitles_Icon`) | skins/default/textures/toolbar_icons/group_titles.png | copied unchanged |
| app_settings/commands_plus.xml `plus_group_titles` | app_settings/commands.xml `group_titles` | the same command (Floater.Toggle) |
| notifications.xml (en, ja) `PlusGroupTitleRegion`, `PlusGroupTitleRegionNotFound` | `FSSetTitleRegion`, `FSSetTitleRegionNotFound` | new names |
| panel_people.xml (en, ja) `plus_group_titles_btn` | panel_fs_contacts_groups.xml `titles_btn` (FSFloaterContacts::onGroupTitlesButtonClicked) | Charis has the official People window, not Firestorm's contacts window: an icon button in its Groups tab, beside the gear |

## Not taken

- The settings backup's row for the file (panel_preferences_backup.xml `restore_per_account_row_regiongrouptitles`):
  Charis has no settings backup.
- The help topic `fs_group_titles`.
