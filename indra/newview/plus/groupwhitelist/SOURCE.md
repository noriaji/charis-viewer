# Group chat black and white lists: origin

Charis's own feature. No code is taken from another viewer; Firestorm's group chat
muting was read for the idea and for what to avoid.

| | |
|---|---|
| Read (not copied) | Firestorm (Phoenix Firestorm Project), https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `443cc2b4ebf9e00da5bb52f00a3e45be790d4a75` (2026-05-25) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`) in the files read: indra/newview/llimview.cpp, llimprocessing.cpp, exogroupmutelist.cpp (Katharine Berry, 2012) |
| What was read | `LLViewerChatterBoxInvitation::post()` "Mute group chat port from Phoenix" (`FSMuteAllGroups`, `FSMuteGroupWhenNoticesDisabled`, the IM_SESSION_LEAVE sent by "KC"); the per-group mute `exoGroupMuteList` and its use in `LLIMMgr::addMessage()` and `IM_SESSION_SEND` (llimprocessing.cpp); FIRE-14222 (the agent's own lines are not muted) |

## Differences from Firestorm

- A whitelist: "only whitelisted groups" refuses every other group (Firestorm has only
  "mute all groups" and "mute groups whose notices are off", and a per-group mute).
- The decision is made from the session id and our own saved lists, never from
  `gAgent.getGroupData()` (Firestorm lets the chat through when the group data is missing,
  e.g. just after login).
- One check for every kind of ChatterBoxInvitation (text, voice, immediate) and one in
  `LLIMMgr::addMessage()`, which every group chat line goes through; Firestorm's
  "mute all" checks only the text invitation, and its deferred / snoozed group chats are
  restored without it.
- Refused chat leaves no notice in nearby chat (Firestorm: `FSReportMutedGroupChat`).
- The lists are a file of their own per account (`plus_group_chat_lists.xml`); Firestorm
  keeps its per-group mutes in the server mute list as `Group:<id>`.
