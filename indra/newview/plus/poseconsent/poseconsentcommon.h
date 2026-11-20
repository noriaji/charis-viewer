/**
 * @file poseconsentcommon.h
 * @brief Inside plus/poseconsent only: what the two ledgers of the folder share. poseconsent.cpp (stage 1, Aurora's
 *        pose consent: your screen only) and posereflect.cpp (stage 2, research/374b: the pose is shown on the posed
 *        person's own screen) each have a poseconsentcore.h ledger; the world they look at (who is near, the clock, the
 *        bridge) and the confirmation of a request in are the same code.
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

#ifndef PLUS_POSECONSENTCOMMON_H
#define PLUS_POSECONSENTCOMMON_H

#include "poseconsentcore.h"

#include "plus/lslbridge/lslbridge.h"

#include "lluuid.h"
#include "llstring.h"

#include <string>

namespace plus::poseconsent
{
    constexpr double RANGE_METERS = 50.0;      // Aurora's "nearby"

    core::Id toId(const LLUUID& uuid);
    LLUUID toUUID(const core::Id& id);

    // The five actions of a pose message: the ledger's and the bridge's. false for a value that is none of the five
    // (it is dropped, never taken as another action).
    bool toCore(lslbridge::PoseAction action, core::Action& out);
    lslbridge::PoseAction toBridge(core::Action action);
    const char* actionName(core::Action action);

    // Seconds since the viewer started (F64, never goes back): the clock of the ledgers.
    double now();
    // Quitting, or a logout asked for.
    bool leaving();
    // The bridge is there: running, or getting ready (Starting). Not off, taken off, failed, silent.
    bool bridgeThereNow();

    // Where `peer` is from us, when they are in our region (the avatar's own position, else the region's coarse
    // location). isPresent(): also not muted.
    bool locate(const LLUUID& peer, double& distance);
    bool isPresent(const LLUUID& peer, double& distance);

    // The name for a notice of ours, or the id while the name is not known (never what a message said).
    std::string nameForNotice(const LLUUID& peer);
    // A line of strings_plus.xml in the official tip (SystemMessageTip).
    void showMessage(const char* string_id, const LLStringUtil::format_map_t& args);

    // What the two ledgers' Env have in common: who is near, who is a friend, the bridge, a new nonce. The rest (the
    // block list, the policy, the send, the notices, consentLost) is each ledger's own.
    class WorldEnv : public core::Env
    {
    public:
        core::Id self() const override;
        bool bridgeRunning() const override;
        bool bridgeThere() const override { return bridgeThereNow(); }
        bool inRegion(const core::Id& peer) const override;     // muted or not
        bool present(const core::Id& peer) const override;
        bool nearby(const core::Id& peer) const override;
        bool isFriend(const core::Id& peer) const override;
        core::Id newNonce() override;
    };

    // The confirmation of the requests in of one ledger: one LLNotification at a time, for the oldest request in. sync()
    // runs after everything that can change the requests in (a notification whose request is gone is taken down, the
    // next one is shown). The answer is checked again by the ledger. A request allowed while the bridge could not send
    // it (busy) is not shown again until the bridge runs.
    class Confirmation
    {
    public:
        struct Config
        {
            core::Ledger* ledger = nullptr;
            const char*   stage = "pose";           // for the log: "pose" (stage 1) or "reflect" (stage 2)
            const char*   notification = nullptr;   // notifications.xml: [NAME] [DISTANCE]; buttons 0 allow, 1 refuse, 2 block (and allowAltOption: another allow)
            void (*afterChange)() = nullptr;        // run after an answer: the owner's sync of everything that follows the ledger
            void (*onNameArrival)() = nullptr;      // run (main thread) when the asker's name arrives: the owner's sync again
            int           allowAltOption = -1;      // another button that allows (its index); -1: none
            // After the ledger took the answer, before afterChange(): who, which button, what became of it
            void (*onAnswered)(const LLUUID& peer, int option, core::Answer answer) = nullptr;
        };
        explicit Confirmation(const Config& config) : mConfig(config) {}

        void sync();
        void takeDown();
        bool isShown() const { return mShownNonce.notNull(); }

    private:
        void onAnswer(const LLSD& notification, const LLSD& response);

        Config mConfig;
        LLUUID mShownNonce;         // the request whose confirmation is up; null: none
        LLUUID mShownNotification;  // its LLNotification's id (looked up, not held: no notification outlives the UI)
        LLUUID mNameAskedFor;       // the asker whose name the confirmation last waited for
        double mWaitUntil = 0.0;    // no confirmation is shown before (a "busy" answer)
    };
}

#endif // PLUS_POSECONSENTCOMMON_H
