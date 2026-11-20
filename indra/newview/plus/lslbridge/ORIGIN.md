# LSL bridge (pose bridge) — origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `443cc2b4ebf9e00da5bb52f00a3e45be790d4a75` (2026-05-25, master; the bridge files last changed in `8c4c9fbe96`, 2024-09-06) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=fsviewerlgpl$`); the files here carry the viewer's LGPL 2.1-only header, and lslbridge.cpp (which takes code from fslslbridge.cpp) also Firestorm's copyright line |
| Authors | The Phoenix Firestorm Project (fslslbridge.cpp/.h, fslslbridgerequest.cpp; the bridge script by Tozh Taurog, Arrehn Oberlander, Tonya Souther) |
| Design | research/336-poser-aurora-compat.md (§2 the Aurora-compatible pose1 messages, §3, §4.1, §5, §7), research/336-api-contract.md (`lslbridge.h`) |

What it is here: the attachment that carries the pose-consent messages (research/336) between avatars in a region.
Nothing else of Firestorm's bridge is taken.

## Files

| Here | Firestorm | Changes |
|---|---|---|
| lslbridge.cpp | fslslbridge.cpp, fslslbridgerequest.cpp | Rewritten around a step machine with the states of `lslbridge.h` (the table in research/336). Taken over: the bridge folder in the inventory, the library rock copied into it (`LIB_ROCK_NAME`, found in Library/Objects/Landscaping and then anywhere), worn on HUD Center 2 (`FS_BRIDGE_POINT` 31, added, not replacing), made small and clear (`setupBridgePrim` as `makeSmallAndClear()`: the shape, scale, position, `MultipleObjectUpdate` and clear faces as they were; dropped: `gAgentAvatarp->clampAttachmentPositions()` (Firestorm's, the official avatar has none), `addFlags(FLAGS_TEMPORARY_ON_REZ)` and `updateFlags()` (`LLPrimitive::addFlags` sets the misc flags only, so they did nothing; the script makes a bridge put on the ground temporary)), the worn object's contents checked (`inventoryChanged`), the script made in the inventory, uploaded (`UpdateScriptAgent`, mono) and put into the worn object (`saveScript`), other bridges taken off, the script's HTTP-in URL learned from its llOwnerSay line and posted to. Changed: Charis names (`#Charis` folder in the inventory root, made through `plus::makeCharisFolder()` shared with plus/ao; the object and its script `#Charis Bridge v<version>`); the line is taken only from the worn bridge item itself (the root of the speaker is our attachment of that item), not from any object that knows the folder's id; the bridge object is the one this viewer copied from the library for the account, recorded then (`PlusPoseBridgeItem`, per account, saved at once, hidden from the Debug Settings): FindItem wears only it (asked for by its id and waited for when it is not read yet) and the line before the item is known (its announcement comes right after the viewer's call at login) is taken only from it, never from an object found by its name or place (Firestorm finds its bridge by name in its folder: someone could give a folder and an object of those names with scripts in other prims, and the viewer would wear it); objects of the bridge's name that were not recorded are only told in the log; a bridge with other prims linked to it is never used (taken off and made anew, a repair); the setting is per account (`PlusPoseBridgeEnable`, on by default as Firestorm's `UseLSLBridge` is, since 2026-11-15, research/353: each account can turn it off; until the account's settings are read at login the bridge only waits; it starts once the login's outfit is on, and not in a safe start); the bridge's URL lines of anything else are dropped unread; the URL must be a Second Life simulator's (`proto::isBridgeUrl`); the script takes a token it makes itself (`llGenerateKey`) for every URL and refuses posts without it; posts have a 10 s timeout, no retries, no redirects, and are not logged by us (`callbackHttpPost` logs the URL at every post; a post that fails is still logged by the official `HttpCoroHandler::onCompleted()`, the URL's capability id replaced by "(capability)" by llerror's `scrubCapabilities`, the simulator's host name shown; the token is in the body only, so the log does not let anyone post to the bridge); a bridge is not asked to be worn without an attachment place to spare, one always left for the person, counted by the attachments worn and by those the COF lists with the temporary ones worn (one count of them, `countTempAttachments()`) (Failed NoSlot: not tried again by the clock, a new attempt once a change of the attachments leaves a place, `failedForGood()` false; the other Failed with no new try to come, no library object, not Second Life or the retries used up, are tried again by turning the setting off and on, `failedForGood()` true); a bridge that started by itself is only put on again by its repairs for not answering (made anew only for another script version or linked parts); repairs are counted (3, then Failed and new tries later; the failures are shown only when the person turned the bridge on in this session, else logged: on by default, a slow login or a grid without it would show them to everyone) instead of re-attaching forever; the script is made at most 5 times from the attempt's start or the last answered hello (those cut short by a take-off too), a take-off while it is made is a take-off as any; a wear asked for that comes after the bridge stopped is taken off again; a bridge taken off is put on again 3 times in 10 minutes; only items under our exact names are taken off or moved to the trash, never one a guard (`plus::invguard`) keeps; a URL lost to a region change or a failed post is waited for (30 s) before the bridge counts as silent, and a silent bridge is waited for again across a region change or back where scripts run before it is repaired; the hello, and the pose lines taken, wait until the worn object's contents, asked of the simulator again (not the copy kept; asked once more when a request was already on the way; an answer is taken only for the ask it answered) at every new URL, were seen holding no other script; a pose line of the worn bridge that does not pass is dropped unread (it names a peer: never shown, logged or translated; also when the speaker's object is not seen, or not confirmed as the worn bridge, but its id is the bridge's seen clean), another object's line in that form goes on as any line; a bridge that comes off while its hello is waited for counts as a repair (its script takes itself off when its contents never come to the viewer); the library is "without the rock" only once all of its folders are loaded; a region that gives no URL is waited out without repairs; no OpenSim (Firestorm returns there too). The attach and detach are seen through `LLAppearanceMgr::setAttachmentsChangedCallback` and a 1-second tick instead of hooks in llvoavatarself.cpp. |
| lslbridgeproto.h/.cpp | (the parsing in `FSLSLBridge::lslToViewer`) | New: the bridge's lines and the bodies posted to it, checked strictly (lengths, fields, UUIDs, the URL's host and path); the shape of a refused announcement for the log without its secrets. Plain C++, tested by `lslbridgeproto_test.cpp` (not built). |
| lslbridge.h | none | The interface for the other branches (research/336-api-contract.md); its declarations as the contract has them (`setSendResultHandler` added with the contract on 2026-11-14: the result of every accepted `sendPose`, once), its comments made to say what the code does. Added 2026-11-20 (research/374, 374b): `sendShare`/`sendShareEnd`/`setShareHandler`, `sendReflect`/`setReflectHandler`/`setReflectResultHandler`, `sendPush`/`sendPushEnd`/`setPushHandler`. |
| lslbridgehooks.h | the `lslToViewer` call in llviewermessage.cpp; Firestorm's own changes in llappearancemgr.cpp (`<FS:TS>` FIRE-3018, `<FS:Ansariel>` "Don't add LSL bridge to outfit", `<FS:TT>` "Reinserting bridge at outfit replace") | The outfit's three, called from llappearancemgr.cpp (research/353): `isBridgeItem()` (the recorded object only, not by its name as Firestorm does) leaves the bridge out of a saved outfit (`slamCategoryLinks`: Save and Save As; Firestorm also in `shallowCopyCategoryContents`, which here only copies the library's gestures), `dropBridgeLinks()` out of the outfit's "changed" compare (from the COF and from the outfit), `placeBridgeInNewCof()` (with `bridgeForOutfit()`, which the outfit backup's restore asks too) keeps the worn bridge in the new COF of a replace or an add while it is to run and has a place, the person's attachments first (theirs, the temporary ones, the bridge and one place left; else it is left out, comes off, and is put on again once the attachments leave a place: Firestorm always puts its bridge back in). plus/outfitbackup leaves it out of its copy and placements. `handleOwnerSay()`: called earlier in `process_chat_from_simulator` (before the speech particles and RLVa), for the llOwnerSay lines of our own objects; the bridge's URL lines are taken there whoever said them, and its pose lines when the bridge is known as the speaker (seen as the worn bridge, or by its id, the one whose contents were seen clean): those reach neither the chat, RLVa nor the translation. Its share, reflect and push lines (version 1.2) are handled the same way and are dropped whoever the speaker is. A pose line of a speaker not known as the bridge goes on as any line (only a script that is not the bridge's could say one). |
| app_settings/plus/lslbridge/charis_bridge.lsl | fs_resources/EBEDD1D2-A320-43f5-88CF-DD47BBCA5DFB.lsltxt | Written for Charis. From Firestorm's script only these ideas: the URL asked for on attach and region change and announced to the owner, a bridge put on the ground made temporary, other scripts in the bridge stopped, the bridge taking itself off in another viewer (after 60 seconds without a call here). Its own: the call (the script says nothing before the owner's viewer called), the token and the hello for every URL, the "no URL" line, the pose1 relay (below). None of Firestorm's commands (radar altitudes, script info, movelock, flight assist, teleport, the OpenCollar / LockMeister AO). |
| lslbridge_lsl_check.py | none | Desk check of the script: agreement with lslbridgeproto and lslbridge.cpp, a lint, and a small interpreter of the LSL the script is written in that runs its own text on the cases of the handoff's table; `--mutants` breaks it 353 ways and each must be caught: the script 99, lslbridge.cpp 238, and 16 of llappearancemgr.cpp's hooks, plus/outfitbackup, the poser and their texts (research/353). Not built. |

## Aurora

The pose1 messages (channel -184735220, `pose1|<nonce>|<action>|<target>`, the five actions, 130 bytes) and the
`<auroraPose>` line are Aurora's (https://github.com/Aurora-Viewer/Aurora-Viewer, GPL 3), so that Charis and
Aurora users can ask each other. The script follows that format and the checks research/336 §2 describes. Aurora's
public bridge script (query.lsl, main of 2026-11-13) was read once to confirm the format and the point the bridge is
worn on; the first version of the relay here came out close to Aurora's listen handler (the same order of checks and
the same names), and the pre-merge review asked for it to be written again: it was rewritten from the §2 text alone,
without looking at Aurora's script again, in its own parts (`wearerOf()` finds the sending attachment's wearer,
`requestTaken()` keeps the request gap in two lists, `relayPose()` checks the message first and the sender after),
names and order. No code of Aurora is in it.

