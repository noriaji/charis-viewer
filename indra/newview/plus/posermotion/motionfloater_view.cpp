/**
 * @file motionfloater_view.cpp
 * @brief "Motion Between Poses": the widgets (rewritten only when a value changes) and the status line.
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

// refresh() runs on every tick of the window (0.1 s, 0.03 s while generating) and after every action, so it only compares:
// a widget, a label or the strip is written when what it shows changed (research/149 (R), the speed rules).

#include "llviewerprecompiledheaders.h"

#include "motionfloater_impl.h"

#include <algorithm>
#include <cmath>

namespace plus::posermotion
{
    namespace floaterui
    {
        // ---------------------------------------------------------------- what can be done now
        static bool modelReady()
        {
            return motionModel().state() == ModelState::Installed;
        }

        /// Why "generate" cannot be pressed for the selected segment (the first that holds).
        Why MotionFloater::why(SelfPoseState state) const
        {
            switch (state)
            {
            case SelfPoseState::NotLoggedIn:   return Why::NotLoggedIn;
            case SelfPoseState::NoPoser:       return Why::NoPoser;
            case SelfPoseState::NotPosing:     return Why::NotPosing;
            case SelfPoseState::OtherSelected: return Why::Other;
            case SelfPoseState::Posing:        break;
            }
            if (mTimeline.keyCount() < 2)
            {
                return Why::NeedKeys;
            }
            const SegmentInfo* segment = mTimeline.findSegment(selectedPair());
            if (!segment)
            {
                return Why::NeedKeys;
            }
            if (segment->method != SegMethod::Generate)
            {
                return Why::NotGenerate;
            }
            if (segment->same_poses)
            {
                return Why::Same;
            }
            if (!modelReady())
            {
                return motionModel().published() ? Why::NoModel : Why::Unpublished;
            }
            if (mTimeline.canRequestGenerate(segment->pair) == TimelineResult::NeedPrev)
            {
                return Why::NeedPrev; // the same rule as requestGenerate (a queued or running segment is not this)
            }
            return Why::None;
        }

        /// Some Generate segment has no motion yet (what "generate all" would make).
        bool MotionFloater::allTargets() const
        {
            for (S32 i = 0; i < mTimeline.segmentCount(); ++i)
            {
                const SegmentInfo& segment = mTimeline.segment(i);
                if (segment.method == SegMethod::Generate && !segment.same_poses
                    && (segment.state == SegState::Unmade || segment.state == SegState::Failed))
                {
                    return true;
                }
            }
            return false;
        }

        void setEnabledIf(LLView* view, bool on)
        {
            if (view->getEnabled() != on)
            {
                view->setEnabled(on);
            }
        }

        void setLabelIf(plus::psui::PsCtrl* ctrl, const std::string& text)
        {
            if (ctrl->getLabelText() != text)
            {
                ctrl->setLabelText(text);
            }
        }

        void setTextIf(LLTextBox* box, std::string& shown, const std::string& text)
        {
            if (text != shown)
            {
                shown = text;
                box->setText(text);
                box->setToolTip(text);
            }
        }

        // ---------------------------------------------------------------- the widgets
        void MotionFloater::refresh(SelfPoseState state)
        {
            if (!mGenSegment)
            {
                return; // before postBuild
            }
            refreshModel();
            refreshKeys(state);
            refreshSegment(state);

            const bool posing   = state == SelfPoseState::Posing;
            const bool busy     = mTimeline.busy();
            const bool joinable = mTimeline.keyCount() >= 2;
            const bool playable = posing && joinable && !busy;
            setEnabledIf(mPlay, playable || mPlayback.playing()); // a running playback can always be stopped (the poser may have gone to another avatar)
            setLabelIf(mPlay, word(mPlayback.playing() ? W::BtnStop : W::BtnPlay));
            refreshFrame();
            setEnabledIf(mSave, joinable && !busy && state != SelfPoseState::NotLoggedIn && !mName->getText().empty());
            setEnabledIf(mDiscard, mBefore.has() && posing);

            mTimelineView->setTimeline(mTimeline);
            mTimelineView->setSelection(mSelKey, selectedPair());
            mTimelineView->setEditable(!busy && !mPlayback.playing());
            if (!mTimelineView->dragging())
            {
                // with no clip loaded the needle rests at 0 (a click on the ruler with nothing to show leaves it there otherwise)
                mTimelineView->setPlayheadSec(mPlayback.hasClip() ? (F64)mPlayback.frame() / (F64)CLIP_FPS : 0.0);
            }
            refreshStatus(state);
        }

        /// The chips, the selected key's line and its hold, the buttons of the keys.
        void MotionFloater::refreshKeys(SelfPoseState state)
        {
            const S32 count = mTimeline.keyCount();
            if (mTimeline.indexOf(mSelKey) < 0)
            {
                mSelKey = count > 0 ? mTimeline.key(count - 1).id : NO_KEY; // what was selected is gone: the last one
            }
            mKeyRow->setKeys(mTimeline);
            mKeyRow->setSelected(mSelKey);

            const bool posing = state == SelfPoseState::Posing;
            const bool has    = mSelKey != NO_KEY;
            setEnabledIf(mAddKey, posing && count < TIMELINE_MAX_KEYS);
            setEnabledIf(mLoopKey, count >= 2 && count < TIMELINE_MAX_KEYS && mTimeline.loopHoldSec() != 0.0);
            setEnabledIf(mUpdateKey, posing && has);
            setEnabledIf(mDuplicateKey, has && count < TIMELINE_MAX_KEYS);
            setEnabledIf(mDeleteKey, has);
            setEnabledIf(mHoldSlider, has);

            const S32 index = mTimeline.indexOf(mSelKey);
            if (mSelKey != mKeyShown || index != mKeyIndexShown)
            {
                mKeyShown      = mSelKey;
                mKeyIndexShown = index;
                setLabelIf(mKeyLabel, index >= 0 ? format(W::KeySelected, "[N]", std::to_string(index + 1)) : word(W::KeyNone));
            }
            if (const TimelineKey* key = has ? mTimeline.find(mSelKey) : nullptr)
            {
                const S32 slider10 = (S32)std::lround(mHoldSlider->getValue().asReal() * 10.0);
                const S32 key10    = (S32)std::lround((F64)key->hold_sec * 10.0);
                if (key10 != slider10 && !mHoldSlider->hasMouseCapture())
                {
                    mHoldSlider->setValue(LLSD(key->hold_sec));
                    mHoldShown = -1;
                }
                const S32 shown10 = (S32)std::lround(mHoldSlider->getValue().asReal() * 10.0);
                if (shown10 != mHoldShown)
                {
                    mHoldShown = shown10;
                    mHoldSlider->setDisplayText(format(W::Seconds, "[SEC]", llformat("%.1f", shown10 / 10.0)));
                }
            }
            refreshPass();
        }

        /// The selected key's passing (research/361 sections 1.2 and 1.3), as passViewOf says (the rule is the timeline's and is
        /// tested there): its mode and strength, the automatic judgement on the first choice ("Auto (passes)" / "Auto (stops)"),
        /// and one line: why it cannot be passed, how to stop at it, or how to pass it.
        void MotionFloater::refreshPass()
        {
            const PassView now = passViewOf(mTimeline, mSelKey);
            if (mPassShown && *mPassShown == now)
            {
                return;
            }
            mPassShown = now;
            mPassMode->setValue(LLSD((S32)now.mode));
            mStrength->setValue(LLSD((S32)now.strength));
            const W auto_word = now.judged == 1 ? W::PassAutoThrough : now.judged == 0 ? W::PassAutoStop : W::PassAuto;
            if (auto_word != mPassAutoWord)
            {
                mPassAutoWord = auto_word;
                mPassMode->setChoices({ word(auto_word), word(W::PassThrough) });
            }
            W info = W::Count;
            switch (now.note)
            {
            case PassNote::None:                 break;
            case PassNote::StopHint:             info = W::PassStopHint; break;
            case PassNote::PassHint:             info = W::PassPassHint; break;
            case PassNote::EndKey:               info = W::PassWhyEnd; break;
            case PassNote::NeighbourNotGenerate: info = W::PassWhyNeighbour; break;
            case PassNote::Hold:                 info = W::PassWhyHold; break;
            case PassNote::SamePose:             info = W::PassWhySame; break;
            }
            static const std::string NOTHING;
            setLabelIf(mPassInfo, info == W::Count ? NOTHING : word(info));
            setEnabledIf(mPassMode, now.mode_enabled);
            setEnabledIf(mStrength, now.strength_enabled); // the strength counts only where the motion goes through the key
        }

        /// The selected segment's method, length (Smooth/Linear: the slider; Generate: the timeline's right-end drag) and the buttons that make motions.
        void MotionFloater::refreshSegment(SelfPoseState state)
        {
            const PairId pair = selectedPair();
            const SegmentInfo* segment = pair.valid() ? mTimeline.findSegment(pair) : nullptr;
            const bool have = segment != nullptr;
            const bool busy = mTimeline.busy();
            const bool segment_busy = have && (segment->state == SegState::Queued || segment->state == SegState::Running);
            const bool generate = have && segment->method == SegMethod::Generate;

            if (!have)
            {
                if (mSegShown.method != -2)
                {
                    mSegShown = SegmentShown();
                    mSegShown.method = -2;
                    setLabelIf(mSegmentLabel, word(W::SegmentNone));
                }
            }
            else
            {
                SegmentShown now;
                now.pair      = pair;
                now.method    = (S32)segment->method;
                now.seconds10 = (S32)std::lround((F64)segment->seconds * 10.0);
                now.count     = segment->answer_count;
                now.selected  = segment->selected;
                now.state     = (S32)segment->state;
                if (!(now == mSegShown))
                {
                    mSegShown = now;
                    std::string title = word(W::SegmentTitle);
                    LLStringUtil::replaceString(title, "[FROM]", std::to_string(mTimeline.indexOf(pair.from) + 1));
                    LLStringUtil::replaceString(title, "[TO]", std::to_string(mTimeline.indexOf(pair.to) + 1));
                    setLabelIf(mSegmentLabel, title);
                    mMethodSeg->setValue(LLSD(now.method));
                    mSecondsSlider->setVisible(!generate);
                    if (!generate && !mSecondsSlider->hasMouseCapture())
                    {
                        mSecondsSlider->setValue(LLSD(segment->seconds));
                        mSecondsShown = -1;
                    }
                }
            }

            const S32 seconds10 = (S32)std::lround(mSecondsSlider->getValue().asReal() * 10.0);
            if (seconds10 != mSecondsShown)
            {
                mSecondsShown = seconds10;
                mSecondsSlider->setDisplayText(format(W::Seconds, "[SEC]", llformat("%.1f", seconds10 / 10.0)));
            }

            setEnabledIf(mMethodSeg, have && !segment_busy);
            setEnabledIf(mSecondsSlider, have && !generate && !segment_busy);

            const bool can = why(state) == Why::None && !segment_busy;
            setEnabledIf(mGenSegment, can);
            refreshMark(segment);
            setEnabledIf(mGenAll, state == SelfPoseState::Posing && mTimeline.keyCount() >= 2 && modelReady() && allTargets());
            setEnabledIf(mGenCancel, busy);
        }

        /// The selected segment's one mark and its reasons (research/361 section 2.1: "regenerating is advised" for its ends, its
        /// targets, its context or its passing), and the note of a segment that was taken off the queue (PrevMissing).
        void MotionFloater::refreshMark(const SegmentInfo* segment)
        {
            MarkShown now;
            if (segment)
            {
                now.pair    = segment->pair;
                now.reasons = segment->regen_reasons;
                now.fail    = segment->fail;
                now.state   = (S32)segment->state;
            }
            if (now == mMarkShown)
            {
                return;
            }
            mMarkShown = now;
            std::string text;
            if (segment && segment->fail == JobError::PrevMissing &&
                segment->state != SegState::Queued && segment->state != SegState::Running)
            {
                text = word(W::FailPrevMissing);
            }
            if (segment && segment->regen_reasons != 0)
            {
                if (!text.empty())
                {
                    text += '\n'; // the mark on a line of its own (no space to strip, in ja either)
                }
                text += mTimelineView->regenText(segment->regen_reasons);
            }
            setTextIf(mSegMark, mMarkText, text);
        }

        void MotionFloater::refreshStatus(SelfPoseState state)
        {
            // most pressing first: the helper loading, the requests under way, then what the last action said, then why
            // "generate" waits (pointers to the words read once: no string is made here but the progress, when it changes)
            const std::string* text = nullptr;
            if (motionClient().state() == HelperState::Loading)
            {
                text = &word(W::StatusLoading);
            }
            else if (mTimeline.busy())
            {
                const S32 pending = mTimeline.queuedCount() + (mTimeline.running() ? 1 : 0);
                const S32 total   = std::max(mGenTotal, pending);
                const S32 done    = std::max(total - pending, 0);
                if (total * 100 + done != mProgressKey)
                {
                    mProgressKey  = total * 100 + done;
                    mProgressText = format(W::StatusProgress, "[DONE]", std::to_string(done));
                    LLStringUtil::replaceString(mProgressText, "[TOTAL]", std::to_string(total));
                }
                text = &mProgressText;
            }
            if (!text && mNote != Note::None)
            {
                text = &mNoteText;
            }
            if (!text)
            {
                switch (why(state))
                {
                case Why::None:
                case Why::NotGenerate: break; // a Smooth or Linear segment needs nothing: no word
                case Why::NotLoggedIn: text = &word(W::WhyNotLoggedIn); break;
                case Why::NoPoser:     text = &word(W::WhyNoPoser); break;
                case Why::NotPosing:   text = &word(W::WhyNotPosing); break;
                case Why::Other:       text = &word(W::WhyOther); break;
                case Why::NoModel:     text = &word(W::WhyNoModel); break;
                case Why::Unpublished: text = &word(W::WhyUnpublished); break;
                case Why::NeedKeys:    text = &word(W::WhyNeedKeys); break;
                case Why::Same:        text = &word(W::WhySame); break;
                case Why::NeedPrev:    text = &word(W::NoteNeedPrev); break;
                }
            }
            static const std::string NOTHING;
            setTextIf(mStatus, mStatusShown, text ? *text : NOTHING);
        }

        void MotionFloater::refreshFrame()
        {
            const S32 count = mPlayback.frameCount();
            const S32 frame = mPlayback.frame();
            if (frame == mFrameShown && count == mCountShown)
            {
                return;
            }
            mFrameShown = frame;
            mCountShown = count;
            std::string text = word(W::FrameOf);
            LLStringUtil::format_map_t args;
            args["[FRAME]"] = count > 0 ? std::to_string(frame + 1) : std::string("0");
            args["[COUNT]"] = std::to_string(count);
            LLStringUtil::format(text, args);
            mFrameLabel->setLabelText(text);
        }

        W failureWord(JobError error)
        {
            switch (error)
            {
            case JobError::LowMemory:       return W::FailLowMemory;
            case JobError::NoHelper:        return W::FailNoHelper;
            case JobError::BadFolder:       return W::FailBadFolder;
            case JobError::BadInput:        return W::FailBadInput;
            case JobError::ModelMissing:    return W::FailModelMissing;
            case JobError::ModelUnreadable: return W::FailModelUnreadable;
            case JobError::Timeout:         return W::FailTimeout;
            case JobError::HelperEnded:     return W::FailHelperEnded;
            case JobError::RunFailed:       return W::FailRun;
            case JobError::BadAnswer:       return W::FailBadAnswer;
            case JobError::CannotBuild:     return W::FailConvert;
            case JobError::PrevMissing:     return W::FailPrevMissing;
            case JobError::None:            break;
            }
            return W::FailRun;
        }

        void MotionFloater::setNote(Note note, const std::string& detail)
        {
            mNote = note;
            switch (note)
            {
            case Note::None:          mNoteText.clear(); break;
            case Note::Failure:       break; // setNoteJob() makes its words
            case Note::Convert:       mNoteText = word(W::FailConvert); break;
            case Note::Touched:       mNoteText = word(W::NoteTouched); break;
            case Note::Gone:          mNoteText = word(W::NoteGone); break;
            case Note::Saved:         mNoteText = format(W::NoteSaved, "[NAME]", detail); break;
            case Note::SaveBadName:   mNoteText = word(W::NoteSaveBadName); break;
            case Note::SaveFailed:    mNoteText = word(W::NoteSaveFailed); break;
            case Note::Discarded:     mNoteText = word(W::NoteDiscarded); break;
            case Note::DiscardFailed: mNoteText = word(W::NoteDiscardFailed); break;
            case Note::TooManyKeys:   mNoteText = word(W::NoteTooManyKeys); break;
            case Note::TooLong:       mNoteText = word(W::NoteTooLong); break;
            case Note::Busy:          mNoteText = word(W::NoteBusy); break;
            case Note::SamePoses:     mNoteText = word(W::NoteSamePoses); break;
            case Note::CaptureFailed: mNoteText = word(W::NoteCaptureFailed); break;
            case Note::ApplyFailed:   mNoteText = word(W::NoteApplyFailed); break;
            case Note::NeedPrev:      mNoteText = word(W::NoteNeedPrev); break;
            }
        }

        /// A job's failure (the timeline's own kinds) as the status line's note.
        void MotionFloater::setNoteJob(JobError error)
        {
            mNote     = Note::Failure;
            mNoteText = word(failureWord(error));
        }
    } // namespace floaterui
} // namespace plus::posermotion
