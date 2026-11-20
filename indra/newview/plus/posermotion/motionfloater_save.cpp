/**
 * @file motionfloater_save.cpp
 * @brief "Motion Between Poses": save as BVH and discard.
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

#include "llviewerprecompiledheaders.h"

#include "motionfloater_impl.h"

#include "llnotificationsutil.h"

namespace plus::posermotion
{
    namespace floaterui
    {
        // ---------------------------------------------------------------- save, discard
        void MotionFloater::onSave()
        {
            if (mTimeline.keyCount() < 2 || mTimeline.busy())
            {
                return;
            }
            stopPlayback();
            endSeekEdit();
            const std::string name = mName->getText();
            if (!plus::isSafeFileStem(name))
            {
                setNote(Note::SaveBadName);
                refresh(selfPoseState());
                return;
            }
            if (!clipBvhExists(name))
            {
                saveNow(name);
                return;
            }
            confirmThen(format(W::ConfirmOverwrite, "[NAME]", name), [name](MotionFloater& f) { f.saveNow(name); });
        }

        void MotionFloater::saveNow(const std::string& name)
        {
            if (mTimeline.keyCount() < 2 || mTimeline.busy())
            {
                return; // the keys went (or a generation began) while the question was open
            }
            if (mTimeline.stale() && !refreshTimeline(false))
            {
                refresh(selfPoseState());
                return;
            }
            if (mTimeline.stale())
            {
                return;
            }
            switch (saveClipBvh(name, mTimeline.clip(), mTimeline.layout()))
            {
            case BvhSaveResult::Saved:       setNote(Note::Saved, name); break;
            case BvhSaveResult::BadName:     setNote(Note::SaveBadName); break;
            case BvhSaveResult::NoPoser:
            case BvhSaveResult::WriteFailed: setNote(Note::SaveFailed); break;
            }
            refresh(selfPoseState());
        }

        void MotionFloater::onDiscard()
        {
            if (!mBefore.has())
            {
                return;
            }
            stopPlayback();
            endSeekEdit();
            const bool recording = plus::posermcp::beginUserEdit();
            bool restored = false;
            try
            {
                plus::posermcp::applySelfPose(mBefore.get());
                restored = true;
            }
            catch (const std::exception&)
            {
                // not posing any more: the pose stays, mBefore too
            }
            if (recording)
            {
                plus::posermcp::endUserEdit(HISTORY_SOURCE, "Motion Between Poses: discarded");
            }
            if (restored)
            {
                dropAll();
                setNote(Note::Discarded);
            }
            else
            {
                setNote(Note::DiscardFailed);
            }
            refresh(selfPoseState());
        }
    } // namespace floaterui
} // namespace plus::posermotion
