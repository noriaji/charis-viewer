/**
 * @file posermotion.cpp
 * @brief "Motion Between Poses": the feature's model and helper client, its settings and its end at quit (see
 * posermotion.h).
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

#include "posermotion.h"

#include "plus/plus.h"
#include "plus/memguard/memguard.h"
#include "plus/subtitle/subtitleclient.h"     // helperPath()
#include "plus/subtitle/subtitledownload.h"

#include "llevents.h"
#include "lleventtimer.h"
#include "llthread.h"
#include "llviewercontrol.h"

#include <algorithm>

#if LL_WINDOWS
#include <windows.h>
#endif

namespace plus::posermotion
{
    namespace
    {
        constexpr F32 DOWNLOAD_POLL_SEC = 0.2f;

        MotionModel*  sModel  = nullptr;   // made on first use, never destroyed (posermotion.h)
        MotionClient* sClient = nullptr;

        // Follows the download while it runs, so that it goes on when the window is closed (research/324 section
        // 5.2, as plus/subtitle's FetchPoller). Deletes itself when the download is over.
        class DownloadPoller : public LLEventTimer
        {
        public:
            DownloadPoller() : LLEventTimer(DOWNLOAD_POLL_SEC) { sAlive = true; }
            ~DownloadPoller() override { sAlive = false; }

            bool tick() override
            {
                MotionModel& model = motionModel();
                model.poll();
                const ModelState s = model.state();
                return !(s == ModelState::Downloading || s == ModelState::Verifying);   // true: delete me
            }

            static bool sAlive;
        };

        bool DownloadPoller::sAlive = false;

        // The viewer's download: plus/subtitle's ModelDownload (curl.exe, https only, size and SHA-256 before the name)
        class DownloadFetcher : public ModelFetcher
        {
        public:
            bool start(const MotionModelSpec& spec, const std::string& folder, ModelFailure& failure) override
            {
                const plus::subtitle::ModelSource source{ spec.url, spec.file_name, spec.size_bytes, spec.sha256 };
                std::string error;
                if (!mDownload.start(source, folder, error))
                {
                    // ModelDownload's words carry no path (the folder, curl's absence, the address's form)
                    LL_WARNS("PoserMotion") << "cannot start the model download: " << error << LL_ENDL;
                    failure = ModelFailure::CannotStart;
                    return false;
                }
                mFailureLogged = false;
                if (!DownloadPoller::sAlive)
                {
                    new DownloadPoller;   // deletes itself when the download is over
                }
                return true;
            }

            void cancel() override { mDownload.cancel(); }

            Progress progress() const override
            {
                switch (mDownload.state())
                {
                case plus::subtitle::ModelDownload::State::Running:   return Progress::Running;
                case plus::subtitle::ModelDownload::State::Verifying: return Progress::Verifying;
                case plus::subtitle::ModelDownload::State::Done:      return Progress::Done;
                case plus::subtitle::ModelDownload::State::Failed:    return Progress::Failed;
                case plus::subtitle::ModelDownload::State::Idle:      break;
                }
                return Progress::Idle;
            }

            double fraction() const override { return mDownload.fraction(); }

            ModelFailure failure() const override
            {
                if (!mFailureLogged && mDownload.state() == plus::subtitle::ModelDownload::State::Failed)
                {
                    // once per download; ModelDownload's sentence names no path (curl's code, size, SHA-256, the rename's error)
                    mFailureLogged = true;
                    LL_WARNS("PoserMotion") << "the model download failed: " << mDownload.failure() << LL_ENDL;
                }
                using Kind = plus::subtitle::ModelDownload::FailureKind;
                switch (mDownload.failureKind())
                {
                case Kind::Size: return ModelFailure::Size;
                case Kind::Hash: return ModelFailure::Hash;
                case Kind::Disk: return ModelFailure::Disk;
                case Kind::Network:
                case Kind::None: break;
                }
                return ModelFailure::Network;
            }

        private:
            plus::subtitle::ModelDownload mDownload;
            mutable bool mFailureLogged = false;   // the failure of this download is in the log (start() resets it)
        };

        // Room for the helper's model (about 0.8 GB): the free physical memory and memguard's word (research/324
        // section 2.5). Read once per start, not while the helper runs.
        bool enoughMemory()
        {
            if (plus::memguard::low())
            {
                return false;
            }
#if LL_WINDOWS
            MEMORYSTATUSEX memory = {};
            memory.dwLength = sizeof(memory);
            if (GlobalMemoryStatusEx(&memory) && memory.ullAvailPhys < MOTION_MIN_FREE_BYTES)
            {
                return false;
            }
#endif
            return true;
        }

        MotionClientConfig viewerConfig()
        {
            static LLCachedControl<S32> threads(gSavedSettings, "PlusPoserMotionThreads", 0);
            MotionClientConfig config;
            config.helper_exe   = plus::subtitle::helperPath(MOTION_HELPER_EXE);
            config.model_folder = motionModel().folder();
            config.model_file   = motionModelSpec().file_name;
            config.threads      = std::clamp((S32)threads, 0, MOTION_MAX_THREADS);
            config.enough_memory = &enoughMemory;
            config.on_model_failure = [](ModelLoadFailure why) { motionModel().onHelperLoadFailure(why); };
            return config;
        }

        void init()
        {
            // The viewer quits (LLApp's "quitting", before cleanup()): the helper and curl end now, their threads
            // are joined, not left for the static destructors (research/324 section 5.3). On the main thread only
            // (the client and the download are the main thread's).
            LLEventPumps::instance().obtain("LLApp").listen("PlusPoserMotion", [](const LLSD& event)
            {
                if (event["status"].asString() == "quitting" && on_main_thread())
                {
                    if (sClient)
                    {
                        sClient->stop();
                    }
                    if (sModel)
                    {
                        sModel->shutdown();
                    }
                }
                return false;
            });
        }

        plus::FeatureRegistration sRegistration({ "posermotion", &init });
    }

    MotionModel& motionModel()
    {
        if (!sModel)
        {
            sModel = new MotionModel(motionModelSpec(), motionModelFolder(), std::make_unique<DownloadFetcher>());
        }
        return *sModel;
    }

    MotionClient& motionClient()
    {
        if (!sClient)
        {
            sClient = new MotionClient(&viewerConfig);
        }
        return *sClient;
    }

    bool poserMotionLog()
    {
        static LLCachedControl<bool> log(gSavedSettings, "PlusPoserMotionLog", false);
        return log;
    }
} // namespace plus::posermotion
