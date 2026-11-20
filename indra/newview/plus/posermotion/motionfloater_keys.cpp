/**
 * @file motionfloater_keys.cpp
 * @brief "Motion Between Poses": the keys (chips, menu, selection), the selected segment's inputs, the timeline strip's events.
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

// Every action that changes the keys or a segment follows one order (research/357 section 4.2): the playback stops first
// (prepareEdit), the timeline is changed (a TimelineResult says whether it was refused, resultOk), then the joined clip,
// the playback and the widgets are brought up to date (afterEdit). The selected key is the window's; the selected segment
// is the one that starts at it (the last key: the one that ends at it).

#include "llviewerprecompiledheaders.h"

#include "motionfloater_impl.h"

#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "lltimer.h"
#include "lluictrlfactory.h"
#include "llviewermenu.h"

#include <algorithm>
#include <cmath>

namespace plus::posermotion
{
    namespace floaterui
    {
        void MotionFloater::prepareEdit()
        {
            stopPlayback();
            endSeekEdit();
        }

        /// Ok, or false with the note that says why (the widgets are written again from the model).
        bool MotionFloater::resultOk(TimelineResult result)
        {
            mSegShown  = SegmentShown();
            mPassShown.reset();
            switch (result)
            {
            case TimelineResult::Ok:           return true;
            case TimelineResult::TooManyKeys:  setNote(Note::TooManyKeys); break;
            case TimelineResult::TooLong:      setNote(Note::TooLong); break;
            case TimelineResult::Busy:         setNote(Note::Busy); break;
            case TimelineResult::SamePoses:    setNote(Note::SamePoses); break;
            case TimelineResult::NeedPrev:     setNote(Note::NeedPrev); break;
            case TimelineResult::NotReady:
            case TimelineResult::BuildFailed:  setNote(Note::Convert); break;
            case TimelineResult::NoSuchKey:
            case TimelineResult::NoSuchPair:
            case TimelineResult::BadValue:
            case TimelineResult::NeedsKeys:    break; // nothing to say: the widgets show what is there
            }
            return false;
        }

        /// After an edit: when the timeline changed, the stale segments' clips and the joined clip are made again (only what the
        /// edit made stale: no fresh skeleton here, see prepareRetarget); the widgets show the model either way.
        void MotionFloater::afterEdit(bool changed)
        {
            mSegShown = SegmentShown();
            if (changed)
            {
                refreshTimeline(false);
            }
            refresh(selfPoseState());
        }

        PairId MotionFloater::selectedPair() const
        {
            const S32 count = mTimeline.keyCount();
            const S32 index = mTimeline.indexOf(mSelKey);
            if (count < 2 || index < 0)
            {
                return PairId();
            }
            const S32 first = index + 1 < count ? index : index - 1;
            return PairId{ mTimeline.key(first).id, mTimeline.key(first + 1).id };
        }

        // ---------------------------------------------------------------- the pose from before
        /// The pose now, when none is kept from before yet: taken just before the first write to the avatar and kept only
        /// once that write went on (BeforeKeeper::commit; any other way out drops it).
        std::optional<history::Pose> MotionFloater::offerBefore()
        {
            std::optional<history::Pose> offered;
            if (mBefore.wanted())
            {
                history::Pose pose;
                if (plus::posermcp::captureSelfPose(pose))
                {
                    offered = std::move(pose);
                }
            }
            return offered;
        }

        /// The key's pose on the own avatar (the poser's applySelfPose, which throws when nobody is posed), as one entry of the
        /// pose history.
        bool MotionFloater::applyKeyPose(KeyId id)
        {
            const TimelineKey* key = mTimeline.find(id);
            if (!key || selfPoseState() != SelfPoseState::Posing)
            {
                return false;
            }
            const history::Pose pose = key->pose;   // a copy: nothing below leaves `key` in use
            std::optional<history::Pose> offered = offerBefore();
            const bool recording = plus::posermcp::beginUserEdit();
            bool applied = false;
            try
            {
                plus::posermcp::applySelfPose(pose);
                applied = true;
            }
            catch (const std::exception&)
            {
                // not posing any more: nothing was written
            }
            if (recording)
            {
                plus::posermcp::endUserEdit(HISTORY_SOURCE, "Motion Between Poses: key");
            }
            if (applied)
            {
                mBefore.commit(offered);
            }
            offered.reset();
            return applied;
        }

        // ---------------------------------------------------------------- the chips
        void MotionFloater::selectKey(KeyId id)
        {
            prepareEdit();
            const S32 index = mTimeline.indexOf(id);
            if (index < 0)
            {
                return;
            }
            mSelKey = id;
            setNote(Note::None);
            mSegShown = SegmentShown();
            if (selfPoseState() == SelfPoseState::Posing)
            {
                if (!applyKeyPose(id))
                {
                    setNote(Note::ApplyFailed);
                }
                else if (mPlayback.hasClip() && !mTimeline.stale() && mLoadedClip == mTimeline.clipVersion())
                {
                    mPlayback.seek(mTimeline.keyFrame(index)); // the playhead follows the key (nothing is written)
                }
            }
            refresh(selfPoseState());
        }

        void MotionFloater::onKeyRow()
        {
            const MotionKeyRow::Event event = mKeyRow->lastEvent();
            switch (event.kind)
            {
            case MotionKeyRow::Event::Kind::Select:
                selectKey(event.id);
                break;
            case MotionKeyRow::Event::Kind::Move:
            {
                prepareEdit();
                const bool ok = resultOk(mTimeline.moveKey(event.id, event.to_index));
                if (ok)
                {
                    mSelKey = event.id;
                    setNote(Note::None);
                }
                afterEdit(ok);
                break;
            }
            case MotionKeyRow::Event::Kind::Menu:
                showKeyMenu(event.id, event.x, event.y);
                break;
            case MotionKeyRow::Event::Kind::None:
                break;
            }
        }

        void MotionFloater::showKeyMenu(KeyId id, S32 x, S32 y)
        {
            if (mTimeline.indexOf(id) < 0)
            {
                return;
            }
            mSelKey = id;
            refresh(selfPoseState());
            if (LLContextMenu* old_menu = mMenuHandle.get())
            {
                old_menu->die();
                mMenuHandle.markDead();
            }
            LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
            LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
            registrar.add("Plus.PoserMotion.Key.Action", [this](LLUICtrl*, const LLSD& param) { onKeyMenu(param.asString()); });
            enable_registrar.add("Plus.PoserMotion.Key.Enable", [this](LLUICtrl*, const LLSD& param) { return keyMenuEnabled(param.asString()); });

            LLContextMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>(
                "menu_plus_poser_motion_key.xml", LLMenuGL::sMenuContainer, LLViewerMenuHolderGL::child_registry_t::instance());
            if (!menu)
            {
                return;
            }
            mMenuHandle = menu->getHandle();
            menu->show(x, y);
            LLMenuGL::showPopup(mKeyRow, menu, x, y);
        }

        void MotionFloater::onKeyMenu(const std::string& what)
        {
            if (what == "update")
            {
                onUpdateKey();
            }
            else if (what == "duplicate")
            {
                onDuplicateKey();
            }
            else if (what == "delete")
            {
                onDeleteKey();
            }
            else if (what == "clear")
            {
                onClearKeys();
            }
        }

        bool MotionFloater::keyMenuEnabled(const std::string& what) const
        {
            if (what == "duplicate")
            {
                return mTimeline.keyCount() < TIMELINE_MAX_KEYS;
            }
            return what != "update" || selfPoseState() == SelfPoseState::Posing;
        }

        // ---------------------------------------------------------------- the buttons of the keys
        void MotionFloater::onAddKey()
        {
            prepareEdit();
            history::Pose pose;
            if (selfPoseState() != SelfPoseState::Posing || !plus::posermcp::captureSelfPose(pose))
            {
                setNote(Note::CaptureFailed);
                refresh(selfPoseState());
                return;
            }
            KeyId id = NO_KEY;
            const bool ok = resultOk(mTimeline.addKey(pose, &id));
            if (ok)
            {
                mSelKey = id;
                setNote(Note::None);
            }
            afterEdit(ok);
        }

        void MotionFloater::onUpdateKey()
        {
            prepareEdit();
            history::Pose pose;
            if (selfPoseState() != SelfPoseState::Posing || !plus::posermcp::captureSelfPose(pose))
            {
                setNote(Note::CaptureFailed);
                refresh(selfPoseState());
                return;
            }
            const bool ok = resultOk(mTimeline.updateKey(mSelKey, pose));
            if (ok)
            {
                setNote(Note::None);
            }
            afterEdit(ok);
        }

        void MotionFloater::onDuplicateKey()
        {
            prepareEdit();
            KeyId id = NO_KEY;
            const bool ok = resultOk(mTimeline.duplicateKey(mSelKey, &id));
            if (ok)
            {
                mSelKey = id;
                setNote(Note::None);
            }
            afterEdit(ok);
        }

        void MotionFloater::onDeleteKey()
        {
            prepareEdit();
            const S32 index = mTimeline.indexOf(mSelKey);
            const bool ok = index >= 0 && resultOk(mTimeline.removeKey(mSelKey));
            if (ok)
            {
                const S32 left = mTimeline.keyCount();
                mSelKey = left > 0 ? mTimeline.key(std::min(index, left - 1)).id : NO_KEY;
                setNote(Note::None);
            }
            afterEdit(ok);
        }

        void MotionFloater::onLoopKey()
        {
            prepareEdit();
            KeyId id = NO_KEY;
            const bool ok = resultOk(mTimeline.closeLoop(&id));
            if (ok)
            {
                mSelKey = id != NO_KEY ? id : mSelKey;
                setNote(Note::None);
            }
            afterEdit(ok);
        }

        void MotionFloater::onClearKeys()
        {
            if (mTimeline.keyCount() == 0)
            {
                return;
            }
            confirmThen(format(W::ConfirmClear, "[COUNT]", std::to_string(mTimeline.keyCount())),
                        [](MotionFloater& f) { f.clearKeysNow(); });
        }

        /// "Delete all keys", once confirmed: the one way besides logout to drop the keys (research/357 section 4.2).
        void MotionFloater::clearKeysNow()
        {
            prepareEdit();
            dropKeys();
            setNote(Note::None);
            mSegShown = SegmentShown();
            refresh(selfPoseState());
        }

        /// The selected key, ready to change (the playback stopped); false (the widgets written again) when there is none.
        /// (onHold keeps its own check: with no key its slider is greyed, and it never wrote the widgets again.)
        bool MotionFloater::beginKeyEdit()
        {
            if (mTimeline.indexOf(mSelKey) < 0)
            {
                refresh(selfPoseState());
                return false;
            }
            prepareEdit();
            return true;
        }

        void MotionFloater::onHold()
        {
            if (mTimeline.indexOf(mSelKey) < 0)
            {
                return;
            }
            prepareEdit();
            afterEdit(resultOk(mTimeline.setHold(mSelKey, (F32)mHoldSlider->getValue().asReal())));
        }

        /// The selected key's passing (research/361 section 1.2): Auto or Through. Taken while segments are queued or running
        /// too (the timeline marks an answer that was asked otherwise).
        void MotionFloater::onPassMode()
        {
            if (beginKeyEdit())
            {
                const PassMode mode = mPassMode->getValue().asInteger() == 1 ? PassMode::Through : PassMode::Auto;
                afterEdit(resultOk(mTimeline.setPass(mSelKey, mode)));
            }
        }

        void MotionFloater::onStrength()
        {
            if (beginKeyEdit())
            {
                const PassStrength strength = mStrength->getValue().asInteger() == 0 ? PassStrength::Weak : PassStrength::Normal;
                afterEdit(resultOk(mTimeline.setStrength(mSelKey, strength)));
            }
        }

        // ---------------------------------------------------------------- the selected segment's inputs
        /// The selected segment, ready to change (the playback stopped); false (the widgets written again) when there is none.
        bool MotionFloater::beginSegmentEdit(PairId& pair)
        {
            pair = selectedPair();
            if (!pair.valid())
            {
                refresh(selfPoseState());
                return false;
            }
            prepareEdit();
            return true;
        }

        void MotionFloater::onMethod()
        {
            PairId pair;
            if (!beginSegmentEdit(pair))
            {
                return;
            }
            const S32 choice = mMethodSeg->getValue().asInteger();
            const SegMethod method = choice == 1 ? SegMethod::Smooth : choice == 2 ? SegMethod::Linear : SegMethod::Generate;
            afterEdit(resultOk(mTimeline.setMethod(pair, method)));
        }

        void MotionFloater::onSecondsSlider()
        {
            PairId pair;
            if (!beginSegmentEdit(pair))
            {
                return;
            }
            afterEdit(resultOk(mTimeline.setSeconds(pair, (F32)mSecondsSlider->getValue().asReal())));
        }

        // ---------------------------------------------------------------- the strip's events
        void MotionFloater::onTimelineEvent()
        {
            const MotionTimelineView::Event event = mTimelineView->lastEvent();
            switch (event.kind)
            {
            case MotionTimelineView::Event::Kind::Seek:
                if (preparePlayback())
                {
                    seekTo((S32)std::lround(event.seek_sec * CLIP_FPS));
                }
                else
                {
                    // nothing to show a frame of now (generating, not posing): the needle goes back to where the playback is
                    mTimelineView->setPlayheadSec(mPlayback.hasClip() ? (F64)mPlayback.frame() / (F64)CLIP_FPS : 0.0);
                }
                break;
            case MotionTimelineView::Event::Kind::Resize:
                prepareEdit();
                afterEdit(resultOk(event.method == SegMethod::Generate ? mTimeline.setTokens(event.pair, event.tokens)
                                                                       : mTimeline.setSeconds(event.pair, event.seconds)));
                break;
            case MotionTimelineView::Event::Kind::None:
                break;
            }
        }
    } // namespace floaterui
} // namespace plus::posermotion
