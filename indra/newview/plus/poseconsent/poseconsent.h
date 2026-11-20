#pragma once
#include <functional>
#include <string>
#include <vector>
#include "lluuid.h"

// The consent ledger for posing another avatar on one's own screen, compatible with Aurora (research/336;
// the contract between the branches: research/336-api-contract.md). The state machine is poseconsentcore.h;
// poseconsent.cpp ties it to the bridge (plus/lslbridge), the confirmation, the setting and the block list.
namespace plus::poseconsent
{
    enum class Status { Ask, Pending, Accepted, Refused, NoAnswer };
    enum class Policy { Everyone = 0, Friends = 1, Nobody = 2 };

    struct Granted { LLUUID peer; bool withdrawPending = false; };   // someone we allowed to pose us (for the Withdraw reminder)

    // Main thread only.
    Status status(const LLUUID& peer);
    bool   canAsk(const LLUUID& peer);          // bridge Running, nearby (<= 50 m, same region), not muted, status Ask/NoAnswer, no cooldown
    bool   ask(const LLUUID& peer);             // a person pressed "Ask": sends request, status -> Pending
    void   cancel(const LLUUID& peer);          // Pending -> cancel, Accepted -> end (sends cancel)
    bool   canPose(const LLUUID& peer);         // accepted AND nearby. The poser asks this every time.
    Policy policy();                            // PlusPoseAskPolicy
    const std::vector<Granted>& grantedList();
    void   withdraw(const LLUUID& peer);        // we stop allowing `peer`
    // (research/374b) We allowed `peer` to pose us and have not withdrawn it (a withdraw not sent yet counts as
    // withdrawn): the stage 2 of posereflect.h needs it for each pose it takes.
    bool   mayPoseUs(const LLUUID& peer);

    // (added 2026-11-13 after the 2nd review of poser-others)
    struct Outgoing { LLUUID peer; Status status; };   // Pending or Accepted, ours
    // The people we asked or may pose, for the poser's model list (also when their avatar is out of draw distance).
    // Main thread; valid until the next call into plus::poseconsent (as grantedList()).
    const std::vector<Outgoing>& outgoingList();

    // Called when an Accepted peer is no longer usable (any exit of Accepted, and at the 1 s idle when it stops being usable).
    // The poser must stop the pose of `peer` with the region-independent stop. Also called at logout/teleport/region change for every accepted peer.
    using ConsentLostHandler = std::function<void(const LLUUID& peer)>;
    void setConsentLostHandler(ConsentLostHandler handler);

    // A short localised notice for the poser window ("consent ended" ...). key = a string id in strings_plus.xml.
    using NoticeHandler = std::function<void(const std::string& stringId, const LLUUID& peer)>;
    void setNoticeHandler(NoticeHandler handler);

    // The once-a-second look of the ledger (time-outs, who left or went out of range). poseconsent registers its own
    // idle callback that calls this every frame; it returns at once while nothing is held, and does its work at
    // most once a second however often it is called.
    void idle();
}