## Pose sharing, reflect and push (version 1.2, research/374 and 374b)

Charis-to-Charis only; nothing of Aurora's or Firestorm's (another viewer's or Aurora's bridge listens on none of
the three channels). The script, `lslbridgeproto` and `lslbridge.cpp` carry three more kinds of message, each on its own
positive channel and only after the viewer's hello:

| Kind | Viewer to bridge | Bridge to bridge | Bridge to viewer |
|---|---|---|---|
| share | `share\|<token>\|<aud>\|<seq>\|<payload>`, `shareend\|<token>\|<seq>` | `llShout` on `SHARE_CHANNEL` (473520191), `share1\|<aud>\|<seq>\|<payload>` (an empty payload: the end) | `<charisShare>1\|<peer>\|<aud>\|<seq>\|<payload></charisShare>` |
| reflect | `reflect\|<token>\|<action>\|<nonce>\|<target>` | `llRegionSayTo` the target on `REFLECT_CHANNEL` (473520192), `reflect1\|<nonce>\|<action>\|<target>` | `<charisReflect><nonce>\|<action>\|<peer></charisReflect>` |
| push | `push\|<token>\|<target>\|<seq>\|<payload>`, `pushend\|<token>\|<target>\|<seq>` | `llRegionSayTo` the target on `PUSH_CHANNEL` (473520193), `push1\|<seq>\|<payload>` | `<charisPush>1\|<peer>\|<seq>\|<payload></charisPush>` |

