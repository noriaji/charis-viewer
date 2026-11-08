/**
 * @file camerashotfloater.cpp
 * @brief Camera shots: the window (floater_plus_camera_shot.xml).
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

// The window keeps its choices in gSavedSettings (PlusCameraShot*), shared by every
// account. Menu: "Camera Shot" in menu_plus.xml (Floater.Toggle plus_camera_shot).
// Nothing is sent to the simulator; only the view of this viewer changes.

#include "llviewerprecompiledheaders.h"

#include "camerashot.h"
#include "plus.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "llviewercontrol.h"

namespace plus::camerashot
{
    namespace
    {
        constexpr char FLOATER_NAME[] = "plus_camera_shot";
        // Avatars farther from the camera than this are not offered.
        constexpr float SUBJECT_RADIUS = 60.f;

        constexpr char SETTING_AMOUNT[]  = "PlusCameraShotAmount";
        constexpr char SETTING_SECONDS[] = "PlusCameraShotSeconds";
        constexpr char SETTING_ORBIT[]   = "PlusCameraShotOrbit";
        constexpr char SETTING_EASE[]    = "PlusCameraShotEase";
        constexpr char SETTING_PART[]    = "PlusCameraShotPart";

        // A slider's setting; a broken one (NaN, infinite: it would pass the slider's clamp) gives its default.
        F32 finiteSetting(const char* name)
        {
            const F32 value = gSavedSettings.getF32(name);
            if (llfinite(value))
            {
                return value;
            }
            const LLControlVariable* control = gSavedSettings.getControl(name);
            return control ? (F32)control->getDefault().asReal() : 0.f;
        }

        class CameraShotFloater : public LLFloater
        {
            friend class ::LLFloaterReg;

        public:
            bool postBuild() override
            {
                mSubject = getChild<LLComboBox>("subject");
                mPart    = getChild<LLComboBox>("part");
                mEase    = getChild<LLComboBox>("ease");
                mStatus  = getChild<LLTextBox>("status");
                mStop    = getChild<LLButton>("btn_stop");

                getChild<LLButton>("btn_refresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshSubjects(); });
                // Each button names its move and its direction. The dolly zoom's two directions:
                // "recede" is the camera coming closer with the angle widening (the background draws
                // away from the subject), "rush" the camera going away with the angle narrowing
                // (the background closes in on it).
                struct MoveButton { const char* name; Kind kind; bool in; };
                for (const MoveButton& spec : { MoveButton{ "btn_dolly_in", Kind::Dolly, true },
                                                MoveButton{ "btn_dolly_out", Kind::Dolly, false },
                                                MoveButton{ "btn_zoom_in", Kind::Zoom, true },
                                                MoveButton{ "btn_zoom_out", Kind::Zoom, false },
                                                MoveButton{ "btn_dollyzoom_recede", Kind::DollyZoom, true },
                                                MoveButton{ "btn_dollyzoom_rush", Kind::DollyZoom, false },
                                                MoveButton{ "btn_orbit_right", Kind::Orbit, true },
                                                MoveButton{ "btn_orbit_left", Kind::Orbit, false } })
                {
                    getChild<LLButton>(spec.name)->setCommitCallback(
                        [this, spec](LLUICtrl*, const LLSD&) { shoot(spec.kind, spec.in); });
                }
                mStop->setCommitCallback([](LLUICtrl*, const LLSD&) { stop(); });
                mBack = getChild<LLButton>("btn_back");
                mBack->setCommitCallback([](LLUICtrl*, const LLSD&) { goToStart(); });

                // The choices are saved as they are made.
                getChild<LLSliderCtrl>("amount")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
                    gSavedSettings.setF32(SETTING_AMOUNT, (F32)ctrl->getValue().asReal());
                });
                getChild<LLSliderCtrl>("orbit_degrees")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
                    gSavedSettings.setF32(SETTING_ORBIT, (F32)ctrl->getValue().asReal());
                });
                getChild<LLSliderCtrl>("seconds")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
                    gSavedSettings.setF32(SETTING_SECONDS, (F32)ctrl->getValue().asReal());
                });
                mEase->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
                    gSavedSettings.setS32(SETTING_EASE, ctrl->getValue().asInteger());
                });
                mPart->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
                    gSavedSettings.setS32(SETTING_PART, ctrl->getValue().asInteger());
                });

                getChild<LLSliderCtrl>("amount")->setValue(finiteSetting(SETTING_AMOUNT));
                getChild<LLSliderCtrl>("seconds")->setValue(finiteSetting(SETTING_SECONDS));
                getChild<LLSliderCtrl>("orbit_degrees")->setValue(finiteSetting(SETTING_ORBIT));
                mEase->setValue(gSavedSettings.getS32(SETTING_EASE));
                mPart->setValue(gSavedSettings.getS32(SETTING_PART));
                refreshSubjects();
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                LLFloater::onOpen(key);
                refreshSubjects();
            }

            void draw() override
            {
                const bool is_running = running();
                mStop->setEnabled(is_running);
                mBack->setEnabled(hasStart());
                if (is_running)
                {
                    const int percent = (int)(progress() * 100.f);
                    if (percent != mPercentShown)
                    {
                        LLStringUtil::format_map_t args;
                        args["[PERCENT]"] = std::to_string(percent);
                        mStatus->setText(getString("shot_running", args));
                        mPercentShown = percent;
                    }
                    mIdleText = false;
                    mError.clear(); // a refusal while shooting was covered by the percent: the idle text comes after
                }
                else
                {
                    mPercentShown = -1;
                    if (!mIdleText && mError.empty())
                    {
                        mStatus->setText(getString("shot_idle"));
                        mIdleText = true;
                    }
                }
                LLFloater::draw();
            }

        private:
            explicit CameraShotFloater(const LLSD& key) : LLFloater(key) {}

            void refreshSubjects()
            {
                const LLUUID chosen = mSubject->getValue().asUUID();
                mSubject->clearRows();
                mSubject->add(getString("shot_subject_none"), LLSD(LLUUID::null));
                for (const Subject& subject : nearbySubjects(SUBJECT_RADIUS))
                {
                    std::string label = subject.name;
                    if (subject.id == gAgentID)
                    {
                        LLStringUtil::format_map_t args;
                        args["[NAME]"] = subject.name;
                        label          = getString("shot_subject_me", args);
                    }
                    mSubject->add(label, LLSD(subject.id));
                }
                // The same avatar stays chosen if still near; else the first row.
                if (!mSubject->setCurrentByID(chosen))
                {
                    mSubject->selectFirstItem();
                }
            }

            void shoot(Kind kind, bool in)
            {
                ShotParams params;
                params.kind    = kind;
                params.in      = in;
                params.amount  = getChild<LLSliderCtrl>("amount")->getValue().asReal();
                params.seconds = getChild<LLSliderCtrl>("seconds")->getValue().asReal();
                params.degrees = getChild<LLSliderCtrl>("orbit_degrees")->getValue().asReal();
                params.ease    = static_cast<Ease>(llclamp(mEase->getValue().asInteger(), 0, 3));
                params.part    = static_cast<Part>(llclamp(mPart->getValue().asInteger(), 0, 2));
                params.subject = mSubject->getValue().asUUID();

                std::string error;
                if (start(params, error))
                {
                    mError.clear();
                    return;
                }
                mError = error;
                mStatus->setText(getString(error));
                mIdleText     = false;
                mPercentShown = -1; // a shot still running says so again at the next frame
            }

            LLComboBox* mSubject = nullptr;
            LLComboBox* mPart    = nullptr;
            LLComboBox* mEase    = nullptr;
            LLTextBox*  mStatus  = nullptr;
            LLButton*   mStop    = nullptr;
            LLButton*   mBack    = nullptr;
            std::string mError;       // the key of the last refusal, shown until the next press
            bool        mIdleText = false;
            int         mPercentShown = -1; // the percent the status says while a shot runs (-1: it says something else)
        };

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_camera_shot.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<CameraShotFloater>);
        }

        plus::FeatureRegistration sRegistration({ "camerashot", &init, {} });
    }
}
