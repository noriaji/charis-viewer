/**
 * @file subtitlecore.h
 * @brief SubtitleCore: what a subtitle window does (recognizing, translating, downloading the models), apart from the window.
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

// The XUI window (subtitlefloater.cpp) and the RmlUi window (subtitlerml.cpp) each own one SubtitleCore and are its
// SubtitleView. The window calls open() when it opens, close() when it closes, tick() every frame it is drawn, and the
// button / setting handlers; the core says what to show through the view (always on the main thread).

#ifndef PLUS_SUBTITLE_CORE_H
#define PLUS_SUBTITLE_CORE_H

#include "stdtypes.h"

#include <memory>
#include <string>

namespace plus::subtitle
{
    class SubtitleView
    {
    public:
        virtual ~SubtitleView() = default;
        // The window's <string> of that name.
        virtual std::string str(const std::string& name) = 0;
        virtual void setStatus(const std::string& text) = 0;
        // The whole log (replaces the old text; the newest line is at the end, which is shown).
        virtual void setLog(const std::string& text) = 0;
        // The most the log can hold (bytes): the oldest lines are left out beyond it.
        virtual S32 logMaxBytes() const = 0;
        // The model button: shown or not; label empty: not changed.
        virtual void setDownloadButton(bool visible, const std::string& label) = 0;
        virtual void setFetchButton(bool visible, const std::string& label) = 0;
        // The sources and licenses of the translation models, for the fetch button's tool tip after its own text (empty: none).
        virtual void setFetchCredits(const std::string& credits) = 0;
    };

    class SubtitleCore
    {
    public:
        explicit SubtitleCore(SubtitleView& view);
        ~SubtitleCore();     // stops the helpers (the model download goes on: sharedModelDownload)
        SubtitleCore(const SubtitleCore&) = delete;
        SubtitleCore& operator=(const SubtitleCore&) = delete;

        void open();
        void close();
        void tick();
        void onDownloadButton();
        void onFetchButton();
        // The way of translating, the language or the on/off was chosen again.
        void onTranslateServiceChanged();

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif // PLUS_SUBTITLE_CORE_H
