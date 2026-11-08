# Area Search: origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `443cc2b4ebf9e00da5bb52f00a3e45be790d4a75` (2026-05-25, master) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`); the Firestorm copyright lines are kept in every file taken from it |
| Authors | Techwolf Lupindo (FSAreaSearch, 2012; later changes by Cron, Beq Janus, minerjr and Neremyn), Ansariel Hiller (the list's menu, fsareasearchmenu, 2014) |

The code was rewritten for this viewer (the search and the window are separate
classes, no official file carries the message handler), following Firestorm's
behaviour; the parts noted below are Firestorm's.

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| areasearch.h/.cpp | the search part of indra/newview/fsareasearch.h/.cpp (`FSAreaSearch`: the object pass, the property requests and their answers, the filters) | namespace `plus::areasearch`; the search (`Engine`) knows nothing of the window; ObjectProperties is taken by swapping the message handler while the window is open (Firestorm changes llviewermessage.cpp and llselectmgr.cpp); the interest list is set and put back here (Firestorm changes llagent.cpp); land impact is re-read in the once-a-second pass (Firestorm is told by llviewerobjectlist.cpp); requests per region capped at 762 and sent again after 30 s as in Firestorm |
| areasearchfloater.cpp | the window part of fsareasearch.cpp (`FSAreaSearch` as a floater, `FSPanelAreaSearchList`, `...Find`, `...Filter`, `...Options`) and fsareasearchmenu.cpp (`FSAreaSearchMenu`); the list's right-click as Firestorm's `FSScrollListCtrl` does it | one plain `LLFloater` with four tabs; the list is made as the window opens; a quick filter above the list; a double click can zoom the camera in; number columns sort as numbers; `zoomTo()` kept as in Firestorm |
| areasearch.cmake | - | new (plus foundation) |
| skins/default/xui/{en,ja}/floater_plus_area_search.xml | floater_fs_area_search.xml and its panels | one file; most control names kept; quick filter field added |
| skins/default/xui/{en,ja}/menu_plus_area_search.xml | menu_fs_area_search.xml | names kept where the action is the same; Copy row, Select all, Show mine only added |
| app_settings/settings_plus.xml (area search part) | settings.xml (`FSAreaSearch*`) | new names `PlusAreaSearchColumns`, `PlusAreaSearchFilters`, `PlusAreaSearchDoubleClick` |

The one official hook (`LLViewerRegion::killInvisibleObjects()`, objects out of
view are kept while the window is open, as Firestorm's FIRE-32668) is marked
`<plus>` (plus/plus-hooks.sh lists the hooks).

RLVa (plus/rlv, research/248 3.4): @shownames anonymizes the owner and the last
owner as Firestorm's `RLVa_hideNameIfRestricted()` does (and a creator who is the
owner, which Firestorm shows); the list is made again when it is switched. A group's
name is not anonymized (Firestorm anonymizes whatever stands in the owner column).
Teleport and sit go through the official paths that check RLVa (llagent.cpp; the
sit of `sitOn()` is rlv-sit's).

## Not taken

- RLVa's interaction checks of the list's menu (`FSAreaSearch::onContextMenuItemEnable()`:
  touch, edit, buy ... under @touch, @edit, @interact and the like): those commands come
  with stage 3 of research/248.
- The script info of the LSL bridge and the asset blacklist (neither exists here).
