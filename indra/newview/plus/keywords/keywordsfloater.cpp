/**
 * @file keywordsfloater.cpp
 * @brief Chat keyword alerts: the settings window (plus/keywords).
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

// Menu: "Chat Keywords" in menu_plus.xml (Floater.Toggle plus_keywords). The boxes and fields are
// bound to the settings (control_name, see keywords.cpp); this class only does the try-out field
// (type a line: it says whether the list would catch it) and the sound's two buttons.

#include "llviewerprecompiledheaders.h"

#include "keywords.h"
#include "plus.h"

#include "llbutton.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "lltextbox.h"
#include "llui.h"
#include "llviewercontrol.h"

namespace plus::keywords
{
    namespace
    {
        const char* const FLOATER_NAME = "plus_keywords";
        const char* const SET_SOUND    = "PlusKeywordsSound";

        class KeywordsFloater : public LLFloater
        {
            friend class ::LLFloaterReg;

        public:
            bool postBuild() override
            {
                mTry    = getChild<LLLineEditor>("try_text");
                mResult = getChild<LLTextBox>("try_result");
                getChild<LLButton>("sound_preview")->setClickedCallback([this](LLUICtrl*, const LLSD&) { previewSound(); });
                getChild<LLButton>("sound_default")->setClickedCallback([this](LLUICtrl*, const LLSD&) { resetSound(); });
                return true;
            }

            void draw() override
            {
                updateResult();
                LLFloater::draw();
            }

        private:
            KeywordsFloater(const LLSD& key) : LLFloater(key) {}

            // The list may be edited while text stands in the field: the text is judged again when it
            // or the list changed (looked at every frame; the result is set only when it changes).
            void updateResult()
            {
                const std::string& text       = mTry->getText();
                const unsigned     generation = listGeneration();
                if (generation == mJudgedGeneration && text == mJudgedText)
                {
                    return;
                }
                mJudgedGeneration = generation;
                mJudgedText       = text;

                std::string result;
                if (text.empty())
                {
                    result = getString("try_hint");
                }
                else
                {
                    std::string word;
                    if (testText(text, word))
                    {
                        LLStringUtil::format_map_t args;
                        args["[WORD]"] = word;
                        result = getString("try_hit");
                        LLStringUtil::format(result, args);
                    }
                    else
                    {
                        result = getString("try_miss");
                    }
                }
                if (result != mShown)
                {
                    mShown = result;
                    mResult->setText(result);
                }
            }

            void previewSound()
            {
                const LLUUID sound(gSavedPerAccountSettings.getString(SET_SOUND));
                if (sound.notNull() && LLUI::getInstance()->mAudioCallback)
                {
                    LLUI::getInstance()->mAudioCallback(sound);
                }
            }

            void resetSound()
            {
                if (LLControlVariable* control = gSavedPerAccountSettings.getControl(SET_SOUND))
                {
                    control->resetToDefault(true);
                }
            }

            LLLineEditor* mTry    = nullptr;
            LLTextBox*    mResult = nullptr;
            std::string   mShown;
            std::string   mJudgedText;          // the text the result is for
            unsigned      mJudgedGeneration = 0; // ... and the list's (listGeneration(), from 1: 0 is none yet)
        };

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_keywords.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<KeywordsFloater>);
        }

        plus::FeatureRegistration sRegistration({ "keywords", &init, {} });
    }
}
