/**
 * @file translatefetch.cpp
 * @brief Subtitles (plus/subtitle, research/217): fetching the local translation models.
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

#include "translatefetch.h"
#include "subtitleclient.h"
#include "translatearchive.h"

#include "lldir.h"
#include "llfile.h"
#include "llevents.h"
#include "lleventtimer.h"
#include "llthread.h"
#include "llviewercontrol.h"

namespace plus
{
    namespace subtitle
    {
        namespace
        {
            // One archive per direction, as tools/translate-helper/pack.py writes them (2026-10-30, research/222): the file is
            // "<pair>.ctm" in the folder the models are fetched from; size and SHA-256 are those of the file.
            struct Archive
            {
                const char* pair;
                uint64_t size;
                const char* sha256;
                const char* credit;   // where the model comes from, under which license
            };

            const std::vector<Archive>& archives()
            {
                static const std::vector<Archive> sArchives = {
                    { "en-ja", 63769870ULL, "55de2ae5f8d5896b452c42bdf4573e414f8d20543a3558af9ee7583a2c66f731",
                      "FuguMT (staka, CC BY-SA 4.0)" },
                    { "ja-en", 80193102ULL, "b18f9c0f3836e4a0da94e82a1722ff9734271d5568148cc86fbe32534fe374c6",
                      "Helsinki-NLP opus-mt-ja-en (Apache 2.0)" },
                    { "en-zh", 82547958ULL, "7d0da9fd9708164575ab908c28bb12107cbf0a101789aa5b8a99b43daeb46965",
                      "Helsinki-NLP opus-mt-en-zh (Apache 2.0)" },
                    { "en-es", 82536685ULL, "24f019540587c4a6d533b0eb20a300e6f347f93e67db60b008589a883adb156d",
                      "Helsinki-NLP opus-mt-en-es (Apache 2.0)" },
                    { "en-fr", 79407751ULL, "beb44e5438b467b909f7b535b682d8545a7fe9932179949f351e74855fcb926e",
                      "Helsinki-NLP opus-mt-en-fr (Apache 2.0)" },
                    { "en-ru", 82522565ULL, "022a02da6c91ebad5ee6c2527fd89401bad17bacaf6cc9add477c4cd4084b192",
                      "Helsinki-NLP opus-mt-en-ru (Apache 2.0)" },
                    { "en-it", 91031397ULL, "a1b5b16fa2552cd34d74fdda6c5bfda1280c429349a4cb77293f5dbb5569c1df",
                      "Helsinki-NLP opus-mt-en-it (Apache 2.0)" },
                    { "en-de", 78597251ULL, "23b651073c86dbbfab26de9df14af516f75b349ab613c9a5d65969a9cc7845c4",
                      "Helsinki-NLP opus-mt-en-de (CC BY 4.0)" },
                    { "en-pt", 238316571ULL, "17d1255b8b8fd7a580dcbaa46990cc22dd342ad5df2ab09927ee1ee8091f1a8f",
                      "Helsinki-NLP opus-mt-tc-big-en-pt (CC BY 4.0)" },
                    // one multilingual model answering ja-ko, ko-ja, en-ko and ko-en (research/228); 498 MB
                    { "m2m", 498439513ULL, "4c562b9dbbe97ad2eef2d736fd9ef6afc86ca9ec097db199a1c3bc246c8b29b1",
                      "Facebook AI Research M2M100 418M (MIT)" },
                };
                return sArchives;
            }

            ModelSource sourceOf(const Archive& a)
            {
                return { "", std::string(a.pair) + ".ctm", a.size, a.sha256 };
            }

            std::string pairOf(const ModelSource& s)
            {
                return s.fileName.substr(0, s.fileName.find('.'));
            }

            // "en-ja" -> "ja"
            std::string targetOf(const std::string& pair)
            {
                if (pair == "m2m")
                {
                    return "ko";   // fetched when Korean is the language to translate into (Korean text is read with it too)
                }
                const size_t dash = pair.find('-');
                return dash == std::string::npos ? std::string() : pair.substr(dash + 1);
            }
        }

        std::string translateModelRoot()
        {
            return modelFolder() + gDirUtilp->getDirDelimiter() + "translate";
        }

        std::string translateModelFolder(const std::string& pair)
        {
            return translateModelRoot() + gDirUtilp->getDirDelimiter() + pair;
        }

        std::vector<std::string> translatePairsFor(const std::string& target)
        {
            std::vector<std::string> out;
            for (const Archive& a : archives())
            {
                if (targetOf(a.pair) == target)
                {
                    out.push_back(a.pair);
                }
            }
            return out;
        }

        // The four directions with Korean are answered by the one multilingual model; the others have a model each.
        std::string translateArchiveOfPair(const std::string& pair)
        {
            return (pair == "ja-ko" || pair == "ko-ja" || pair == "en-ko" || pair == "ko-en") ? "m2m" : pair;
        }

        bool translatePairInstalled(const std::string& pair)
        {
            return translateModelInstalled(translateModelFolder(translateArchiveOfPair(pair)));
        }

        bool translateModelsInstalledFor(const std::string& target)
        {
            const std::vector<std::string> pairs = translatePairsFor(target);
            for (const std::string& pair : pairs)
            {
                if (!translatePairInstalled(pair))
                {
                    return false;
                }
            }
            return !pairs.empty();
        }

        bool translateKoreanInstalled()
        {
            return translateModelInstalled(translateModelFolder("m2m"));
        }

        uint64_t translateModelBytesMissing(const std::string& target, bool withKorean)
        {
            uint64_t bytes = 0;
            for (const Archive& a : archives())
            {
                const bool wanted = targetOf(a.pair) == target || (withKorean && std::string(a.pair) == "m2m");
                if (wanted && !translateModelInstalled(translateModelFolder(a.pair)))
                {
                    bytes += a.size;
                }
            }
            return bytes;
        }

        std::string translateModelBaseUrl()
        {
            // Where the models are published: GitHub Releases of noriaji/charis-models, release models-1 (2026-11-01, research/228). The
            // files there are pinned by size and SHA-256 in the table above; a new set of models gets a new release (models-2 ...) and a
            // new address here. The setting can point to another folder (for a test).
            constexpr char BUILT_IN_URL[] = "https://github.com/noriaji/charis-models/releases/download/models-1/";
            const std::string url = gSavedSettings.getString("PlusTranslateModelUrl");
            return url.empty() ? std::string(BUILT_IN_URL) : url;
        }

        std::string translateModelCredits(const std::string& target)
        {
            std::string out;
            for (const Archive& a : archives())
            {
                if (targetOf(a.pair) == target)
                {
                    out += (out.empty() ? "" : "; ") + std::string(a.pair) + ": " + a.credit + ", converted to CTranslate2";
                }
            }
            return out;
        }

        namespace
        {
            // Follows the shared fetch while it runs, so that a fetch started in a window goes on when the window is closed (and the next
            // file, the unpacking and the end are not left waiting for a window to be drawn). It deletes itself when the fetch is over.
            class FetchPoller : public LLEventTimer
            {
            public:
                FetchPoller() : LLEventTimer(0.5f) { sAlive = true; }
                ~FetchPoller() override { sAlive = false; }

                bool tick() override
                {
                    TranslateModelFetch& fetch = sharedTranslateFetch();
                    fetch.poll();
                    const TranslateModelFetch::State s = fetch.state();
                    return !(s == TranslateModelFetch::State::Running || s == TranslateModelFetch::State::Unpacking);
                }

                static bool sAlive;
            };

            bool FetchPoller::sAlive = false;
        }

        TranslateModelFetch& sharedTranslateFetch()
        {
            // Never destroyed: the viewer ends with it (its curl is tied to the viewer by a job object). When the viewer
            // quits, the fetch is cancelled and the unpack's worker joined, so none is left running into the end with a
            // file half written (research/275 L109).
            static TranslateModelFetch* const sFetch = []
            {
                TranslateModelFetch* fetch = new TranslateModelFetch;
                LLEventPumps::instance().obtain("LLApp").listen("PlusTranslateFetch", [fetch](const LLSD& event)
                {
                    // The main thread only: cancel() joins the unpack's worker, which poll() (main thread) also looks at
                    if (event["status"].asString() == "quitting" && on_main_thread())
                    {
                        fetch->cancel();
                    }
                    return false;
                });
                return fetch;
            }();
            return *sFetch;
        }

        TranslateModelFetch::~TranslateModelFetch()
        {
            cancel();
        }

        void TranslateModelFetch::fail(const std::string& why)
        {
            {
                std::lock_guard<std::mutex> lk(mMutex);
                mFailure = why;
            }
            mState = int(State::Failed);
        }

        bool TranslateModelFetch::start(const std::string& baseUrl, const std::string& target, std::string& error, bool withKorean)
        {
            const State now = state();
            if (now == State::Running || now == State::Unpacking)
            {
                error = "already fetching";
                return false;
            }
            if (mUnpack.joinable())
            {
                mUnpack.join();
            }
            if (baseUrl.empty())
            {
                error = "the translation models have no download address yet";
                return false;
            }
            mQueue.clear();
            for (const Archive& a : archives())
            {
                // the models of the target language, and the Korean one too when Korean text is to be read (not only written)
                const bool wanted = targetOf(a.pair) == target || (withKorean && std::string(a.pair) == "m2m");
                if (wanted && !translateModelInstalled(translateModelFolder(a.pair)))
                {
                    mQueue.push_back(sourceOf(a));
                }
            }
            if (mQueue.empty())
            {
                error = "there is nothing to fetch for this language (installed already, or no model)";
                return false;
            }
            mBaseUrl = baseUrl;
            if (mBaseUrl.back() != '/')
            {
                mBaseUrl += '/';
            }
            // checked here too, so that a bad setting is refused before anything is queued (ModelDownload checks each address)
            if (!validDownloadUrl(mBaseUrl + "en-ja.ctm"))
            {
                error = "the download address is not an https address of the allowed form";
                return false;
            }
            mIndex = 0;
            if (!startCurrent(error))
            {
                return false;
            }
            mState = int(State::Running);
            if (this == &sharedTranslateFetch() && !FetchPoller::sAlive)
            {
                new FetchPoller;   // deletes itself when the fetch ends
            }
            return true;
        }

        std::string TranslateModelFetch::currentArchive() const
        {
            const State s = state();
            if ((s != State::Running && s != State::Unpacking) || mIndex >= mQueue.size())
            {
                return std::string();
            }
            return pairOf(mQueue[mIndex]);
        }

        bool TranslateModelFetch::startCurrent(std::string& error)
        {
            ModelSource source = mQueue[mIndex];
            source.url = mBaseUrl + source.fileName;
            return mDownload.start(source, translateModelRoot(), error);
        }

        void TranslateModelFetch::cancel()
        {
            mDownload.cancel();
            mStopUnpack = true;   // the unpack ends between two pieces: no wait for the 500 MB of the Korean model
            if (mUnpack.joinable())
            {
                mUnpack.join();
            }
            mStopUnpack = false;
            if (state() == State::Running || state() == State::Unpacking)
            {
                mState = int(State::Idle);
            }
        }

        void TranslateModelFetch::poll()
        {
            const State now = state();
            if (now == State::Running)
            {
                switch (mDownload.state())
                {
                    case ModelDownload::State::Done:
                    {
                        const ModelSource a = mQueue[mIndex];
                        const std::string archive = translateModelRoot() + gDirUtilp->getDirDelimiter() + a.fileName;
                        const std::string folder = translateModelFolder(pairOf(a));
                        mUnpackResult = 0;
                        mStopUnpack = false;
                        mState = int(State::Unpacking);
                        mUnpack = std::thread([this, archive, folder] {
                            std::string why;
                            if (unpackTranslateArchive(archive, folder, why, &mStopUnpack))
                            {
                                mUnpackResult = 1;
                            }
                            else
                            {
                                {
                                    std::lock_guard<std::mutex> lk(mMutex);
                                    mFailure = why;
                                }
                                mUnpackResult = 2;
                            }
                            LLFile::remove(archive);
                        });
                        break;
                    }
                    case ModelDownload::State::Failed:
                        fail(mDownload.failure());
                        break;
                    case ModelDownload::State::Idle:   // cancelled
                        mState = int(State::Idle);
                        break;
                    default:
                        break;
                }
            }
            else if (now == State::Unpacking && mUnpackResult.load() != 0)
            {
                if (mUnpack.joinable())
                {
                    mUnpack.join();
                }
                if (mUnpackResult.load() == 2)
                {
                    mState = int(State::Failed);
                    return;
                }
                if (++mIndex >= mQueue.size())
                {
                    mState = int(State::Done);
                    return;
                }
                std::string error;
                if (!startCurrent(error))
                {
                    fail(error);
                    return;
                }
                mState = int(State::Running);
            }
        }

        double TranslateModelFetch::fraction() const
        {
            if (mQueue.empty())
            {
                return 0.0;
            }
            const State now = state();
            if (now == State::Done)
            {
                return 1.0;
            }
            const double part = (now == State::Running) ? mDownload.fraction() : 1.0;
            return (double(mIndex) + part) / double(mQueue.size());
        }

        std::string TranslateModelFetch::failure() const
        {
            std::lock_guard<std::mutex> lk(mMutex);
            return mFailure;
        }
    }
}
