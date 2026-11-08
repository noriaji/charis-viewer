/**
 * @file translatesettings.h
 * @brief Translation (plus/translate, research/228): the "this PC" part of the chat translation settings window.
 *
 * The official window (LLFloaterTranslationSettings, Preferences > Chat > Translation) lets one pick a service and enter its
 * key. This adds the choice "this PC": a panel (local_panel in the window's XUI) that says whether the models for the chosen
 * language are here, fetches them (the button is the user's go-ahead for the download), and the dialect for translations into
 * Japanese (which every service gets). The official window only calls this class from a few marked places.
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

#ifndef PLUS_TRANSLATE_TRANSLATESETTINGS_H
#define PLUS_TRANSLATE_TRANSLATESETTINGS_H

#include "llframetimer.h"

#include <functional>
#include <memory>
#include <string>

class LLFloater;

namespace plus
{
    namespace translate
    {
        class LocalSettings
        {
        public:
            // floater: the translation settings window. changed: called when what the window needs to know changes by itself
            // (a fetch ended), so that it can enable or disable its OK button again.
            LocalSettings(LLFloater* floater, std::function<void()> changed);
            ~LocalSettings();

            void postBuild();
            void onOpen();
            // The choice of the service (the radio's value) or the language (the combo's value) changed, or the window opened.
            // on: translation is switched on in the window (the parts are disabled when it is not).
            void refresh(const std::string& service, const std::string& language, bool on);
            // Every frame the window is drawn: follows a fetch.
            void draw();
            // OK was pressed: what this class owns (the dialect) is written to the settings, and what the window saves goes to the
            // log (the values refresh() was given last).
            void apply();
            // Are the models for translating into this language here?
            bool ready(const std::string& language) const;

        private:
            struct Impl;
            std::unique_ptr<Impl> mImpl;
        };
    }
}

#endif // PLUS_TRANSLATE_TRANSLATESETTINGS_H
