# Animation Overrider: origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `443cc2b4ebf9e00da5bb52f00a3e45be790d4a75` (2026-05-25), and the AO changes up to `6f5edd189d` (2026-10-30, Firestorm 7.2.5.81788) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), headers kept in every file taken from it |
| Author | Zi Ree (Animation Overrider, 2011); FIRE-3801 and other fixes by Nicky Dasmijn (ND) |

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| aoset.h/.cpp | indra/newview/aoset.h/.cpp | namespace `plus::ao`; `AOSet` no longer an `LLEventTimer` (the cycle timer is an `LLFrameTimer` the engine's idle reads), so sets can be deleted at any time (no FIRE-3801 "old sets" list); the state table is built on first use; a link's sort order may be any increasing numbers; `resolveAsset()` reads the asset of the original item, not of the link; set names may be any UTF-8 without `:` or `|` (Firestorm: ASCII only) |
| aoengine.h/.cpp | indra/newview/aoengine.h/.cpp | the overrider (`override()`, `enable()`, `cycle()`, sit, mouselook, water, region change) as in Firestorm, with null checks, and below water the swimming state is also used by `cycle()` and `playAnimation()`; the AO's own timers (`AOTimerCollection`, `AOSitCancelTimer`) replaced by the idle; editing functions rewritten: memory first, no `reload()` after a change (see aostore) |
| aostore.h/.cpp | the inventory parts of aoengine.cpp (`tick()`, `update()`, `reloadStateAnimations()`, `addSet()`, `addAnimation()`, `createAnimationLink()`, `removeAnimation()`, `removeSet()`, `purgeFolder()`, `findForeignItems()`, `updateSortOrder()`, `saveSet()`, `saveState()`) | new: requests batched (all new state folders of a set in one, all links of a state folder in one, the sort order in the link's description from the start); the folder is read again only at login, from Reload, or when something else changed it, and then only what differs is taken over; Firestorm's folder layout and names unchanged |
| aotransfer.h/.cpp | `importNotecard()`, `onNotecardLoadComplete()`, `parseNotecard()`, `processImport()` of aoengine.cpp | the notecard read with `LLNotecard` instead of looking for "Text length"; problems collected into one message; the set made with the same batched requests. File export and import (`.charis-ao.xml`) are new |
| aofloater.h/.cpp | indra/newview/ao.h/.cpp (`FloaterAO`) | a plain `LLFloater` (Firestorm: `LLTransientDockableFloater`, docked to the utility bar); no reload cover; sets referred to by serial number; several dropped animations added in one batch; "AO on" check box, Import / Export / Export All buttons and a status line added; state names shown translated |
| ao.h/.cpp | the AO calls in llvoavatar.cpp, llagentcamera.cpp, llappviewer.cpp, llviewerfloaterreg.cpp | registration with the plus foundation; login, mouselook, water and quitting are detected here (no hooks) |
| skins/default/xui/{en,ja}/floater_plus_ao.xml | floater_ao.xml + panel_ao.xml (en, ja) | one file; control names kept; notification texts as floater strings |
| app_settings/settings_plus.xml (ao part) | settings_per_account.xml | `UseAO` -> `PlusAOEnabled`, `UseAOStands` -> `PlusAOStands`, `FSCurrentAOState` -> `PlusAOCurrentState`, `UseFullAOInterface` -> `PlusAOFullInterface`, `floater_rect_animation_overrider_full` -> `PlusAOFullRect`; new `PlusAOLogTiming` |

## Not taken

- The inventory protection (`LockAOFolders` and its hooks in llinventorybridge.cpp,
  llinventoryfunctions.cpp, llinventorymodel.cpp): the #AO folder can be edited
  by hand; the store notices the change and takes it over instead.
- `PauseAO`, the chat bar commands (chatbar_as_cmdline.cpp), the utility bar
  button and its menu, the AO toggle in llfloaterbvhpreview.cpp, RLVa.

The inventory layout is Firestorm's, but below our own folder: `#Charis/#AO/...`
(2026-11-02; before that `#Firestorm/#AO`), so that a user who never used Firestorm
does not get a Firestorm folder. Firestorm's sets are copied over on request: the
"Get from Firestorm" button, or the offer after login while no set exists
(`AOStore::importFromFirestorm()`; links keep their list places; a set whose name we
have is left out; Firestorm's folder is only read).

## Toolbar button

| Here | Firestorm | Changes |
|---|---|---|
| app_settings/commands_plus.xml `plus_ao`, strings_plus.xml `Command_PlusAO_*` | commands.xml `animationoverride` | opens `plus_ao`; no `checkbox_control` (not in the official command manager); also in the default bottom toolbar (app_settings/toolbars.xml, `<plus>` block) |
| skins/default/textures/plus/ao_18.png (`Plus_AO_Icon`) | skins/default/textures/toolbar_icons/animation_override.png | copied unchanged |

## Taken from the 2026 AO update (Firestorm 72dfe6431c ... b0640f15c8)

Authors: PanteraPolnocy (the AO changes of 2026), Zi Ree (ideas). Ported by function, not as a patch
(the engine here keeps its own structure, see above):

| Firestorm | Here |
|---|---|
| Independent tracks per state (`AOTrack`, folder `<name>:TR[:CT][:CY][:RN]`) | `AOTrack` in aoset.h; played by `AOEngine::startStateTracks()` / `refreshTracks()`; their cycle is read in the idle (`tickTracks()`), not by an `LLEventTimer` (`AOTrackTimer`) |
| Animation groups (`AOAnimationStep`, folder `<name>:OR<n>`) | an `AOAnimation` with `mIsGroup` and `mMembers`; the main list stays `AOState::mAnimations` |
| "Always" state (`AOSet::AlwaysPlay`) | `plus::ao::AlwaysPlay`; `startAlwaysAnimations()`, the region may lose them: checked every 3 s (`reassertAlways(true)`) and at a region change (FIRE-36811) |
| Clone a set | `AOEngine::cloneSet()`, the "+" button as a menu |
| Context menu of the list, "find the original", drag and drop (onto an animation: group, onto a group: join, a folder: group, onto a track: add) | `AOFloater::showContextMenu()`, `handleDragAndDrop()` |
| Cycle off: back to the first entry; the playing follows moved or removed entries | `setCycle()`, `syncPlaying()` |
| Positions kept over a reload; cold inventory cache | already so here (`replaceAnimations()` keeps the place by link; the loading waits until the folders are complete, now four levels deep) |

Not taken: the "drop zone" overlay while dragging; FS's rename-group dialog is a notification of our own
(`PlusAORenameGroup`). New: groups, tracks and the Always state in the `.charis-ao.xml` file (version 2;
a version 1 file is read as before).
