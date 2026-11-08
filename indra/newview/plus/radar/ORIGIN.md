# Radar: origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `443cc2b4ebf9e00da5bb52f00a3e45be790d4a75` (2026-05-25, master) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`); the Firestorm copyright lines are kept in every file taken from it |
| Authors | Ansariel Hiller (FSRadar, FSRadarEntry, FSPanelRadar, FSFloaterRadar, FSRadarMenu, 2013), Arrehn Oberlander (FSRadarListCtrl, 2011) |

The code was rewritten for this viewer as a window of its own (Firestorm shows its
radar in the People floater and in a window of its own, `fs_radar`, and calls it
from about ten official files; here no official file is changed), following
Firestorm's behaviour; the parts noted below are Firestorm's.

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| radar.h/.cpp | indra/newview/fsradar.h/.cpp (`FSRadar::updateRadarList()`, the reports), fsradarentry.h/.cpp (`FSRadarEntry`: name, age, payment info) | namespace `plus::radar`; `Engine` knows nothing of the window; started from the plus foundation at login, stopped by `LLSingleton`; names follow `UseDisplayNames` and `PlusRadarNameFormat` itself; profile data through its own `LLAvatarPropertiesObserver`; no report in the first sweep after login or a region change (Firestorm reports everyone around as entering); chat and shout ranges fixed at 20 m and 100 m (Firestorm reads the simulator's); reports go to nearby chat only |
| radarfloater.cpp | fspanelradar.cpp, fsfloaterradar.cpp (the list and its columns), fsradarmenu.cpp (`FSRadarMenu`), fsradarlistctrl.cpp (the list's right-click) | one plain `LLFloater` (list and settings tabs); the list is updated by difference (selection and scroll kept); distance, age and time seen sort as numbers; `zoomTo()` does what Firestorm's `LLAvatarActions::zoomIn()` does (the official LLAvatarActions has none) |
| radar.cmake | - | new (plus foundation) |
| skins/default/xui/{en,ja}/floater_plus_radar.xml | - | new layout (floater_fs_radar.xml / panel_fs_radar.xml were not used) |
| skins/default/xui/{en,ja}/menu_plus_radar.xml | menu_fs_radar.xml, menu_fs_radar_multiselect.xml | one menu for one or several avatars; most labels kept |
| app_settings/settings_plus.xml (radar part) | settings.xml (`FSRadar*`, `RadarReport*`) | new names `PlusRadar*` |
| skins/default/xui/{en,ja}/strings_plus.xml (`PlusRadar*`) | the radar report strings of strings.xml | rewritten (the name is a link, the distance is added) |

## Not taken

- RLVa's @shownearby (not in this viewer's RLVa yet). @shownames is taken (plus/rlv,
  research/248 3.4): anonymized names, no age, no bold for friends, no menu on the list;
  an @shownames exception is shown by name (Firestorm's radar hides every name).
- The LSL bridge (heights above 1020 m, alerts to scripts), contact sets,
  the asset blacklist (derender), voice levels, profile notes, age alerts.
- Report sounds (`UISndRadar*` do not exist here) and report toasts (nearby chat only).
- The minimap inside the window and the marks on the minimap.
- Estate and parcel actions on avatars (eject, freeze, kick, teleport home, ban):
  the official viewer has no callable functions for them.
