/**
 * @file translatefetch.h
 * @brief Subtitles (plus/subtitle, research/217, 222): fetching the local translation models.
 *
 * One small archive per direction (English to Japanese, Japanese to English, English to Chinese, Spanish, French, German,
 * Russian, Italian, Portuguese; 60 to 240 MB each), none shipped. The window's button is the user's go-ahead. Each one is
 * fetched with ModelDownload (curl.exe, HTTPS only, size and SHA-256 checked) and then unpacked on a worker thread into
 * <modelFolder>/translate/<pair>/, where charis-translate.exe reads it. Only the directions the chosen target language
 * needs, and that are not installed yet, are fetched.
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

#ifndef PLUS_SUBTITLE_TRANSLATEFETCH_H
#define PLUS_SUBTITLE_TRANSLATEFETCH_H

#include "subtitledownload.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace plus
{
    namespace subtitle
    {
        // <modelFolder>/translate: the folder that holds one sub-folder per direction ("en-ja", "ja-en", "en-zh", ...).
        std::string translateModelRoot();
        // The folder of one direction.
        std::string translateModelFolder(const std::string& pair);

        // The directions that can be fetched whose target language is `target` (two letters: "ja", "en", ...). Empty when none.
        std::vector<std::string> translatePairsFor(const std::string& target);
        // True when every direction translatePairsFor(target) names is installed (false when there are none).
        bool translateModelsInstalledFor(const std::string& target);
        // The archive (and folder) a direction is answered by: its own name, or "m2m" for the four directions with Korean.
        std::string translateArchiveOfPair(const std::string& pair);
        // True when this direction is installed.
        bool translatePairInstalled(const std::string& pair);

        // The Korean model (one multilingual model, about 500 MB) is installed.
        bool translateKoreanInstalled();
        // Bytes still to fetch for `target` (and, with withKorean, for the Korean model); 0 when everything is here.
        uint64_t translateModelBytesMissing(const std::string& target, bool withKorean);
        // The folder (an https address) the models are fetched from: the setting PlusTranslateModelUrl, else the address built in
        // (empty until the models are published: the fetch then says so).
        std::string translateModelBaseUrl();

        // The models' own words: where they come from and under which license, for the directions of `target` (the log, the tooltip).
        std::string translateModelCredits(const std::string& target);

        class TranslateModelFetch;
        // The one fetch of the translation models in the viewer: the subtitle window and the chat translation settings window both
        // show and start it, so two windows never fetch the same archive at once (they would fight for one .part file).
        TranslateModelFetch& sharedTranslateFetch();

        class TranslateModelFetch
        {
        public:
            enum class State { Idle, Running, Unpacking, Done, Failed };

            TranslateModelFetch() = default;
            ~TranslateModelFetch();   // cancels

            // Main thread. baseUrl is the folder the archives are in (".../en-ja.ctm" follows); target is the language the
            // sentences are translated into. False (and why) when it cannot start or there is nothing to fetch; the state
            // stays Idle.
            // withKorean: also the Korean model (about 500 MB), so that Korean text can be read; it is fetched for Korean as the
            // target in any case.
            bool start(const std::string& baseUrl, const std::string& target, std::string& error, bool withKorean = false);
            void cancel();
            // Main thread: moves on to the next step. The shared fetch is polled by a timer of its own while it runs (translatefetch.cpp),
            // so it goes on whether or not a window is open; a window may call this too (it does no harm).
            void poll();
            // The archive being fetched or unpacked now ("en-ja", "m2m", ...); "" when nothing runs.
            std::string currentArchive() const;

            State state() const { return State(mState.load()); }
            // 0..1 over all the archives that are being fetched.
            double fraction() const;
            std::string failure() const;

        private:
            void fail(const std::string& why);
            bool startCurrent(std::string& error);

            std::atomic<int>  mState{ int(State::Idle) };
            ModelDownload     mDownload;
            std::vector<ModelSource> mQueue;     // what is still to fetch, in order
            size_t            mIndex = 0;        // the one being fetched or unpacked
            std::string       mBaseUrl;
            std::thread       mUnpack;
            std::atomic<int>  mUnpackResult{ 0 };   // 0 running, 1 done, 2 failed
            std::atomic<bool> mStopUnpack{ false }; // cancel(): the unpack in progress ends at once
            mutable std::mutex mMutex;
            std::string       mFailure;
        };
    }
}

#endif // PLUS_SUBTITLE_TRANSLATEFETCH_H
