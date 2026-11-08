# Quick Preferences: origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `10bd3c9f930c76e1427ddd4ecece6cdf36b4406d` (2026-05-22) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), headers kept in every file taken from it |
| Authors | WoLf Loonie (2011), Zi Ree (dynamic list, 2013), Ansariel Hiller (2013), Cinder Biscuits (2013) |

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| quickprefsfloater.h/.cpp | indra/newview/quickprefs.h/.cpp (`FloaterQuickPrefs`) | namespace `plus::quickprefs`; a plain `LLFloater` (Firestorm: `LLTransientDockableFloater` docked to a toolbar button); lambdas and scoped connections instead of `boost::bind` and raw `this` captured by region callbacks (handles); the list is saved only when it changed; a row whose setting does not exist here is left out when the list is read; a duplicate setting restores the row's own setting in the combo; the labels of the standard list are floater strings (`translation_id`) |
| quickprefs.cpp | the registration in llviewerfloaterreg.cpp | registration with the plus foundation |
| skins/default/xui/{en,ja}/floater_plus_quickprefs.xml | floater_quickprefs.xml (en, ja) | names kept; `QuickPrefs*` settings -> `PlusQuickPrefs*`; fixed part shortened (see below); notification texts as floater strings |
| skins/default/xui/{en,ja}/panel_plus_quickprefs_item.xml | panel_quickprefs_item.xml (en, ja) | `QuickPrefsEditMode` -> `PlusQuickPrefsEditMode` |
| app_settings/plus/quickprefs/quick_preferences.xml | app_settings/quick_preferences.xml | only settings that exist here; Name Tags is a 0-2 slider (Firestorm: radio); Max Bandwidth added (Firestorm has it as a fixed row) |
| app_settings/settings_plus.xml (quickprefs part) | settings.xml | `QuickPrefsEditMode` -> `PlusQuickPrefsEditMode`, `QuickPrefsSelectedControl` -> `PlusQuickPrefsSelectedControl` |

The user's own list is `plus_quick_preferences.xml` in the user settings folder
(Firestorm: `QuickPreferences` in the settings folder). It has the same format, so
a Firestorm file can be copied over it; settings that do not exist here are left out.

## Not taken

- Phototools (`phototools` floater, vignette, shadow and SSAO sliders): the photo tools (plus/photomode) do that.
- RLVa (the windlight restrictions).
- OpenSim legacy Windlight presets.
- The toolbar button and docking (`dockToToolbarButton()`); the window opens from the Plus menu.
- The fixed Max Complexity row and the fixed Max Bandwidth row (bandwidth is in the standard list;
  `IndirectMaxComplexity` needs the preferences' own code, so it is not offered).
- The `RenderAvatarMaxVisible` -> `IndirectMaxNonImpostors` conversion of old list files.