The script reads no payload: it checks its length (900 at most, a multiple of 4), that it holds none of `< > " space`
and a newline, and the sequence number (9 digits at most, written back as it is); the viewer checks the base64 and
what the pose means (`proto::parseShareLine`, `parsePushLine`). The peer of every told line is the simulator's owner of
the sending attachment (`wearerOf`), never a name or key in a message. A sender is held to one share or push every
0.3 s (an avatar's, not an object's; share and push each have their gap, a reflect request keeps the 5 s gap of pose
requests, kept under its own keys in the same table of 64 askers), 32 senders are kept (a new one takes the place of one whose last time is older than the gap, and is not taken when there is none; llResetTime() at every wear keeps the float clock fine); the script says at most 6 things a second and the viewer
launches at most 4 shares and pushes in any second in all (a sliding window of the launches, both lanes together; the
ends have a window of their own, 2 in any second, so the 6 things a second the script says are kept; a newer body that replaces a waiting one is never refused for the rate), with a lane of its own for
the HTTP posts that keeps the latest body only (at most 8 posts on their way, also when a waiting body is flushed, 8 waiting, not part of the 16 of
`sendPose`), a key's posts 0.35 s apart counted from the END of the last post (the bridge says a post when it arrives,
before the answer comes back, so its own gap is at least that; the receiving gap is 0.3 s). A busy simulator's or a
timed-out sharing post is not counted toward a lost URL as the hello's and the pose messages' are (3 in a row): the URL is
taken as lost when sharing posts have been failing for 15 s with no answer between and no quiet of 12 s or more between two
failures (any answer of the bridge, ok, 429 or another refusal, ends the run; told in the log when the run starts and when
it ends). The script's rate refusal (429, `PostResult::Rate`) is a debug line; any other refusal of a sharing post
is warned once for a URL (a new URL's hello renews the warning). The script cuts a heard share or push with `llSubStringIndex` and
makes no list of it (anyone near can shout at it). The told lines pass the same checks as the pose lines (the worn bridge,
its contents seen clean, the form, not naming ourselves, not while Off) and are never shown: a line of this form from an
object not confirmed as the worn bridge is dropped too (unlike a pose line, which goes on as any line of such an object). The line sizes (1000 bytes) and the body (1100, the whole LLSD text: the longest is a push, 989 + 30 = 1019) leave room for a
whole pose (about 780 base64 characters, research/374 section 3). Not read from any Firestorm or Aurora source: the three kinds are this project's own.

## Not taken

Everything of Firestorm's bridge that is not the pose relay: `getZOffsets` (radar), `getScriptInfo`, movelock,
`llMoveToTarget` teleports, LSL flight assist, the OpenCollar / LockMeister AO interface, `UseLSLBridge` and its
settings, the bridge's own floater and menu items, the `FSLSLBridgeRequestRadarPos_Success` responder, the RLVa
`canDetach` exception (an RLVa lock that keeps the bridge on counts as a failed repair here), the detach and
re-attach "to save" after the script is put in (the contents are checked at every wear instead).

## Known

Solved in script version 1.1 (research/366): the bridge never speaks first. Its llOwnerSay lines (URL and token) would
show in the owner's nearby chat in a viewer other than Charis (Firestorm, Aurora, the official viewer), which may log
them or send them to a translation service; someone who got such a line could post to that URL with its token and make
the bridge send pose1 messages in the owner's name. Now the Charis Viewer calls first (`proto::probeText(key)`: the call text and the key of the worn bridge object,
whispered on the positive channel `proto::PROBE_CHANNEL`, sent straight as ChatFromViewer so that RLV's @sendchannel does
not make the bridge deaf), the script listens to the owner avatar alone on that channel and says its URL only after a call
that holds its own key; other viewers never call, so the bridge says nothing there and takes itself off after 60 seconds.
The key is what keeps others from calling: what the owner's avatar says is not only what they type, it is also the label
of a dialog button (llDialog, llTextBox) of someone's object, which they press, and the text of a
secondlife:///app/chat/<channel>/<text> link anyone's chat line can show (the official viewer and Firestorm both run
it, any length): so the text of a call is no secret. The key of a bridge worn on a HUD point is on no list
(llGetAttachedList and the sensors leave HUDs out, other avatars' viewers never get a HUD) and it changes at every wear;
a bridge worn on the body has a key anyone can read. So the script takes a call on a HUD point only (31 to 38; a bridge
on the body takes itself off after 60 seconds, and the Charis Viewer, which calls only a HUD bridge, puts it on the
HUD again: `stepWear` repairs it; one moved to the body by hand while it runs says nothing at its next URL, the viewer's
wait for that ends Silent and a repair puts it on the HUD), and the key leaves the viewer only in the call and in the relay of a pose request
(llRegionSayTo to the other avatar) while the Charis Viewer, which hides the bridge's lines, runs it. Once called, a new URL (region change, teleport) is said again
without a call (only a call with the key could have come). A bridge script that is reset while it runs says nothing until
the viewer calls again: the viewer calls again only after its posts to the old URL fail (3 in a row), or after the
script took itself off 60 seconds later and is worn again (one of the 3 re-wears in 10 minutes): a rare action,
accepted. The chat's rule that a
positive channel is shown to nobody is the simulator's, not seen in this source: checked on a real session (research/366
section 5.3). What is left: a bridge of version 1.0 (only built by development builds, never in a release) worn in another
viewer still says its lines (3 per URL, off after 45 seconds). The Charis Viewer never wears one: a recorded bridge of an
old version is made anew at the login (`stepFindItem`; one whose wear is still on the way is waited for, then taken off, and
its link in the outfit folder removed, else the next login in another viewer would wear it again), so starting Charis once
removes it.

