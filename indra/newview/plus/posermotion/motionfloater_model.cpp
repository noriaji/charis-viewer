/**
 * @file motionfloater_model.cpp
 * @brief "Motion Between Poses": the model's button and label (machine M, research/324 section 5).
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

#include <cmath>

namespace plus::posermotion
{
    namespace floaterui
    {
        /// The model's size as the window says it ("723": MiB, research/324 P03; the table holds bytes).
        std::string modelSizeMb()
        {
            constexpr F64 BYTES_PER_MB = 1024.0 * 1024.0;
            return llformat("%.0f", (F64)motionModel().sizeBytes() / BYTES_PER_MB);
        }

        // ---------------------------------------------------------------- the model (machine M)
        void MotionFloater::onModelButton()
        {
            MotionModel& model = motionModel();
            switch (model.state())
            {
            case ModelState::Missing:
            case ModelState::FetchFailed:
            case ModelState::Broken:
                confirmFetch();
                break;
            case ModelState::Downloading:
            case ModelState::Verifying:
                model.cancelDownload();
                break;
            case ModelState::Installed:
                confirmRemove();
                break;
            case ModelState::Unknown:
                model.refresh();
                break;
            }
            mModelKey = -1;
            refresh(selfPoseState());
        }

        void MotionFloater::confirmFetch()
        {
            MotionModel& model = motionModel();
            if (!model.published())
            {
                return;
            }
            // what goes out, where to, what it is and under which licence (research/324 section 4.2)
            confirmThen(format(W::ConfirmFetch, "[SIZE]", modelSizeMb()), [](MotionFloater& f)
            {
                f.stopHelper();
                motionClient().stop(); // the helper may hold the file
                motionModel().startDownload();
            });
        }

        void MotionFloater::confirmRemove()
        {
            confirmThen(format(W::ConfirmRemove, "[SIZE]", modelSizeMb()), [](MotionFloater& f)
            {
                f.stopHelper();
                motionClient().stop(); // before the file goes
                motionModel().remove(); // false: in use (another viewer), the state and failure() say so
            });
        }

        W reasonWord(ModelFailure failure)
        {
            switch (failure)
            {
            case ModelFailure::NotPublished: return W::ReasonNotPublished;
            case ModelFailure::CannotStart:  return W::ReasonCannotStart;
            case ModelFailure::BadFolder:    return W::ReasonBadFolder;
            case ModelFailure::Network:      return W::ReasonNetwork;
            case ModelFailure::Size:         return W::ReasonSize;
            case ModelFailure::Hash:         return W::ReasonHash;
            case ModelFailure::Disk:         return W::ReasonDisk;
            case ModelFailure::InUse:        return W::ReasonInUse;
            case ModelFailure::None:         break;
            }
            return W::ReasonNetwork;
        }

        void MotionFloater::refreshModel()
        {
            MotionModel& model = motionModel();
            const ModelState   state   = model.state();
            const ModelFailure failure = model.failure();
            const S32 percent = state == ModelState::Downloading ? (S32)std::floor(model.fraction() * 100.0) : 0;
            const S32 key     = (S32)state * 100000 + (S32)failure * 1000 + (model.published() ? 500 : 0) + percent;
            if (key == mModelKey)
            {
                return;
            }
            mModelKey = key;

            const std::string size = modelSizeMb();
            std::string label;
            W button = W::BtnFetch;
            bool can_press = true;
            switch (state)
            {
            case ModelState::Unknown:
                label = word(W::ModelChecking);
                can_press = false;
                break;
            case ModelState::Missing:
                label     = model.published() ? format(W::ModelMissing, "[SIZE]", size) : word(W::ModelUnpublished);
                can_press = model.published();
                break;
            case ModelState::Downloading:
                label  = format(W::ModelDownloading, "[PERCENT]", std::to_string(percent));
                button = W::BtnCancel;
                break;
            case ModelState::Verifying:
                label  = word(W::ModelVerifying);
                button = W::BtnCancel;
                break;
            case ModelState::Installed:
                label  = word(failure == ModelFailure::InUse ? W::ModelInUse : W::ModelInstalled);
                button = W::BtnRemove;
                break;
            case ModelState::FetchFailed:
                label     = format(W::ModelFailed, "[REASON]", word(reasonWord(failure)));
                button    = W::BtnRetry;
                can_press = model.published();
                break;
            case ModelState::Broken:
                label     = word(failure == ModelFailure::InUse ? W::ModelInUse : W::ModelBroken);
                button    = W::BtnRefetch;
                can_press = model.published();
                break;
            }
            mModelLabel->setLabelText(label);
            setLabelIf(mModelButton, word(button));
            setEnabledIf(mModelButton, can_press);
        }
    } // namespace floaterui
} // namespace plus::posermotion
