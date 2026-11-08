# Chat keyword alerts: origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `443cc2b4ebf9e00da5bb52f00a3e45be790d4a75` (2026-05-25, master) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=fsviewerlgpl$` in Firestorm, the same LGPL 2.1 text); the Firestorm copyright line is kept in every file taken from it |
| Authors | The Phoenix Firestorm Project, Inc. (FSKeywords, 2011) |

The code was rewritten for this viewer, following Firestorm's behaviour (which
lines are looked at, color, sound); the matching is new.

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| keywords.h/.cpp | indra/newview/fskeywords.h/.cpp (`FSKeywords`: the word list, `chatContainsKeyword()`, the sound) | namespace `plus::keywords`; matching on characters (UTF-32) with the same folding of words and text (case, full/half width, hiragana/katakana), so that Japanese words match, also with "whole words" (Firestorm matches UTF-8 bytes with `boost::regex` and `\b`, which never matches a Japanese word); the own names added to the list; full-width comma, ideographic comma and new line also separate words; empty words ignored; no sound in Do Not Disturb, at most one sound per 0.5 s; optional notice; the color through `colorFor()` / `colorNameFor()` |
| keywordsfloater.cpp | - | new: a settings window with a test field (Firestorm has the settings in the chat page of the preferences) |
| keywords.cmake | - | new (plus foundation) |
| skins/default/xui/{en,ja}/floater_plus_keywords.xml | - | new |
| app_settings/settings_plus.xml (keywords part) | the settings `FSKeywordOn`, `FSKeywords`, `FSKeywordInChat`, `FSKeywordInIM`, `FSKeywordCaseSensitive`, `FSKeywordMatchWholeWords`, `FSKeywordSpeakersName`, ... | new names `PlusKeywords*` (all per account), and new settings for the folding, own names and notice |

The official hooks are marked `<plus>` (plus/plus-hooks.sh lists the hooks). Nearby chat
(llviewermessage.cpp `process_chat_from_simulator()`) and the color (llviewerchat.cpp
`getChatColor()`) are where Firestorm has them; instant messages are checked in
llimview.cpp `LLIMMgr::addMessage()` (Firestorm: in llimprocessing.cpp).

## Not taken

- Growl, instant messages from objects (llimprocessing `IM_FROM_TASK`), friend
  online notices (llcallingcard), the radar reports (fsradar).