A bridge on the body is not repaired onto the HUD where an RLV lock (`@remattach=n` and the like) keeps it there: its script
takes itself off every 60 seconds and RLVa wears it again (rlvlocks.cpp), the viewer ends Failed (nothing it can do), and
the bridge says nothing in another viewer either. The key of a bridge changed by moving it between attach points (the
object may not be made anew, so the key may stay) read while it was on the body in another viewer would let a call
through if the person then moves it to the HUD by hand within 60 seconds and also follows a link in the chat: too
contrived, accepted. Not known without a real session: whether moving a worn object between attach points raises
`attach` or changes its key.

A pose1 message is taken from any attachment the other avatar wears, as Aurora takes it (the relay names the wearer
the simulator gives, never one written in the message). The nonce of a request reaches every attachment of the asked
avatar (llRegionSayTo to an avatar), so another scripted attachment they wear can accept in their name: the consent
stays tied to that avatar (it is their own attachment), but it is not always their own choice. Taking the answers only
from HUD Center 2 (point 31, where the Charis and Aurora bridges are worn) would narrow that, not close it (a HUD
with other scripts worn on Center 2 would still be heard). Accepted as it is (decided by the project, 2026-11-14):
nothing is narrowed.

The posts to the bridge's URL check the certificate and that it names the URL's host (`setSSLVerifyPeer(true)`,
`setSSLVerifyHost(true)`, as updatecheck, Velopack and the translation do; HttpOptions' host check is off by
default): a man in the middle would otherwise read the token and could accept in the owner's name. Not seen yet on
an HTTP-in URL (`<…>.secondlife.io:<port>`): the first check in Second Life looks for "Bridge starting -> running";
"Bridge post refused: the certificate does not name the host" (once) means it does not: curl's certificate errors
(51, 60) are told apart from a timeout and the bridge is Failed (Make) at once, not made anew (a new bridge gets an
URL of the same kind), and tried again a few times later, at the retries' own gaps (the script says the same URL
again every 15 s: those announcements are not taken until the retry's time or a region change). It fails closed.
