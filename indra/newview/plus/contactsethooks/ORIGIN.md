# Contact set hooks: origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| License | GNU LGPL 2.1 only |
| Design | research/333 (branch cs-color) |

Firestorm calls `LGGContactSets` (Greg Hendrickson's contact sets, ported in plus/contactsets) from the
official files directly. Here the official files call `plus::cshooks` (cshooks.h), which asks
`LGGContactSets` only while the place's setting is on and a set exists, and keeps the colour of an
avatar for a second (the name tags and the minimap ask on every frame).

## Places taken

| Here | Firestorm | Changes |
|---|---|---|
| llvoavatar.cpp `idleUpdateNameTagText()` (tag and chat bubble) | llvoavatar.cpp `getNameTagColor()`, `idleUpdateNameTagText()` | the tag is built again when the set colour changes (Charis builds it on changes only); `colorize()` is not called (the official friend, mute and name colours stay) |
| llnetmap.cpp `draw()` | llnetmap.cpp `getAvatarColor()` | no `colorize()`; no marks (`sAvatarMarksMap` is not in this viewer) |
| plus/radar radarfloater.cpp `styleRow()` | fsradar.cpp `updateRadarList()` | the name cell's colour |
| llviewerchat.cpp `getChatColor()` (the LLUIColor one) | llviewerchat.cpp | after the official colour, before the keyword colour; muted lines stay grey |
| llavatarlistitem.cpp / llavatarlist.cpp | the same (FIRE-32748, FIRE-31733, FIRE-36478) | `setUseContactSetColors()` / `setUseContactSetListStyle()` for the lists that ask (plus/contactsets peopletab, llpanelpeople); the list follows cshooks' change signal itself |
| llcallingcard.cpp `processNotify()`, llnotificationtiphandler.cpp | the same (FIRE-3522) | the nearby chat line goes through `LLHandlerUtil::logToNearbyChat()` (Charis has no `OnlineOfflinetoNearbyChat`) |
| llviewermessage.cpp `send_do_not_disturb_message()` | the same, through `LLIMProcessing::getAutoresponseTextForAvatar()` | the busy answer only: Charis has no autorespond / non-friends autorespond modes |

## Not taken

- `LGGContactSets::colorize()` (Firestorm's one place for the friend, muted, Linden and Firestorm-team colours): the
  official colours stay where they are.
- The mouselook IFF markers (llviewerwindow.cpp): not in this viewer.
- Client tag colours (no client tags here).
