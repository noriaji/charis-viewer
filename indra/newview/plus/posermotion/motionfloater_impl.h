/**
 * @file motionfloater_impl.h
 * @brief "Motion Between Poses": the window's class, shared by the motionfloater_*.cpp files (not for other code).
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

// The window's members are defined in several files by what they do (motionfloater.cpp: building, the tick; _model: machine M
// and its widgets; _keys: the keys, their chips and menu, the selected segment's inputs; _generate: the queue of generate
// requests and the answers, stopHelper() and dropKeys(); _play: machine C2, the playback of the joined clip, dropClip() and
// dropAll(); _save: save and discard; _view: the widgets, the status line; _keyrow: the row of chips). The class and what it
// is made of live here, in a named namespace (an unnamed one would make each file's copy a different class). Nothing here
// is used outside plus/posermotion.

#ifndef PLUS_POSERMOTION_MOTIONFLOATER_IMPL_H
#define PLUS_POSERMOTION_MOTIONFLOATER_IMPL_H

#include "motionfloater.h"

#include "motionfloater_keyrow.h"
#include "motionplayback.h"
#include "motionposer.h"
#include "motionsession.h"
#include "motiontimeline.h"
#include "motiontimelineview.h"
#include "motiontracks.h"
#include "posermotion.h"
#include "restskeleton.h"

#include "plus/photoscreen/psui/pswidgets.h"
#include "plus/plus.h"
#include "plus/poser/fsposingmotion.h"
#include "plus/posermcp/posehistorytools.h"

#include "llcallbacklist.h"
#include "llmenugl.h"
#include "lleventtimer.h"
#include "llfloater.h"
#include "lllineeditor.h"
#include "lltextbox.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace plus::posermotion
{
    namespace floaterui
    {
        namespace history = plus::posermcp::history;
        using plus::psui::PsButton;
        using plus::psui::PsCheck;
        using plus::psui::PsLabel;
        using plus::psui::PsSegment;
        using plus::psui::PsSlider;

        inline constexpr F32 TICK_SEC          = 0.1f;   // the window's tick while it is open
        inline constexpr F32 GENERATE_TICK_SEC = 0.03f;  // ... while the helper is asked for motions (one answer a tick)
        inline constexpr const char* POSER_FLOATER = "fs_poser";
        inline constexpr const char* HISTORY_SOURCE = "motion";

        // The words of the window (the XML's <string>s, en and ja), read once.
        enum class W : S32
        {
            ModelChecking, ModelMissing, ModelUnpublished, ModelDownloading, ModelVerifying, ModelInstalled,
            ModelFailed, ModelBroken, ModelInUse,
            BtnFetch, BtnCancel, BtnRemove, BtnRetry, BtnRefetch,
            ReasonNotPublished, ReasonCannotStart, ReasonBadFolder, ReasonNetwork, ReasonSize, ReasonHash, ReasonDisk,
            ReasonInUse,
            ConfirmFetch, ConfirmRemove, ConfirmOverwrite, ConfirmClear,
            Seconds, FrameOf, KeySelected, KeyNone, SegmentTitle, SegmentNone,
            MethodGenerate, MethodSmooth, MethodLinear,
            SegmentUnmade, SegmentQueued, SegmentRunning, SegmentMade, SegmentFailed, SegmentRegen, HoldWord, UnitSecond,
            BtnPlay, BtnStop,
            StatusLoading, StatusProgress,
            WhyNotLoggedIn, WhyNoPoser, WhyNotPosing, WhyOther, WhyNoModel, WhyUnpublished, WhyNeedKeys, WhyNotGenerate,
            WhySame,
            FailLowMemory, FailNoHelper, FailBadFolder, FailBadInput, FailModelMissing, FailModelUnreadable,
            FailTimeout, FailHelperEnded, FailRun, FailBadAnswer, FailConvert, FailPrevMissing,
            NoteTouched, NoteGone, NoteSaved, NoteSaveBadName, NoteSaveFailed, NoteDiscarded, NoteDiscardFailed,
            NoteTooManyKeys, NoteTooLong, NoteBusy, NoteSamePoses, NoteCaptureFailed, NoteApplyFailed, NoteNeedPrev,
            PassAuto, PassAutoThrough, PassAutoStop, PassThrough,
            PassWhyEnd, PassWhyNeighbour, PassWhyHold, PassWhySame, PassStopHint, PassPassHint,
            RegenEnds, RegenTargets, RegenContext, RegenPass, RegenJoin,
            Count
        };

        /// gIdleCallbacks for as long as the object says (the playback, C2), removed in every way out.
        class IdleHook
        {
        public:
            ~IdleHook() { stop(); }
            void start(LLCallbackList::callback_t fn, void* data)
            {
                if (mFn)
                {
                    return;
                }
                mFn   = fn;
                mData = data;
                gIdleCallbacks.addFunction(fn, data);
            }
            bool active() const { return mFn != nullptr; }
            void stop()
            {
                if (mFn)
                {
                    gIdleCallbacks.deleteFunction(mFn, mData);
                    mFn   = nullptr;
                    mData = nullptr;
                }
            }

        private:
            LLCallbackList::callback_t mFn   = nullptr;
            void*                      mData = nullptr;
        };

        /// The viewer's side of PoseEditT (motionsession.h): the pose history (plus/posermcp, IK drag's
        /// beginUserEdit / holdUserEdit / endUserEdit) and the poser drawing the targets at once (SnapToTargetScope)
        /// for the motion of the joints the window writes.
        struct ViewerEditBackend
        {
            const ResolvedJoints* joints = nullptr;
            std::optional<FSPosingMotion::SnapToTargetScope> snap;

            bool beginRecord() { return plus::posermcp::beginUserEdit(); }
            void holdRecord() { plus::posermcp::holdUserEdit(); }
            void endRecord(const char* summary) { plus::posermcp::endUserEdit(HISTORY_SOURCE, summary); }
            bool startSnap()
            {
                FSPosingMotion* motion = joints ? resolvedMotion(*joints) : nullptr;
                if (motion)
                {
                    snap.emplace(*motion);
                }
                return motion != nullptr;
            }
            void stopSnap() { snap.reset(); }
        };
        using PoseEdit = PoseEditT<ViewerEditBackend>;

        /// Why "generate" cannot be pressed for the selected segment (the first that holds), or None.
        enum class Why
        {
            None, NotLoggedIn, NoPoser, NotPosing, Other, NoModel, Unpublished, NeedKeys, NotGenerate, Same,
            NeedPrev, // the segment starts at a passed key and the one before it has no answer (research/361)
        };

        /// What the status line says after an action (until the next one).
        enum class Note
        {
            None, Failure, Convert, Touched, Gone, Saved, SaveBadName, SaveFailed, Discarded, DiscardFailed,
            TooManyKeys, TooLong, Busy, SamePoses, CaptureFailed, ApplyFailed, NeedPrev,
        };

        /// The helper's failure as the timeline names it (the window's side of research/357 section 4.1).
        JobError jobErrorOf(MotionFailure failure);

        /// What the selected segment's inputs showed last (they are written only when a number changes).
        struct SegmentShown
        {
            PairId pair;
            S32    method    = -1;
            S32    seconds10 = -1;  // seconds in tenths
            S32    count     = -1;  // answers
            S32    selected  = -2;
            S32    state     = -1;
            bool   operator==(const SegmentShown&) const = default;
        };

        /// What the selected segment's line of marks showed last.
        struct MarkShown
        {
            PairId   pair;
            U8       reasons = 0xff;  // SegmentInfo::regen_reasons
            JobError fail    = JobError::None;
            S32      state   = -1;
            bool     operator==(const MarkShown&) const = default;
        };

        class MotionFloater : public LLFloater
        {
        public:
            // passing through (research/361) is on in the window, which has its controls (the timeline's default is off)
            explicit MotionFloater(const LLSD& key) : LLFloater(key) { mTimeline.setPassingEnabled(true); }
            ~MotionFloater() override { shutdown(); }

            bool postBuild() override;
            void onOpen(const LLSD& key) override;
            void onClose(bool app_quitting) override;

            void onTick();

        private:
            class Ticker : public LLEventTimer
            {
            public:
                explicit Ticker(const LLHandle<LLFloater>& floater) : LLEventTimer(TICK_SEC), mFloater(floater) {}
                bool tick() override
                {
                    if (MotionFloater* f = static_cast<MotionFloater*>(mFloater.get()))
                    {
                        f->onTick();
                    }
                    return false; // keep ticking: the window deletes it
                }
                void setPeriod(F32 seconds) { mPeriod = seconds; }

            private:
                LLHandle<LLFloater> mFloater;
            };

            const std::string& word(W w) const { return mWords[(size_t)w]; }
            std::string        format(W w, const char* tag, const std::string& value) const;

            // actions
            void onModelButton();
            void onSave();
            void onDiscard();
            void confirmFetch();
            void confirmRemove();
            void saveNow(const std::string& name);
            void confirmThen(const std::string& message, std::function<void(MotionFloater&)> yes);

            // the keys and the selected segment (motionfloater_keys.cpp)
            void prepareEdit();                    // before anything that changes the keys or the segments: the playback stops
            bool resultOk(TimelineResult result);  // false: the note says why, the widgets show the model again
            void afterEdit(bool changed);                      // the keys or a segment changed: the joined clip, the playback, the widgets
            void onKeyRow();
            void onKeyMenu(const std::string& what);
            bool keyMenuEnabled(const std::string& what) const;
            void showKeyMenu(KeyId id, S32 x, S32 y);
            void onAddKey();
            void onUpdateKey();
            void onDuplicateKey();
            void onDeleteKey();
            void onLoopKey();
            void onClearKeys();
            void clearKeysNow();
            void onHold();
            void onPassMode();
            void onStrength();
            void selectKey(KeyId id);
            bool applyKeyPose(KeyId id);
            std::optional<history::Pose> offerBefore();
            PairId selectedPair() const;
            bool beginKeyEdit();
            bool beginSegmentEdit(PairId& pair);
            void onMethod();
            void onSecondsSlider();
            void onTimelineEvent();

            // generate (motionfloater_generate.cpp)
            bool prepareRetarget(bool fresh);
            bool refreshTimeline(bool fresh);
            using SelectedRequest = TimelineResult (*)(MotionTimeline& timeline, PairId pair, S32& jobs);
            void generateSelected(SelectedRequest request);
            void onGenerateSegment();
            void onGenerateAll();
            void onGenerateCancel();
            void startGeneration(S32 jobs);
            void pumpGeneration(F64 now);
            void startNextJob(F64 now);
            void takeAnswer();
            void failJob(JobError error);
            void finishGeneration();
            void cancelGeneration();
            void setTickPeriod();

            // machine C (motionfloater_play.cpp)
            bool preparePlayback();
            bool loadPlayback();
            void syncPlayback();
            void onPlay();
            void seekTo(S32 frame);
            void playFrame();
            static void onIdleFrame(void* data) { static_cast<MotionFloater*>(data)->playFrame(); }
            WriteResult writeShown(WriteCheck check);
            void stopPlayback();
            void endSeekEdit();
            void dropClip();
            void dropAll();
            void dropKeys();
            void stopHelper();
            void followAvatar(SelfPoseState state);
            void shutdown();

            // the widgets (motionfloater_view.cpp)
            Why  why(SelfPoseState state) const;
            bool allTargets() const;
            void refresh(SelfPoseState state);
            void refreshKeys(SelfPoseState state);
            void refreshPass();
            void refreshSegment(SelfPoseState state);
            void refreshMark(const SegmentInfo* segment);
            void refreshModel();
            void refreshStatus(SelfPoseState state);
            void refreshFrame();
            void setNote(Note note, const std::string& detail = {});
            void setNoteJob(JobError error);

            std::array<std::string, (size_t)W::Count> mWords;

            PsLabel*      mModelLabel    = nullptr;
            PsButton*     mModelButton   = nullptr;
            MotionKeyRow* mKeyRow        = nullptr;
            PsButton*     mAddKey        = nullptr;
            PsButton*     mLoopKey       = nullptr;
            PsLabel*      mKeyLabel      = nullptr;
            PsSlider*     mHoldSlider    = nullptr;
            PsButton*     mUpdateKey     = nullptr;
            PsButton*     mDuplicateKey  = nullptr;
            PsButton*     mDeleteKey     = nullptr;
            PsSegment*    mPassMode      = nullptr;   // the selected key: Auto | Through (the first choice says the judgement)
            PsSegment*    mStrength      = nullptr;   // Weak | Normal
            PsLabel*      mPassInfo      = nullptr;   // why it cannot be passed, how to stop at it, or how to pass it (Auto judged "stops")
            PsLabel*      mSegmentLabel  = nullptr;
            PsSegment*    mMethodSeg     = nullptr;
            PsSlider*     mSecondsSlider = nullptr;
            LLTextBox*    mSegMark       = nullptr;   // the selected segment's one mark and its reasons, or PrevMissing
            PsButton*     mGenSegment    = nullptr;
            PsButton*     mGenAll        = nullptr;
            PsButton*     mGenCancel     = nullptr;
            LLTextBox*    mStatus        = nullptr;
            MotionTimelineView* mTimelineView = nullptr;
            PsButton*     mPlay          = nullptr;
            PsCheck*      mLoop          = nullptr;
            PsLabel*      mFrameLabel    = nullptr;   // where the playback is ("Frame 12 / 58"); the strip's needle moves it
            LLLineEditor* mName          = nullptr;
            PsButton*     mSave          = nullptr;
            PsButton*     mDiscard       = nullptr;

            std::unique_ptr<Ticker> mTicker;
            bool                    mFastTick = false;     // the tick is GENERATE_TICK_SEC
            LLHandle<LLContextMenu> mMenuHandle;           // the key chips' menu, while it is open

            // the keys and what was made for them (research/357); they outlive the window's closing (dropKeys only)
            MotionTimeline               mTimeline;
            KeyId                        mSelKey = NO_KEY;       // the selected key (the selected segment starts at it)
            BeforeKeeper<history::Pose>  mBefore;                // the pose from before this window first wrote the avatar
            SomaRetarget                 mRetarget;
            bool                         mRetargetOk = false;

            // the queue of generate requests: the timeline holds it, the window asks the helper one at a time
            MotionAnswer mAnswer;
            S32          mGenTotal = 0;         // jobs asked since the queue was last empty (for "done / total")
            PairId       mJobPair;              // the segment the request out is for

            // C: the playback of the joined clip (mTimeline.clip()) and the joints it writes
            ClipPlayback   mPlayback;
            ResolvedJoints mJoints;
            KeyPose        mFrameKey;       // a sampled frame (sized once per clip: no allocation while playing)
            U32            mLoadedLayout = 0;   // layoutVersion / clipVersion of the timeline that mJoints / mPlayback hold
            U32            mLoadedClip   = 0;
            IdleHook       mFrameHook;      // C2 only
            PoseEdit       mEdit;
            bool           mSeekEdit = false;  // a frame was shown by the needle: one history entry until endSeekEdit()
            bool           mLoopOn     = false; // the loop check, read when it changes (not every frame)

            // the status line and what the widgets show (rewritten only when it changes)
            Note          mNote        = Note::None;
            std::string   mNoteText;   // the note's words, made when it is set
            std::string   mProgressText;
            S32           mProgressKey = -1;
            std::string   mStatusShown;
            SegmentShown  mSegShown;
            std::optional<PassView> mPassShown;   // what the selected key's passing controls show (none: not written yet)
            W             mPassAutoWord = W::Count;   // the word on the first choice of mPassMode now
            MarkShown     mMarkShown;
            std::string   mMarkText;
            S32           mModelKey    = -1;
            S32           mSecondsShown = -1;
            S32           mHoldShown   = -1;
            KeyId         mKeyShown    = NO_KEY;
            S32           mKeyIndexShown = -1;
            S32           mFrameShown  = -1;
            S32           mCountShown  = -1;
        };

        // helpers shared by the files (defined where they are used most)
        std::string modelSizeMb();
        W           reasonWord(ModelFailure failure);
        W           failureWord(JobError error);
        void        setEnabledIf(LLView* view, bool on);
        void        setLabelIf(plus::psui::PsCtrl* ctrl, const std::string& text);
        /// The text box shows `text` (and has it as its tool tip) when it differs from `shown`, which keeps it.
        void        setTextIf(LLTextBox* box, std::string& shown, const std::string& text);
    } // namespace floaterui
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONFLOATER_IMPL_H
