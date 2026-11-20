/**
 * @file motionfloater_generate.cpp
 * @brief "Motion Between Poses": the window's queue of generate requests, the answers, the joined clip's refresh, stopHelper() and dropKeys().
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

// The queue itself is the timeline's (MotionTimeline::requestGenerate / requestAll / nextJob / onAnswer / onFailure,
// research/357 section 4.1); this file only carries it to the helper, one request at a time: each tick reads what the
// helper said (pumpGeneration), then sends the next queued segment when none is out. The window's tick is shortened
// while anything is queued or running (setTickPeriod), because one answer is read per tick.

#include "llviewerprecompiledheaders.h"

#include "motionfloater_impl.h"

#include <algorithm>

#include "lltimer.h"
#include "llvoavatarself.h"

#include <utility>

namespace plus::posermotion
{
    namespace floaterui
    {
        JobError jobErrorOf(MotionFailure failure)
        {
            switch (failure)
            {
            case MotionFailure::LowMemory:       return JobError::LowMemory;
            case MotionFailure::NoHelper:        return JobError::NoHelper;
            case MotionFailure::BadFolder:       return JobError::BadFolder;
            case MotionFailure::BadInput:        return JobError::BadInput;
            case MotionFailure::ModelMissing:    return JobError::ModelMissing;
            case MotionFailure::ModelUnreadable: return JobError::ModelUnreadable;
            case MotionFailure::Timeout:         return JobError::Timeout;
            case MotionFailure::HelperEnded:     return JobError::HelperEnded;
            case MotionFailure::RunFailed:       return JobError::RunFailed;
            case MotionFailure::BadAnswer:       return JobError::BadAnswer;
            case MotionFailure::None:            break;
            }
            return JobError::RunFailed;
        }

        // ---------------------------------------------------------------- the model of the avatar's skeleton
        /// The rest skeleton of the own avatar as the model's retargeter (made again when the caller says "fresh", else kept).
        /// It is made fresh when the window opens, when a playback is loaded and when the user asks for motions (not on every edit).
        bool MotionFloater::prepareRetarget(bool fresh)
        {
            if (mRetargetOk && !fresh)
            {
                return true;
            }
            mRetargetOk = false;
            RestSkeleton rest;
            if (!isAgentAvatarValid() || !buildRestSkeleton(*gAgentAvatarp, rest))
            {
                LL_WARNS("PoserMotion") << "no rest skeleton" << LL_ENDL;
                return false;
            }
            const RetargetResult r = mRetarget.init(rest);
            if (r != RetargetResult::Ok)
            {
                LL_WARNS("PoserMotion") << "the rest skeleton could not be used: " << describe(r) << LL_ENDL;
                return false;
            }
            mRetargetOk = true;
            return true;
        }

        /// After an edit: the layout, the stale segments' clips and the joined clip are made again (the rest skeleton
        /// too when `fresh`: every clip then). The playback, when it has the clip loaded, follows it.
        bool MotionFloater::refreshTimeline(bool fresh)
        {
            if (mTimeline.keyCount() < 2)
            {
                if (mPlayback.hasClip())
                {
                    dropClip();
                }
                return true;
            }
            if (!prepareRetarget(fresh))
            {
                setNote(Note::Convert);
                return false;
            }
            if (fresh)
            {
                mTimeline.invalidateClips();
            }
            const TimelineResult r = mTimeline.refresh(mRetarget);
            if (r != TimelineResult::Ok)
            {
                LL_WARNS("PoserMotion") << "the keys could not be joined into a clip: " << describe(r) << LL_ENDL;
                setNote(r == TimelineResult::TooLong ? Note::TooLong : Note::Convert);
                return false;
            }
            syncPlayback();
            return true;
        }

        // ---------------------------------------------------------------- the user's buttons
        /// A request for the selected segment, once the keys are made again from a fresh skeleton (the queue is the timeline's).
        void MotionFloater::generateSelected(SelectedRequest request)
        {
            const SelfPoseState state = selfPoseState();
            if (why(state) != Why::None)
            {
                refresh(state);
                return;
            }
            prepareEdit();
            S32 jobs = 0;
            if (refreshTimeline(true) && resultOk(request(mTimeline, selectedPair(), jobs)))
            {
                startGeneration(jobs);
            }
            refresh(selfPoseState());
        }

        void MotionFloater::onGenerateSegment()
        {
            generateSelected([](MotionTimeline& timeline, PairId pair, S32& jobs)
            {
                jobs = 1;
                return timeline.requestGenerate(pair);
            });
        }

        void MotionFloater::onGenerateAll()
        {
            const SelfPoseState state = selfPoseState();
            const Why w = why(state);
            if (w != Why::None && w != Why::NotGenerate && w != Why::Same && w != Why::NeedPrev)
            {
                refresh(state); // not about the selected segment: the model, the poser, too few keys
                return;
            }
            prepareEdit();
            if (refreshTimeline(true))
            {
                startGeneration(mTimeline.requestAll());
            }
            refresh(selfPoseState());
        }

        void MotionFloater::onGenerateCancel()
        {
            cancelGeneration();
            setNote(Note::None);
            refresh(selfPoseState());
        }

        void MotionFloater::startGeneration(S32 jobs)
        {
            if (jobs <= 0)
            {
                return;
            }
            mGenTotal += jobs;
            mSegShown = SegmentShown();
            setNote(Note::None);
            setTickPeriod();
        }

        // ---------------------------------------------------------------- the helper, one request at a time
        void MotionFloater::pumpGeneration(F64 now)
        {
            MotionClient& client = motionClient();
            const MotionEvent event = client.update(now, mAnswer); // also ends an idle helper on its timeout
            if (mTimeline.running())
            {
                switch (event)
                {
                case MotionEvent::Answer:
                    takeAnswer();
                    break;
                case MotionEvent::Failed:
                    failJob(jobErrorOf(client.lastFailure()));
                    break;
                case MotionEvent::None:
                    if (!client.busy())
                    {
                        failJob(JobError::HelperEnded); // stopped elsewhere (the model was removed): nothing will come
                    }
                    break;
                }
            }
            if (!mTimeline.running() && mTimeline.queuedCount() > 0 && !client.busy())
            {
                startNextJob(now);
            }
        }

        void MotionFloater::startNextJob(F64 now)
        {
            if (!prepareRetarget(false))
            {
                cancelGeneration(); // the queue goes back to S0 / S3; nothing can be asked without the skeleton
                setNote(Note::Convert);
                return;
            }
            // the segments that wait now: only their own failures are news when nextJob ends the queue (a failure kept from
            // an earlier run is not)
            std::vector<PairId> waiting;
            for (S32 i = 0; i < mTimeline.segmentCount(); ++i)
            {
                if (mTimeline.segment(i).state == SegState::Queued)
                {
                    waiting.push_back(mTimeline.segment(i).pair);
                }
            }
            const std::optional<GenerateJob> job = mTimeline.nextJob(mRetarget);
            if (!job)
            {
                if (!mTimeline.busy())
                {
                    finishGeneration(); // what was queued is gone (its pair, or its method changed, or nextJob failed it itself)
                    // a failure that only nextJob could see: the keys could not be made into a layout or a request (the
                    // segments say BadInput), or a passed key's segment before it had no answer (PrevMissing)
                    Note note = Note::None;
                    for (const PairId& pair : waiting)
                    {
                        const SegmentInfo* seg = mTimeline.findSegment(pair);
                        if (!seg)
                        {
                            continue;
                        }
                        if (seg->fail == JobError::BadInput)
                        {
                            note = Note::Convert; // the first of them says (a conversion failure is the more basic)
                            break;
                        }
                        if (seg->fail == JobError::PrevMissing)
                        {
                            note = Note::NeedPrev;
                        }
                    }
                    if (note != Note::None)
                    {
                        setNote(note);
                    }
                }
                return;
            }
            MotionClient& client = motionClient();
            // the context and the targets as the timeline made them (a passed key's: research/361)
            static_assert(TIMELINE_CONTEXT_FRAMES == MOTION_CONTEXT_FRAMES && TIMELINE_MAX_TARGETS == MOTION_MAX_TARGETS,
                          "the job's arrays are copied into the request's");
            GenerateRequest request;
            std::copy(job->context, job->context + MOTION_CONTEXT_FRAMES, request.context);
            std::copy(job->targets, job->targets + MOTION_MAX_TARGETS, request.targets);
            request.ntargets = job->ntargets;
            request.tokens   = job->tokens;
            switch (client.generate(request, now))
            {
            case GenerateResult::Started:
                mJobPair = job->pair;
                setNote(Note::None);
                break;
            case GenerateResult::Refused:
                failJob(jobErrorOf(client.lastFailure()));
                break;
            case GenerateResult::Busy:
                failJob(JobError::HelperEnded); // somebody else's request is out: not ours to wait for
                break;
            }
        }

        /// H2 -> H3 with an answer: the segment is Made (a refit for the keys as they are now), or Failed.
        void MotionFloater::takeAnswer()
        {
            const F64 t0 = LLTimer::getTotalSeconds();
            const S32 answer_tokens = mAnswer.tokens;
            if (!prepareRetarget(false))
            {
                failJob(JobError::CannotBuild); // the running request ends here (the answer cannot be used without the skeleton)
                return;
            }
            const AnswerFate fate = mTimeline.onAnswer(mRetarget, std::move(mAnswer.frames));
            if (fate == AnswerFate::CannotBuild)
            {
                // why: the whole would pass the limit of 30 s (this answer, in the place of the segment's length), else the
                // segment says (a wrong length, a refit that failed)
                const SegmentInfo* segment = mTimeline.findSegment(mJobPair);
                const S32 grown = mTimeline.totalFrames() - (segment ? segment->frames : 0) + (timelineFramesOfTokens(answer_tokens) - 1);
                if (segment && segment->fail == JobError::CannotBuild && grown > TIMELINE_MAX_FRAMES)
                {
                    setNote(Note::TooLong);
                }
                else
                {
                    setNoteJob(segment && segment->fail != JobError::None ? segment->fail : JobError::CannotBuild);
                }
            }
            if (poserMotionLog())
            {
                LL_INFOS("PoserMotion") << "answer: " << answer_tokens << " tokens, helper " << mAnswer.helper_ms << " ms, wait "
                                        << mAnswer.wait_sec << " s, fitted in " << (LLTimer::getTotalSeconds() - t0) * 1000.0
                                        << " ms, " << (fate == AnswerFate::Used ? "used" : fate == AnswerFate::Dropped ? "dropped" : "not usable")
                                        << LL_ENDL;
            }
            mSegShown = SegmentShown();
            syncPlayback();
            if (!mTimeline.busy())
            {
                finishGeneration();
            }
        }

        /// A job failed: the timeline puts the queue back (a whole-run error) or marks the one segment (its own error).
        void MotionFloater::failJob(JobError error)
        {
            mTimeline.onFailure(error);
            setNoteJob(error);
            mSegShown = SegmentShown();
            if (!mTimeline.busy())
            {
                finishGeneration();
            }
        }

        void MotionFloater::finishGeneration()
        {
            mGenTotal = 0;
            mSegShown = SegmentShown();
            setTickPeriod();
        }

        /// The user's "cancel" (and the poser going away): the queue empties, a request out is forgotten at the helper.
        void MotionFloater::cancelGeneration()
        {
            if (mTimeline.cancelGeneration())
            {
                motionClient().cancel(LLTimer::getTotalSeconds());
            }
            finishGeneration();
        }

        void MotionFloater::setTickPeriod()
        {
            const bool fast = mTimeline.busy();
            if (mTicker && fast != mFastTick)
            {
                mFastTick = fast;
                mTicker->setPeriod(fast ? GENERATE_TICK_SEC : TICK_SEC);
            }
        }

        /// The window closes, the user logs out, the model goes: the queue is dropped and the helper ends.
        void MotionFloater::stopHelper()
        {
            mTimeline.cancelGeneration();
            finishGeneration();
            motionClient().stop();
        }

        /// The keys and what was made for them are dropped: logout, the own avatar gone, "delete all keys" (nothing else).
        void MotionFloater::dropKeys()
        {
            dropClip();
            if (mTimeline.clearAll())
            {
                motionClient().cancel(LLTimer::getTotalSeconds());
            }
            mSelKey = NO_KEY;
            finishGeneration();
        }
    } // namespace floaterui
} // namespace plus::posermotion
