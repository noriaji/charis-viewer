/**
 * @file subtitlefloater.cpp
 * @brief Subtitles (plus/subtitle, research/137): the window. While it is open, the voice you hear
 *        is listened to and written out, each sentence with the name of who spoke it; closing it
 *        stops everything and frees the helper's memory.
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

#include "plus.h"
#include "subtitlecore.h"
#include "subtitlerml.h"
#include "plus/safemode/safemode.h"

#include "llbutton.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llviewercontrol.h"

namespace plus
{
    namespace subtitle
    {
        namespace
        {
            constexpr char FLOATER_NAME[] = "plus_subtitle";

            // The window itself; what it does is SubtitleCore's (subtitlecore.cpp), shared with the RmlUi window.
            class SubtitleFloater : public LLFloater, public SubtitleView
            {
                friend class ::LLFloaterReg;

            public:
                bool postBuild() override
                {
                    mLog    = getChild<LLTextEditor>("log");
                    mStatus = getChild<LLTextBox>("status");
                    mDownloadButton = getChild<LLButton>("download_model");
                    mDownloadButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { mCore.onDownloadButton(); });
                    mDownloadButton->setVisible(false);
                    mFetchButton = getChild<LLButton>("download_translate_model");
                    mFetchButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { mCore.onFetchButton(); });
                    mFetchButton->setVisible(false);
                    mFetchTip = mFetchButton->getToolTip();
                    for (const char* name : { "translate_service", "translate", "translate_to" })
                    {
                        getChild<LLUICtrl>(name)->setCommitCallback([this](LLUICtrl*, const LLSD&) { mCore.onTranslateServiceChanged(); });
                    }
                    return true;
                }

                void onOpen(const LLSD& key) override
                {
                    LLFloater::onOpen(key);
                    mCore.open();
                }

                void onClose(bool app_quitting) override
                {
                    mCore.close();
                    LLFloater::onClose(app_quitting);
                }

                void draw() override
                {
                    mCore.tick();
                    LLFloater::draw();
                }

                // ---- SubtitleView ----
                std::string str(const std::string& name) override { return getString(name); }
                void setStatus(const std::string& text) override { mStatus->setText(text); }
                void setLog(const std::string& text) override
                {
                    mLog->setText(text);
                    mLog->endOfDoc();
                }
                S32 logMaxBytes() const override { return mLog->getMaxTextLength(); }
                void setDownloadButton(bool visible, const std::string& label) override
                {
                    if (!label.empty())
                    {
                        mDownloadButton->setLabel(label);
                    }
                    mDownloadButton->setVisible(visible);
                }
                void setFetchButton(bool visible, const std::string& label) override
                {
                    mFetchButton->setVisible(visible);
                    mFetchButton->setLabel(label);
                }
                void setFetchCredits(const std::string& credits) override
                {
                    mFetchButton->setToolTip(credits.empty() ? mFetchTip : mFetchTip + "\n\n" + credits);
                }

            private:
                SubtitleFloater(const LLSD& key) : LLFloater(key), mCore(*this) {}

                LLTextEditor* mLog = nullptr;
                LLTextBox*    mStatus = nullptr;
                LLButton*     mDownloadButton = nullptr;
                LLButton*     mFetchButton = nullptr;
                std::string   mFetchTip;         // the button's tool tip from the XML; the credits go after it
                SubtitleCore  mCore;
            };

            // The window is the RmlUi one (subtitlerml.cpp, research/386). This XUI version is frozen (not changed any
            // more: a setting added to the window goes to both) and is used in safe mode, where RmlUi is not started,
            // and with PlusRmlUiWindows off (the choice is made once, here: it takes effect at the next start).
            void init()
            {
                if (plus::safemode::active() || !gSavedSettings.getBOOL("PlusRmlUiWindows"))
                {
                    LLFloaterReg::add(FLOATER_NAME, "floater_plus_subtitle.xml",
                                      (LLFloaterBuildFunc)&LLFloaterReg::build<SubtitleFloater>);
                }
                else
                {
                    addRmlFloater(FLOATER_NAME);
                }
            }

            plus::FeatureRegistration sRegistration({ "subtitle", &init, {} });
        }
    }
}
