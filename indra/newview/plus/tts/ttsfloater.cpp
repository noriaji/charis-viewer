/**
 * @file ttsfloater.cpp
 * @brief The chat read-aloud window (plus/tts): the switches, the details, the voice choices.
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

// Most boxes are bound to the settings (control_name, see tts.cpp). This class shows or hides the
// details (the front shows only the switch and what to read), fills the two voice choices once the
// engine has listed the system's voices, and runs the three buttons.

#include "llviewerprecompiledheaders.h"

#include "tts.h"
#include "plus.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llpanel.h"
#include "lltextbox.h"
#include "llui.h"
#include "llviewercontrol.h"

namespace plus::tts
{
    namespace
    {
        const char* const FLOATER_NAME   = "plus_tts";
        const char* const SET_SHOW       = "PlusTtsShowDetails";
        const char* const SET_VOICE_M    = "PlusTtsVoiceMale";
        const char* const SET_VOICE_F    = "PlusTtsVoiceFemale";
        const char* const VALUE_AUTO     = "auto";

        const S32 HEIGHT_FRONT  = 252;
        const S32 HEIGHT_DETAIL = 318;
        const F32 VOICES_RETRY_SECONDS = 0.5f; // while the engine has listed no voices yet

        class TtsFloater : public LLFloater
        {
            friend class ::LLFloaterReg;

        public:
            explicit TtsFloater(const LLSD& key) : LLFloater(key) {}

            bool postBuild() override
            {
                mDetail   = getChild<LLPanel>("detail");
                mMale     = mDetail->getChild<LLComboBox>("voice_male");
                mFemale   = mDetail->getChild<LLComboBox>("voice_female");
                mInfo     = mDetail->getChild<LLTextBox>("voices_info");

                getChild<LLCheckBoxCtrl>("show_details")->setCommitCallback([this](LLUICtrl*, const LLSD& value) { applyDetail(value.asBoolean()); });
                mMale->setCommitCallback([this](LLUICtrl*, const LLSD&) { chooseVoice(mMale, SET_VOICE_M); });
                mFemale->setCommitCallback([this](LLUICtrl*, const LLSD&) { chooseVoice(mFemale, SET_VOICE_F); });
                mDetail->getChild<LLButton>("sample")->setClickedCallback([](LLUICtrl*, const LLSD&) { speakSample(); });
                mDetail->getChild<LLButton>("stop")->setClickedCallback([](LLUICtrl*, const LLSD&) { stopSpeaking(); });
                mDetail->getChild<LLButton>("clear_hand")->setClickedCallback([](LLUICtrl*, const LLSD&) { clearAvatarVoiceModes(); });

                applyDetail(gSavedPerAccountSettings.getBOOL(SET_SHOW));
                fillVoices();
                return true;
            }

            void draw() override
            {
                // The engine lists the voices a moment after it has started: ask again now and then.
                if (!mVoicesFilled && mDetail->getVisible() && mRetry.getElapsedTimeF32() >= VOICES_RETRY_SECONDS)
                {
                    fillVoices();
                }
                LLFloater::draw();
            }

        private:
            void applyDetail(bool show)
            {
                mDetail->setVisible(show);
                LLRect rect = getRect();
                rect.mBottom = rect.mTop - (show ? HEIGHT_FRONT + HEIGHT_DETAIL : HEIGHT_FRONT);
                setShape(rect);
            }

            void fillVoices()
            {
                mRetry.reset();
                // The status first: the engine lists its voices before it says it is ready.
                const EngineStatus status = engineStatus();
                const std::vector<VoiceName> voices = availableVoices();
                if (voices.empty())
                {
                    // The engine failed to start, or started with no voice: none will come, stop asking.
                    if (status != EngineStatus::Starting)
                    {
                        mVoicesFilled = true;
                        mInfo->setText(getString(status == EngineStatus::Failed ? "voices_failed" : "voices_none"));
                        return;
                    }
                    if (!mNoneShown)
                    {
                        mInfo->setText(getString("voices_none"));
                        mNoneShown = true;
                    }
                    return;
                }
                mVoicesFilled = true;
                fillCombo(mMale, voices, true, gSavedPerAccountSettings.getString(SET_VOICE_M));
                fillCombo(mFemale, voices, false, gSavedPerAccountSettings.getString(SET_VOICE_F));
                LLStringUtil::format_map_t args;
                args["[COUNT]"] = llformat("%d", static_cast<int>(voices.size()));
                mInfo->setText(getString("voices_count", args));
            }

            // The voices of the gender first, the others after them (a man's voice can be set to any).
            void fillCombo(LLComboBox* combo, const std::vector<VoiceName>& voices, bool male, const std::string& current)
            {
                combo->removeall();
                combo->add(getString("voice_auto"), LLSD(VALUE_AUTO));
                for (int pass = 0; pass < 2; ++pass)
                {
                    for (const VoiceName& v : voices)
                    {
                        if ((v.male == male) == (pass == 0))
                        {
                            combo->add(v.name + " (" + v.language + ")", LLSD(v.name));
                        }
                    }
                }
                combo->setValue(LLSD(current.empty() ? std::string(VALUE_AUTO) : current));
            }

            void chooseVoice(LLComboBox* combo, const char* setting)
            {
                const std::string value = combo->getValue().asString();
                gSavedPerAccountSettings.setString(setting, value == VALUE_AUTO ? std::string() : value);
            }

            LLPanel*    mDetail = nullptr;
            LLComboBox* mMale   = nullptr;
            LLComboBox* mFemale = nullptr;
            LLTextBox*  mInfo   = nullptr;
            bool        mVoicesFilled = false;  // also when no voice will come (the engine failed): no more asking
            bool        mNoneShown    = false;  // "no voices" is on the info line
            LLFrameTimer mRetry;                // since the voices were last asked for
        };

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_tts.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<TtsFloater>);
        }

        plus::FeatureRegistration sRegistration({ "tts-floater", &init, {} });
    }
}
