# Poser — origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `10bd3c9f930c76e1427ddd4ecece6cdf36b4406d` (2026-05-22, master) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`), headers kept in every file |
| Authors | Angeldark Raymaker (poser, animator, joint pose, pose state, trackpad; FIRE-30873), Beq Janus (in-world joint manipulators), chanayane (BVH fixes); fsvirtualtrackpad derives from LLVirtualTrackball by Andrey Lihatskiy (Linden Research) |

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| fsfloaterposer.* | indra/newview/fsfloaterposer.* | toolset from `getPoserToolset()`; overwrite prompt uses the official `GenericAlertYesCancel` notification with the floater string `confirm_overwrite_message`; shipped hand presets read from `app_settings/plus/poser/hand_presets`; beacon colour falls back when `AreaSearchBeaconColor` is missing; pose names are listed as the files are named and saved only under `plus::isSafeFileStem` (Firestorm wrote `..\x` outside the folder); the pose and BVH files are written next to the old one, checked, then moved over it; the tools let go of an avatar no longer selected; the folder opens through `plus::openFolder`; hand tab "Easy" (finger curl/spread sliders, whole-hand bars, copy to the other hand, save the hand as a preset: `// <plus> hand tab "Easy"` blocks and the `onHandEasy*` / `refreshHandEasy` functions at the end of the file); the hand presets list keeps the file name as the row value and shows a display name from the floater strings (`handPresetDisplayName`), and a click applies the preset to the hand last chosen with Set Left / Set Right (`// <plus> hand preset list` block); a right click on a row opens a menu to delete the user's copy of a hand preset (`// <plus> hand preset list: delete`, `menu_fs_poser_hand_preset.xml`; a shipped preset is refused); `FSPoserAnimator::getJointSeenRotation` (fsposeranimator.*, `<plus>`) |
| handcurl.*, handcurl_test.cpp | none (Charis) | finger curl / spread sliders over the joint values for the hand tab "Easy" (research/315); stand-alone test, not in the build |
| fsposeranimator.*, fsjointpose.* | same names | precompiled header include; `friend class PlusPoserTools` in fsposeranimator.h (below); research/374 (`// <plus>`): `PoseOwner` / `sPoseOwner` / `sSharedOthers` (who holds an avatar's posing motion: the window, a share, a push), `beginPosing()` (the one place an owner takes an avatar from another; `tryPosingAvatar()` and `startShared()` call it), `ownerOf` / `motionOf` / `sharedMotion` / `forceStopAllShared`, `FSJointPose::setSharedTarget()` (sets a joint to an absolute rotation without the undo queue), `FSJointPose::setOwnerSerial()` and `UndoBatch::motionSerial` (a drag's undo is valid only on the motion it was made on; the constructor no longer counts `sUndoSerial`), `plus::poser::usableQuaternion` / `clampedPosition` / `clampedScaleDelta` (the value checks of loading, shared) |
| fsposingmotion.* | same names | precompiled header include; a joint the skeleton lacks is skipped (animations load with allow_invalid_joints: Recapture crashed on it); `SnapToTargetScope` (Motion Between Poses, plus/posermotion: while it lives, the motion draws its joints at their targets at once instead of easing towards them) and `onUpdate` walks the joint poses by reference (it copied each one, with its name and undo queue, every frame) (`// <plus>`); `instanceSerial()` (a number new for every motion object, research/374: a shared pose's joint cache must not trust a recycled motion id) and `jointPoses()` |
| fsposestate.* | same names | precompiled header include, and the includes it needs (llviewerobjectlist.h, llvoavatarself.h) |
| fsvirtualtrackpad.* | same names | precompiled header include only |
| fsmaniprotatejoint.* | same names | derives from `LLManip` instead of `LLManipRotate`; the private LLManipRotate members/helpers it used are declared in the class (helpers copied from llmaniprotate.cpp); no GL_CLIP_PLANE0 / glGetError() |
| fsmaniptranslatejoint.* | same names | derives from `LLManip` instead of `LLManipTranslate`, members declared in the class; unused grid texture and snap-offset code dropped |
| fstoolcomppose.* | `FSToolCompPose`, `FSToolCompPoseTranslate` in lltoolcomp.*, `gPoserToolset` in lltoolmgr.* | moved into own files, LLSingleton; drawn through `isAlwaysRendered()` instead of a hook in llviewerwindow.cpp; translate tool no longer hands CTRL+SHIFT to the object scale tool |
| poser.cpp | llviewerfloaterreg.cpp line, textures.xml entries | registration with the plus foundation |
| skins/default/xui/{en,ja}/floater_fs_poser.xml, menu_fs_poser_poses_btn.xml, en/widgets/fs_virtual_trackpad.xml | same | + `confirm_overwrite_message` string; + the hand tab "Easy" (`hands_easy_panel`, marked `<!-- plus: hand tab Easy -->`); + the "Motion Between Poses" button in `button_controls_panel` (an icon in the left 21 px of the spacer, opens `plus_poser_motion` with the official `Floater.ToggleOrBringToFront`; `<!-- plus -->`) |
| app_settings/plus/poser/hand_presets/*.xml | app_settings/poses/hand_presets/*.xml | none; Charis adds Open_Hand, Peace, Rock, Gun (made from the finger model of `handcurl.cpp` by `handcurl_genpresets_test.cpp`; their version is `NAMED_HAND_FILE_VERSION` there, raised when a hand changes: a file is copied to the user's folder only when the user has no copy of that version or newer. The files are compared with the model by that tool's `--check` and by `tools/hand_presets.py`) |
| skins/default/textures/plus/*.png | textures/icons, windows | none (declared in poser.cpp) |
| app_settings/settings_plus.xml (poser part) | app_settings/settings.xml | + `FSManipRotateJointTargetSize` (Firestorm only declares it from code) |

Changes for other plus features: `FSFloaterPoser` and `FSPoserAnimator` declare
`friend class PlusPoserTools` (plus/posermcp, the local MCP endpoint), so that
feature reuses the window's animator, save/load and permission code instead
of copying it. Marked `// <plus>`.

Other people's avatars with their consent (Charis, research/336; marked
`// <plus> research/336` and `<!-- plus: ... research/336 -->`): the model list
also shows nearby people who can be asked (or were refused / did not answer),
and the people asked or allowed at any distance and also out of draw distance
(`plus::poseconsent::outgoingList`), each row with the status of the consent
kept by `plus::poseconsent` in a column of its own, shown only while another
person is listed (`FSFloaterPoser::syncOtherRows`; names as display name + user
name, hidden under RLVa `@shownames`; a waiting or allowed row stays until the
consent ends). The IK tool works on another person like on the own avatar, while they allow it. Firestorm's empty
`havePermissionToAnimateOtherAvatar` asks `plus::poseconsent::canPose` every
time. One button asks / takes the request back / ends the consent
(`onConsentButton`; only a press asks), and the window says "only on your
screen" while another person is selected or anyone else is posed. The IK switch
and tool do not depend on who is selected; the rotate, move and IK tools take an avatar only as the selection
gives one (implied permission, or `canPose`). Another person's pose is never
saved (pose, BVH, hand preset: the own avatar and owned animeshes only) and is
stopped with `FSPoserAnimator::forceStopPosingAvatar` (no region check, the
motion removed, the static tables `sAvatarIdToRegisteredAnimationId`,
`FSPoseState::sMotionStates` / `sCaptureOrder` cleared;
`FSPoseState::forgetAvatar`) at every end: the stop button, the end of the
consent (`setConsentLostHandler`, poser.cpp), a one-second safety net in the
idle loop (poser.cpp, runs with the window closed too; also stops a pose that no
longer runs on a live avatar, `FSPoserAnimator::isOtherPoseAlive`), the window
closing, the row leaving the list. The poser's MCP (plus/posermcp) offers and
poses the own avatar and animeshes only, as before
(`PlusPoserTools::findAvatar`, `canPose` through
`havePermissionToAnimateAvatar`); its one change is that `status` leaves other
people out of `posing` and `posesSettled` does not wait for their poses
(posertools.cpp). The model list is 63 px shorter for the button and its lines.
Another person posed is never a silhouette or an impostor by distance: the
official `LLVOAvatar::exemptFromFullDistance()` (llvoavatar.cpp, in the
research/343 block) asks `plus::poser::isPosingOther()` (poser.h, the poser's
small door for official files); once the pose ends, the person follows the
distances again.

Not taken: fsfloaterposestand / fspose (pose stand, a different feature),
fsjointrotatetool.* (an unused stub in Firestorm), the toolbar button
(commands.xml + strings.xml). Firestorm's poser has no RLVa checks (only its pose
stand does); the avatar list here holds the agent and its own animesh, and the
other people of the consent rows (above: those who can be asked, are waiting,
allowed it, refused or did not answer).

User pose files are stored where Firestorm stores them
(`<user settings>/poses/*.xml`), so poses can be copied between the viewers.
