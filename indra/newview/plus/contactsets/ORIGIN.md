# Contact sets: origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `443cc2b4ebf9e00da5bb52f00a3e45be790d4a75` (2026-05-25, master) |
| License | lggcontactsets.*: GNU LGPL 2.1 or later (Greg Hendrickson's notice, kept). fsfloatercontactsetconfiguration.*, fsfloateraddtocontactset.*: Cinder Roxley's permissive license (the Boost-style notice, kept whole in each file). contactsets.* and the window files: this viewer's (LGPL 2.1 only) |
| Authors | Greg Hendrickson (LordGregGreg Back, LGGContactSets, 2011), Cinder Roxley (the two windows and the signals, 2013), Ansariel Hiller (the later Firestorm changes) |
| Design | research/333 (this branch: cs-core; the People tab is cs-people, the colours in the lists, tags, chat and map are cs-color) |

Firestorm's class, functions, enum and file are kept (`LGGContactSets`, `settings_friends_groups.xml` with its
keys `All Sets` / `extraAvs` / `Pseudonyms` / `globalSettings`), so a Firestorm user's contact sets are read as
they are and the file can go back to Firestorm. The windows keep Firestorm's registered names
(`fs_add_contact`, `fs_contact_set_config`) and their widgets' names.

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| lggcontactsets.h | indra/newview/lggcontactsets.h | the same public functions (the parallel branches use them as they are), but those nothing calls here (below). Added: `CS_FLOATER_ADD` / `CS_FLOATER_CONFIG`, `cleanupSingleton()`; `setDefaultColor()` now saves and signals; private: `clearAll()`, `markChanged()`, `signalWhenNameKnown()`, `disconnectNameRequests()`, the colour cache; the sets' map finds a `string_view` without a copy (`std::less<>`) |
| lggcontactsets.cpp | indra/newview/lggcontactsets.cpp | see below |
| fsfloatercontactsetconfiguration.h/.cpp | indra/newview/fsfloatercontactsetconfiguration.h/.cpp | closes when its set is removed or renamed elsewhere, or is not a set (on the next idle); Enter in the name renames too (the name as it is: nothing); the rename takes the name as it was kept; the names in RenameContactSetFailure go through `plus::safeText()`; the cone's constants are the official defaults |
| fsfloateraddtocontactset.h/.cpp | indra/newview/fsfloateraddtocontactset.h/.cpp | the set is the pulldown's value (not its words: "no sets" was added as a set); null ids left out; the set's name in the notices through `plus::safeText()`; the list is refilled only for `UPDATED_LISTS` (Firestorm's `if (type)`, the same, written out) and keeps the chosen set |
| contactsets.h/.cpp | - | new: the login (`onLogin()`), the display name's change, the file's reading (`readFile()`, `fileContent()` for plus/importsettings), `reloadAfterImport()`, the windows (`showAddToSet` / `showMoveToSet` / `showSetConfig`: Firestorm's `LLAvatarActions::addToContactSet` / `moveToContactSet`, which this viewer's official `LLAvatarActions` has not) and the questions (`askAddSet` / `askRemoveSet` / `askRemoveFromSet` / `askSetPseudonym`: Firestorm's `LLPanelPeople` code); the feature registration |
| contactsets.cmake | - | new (plus foundation) |
| skins/default/xui/{en,ja}/floater_plus_contact_set_configuration.xml | floater_fs_contact_set_configuration.xml | the photo screen's look (psui compatible widgets, so the C++ keeps Firestorm's types); two columns, 800 x 588; the colour pickers commit on OK (Firestorm: as they move, each a save of the file); the autoresponses take an IM's length (1023 bytes; Firestorm's text boxes stopped at 255); `OnlineOfflinetoNearbyChat` (a Firestorm setting) is not here; texts rewritten (en, ja) |
| skins/default/xui/{en,ja}/floater_plus_contact_set_add.xml | floater_fs_contact_add.xml | the photo screen's look; texts rewritten |
| notifications.xml (en, ja; official files, marked) | notifications.xml (`AddNewContactSet`, `RemoveContactSet`, `RemoveContactFromSet`, `RemoveContactsFromSet`, `SetAvatarPseudonym`, `SetAvatarPseudonymMultiple`, `RenameContactSetFailure`, `AddToContactSetSingleSuccess`, `AddToContactSetMultipleSuccess`, `DisplayNameUpdateRemoveAlias`) | the names kept; the inputs at most 63 characters; the two "added" notices are tips; ja rewritten |
| app_settings/settings_plus.xml | settings.xml (`FSContactSetsColorize{MiniMap,NameTag,Radar,Chat,Friends}`, `FSContactSetsNotification{Toast,NearbyChat}`) | the same names, types and defaults (so a Firestorm user's values come over with the bringing over of settings) |

## lggcontactsets.cpp: what changed

- Reading: `plus::contactsets::readFile()` (at most 8 MB, nested at most 16 deep, a map) and every value checked:
  ids (`LLUUID::validate`, not null), colours (3 or 4 finite numbers, held to 0..1), set names and aliases (one
  line, printable, at most 255 bytes), autoresponses (printable, at most 1023 bytes), at most 1000 sets and
  200,000 ids; what is left out is counted in the log. A file that cannot be read is moved aside (`.bad`,
  `plus::putAside`) and the sets start empty (Firestorm read it as empty and wrote over it at the next change).
- `loadFromDisk()` can be called again (the bringing over of settings): what was read before goes first
  (Firestorm added to it). With no account folder nothing is read or written.
- Saving: `plus::writeFileReplacing()` (never a cut-off file); a failed save is a warning, the sets stay in memory.
  Nothing is saved when the singleton goes (every change was saved when made).
- The alias: Firestorm wrote it into the name cache's entries (and erased and fetched the name again at each
  change); here the name cache applies it whenever it gives a name out (`llavatarnamecache.cpp`,
  `setCustomNameCheckCallback`, the same callback type), so the cache and its file keep the real names. The
  name tags are made again (`LLVOAvatar::invalidateNameTag`) and the lists told (the signal, once the name is known).
  The callback is a function that reads a pointer `cleanupSingleton()` clears (no lock per name).
