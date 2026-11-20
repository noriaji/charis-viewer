/**
 * @file motionfloater.cpp
 * @brief "Motion Between Poses": the window: building, the tick, the registration (the rest of it is in motionfloater_*.cpp, motionfloater_impl.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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
 * $/LicenseInfo$
 */

// What runs when (research/324 section 6): nothing while the window is closed. While it is open, a 0.1 s tick
// (Ticker; 0.03 s while motions are being generated, so that an answer is read each tick) reads the helper's answers and
// asks it for the next one, follows the model's state and the poser, and rewrites the widgets whose words changed.
// While the joined clip plays (C2), an idle callback (IdleHook) writes one frame per frame: the frame from the real time
// (ClipPlayback), sampled without allocation (samplePose), written to the resolved joints (writeSelfFrame). The words are
// read from the XML once (Words) and formatted only when a number in them changes.
//
// What the window holds (research/357): the keys and what was made for them (MotionTimeline), kept when the window closes
// and dropped by dropKeys() only (logout, the own avatar gone, "delete all keys"); the clip, its playback and the pose
// from before are dropped by dropClip() / dropAll(). Every way into C0 goes through dropClip() (and dropAll() for the pose
// from before), every way out of C2 through stopPlayback(); the pose history's entry and the poser's easing are held by
// PoseEdit, which ends with them.

#include "llviewerprecompiledheaders.h"

#include "motionfloater_impl.h"

#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "lltimer.h"

#include <iterator>

namespace plus::posermotion
{
    namespace floaterui
    {
        constexpr const char* WORD_NAMES[] = {
            "model_checking", "model_missing", "model_unpublished", "model_downloading", "model_verifying",
            "model_installed", "model_failed", "model_broken", "model_in_use",
            "btn_fetch", "btn_cancel", "btn_remove", "btn_retry", "btn_refetch",
            "reason_not_published", "reason_cannot_start", "reason_bad_folder", "reason_network", "reason_size",
            "reason_hash", "reason_disk", "reason_in_use",
            "confirm_fetch", "confirm_remove", "confirm_overwrite", "confirm_clear",
            "seconds", "frame_of", "key_selected", "key_none", "segment_title", "segment_none",
            "method_generate", "method_smooth", "method_linear",
            "segment_unmade", "segment_queued", "segment_running", "segment_made", "segment_failed", "segment_regen",
            "hold_word", "unit_second",
            "btn_play", "btn_stop",
            "status_loading", "status_progress",
            "why_not_logged_in", "why_no_poser", "why_not_posing", "why_other", "why_no_model", "why_unpublished",
            "why_need_keys", "why_not_generate", "why_same",
            "fail_low_memory", "fail_no_helper", "fail_bad_folder", "fail_bad_input", "fail_model_missing",
            "fail_model_unreadable", "fail_timeout", "fail_helper_ended", "fail_run", "fail_bad_answer", "fail_convert", "fail_prev_missing",
            "note_touched", "note_gone", "note_saved", "note_save_bad_name", "note_save_failed", "note_discarded",
            "note_discard_failed", "note_too_many_keys", "note_too_long", "note_busy", "note_same_poses",
            "note_capture_failed", "note_apply_failed", "note_need_prev",
            "pass_auto", "pass_auto_through", "pass_auto_stop", "pass_through",
            "pass_why_end", "pass_why_neighbour", "pass_why_hold", "pass_why_same", "pass_stop_hint", "pass_pass_hint",
            "regen_ends", "regen_targets", "regen_context", "regen_pass", "regen_join",
        };
        static_assert(std::size(WORD_NAMES) == (size_t)W::Count, "one XML name per word");
        // the window reads the timeline's and the client's numbers as one (motiontimeline_test.cpp has the same lines for its build)
        static_assert(TIMELINE_TOKENS_AUTO == MOTION_TOKENS_AUTO && TIMELINE_TOKENS_MIN == MOTION_TOKENS_MIN &&
                      TIMELINE_TOKENS_MAX == MOTION_TOKENS_MAX && TIMELINE_TOKENS_DEFAULT == MOTION_TOKENS_DEFAULT,
                      "the timeline's token limits are the client's");
        static_assert(timelineFramesOfTokens(TIMELINE_TOKENS_MIN) == motionFramesOfTokens(MOTION_TOKENS_MIN) &&
                      timelineFramesOfTokens(TIMELINE_TOKENS_MAX) == motionFramesOfTokens(MOTION_TOKENS_MAX),
                      "an answer has the same frames in both");

