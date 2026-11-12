/**
 * @file fsfloaterposer.cpp
 * @brief View Model for posing your (and other) avatar(s).
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2024 Angeldark Raymaker @ Second Life
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsfloaterposer.h"
#include "fsposeranimator.h"
#include "fsvirtualtrackpad.h"
#include "plus.h"
#include "llagent.h"
#include "llappviewer.h"
#include "llavatarnamecache.h"
#include "llcheckboxctrl.h"
#include "llcommonutils.h"
#include "llcontrolavatar.h"
#include "lldiriterator.h"
#include "llinventoryfunctions.h"
#include "llloadingindicator.h"
#include "llmutelist.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llsdserialize.h"
#include "llsliderctrl.h"
#include "llstring.h"
#include "lltabcontainer.h"
#include "lltextbox.h" // <plus>
#include "llmenugl.h" // <plus> the right-click menu of the hand preset list
#include "lluictrlfactory.h" // <plus>
#include "lluicolortable.h" // <plus>
#include "fstoolcomppose.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llwindow.h"
#include "v4color.h"

namespace
{
constexpr char             POSE_INTERNAL_FORMAT_FILE_MASK[]    = "*.xml";
constexpr char             POSE_INTERNAL_FORMAT_FILE_EXT[]     = ".xml";
constexpr char             POSE_EXTERNAL_FORMAT_FILE_EXT[]     = ".bvh";
constexpr char             POSE_SAVE_SUBDIRECTORY[]            = "poses";
constexpr std::string_view POSE_PRESETS_HANDS_SUBDIRECTORY     = "hand_presets";
// Port note: the shipped presets live in app_settings/plus/poser/hand_presets
// (Firestorm: app_settings/poses/hand_presets); app_settings/plus/ is what the
// plus hook in viewer_manifest.py packages.
constexpr char             POSE_PRESETS_SOURCE_DIR_PLUS[]      = "plus";
constexpr char             POSE_PRESETS_SOURCE_DIR_FEATURE[]   = "poser";
constexpr char             XML_LIST_HEADER_STRING_PREFIX[]     = "header_";
constexpr char             XML_LIST_TITLE_STRING_PREFIX[]      = "title_";
constexpr char             XML_JOINT_TRANSFORM_STRING_PREFIX[] = "joint_transform_";
constexpr char             XML_JOINT_FRAME_TRANSFORM_PREFIX[]  = "joint_frame_";
constexpr char             XML_JOINT_DELTAROT_STRING_PREFIX[]  = "joint_delta_rotate_";
constexpr char             BVH_JOINT_TRANSFORM_STRING_PREFIX[] = "bvh_joint_transform_";
constexpr std::string_view POSER_TRACKPAD_SENSITIVITY_SAVE_KEY = "FSPoserTrackpadSensitivity";
constexpr std::string_view POSER_STOPPOSINGWHENCLOSED_SAVE_KEY = "FSPoserStopPosingWhenClosed";
constexpr std::string_view POSER_SAVEEXTERNALFORMAT_SAVE_KEY   = "FSPoserSaveExternalFileAlso";
constexpr std::string_view POSER_SAVECONFIRMREQUIRED_SAVE_KEY  = "FSPoserOnSaveConfirmOverwrite";
constexpr std::string_view POSER_UNLOCKPELVISINBVH_SAVE_KEY    = "FSPoserPelvisUnlockedForBvhSave";
constexpr std::string_view POSER_SHOWBONEHIGHLIGHTS_SAVE_KEY   = "FSManipShowJointMarkers";
constexpr char             ICON_SAVE_OK[]                      = "icon_rotation_is_own_work";
constexpr char             ICON_SAVE_FAILED[]                  = "icon_save_failed_button";

}  // namespace

/// <summary>
/// The amount of deflection 'one unit' on the trackpad translates to in radians.
/// The trackpad ordinarily has a range of +1..-1; multiplied by PI, gives PI to -PI, or all 360 degrees of deflection.
/// </summary>
constexpr F32 NormalTrackpadRangeInRads = F_PI;

FSFloaterPoser::FSFloaterPoser(const LLSD& key) : LLFloater(key)
{
    // Bind requests, other controls are find-and-binds, see postBuild()
    mCommitCallbackRegistrar.add("Poser.RefreshAvatars", [this](LLUICtrl*, const LLSD&) { onAvatarsRefresh(); });
    mCommitCallbackRegistrar.add("Poser.StartStopAnimating", [this](LLUICtrl*, const LLSD&) { onPoseStartStop(); });
    mCommitCallbackRegistrar.add("Poser.ToggleLoadSavePanel", [this](LLUICtrl*, const LLSD&) { onToggleLoadSavePanel(); });
    mCommitCallbackRegistrar.add("Poser.ToggleVisualManipulators", [this](LLUICtrl*, const LLSD&) { onToggleVisualManipulators(); });
    mCommitCallbackRegistrar.add("Poser.ToggleIKTool", [this](LLUICtrl* ctrl, const LLSD&) { onToggleIKTool(ctrl); }); // <plus>
    mCommitCallbackRegistrar.add("Poser.ClearIKPins", [this](LLUICtrl*, const LLSD&) { onClearIKPins(); }); // <plus>
    mCommitCallbackRegistrar.add("Poser.ToggleRotationFrame", [this](LLUICtrl* button, const LLSD&) { onToggleRotationFrameButton(button); });

    mCommitCallbackRegistrar.add("Poser.UndoLastRotation", [this](LLUICtrl*, const LLSD&) { onUndoLastChange(); });
    mCommitCallbackRegistrar.add("Poser.RedoLastRotation", [this](LLUICtrl*, const LLSD&) { onRedoLastChange(); });
    mCommitCallbackRegistrar.add("Poser.ToggleMirrorChanges", [this](LLUICtrl*, const LLSD&) { onToggleMirrorChange(); });
    mCommitCallbackRegistrar.add("Poser.ToggleSympatheticChanges", [this](LLUICtrl*, const LLSD&) { onToggleSympatheticChange(); });
    mCommitCallbackRegistrar.add("Poser.AdjustTrackPadSensitivity", [this](LLUICtrl*, const LLSD&) { onAdjustTrackpadSensitivity(); });

    mCommitCallbackRegistrar.add("Poser.PositionSet", [this](LLUICtrl*, const LLSD&) { onPositionSet(); });
    mCommitCallbackRegistrar.add("Poser.SetToTPose", [this](LLUICtrl*, const LLSD&) { onSetAvatarToTpose(); });

    mCommitCallbackRegistrar.add("Poser.Advanced.ScaleSet", [this](LLUICtrl*, const LLSD&) { onScaleSet(); });
    mCommitCallbackRegistrar.add("Poser.UndoLastPosition", [this](LLUICtrl*, const LLSD&) { onUndoLastChange(); });
    mCommitCallbackRegistrar.add("Poser.RedoLastPosition", [this](LLUICtrl*, const LLSD&) { onRedoLastChange(); });
    mCommitCallbackRegistrar.add("Poser.ResetJoint", [this](LLUICtrl*, const LLSD& data) { onResetJoint(data); });

    mCommitCallbackRegistrar.add("Poser.Save", [this](LLUICtrl*, const LLSD&) { onClickPoseSave(); });
    mCommitCallbackRegistrar.add("Pose.Menu", [this](LLUICtrl*, const LLSD& data) { onPoseMenuAction(data); });
    mCommitCallbackRegistrar.add("Poser.BrowseCache", [this](LLUICtrl*, const LLSD&) { onClickBrowsePoseCache(); });
    mCommitCallbackRegistrar.add("Poser.LoadLeftHand", [this](LLUICtrl*, const LLSD&) { onClickLoadLeftHandPose(); });
    mCommitCallbackRegistrar.add("Poser.LoadRightHand", [this](LLUICtrl*, const LLSD&) { onClickLoadRightHandPose(); });
    // <plus> hand tab "Easy" (research/315)
    mCommitCallbackRegistrar.add("Poser.HandEasy", [this](LLUICtrl* ctrl, const LLSD& data) { onHandEasyCommit(ctrl, data.asString()); });
    mCommitCallbackRegistrar.add("Poser.HandEasySide", [this](LLUICtrl*, const LLSD&) { refreshHandEasy(); });
    mCommitCallbackRegistrar.add("Poser.HandEasyCopy", [this](LLUICtrl*, const LLSD& data) { onHandEasyCopy(data.asString() == "right"); });
    mCommitCallbackRegistrar.add("Poser.HandEasySave", [this](LLUICtrl*, const LLSD&) { onHandEasySave(); });
    // </plus>

    mCommitCallbackRegistrar.add("Poser.FlipPose", [this](LLUICtrl*, const LLSD&) { onClickFlipPose(); });
    mCommitCallbackRegistrar.add("Poser.FlipJoint", [this](LLUICtrl*, const LLSD&) { onClickFlipSelectedJoints(); });
    mCommitCallbackRegistrar.add("Poser.RecaptureSelectedBones", [this](LLUICtrl*, const LLSD&) { onClickRecaptureSelectedBones(); });
    mCommitCallbackRegistrar.add("Poser.TogglePosingSelectedBones", [this](LLUICtrl*, const LLSD&) { onClickToggleSelectedBoneEnabled(); });

    mCommitCallbackRegistrar.add("Poser.CommitSpinner", [this](LLUICtrl* spinner, const LLSD& data) { onCommitSpinner(spinner, data); });
    mCommitCallbackRegistrar.add("Poser.CommitSlider", [this](LLUICtrl* slider, const LLSD& data) { onCommitSlider(slider, data); });
    mCommitCallbackRegistrar.add("Poser.Symmetrize", [this](LLUICtrl*, const LLSD& data) { onClickSymmetrize(data); });

    mLoadPoseTimer = new FSLoadPoseTimer(boost::bind(&FSFloaterPoser::timedReload, this));
}

bool FSFloaterPoser::postBuild()
{
    mAvatarTrackball = getChild<FSVirtualTrackpad>("limb_rotation");
    mAvatarTrackball->setCommitCallback([this](LLUICtrl *, const LLSD &) { onTrackballChanged(); });

    mJointsTabs = getChild<LLTabContainer>("joints_tabs");
    mJointsTabs->setCommitCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            onJointTabSelect();
            setRotationChangeButtons(false, false);
        });

    mAvatarSelectionScrollList = getChild<LLScrollListCtrl>("avatarSelection_scroll");
    mAvatarSelectionScrollList->setCommitOnSelectionChange(true);
    mAvatarSelectionScrollList->setCommitCallback([this](LLUICtrl *, const LLSD &) { onAvatarSelect(); });

    mBodyJointsScrollList = getChild<LLScrollListCtrl>("body_joints_scroll");
    mBodyJointsScrollList->setCommitOnSelectionChange(true);
    mBodyJointsScrollList->setCommitCallback([this](LLUICtrl *, const LLSD &) { onJointTabSelect(); });

    mFaceJointsScrollList = getChild<LLScrollListCtrl>("face_joints_scroll");
    mFaceJointsScrollList->setCommitOnSelectionChange(true);
    mFaceJointsScrollList->setCommitCallback([this](LLUICtrl *, const LLSD &) { onJointTabSelect(); });

    mHandJointsScrollList = getChild<LLScrollListCtrl>("hand_joints_scroll");
    mHandJointsScrollList->setCommitOnSelectionChange(true);
    mHandJointsScrollList->setCommitCallback([this](LLUICtrl *, const LLSD &) { onJointTabSelect(); });

    mMiscJointsScrollList = getChild<LLScrollListCtrl>("misc_joints_scroll");
    mMiscJointsScrollList->setCommitOnSelectionChange(true);
    mMiscJointsScrollList->setCommitCallback([this](LLUICtrl *, const LLSD &) { onJointTabSelect(); });

    mCollisionVolumesScrollList = getChild<LLScrollListCtrl>("collision_volumes_scroll");
    mCollisionVolumesScrollList->setCommitOnSelectionChange(true);
    mCollisionVolumesScrollList->setCommitCallback([this](LLUICtrl *, const LLSD &) { onJointTabSelect(); });

    mEntireAvJointScroll = getChild<LLScrollListCtrl>("entireAv_joint_scroll");

    mPosesScrollList = getChild<LLScrollListCtrl>("poses_scroll");
    mPosesScrollList->setCommitOnSelectionChange(true);
    mPosesScrollList->setCommitCallback([this](LLUICtrl *, const LLSD &) { onPoseFileSelect(); });

    mToggleVisualManipulators = getChild<LLButton>("toggleVisualManipulators");
    mToggleVisualManipulators->setToggleState(true);
    mToggleIKToolBtn  = getChild<LLButton>("toggle_IKTool");   // <plus>
    mIKToolCheckbox   = getChild<LLUICtrl>("ik_tool_checkbox"); // <plus>

    mTrackpadSensitivitySlider = getChild<LLSliderCtrl>("trackpad_sensitivity_slider");

    mPoseSaveNameEditor = getChild<LLLineEditor>("pose_save_name");
    mPoseSaveNameEditor->setPrevalidate(&LLTextValidate::validateASCIIPrintableNoPipe);

    mHandPresetsScrollList = getChild<LLScrollListCtrl>("hand_presets_scroll");
    // <plus> a click on a preset applies it to the hand last chosen with the Set buttons. The mouse-up signal, not the commit
    // (which also comes from the arrow keys): moving the selection with the keyboard must not apply anything.
    mHandPresetsScrollList->setMouseUpCallback([this](LLUICtrl*, S32 x, S32 y, MASK mask) { onHandPresetClick(x, y, mask); });
    // The right button opens the menu (the list calls this only for a click on a row); it applies nothing: the mouse-up
    // signal above is the left button's.
    mHandPresetsScrollList->setRightMouseDownCallback([this](LLUICtrl*, S32 x, S32 y, MASK) { onHandPresetRightClick(x, y); });
    mHandPresetLeftBtn  = getChild<LLButton>("button_loadHandPoseLeft");
    mHandPresetRightBtn = getChild<LLButton>("button_loadHandPoseRight");
    // </plus>

    mPosXSlider = getChild<LLSliderCtrl>("av_position_inout");
    mPosYSlider = getChild<LLSliderCtrl>("av_position_leftright");
    mPosZSlider = getChild<LLSliderCtrl>("av_position_updown");

    mAdvRotXSlider = getChild<LLSliderCtrl>("limb_pitch_slider");
    mAdvRotYSlider = getChild<LLSliderCtrl>("limb_yaw_slider");
    mAdvRotZSlider = getChild<LLSliderCtrl>("limb_roll_slider");

    mAdvPosXSlider = getChild<LLSliderCtrl>("Advanced_Position_X");
    mAdvPosYSlider = getChild<LLSliderCtrl>("Advanced_Position_Y");
    mAdvPosZSlider = getChild<LLSliderCtrl>("Advanced_Position_Z");

    mAdvScaleXSlider = getChild<LLSliderCtrl>("Advanced_Scale_X");
    mAdvScaleYSlider = getChild<LLSliderCtrl>("Advanced_Scale_Y");
    mAdvScaleZSlider = getChild<LLSliderCtrl>("Advanced_Scale_Z");

    mPosesLoadSavePnl = getChild<LLPanel>("poses_loadSave");
    mStartStopPosingBtn = getChild<LLButton>("start_stop_posing_button");
    mToggleLoadSavePanelBtn = getChild<LLButton>("toggleLoadSavePanel");
    mBrowserFolderBtn = getChild<LLButton>("open_poseDir_button");
    mLoadPosesBtn = getChild<LLButton>("load_poses_button");
    mSavePosesBtn = getChild<LLButton>("save_poses_button");
    mSavePosesBtn->setMouseLeaveCallback([this](LLUICtrl*, const LLSD&) { onMouseLeaveSavePoseBtn(); });

    mFlipPoseBtn = getChild<LLButton>("FlipPose_avatar");
    mFlipJointBtn = getChild<LLButton>("FlipJoint_avatar");
    mRecaptureBtn = getChild<LLButton>("button_RecaptureParts");
    mTogglePosingBonesBtn = getChild<LLButton>("toggle_PosingSelectedBones");
    mToggleLockWorldRotBtn = getChild<LLButton>("toggle_LockWorldRotation");
    mToggleLockWorldRotBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClickLockWorldRotBtn(); });

    mToggleMirrorRotationBtn = getChild<LLButton>("button_toggleMirrorRotation");
    mToggleSympatheticRotationBtn = getChild<LLButton>("button_toggleSympatheticRotation");
    mToggleDeltaModeBtn = getChild<LLButton>("delta_mode_toggle");
    mRedoChangeBtn = getChild<LLButton>("button_redo_change");
    mUndoChangeBtn = getChild<LLButton>("undo_change");
    mSetToTposeButton = getChild<LLButton>("set_t_pose_button");
    mBtnJointReset = getChild<LLButton>("poser_joint_reset");

    mBtnWorldFrame = getChild<LLButton>("poser_world_frame_toggle");
    mBtnAvatarFrame = getChild<LLButton>("poser_avatar_frame_toggle");
    mBtnScreenFrame = getChild<LLButton>("poser_screen_frame_toggle");

    mJointsParentPnl = getChild<LLPanel>("joints_parent_panel");
    mTrackballPnl = getChild<LLPanel>("trackball_panel");
    mPositionPnl = getChild<LLPanel>("position_panel");
    mMoveTabPnl = getChild<LLPanel>("move_tab_panel");
    mTrackballButtonPnl = getChild<LLPanel>("trackball_button_panel");
    mPositionRotationPnl = getChild<LLPanel>("positionRotation_panel");
    mBodyJointsPnl = getChild<LLPanel>("body_joints_panel");
    mFaceJointsPnl = getChild<LLPanel>("face_joints_panel");
    mHandsTabs = getChild<LLTabContainer>("hands_tabs");
    mHandsJointsPnl = mHandsTabs->getChild<LLPanel>("hands_joints_panel");
    // <plus> hand tab "Easy"
    mHandEasyPnl = mHandsTabs->getChild<LLPanel>("hands_easy_panel");
    mHandEasySideCmb = mHandEasyPnl->getChild<LLUICtrl>("he_side");
    mHandEasySaveNameEditor = mHandEasyPnl->getChild<LLUICtrl>("he_save_name");
    {
        static const char* const FINGER_IDS[plus::handcurl::FINGER_COUNT] = { "index", "middle", "ring", "pinky" };
        for (S32 i = 0; i < plus::handcurl::FINGER_COUNT; ++i)
        {
            mHandEasyCurl[i]       = mHandEasyPnl->getChild<LLSliderCtrl>(std::string("he_curl_") + FINGER_IDS[i]);
            mHandEasySpread[i]     = mHandEasyPnl->getChild<LLSliderCtrl>(std::string("he_spread_") + FINGER_IDS[i]);
            mHandEasyCurlLabel[i]  = mHandEasyPnl->getChild<LLTextBox>(std::string("he_curl_label_") + FINGER_IDS[i]);
            mHandEasySpreadLabel[i] = mHandEasyPnl->getChild<LLTextBox>(std::string("he_spread_label_") + FINGER_IDS[i]);
        }
    }
    mHandEasyThumbWrap       = mHandEasyPnl->getChild<LLSliderCtrl>("he_thumb_wrap");
    mHandEasyThumbSwing      = mHandEasyPnl->getChild<LLSliderCtrl>("he_thumb_swing");
    mHandEasyThumbArch       = mHandEasyPnl->getChild<LLSliderCtrl>("he_thumb_arch");
    mHandEasyThumbBend       = mHandEasyPnl->getChild<LLSliderCtrl>("he_thumb_bend");
    mHandEasyThumbLabel[0]   = mHandEasyPnl->getChild<LLTextBox>("he_thumb_wrap_label");
    mHandEasyThumbLabel[1]   = mHandEasyPnl->getChild<LLTextBox>("he_thumb_swing_label");
    mHandEasyThumbLabel[2]   = mHandEasyPnl->getChild<LLTextBox>("he_thumb_arch_label");
    mHandEasyThumbLabel[3]   = mHandEasyPnl->getChild<LLTextBox>("he_thumb_bend_label");
    mHandEasyGrip            = mHandEasyPnl->getChild<LLSliderCtrl>("he_grip");
    mHandEasyFan             = mHandEasyPnl->getChild<LLSliderCtrl>("he_fan");
    mHandsTabs->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshHandEasy(); });
    // </plus>
    mMiscJointsPnl = getChild<LLPanel>("misc_joints_panel");
    mCollisionVolumesPnl = getChild<LLPanel>("collision_volumes_panel");

    mUnlockPelvisInBvhSaveCbx = getChild<LLCheckBoxCtrl>("unlock_pelvis_for_bvh_save_checkbox");
    mUnlockPelvisInBvhSaveCbx->setVisible(getSavingToBvh());
    mAlsoSaveBvhCbx = getChild<LLCheckBoxCtrl>("also_save_bvh_checkbox");
    mAlsoSaveBvhCbx->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickSavingToBvh(); });

    mTrackpadSensitivitySpnr = getChild<LLUICtrl>("trackpad_sensitivity_spinner");
    mYawSpnr                 = getChild<LLUICtrl>("limb_yaw_spinner");
    mYawSpnr->setCommitCallback([this](LLUICtrl*, const LLSD&) { onYawPitchRollChanged(); });
    mPitchSpnr               = getChild<LLUICtrl>("limb_pitch_spinner");
    mPitchSpnr->setCommitCallback([this](LLUICtrl*, const LLSD&) { onYawPitchRollChanged(); });
    mRollSpnr = getChild<LLUICtrl>("limb_roll_spinner");
    mRollSpnr->setCommitCallback([this](LLUICtrl*, const LLSD&) { onYawPitchRollChanged(); });
    mUpDownSpnr              = getChild<LLUICtrl>("av_position_updown_spinner");
    mLeftRightSpnr           = getChild<LLUICtrl>("av_position_leftright_spinner");
    mInOutSpnr               = getChild<LLUICtrl>("av_position_inout_spinner");
    mAdvPosXSpnr             = getChild<LLUICtrl>("adv_posx_spinner");
    mAdvPosYSpnr             = getChild<LLUICtrl>("adv_posy_spinner");
    mAdvPosZSpnr             = getChild<LLUICtrl>("adv_posz_spinner");
    mScaleXSpnr              = getChild<LLUICtrl>("adv_scalex_spinner");
    mScaleYSpnr              = getChild<LLUICtrl>("adv_scaley_spinner");
    mScaleZSpnr              = getChild<LLUICtrl>("adv_scalez_spinner");

    return true;
}

void FSFloaterPoser::onOpen(const LLSD& key)
{
    createUserPoseDirectoryIfNeeded();
    onAvatarsRefresh();
    refreshJointScrollListMembers();
    onJointTabSelect();
    refreshPoseScroll(mHandPresetsScrollList, POSE_PRESETS_HANDS_SUBDIRECTORY);
    startPosingSelf();

    // <plus> opened again while it is open (showInstance): the switch decides, so that the toolset of the
    // poser is not taken for the one to go back to (mLastToolset, see enableVisualManipulators)
    if (mToggleVisualManipulators->getValue().asBoolean())
        enableVisualManipulators();
    // </plus>
    LLFloater::onOpen(key);
}

void FSFloaterPoser::onFocusReceived()
{
    LLEditMenuHandler::gEditMenuHandler = this;
}

void FSFloaterPoser::onFocusLost()
{
    if (LLEditMenuHandler::gEditMenuHandler == this)
        LLEditMenuHandler::gEditMenuHandler = nullptr;
}

void FSFloaterPoser::draw()
{
    LLFloater::draw();
    refreshIKPinsText(); // <plus>

    drawOnHoverJointHint();
}

void FSFloaterPoser::markSelectedJointsToHighlight()
{
    bool toolsEnabled = mToggleVisualManipulators->getValue().asBoolean();
    if (toolsEnabled)
        return;

    bool showHighlights = gSavedSettings.getBOOL(POSER_SHOWBONEHIGHLIGHTS_SAVE_KEY);
    if (!showHighlights)
        return;

    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return;

    std::string jointName   = selectedJoints[0]->jointName();
    bool        isRightLimb = jointName.find("Right") != std::string::npos;
    bool        isLeftLimb  = jointName.find("Left") != std::string::npos;

    if (!(isRightLimb || isLeftLimb))
        return;

    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    mLastSelectedJoint = selectedJoints[0];
    timeFadeStartedMicrosec = gFrameTime;
}

void FSFloaterPoser::drawOnHoverJointHint()
{
    if (!mLastSelectedJoint)
        return;

    constexpr U64 GLOW_TIME_US = 300000;
    U64           fadeTimeUs   = gFrameTime - timeFadeStartedMicrosec;
    if (fadeTimeUs > GLOW_TIME_US)
        return;

    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    LLJoint* joint = avatar->getJoint(std::string(mLastSelectedJoint->jointName()));
    if (!joint)
        return;

    F32              alphaFade            = 1.f * (GLOW_TIME_US - fadeTimeUs) / GLOW_TIME_US;
    // Port note: AreaSearchBeaconColor is a Firestorm colour; fall back to the same blue.
    static LLUIColor mBeaconColor         = LLUIColorTable::getInstance()->getColor("AreaSearchBeaconColor", LLColor4(0.f, 0.f, 1.f, 0.5f));
    LLColor4         beaconColour         = mBeaconColor.get();
    beaconColour.setAlpha(alphaFade);
    LLVector3        joint_world_position = joint->getWorldPosition();

    static LLCachedControl<S32> beacon_line_width(gSavedSettings, "DebugBeaconLineWidth");
    gObjectList.addDebugBeacon(joint_world_position, "", beaconColour, beaconColour, beacon_line_width);
}

void FSFloaterPoser::enableVisualManipulators()
{
    if (!gAgentAvatarp || gAgentAvatarp.isNull())
    {
        mToggleVisualManipulators->setToggleState(false);
        refreshIKToolButton(); // <plus>
        return;
    }

    if (LLToolMgr::getInstance()->getCurrentToolset() != gCameraToolset &&
        LLToolMgr::getInstance()->getCurrentToolset() != getPoserToolset()) // <plus> never the poser's own
        mLastToolset = LLToolMgr::getInstance()->getCurrentToolset();

    LLToolMgr::getInstance()->setCurrentToolset(getPoserToolset());
    selectPoserTool(); // <plus> the IK tool while it is on, else the rotate one
    refreshIKToolButton(); // <plus>

    LLVOAvatar* avatar = getUiSelectedAvatar();
    setVisualManipulators(avatar);
}

void FSFloaterPoser::disableVisualManipulators()
{
    // Port note: Firestorm restores mLastToolset unconditionally and keeps the
    // poser toolset when it was entered from the camera toolset (mLastToolset
    // unset). Only leave the poser toolset, and fall back to the basic one.
    LLToolMgr* toolMgr = LLToolMgr::getInstance();
    if (toolMgr->getCurrentToolset() == getPoserToolset())
        toolMgr->setCurrentToolset(mLastToolset ? mLastToolset : gBasicToolset);

    setVisualManipulators(nullptr);
    refreshIKToolButton(); // <plus>
}

void FSFloaterPoser::setVisualManipulators(LLVOAvatar* avatar)
{
    FSToolCompPose::getInstance()->setAvatar(avatar);
    FSToolCompPoseTranslate::getInstance()->setAvatar(avatar);
    FSToolCompPoseIK::getInstance()->setAvatar(avatar); // <plus>

    if (!avatar)
        return;

    // <plus> The rotate guide takes the joint selected in the list of the tab on view. The settings and model tabs
    // have no list: then the list of the tab that had one last is used, and with none the joint is left as it is
    // (an empty selection would clear it, and the guide would be gone until a joint is picked again).
    if (getScrollListForTab(mJointsTabs->getCurrentPanel()))
    {
        auto selectedJoints = getUiSelectedPoserJoints();
        updateManipWithFirstSelectedJoint(selectedJoints);
    }
    else if (mLastJointListTab)
    {
        updateManipWithFirstSelectedJoint(getSelectedPoserJointsOfTab(mLastJointListTab));
    }
    // </plus>
}

// <plus>
void FSFloaterPoser::rememberJointListTab()
{
    LLPanel* tab = mJointsTabs->getCurrentPanel();
    if (getScrollListForTab(tab))
        mLastJointListTab = tab;
}

std::vector<FSPoserAnimator::FSPoserJoint*> FSFloaterPoser::getSelectedPoserJointsOfTab(LLPanel* tab) const
{
    std::vector<FSPoserAnimator::FSPoserJoint*> joints;
    LLScrollListCtrl* scrollList = getScrollListForTab(tab);
    if (!scrollList)
        return joints;
    for (auto item : scrollList->getAllSelected())
    {
        if (auto* userData = static_cast<FSPoserAnimator::FSPoserJoint*>(item->getUserdata()))
            joints.push_back(userData);
    }
    return joints;
}
// </plus>

void FSFloaterPoser::onClose(bool app_quitting)
{
    if (gSavedSettings.getBOOL(POSER_STOPPOSINGWHENCLOSED_SAVE_KEY))
    {
        stopPosingAllAvatars();
    }

    disableVisualManipulators();
    // <plus> the hand preset menu is a child of the menu holder, not of this floater: it goes with the floater's closing
    if (LLContextMenu* menu = mHandPresetMenuHandle.get())
    {
        menu->die();
        mHandPresetMenuHandle.markDead();
    }
    // </plus>
    delete mLoadPoseTimer;
    LLFloater::onClose(app_quitting);
}

void FSFloaterPoser::refreshPoseScroll(LLScrollListCtrl* posesScrollList, std::optional<std::string_view> subDirectory)
{
    if (!posesScrollList)
        return;

    posesScrollList->clearRows();

    std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY);
    if (subDirectory.has_value())
        gDirUtilp->append(dir, std::string(subDirectory.value()));

    static const std::string date_format_str = getString(gSavedSettings.getBOOL("Use24HourClock") ? "FiledateFormat" : "FiledateFormatAMPM");
    std::string   file;
    llstat        stat_data;
    time_t        last_modified = 0;
    LLDirIterator dir_iter(dir, POSE_INTERNAL_FORMAT_FILE_MASK);
    while (dir_iter.next(file))
    {
        std::string path = gDirUtilp->add(dir, file);
        // Port note: Firestorm shows the name unescaped, but saves it as typed (no escape): a pose
        // saved as "a%20b" was listed as "a b" and could not be loaded. The file's name as it is,
        // under the rule of loading (plus::isLoadableFileStem, looser than saving's so that poses
        // saved by Firestorm, "Sit..2" or a long Japanese name, still show; the MCP's list_poses
        // lists the same).
        // A file named just ".xml" keeps its extension (getBaseFileName leaves a name that starts
        // with its only dot), and ".xml.xml" would be read: not listed.
        std::string name = gDirUtilp->getBaseFileName(file, true);
        if (name == file || !plus::isLoadableFileStem(name))
        {
            LL_WARNS("Posing") << "Skipping: not a name a pose can be loaded by: " << file << LL_ENDL;
            continue;
        }

        if (LLFile::stat(path, &stat_data))
            last_modified = 0;
        else
            last_modified = stat_data.st_mtime;

        std::string date_str = date_format_str;
        LLSD substitution;
        substitution["datetime"] = (F64)last_modified;
        LLStringUtil::format(date_str, substitution);

        LLSD row;
        row["columns"][0]["column"] = "load_file_name_column";
        // <plus> the file's name stays the row's value; hand presets show a translated display name
        row["value"] = name;
        row["columns"][0]["value"] = (subDirectory.has_value() && *subDirectory == POSE_PRESETS_HANDS_SUBDIRECTORY) ? handPresetDisplayName(name) : name;
        // </plus>
        row["columns"][1]["column"] = "load_file_date_column";
        row["columns"][1]["value"] = date_str;
        row["columns"][2]["column"] = "load_file_date_sort_column";
        row["columns"][2]["value"] = llformat("%u", (U64)last_modified);

        llifstream infile(path.c_str());
        if (!infile.is_open())
        {
            LL_WARNS("Posing") << "Skipping: Cannot read file in: " << path << LL_ENDL;
            continue;
        }

        LLSD data;
        if (LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(data, infile))
        {
            LL_WARNS("Posing") << "Skipping: Failed to parse pose file: " << path << LL_ENDL;
            continue;
        }

        posesScrollList->addElement(row);
    }
}

void FSFloaterPoser::onPoseFileSelect()
{
    LLScrollListItem* item = mPosesScrollList->getFirstSelected();
    if (!item)
        return;

    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!havePermissionToAnimateAvatar(avatar) && !havePermissionToAnimateOtherAvatar(avatar))
        return;

    bool enableButtons = mPoserAnimator.isPosingAvatar(avatar);
    mLoadPosesBtn->setEnabled(enableButtons);
    mSavePosesBtn->setEnabled(enableButtons);

    std::string poseName = item->getColumn(0)->getValue().asString();
    if (poseName.empty())
        return;

    LLStringExplicit name = LLStringExplicit(poseName);
    mPoseSaveNameEditor->setEnabled(enableButtons);
    mPoseSaveNameEditor->setText(name);

    bool isDeltaSave = !poseFileStartsFromTeePose(name);
    if (isDeltaSave)
        mLoadPosesBtn->setLabel(tryGetString("LoadDiffLabel"));
    else
        mLoadPosesBtn->setLabel(tryGetString("LoadPoseLabel"));
}

void FSFloaterPoser::doPoseSave(LLVOAvatar* avatar, const std::string& filename)
{
    if (!avatar)
        return;

    bool successfulSave = savePoseToXml(avatar, filename);
    if (successfulSave)
    {
        refreshPoseScroll(mPosesScrollList);
        setUiSelectedAvatarSaveFileName(filename);

        if (getSavingToBvh())
            savePoseToBvh(avatar, filename);

        mSavePosesBtn->setImageOverlay(tryGetString(ICON_SAVE_OK), mSavePosesBtn->getImageOverlayHAlign());
        setSavePosesButtonText(!mPoserAnimator.allBaseRotationsAreZero(avatar));
    }
    else
    {
         mSavePosesBtn->setImageOverlay(tryGetString(ICON_SAVE_FAILED), mSavePosesBtn->getImageOverlayHAlign());
    }
}

void FSFloaterPoser::onClickPoseSave()
{
    // Port note: Firestorm puts the typed name into the path as it is, so "..\x" was written
    // outside the poses folder. The rule of saving is the MCP's save_pose's too (plus::isSafeFileStem).
    std::string filename = mPoseSaveNameEditor->getValue().asString();
    if (!plus::isSafeFileStem(filename))
    {
        mSavePosesBtn->setImageOverlay(tryGetString(ICON_SAVE_FAILED), mSavePosesBtn->getImageOverlayHAlign());
        return;
    }

    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!havePermissionToAnimateAvatar(avatar) && !havePermissionToAnimateOtherAvatar(avatar))
    {
        mSavePosesBtn->setImageOverlay(tryGetString(ICON_SAVE_FAILED), mSavePosesBtn->getImageOverlayHAlign());
        return;
    }

    // if prompts are disabled or file doesn't exist, do the save immediately:
    const bool prompt = gSavedSettings.getBOOL(POSER_SAVECONFIRMREQUIRED_SAVE_KEY);

    std::string fullPath = gDirUtilp->getExpandedFilename(
        LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY, filename + POSE_INTERNAL_FORMAT_FILE_EXT);
    const bool exists = gDirUtilp->fileExists(fullPath);

    if (!prompt || !exists)
    {
        // new file or no overwrite guard
        doPoseSave(avatar, filename);
    }
    else
    {
        // show a modal dialog, passing the pose name along
        // Port note: Firestorm has its own ConfirmPoserOverwrite notification; use the
        // official generic one with the text from this floater's XUI instead.
        std::string message = tryGetString("confirm_overwrite_message");
        LLStringUtil::format_map_t format_args;
        format_args["[POSE_NAME]"] = filename;
        LLStringUtil::format(message, format_args);

        LLSD args;
        args["MESSAGE"] = message;

        LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(), // no payload
            [this, avatar, filename](const LLSD& notification, const LLSD& response)
            {
                if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                {
                    // user clicked “Yes”
                    doPoseSave(avatar, filename);
                }
                // else do nothing (cancel)
            });
    }
}

void FSFloaterPoser::onMouseLeaveSavePoseBtn()
{
    mSavePosesBtn->setImageOverlay(tryGetString("icon_save_button"), mSavePosesBtn->getImageOverlayHAlign());

    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    setSavePosesButtonText(!mPoserAnimator.allBaseRotationsAreZero(avatar));
}

void FSFloaterPoser::createUserPoseDirectoryIfNeeded()
{
    std::string userPath =
        gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY);

    std::string userHandPresetsPath = userPath + gDirUtilp->getDirDelimiter() + std::string(POSE_PRESETS_HANDS_SUBDIRECTORY);

    try
    {
        if (!gDirUtilp->fileExists(userHandPresetsPath))
        {
            if (!gDirUtilp->fileExists(userPath))
            {
                LL_WARNS("Poser") << "Couldn't find folder: " << userPath << " - creating one." << LL_ENDL;
                LLFile::mkdir(userPath);
            }

            if (!gDirUtilp->fileExists(userHandPresetsPath))
            {
                LL_WARNS("Poser") << "Couldn't find folder: " << userHandPresetsPath << " - creating one." << LL_ENDL;
                LLFile::mkdir(userHandPresetsPath);
            }
        }

        std::string sourcePresetPath =
            gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, POSE_PRESETS_SOURCE_DIR_PLUS, POSE_PRESETS_SOURCE_DIR_FEATURE,
                                           std::string(POSE_PRESETS_HANDS_SUBDIRECTORY));

        if (!gDirUtilp->fileExists(sourcePresetPath))
        {
            LL_WARNS("Poser") << "Can not copy poser presets because failed to find path: " << sourcePresetPath << LL_ENDL;
            return;
        }

        auto posesToCopy = gDirUtilp->getFilesInDir(sourcePresetPath);
        for (const auto& pose : posesToCopy)
        {
            std::string source      = sourcePresetPath + gDirUtilp->getDirDelimiter() + pose;
            std::string destination = userHandPresetsPath + gDirUtilp->getDirDelimiter() + pose;

            S32 sourceVersion = tryGetPoseVersion(source);
            S32 destinationVersion = tryGetPoseVersion(destination);
            if (destinationVersion >= sourceVersion)
                continue;

            if (gDirUtilp->fileExists(destination))
            {
                LL_WARNS("Poser") << "Removing pose file " << destination << " to replace with updated version " << LL_ENDL;
                LLFile::remove(destination);
            }

            if (!LLFile::copy(source, destination))
                LL_WARNS("Poser") << "Failed to copy " << source << " to " << destination << LL_ENDL;
        }
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Posing") << "Exception caught trying to create/update poses: " << e.what() << LL_ENDL;
    }
}

bool FSFloaterPoser::savePoseToXml(LLVOAvatar* avatar, const std::string& poseFileName)
{
    if (poseFileName.empty())
        return false;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return false;

    createUserPoseDirectoryIfNeeded();

    try
    {
        bool savingDiff = !mPoserAnimator.allBaseRotationsAreZero(avatar);
        LLSD record;
        record["version"]["value"] = (S32)8;
        record["startFromTeePose"]["value"] = !savingDiff;

        if (savingDiff)
            mPoserAnimator.savePosingState(avatar, false, &record);

        LLVector3 rotation, position, scale, zeroVector;
        bool      baseRotationIsZero, userSetBaseRotationToZero;

        for (const FSPoserAnimator::FSPoserJoint& pj : mPoserAnimator.PoserJoints)
        {
            std::string bone_name = pj.jointName();
            bool posingThisJoint  = mPoserAnimator.isPosingAvatarJoint(avatar, pj);
            bool jointRotLocked   = mPoserAnimator.getRotationIsWorldLocked(avatar, pj);
            bool jointRotMirrored = mPoserAnimator.getRotationIsMirrored(avatar, pj);

            record[bone_name]             = bone_name;
            record[bone_name]["enabled"]  = posingThisJoint;
            if (!posingThisJoint)
                continue;

            record[bone_name]["mirrored"] = jointRotMirrored;

            if (!mPoserAnimator.tryGetJointSaveVectors(avatar, pj, &rotation, &position, &scale, &baseRotationIsZero,
                                                       &userSetBaseRotationToZero))
                continue;

            bool jointRotPosScaleAllZero = rotation == zeroVector && position == zeroVector && scale == zeroVector;

            if (savingDiff && jointRotPosScaleAllZero)
                continue;

            record[bone_name]["jointBaseRotationIsZero"]   = baseRotationIsZero;
            record[bone_name]["userSetBaseRotationToZero"] = userSetBaseRotationToZero;
            record[bone_name]["rotation"]                  = rotation.getValue();
            record[bone_name]["position"]                  = position.getValue();
            record[bone_name]["scale"]                     = scale.getValue();
            record[bone_name]["worldLocked"]               = jointRotLocked;
        }

        std::string fullSavePath =
            gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY, poseFileName + POSE_INTERNAL_FORMAT_FILE_EXT);

        // Port note: Firestorm writes over the pose file directly, so a full disk left a cut-off
        // file in place of the old pose. Written next to it, checked, then moved over it.
        const std::string tempSavePath = fullSavePath + ".tmp";
        {
            llofstream file;
            file.open(tempSavePath.c_str());
            if (!file.is_open())
            {
                LL_WARNS("Poser") << "Unable to save pose!" << LL_ENDL;
                return false;
            }
            LLSDSerialize::toPrettyXML(record, file);
            file.close();
            if (file.fail())
            {
                LL_WARNS("Poser") << "Unable to save pose: cannot write " << tempSavePath << LL_ENDL;
                LLFile::remove(tempSavePath);
                return false;
            }
        }
        if (LLFile::rename(tempSavePath, fullSavePath) != 0)
        {
            LL_WARNS("Poser") << "Unable to save pose: cannot replace " << fullSavePath << LL_ENDL;
            LLFile::remove(tempSavePath);
            return false;
        }
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Posing") << "Exception caught in saveToXml: " << e.what() << LL_ENDL;
        return false;
    }

    return true;
}

void FSFloaterPoser::onClickToggleSelectedBoneEnabled()
{
    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return;

    LLVOAvatar *avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    for (auto item : selectedJoints)
    {
        bool currentlyPosing = mPoserAnimator.isPosingAvatarJoint(avatar, *item);
        mPoserAnimator.setPosingAvatarJoint(avatar, *item, !currentlyPosing);
    }

    refreshRotationSlidersAndSpinners();
    refreshTrackpadCursor();
    refreshTextHighlightingOnJointScrollLists();
}

void FSFloaterPoser::onClickFlipSelectedJoints()
{
    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return;

    LLVOAvatar *avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    for (auto item : selectedJoints)
    {
        // need to be posing the joint to flip
        bool currentlyPosingJoint = mPoserAnimator.isPosingAvatarJoint(avatar, *item);
        if (!currentlyPosingJoint)
            continue;

        auto oppositeJoint = mPoserAnimator.getPoserJointByName(item->mirrorJointName());
        if (oppositeJoint)
        {
            bool currentlyPosingOppositeJoint = mPoserAnimator.isPosingAvatarJoint(avatar, *oppositeJoint);
            if (!currentlyPosingOppositeJoint)
                continue;
        }

        // If one selects a joint and its opposite, this would otherwise flip each of them sequentially to yeild no net result.
        // This allows the user to select a joint and its opposite, or only select one joint, and get the same result.
        if (std::find(selectedJoints.begin(), selectedJoints.end(), oppositeJoint) != selectedJoints.end())
        {
            if (!item->dontFlipOnMirror())
                continue;
        }

        mPoserAnimator.reflectJoint(avatar, item);
    }

    refreshRotationSlidersAndSpinners();
    enableOrDisableRedoAndUndoButton();
    refreshTrackpadCursor();
}

void FSFloaterPoser::onClickFlipPose()
{
    LLVOAvatar *avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    mPoserAnimator.flipEntirePose(avatar);

    refreshRotationSlidersAndSpinners();
    refreshTrackpadCursor();
}

void FSFloaterPoser::onClickRecaptureSelectedBones()
{
    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return;

    LLVOAvatar *avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    mPoserAnimator.updatePosingState(avatar, selectedJoints);

    for (auto item : selectedJoints)
    {
        bool currentlyPosing = mPoserAnimator.isPosingAvatarJoint(avatar, *item);
        if (currentlyPosing)
            continue;

        mPoserAnimator.recaptureJoint(avatar, *item);
    }

    setSavePosesButtonText(true);
    refreshRotationSlidersAndSpinners();
    refreshPositionSlidersAndSpinners();
    refreshScaleSlidersAndSpinners();
    refreshTrackpadCursor();
    refreshTextHighlightingOnJointScrollLists();
    enableOrDisableRedoAndUndoButton();
}

void FSFloaterPoser::updatePosedBones(const std::string& jointName, const LLQuaternion& rotation, const LLVector3& position, const LLVector3& scale)
{
    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    bool haveImplicitPermission = havePermissionToAnimateAvatar(avatar); // self & control avatars you own
    bool iCanPoseThem           = havePermissionToAnimateOtherAvatar(avatar);
    if (!haveImplicitPermission && !iCanPoseThem)
        return;

    const FSPoserAnimator::FSPoserJoint* poserJoint = mPoserAnimator.getPoserJointByName(jointName);
    if (!poserJoint)
        return;

    bool                   savingToExternal = getSavingToBvh();
    E_PoserReferenceFrame  frame            = getReferenceFrame();
    E_BoneDeflectionStyles defl             = getUiSelectedBoneDeflectionStyle();
    mPoserAnimator.updateJointFromManip(avatar, poserJoint, savingToExternal, defl, frame, rotation, position, scale); 

    refreshRotationSlidersAndSpinners();
    refreshPositionSlidersAndSpinners();
    refreshScaleSlidersAndSpinners();
    refreshTrackpadCursor();
    enableOrDisableRedoAndUndoButton();
    refreshTextHighlightingOnJointScrollLists();
}

LLQuaternion FSFloaterPoser::getManipGimbalRotation(const std::string& jointName)
{
    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return LLQuaternion();

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return LLQuaternion();

    const FSPoserAnimator::FSPoserJoint* poserJoint = mPoserAnimator.getPoserJointByName(jointName);
    if (!poserJoint)
        return LLQuaternion();

    E_PoserReferenceFrame frame = getReferenceFrame();

    return mPoserAnimator.getManipGimbalRotation(avatar, poserJoint, frame);
}

void FSFloaterPoser::onClickBrowsePoseCache()
{
    createUserPoseDirectoryIfNeeded();

    std::string pathname = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY);
    plus::openFolder(pathname); // Firestorm: gViewerWindow->getWindow()->openFile(pathname)
}

void FSFloaterPoser::onClickSymmetrize(const S32 ID)
{
    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    mPoserAnimator.symmetrizeLeftToRightOrRightToLeft(avatar, ID == 2);

    refreshRotationSlidersAndSpinners();
    enableOrDisableRedoAndUndoButton();
    refreshTrackpadCursor();

    if (getSavingToBvh())
        refreshTextHighlightingOnJointScrollLists();
}

void FSFloaterPoser::onCommitSpinner(const LLUICtrl* spinner, const S32 id)
{
    if (!spinner)
        return;

    F32 value = (F32)spinner->getValue().asReal();

    switch (id)
    {
        case 0: // av_position_updown_spinner
        {
            mAdvPosZSpnr->setValue(value);
            onPositionSet();
            break;
        }
        case 1: // av_position_leftright
        {
            mAdvPosYSpnr->setValue(value);
            onPositionSet();
            break;
        }
        case 2: // av_position_inout_spinner
        {
            mAdvPosXSpnr->setValue(value);
            onPositionSet();
            break;
        }
        case 3: // trackpad_sensitivity_spinner
        {
            onAdjustTrackpadSensitivity();
            break;
        }
        case 7: // adv_posx_spinner
        case 8: // adv_posy_spinner
        case 9: // adv_posz_spinner
        {
            onPositionSet();
            break;
        }
        case 10: // adv_scalex_spinner
        case 11: // adv_scaley_spinner
        case 12: // adv_scalez_spinner
        {
            onScaleSet();
            break;
        }

        default:
            LL_WARNS("Posing") << "onCommitSpinner passed invalid parameter: " << id << LL_ENDL;
            break;
    }
}

void FSFloaterPoser::onCommitSlider(const LLUICtrl* slider, const S32 id)
{
    if (!slider)
        return;

    F32 value = (F32)slider->getValue().asReal();

    switch (id)
    {
        case 0: // av_position_updown
        case 9: // Advanced_Position_Z
        {
            mAdvPosZSpnr->setValue(value);
            onPositionSet();
            break;
        }

        case 1: // av_position_leftright
        case 8: // Advanced_Position_Y
        {
            mAdvPosYSpnr->setValue(value);
            onPositionSet();
            break;
        }

        case 2: // av_position_inout
        case 7: // Advanced_Position_X
        {
            mAdvPosXSpnr->setValue(value);
            onPositionSet();
            break;
        }

        case 4: // limb_pitch_slider
        {
            mPitchSpnr->setValue(value);
            onYawPitchRollChanged();
            break;
        }
        case 5: // limb_yaw_slider
        {
            mYawSpnr->setValue(value);
            onYawPitchRollChanged();
            break;
        }
        case 6: // limb_roll_slider
        {
            mRollSpnr->setValue(value);
            onYawPitchRollChanged();
            break;
        }

        case 10: // Advanced_Scale_X
        {
            mScaleXSpnr->setValue(value);
            onScaleSet();
            break;
        }
        case 11: // Advanced_Scale_Y
        {
            mScaleYSpnr->setValue(value);
            onScaleSet();
            break;
        }
        case 12: // Advanced_Scale_Z
        {
            mScaleZSpnr->setValue(value);
            onScaleSet();
            break;
        }

        default:
            LL_WARNS("Posing") << "onCommitSlider passed invalid parameter: " << id << LL_ENDL;
            break;
    }
}

void FSFloaterPoser::onPoseMenuAction(const LLSD& param)
{
    std::string loadStyle = param.asString();
    if (loadStyle.empty())
        return;

    LLScrollListItem* item = mPosesScrollList->getFirstSelected();
    if (!item)
        return;

    std::string poseName = item->getColumn(0)->getValue().asString();

    E_LoadPoseMethods loadType = ROT_POS_AND_SCALES; // the default is to load everything
    if (loadStyle == "rotation")
        loadType = ROTATIONS;
    else if (loadStyle == "position")
        loadType = POSITIONS;
    else if (loadStyle == "scale")
        loadType = SCALES;
    else if (loadStyle == "rot_pos")
        loadType = ROTATIONS_AND_POSITIONS;
    else if (loadStyle == "rot_scale")
        loadType = ROTATIONS_AND_SCALES;
    else if (loadStyle == "pos_scale")
        loadType = POSITIONS_AND_SCALES;
    else if (loadStyle == "all")
        loadType = ROT_POS_AND_SCALES;
    else if (loadStyle == "selective")
        loadType = SELECTIVE;
    else if (loadStyle == "selective_rot")
        loadType = SELECTIVE_ROT;

    mLoadPoseTimer->tryLoading(poseName, loadType);
    setLoadingProgress(true);
}

void FSFloaterPoser::timedReload()
{
    if (!mLoadPoseTimer)
        return;

    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!havePermissionToAnimateAvatar(avatar) && !havePermissionToAnimateOtherAvatar(avatar))
        return;

    if (loadPoseFromXml(avatar, mLoadPoseTimer->getPosePath(), mLoadPoseTimer->getLoadMethod()))
    {
        mLoadPoseTimer->completeLoading();
        onJointTabSelect();
        refreshJointScrollListMembers();
        setSavePosesButtonText(!mPoserAnimator.allBaseRotationsAreZero(avatar));
    }

    if (mLoadPoseTimer->loadCompleteOrFailed())
        setLoadingProgress(false);
}

void FSFloaterPoser::setLoadingProgress(bool started)
{
    LLLoadingIndicator* mLoadingIndicator = findChild<LLLoadingIndicator>("progress_indicator");
    if (!mLoadingIndicator)
        return;

    mLoadingIndicator->setVisible(started);
    started ? mLoadingIndicator->start() : mLoadingIndicator->stop();
}

void FSFloaterPoser::onClickLoadLeftHandPose()
{
    setHandPresetTarget(false); // <plus>
    onClickLoadHandPose(false);
}

void FSFloaterPoser::onClickLoadRightHandPose()
{
    setHandPresetTarget(true); // <plus>
    onClickLoadHandPose(true);
}

S32 FSFloaterPoser::tryGetPoseVersion(std::string pathToPoseFile)
{
    S32 version = -1;
    if (pathToPoseFile.empty())
        return version;

    if (!gDirUtilp->fileExists(pathToPoseFile))
        return version;

    try
    {
        LLSD       pose;
        llifstream infile;

        infile.open(pathToPoseFile);
        if (!infile.is_open())
            return version;

        while (!infile.eof())
        {
            S32 lineCount = LLSDSerialize::fromXML(pose, infile);
            if (lineCount == LLSDParser::PARSE_FAILURE)
            {
                LL_WARNS("Posing") << "Failed to parse pose file: " << pathToPoseFile << LL_ENDL;
                return version;
            }

            for (LLSD::map_const_iterator itr = pose.beginMap(); itr != pose.endMap(); ++itr)
            {
                std::string const& name        = itr->first;
                LLSD const&        control_map = itr->second;

                if (name == "version")
                    return (S32)control_map["value"].asInteger();
            }
        }
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Posing") << "Threw an exception trying read the pose file: " << pathToPoseFile << " exception: " << e.what() << LL_ENDL;
    }

    return version;
}

void FSFloaterPoser::onClickLoadHandPose(bool isRightHand)
{
    LLScrollListItem* item = mHandPresetsScrollList->getFirstSelected();
    if (!item)
        return;

    // <plus> the row's value is the file's name; the text of its first column is the display name (handPresetDisplayName)
    std::string poseName = item->getValue().asString();
    // </plus>
    std::string pathname = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY, std::string(POSE_PRESETS_HANDS_SUBDIRECTORY));
    if (!gDirUtilp->fileExists(pathname))
        return;

    std::string fullPath =
        gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY, std::string(POSE_PRESETS_HANDS_SUBDIRECTORY), poseName + POSE_INTERNAL_FORMAT_FILE_EXT);

    LLVOAvatar* avatar   = getUiSelectedAvatar();
    if (!avatar)
        return;
    if (!havePermissionToAnimateAvatar(avatar) && !havePermissionToAnimateOtherAvatar(avatar))
        return;
    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    try
    {
        LLSD       pose;
        llifstream infile;
        LLVector3  vec3;

        infile.open(fullPath);
        if (!infile.is_open())
            return;

        while (!infile.eof())
        {
            S32 lineCount = LLSDSerialize::fromXML(pose, infile);
            if (lineCount == LLSDParser::PARSE_FAILURE)
            {
                LL_WARNS("Posing") << "Failed to parse loading a file for a hand: " << poseName << LL_ENDL;
                return;
            }

            for (LLSD::map_const_iterator itr = pose.beginMap(); itr != pose.endMap(); ++itr)
            {
                std::string const& name        = itr->first;
                LLSD const&        control_map = itr->second;

                if (name.find("Hand") == std::string::npos)
                    continue;
                if (isRightHand != (name.find("Left") == std::string::npos))
                    continue;

                const FSPoserAnimator::FSPoserJoint* poserJoint = mPoserAnimator.getPoserJointByName(name);
                if (!poserJoint)
                    continue;

                // <plus> a joint the user took out of the pose ("pose this joint" off) is left alone, as the rotation
                // controls leave it: a value loaded into it would not show, and would jump in when the joint is posed again
                if (!mPoserAnimator.isPosingAvatarJoint(avatar, *poserJoint))
                    continue;
                // </plus>

                vec3.setValue(control_map["rotation"]);
                mPoserAnimator.loadJointRotation(avatar, poserJoint, true, true, vec3);
            }
        }

        addBoldToScrollList(mHandJointsScrollList, avatar);
        // <plus> what shows the joints follows: the rotation controls of the selected joint (they kept the angles of
        // before the preset), the Easy sliders, and the undo / redo buttons (loading a joint drops its undo history and
        // then keeps the state from before the preset as its one step, unless the joint was changed a moment before:
        // FSJointPose::addStateToUndo)
        refreshRotationSlidersAndSpinners();
        refreshTrackpadCursor();
        enableOrDisableRedoAndUndoButton();
        // </plus>
    }
    catch ( const std::exception& e )
    {
        LL_WARNS("Posing") << "Threw an exception trying to load a hand pose: " << poseName << " exception: " << e.what() << LL_ENDL;
    }
}

bool FSFloaterPoser::poseFileStartsFromTeePose(const std::string& poseFileName)
{
    std::string pathname = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY);
    if (!gDirUtilp->fileExists(pathname))
        return false;

    std::string fullPath =
        gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY, poseFileName + POSE_INTERNAL_FORMAT_FILE_EXT);

    try
    {
        LLSD       pose;
        llifstream infile;
        bool       startFromZeroRot = false;

        infile.open(fullPath);
        if (!infile.is_open())
            return false;

        S32 lineCount = LLSDSerialize::fromXML(pose, infile);
        if (lineCount == LLSDParser::PARSE_FAILURE)
            return startFromZeroRot;

        for (LLSD::map_const_iterator itr = pose.beginMap(); itr != pose.endMap(); ++itr)
        {
            std::string const& name        = itr->first;
            LLSD const&        control_map = itr->second;

            if (name == "startFromTeePose")
                startFromZeroRot = control_map["value"].asBoolean();
        }

        return startFromZeroRot;
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Posing") << "Unable to load or parse the pose: " << poseFileName << " exception: " << e.what() << LL_ENDL;
    }

    return false;
}

bool FSFloaterPoser::loadPoseFromXml(LLVOAvatar* avatar, const std::string& poseFileName, E_LoadPoseMethods loadMethod)
{
    bool        loadSuccess = false;
    std::string pathname = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY);
    if (!gDirUtilp->fileExists(pathname))
        return loadSuccess;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return loadSuccess;

    std::string fullPath =
        gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY, poseFileName + POSE_INTERNAL_FORMAT_FILE_EXT);

    bool loadRotations = loadMethod == ROTATIONS || loadMethod == ROTATIONS_AND_POSITIONS || loadMethod == ROTATIONS_AND_SCALES ||
                         loadMethod == ROT_POS_AND_SCALES || loadMethod == SELECTIVE || loadMethod == SELECTIVE_ROT;
    bool loadPositions = loadMethod == POSITIONS || loadMethod == ROTATIONS_AND_POSITIONS || loadMethod == POSITIONS_AND_SCALES ||
                         loadMethod == ROT_POS_AND_SCALES || loadMethod == SELECTIVE;
    bool loadScales    = loadMethod == SCALES || loadMethod == POSITIONS_AND_SCALES || loadMethod == ROTATIONS_AND_SCALES ||
                         loadMethod == ROT_POS_AND_SCALES || loadMethod == SELECTIVE;
    bool loadSelective = loadMethod == SELECTIVE || loadMethod == SELECTIVE_ROT;

    try
    {
        LLSD         pose;
        llifstream   infile;
        LLVector3    vec3;
        LLQuaternion quat;
        bool         enabled;
        bool         setJointBaseRotationToZero;
        bool         userSetBaseRotationToZero;
        bool         worldLocked;
        bool         mirroredJoint;
        S32          version = 0;
        bool startFromZeroRot = true;

        infile.open(fullPath);
        if (!infile.is_open())
            return loadSuccess;

        loadSuccess = true;
        while (!infile.eof())
        {
            S32 lineCount = LLSDSerialize::fromXML(pose, infile);
            if (lineCount == LLSDParser::PARSE_FAILURE)
            {
                LL_WARNS("Posing") << "Failed to parse file: " << poseFileName << LL_ENDL;
                return loadSuccess;
            }

            for (LLSD::map_const_iterator itr = pose.beginMap(); itr != pose.endMap(); ++itr)
            {
                std::string const& name        = itr->first;
                LLSD const&        control_map = itr->second;

                if (name == "startFromTeePose")
                    startFromZeroRot = control_map["value"].asBoolean();

                if (name == "version")
                    version = (S32)control_map["value"].asInteger();
            }

            if (version > 5 && startFromZeroRot)
                mPoserAnimator.setAllAvatarStartingRotationsToZero(avatar);

            bool loadPositionsAndScalesAsDeltas = false;
            if (version > 3)
                loadPositionsAndScalesAsDeltas = true;

            for (LLSD::map_const_iterator itr = pose.beginMap(); itr != pose.endMap(); ++itr)
            {
                std::string const& name        = itr->first;
                LLSD const&        control_map = itr->second;

                const FSPoserAnimator::FSPoserJoint *poserJoint = mPoserAnimator.getPoserJointByName(name);
                if (!poserJoint)
                    continue;

                if (loadSelective && mPoserAnimator.isPosingAvatarJoint(avatar, *poserJoint))
                    continue;

                if (control_map.has("enabled"))
                {
                    enabled = control_map["enabled"].asBoolean();
                    mPoserAnimator.setPosingAvatarJoint(avatar, *poserJoint, enabled || loadSelective);
                }

                if (control_map.has("jointBaseRotationIsZero"))
                    setJointBaseRotationToZero = control_map["jointBaseRotationIsZero"].asBoolean();
                else
                    setJointBaseRotationToZero = startFromZeroRot;

                if (control_map.has("userSetBaseRotationToZero"))
                    userSetBaseRotationToZero = control_map["userSetBaseRotationToZero"].asBoolean();
                else
                    userSetBaseRotationToZero = startFromZeroRot;

                if (loadPositions && control_map.has("position"))
                    vec3.setValue(control_map["position"]);
                else
                    vec3.clear();

                mPoserAnimator.loadJointPosition(avatar, poserJoint, loadPositionsAndScalesAsDeltas, vec3);

                if (loadRotations && control_map.has("rotation"))
                    vec3.setValue(control_map["rotation"]);
                else
                    vec3.clear();

                mPoserAnimator.loadJointRotation(avatar, poserJoint, setJointBaseRotationToZero, userSetBaseRotationToZero, vec3);

                if (loadScales && control_map.has("scale"))
                    vec3.setValue(control_map["scale"]);
                else
                    vec3.clear();

                mPoserAnimator.loadJointScale(avatar, poserJoint, loadPositionsAndScalesAsDeltas, vec3);

                worldLocked = control_map.has("worldLocked") ? control_map["worldLocked"].asBoolean() : false;
                mPoserAnimator.setRotationIsWorldLocked(avatar, *poserJoint, worldLocked);

                mirroredJoint = control_map.has("mirrored") ? control_map["mirrored"].asBoolean() : false;
                mPoserAnimator.setRotationIsMirrored(avatar, *poserJoint, mirroredJoint);
            }

            if (version > 6 && !startFromZeroRot && !loadSelective)
                loadSuccess = mPoserAnimator.loadPosingState(avatar, true, pose);
        }
    }
    catch ( const std::exception & e )
    {
        loadSuccess = false;
        LL_WARNS("Posing") << "Everything caught fire trying to load the pose: " << poseFileName << " exception: " << e.what() << LL_ENDL;
    }

    return loadSuccess;
}

void FSFloaterPoser::startPosingSelf()
{
    if (!gAgentAvatarp || gAgentAvatarp.isNull())
        return;

    setUiSelectedAvatar(gAgentAvatarp->getID());
    LLVOAvatar* avatar = getAvatarByUuid(gAgentAvatarp->getID());
    if (!avatar)
        return;

    bool arePosingSelected = mPoserAnimator.isPosingAvatar(avatar);
    if (!arePosingSelected && couldAnimateAvatar(avatar))
        mPoserAnimator.tryPosingAvatar(avatar);

    onAvatarSelect();
}

void FSFloaterPoser::stopPosingAllAvatars()
{
    if (!gAgentAvatarp || gAgentAvatarp.isNull() || !mAvatarSelectionScrollList)
        return;

    for (auto listItem : mAvatarSelectionScrollList->getAllData())
    {
        LLScrollListCell* cell = listItem->getColumn(COL_UUID);
        if (!cell)
            continue;

        LLUUID      selectedAvatarId = cell->getValue().asUUID();
        LLVOAvatar* listAvatar       = getAvatarByUuid(selectedAvatarId);

        if (mPoserAnimator.isPosingAvatar(listAvatar))
            mPoserAnimator.stopPosingAvatar(listAvatar);
    }

    onAvatarSelect();
}

void FSFloaterPoser::onPoseStartStop()
{
    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    bool arePosingSelected = mPoserAnimator.isPosingAvatar(avatar);
    if (arePosingSelected)
    {
        mPoserAnimator.stopPosingAvatar(avatar);
    }
    else
    {
        if (!couldAnimateAvatar(avatar))
            return;

        if (!havePermissionToAnimateAvatar(avatar))
            return;

        mPoserAnimator.tryPosingAvatar(avatar);
    }

    onAvatarsRefresh();
    onAvatarSelect();
}

bool FSFloaterPoser::couldAnimateAvatar(LLVOAvatar* avatar) const
{
    if (!avatar || avatar->isDead())
        return false;
    if (avatar->getRegion() != gAgent.getRegion())
        return false;

    return true;
}

bool FSFloaterPoser::havePermissionToAnimateAvatar(LLVOAvatar* avatar) const
{
    if (!avatar || avatar->isDead())
        return false;
    if (avatar->isSelf())
        return true;

    if (avatar->isControlAvatar())
    {
        LLControlAvatar*      control_av     = dynamic_cast<LLControlAvatar*>(avatar);
        const LLVOVolume*     rootVolume     = control_av->mRootVolp;
        const LLViewerObject* rootEditObject = (rootVolume) ? rootVolume->getRootEdit() : NULL;
        if (!rootEditObject)
            return false;

        return rootEditObject->permYouOwner();
    }

    return false;
}

bool FSFloaterPoser::havePermissionToAnimateOtherAvatar(LLVOAvatar* avatar) const
{
    if (!avatar || avatar->isDead())
        return false;

    return false;
}

void FSFloaterPoser::poseControlsEnable(bool enable)
{
    mTrackballPnl->setEnabled(enable);
    mFlipPoseBtn->setEnabled(enable);
    mFlipJointBtn->setEnabled(enable);
    mRecaptureBtn->setEnabled(enable);
    mTogglePosingBonesBtn->setEnabled(enable);
    mLoadPosesBtn->setEnabled(enable);
    mSavePosesBtn->setEnabled(enable);
    mPoseSaveNameEditor->setEnabled(enable);
    mBtnJointReset->setEnabled(enable);
    mRedoChangeBtn->setEnabled(enable);
    mUndoChangeBtn->setEnabled(enable);
    mPositionPnl->setEnabled(enable);
    mMoveTabPnl->setEnabled(enable);
    mTrackballButtonPnl->setEnabled(enable);
}

void FSFloaterPoser::refreshJointScrollListMembers()
{
    mEntireAvJointScroll->clearRows();
    mBodyJointsScrollList->clearRows();
    mFaceJointsScrollList->clearRows();
    mHandJointsScrollList->clearRows();
    mMiscJointsScrollList->clearRows();
    mCollisionVolumesScrollList->clearRows();

    std::vector<FSPoserAnimator::FSPoserJoint>::const_iterator poserJoint_iter;
    for (poserJoint_iter = mPoserAnimator.PoserJoints.begin();
         poserJoint_iter != mPoserAnimator.PoserJoints.end();
         ++poserJoint_iter)
    {
        LLSD row = createRowForJoint(poserJoint_iter->jointName(), false);
        if (!row)
            continue;

        LLScrollListItem *item = nullptr;
        bool hasListHeader = hasXuiString(XML_LIST_HEADER_STRING_PREFIX + poserJoint_iter->jointName());

        switch (poserJoint_iter->boneType())
        {
            case WHOLEAVATAR:
                item = mEntireAvJointScroll->addElement(row);
                mEntireAvJointScroll->selectFirstItem();
                break;

            case BODY:
                if (hasListHeader)
                    addHeaderRowToScrollList(poserJoint_iter->jointName(), mBodyJointsScrollList);

                item = mBodyJointsScrollList->addElement(row);
                break;

            case FACE:
                if (hasListHeader)
                    addHeaderRowToScrollList(poserJoint_iter->jointName(), mFaceJointsScrollList);

                item = mFaceJointsScrollList->addElement(row);
                break;

            case HANDS:
                if (hasListHeader)
                    addHeaderRowToScrollList(poserJoint_iter->jointName(), mHandJointsScrollList);

                item = mHandJointsScrollList->addElement(row);
                break;

            case MISC:
                if (hasListHeader)
                    addHeaderRowToScrollList(poserJoint_iter->jointName(), mMiscJointsScrollList);

                item = mMiscJointsScrollList->addElement(row);
                break;

            case COL_VOLUMES:
                if (hasListHeader)
                    addHeaderRowToScrollList(poserJoint_iter->jointName(), mCollisionVolumesScrollList);

                item = mCollisionVolumesScrollList->addElement(row);
                break;
        }

        if (item)
            item->setUserdata((void*) &*poserJoint_iter);
    }

    refreshTextHighlightingOnJointScrollLists();
}

void FSFloaterPoser::addHeaderRowToScrollList(const std::string& jointName, LLScrollListCtrl* bodyJointsScrollList)
{
    LLSD headerRow = createRowForJoint(jointName, true);
    if (!headerRow)
        return;

    LLScrollListItem *hdrRow = bodyJointsScrollList->addElement(headerRow);
    hdrRow->setEnabled(false);
}

LLSD FSFloaterPoser::createRowForJoint(const std::string& jointName, bool isHeaderRow)
{
    if (jointName.empty())
        return {};

    std::string headerValue = "";
    if (isHeaderRow)
        headerValue = tryGetString("icon_category");

    std::string jointValue    = jointName;
    std::string parameterName = (isHeaderRow ? XML_LIST_HEADER_STRING_PREFIX : XML_LIST_TITLE_STRING_PREFIX) + jointName;
    if (hasXuiString(parameterName))
        jointValue = getString(parameterName);
    else
        return {};

    LLSD row;
    row["columns"][COL_ICON]["column"] = "icon";
    row["columns"][COL_ICON]["type"]   = "icon";
    row["columns"][COL_ICON]["value"]  = headerValue;
    row["columns"][COL_NAME]["column"] = "joint";
    row["columns"][COL_NAME]["value"]  = jointValue;

    return row;
}

void FSFloaterPoser::onToggleLoadSavePanel()
{
    if (isMinimized())
        return;

    // Get the load/save button toggle state, find the load/save panel, and set its visibility
    bool loadSavePanelExpanded = mToggleLoadSavePanelBtn->getValue().asBoolean();
    mPosesLoadSavePnl->setVisible(loadSavePanelExpanded);
    mLoadPosesBtn->setVisible(loadSavePanelExpanded);
    mSavePosesBtn->setVisible(loadSavePanelExpanded);
    mBrowserFolderBtn->setVisible(loadSavePanelExpanded);

    // change the width of the Poser panel for the (dis)appearance of the load/save panel
    S32 currentWidth       = getRect().getWidth();
    S32 loadSavePanelWidth = mPosesLoadSavePnl->getRect().getWidth();

    S32 poserFloaterHeight = getRect().getHeight();
    S32 poserFloaterWidth  = loadSavePanelExpanded ? currentWidth + loadSavePanelWidth : currentWidth - loadSavePanelWidth;

    if (poserFloaterWidth < 0)
        return;

    reshape(poserFloaterWidth, poserFloaterHeight);

    if (loadSavePanelExpanded)
        refreshPoseScroll(mPosesScrollList);
}

void FSFloaterPoser::onToggleMirrorChange()
{
    setRotationChangeButtons(true, false);
}

void FSFloaterPoser::onToggleSympatheticChange()
{
    setRotationChangeButtons(false, true);
}

void FSFloaterPoser::setRotationChangeButtons(bool togglingMirror, bool togglingSympathetic)
{
    if (togglingSympathetic)
        mToggleMirrorRotationBtn->setValue(false);

    if (togglingMirror)
        mToggleSympatheticRotationBtn->setValue(false);

    refreshTrackpadCursor();
}

void FSFloaterPoser::onToggleRotationFrameButton(const LLUICtrl* toggleButton)
{
    if (!toggleButton)
        return;

    if (bool buttonDown = toggleButton->getValue().asBoolean())
    {
        mBtnAvatarFrame->setValue(toggleButton == mBtnAvatarFrame);
        mBtnScreenFrame->setValue(toggleButton == mBtnScreenFrame);
        mBtnWorldFrame->setValue(toggleButton == mBtnWorldFrame);
    }

    FSToolCompPose::getInstance()->setReferenceFrame(getReferenceFrame());
    FSToolCompPoseTranslate::getInstance()->setReferenceFrame(getReferenceFrame());
    refreshRotationSlidersAndSpinners();
}

// <plus> The last IK drag (research/226) is one step for the undo and redo buttons, whichever joints are selected,
// while nothing else has changed a joint since.
bool FSFloaterPoser::lastIkDragIsNext(LLVOAvatar* avatar, bool undo) const
{
    const FSJointPose::UndoBatch* batch = FSJointPose::lastUndoBatch();
    return batch && avatar && batch->avatar == avatar->getID() && batch->undone != undo;
}

bool FSFloaterPoser::undoOrRedoIkDrag(LLVOAvatar* avatar, bool undo)
{
    if (!lastIkDragIsNext(avatar, undo))
        return false;
    if (!mPoserAnimator.undoOrRedoJointsAsOne(avatar, FSJointPose::lastUndoBatch()->joints, undo))
        return false;
    FSJointPose::setLastUndoBatchUndone(undo);

    enableOrDisableRedoAndUndoButton();
    refreshRotationSlidersAndSpinners();
    refreshPositionSlidersAndSpinners();
    refreshScaleSlidersAndSpinners();
    refreshTrackpadCursor();
    refreshTextHighlightingOnJointScrollLists();
    return true;
}

void FSFloaterPoser::onIkDragDone()
{
    enableOrDisableRedoAndUndoButton();
}
// </plus>

void FSFloaterPoser::onUndoLastChange()
{
    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    if (undoOrRedoIkDrag(avatar, true)) // <plus>
        return;

    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return;

    bool stepped = false; // <plus>
    for (auto item : selectedJoints)
    {
        bool currentlyPosing = mPoserAnimator.isPosingAvatarJoint(avatar, *item);
        if (currentlyPosing)
        {
            mPoserAnimator.undoLastJointChange(avatar, *item, getUiSelectedBoneDeflectionStyle());
            stepped = true; // <plus>
        }
    }
    // <plus> the joints' own steps from here on (no mixing with the drag's); a selection with no posed joint
    // took no step, and the drag stays the next one (research/275 L095)
    if (stepped)
        FSJointPose::forgetLastUndoBatch();
    // </plus>

    enableOrDisableRedoAndUndoButton();
    refreshRotationSlidersAndSpinners();
    refreshPositionSlidersAndSpinners();
    refreshScaleSlidersAndSpinners();
    refreshTrackpadCursor();
    if (getSavingToBvh())
        refreshTextHighlightingOnJointScrollLists();
}

void FSFloaterPoser::onSetAvatarToTpose()
{
    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    setSavePosesButtonText(false);
    mPoserAnimator.setAllAvatarStartingRotationsToZero(avatar);
    refreshTextHighlightingOnJointScrollLists();
    enableOrDisableRedoAndUndoButton();
    // <plus> what is seen changed (the base rotations went to zero): the rotation controls and the hand tab's Easy sliders follow
    refreshRotationSlidersAndSpinners();
    refreshTrackpadCursor();
    // </plus>
}

void FSFloaterPoser::onResetJoint(const LLSD& data)
{
    int resetType = data.asInteger();

    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return;

    for (auto item : selectedJoints)
    {
        bool currentlyPosing = mPoserAnimator.isPosingAvatarJoint(avatar, *item);
        if (!currentlyPosing)
            continue;

        mPoserAnimator.resetJoint(avatar, *item, getUiSelectedBoneDeflectionStyle());
    }

    refreshRotationSlidersAndSpinners();
    refreshPositionSlidersAndSpinners();
    refreshScaleSlidersAndSpinners();
    refreshTrackpadCursor();
    enableOrDisableRedoAndUndoButton();
    refreshTextHighlightingOnJointScrollLists();
}

void FSFloaterPoser::onRedoLastChange()
{
    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    if (undoOrRedoIkDrag(avatar, false)) // <plus>
        return;

    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return;

    bool stepped = false; // <plus>
    for (auto item : selectedJoints)
    {
        bool currentlyPosing = mPoserAnimator.isPosingAvatarJoint(avatar, *item);
        if (currentlyPosing)
        {
            mPoserAnimator.redoLastJointChange(avatar, *item, getUiSelectedBoneDeflectionStyle());
            stepped = true; // <plus>
        }
    }
    // <plus> the joints' own steps from here on (no mixing with the drag's); a selection with no posed joint
    // took no step, and the drag stays the next one (research/275 L095)
    if (stepped)
        FSJointPose::forgetLastUndoBatch();
    // </plus>

    enableOrDisableRedoAndUndoButton();
    refreshRotationSlidersAndSpinners();
    refreshTrackpadCursor();
    refreshScaleSlidersAndSpinners();
    refreshPositionSlidersAndSpinners();
    if (getSavingToBvh())
        refreshTextHighlightingOnJointScrollLists();
}

void FSFloaterPoser::enableOrDisableRedoAndUndoButton()
{
    mRedoChangeBtn->setEnabled(false);
    mUndoChangeBtn->setEnabled(false);

    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    // <plus> the last IK drag is one step, whichever joints are selected
    bool shouldEnableRedoButton = lastIkDragIsNext(avatar, false);
    bool shouldEnableUndoButton = lastIkDragIsNext(avatar, true);
    mRedoChangeBtn->setEnabled(shouldEnableRedoButton);
    mUndoChangeBtn->setEnabled(shouldEnableUndoButton);
    // </plus>

    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return;

    for (auto item : selectedJoints)
    {
        bool currentlyPosing = mPoserAnimator.isPosingAvatarJoint(avatar, *item);
        if (!currentlyPosing)
            continue;

        shouldEnableRedoButton |= mPoserAnimator.canRedoOrUndoJointChange(avatar, *item);
        shouldEnableUndoButton |= mPoserAnimator.canRedoOrUndoJointChange(avatar, *item, true);
    }

    mRedoChangeBtn->setEnabled(shouldEnableRedoButton);
    mUndoChangeBtn->setEnabled(shouldEnableUndoButton);
}

void FSFloaterPoser::onToggleVisualManipulators()
{
    bool tools_enabled = mToggleVisualManipulators->getValue().asBoolean();

    if (tools_enabled)
    {
        enableVisualManipulators();
    }
    else
    {
        disableVisualManipulators();
    }
}

// <plus> IK drag (research/226)
// The IK button under the settings (always in view) and the checkbox of the settings tab are the same switch:
// whichever was used, the other follows.
void FSFloaterPoser::onToggleIKTool(const LLUICtrl* source)
{
    if (!source)
        return;
    setIKToolOn(source->getValue().asBoolean());
}

void FSFloaterPoser::setIKToolOn(bool on)
{
    mIKToolOn = on;
    if (mIKToolCheckbox)
        mIKToolCheckbox->setValue(on); // setValue does not commit, so no second toggle
    if (mToggleIKToolBtn)
        mToggleIKToolBtn->setToggleState(on);

    if (!mToggleVisualManipulators->getValue().asBoolean() || LLToolMgr::getInstance()->getCurrentToolset() != getPoserToolset())
        return; // picked up when the visual manipulators are switched on
    // Only the tool changes: the avatar and the joint of the rotate guide are kept by whoever sets them (the list,
    // the visual manipulators switch), so no one touches them while IK is on or when it goes off.
    selectPoserTool();
}

void FSFloaterPoser::selectPoserTool()
{
    LLToolset* toolset = LLToolMgr::getInstance()->getCurrentToolset();
    toolset->selectTool(mIKToolOn ? static_cast<LLTool*>(FSToolCompPoseIK::getInstance())
                                  : static_cast<LLTool*>(FSToolCompPose::getInstance()));
}

// IK drag works on the visual manipulators, so the button can be pressed only while they are on.
void FSFloaterPoser::refreshIKToolButton()
{
    if (mToggleIKToolBtn && mToggleVisualManipulators)
        mToggleIKToolBtn->setEnabled(mToggleVisualManipulators->getValue().asBoolean());
}

void FSFloaterPoser::onClearIKPins()
{
    if (FSToolCompPoseIK::instanceExists())
        FSToolCompPoseIK::getInstance()->getManip()->clearPins();
}

void FSFloaterPoser::refreshIKPinsText()
{
    if (!FSToolCompPoseIK::instanceExists())
        return;
    FSManipIKJoint* manip = FSToolCompPoseIK::getInstance()->getManip();
    if (manip->pinsVersion() == mIKPinsShown)
        return;
    mIKPinsShown = manip->pinsVersion();
    std::string list;
    for (const std::string& name : manip->pinnedJoints())
        list += (list.empty() ? "" : ", ") + name.substr(name.size() > 1 ? 1 : 0);
    if (list.empty())
        list = getString("ik_no_pins");
    if (LLUICtrl* text = findChild<LLUICtrl>("ik_pins_text"))
        text->setValue(list);
}
// </plus>

void FSFloaterPoser::selectJointByName(const std::string& jointName)
{
    LLTabContainer* tabContainer = mJointsTabs;
    std::vector<LLPanel*> panels = {
        mPositionRotationPnl,
        mBodyJointsPnl,
        mFaceJointsPnl,
        mHandsTabs,
        mMiscJointsPnl,
        mCollisionVolumesPnl
    };
    
    std::vector<LLScrollListCtrl*> scrollLists = {
        mEntireAvJointScroll,
        mBodyJointsScrollList,
        mFaceJointsScrollList,
        mHandJointsScrollList,
        mMiscJointsScrollList,
        mCollisionVolumesScrollList
    };

    bool found = false;
    for (S32 i = 0; i < tabContainer->getTabCount(); ++i)
    {
        LLPanel* panel = tabContainer->getPanelByIndex(i);
        tabContainer->selectTabPanel(panel);

        // Special handling for Hands tab
        if (panel == mHandsTabs)
        {
            mHandsTabs->selectTabPanel(mHandsJointsPnl);
        }

        for (auto scrollList : scrollLists)
        {
            scrollList->deselectAllItems();
        }

        auto scrollList = getScrollListForTab(panel);

        std::vector<LLScrollListItem*> items = scrollList->getAllData();
        for (auto item : items)
        {
            auto* userData = static_cast<FSPoserAnimator::FSPoserJoint*>(item->getUserdata());
            if (userData && userData->jointName() == jointName)
            {
                tabContainer->selectTab(i);
                scrollList->selectNthItem(scrollList->getItemIndex(item));
                scrollList->scrollToShowSelected();
                getUiSelectedPoserJoints();
                rememberJointListTab(); // <plus>
                return; // Exit the loop once we've found and selected the joint
            }
        }
    }
    LL_WARNS() << "Joint not found: " << jointName << LL_ENDL;
}

LLScrollListCtrl* FSFloaterPoser::getScrollListForTab(LLPanel* tabPanel) const
{
    if (tabPanel == mPositionRotationPnl)
    {
        return mEntireAvJointScroll;
    }
    else if (tabPanel == mBodyJointsPnl)
    {
        return mBodyJointsScrollList;
    }
    else if (tabPanel == mFaceJointsPnl)
    {
        return mFaceJointsScrollList;
    }
    else if (tabPanel == mHandsTabs)
    {
        return mHandJointsScrollList;
    }
    else if (tabPanel == mMiscJointsPnl)
    {
        return mMiscJointsScrollList;
    }
    else if (tabPanel == mCollisionVolumesPnl)
    {
        return mCollisionVolumesScrollList;
    }

    return nullptr;
}

std::vector<FSPoserAnimator::FSPoserJoint*> FSFloaterPoser::getUiSelectedPoserJoints() const
{
    std::vector<FSPoserAnimator::FSPoserJoint*> joints;

    auto activeTab = mJointsTabs->getCurrentPanel();
    if (!activeTab)
    {
        return joints;
    }

    LLScrollListCtrl* scrollList{ nullptr };
    
    scrollList = getScrollListForTab(activeTab);
    if (activeTab == mPositionRotationPnl)
    {
        mEntireAvJointScroll->selectFirstItem();
    }
    else if (activeTab == mHandsTabs )
    {
        auto activeHandsSubTab = mHandsTabs->getCurrentPanel();
        if (!activeHandsSubTab)
        {
            return joints;
        }
    }
    if (!scrollList)
    {
        return joints;
    }

    joints = getSelectedPoserJointsOfTab(activeTab); // <plus> one place for the loop

    updateManipWithFirstSelectedJoint(joints);

    return joints;
}

void FSFloaterPoser::updateManipWithFirstSelectedJoint(const std::vector<FSPoserAnimator::FSPoserJoint*>& joints) const
{
    auto avatarp = getUiSelectedAvatar();
    if (!avatarp)
    {
        // <plus> the selected one is gone (or none is selected): the tools let go of it too,
        // so that they do not keep pointers to an avatar that may be freed
        FSToolCompPose::getInstance()->setAvatar(nullptr);
        FSToolCompPoseTranslate::getInstance()->setAvatar(nullptr);
        FSToolCompPoseIK::getInstance()->setAvatar(nullptr);
        FSToolCompPose::getInstance()->setJoint(nullptr);
        FSToolCompPoseTranslate::getInstance()->setJoint(nullptr);
        // </plus>
        return;
    }

    bool haveImplicitPermission = havePermissionToAnimateAvatar(avatarp);
    bool iCanPoseThem           = havePermissionToAnimateOtherAvatar(avatarp);

    if ((joints.size() >= 1) && (haveImplicitPermission || iCanPoseThem))
    {
        FSToolCompPose::getInstance()->setJoint(avatarp->getJoint(joints[0]->jointName()));
        FSToolCompPoseTranslate::getInstance()->setJoint(avatarp->getJoint(joints[0]->jointName()));
    }
    else
    {
        FSToolCompPose::getInstance()->setJoint(nullptr);
        FSToolCompPoseTranslate::getInstance()->setJoint(nullptr);
    }
}

E_RotationStyle FSFloaterPoser::getUiSelectedBoneRotationStyle(const std::string& jointName) const
{
    if (jointName.empty())
        return ABSOLUTE_ROT;

    bool hasRotationStylePreferenceParameter = hasXuiString(XML_JOINT_DELTAROT_STRING_PREFIX + jointName);
    if (!hasRotationStylePreferenceParameter)
        return ABSOLUTE_ROT;
 
    std::string paramValue = getString(XML_JOINT_DELTAROT_STRING_PREFIX + jointName);
    if (paramValue == "true")
        return DELTAIC_ROT;

    return ABSOLUTE_ROT;
}

E_BoneDeflectionStyles FSFloaterPoser::getUiSelectedBoneDeflectionStyle() const
{
    bool isDelta = mToggleDeltaModeBtn->getValue().asBoolean();

    if (mToggleMirrorRotationBtn->getValue().asBoolean())
        return isDelta ? MIRROR_DELTA : MIRROR;

    if (mToggleSympatheticRotationBtn->getValue().asBoolean())
        return isDelta ? SYMPATHETIC_DELTA : SYMPATHETIC;

    return isDelta ? DELTAMODE : NONE;
}

LLVOAvatar* FSFloaterPoser::getUiSelectedAvatar() const
{
    LLScrollListItem* item = mAvatarSelectionScrollList->getFirstSelected();
    if (!item)
        return nullptr;

    LLScrollListCell* cell = item->getColumn(COL_UUID);
    if (!cell)
        return nullptr;

    LLUUID selectedAvatarId = cell->getValue().asUUID();

    return getAvatarByUuid(selectedAvatarId);
}

void FSFloaterPoser::setUiSelectedAvatar(const LLUUID& avatarToSelect)
{
    for (auto listItem : mAvatarSelectionScrollList->getAllData())
    {
        LLScrollListCell* cell = listItem->getColumn(COL_UUID);
        if (!cell)
            continue;

        LLUUID avatarId = cell->getValue().asUUID();
        if (avatarId != avatarToSelect)
            continue;

        listItem->setSelected(true);
        break;
    }
}

void FSFloaterPoser::setPoseSaveFileTextBoxToUiSelectedAvatarSaveFileName()
{
    LLScrollListItem* item = mAvatarSelectionScrollList->getFirstSelected();
    if (!item)
        return;

    LLScrollListCell* cell = item->getColumn(COL_SAVE);
    if (!cell)
        return;

    std::string lastSetName = cell->getValue().asString();
    if (lastSetName.empty())
        return;

    LLStringExplicit name = LLStringExplicit(lastSetName);
    mPoseSaveNameEditor->setText(name);
}

void FSFloaterPoser::setUiSelectedAvatarSaveFileName(const std::string& saveFileName)
{
    LLScrollListItem* item = mAvatarSelectionScrollList->getFirstSelected();
    if (!item)
        return;

    LLScrollListCell* cell = item->getColumn(COL_SAVE);
    if (!cell)
        return;

    return cell->setValue(saveFileName);
}

LLVOAvatar* FSFloaterPoser::getAvatarByUuid(const LLUUID& avatarToFind) const
{
    if (avatarToFind.isNull())
        return nullptr;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        if (avatarToFind != character->getID())
            continue;

        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        return avatar;
    }

    return nullptr;
}

void FSFloaterPoser::onPositionSet()
{
    F32 posX = (F32)mAdvPosXSpnr->getValue().asReal();
    F32 posY = (F32)mAdvPosYSpnr->getValue().asReal();
    F32 posZ = (F32)mAdvPosZSpnr->getValue().asReal();
    
    mInOutSpnr->setValue(posX);
    mLeftRightSpnr->setValue(posY);
    mUpDownSpnr->setValue(posZ);
    mAdvPosXSlider->setValue(posX);
    mAdvPosYSlider->setValue(posY);
    mAdvPosZSlider->setValue(posZ);
    mPosXSlider->setValue(posX);
    mPosYSlider->setValue(posY);
    mPosZSlider->setValue(posZ);

    setSelectedJointsPosition(posX, posY, posZ);
    refreshPositionSlidersAndSpinners();
    enableOrDisableRedoAndUndoButton();
}

void FSFloaterPoser::onScaleSet()
{
    F32 scX = (F32)mScaleXSpnr->getValue().asReal();
    F32 scY = (F32)mScaleYSpnr->getValue().asReal();
    F32 scZ = (F32)mScaleZSpnr->getValue().asReal();

    mAdvScaleXSlider->setValue(scX);
    mAdvScaleYSlider->setValue(scY);
    mAdvScaleZSlider->setValue(scZ);

    setSelectedJointsScale(scX, scY, scZ);
    refreshScaleSlidersAndSpinners();
    enableOrDisableRedoAndUndoButton();
}

void FSFloaterPoser::onTrackballChanged()
{
    LLVector3 trackPadDeltaPos;
    LLSD      deltaPosition = mAvatarTrackball->getValueDelta();

    if (deltaPosition.isArray() && deltaPosition.size() == 3)
        trackPadDeltaPos.setValue(deltaPosition);
    else
        return;

    F32 trackPadSensitivity = llmax(gSavedSettings.getF32(POSER_TRACKPAD_SENSITIVITY_SAVE_KEY), 0.0001f);

    trackPadDeltaPos[VX] *= NormalTrackpadRangeInRads * trackPadSensitivity * RAD_TO_DEG;
    trackPadDeltaPos[VY] *= NormalTrackpadRangeInRads * trackPadSensitivity * RAD_TO_DEG;
    trackPadDeltaPos[VZ] *= NormalTrackpadRangeInRads * RAD_TO_DEG;

    F32 axis1 = clipRange((F32)mYawSpnr->getValue().asReal() + trackPadDeltaPos[VX]);
    F32 axis2 = (F32)mPitchSpnr->getValue().asReal() + trackPadDeltaPos[VY];
    F32 axis3 = (F32)mRollSpnr->getValue().asReal() + trackPadDeltaPos[VZ];

    mYawSpnr->setValue(axis1);
    mPitchSpnr->setValue(axis2);
    mRollSpnr->setValue(axis3);

    onYawPitchRollChanged(true);
}

F32 FSFloaterPoser::clipRange(F32 value)
{
    F32 result = fmodf(value, 3600.f); // to avoid time consuming while loops
    while (result > 180.f)
        result -= 360.f;
    while (result < -180.f)
        result += 360.f;

    return result;
}

void FSFloaterPoser::onYawPitchRollChanged(bool skipUpdateTrackpad)
{
    LLVector3 absoluteRotation, deltaRotation;
    absoluteRotation.mV[VX] = (F32)mYawSpnr->getValue().asReal();
    absoluteRotation.mV[VY] = (F32)mPitchSpnr->getValue().asReal();
    absoluteRotation.mV[VZ] = (F32)mRollSpnr->getValue().asReal();

    mAdvRotXSlider->setValue(absoluteRotation.mV[VY]);
    mAdvRotYSlider->setValue(absoluteRotation.mV[VX]);
    mAdvRotZSlider->setValue(absoluteRotation.mV[VZ]);

    absoluteRotation *= DEG_TO_RAD;
    deltaRotation = absoluteRotation - mLastSliderRotation;
    mLastSliderRotation = absoluteRotation;

    setSelectedJointsRotation(absoluteRotation, deltaRotation);
    enableOrDisableRedoAndUndoButton();

    if (!skipUpdateTrackpad)
        refreshTrackpadCursor();
}

void FSFloaterPoser::onAdjustTrackpadSensitivity()
{
    refreshTrackpadCursor();
}

void FSFloaterPoser::refreshTrackpadCursor()
{
    F32 trackPadSensitivity = llmax(gSavedSettings.getF32(POSER_TRACKPAD_SENSITIVITY_SAVE_KEY), 0.0001f);
    F32 axis1 = (F32)mYawSpnr->getValue().asReal() * DEG_TO_RAD / NormalTrackpadRangeInRads / trackPadSensitivity;
    F32 axis2 = (F32)mPitchSpnr->getValue().asReal() * DEG_TO_RAD / NormalTrackpadRangeInRads / trackPadSensitivity;
    F32 axis3 = (F32)mRollSpnr->getValue().asReal() * DEG_TO_RAD / NormalTrackpadRangeInRads;

    mAvatarTrackball->setValue(axis1, axis2, axis3);
}

void FSFloaterPoser::refreshRotationSlidersAndSpinners()
{
    refreshRotationControlsOfSelectedJoint();
    refreshHandEasy(); // <plus> a joint edited by hand: the Easy sliders follow (returns at once while that tab is hidden)
}

void FSFloaterPoser::refreshRotationControlsOfSelectedJoint()
{
    LLVector3 rotation = getRotationOfFirstSelectedJoint();

    mLastSliderRotation = rotation;
    mYawSpnr->setValue(rotation.mV[VX] *= RAD_TO_DEG);
    mPitchSpnr->setValue(rotation.mV[VY] *= RAD_TO_DEG);
    mRollSpnr->setValue(rotation.mV[VZ] *= RAD_TO_DEG);
    mAdvRotXSlider->setValue(rotation.mV[VY]);
    mAdvRotYSlider->setValue(rotation.mV[VX]);
    mAdvRotZSlider->setValue(rotation.mV[VZ]);
}

void FSFloaterPoser::refreshPositionSlidersAndSpinners()
{
    LLVector3 position = getPositionOfFirstSelectedJoint();

    mAdvPosXSlider->setValue(position.mV[VX]);
    mAdvPosXSpnr->setValue(position.mV[VX]);
    mAdvPosYSlider->setValue(position.mV[VY]);
    mAdvPosYSpnr->setValue(position.mV[VY]);
    mAdvPosZSlider->setValue(position.mV[VZ]);
    mAdvPosZSpnr->setValue(position.mV[VZ]);

    auto activeTab = mJointsTabs->getCurrentPanel();
    if (!activeTab)
        return;

    if (activeTab != mPositionRotationPnl)
        return; // if the active tab isn't the av position one, don't set anything.

    mPosXSlider->setValue(position.mV[VX]);
    mInOutSpnr->setValue(position.mV[VX]);
    mPosYSlider->setValue(position.mV[VY]);
    mLeftRightSpnr->setValue(position.mV[VY]);
    mPosZSlider->setValue(position.mV[VZ]);
    mUpDownSpnr->setValue(position.mV[VZ]);
}

void FSFloaterPoser::refreshScaleSlidersAndSpinners()
{
    LLVector3 rotation = getScaleOfFirstSelectedJoint();

    mAdvScaleXSlider->setValue(rotation.mV[VX]);
    mScaleXSpnr->setValue(rotation.mV[VX]);
    mAdvScaleYSlider->setValue(rotation.mV[VY]);
    mScaleYSpnr->setValue(rotation.mV[VY]);
    mAdvScaleZSlider->setValue(rotation.mV[VZ]);
    mScaleZSpnr->setValue(rotation.mV[VZ]);
}

void FSFloaterPoser::setSelectedJointsPosition(F32 x, F32 y, F32 z)
{
    LLVOAvatar *avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    LLVector3              vec3  = LLVector3(x, y, z);
    E_BoneDeflectionStyles defl  = getUiSelectedBoneDeflectionStyle();
    E_PoserReferenceFrame  frame = getReferenceFrame();

    for (auto item : getUiSelectedPoserJoints())
    {
        bool currentlyPosingJoint = mPoserAnimator.isPosingAvatarJoint(avatar, *item);
        if (!currentlyPosingJoint)
            continue;

        mPoserAnimator.setJointPosition(avatar, item, vec3, frame, defl);
    }
}

void FSFloaterPoser::setSelectedJointsRotation(const LLVector3& absoluteRot, const LLVector3& deltaRot)
{
    LLVOAvatar *avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    auto                   selectedJoints   = getUiSelectedPoserJoints();
    bool                   savingToExternal = getSavingToBvh();
    E_BoneDeflectionStyles deflection       = getUiSelectedBoneDeflectionStyle();
    E_PoserReferenceFrame  frame            = getReferenceFrame();

    for (auto item : selectedJoints)
    {
        bool currentlyPosingJoint = mPoserAnimator.isPosingAvatarJoint(avatar, *item);
        if (!currentlyPosingJoint)
            continue;

        auto oppositeJoint = mPoserAnimator.getPoserJointByName(item->mirrorJointName());
        if (oppositeJoint)
        {
            bool oppositeJointAlsoSelectedOnUi =
                std::find(selectedJoints.begin(), selectedJoints.end(), oppositeJoint) != selectedJoints.end();

            bool deflectionDoesOppositeLimbs = !(deflection == NONE || deflection == DELTAMODE);
            if (oppositeJointAlsoSelectedOnUi && deflectionDoesOppositeLimbs && item->dontFlipOnMirror())
                continue;
        }

        S32                   jointNegation = getJointNegation(frame, item->jointName());
        E_BoneAxisTranslation translation   = getJointTranslation(frame, item->jointName());
        E_RotationStyle       style         = getUiSelectedBoneRotationStyle(item->jointName());

        mPoserAnimator.setJointRotation(avatar, item, absoluteRot, deltaRot, deflection, frame, translation, jointNegation,
                                        savingToExternal, style);
    }

    if (savingToExternal)
        refreshTextHighlightingOnJointScrollLists();
}

void FSFloaterPoser::setSelectedJointsScale(F32 x, F32 y, F32 z)
{
    LLVOAvatar *avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    LLVector3              vec3  = LLVector3(x, y, z);
    E_BoneDeflectionStyles defl  = getUiSelectedBoneDeflectionStyle();
    E_PoserReferenceFrame  frame = getReferenceFrame();

    for (auto item : getUiSelectedPoserJoints())
    {
        bool currentlyPosingJoint = mPoserAnimator.isPosingAvatarJoint(avatar, *item);
        if (!currentlyPosingJoint)
            continue;

        mPoserAnimator.setJointScale(avatar, item, vec3, frame, defl);
    }
}

LLVector3 FSFloaterPoser::getRotationOfFirstSelectedJoint() const
{
    LLVector3 rotation;
    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return rotation;

    LLVOAvatar *avatar = getUiSelectedAvatar();
    if (!avatar)
        return rotation;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return rotation;

    E_PoserReferenceFrame frame = getReferenceFrame();

    rotation = mPoserAnimator.getJointRotation(avatar, *selectedJoints.front(), getJointTranslation(frame, selectedJoints.front()->jointName()),
                                        getJointNegation(frame, selectedJoints.front()->jointName()));

    return rotation;
}

LLVector3 FSFloaterPoser::getPositionOfFirstSelectedJoint() const
{
    LLVector3 position;
    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return position;

    LLVOAvatar *avatar = getUiSelectedAvatar();
    if (!avatar)
        return position;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return position;

    position = mPoserAnimator.getJointPosition(avatar, *selectedJoints.front());
    return position;
}

LLVector3 FSFloaterPoser::getScaleOfFirstSelectedJoint() const
{
    LLVector3 scale;
    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return scale;

    LLVOAvatar *avatar = getUiSelectedAvatar();
    if (!avatar)
        return scale;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return scale;

    scale = mPoserAnimator.getJointScale(avatar, *selectedJoints.front());
    return scale;
}

void FSFloaterPoser::onJointTabSelect()
{
    rememberJointListTab(); // <plus>
    refreshPositionSlidersAndSpinners();
    refreshRotationSlidersAndSpinners(); 
    refreshTrackpadCursor();
    enableOrDisableRedoAndUndoButton();
    refreshScaleSlidersAndSpinners();
    markSelectedJointsToHighlight();
}

E_BoneAxisTranslation FSFloaterPoser::getJointTranslation(E_PoserReferenceFrame frame, const std::string& jointName) const
{
    if (jointName.empty())
        return SWAP_NOTHING;

    std::string paramName;
    if (frame == POSER_FRAME_BONE)
        paramName = XML_JOINT_TRANSFORM_STRING_PREFIX + jointName;
    else
        paramName = XML_JOINT_FRAME_TRANSFORM_PREFIX + jointName;

    bool hasTransformParameter = hasXuiString(paramName);
    if (!hasTransformParameter)
        return SWAP_NOTHING;

    std::string paramValue = getString(paramName);

    if (strstr(paramValue.c_str(), "SWAP_YAW_AND_ROLL"))
        return SWAP_YAW_AND_ROLL;
    else if (strstr(paramValue.c_str(), "SWAP_YAW_AND_PITCH"))
        return SWAP_YAW_AND_PITCH;
    else if (strstr(paramValue.c_str(), "SWAP_ROLL_AND_PITCH"))
        return SWAP_ROLL_AND_PITCH;
    else if (strstr(paramValue.c_str(), "SWAP_X2Y_Y2Z_Z2X"))
        return SWAP_X2Y_Y2Z_Z2X;
    else if (strstr(paramValue.c_str(), "SWAP_X2Z_Y2X_Z2Y"))
        return SWAP_X2Z_Y2X_Z2Y;
    else
        return SWAP_NOTHING;
}

S32 FSFloaterPoser::getJointNegation(E_PoserReferenceFrame frame, const std::string& jointName) const
{
    S32 result = NEGATE_NOTHING;

    if (jointName.empty())
        return result;

    std::string paramName;
    if (frame == POSER_FRAME_BONE)
        paramName = XML_JOINT_TRANSFORM_STRING_PREFIX + jointName;
    else
        paramName = XML_JOINT_FRAME_TRANSFORM_PREFIX + jointName;

    bool hasNegationParameter = hasXuiString(paramName);
    if (!hasNegationParameter)
        return result;

    std::string paramValue = getString(paramName);

    if (strstr(paramValue.c_str(), "NEGATE_YAW"))
        result |= NEGATE_YAW;
    if (strstr(paramValue.c_str(), "NEGATE_PITCH"))
        result |= NEGATE_PITCH;
    if (strstr(paramValue.c_str(), "NEGATE_ROLL"))
        result |= NEGATE_ROLL;
    if (strstr(paramValue.c_str(), "NEGATE_ALL"))
        return NEGATE_ALL;

    return result;
}

E_PoserReferenceFrame FSFloaterPoser::getReferenceFrame() const
{
    bool toggleButtonValue = mBtnScreenFrame->getValue().asBoolean();
    if (toggleButtonValue)
        return POSER_FRAME_CAMERA;

    toggleButtonValue = mBtnAvatarFrame->getValue().asBoolean();
    if (toggleButtonValue)
        return POSER_FRAME_AVATAR;

    toggleButtonValue = mBtnWorldFrame->getValue().asBoolean();
    if (toggleButtonValue)
        return POSER_FRAME_WORLD;

    return POSER_FRAME_BONE;
}

/// <summary>
/// An event handler for selecting an avatar or animesh on the POSES_AVATAR_SCROLL_LIST_NAME.
/// In general this will refresh the views for joints or their proxies, and (dis/en)able elements of the view.
/// </summary>
void FSFloaterPoser::onAvatarSelect()
{
    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    bool          isSelf                 = avatar->isSelf();
    bool          haveImplicitPermission = havePermissionToAnimateAvatar(avatar); // self & control avatars you own
    bool          haveExplicitPermission = havePermissionToAnimateOtherAvatar(avatar); // as permissions allow

    if (haveImplicitPermission || haveExplicitPermission)
    {
        FSToolCompPose::getInstance()->setAvatar(avatar);
        FSToolCompPoseTranslate::getInstance()->setAvatar(avatar);
        FSToolCompPoseIK::getInstance()->setAvatar(avatar); // <plus>
    }
    else
    {
        FSToolCompPose::getInstance()->setAvatar(nullptr);
        FSToolCompPoseTranslate::getInstance()->setAvatar(nullptr);
        FSToolCompPoseIK::getInstance()->setAvatar(nullptr); // <plus>
    }

    bool arePosingSelected = mPoserAnimator.isPosingAvatar(avatar);

    mStartStopPosingBtn->setEnabled(haveImplicitPermission);
    mStartStopPosingBtn->setValue(arePosingSelected);

    mSetToTposeButton->setEnabled(haveImplicitPermission && arePosingSelected);
    poseControlsEnable(arePosingSelected && haveImplicitPermission);

    refreshTextHighlightingOnAvatarScrollList();
    refreshTextHighlightingOnJointScrollLists();
    onJointTabSelect();
    setPoseSaveFileTextBoxToUiSelectedAvatarSaveFileName();
}

uuid_vec_t FSFloaterPoser::getNearbyAvatarsAndAnimeshes() const
{
    uuid_vec_t avatar_ids;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!avatarIsNearbyMe(avatar))
            continue;

        bool isMuted = LLMuteList::getInstance()->isMuted(avatar->getID());
        if (isMuted)
            continue;

        bool isSelfOrCtrl = avatar->isControlAvatar() || avatar->isSelf();

        if (!isSelfOrCtrl)
            continue;

        avatar_ids.emplace_back(character->getID());
    }

    return avatar_ids;
}

bool FSFloaterPoser::avatarIsNearbyMe(LLCharacter* character) const
{
    if (!gAgentAvatarp || gAgentAvatarp.isNull() || !character)
        return false;

    LLVector3 separationVector = character->getCharacterPosition() - gAgentAvatarp->getCharacterPosition();

    return separationVector.magVec() < 50.f;
}

uuid_vec_t FSFloaterPoser::getCurrentlyListedAvatarsAndAnimeshes() const
{
    uuid_vec_t avatar_ids;

    for (auto listItem : mAvatarSelectionScrollList->getAllData())
    {
        LLScrollListCell* cell = listItem->getColumn(COL_UUID);
        if (!cell)
            continue;

        LLUUID avatarId = cell->getValue().asUUID();
        avatar_ids.emplace_back(avatarId);
    }

    return avatar_ids;
}

S32 FSFloaterPoser::getAvatarListIndexForUuid(const LLUUID& toFind) const
{
    S32 result = -1;
    for (auto listItem : mAvatarSelectionScrollList->getAllData())
    {
        result++;

        LLScrollListCell* cell = listItem->getColumn(COL_UUID);
        if (!cell)
            continue;

        LLUUID avatarId = cell->getValue().asUUID();
        if (avatarId == toFind)
            return result;
    }

    return -1;
}

void FSFloaterPoser::onAvatarsRefresh()
{
    uuid_vec_t avatarsToAddToList, avatarsToRemoveFromList;
    uuid_vec_t nearbyAvatarIds = getNearbyAvatarsAndAnimeshes();
    uuid_vec_t currentlyListedAvatars = getCurrentlyListedAvatarsAndAnimeshes();
    LLCommonUtils::computeDifference(nearbyAvatarIds, currentlyListedAvatars, avatarsToAddToList, avatarsToRemoveFromList);

    for (LLUUID toRemove : avatarsToRemoveFromList)
    {
        S32 indexToRemove = getAvatarListIndexForUuid(toRemove);
        if (indexToRemove >= 0)
            mAvatarSelectionScrollList->deleteSingleItem(indexToRemove);
    }

    std::string iconObjectName = "Inv_Object";
    if (hasXuiString("icon_object"))
        iconObjectName = getString("icon_object");

    // Add non-Animesh avatars
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLUUID uuid = character->getID();
        if (std::find(avatarsToAddToList.begin(), avatarsToAddToList.end(), uuid) == avatarsToAddToList.end())
            continue;

        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!couldAnimateAvatar(avatar))
            continue;

        if (avatar->isControlAvatar())
            continue;

        LLAvatarName av_name;
        if (!LLAvatarNameCache::get(uuid, &av_name))
            continue;

        if (LLMuteList::getInstance()->isMuted(uuid))
            continue;

        if (!avatar->isSelf())
            continue;

        LLSD row;
        row["columns"][COL_ICON]["column"] = "icon";
        row["columns"][COL_ICON]["type"]   = "icon";
        row["columns"][COL_ICON]["value"]  = getIconNameForAvatar(avatar);
        row["columns"][COL_NAME]["column"] = "name";
        row["columns"][COL_NAME]["value"]  = av_name.getDisplayName();
        row["columns"][COL_UUID]["column"] = "uuid";
        row["columns"][COL_UUID]["value"]  = uuid;
        row["columns"][COL_SAVE]["column"] = "saveFileName";
        row["columns"][COL_SAVE]["value"]  = "";
        LLScrollListItem* item             = mAvatarSelectionScrollList->addElement(row);
    }

    // Add Animesh avatars
    for (auto character : LLControlAvatar::sInstances)
    {
        LLUUID uuid = character->getID();
        if (std::find(avatarsToAddToList.begin(), avatarsToAddToList.end(), uuid) == avatarsToAddToList.end())
            continue;

        LLControlAvatar* avatar = dynamic_cast<LLControlAvatar* >(character);
        if (!couldAnimateAvatar(avatar))
            continue;

        LLAvatarName av_name;
        std::string animeshName = getControlAvatarName(avatar);
        if (animeshName.empty())
            continue;

        LLSD row;
        row["columns"][COL_ICON]["column"] = "icon";
        row["columns"][COL_ICON]["type"]   = "icon";
        row["columns"][COL_ICON]["value"]  = iconObjectName;
        row["columns"][COL_NAME]["column"] = "name";
        row["columns"][COL_NAME]["value"]  = animeshName;
        row["columns"][COL_UUID]["column"] = "uuid";
        row["columns"][COL_UUID]["value"]  = avatar->getID();
        row["columns"][COL_SAVE]["column"] = "saveFileName";
        row["columns"][COL_SAVE]["value"]  = "";
        mAvatarSelectionScrollList->addElement(row);
    }

    mAvatarSelectionScrollList->updateLayout();
    refreshTextHighlightingOnAvatarScrollList();
}

std::string FSFloaterPoser::getIconNameForAvatar(LLVOAvatar* avatar)
{
    std::string iconName = "Inv_BodyShape";
    if (hasXuiString("icon_category"))
        iconName = getString("icon_category");

    if (!avatar)
        return iconName;

    if (avatar->isControlAvatar() && hasXuiString("icon_object"))
        return getString("icon_object");

    return iconName;
}

std::string FSFloaterPoser::getControlAvatarName(const LLControlAvatar* avatar)
{
    if (!avatar)
        return {};

    const LLVOVolume* rootVolume = avatar->mRootVolp;
    const LLViewerObject* rootEditObject = rootVolume ? rootVolume->getRootEdit() : nullptr;
    if (!rootEditObject)
        return {};

    const LLViewerInventoryItem* attachedItem =
        (rootEditObject->isAttachment()) ? gInventory.getItem(rootEditObject->getAttachmentItemID()) : nullptr;

    if (attachedItem)
        return attachedItem->getName();

    if (rootEditObject->permYouOwner())
        return avatar->getFullname();

    return {};
}

void FSFloaterPoser::refreshTextHighlightingOnAvatarScrollList()
{
    for (auto listItem : mAvatarSelectionScrollList->getAllData())
    {
        LLScrollListCell* cell = listItem->getColumn(COL_UUID);
        if (!cell)
            continue;

        LLUUID selectedAvatarId = cell->getValue().asUUID();
        LLVOAvatar* listAvatar = getAvatarByUuid(selectedAvatarId);

        ((LLScrollListText*)listItem->getColumn(COL_ICON))->setValue(getIconNameForAvatar(listAvatar));

        if (mPoserAnimator.isPosingAvatar(listAvatar))
            ((LLScrollListText*)listItem->getColumn(COL_NAME))->setFontStyle(LLFontGL::BOLD);
        else
            ((LLScrollListText*)listItem->getColumn(COL_NAME))->setFontStyle(LLFontGL::NORMAL);
    }
}

void FSFloaterPoser::refreshTextHighlightingOnJointScrollLists()
{
    LLVOAvatar *avatar = getUiSelectedAvatar();

    addBoldToScrollList(mBodyJointsScrollList, avatar);
    addBoldToScrollList(mFaceJointsScrollList, avatar);
    addBoldToScrollList(mHandJointsScrollList, avatar);
    addBoldToScrollList(mMiscJointsScrollList, avatar);
    addBoldToScrollList(mCollisionVolumesScrollList, avatar);
}

void FSFloaterPoser::setSavePosesButtonText(bool setAsSaveDiff)
{
    setAsSaveDiff ? mSavePosesBtn->setLabel(tryGetString("SaveDiffLabel")) : mSavePosesBtn->setLabel(tryGetString("SavePoseLabel"));
}

void FSFloaterPoser::addBoldToScrollList(LLScrollListCtrl* list, LLVOAvatar* avatar)
{
    if (!avatar)
        return;

    if (!list)
        return;

    std::string iconValue                    = "";
    bool        considerExternalFormatSaving = getSavingToBvh();

    for (auto listItem : list->getAllData())
    {
        FSPoserAnimator::FSPoserJoint* poserJoint = static_cast<FSPoserAnimator::FSPoserJoint*>(listItem->getUserdata());
        if (!poserJoint)
            continue;

        ((LLScrollListText*)listItem->getColumn(COL_ICON))->setValue(getScrollListIconForJoint(avatar, *poserJoint));

        if (mPoserAnimator.isPosingAvatarJoint(avatar, *poserJoint))
            ((LLScrollListText*)listItem->getColumn(COL_NAME))->setFontStyle(LLFontGL::BOLD);
        else
            ((LLScrollListText*)listItem->getColumn(COL_NAME))->setFontStyle(LLFontGL::NORMAL);
    }
}

std::string FSFloaterPoser::getScrollListIconForJoint(LLVOAvatar* avatar, FSPoserAnimator::FSPoserJoint joint)
{
    if (!avatar)
        return {};

    if (mPoserAnimator.getRotationIsWorldLocked(avatar, joint))
        return tryGetString("icon_rotation_is_world_locked");

    if (!getSavingToBvh())
        return {};

    if (joint.boneType() == COL_VOLUMES)
        return tryGetString("icon_rotation_does_not_export");

    if (mPoserAnimator.userSetBaseRotationToZero(avatar, joint))
    {
        if (mPoserAnimator.exportRotationWillLockJoint(avatar, joint))
            return tryGetString("icon_rotation_bvh_locked_edited");
        else
            return tryGetString("icon_rotation_bvh_locked_unedited");
    }
    else
        return tryGetString("icon_rotation_bvh_unlocked");
}

std::string FSFloaterPoser::tryGetString(std::string_view name)
{
    if (name.empty())
        return {};

    return hasXuiString(name) ? getString(name) : "";
}

bool FSFloaterPoser::savePoseToBvh(LLVOAvatar* avatar, const std::string& poseFileName)
{
    if (poseFileName.empty())
        return false;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return false;

    bool writeSuccess = false;

    try
    {
        std::string pathname = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY);
        if (!gDirUtilp->fileExists(pathname))
        {
            LL_WARNS("Poser") << "Couldn't find folder: " << pathname << " - creating one." << LL_ENDL;
            LLFile::mkdir(pathname);
        }

        std::string fullSavePath =
            gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY, poseFileName + POSE_EXTERNAL_FORMAT_FILE_EXT);

        // As savePoseToXml(): written next to it, checked, then moved over it (a full disk does
        // not leave a cut-off file in place of the old one).
        const std::string tempSavePath = fullSavePath + ".tmp";
        {
            llofstream file(tempSavePath.c_str());
            if (!file.is_open())
            {
                LL_WARNS("Poser") << "Unable to save pose!" << LL_ENDL;
                return false;
            }

            writeSuccess = writePoseAsBvh(&file, avatar);

            file.close();
            if (!writeSuccess || file.fail())
            {
                LL_WARNS("Poser") << "Unable to save pose: cannot write " << tempSavePath << LL_ENDL;
                LLFile::remove(tempSavePath);
                return false;
            }
        }
        if (LLFile::rename(tempSavePath, fullSavePath) != 0)
        {
            LL_WARNS("Poser") << "Unable to save pose: cannot replace " << fullSavePath << LL_ENDL;
            LLFile::remove(tempSavePath);
            return false;
        }
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Posing") << "Exception caught in SaveToBVH: " << e.what() << LL_ENDL;
        return false;
    }

    return writeSuccess;
}

bool FSFloaterPoser::writePoseAsBvh(llofstream* fileStream, LLVOAvatar* avatar)
{
    if (!fileStream || !avatar)
        return false;

    *fileStream << "HIERARCHY" << std::endl;
    auto startingJoint = mPoserAnimator.getPoserJointByName("mPelvis");
    writeBvhFragment(fileStream, avatar, startingJoint, 0);
    *fileStream << "MOTION" << std::endl;
    *fileStream << "Frames:    2" << std::endl;
    *fileStream << "Frame Time: 1" << std::endl;
    writeFirstFrameOfBvhMotion(fileStream, startingJoint);
    *fileStream << std::endl;
    writeBvhMotion(fileStream, avatar, startingJoint);
    *fileStream << std::endl;

    return true;
}

void FSFloaterPoser::writeBvhFragment(llofstream* fileStream, LLVOAvatar* avatar, const FSPoserAnimator::FSPoserJoint* joint, S32 tabStops)
{
    if (!joint)
        return;

    auto saveAxis = getBvhJointTranslation(joint->jointName());

    switch (joint->boneType())
    {
        case WHOLEAVATAR:
            *fileStream << "ROOT " + joint->jointName() << std::endl;
            *fileStream << "{" << std::endl;
            *fileStream << getTabs(tabStops + 1) + "OFFSET " + joint->bvhOffset() << std::endl;
// <AS:chanayane> BVH fixes
            *fileStream << getTabs(tabStops + 1) + "CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation" << std::endl;
// </AS:chanayane>
            break;

        default:
            *fileStream << getTabs(tabStops) + "JOINT " + joint->jointName() << std::endl;
            *fileStream << getTabs(tabStops) + "{" << std::endl;
            *fileStream << getTabs(tabStops + 1) + "OFFSET " + joint->bvhOffset() << std::endl;

            switch (saveAxis)
            {
                default:
                case SWAP_NOTHING:
                    *fileStream << getTabs(tabStops + 1) + "CHANNELS 3 Xrotation Yrotation Zrotation" << std::endl;
                    break;

                case SWAP_YAW_AND_ROLL:
                    *fileStream << getTabs(tabStops + 1) + "CHANNELS 3 Zrotation Yrotation Xrotation" << std::endl;
                    break;

                case SWAP_ROLL_AND_PITCH:
                    *fileStream << getTabs(tabStops + 1) + "CHANNELS 3 Xrotation Zrotation Yrotation" << std::endl;
                    break;

                case SWAP_X2Z_Y2X_Z2Y:
                    *fileStream << getTabs(tabStops + 1) + "CHANNELS 3 Yrotation Zrotation Xrotation" << std::endl;
                    break;

                case SWAP_X2Y_Y2Z_Z2X:
                    *fileStream << getTabs(tabStops + 1) + "CHANNELS 3 Zrotation Xrotation Yrotation" << std::endl;
                    break;

                case SWAP_YAW_AND_PITCH:
                    *fileStream << getTabs(tabStops + 1) + "CHANNELS 3 Yrotation Xrotation Zrotation" << std::endl;
                    break;
            }
            break;
    }

    size_t numberOfBvhChildNodes = joint->bvhChildren().size();
    if (numberOfBvhChildNodes > 0)
    {
        for (size_t index = 0; index != numberOfBvhChildNodes; ++index)
        {
            auto nextJoint = mPoserAnimator.getPoserJointByName(joint->bvhChildren().at(index));
            writeBvhFragment(fileStream, avatar, nextJoint, tabStops + 1);
        }
    }
    else
    {
        // append the End Site offset
        *fileStream << getTabs(tabStops + 1) + "End Site" << std::endl;
        *fileStream << getTabs(tabStops + 1) + "{" << std::endl;
        *fileStream << getTabs(tabStops + 2) + "OFFSET " + joint->bvhEndSite() << std::endl;
        *fileStream << getTabs(tabStops + 1) + "}" << std::endl;
    }

    *fileStream << getTabs(tabStops) + "}" << std::endl;
}

void FSFloaterPoser::writeFirstFrameOfBvhMotion(llofstream* fileStream, const FSPoserAnimator::FSPoserJoint* joint)
{
    if (!joint)
        return;

    switch (joint->boneType())
    {
        case WHOLEAVATAR:
            *fileStream << "0.000000 0.000000 0.000000 0.0 0.0 0.0";
            break;

        default:
            *fileStream << "  0.0 0.0 0.0";
            break;
    }

    size_t numberOfBvhChildNodes = joint->bvhChildren().size();
    for (size_t index = 0; index != numberOfBvhChildNodes; ++index)
    {
        auto nextJoint = mPoserAnimator.getPoserJointByName(joint->bvhChildren().at(index));
        writeFirstFrameOfBvhMotion(fileStream, nextJoint);
    }
}

void FSFloaterPoser::writeBvhMotion(llofstream* fileStream, LLVOAvatar* avatar, const FSPoserAnimator::FSPoserJoint* joint)
{
    if (!joint)
        return;

    bool lockPelvisJoint = gSavedSettings.getBOOL(POSER_UNLOCKPELVISINBVH_SAVE_KEY);

    auto rotation = mPoserAnimator.getJointExportRotation(avatar, *joint, !lockPelvisJoint);
    auto position = mPoserAnimator.getJointPosition(avatar, *joint);

    switch (joint->boneType())
    {
        case WHOLEAVATAR:
            *fileStream << positionToString(position) + " " + rotationToString(rotation);
            break;

        default:
            *fileStream << " " + rotationToString(rotation);
            break;
    }

    size_t numberOfBvhChildNodes = joint->bvhChildren().size();
    for (size_t index = 0; index != numberOfBvhChildNodes; ++index)
    {
        auto nextJoint = mPoserAnimator.getPoserJointByName(joint->bvhChildren().at(index));
        writeBvhMotion(fileStream, avatar, nextJoint);
    }
}

std::string FSFloaterPoser::positionToString(const LLVector3& val)
{
    constexpr F32 metersToInches = 39.37008f;
    return std::to_string(metersToInches * val[VY]) + " " + std::to_string(metersToInches * val[VZ]) + " " + std::to_string(metersToInches * val[VX]);
}

std::string FSFloaterPoser::rotationToString(const LLVector3& val)
{
    return std::to_string(val[VX] * RAD_TO_DEG) + " " + std::to_string(val[VY] * RAD_TO_DEG) + " " + std::to_string(val[VZ] * RAD_TO_DEG);
}

std::string FSFloaterPoser::getTabs(S32 numOfTabstops)
{
    std::string tabSpaces;
    for (S32 i = 0; i < numOfTabstops; i++)
        tabSpaces += "\t";

    return tabSpaces;
}

E_BoneAxisTranslation FSFloaterPoser::getBvhJointTranslation(const std::string& jointName) const
{
    if (jointName.empty())
        return SWAP_X2Y_Y2Z_Z2X;

    bool hasTransformParameter = hasXuiString(BVH_JOINT_TRANSFORM_STRING_PREFIX + jointName);
    if (!hasTransformParameter)
        return SWAP_X2Y_Y2Z_Z2X;

    std::string paramValue = getString(BVH_JOINT_TRANSFORM_STRING_PREFIX + jointName);

    if (strstr(paramValue.c_str(), "SWAP_YAW_AND_ROLL"))
        return SWAP_YAW_AND_ROLL;
    else if (strstr(paramValue.c_str(), "SWAP_YAW_AND_PITCH"))
        return SWAP_YAW_AND_PITCH;
    else if (strstr(paramValue.c_str(), "SWAP_ROLL_AND_PITCH"))
        return SWAP_ROLL_AND_PITCH;
    else if (strstr(paramValue.c_str(), "SWAP_X2Y_Y2Z_Z2X"))
        return SWAP_X2Y_Y2Z_Z2X;
    else if (strstr(paramValue.c_str(), "SWAP_X2Z_Y2X_Z2Y"))
        return SWAP_X2Z_Y2X_Z2Y;
    else
        return SWAP_NOTHING;
}

S32 FSFloaterPoser::getBvhJointNegation(const std::string& jointName) const
{
    S32 result = NEGATE_NOTHING;

    if (jointName.empty())
        return result;

    bool hasTransformParameter = hasXuiString(BVH_JOINT_TRANSFORM_STRING_PREFIX + jointName);
    if (!hasTransformParameter)
        return result;

    std::string paramValue = getString(BVH_JOINT_TRANSFORM_STRING_PREFIX + jointName);

    if (strstr(paramValue.c_str(), "NEGATE_YAW"))
        result |= NEGATE_YAW;
    if (strstr(paramValue.c_str(), "NEGATE_PITCH"))
        result |= NEGATE_PITCH;
    if (strstr(paramValue.c_str(), "NEGATE_ROLL"))
        result |= NEGATE_ROLL;
    if (strstr(paramValue.c_str(), "NEGATE_ALL"))
        return NEGATE_ALL;

    return result;
}

bool FSFloaterPoser::getSavingToBvh() const
{
    return gSavedSettings.getBOOL(POSER_SAVEEXTERNALFORMAT_SAVE_KEY);
}

void FSFloaterPoser::onClickSavingToBvh()
{
    mUnlockPelvisInBvhSaveCbx->setVisible(getSavingToBvh());
    refreshTextHighlightingOnJointScrollLists();
}

void FSFloaterPoser::onClickLockWorldRotBtn()
{
    auto selectedJoints = getUiSelectedPoserJoints();
    if (selectedJoints.empty())
        return;

    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return;

    if (!mPoserAnimator.isPosingAvatar(avatar))
        return;

    for (auto item : selectedJoints)
    {
        bool currentlyPosingJoint = mPoserAnimator.isPosingAvatarJoint(avatar, *item);
        if (!currentlyPosingJoint)
            continue;

        bool newLockState = !mPoserAnimator.getRotationIsWorldLocked(avatar, *item);
        mPoserAnimator.setRotationIsWorldLocked(avatar, *item, newLockState);
    }

    refreshTextHighlightingOnJointScrollLists();
}

FSLoadPoseTimer::FSLoadPoseTimer(FSLoadPoseTimer::callback_t callback) : LLEventTimer(0.5f), mCallback(callback)
{
}

bool FSLoadPoseTimer::tick()
{
    if (!mAttemptLoading)
        return false;
    if (!mCallback)
        return false;

    if (mLoadAttempts >= mMaxLoadAttempts)
        mAttemptLoading = false;

    mLoadAttempts++;
    mCallback();
    return false;
}

void FSLoadPoseTimer::tryLoading(std::string filePath, E_LoadPoseMethods loadMethod)
{
    mPoseFullPath   = filePath;
    mLoadType       = loadMethod;
    mLoadAttempts   = 0;
    mAttemptLoading = true;
}

// <plus> hand tab "Easy" (research/315). Joint values are the truth, the sliders are their copy (handcurl.h).
namespace
{
constexpr const char* HAND_DIGIT_NAMES[plus::handcurl::HAND_DIGITS] = { "Thumb", "Index", "Middle", "Ring", "Pinky" };
constexpr F32         HAND_EASY_WRITE_EPSILON                       = 1e-5f;
constexpr F32         HAND_EASY_SHOWN_EPSILON                       = 0.006f; // a slider already this close to its joint value is left alone (the sliders keep multiples of their 0.01 increment: half of it)

// The joints of a hand as bits, for writeHandEasy's "the joints this slider can move": bit digit * 3 + joint.
constexpr U32 HAND_EASY_KNUCKLE        = 1; // joint 1 of a digit
constexpr U32 HAND_EASY_JOINTS_2_AND_3 = 6;
constexpr U32 HAND_EASY_ALL_JOINTS     = 7;
constexpr U32 handEasyJointsOfDigit(S32 digit, U32 joints) { return joints << (3 * digit); }
constexpr U32 handEasyJointsOfFingers(U32 joints)
{
    U32 bits = 0;
    for (S32 digit = 1; digit < plus::handcurl::HAND_DIGITS; ++digit) // (digit 0 is the thumb)
        bits |= handEasyJointsOfDigit(digit, joints);
    return bits;
}

bool handEasyDiffers(const plus::handcurl::Raw& a, const plus::handcurl::Raw& b)
{
    return std::fabs(a.x - b.x) > HAND_EASY_WRITE_EPSILON || std::fabs(a.y - b.y) > HAND_EASY_WRITE_EPSILON ||
           std::fabs(a.z - b.z) > HAND_EASY_WRITE_EPSILON;
}
} // namespace

bool FSFloaterPoser::isHandEasyRight() const
{
    return mHandEasySideCmb && mHandEasySideCmb->getValue().asString() == "right";
}

LLVOAvatar* FSFloaterPoser::getHandEasyAvatar() const
{
    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar)
        return nullptr;
    if (!havePermissionToAnimateAvatar(avatar) && !havePermissionToAnimateOtherAvatar(avatar))
        return nullptr;
    if (!mPoserAnimator.isPosingAvatar(avatar))
        return nullptr;
    return avatar;
}

const FSPoserAnimator::FSPoserJoint* FSFloaterPoser::getHandEasyJoint(bool isRightHand, S32 digit, S32 joint) const
{
    if (digit < 0 || digit >= plus::handcurl::HAND_DIGITS || joint < 0 || joint > 2)
        return nullptr;
    // the joint list of the animator does not change after it is built: the 30 joints are looked up once
    const FSPoserAnimator::FSPoserJoint*& cached = mHandEasyJointCache[isRightHand ? 1 : 0][digit][joint];
    if (!cached)
    {
        const std::string name = std::string("mHand") + HAND_DIGIT_NAMES[digit] + char('1' + joint) + (isRightHand ? "Right" : "Left");
        cached                 = mPoserAnimator.getPoserJointByName(name);
    }
    return cached;
}

/// Reads the 15 joints as they are seen (public x base rotation, as bone-frame angles: the numbers the hand preset
/// files hold). With a base rotation left in a joint (a Bento hand animation that ran when the posing began) the
/// public part alone is not what is on the screen.
bool FSFloaterPoser::readHandEasy(LLVOAvatar* avatar, bool isRightHand, plus::handcurl::HandRaw& hand)
{
    for (S32 d = 0; d < plus::handcurl::HAND_DIGITS; ++d)
    {
        for (S32 k = 0; k < 3; ++k)
        {
            const FSPoserAnimator::FSPoserJoint* joint = getHandEasyJoint(isRightHand, d, k);
            LLVector3                            v;
            bool                                 baseIsZero = true;
            if (!joint || !mPoserAnimator.getJointSeenRotation(avatar, *joint, &v, &baseIsZero))
                return false;
            hand.joint[d][k] = plus::handcurl::Raw{ v.mV[VX], v.mV[VY], v.mV[VZ] };
        }
    }
    return true;
}

/// Writes the joints this action moved (and every joint that still has a base rotation, which is zeroed, the way
/// loading a hand preset does, so that what is written is what is seen). A joint locked to the world is not written
/// (the correction a moved parent gives it would be written over), and neither is one the user took out of the pose
/// ("pose this joint" off: the value would not show, and would jump in when the joint is posed again), as the rotation
/// controls leave it.
/// A slider writes a digit whole or not at all: sliderJoints are the joints the slider can move (bit digit * 3 + joint;
/// the same for a slider whatever the pose is, so that it does not start or stop working in the middle of a drag), and
/// when one of them (or a joint the slider moved outside them) cannot be written, none of that digit's joints is.
/// (The value of a slider is read from all the joints of its digit, the ones that cannot be written too: with a part
/// of the digit written, the reading stays behind the slider, and every further step of a drag adds that difference to
/// the joints that can be written again - they ran far past the end of their path.) With sliderJoints 0 (a copy: one
/// write of given values, nothing is read back and added) each joint is written or left on its own.
/// Returns whether a joint the action moved was written (the zeroing of a left-over base rotation alone does not count).
bool FSFloaterPoser::writeHandEasy(LLVOAvatar* avatar, bool isRightHand, const plus::handcurl::HandRaw& before,
                                   const plus::handcurl::HandRaw& after, U32 sliderJoints)
{
    const bool savingToExternal = getSavingToBvh();
    bool       wrote            = false; // any joint
    bool       wroteMoved       = false; // a joint the action moved
    auto       canWrite         = [this, avatar](const FSPoserAnimator::FSPoserJoint* joint)
    {
        return joint && mPoserAnimator.isPosingAvatarJoint(avatar, *joint) && !mPoserAnimator.getRotationIsWorldLocked(avatar, *joint);
    };
    for (S32 d = 0; d < plus::handcurl::HAND_DIGITS; ++d)
    {
        bool digitRefused = false;
        for (S32 k = 0; k < 3 && !digitRefused; ++k)
        {
            // one of the slider's joints, or (a slider's set that left a joint out must not let it through) one a
            // slider moved all the same
            const bool slidersJoint = (sliderJoints & handEasyJointsOfDigit(d, 1u << k)) != 0 ||
                                      (sliderJoints != 0 && handEasyDiffers(before.joint[d][k], after.joint[d][k]));
            digitRefused = slidersJoint && !canWrite(getHandEasyJoint(isRightHand, d, k));
        }
        if (digitRefused)
            continue;

        for (S32 k = 0; k < 3; ++k)
        {
            const FSPoserAnimator::FSPoserJoint* joint = getHandEasyJoint(isRightHand, d, k);
            if (!canWrite(joint))
                continue; // (for a slider, not one of its joints: only its left-over base rotation is not zeroed)
            LLVector3 seen;
            bool      baseIsZero = true;
            mPoserAnimator.getJointSeenRotation(avatar, *joint, &seen, &baseIsZero);
            const plus::handcurl::Raw& from = before.joint[d][k];
            const plus::handcurl::Raw& to   = after.joint[d][k];
            const bool moved = handEasyDiffers(from, to);
            if (baseIsZero && !moved)
                continue; // only the joints this action moved: the rest keep their undo history
            // the part that moves, for the joints locked to the world below this one
            const LLVector3 delta(to.x - from.x, to.y - from.y, to.z - from.z);
            mPoserAnimator.setJointRotation(avatar, joint, LLVector3(to.x, to.y, to.z), delta, NONE, POSER_FRAME_BONE, SWAP_NOTHING,
                                            NEGATE_NOTHING, savingToExternal || !baseIsZero || mPoserAnimator.userSetBaseRotationToZero(avatar, *joint),
                                            ABSOLUTE_ROT); // the mark "the user set the base to zero" (Recapture) is kept
            wrote = true;
            wroteMoved |= moved;
        }
    }
    if (wrote)
    {
        addBoldToScrollList(mHandJointsScrollList, avatar); // the marks of the joint list, as the rotation sliders do
        refreshRotationControlsOfSelectedJoint();           // the rotation spinners and the trackball must not keep the old angles
        refreshTrackpadCursor();                            // (the trackball's cursor is made from the spinners)
        enableOrDisableRedoAndUndoButton();                 // a written joint has a step to undo (and no step to redo any more)
    }
    return wroteMoved;
}

/// id: "grip" / "fan" (whole hand), "thumb_wrap" / "thumb_swing" / "thumb_arch" / "thumb_bend", "curl_N" / "sprd_N" (N = 1..4: index..pinky).
void FSFloaterPoser::onHandEasyCommit(LLUICtrl* ctrl, const std::string& id)
{
    using namespace plus::handcurl;
    LLVOAvatar* avatar = getHandEasyAvatar();
    const bool  right  = isHandEasyRight();
    HandRaw     before;
    if (!ctrl || !avatar || !readHandEasy(avatar, right, before))
    {
        refreshHandEasy(); // nothing to write to (not posing, no permission): the slider goes back to what the joints show
        return;
    }

    HandRaw hand = before;
    const F32 v  = clampf((F32)ctrl->getValue().asReal(), -1.f, 1.f);
    // sliderJoints: the joints the slider can move (handcurl.h): a curl all three joints of its finger, a spread the
    // knuckle, the thumb's wrap, swing and arch Thumb1 and with it Thumb2 and 3 (they keep their bend), its bend Thumb2 and 3
    U32 sliderJoints = 0;
    if (id == "grip")
    {
        shiftAllFingerCurl(hand, right, v - meanFingerCurl(hand, right));
        sliderJoints = handEasyJointsOfFingers(HAND_EASY_ALL_JOINTS);
    }
    else if (id == "fan")
    {
        shiftAllFingerSpread(hand, right, v - meanFingerSpread(hand, right));
        sliderJoints = handEasyJointsOfFingers(HAND_EASY_KNUCKLE);
    }
    else if (id == "thumb_wrap" || id == "thumb_swing" || id == "thumb_arch")
    {
        if (id == "thumb_wrap")
            setThumbWrap(hand, right, v);
        else if (id == "thumb_swing")
            setThumbSwing(hand, right, v);
        else
            setThumbArch(hand, right, v);
        sliderJoints = handEasyJointsOfDigit(0, HAND_EASY_ALL_JOINTS);
    }
    else if (id == "thumb_bend")
    {
        setThumbBend(hand, right, v);
        sliderJoints = handEasyJointsOfDigit(0, HAND_EASY_JOINTS_2_AND_3);
    }
    else if (id.size() == 6 && (id.compare(0, 5, "curl_") == 0 || id.compare(0, 5, "sprd_") == 0))
    {
        const S32 number = id[5] - '0';
        if (number < 1 || number > FINGER_COUNT)
            return;
        const Finger finger = static_cast<Finger>(number - 1);
        if (id[0] == 'c')
        {
            setFingerCurl(hand, right, finger, v);
            sliderJoints = handEasyJointsOfDigit(slotOf(finger), HAND_EASY_ALL_JOINTS);
        }
        else
        {
            setFingerSpread(hand, right, finger, v);
            sliderJoints = handEasyJointsOfDigit(slotOf(finger), HAND_EASY_KNUCKLE);
        }
    }
    else
        return;

    // The slider that was dragged stays where it is (a whole-hand bar that clamped would jump back). When none of the
    // joints it moves was written (its digit has a joint that is out of the pose or locked to the world among the
    // slider's: see writeHandEasy; or there was nothing to move) it goes back to what the joints show, like the others.
    const bool wroteMoved = writeHandEasy(avatar, right, before, hand, sliderJoints);
    refreshHandEasy(wroteMoved ? ctrl : nullptr);
}

/// Copies the other hand onto this one so that both look alike (every joint mirrored). A joint of this hand that is out
/// of the pose or locked to the world keeps its value, joint by joint (the rest of its finger is copied).
void FSFloaterPoser::onHandEasyCopy(bool toRightHand)
{
    using namespace plus::handcurl;
    LLVOAvatar* avatar = getHandEasyAvatar();
    if (!avatar)
    {
        showHandEasyMessage("hand_no_pose", std::string());
        return;
    }

    HandRaw source, before;
    if (!readHandEasy(avatar, !toRightHand, source) || !readHandEasy(avatar, toRightHand, before))
    {
        showHandEasyMessage("hand_read_failed", std::string());
        return;
    }

    writeHandEasy(avatar, toRightHand, before, mirrorHand(source), 0); // (0: no slider's joints, each joint on its own)
    refreshHandEasy();
}

/// Brings the sliders to what the joints show. Does nothing while the tab is hidden, and a slider that already shows
/// its value is not written (the gizmo on a hand joint calls this at every mouse move).
void FSFloaterPoser::refreshHandEasy(const LLUICtrl* skipControl)
{
    using namespace plus::handcurl;
    if (!mHandEasyPnl || !mHandEasyPnl->isInVisibleChain())
        return;

    const bool  right  = isHandEasyRight();
    HandRaw     hand;
    LLVOAvatar* avatar = getUiSelectedAvatar();
    if (!avatar || !readHandEasy(avatar, right, hand))
        hand = HandRaw();

    auto setValueIfChanged = [skipControl](LLSliderCtrl* slider, F32 value)
    {
        if (slider && slider != skipControl && std::fabs((F32)slider->getValue().asReal() - value) > HAND_EASY_SHOWN_EPSILON)
            slider->setValue(value);
    };
    auto markCustom = [](LLTextBox* label, bool& shown, bool custom)
    {
        if (!label || shown == custom)
            return;
        shown = custom;
        label->setColor(LLUIColorTable::instance().getColor(custom ? "EmphasisColor" : "LabelTextColor"));
    };

    for (S32 i = 0; i < FINGER_COUNT; ++i)
    {
        const Finger finger = static_cast<Finger>(i);
        bool         custom = false;
        setValueIfChanged(mHandEasyCurl[i], getFingerCurl(hand, right, finger, &custom));
        markCustom(mHandEasyCurlLabel[i], mHandEasyCurlCustom[i], custom);
        // a knuckle outside the slider's range (a preset's hand-made finger): the slider stands at its end, and its
        // first move brings the knuckle into the range; the label says so beforehand, as the curl's does
        setValueIfChanged(mHandEasySpread[i], getFingerSpread(hand, right, finger, &custom));
        markCustom(mHandEasySpreadLabel[i], mHandEasySpreadCustom[i], custom);
    }
    bool custom = false;
    setValueIfChanged(mHandEasyThumbWrap, getThumbWrap(hand, right, &custom));
    markCustom(mHandEasyThumbLabel[0], mHandEasyThumbCustom[0], custom);
    setValueIfChanged(mHandEasyThumbSwing, getThumbSwing(hand, right, &custom));
    markCustom(mHandEasyThumbLabel[1], mHandEasyThumbCustom[1], custom);
    setValueIfChanged(mHandEasyThumbArch, getThumbArch(hand, right, &custom));
    markCustom(mHandEasyThumbLabel[2], mHandEasyThumbCustom[2], custom);
    setValueIfChanged(mHandEasyThumbBend, getThumbBend(hand, right, &custom));
    markCustom(mHandEasyThumbLabel[3], mHandEasyThumbCustom[3], custom);
    setValueIfChanged(mHandEasyGrip, meanFingerCurl(hand, right));
    setValueIfChanged(mHandEasyFan, meanFingerSpread(hand, right));
}

/// A short message to the user (a string of this floater, "[NAME]" replaced by name).
void FSFloaterPoser::showHandEasyMessage(const std::string& stringName, const std::string& name)
{
    std::string                message = tryGetString(stringName);
    LLStringUtil::format_map_t format_args;
    format_args["[NAME]"] = name;
    LLStringUtil::format(message, format_args);
    LLSD args;
    args["MESSAGE"] = message;
    LLNotificationsUtil::add("GenericAlert", args);
}

void FSFloaterPoser::onHandEasySave()
{
    using namespace plus::handcurl;
    // Same rule as the pose save: the typed name becomes a file name (plus::isSafeFileStem), never a path.
    const std::string name = mHandEasySaveNameEditor ? mHandEasySaveNameEditor->getValue().asString() : std::string();
    if (!plus::isSafeFileStem(name))
    {
        showHandEasyMessage("hand_save_failed", name);
        return;
    }

    // A shipped preset is not written over: the user's file would take its place for good (it could not be deleted from
    // here, and the shipped one would not be copied again over the higher version of a user's file).
    if (isShippedHandPreset(name))
    {
        showHandEasyMessage("hand_save_name_taken", name);
        return;
    }

    // Two rows must not show the same text: a name that is the shown name of another preset (a shipped one) is refused.
    for (LLScrollListItem* item : mHandPresetsScrollList->getAllData())
    {
        if (item->getValue().asString() != name && item->getColumn(0)->getValue().asString() == name)
        {
            showHandEasyMessage("hand_save_name_taken", name);
            return;
        }
    }

    LLVOAvatar* avatar = getHandEasyAvatar();
    if (!avatar)
    {
        showHandEasyMessage("hand_no_pose", name);
        return;
    }

    const bool right = isHandEasyRight();
    HandRaw    hand;
    if (!readHandEasy(avatar, right, hand))
    {
        showHandEasyMessage("hand_read_failed", name);
        return;
    }

    const std::string fullPath = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY,
                                                                std::string(POSE_PRESETS_HANDS_SUBDIRECTORY), name + POSE_INTERNAL_FORMAT_FILE_EXT);
    if (!gDirUtilp->fileExists(fullPath))
    {
        saveHandEasyPreset(name, hand, right);
        return;
    }

    std::string                message = tryGetString("hand_overwrite_confirm"); // (a preset of the user's own: shipped ones were refused above)
    LLStringUtil::format_map_t format_args;
    format_args["[NAME]"] = name;
    LLStringUtil::format(message, format_args);
    LLSD args;
    args["MESSAGE"] = message;
    LLHandle<LLFloater> handle = getHandle();
    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(),
        [handle, name, hand, right](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                return;
            if (auto* floater = dynamic_cast<FSFloaterPoser*>(handle.get()))
                floater->saveHandEasyPreset(name, hand, right);
        });
}

/// Writes the hand (as it is seen: base rotation zero) as a preset of the user's hand_presets folder. Both hands are
/// written (the loader of "Set Left" / "Set Right" reads the side it is asked for), the other one mirrored from the
/// chosen one. A failure is told to the user. The name is checked once more here (the one place that writes): a file
/// name, and not a shipped preset's.
bool FSFloaterPoser::saveHandEasyPreset(const std::string& name, const plus::handcurl::HandRaw& hand, bool isRightHand)
{
    using namespace plus::handcurl;
    if (!plus::isSafeFileStem(name))
    {
        showHandEasyMessage("hand_save_failed", name);
        return false;
    }
    if (isShippedHandPreset(name))
    {
        showHandEasyMessage("hand_save_name_taken", name);
        return false;
    }

    createUserPoseDirectoryIfNeeded();

    try
    {
        const HandRaw sides[2] = { isRightHand ? mirrorHand(hand) : hand, isRightHand ? hand : mirrorHand(hand) }; // left, right
        LLSD          record;
        record["version"]["value"] = plus::handcurl::USER_HAND_PRESET_FILE_VERSION; // above every shipped file's: an update does not replace it
        for (S32 side = 0; side < 2; ++side)
        {
            for (S32 d = 0; d < HAND_DIGITS; ++d)
            {
                for (S32 k = 0; k < 3; ++k)
                {
                    const FSPoserAnimator::FSPoserJoint* joint = getHandEasyJoint(side == 1, d, k);
                    if (!joint)
                        continue;
                    const Raw&        r    = sides[side].joint[d][k];
                    const std::string bone = joint->jointName();
                    record[bone]["enabled"]                 = true;
                    record[bone]["jointBaseRotationIsZero"] = true;
                    record[bone]["position"]                = LLVector3::zero.getValue();
                    record[bone]["rotation"]                = LLVector3(r.x, r.y, r.z).getValue();
                    record[bone]["scale"]                   = LLVector3::zero.getValue();
                }
            }
        }

        const std::string fullPath = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY,
                                                                    std::string(POSE_PRESETS_HANDS_SUBDIRECTORY), name + POSE_INTERNAL_FORMAT_FILE_EXT);
        // As savePoseToXml(): written next to it, checked, then moved over it. (The log names the file, not the folder.)
        const std::string tempPath = fullPath + ".tmp";
        {
            llofstream file;
            file.open(tempPath.c_str());
            if (!file.is_open())
            {
                LL_WARNS("Poser") << "Unable to save hand preset " << name << LL_ENDL;
                showHandEasyMessage("hand_save_write_failed", name);
                return false;
            }
            LLSDSerialize::toPrettyXML(record, file);
            file.close();
            if (file.fail())
            {
                LL_WARNS("Poser") << "Unable to save hand preset " << name << ": cannot write the file" << LL_ENDL;
                LLFile::remove(tempPath);
                showHandEasyMessage("hand_save_write_failed", name);
                return false;
            }
        }
        if (LLFile::rename(tempPath, fullPath) != 0)
        {
            LL_WARNS("Poser") << "Unable to save hand preset " << name << ": cannot replace the file" << LL_ENDL;
            LLFile::remove(tempPath);
            showHandEasyMessage("hand_save_write_failed", name);
            return false;
        }
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Posing") << "Exception caught saving a hand preset: " << e.what() << LL_ENDL;
        showHandEasyMessage("hand_save_write_failed", name);
        return false;
    }

    refreshPoseScroll(mHandPresetsScrollList, POSE_PRESETS_HANDS_SUBDIRECTORY);
    return true;
}
// </plus>

// <plus> hand preset list: display names and the click target
/// The one place a hand preset's file name (the identity) becomes the text in the list. The names of the shipped
/// presets come from the floater's strings (hand_preset_name_<file name>, en and ja); any other file (the user's own
/// presets) shows its name as it is.
std::string FSFloaterPoser::handPresetDisplayName(const std::string& fileStem)
{
    const std::string shown = tryGetString("hand_preset_name_" + fileStem);
    return shown.empty() ? fileStem : shown;
}

void FSFloaterPoser::setHandPresetTarget(bool isRightHand)
{
    mHandPresetTarget = isRightHand ? 1 : 0;
    if (mHandPresetLeftBtn)
        mHandPresetLeftBtn->setToggleState(!isRightHand);
    if (mHandPresetRightBtn)
        mHandPresetRightBtn->setToggleState(isRightHand);
}

/// The mouse button went up over the list (the list has selected the row under the pointer by now).
void FSFloaterPoser::onHandPresetClick(S32 x, S32 y, MASK mask)
{
    if (mHandPresetTarget < 0)
        return; // no hand chosen yet: Set Left / Set Right decide the first time
    if (mask != MASK_NONE || !mHandPresetsScrollList->getItemListRect().pointInRect(x, y))
        return; // a modified click, or one on the scroll bar / header
    if (!mHandPresetsScrollList->hitItem(x, y))
        return; // the empty part below the rows: the selection stays, and must not be applied again (it would undo the hand's later edits)
    onClickLoadHandPose(mHandPresetTarget == 1);
}
// </plus>

// <plus> hand preset list: delete (right click)
/// A shipped preset (a file of app_settings) comes back at the next start (createUserPoseDirectoryIfNeeded copies it again), so
/// it is not deleted, and it is not saved over (onHandEasySave). The check is the file in the program's folder, not a list of
/// names; the letters' case does not count ("grip" is the file Grip.xml on Windows, whatever the file system of the program's
/// folder says).
bool FSFloaterPoser::isShippedHandPreset(const std::string& fileStem) const
{
    const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, POSE_PRESETS_SOURCE_DIR_PLUS, POSE_PRESETS_SOURCE_DIR_FEATURE,
                                                           std::string(POSE_PRESETS_HANDS_SUBDIRECTORY));
    const std::string fileName = fileStem + POSE_INTERNAL_FORMAT_FILE_EXT;
    if (gDirUtilp->fileExists(gDirUtilp->add(dir, fileName)))
        return true;
    for (const std::string& shipped : gDirUtilp->getFilesInDir(dir))
    {
        if (LLStringUtil::compareInsensitive(shipped, fileName) == 0)
            return true;
    }
    return false;
}

void FSFloaterPoser::onHandPresetRightClick(S32 x, S32 y)
{
    LLScrollListItem* item = mHandPresetsScrollList->hitItem(x, y);
    if (!item || !LLMenuGL::sMenuContainer)
        return;
    mHandPresetsScrollList->selectItemAt(x, y, MASK_NONE); // the row under the pointer is the one the menu is about

    const std::string fileStem = item->getValue().asString();
    const std::string shown    = item->getColumn(0)->getValue().asString();
    const bool        shipped  = isShippedHandPreset(fileStem);

    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLHandle<LLFloater>                               handle = getHandle();
    registrar.add("Poser.HandPresetDelete",
        [handle, fileStem, shown](LLUICtrl*, const LLSD&)
        {
            auto* floater = dynamic_cast<FSFloaterPoser*>(handle.get());
            if (!floater)
                return;
            std::string                message = floater->tryGetString("hand_delete_confirm");
            LLStringUtil::format_map_t format_args;
            format_args["[NAME]"] = shown;
            LLStringUtil::format(message, format_args);
            LLSD args;
            args["MESSAGE"] = message;
            LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(),
                [handle, fileStem](const LLSD& notification, const LLSD& response)
                {
                    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                        return;
                    if (auto* again = dynamic_cast<FSFloaterPoser*>(handle.get()))
                        again->deleteHandPreset(fileStem);
                });
        });

    if (LLContextMenu* old = mHandPresetMenuHandle.get())
    {
        old->die();
        mHandPresetMenuHandle.markDead();
    }
    LLContextMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>(
        "menu_fs_poser_hand_preset.xml", LLMenuGL::sMenuContainer, LLMenuHolderGL::child_registry_t::instance());
    if (!menu)
        return;
    mHandPresetMenuHandle = menu->getHandle();
    if (LLMenuItemGL* deleteItem = menu->findChild<LLMenuItemGL>("delete"))
    {
        deleteItem->setEnabled(!shipped);
        if (shipped)
            deleteItem->setLabel(tryGetString("hand_delete_builtin_label"));
    }
    menu->show(x, y);
    LLMenuGL::showPopup(mHandPresetsScrollList, menu, x, y);
}

/// Deletes the user's copy of a hand preset (the file of the user's hand_presets folder). The name is the file name the
/// list holds, checked once more by the rule of loading (plus::isLoadableFileStem: no path, no dots that leave the folder);
/// a shipped one is refused again here.
void FSFloaterPoser::deleteHandPreset(const std::string& fileStem)
{
    if (!plus::isLoadableFileStem(fileStem) || isShippedHandPreset(fileStem))
        return;

    const std::string fullPath = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY,
                                                                std::string(POSE_PRESETS_HANDS_SUBDIRECTORY), fileStem + POSE_INTERNAL_FORMAT_FILE_EXT);
    if (gDirUtilp->fileExists(fullPath) && LLFile::remove(fullPath) != 0)
    {
        LL_WARNS("Poser") << "Unable to delete hand preset " << fileStem << LL_ENDL;
        showHandEasyMessage("hand_delete_failed", handPresetDisplayName(fileStem)); // the name the list shows, as the confirmation did
        return;
    }
    // the list is built again: the selection goes with the row; the hand chosen for the click (mHandPresetTarget) stays
    refreshPoseScroll(mHandPresetsScrollList, POSE_PRESETS_HANDS_SUBDIRECTORY);
}
// </plus>