- An empty alias is none (Firestorm kept it and showed `''`).
- Speed (research/333 5): `hasFriendColorThatShouldShow()` returns at once with no sets (the default), then on the
  switches (`LLCachedControl`), then RLVa. `getFriendColor()` makes no copies (Firestorm copied each set's
  members to count them); the colour of an avatar's smallest set is kept in a small cache, emptied at every change
  (`markChanged()`). `notifyForFriend()`, `getAutoresponseForFriend()`, `isFriendInAnySet()` loop over the sets
  without copies.
- Left out, as nothing in this viewer calls them (cs-integ review): `colorize()` (the official friend, muted and
  Linden colours stay where they are: plus/contactsethooks/ORIGIN.md), `exportContactSet()` /
  `saveContactSetToDisk()` (Firestorm's export of one set), `getFriendsInSet()`, `isFriendInSet()`,
  `getListOfNonFriends()`, `getPseudonym()`, `hasVisuallyDifferentPseudonym()`.
- `getContactSet()` warns no more (the lists ask with their own names, and a set's name is the user's: research/287 P13).
  No names, aliases or set names are written to the log.
- RLVa's @shownames through `plus::rlv::canShowMenuName()` (the same check as the lists of branch cs-color).
- Every change goes through `markChanged()` (the version up, the cache emptied). The colour changes signal
  `UPDATED_MEMBERS` (Firestorm's did not: the lists showed them at their next update); "sort by online status"
  signals `UPDATED_LISTS` (the list is ordered again; Firestorm: `UPDATED_MEMBERS`). `addToSet()` with a set
  that does not exist only adds the non-friends to the extra avatars, as Firestorm (Firestorm wrote through
  `string_view::data()`, which need not end the name).
- `removeSet()` finds the set again after taking out its non-friends.

## Hooks in official files (all marked `<plus> contactsets ... </plus>`)

| File | What |
|---|---|
| llstartup.cpp | `plus::contactsets::onLogin()` after the friends list is read (Firestorm's place) |
| llviewerdisplayname.cpp | `onDisplayNameUpdate()`: with an alias, `DisplayNameUpdateRemoveAlias` instead of `DisplayNameUpdate` (Firestorm's also read `FSShowDisplayNameUpdateNotification`, not here) |
| llmessage/llavatarnamecache.h/.cpp | `setCustomNameCheckCallback()` (Firestorm's type), `applyCustomName()` in `getName()`, `getNameCallback()` and the signals of `processName()` |
| skins/default/xui/{en,ja}/notifications.xml | the ten notifications above |

## Not taken

- `fspanelcontactsets.*` (the vintage skin's window), `fsfloatercontacts` (Firestorm's own Contacts window: the
  People window is this viewer's).
- The People tab, its menus, the profile's set and the avatar picker's sets (branch cs-people), and the colours,
  notices and autoresponses where they show (branch cs-color).