        // ---------------------------------------------------------------- building
        bool MotionFloater::postBuild()
        {
            for (size_t i = 0; i < std::size(WORD_NAMES); ++i)
            {
                mWords[i] = getString(WORD_NAMES[i]);
            }
            mModelLabel    = getChild<PsLabel>("model_label");
            mModelButton   = getChild<PsButton>("model_button");
            mKeyRow        = getChild<MotionKeyRow>("key_row");
            mAddKey        = getChild<PsButton>("add_key_button");
            mLoopKey       = getChild<PsButton>("loop_key_button");
            mKeyLabel      = getChild<PsLabel>("key_label");
            mHoldSlider    = getChild<PsSlider>("hold_slider");
            mUpdateKey     = getChild<PsButton>("update_key_button");
            mDuplicateKey  = getChild<PsButton>("duplicate_key_button");
            mDeleteKey     = getChild<PsButton>("delete_key_button");
            mPassMode      = getChild<PsSegment>("pass_mode");
            mStrength      = getChild<PsSegment>("pass_strength");
            mPassInfo      = getChild<PsLabel>("pass_info");
            mSegmentLabel  = getChild<PsLabel>("segment_label");
            mMethodSeg     = getChild<PsSegment>("method_seg");
            mSecondsSlider = getChild<PsSlider>("seconds_slider");
            mSegMark       = getChild<LLTextBox>("segment_mark_text");
            mGenSegment    = getChild<PsButton>("generate_segment_button");
            mGenAll        = getChild<PsButton>("generate_all_button");
            mGenCancel     = getChild<PsButton>("generate_cancel_button");
            mStatus        = getChild<LLTextBox>("status_text");
            mTimelineView  = getChild<MotionTimelineView>("timeline");
            mPlay          = getChild<PsButton>("play_button");
            mLoop          = getChild<PsCheck>("loop_check");
            mFrameLabel    = getChild<PsLabel>("frame_label");
            mName          = getChild<LLLineEditor>("name_editor");
            mSave          = getChild<PsButton>("save_button");
            mDiscard       = getChild<PsButton>("discard_button");

            mModelButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onModelButton(); });
            mKeyRow->setCommitCallback([this](LLUICtrl*, const LLSD&) { onKeyRow(); });
            mAddKey->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddKey(); });
            mLoopKey->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLoopKey(); });
            mHoldSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onHold(); });
            mUpdateKey->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUpdateKey(); });
            mDuplicateKey->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDuplicateKey(); });
            mDeleteKey->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDeleteKey(); });
            mPassMode->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPassMode(); });
            mStrength->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStrength(); });
            mMethodSeg->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMethod(); });
            mSecondsSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSecondsSlider(); });
            mGenSegment->setCommitCallback([this](LLUICtrl*, const LLSD&) { onGenerateSegment(); });
            mGenAll->setCommitCallback([this](LLUICtrl*, const LLSD&) { onGenerateAll(); });
            mGenCancel->setCommitCallback([this](LLUICtrl*, const LLSD&) { onGenerateCancel(); });
            mTimelineView->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTimelineEvent(); });
            mPlay->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPlay(); });
            mSave->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSave(); });
            mDiscard->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDiscard(); });
            mLoop->setCommitCallback([this](LLUICtrl*, const LLSD&) { mLoopOn = mLoop->getValue().asBoolean(); });
            mName->setKeystrokeCallback([this](LLLineEditor*, void*) { refresh(selfPoseState()); }, nullptr);

            MotionTimelineView::Strings strings;
            strings.generate = word(W::MethodGenerate);
            strings.smooth   = word(W::MethodSmooth);
            strings.linear   = word(W::MethodLinear);
            strings.hold     = word(W::HoldWord);
            strings.unmade   = word(W::SegmentUnmade);
            strings.queued   = word(W::SegmentQueued);
            strings.running  = word(W::SegmentRunning);
            strings.made     = word(W::SegmentMade);
            strings.failed   = word(W::SegmentFailed);
            strings.regen    = word(W::SegmentRegen);
            strings.second   = word(W::UnitSecond);
            strings.reason_prefix = getString("reason_prefix");   // ":" (a space is added: the XML may trim it) or ja's full-width colon
            if (strings.reason_prefix == ":")
            {
                strings.reason_prefix += ' ';
            }
            for (size_t e = 1; e < MotionTimelineView::Strings::JOB_ERRORS; ++e)
            {
                strings.job_error[e] = word(failureWord((JobError)e));
            }
            strings.regen_reason = { word(W::RegenEnds), word(W::RegenTargets), word(W::RegenContext), word(W::RegenPass) };
            strings.regen_join   = word(W::RegenJoin);
            if (strings.regen_join == ",")
            {
                strings.regen_join += ' '; // as reason_prefix: the XML may trim the space
            }
            mTimelineView->setStrings(strings);

            mSecondsSlider->setValue(LLSD(SEGMENT_DEFAULT_SEC));
            mHoldSlider->setValue(LLSD(0.f));
            mLoopOn = mLoop->getValue().asBoolean();
            mEdit.backend().joints = &mJoints;
            return true;
        }

        void MotionFloater::onOpen(const LLSD& key)
        {
            LLFloater::onOpen(key);
            // the poses come from the poser: open it next to this one
            if (!LLFloaterReg::instanceVisible(POSER_FLOATER))
            {
                LLFloaterReg::showInstance(POSER_FLOATER);
            }
            motionModel().refresh(); // the file and its size only (no hash, no network)
            mModelKey = -1;
            mSegShown = SegmentShown();
            if (!mTicker)
            {
                mTicker   = std::make_unique<Ticker>(getHandle());
                mFastTick = false;
            }
            prepareEdit(); // opened again while open (the menu, the poser's button): nothing plays across the redo below
            // keys kept from before the window was closed: the avatar's skeleton may be another now (a new login), so the
            // clips are made again from it
            mRetargetOk = false;
            if (mTimeline.keyCount() >= 2 && selfPoseState() != SelfPoseState::NotLoggedIn)
            {
                refreshTimeline(true);
            }
            refresh(selfPoseState());
        }

        void MotionFloater::onClose(bool app_quitting)
        {
            // the pose stays as it is (research/324 section 1.3); the clip, the helper and the generating go, the keys stay
            shutdown();
            LLFloater::onClose(app_quitting);
        }

        void MotionFloater::shutdown()
        {
            mTicker.reset();
            mFastTick = false;
            if (LLContextMenu* menu = mMenuHandle.get())
            {
                menu->die();
                mMenuHandle.markDead();
            }
            stopHelper();
            dropAll();
            mEdit.releaseSnap();
        }

        std::string MotionFloater::format(W w, const char* tag, const std::string& value) const
        {
            std::string text = word(w);
            LLStringUtil::format_map_t args;
            args[tag] = value;
            LLStringUtil::format(text, args);
            return text;
        }

        /// "Yes / Cancel" question: yes runs on the window if it is still there when the answer comes.
        void MotionFloater::confirmThen(const std::string& message, std::function<void(MotionFloater&)> yes)
        {
            LLSD args;
            args["MESSAGE"] = message;
            LLHandle<LLFloater> handle = getHandle();
            LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(), [handle, yes](const LLSD& n, const LLSD& r)
            {
                if (LLNotificationsUtil::getSelectedOption(n, r) != 0)
                {
                    return;
                }
                if (MotionFloater* f = static_cast<MotionFloater*>(handle.get()))
                {
                    yes(*f);
                }
            });
        }

        // ---------------------------------------------------------------- the tick
        void MotionFloater::onTick()
        {
            const F64 now = LLTimer::getTotalSeconds();
            const SelfPoseState state = selfPoseState();
            followAvatar(state);
            pumpGeneration(now);
            setTickPeriod();

            if (mSeekEdit && !mTimelineView->dragging())
            {
                endSeekEdit();
            }
            mEdit.releaseSnapWhenDue(now);
            refresh(state);
        }

        /// The poser and the avatar went away (research/324 section 5.3, "the other side is gone"; the rules are
        /// followActions(), motionsession.h). The keys go only with the own avatar (drop_keys).
        void MotionFloater::followAvatar(SelfPoseState state)
        {
            FollowInput in;
            in.state        = state;
            in.has_clip     = mPlayback.hasClip();
            in.joints_alive = in.has_clip && resolvedJointsHold(mJoints, mTimeline.layout());
            in.has_before   = mBefore.has();
            in.has_keys     = mTimeline.keyCount() > 0;
            in.request_out  = mTimeline.busy();
            const FollowActions act = followActions(in);
            if (act.stop_helper)
            {
                stopHelper();
            }
            if (act.cancel_request && mTimeline.busy())
            {
                cancelGeneration();
            }
            if (act.drop_all)
            {
                dropAll();
            }
            if (act.note_gone)
            {
                setNote(Note::Gone);
            }
            if (act.drop_keys)
            {
                dropKeys();
            }
        }
    } // namespace floaterui

    namespace
    {
        void init()
        {
            LLFloaterReg::add(MOTION_FLOATER, "floater_plus_poser_motion.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<floaterui::MotionFloater>);
        }

        plus::FeatureRegistration sRegistration({ "posermotion-ui", &init });
    } // namespace
} // namespace plus::posermotion
