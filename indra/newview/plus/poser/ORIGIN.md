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
| fsposeranimator.*, fsjointpose.* | same names | precompiled header include; `friend class PlusPoserTools` in fsposeranimator.h (below) |
| fsposingmotion.* | same names | precompiled header include; a joint the skeleton lacks is skipped (animations load with allow_invalid_joints: Recapture crashed on it) |
| fsposestate.* | same names | precompiled header include, and the includes it needs (llviewerobjectlist.h, llvoavatarself.h) |
| fsvirtualtrackpad.* | same names | precompiled header include only |
| fsmaniprotatejoint.* | same names | derives from `LLManip` instead of `LLManipRotate`; the private LLManipRotate members/helpers it used are declared in the class (helpers copied from llmaniprotate.cpp); no GL_CLIP_PLANE0 / glGetError() |
| fsmaniptranslatejoint.* | same names | derives from `LLManip` instead of `LLManipTranslate`, members declared in the class; unused grid texture and snap-offset code dropped |
| fstoolcomppose.* | `FSToolCompPose`, `FSToolCompPoseTranslate` in lltoolcomp.*, `gPoserToolset` in lltoolmgr.* | moved into own files, LLSingleton; drawn through `isAlwaysRendered()` instead of a hook in llviewerwindow.cpp; translate tool no longer hands CTRL+SHIFT to the object scale tool |
| poser.cpp | llviewerfloaterreg.cpp line, textures.xml entries | registration with the plus foundation |
| skins/default/xui/{en,ja}/floater_fs_poser.xml, menu_fs_poser_poses_btn.xml, en/widgets/fs_virtual_trackpad.xml | same | + `confirm_overwrite_message` string; + the hand tab "Easy" (`hands_easy_panel`, marked `<!-- plus: hand tab Easy -->`) |
| app_settings/plus/poser/hand_presets/*.xml | app_settings/poses/hand_presets/*.xml | none; Charis adds Open_Hand, Peace, Rock, Gun (made from the finger model of `handcurl.cpp` by `handcurl_genpresets_test.cpp`; their version is `NAMED_HAND_FILE_VERSION` there, raised when a hand changes: a file is copied to the user's folder only when the user has no copy of that version or newer. The files are compared with the model by that tool's `--check` and by `tools/hand_presets.py`) |
| skins/default/textures/plus/*.png | textures/icons, windows | none (declared in poser.cpp) |
| app_settings/settings_plus.xml (poser part) | app_settings/settings.xml | + `FSManipRotateJointTargetSize` (Firestorm only declares it from code) |

Changes for other plus features: `FSFloaterPoser` and `FSPoserAnimator` declare
`friend class PlusPoserTools` (plus/posermcp, the local MCP endpoint), so that
feature reuses the window's animator, save/load and permission code instead
of copying it. Marked `// <plus>`.

Not taken: fsfloaterposestand / fspose (pose stand, a different feature),
fsjointrotatetool.* (an unused stub in Firestorm), the toolbar button
(commands.xml + strings.xml). Firestorm's poser has no RLVa checks (only its pose
stand does); the avatar list here holds the agent and its own animesh only.

User pose files are stored where Firestorm stores them
(`<user settings>/poses/*.xml`), so poses can be copied between the viewers.
