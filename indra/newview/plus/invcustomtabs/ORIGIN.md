# Custom inventory filter tabs: origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commits | `acafaea8c6`, `e57418a27f`, `763c20fbf7` (FIRE-35598), `d65f501e51` (close confirmation), `f4ba9796c2` (FIRE-36831, hiding tabs), `c0846c53d7` (FIRE-36910); file as of origin/master `38722fe4e4` |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`); the Firestorm copyright lines are kept in both files |
| Authors | PanteraPolnocy (the feature), Hecklezz (FIRE-36910); the idea of a tabbed inventory comes from the Catznip viewer (Kitty Barnett) |

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| invcustomtabs.h/.cpp | indra/newview/fsinventorycustomtabs.h/.cpp (`FSInventoryCustomTabs`) | class `PlusInvCustomTabs`; inner names `Plus*`; helpers split out (button tabs, the tab under the mouse, the notifications, a saved entry, the hidden-tabs menu); the hidden-tabs menu is shown by show() and placed by showPopup() (on a detached window, kept on screen); Firestorm calls show() only; "+" and the arrow act on the mouse press and a double click counts as a second press; the close icon is only drawn (its press is taken the same way) and a double click on it, or right after a press that closed a tab at once (no question), does nothing; the tab container may not select them by any way (a validate callback; Firestorm lets it select them, which shows their empty inventory panel and builds its views), and no click callback is added to their buttons here (a press never reaches them, and the validate callback refuses them too); the tab strip takes no press while it is hidden (single folder view); after another window saved, the changed tabs are updated in place when the number of entries is the same (else made again, and the custom tab shown is selected again by its previous entry, nearest to its old place) and gives back its unsaved words; a saved name always marks the tab as renamed; saved words lose emoji and are cut at 300 characters, labels made from words get the rename's checks; a saved "only links" is read as "include links" (the official setFilterSubString() returns the whole filter to its defaults at the first word typed; Firestorm keeps the value as it is); the arrow's drop is refused like "+" (FIRE-36910); a tab is added at END and moved before the button tabs (Firestorm's insertTuple() takes an index, the official one does not) and selected afterwards; the saved values are bounded on load (hours ago, date direction, permissions, the enums, the name through the rename's checks); a missing field keeps the tab's value (the official `fromParams()` sets every field, Firestorm's only those provided); the close confirmation is not kept as pending when "do not ask again" answered it inside `add()` |
| invcustomtabs.cmake | CMakeLists.txt entries | plus foundation |
| llui/lltabcontainer.{h,cpp} | `getTabContainedAtPoint()`, `setTabPadding()`, `setTabButtonVisible()` | as Firestorm (`getTabContainedAtPoint()` also -1 while not in the visible chain); `moveTabPanel()` and `setTabButtonWidth()` added; `removeTabPanel()` moves the current index left when a tab left of it goes. Marked `<plus>` |
| llinventoryfunctions.cpp | `reset_inventory_filter()`: Firestorm's `showAllItemsPanel()` first | only when a custom tab is shown (`selectAllItemsPanel()`); Firestorm also resets the filters there (FIRE-5160), not taken. Marked `<plus>` |
| llinventorypanel.cpp | - | `openInventoryPanelAndSetSelection()` and `doCreate()` select in the panel shown after `reset_inventory_filter()` when it left a custom tab of the "inventory" window (compared with that window, not the topmost one). Marked `<plus>` |
| llinventoryfilter.cpp | - | `isSinceLogoff()` names LastLogoff per account, as `setDateRangeLastLogoff()` does (it named gSavedSettings; the LLCachedControl cache is shared by name and bound to the group of the first one made, normally setDateRangeLastLogoff()'s, so it worked: now it does not depend on the order; no visible change; Firestorm has the same line). Marked `<plus>` |
| llpanelmaininventory.{h,cpp} | the `FSInventoryCustomTabs::` calls | marked `<plus>`. This viewer has no `FSSplitInventorySearchOverTabs`: only a custom tab keeps its own search words (`onFilterEdit()`, `onFilterSelected()`, `draw()`; `findLinks()` goes to My Inventory first), clearing them keeps its type filter (`onClearSearch()`, as Firestorm's FIRE-5160 for these tabs only), and Received Items follows the words only when a custom tab is left or shown |
| skins/default/xui/{en,ja}/menu_inventory_custom_tab.xml | the same files | callbacks `PlusInvCustomTab.*`; ja wording ours |
| notifications.xml (en, ja) | `FSInventoryCustomTabRename`, `FSInventoryCustomTabClose` | names kept; ja wording ours |
| strings_plus.xml (en, ja) | strings.xml `FSInventoryCustomTabDefaultName`, `FSInventoryCustomTabShowHidden` | names kept; in strings_plus.xml |
| panel_main_inventory.xml | `use_ellipses="true"` on the tab container | as Firestorm; marked |
| app_settings/settings_plus.xml | settings_per_account.xml `FSInventoryCustomTabs` | name and format kept (PerAccount 1) |

## Not taken

- `FSSplitInventorySearchOverTabs` (Firestorm's per-tab search for every tab) and the
  Firestorm-only hooks next to it (the filter dropdown, `onSearchTypeChecked`, the
  finder's permissions, coalesced-objects and links commits).
- The translations other than ja.
