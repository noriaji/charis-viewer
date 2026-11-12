/**
 * @file update.cpp
 * @brief Installing a new Charis Viewer the user agreed to (research/326): the state,
 *        the chip at the bottom right and the test backend.
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

// How it runs (research/326 sections 3, 4.5, 8):
//   plus/updatecheck finds a newer version (latest.json). With a backend (Setup.exe) and
//   PlusUpdateInstall 1 its band and window say "Download and update"; pressed (or at
//   once with PlusUpdateInstall 2) begin() goes Idle -> Checking and calls the backend's
//   start(). The backend reports Downloading, the progress, then Ready, Failed or
//   Cancelled. The chip shows every state but Idle (Ready until hidden with x); while an
//   update is engaged() (not Idle) plus/updatecheck shows no band or window. A Ready that
//   comes after Cancel was pressed is thrown away (the agreement was taken back).
//   Turning PlusUpdateCheck off or PlusUpdateInstall to 0 stops a check or download
//   (cancel) and throws a ready update away (discard): nothing is put in without the
//   user's say.
//   tick() runs on the idle list only while something is going on, the chip is up, or the
//   test backend runs; otherwise nothing of this runs.

#include "llviewerprecompiledheaders.h"

#include "update.h"
#include "plus.h"
#include "photoscreen/photoscreen.h"
#include "photoscreen/psui/psstyle.h"
#include "photoscreen/psui/pswidgets.h"
#include "safemode/safemode_badge.h"

#include "llapp.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llpanel.h"
#include "llrootview.h"
#include "llstartup.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

namespace plus::update
{
    namespace
    {
        constexpr char CHIP_FILE[]        = "panel_plus_update_chip.xml";
        constexpr char SETTING_CHECK[]    = "PlusUpdateCheck";      // plus/updatecheck's on/off: off stops this too
        constexpr char SETTING_INSTALL[]  = "PlusUpdateInstall";
        constexpr char SETTING_DUMMY[]    = "PlusUpdateDummy";

        constexpr U32 INSTALL_ONLY_TELL   = 0;
        constexpr U32 INSTALL_ASK         = 1;
        constexpr U32 INSTALL_AUTOMATIC   = 2;

        constexpr S32 CHIP_MARGIN         = 12;   // the chip to the world view's right and bottom

        // The test backend (PlusUpdateDummy): a download of DUMMY_TOTAL_BYTES in DUMMY_SECONDS,
        // after DUMMY_CHECK_SECONDS of "reading the feed"; 2 fails at DUMMY_FAIL_FRACTION.
        constexpr U32    DUMMY_OFF           = 0;
        constexpr U32    DUMMY_FAIL          = 2;
        constexpr uint64_t DUMMY_TOTAL_BYTES = uint64_t(280) * 1024 * 1024;
        constexpr F64    DUMMY_CHECK_SECONDS = 1.0;
        constexpr F64    DUMMY_SECONDS       = 20.0;
        constexpr F64    DUMMY_FAIL_FRACTION = 0.4;

        // ---- State (all on the main thread; research/326 section 8, table 2) ------------
        Machine sMachine;
        const Backend* sBackend = nullptr;      // the real one (llvelopack.cpp), or none
        const Backend* sActive  = nullptr;      // the one begin() started (real or test); kept until Idle
        std::string sExpectedPackageVersion;    // begin()'s package_version ("" not known)
        std::string sOfferedPackageVersion;     // the feed's (reportPackageVersion()), "" not yet
        bool sTickRunning = false;              // tick() is on the idle list

        bool checkOn()
        {
            static LLCachedControl<bool> on(gSavedSettings, SETTING_CHECK, true);
            return on();
        }

        U32 installMode()
        {
            static LLCachedControl<U32> mode(gSavedSettings, SETTING_INSTALL, INSTALL_ASK);
            const U32 value = mode();
            return value <= INSTALL_AUTOMATIC ? value : INSTALL_ASK;   // a broken value: ask
        }

        U32 dummyMode()
        {
            static LLCachedControl<U32> dummy(gSavedSettings, SETTING_DUMMY, DUMMY_OFF);
            return dummy();
        }

        // ---- The test backend ----------------------------------------------------------
        // Nothing is sent or written; the idle clock makes the progress. Only without a real
        // backend (the zip, a viewer run from the build folder).
        struct DummyRun
        {
            bool running = false;
            bool cancel  = false;
            bool fail    = false;
            bool downloading = false;
            F64  started_at = 0.0;              // LLTimer::getTotalSeconds()
        };
        DummyRun sDummy;

        void dummyStart()
        {
            sDummy = DummyRun();
            sDummy.running = true;
            sDummy.fail = dummyMode() == DUMMY_FAIL;
            sDummy.started_at = LLTimer::getTotalSeconds();
            LL_INFOS("Update") << "Test update (" << SETTING_DUMMY << "): started" << LL_ENDL;
        }

        void dummyCancel()
        {
            sDummy.cancel = true;
        }

        void dummyDiscard()
        {
            LL_INFOS("Update") << "Test update: thrown away" << LL_ENDL;
        }

        void dummyRestart()
        {
            LL_INFOS("Update") << "Test update: \"Restart and update\" pressed; the test does not quit" << LL_ENDL;
        }

        void dummyKeepRunning()
        {
        }

        const Backend DUMMY_BACKEND = { &dummyStart, &dummyCancel, &dummyDiscard, &dummyRestart, &dummyKeepRunning };

        // One step of the test download, from tick().
        void dummyStep()
        {
            if (!sDummy.running)
            {
                return;
            }
            if (sDummy.cancel)
            {
                sDummy.running = false;
                reportCancelled();
                return;
            }
            const F64 elapsed = LLTimer::getTotalSeconds() - sDummy.started_at;
            if (!sDummy.downloading)
            {
                if (elapsed >= DUMMY_CHECK_SECONDS)
                {
                    sDummy.downloading = true;
                    reportDownloading(DUMMY_TOTAL_BYTES);
                }
                return;
            }
            const F64 fraction = llclamp((elapsed - DUMMY_CHECK_SECONDS) / DUMMY_SECONDS, 0.0, 1.0);
            if (sDummy.fail && fraction >= DUMMY_FAIL_FRACTION)
            {
                sDummy.running = false;
                reportFailed(Failure::DownloadFailed);
                return;
            }
            reportProgress(static_cast<uint64_t>(fraction * static_cast<F64>(DUMMY_TOTAL_BYTES)), DUMMY_TOTAL_BYTES);
            if (fraction >= 1.0)
            {
                sDummy.running = false;
                reportReady();
            }
        }

        // The backend begin() would start: the real one, else the test one when asked for.
        const Backend* availableBackend()
        {
            if (sBackend)
            {
                return sBackend;
            }
            return dummyMode() != DUMMY_OFF ? &DUMMY_BACKEND : nullptr;
        }

        // ---- The chip ------------------------------------------------------------------

        class UpdateChip : public LLPanel
        {
        public:
            // Built from the file alone, not yet in a parent (psui::addOverlay() puts it in the root view).
            static UpdateChip* create()
            {
                UpdateChip* chip = new UpdateChip();
                if (!chip->buildFromFile(CHIP_FILE))
                {
                    delete chip;
                    return nullptr;
                }
                return chip;
            }

            bool postBuild() override
            {
                mText   = getChild<LLUICtrl>("chip_text");
                mAction = getChild<psui::PsButton>("chip_action");
                mClose  = getChild<LLView>("chip_close");
                // The buttons only change the state; tick() takes the chip away (a view does not
                // delete itself in its own callback).
                mAction->setCommitCallback([](LLUICtrl*, const LLSD&) { onAction(); });
                getChild<LLUICtrl>("chip_close")->setCommitCallback([](LLUICtrl*, const LLSD&) { onClose(); });
                return true;
            }

            void findSnapRegion(LLView* root)
            {
                if (LLView* view = psui::findSnapRegion(root))
                {
                    mSnapRegion = view->getHandle();
                }
            }

            // The words, the tip and the buttons for the machine's state; only when it changed.
            void refresh(const Machine& machine)
            {
                if (mShownSerial == machine.serial() && mFilled)
                {
                    return;
                }
                mFilled = true;
                mShownSerial = machine.serial();

                LLStringUtil::format_map_t args;
                args["[VERSION]"] = machine.version();
                args["[PERCENT]"] = llformat("%d", machine.percent());
                args["[DONE]"]    = llformat("%llu", (unsigned long long)wholeMB(machine.doneBytes()));
                args["[TOTAL]"]   = llformat("%llu", (unsigned long long)wholeMB(machine.totalBytes()));
                LLStringUtil::format_map_t why_args;
                why_args["[AGREED]"]  = sExpectedPackageVersion.empty() ? std::string("(unknown)") : sExpectedPackageVersion;
                why_args["[OFFERED]"] = sOfferedPackageVersion.empty() ? std::string("(unknown)") : sOfferedPackageVersion;
                args["[WHY]"]     = getString(whyString(machine.failure()), why_args);

                std::string text, tip, button, button_tip;
                bool closable = false;
                switch (machine.phase())
                {
                case Phase::Checking:
                case Phase::Downloading:
                    if (machine.cancelRequested())
                    {
                        text = getString("cancelling");
                    }
                    else if (machine.phase() == Phase::Checking)
                    {
                        text = getString("checking");
                    }
                    else
                    {
                        text = getString(machine.totalBytes() ? "downloading" : "downloading_size_unknown", args);
                    }
                    tip = getString(machine.phase() == Phase::Checking ? "tip_checking"
                                    : machine.totalBytes() ? "tip_downloading" : "tip_downloading_size_unknown", args);
                    button = getString("btn_cancel");
                    button_tip = getString("btn_cancel_tip");
                    break;
                case Phase::Ready:
                    text = getString(machine.restartRequested() ? "quitting" : "ready");
                    tip = getString("tip_ready", args);
                    button = getString("btn_restart");
                    button_tip = getString("btn_restart_tip");
                    closable = true;
                    break;
                case Phase::Failed:
                    text = getString("failed");
                    tip = getString("tip_failed", args);
                    button = getString("btn_retry");
                    button_tip = getString("btn_retry_tip");
                    closable = true;
                    break;
                case Phase::Idle:
                    break;
                }
                mText->setValue(text);
                mText->setToolTip(tip);
                setToolTip(tip);
                mAction->setLabelText(button);
                mAction->setToolTip(button_tip);
                mAction->setEnabled(!machine.cancelRequested() && !machine.restartRequested());
                mClose->setVisible(closable);
            }

            // 12 from the right and the bottom of the area between the toolbars (in the parent's, the root
            // view's, coordinates); only when that moved.
            void place()
            {
                const LLView* parent = getParent();
                if (!parent)
                {
                    return;
                }
                const LLRect area = psui::worldChipArea(*parent, mSnapRegion.get());
                const S32 left   = llmax(area.mLeft, area.mRight - CHIP_MARGIN - getRect().getWidth());
                // Above the safe mode badge, which takes the same corner while it is shown
                const S32 bottom = area.mBottom + CHIP_MARGIN + plus::safemode::badge::cornerHeight();
                if (getRect().mLeft != left || getRect().mBottom != bottom)
                {
                    setOrigin(left, bottom);
                }
            }

        protected:
            UpdateChip() = default;

        private:
            static const char* whyString(Failure failure)
            {
                switch (failure)
                {
                case Failure::NoUpdateInFeed:  return "why_no_update";
                case Failure::FeedUnreadable:  return "why_feed";
                case Failure::PackageMismatch: return "why_package";
                case Failure::HandOffFailed:   return "why_install";
                case Failure::VersionMismatch: return "why_version";
                case Failure::None:
                case Failure::DownloadFailed:  break;
                }
                return "why_download";
            }

            static void onAction();
            static void onClose();

            LLUICtrl*       mText   = nullptr;  // children of this chip: they live as long as it does
            psui::PsButton* mAction = nullptr;
            LLView*         mClose  = nullptr;
            U32             mShownSerial = 0;
            bool            mFilled = false;
            LLHandle<LLView> mSnapRegion;       // the toolbars' snap region (floater_snap_region), when there is one
        };

        LLHandle<LLView> sChip;
        // Where the chip is among the root view's children: at the back in the world (over the 3D picture,
        // under floaters, menus, tool tips and modal windows: it takes no clicks from them), at the front
        // before (over the login panel, which covers the window).
        psui::OverlayOrder sChipOrder = psui::OverlayOrder::Front;
        bool sChipFailed = false;           // the chip's XUI could not be built: not tried again in this run

        void removeChip()
        {
            if (LLView* chip = sChip.get())
            {
                if (LLView* parent = chip->getParent())
                {
                    parent->removeChild(chip);
                }
                delete chip;
            }
            sChip = LLHandle<LLView>();
        }

        // The chip shown for the state, hidden in the photo screen and with the UI hidden.
        void updateChip()
        {
            const bool wanted = sMachine.chipWanted();
            if (!wanted)
            {
                removeChip();
                return;
            }
            LLView* root = gViewerWindow ? gViewerWindow->getRootView() : nullptr;
            if (!root)
            {
                return;     // too early: the next frame
            }
            const psui::OverlayOrder order = LLStartUp::getStartupState() >= STATE_STARTED
                ? psui::OverlayOrder::Back : psui::OverlayOrder::Front;
            UpdateChip* chip = static_cast<UpdateChip*>(sChip.get());
            if (!chip)
            {
                if (sChipFailed)
                {
                    return;
                }
                chip = UpdateChip::create();
                if (!chip)
                {
                    LL_WARNS("Update") << CHIP_FILE << " could not be built; no chip in this run" << LL_ENDL;
                    sChipFailed = true;
                    return;
                }
                chip->findSnapRegion(root);
                psui::addOverlay(*root, *chip, order, sChip);
                sChipOrder = order;
            }
            else if (order != sChipOrder)
            {
                // The world is up: the chip goes under the floaters and menus
                if (order == psui::OverlayOrder::Back)
                {
                    root->sendChildToBack(chip);
                }
                else
                {
                    root->sendChildToFront(chip);
                }
                sChipOrder = order;
            }
            const bool shown = !plus::photoscreen::isActive() && gViewerWindow->getUIVisibility();
            if (chip->getVisible() != shown)
            {
                chip->setVisible(shown);
            }
            if (shown)
            {
                chip->refresh(sMachine);
                chip->place();
            }
        }

        // Checking or Downloading: asks the backend to stop (once); it reports cancelled when it has.
        void requestStop(const char* why)
        {
            if (sMachine.requestCancel())
            {
                LL_INFOS("Update") << why << "; the update is stopped" << LL_ENDL;
                if (sActive)
                {
                    sActive->cancel();
                }
            }
        }

        // Failed or Ready -> Idle; the backend is let go of.
        void clearState()
        {
            if (sMachine.clear())
            {
                sActive = nullptr;
            }
        }

        // Turned off while an update is on its way or waiting: stopped, or thrown away.
        void stopWhenTurnedOff()
        {
            if (checkOn() && installMode() != INSTALL_ONLY_TELL)
            {
                return;
            }
            switch (sMachine.phase())
            {
            case Phase::Checking:
            case Phase::Downloading:
                requestStop("Updates turned off (PlusUpdateCheck or PlusUpdateInstall)");
                break;
            case Phase::Ready:
                LL_INFOS("Update") << "Updates turned off (" << SETTING_CHECK << " or " << SETTING_INSTALL
                                   << "); the downloaded update is not put in" << LL_ENDL;
                if (sActive)
                {
                    sActive->discard();
                }
                clearState();
                break;
            case Phase::Failed:
                clearState();
                break;
            case Phase::Idle:
                break;
            }
        }

        bool tick()
        {
            if (LLApp::isExiting())
            {
                removeChip();
                sTickRunning = false;
                return true;
            }
            stopWhenTurnedOff();
            if (sMachine.restartRequested() && sActive && sActive != &DUMMY_BACKEND
                && !LLAppViewer::instance()->quitRequested())
            {
                // The quit was called off (logging out cancelled): "Restart and update" is offered again, and
                // the update goes in at the next end without starting the viewer again (as before the press).
                LL_INFOS("Update") << "The quit was called off; \"Restart and update\" is offered again" << LL_ENDL;
                if (sMachine.cancelRestart())
                {
                    sActive->keepRunning();
                }
            }
            dummyStep();
            updateChip();
            if (sMachine.phase() == Phase::Idle && !sDummy.running)
            {
                removeChip();
                sTickRunning = false;
                return true;
            }
            return false;
        }

        void ensureTick()
        {
            if (!sTickRunning)
            {
                sTickRunning = true;
                doOnIdleRepeating(&tick);
            }
        }

        void UpdateChip::onAction()
        {
            switch (sMachine.phase())
            {
            case Phase::Checking:
            case Phase::Downloading:
                requestStop("Cancel pressed");
                break;
            case Phase::Ready:
                if (!sMachine.requestRestart())
                {
                    break;      // pressed already: the viewer is quitting
                }
                LL_INFOS("Update") << "\"Restart and update\" pressed" << LL_ENDL;
                if (sActive)
                {
                    sActive->restartNow();
                    if (sActive != &DUMMY_BACKEND)
                    {
                        LLAppViewer::instance()->requestQuit();
                    }
                }
                break;
            case Phase::Failed:
                LL_INFOS("Update") << "Retry pressed" << LL_ENDL;
                if (!begin(sMachine.version(), sExpectedPackageVersion))
                {
                    clearState();   // cannot start now (begin() said why): the chip goes
                }
                break;
            case Phase::Idle:
                break;
            }
        }

        void UpdateChip::onClose()
        {
            if (sMachine.phase() == Phase::Ready)
            {
                sMachine.hideReady();
                LL_INFOS("Update") << "The chip was hidden; the update still goes in when the viewer ends" << LL_ENDL;
            }
            else
            {
                clearState();
            }
        }

        void init()
        {
            LL_INFOS("Update") << (sBackend ? "Updates can be installed (Setup.exe installation)"
                                            : "No installer here (zip, or run from a build folder): updates are only told about")
                               << LL_ENDL;
        }

        plus::FeatureRegistration sRegistration({ "update", &init });
    }

    void setBackend(const Backend* backend)
    {
        if (!sBackend)
        {
            sBackend = backend;
        }
    }

    bool hasInstaller()
    {
        return availableBackend() != nullptr;
    }

    bool canInstall()
    {
        return hasInstaller() && checkOn() && installMode() != INSTALL_ONLY_TELL;
    }

    bool installsWithoutAsking()
    {
        return canInstall() && installMode() == INSTALL_AUTOMATIC;
    }

    bool engaged()
    {
        return sMachine.engaged();
    }

    const std::string& expectedPackageVersion()
    {
        return sExpectedPackageVersion;
    }

    void reportPackageVersion(const std::string& package_version)
    {
        sOfferedPackageVersion = package_version;
    }

    bool begin(const std::string& version, const std::string& package_version)
    {
        if (LLApp::isExiting())
        {
            return false;
        }
        if (!canInstall())
        {
            LL_INFOS("Update") << "Not started: updates cannot be installed now (" << SETTING_CHECK << " off, "
                               << SETTING_INSTALL << " 0, or no installer)" << LL_ENDL;
            return false;
        }
        const Backend* backend = availableBackend();
        if (!sMachine.begin(version))
        {
            LL_INFOS("Update") << "Not started: an update is " << phaseName(sMachine.phase()) << " already" << LL_ENDL;
            return false;
        }
        sActive = backend;
        sExpectedPackageVersion = package_version;
        sOfferedPackageVersion.clear();
        LL_INFOS("Update") << "Installing " << version << " (package " << (package_version.empty() ? "not known" : package_version) << ")" << " (agreed to; " << SETTING_INSTALL << " " << installMode() << ")" << LL_ENDL;
        ensureTick();
        sActive->start();
        return true;
    }

    bool isCancelRequested()
    {
        return sMachine.cancelRequested();
    }

    void reportDownloading(uint64_t total_bytes)
    {
        if (sMachine.downloading(total_bytes))
        {
            LL_INFOS("Update") << "Downloading " << total_bytes << " bytes" << LL_ENDL;
        }
        ensureTick();
    }

    void reportProgress(uint64_t done_bytes, uint64_t total_bytes)
    {
        sMachine.progress(done_bytes, total_bytes);
    }

    void reportReady()
    {
        if (sMachine.cancelRequested())
        {
            // Cancel (or a setting) came while the last of it was being handed over: the agreement was
            // taken back, so it is not put in.
            LL_INFOS("Update") << sMachine.version() << " came in after the update was stopped; not put in" << LL_ENDL;
            if (sActive)
            {
                sActive->discard();
            }
            if (sMachine.cancelled())
            {
                sActive = nullptr;
            }
        }
        else if (sMachine.ready())
        {
            LL_INFOS("Update") << sMachine.version() << " is ready; put in when the viewer ends" << LL_ENDL;
        }
        ensureTick();
    }

    void reportFailed(Failure why)
    {
        if (sMachine.cancelRequested())
        {
            // Stopped by the user (or a setting) before the error came: no failure with Retry is shown
            LL_INFOS("Update") << "The update was stopped (" << failureName(why) << " after Cancel)" << LL_ENDL;
            if (sMachine.cancelled())
            {
                sActive = nullptr;
            }
        }
        else if (sMachine.failed(why))
        {
            LL_WARNS("Update") << "The update failed: " << failureName(why) << LL_ENDL;
        }
        ensureTick();
    }

    void reportCancelled()
    {
        if (sMachine.cancelled())
        {
            LL_INFOS("Update") << "The update was stopped" << LL_ENDL;
            sActive = nullptr;
        }
        ensureTick();
    }
}
