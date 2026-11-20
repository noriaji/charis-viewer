/**
 * @file lslbridge.h
 * @brief The pose bridge (plus/lslbridge): an attachment with a script that carries the pose-consent
 *        messages of Aurora-compatible viewers (research/336). The interface the other branches use
 *        (research/336-api-contract.md); kept as the contract has it.
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

#pragma once
#include <functional>
#include <string>
#include "lluuid.h"

namespace plus::lslbridge
{
    enum class State { Off, Starting, Running, Detached, Failed, Silent };
    // Off: setting off (on by default), or a safe start. Starting: waiting for the login and its outfit (the login screen
    //   included, research/353), creating/attaching/handshaking.
    // Running: the worn bridge answers. Detached: taken off (put on again by itself, 3 times in 10 minutes; off while
    //   its hello is waited for, it stays Starting and a repair is used instead).
    // Failed: could not create (no library item, script upload refused, not Second Life, no attachment place to spare,
    //   3 repairs used up...); the bridge retries by itself where a retry can help (not without a library item, not
    //   outside Second Life: failedForGood(); without an attachment place, once the attachments leave one).
    // Silent: worn but its script has not answered in time (30 s after a region crossing, a wear, a creation or a
    //   teleport); repaired by itself where scripts run (after one more wait when it comes where scripts run, or
    //   across a region change), not while the region gives it no URL.

    enum class PoseAction { Request, Accept, Refuse, Cancel, Withdraw };

    // Main thread only for every function below.
    State state();
    // (added 2026-11-15, research/353) Failed with no new try to come: no library object, not Second Life, or the
    // retries used up. Only turning the setting off and on tries again (and shows why). Not while it waits for an
    // attachment place to spare (tried again once a change of the attachments leaves one).
    bool failedForGood();
    // Create/attach/repair the bridge when the setting PlusPoseBridgeEnable is on (not in a safe start). Cheap and idempotent
    // (it starts only from Off; else the bridge goes on repairing and retrying by itself).
    void ensure();
    // Sends "pose1|<nonce>|<action>|<target>" through the worn bridge (HTTP-in).
    // Returns false when the bridge is not Running, or 16 posts already wait for their answer (nothing is queued).
    bool sendPose(PoseAction action, const LLUUID& nonce, const LLUUID& target);

    // A pose message that reached our own worn bridge, already authenticated by the bridge script
    // and by the viewer (CHAT_TYPE_OWNER, non-null chatter, root attachment item == bridge item (the one this viewer
    // made for the account), no other prim linked to it, owner == self, taken only from the worn object whose
    // contents were seen holding no other script for the URL held (cleanOf)).
    // `peer` is the owner of the sending attachment as the simulator reports it.
    using PoseHandler = std::function<void(const LLUUID& nonce, PoseAction action, const LLUUID& peer)>;
    void setPoseHandler(PoseHandler handler);

    // Called on every change of state() (and once when the handler is set).
    using StateHandler = std::function<void(State state)>;
    void setStateHandler(StateHandler handler);

    // (added 2026-11-14, integration review L2) Result of a sendPose() that was accepted (returned true):
    // ok is the HTTP result of that post only (true iff the bridge answered ok), whatever happened to the URL or the
    // bridge state meanwhile (a result that arrives after the URL changed or after Off still reports its real result).
    // ok == false: timeout, 5xx, refused, a certificate not naming the host... so the message may not have been
    // delivered. Called on the main thread from the next idle (never from inside sendPose), once per accepted
    // sendPose, with the same action/nonce/target (every post ends within its 10 s timeout; not called while the
    // viewer quits). The hello is not reported.
    // poseconsent uses it to mark an undelivered Withdraw as "withdraw pending" again (and to retry).
    using SendResultHandler = std::function<void(PoseAction action, const LLUUID& nonce, const LLUUID& target, bool ok)>;
    void setSendResultHandler(SendResultHandler handler);

    // ------------------------------------------------------------------ pose sharing (research/374, 374b)
    // Each sender below is Running-only, after the hello, and returns false (nothing is queued) otherwise. A send is
    // an HTTP post that ends in the bridge's shout (a share, on SHARE_CHANNEL) or in a message said to one avatar (a
    // reflect request or answer, a push). Seq numbers are 0 to 999999999 and payloads base64 text of at most 900
    // characters (plus::lslbridge::proto); an empty payload is the end of a share or a push.

    // A share of the pose of our own avatar, shouted by the bridge for the Charis Viewers near (100 m). aud: 0
    // everyone, 1 friends only (the receiving viewer keeps to it). The HTTP lane keeps the latest only: the body is
    // posted at once, or waits in the single place of its key (and replaces an older waiting one) while a post of that
    // key is on the way, until 0.35 s have passed since the END of the key's last post (the receiving bridge takes one
    // avatar's share every 0.3 s, so a faster one would be lost), or while 4 shares and pushes have been launched in the
    // last second (both lanes together, a sliding window of the launches). A replacing body is never refused for the
    // rate; false is for what cannot be queued (not Running, 8 posts on their way or waiting, or an end that this body
    // would replace). The end (sendShareEnd, empty payload, for everyone) has a window of its own (2 launched in any
    // second, so the 4 and the 2 stay within the 6 things a second the bridge script says), keeps the 0.35 s and is not
    // replaced by a share; ends sent faster than that are merged like any waiting body (the last one always goes, so
    // do not count on 3 ends reaching the bridge). The posts on their way are at most 8 per lane, also the ones
    // sent from the waiting places. Its own count: not part of the 16 of sendPose.
    bool sendShare(int aud, int seq, const std::string& payload);
    bool sendShareEnd(int seq);

    // A share of another avatar's bridge that reached our own worn bridge, authenticated by the bridge script
    // (peer is the owner of the shouting attachment as the simulator reports it) and by the viewer as for the pose
    // handler: payload is base64 text (checked), empty for the end of that avatar's share, in which case aud is 0.
    // Not called for a line naming ourselves.
    using ShareHandler = std::function<void(const LLUUID& peer, int aud, int seq, const std::string& payload)>;
    void setShareHandler(ShareHandler handler);

    // The requests and answers of the reflect of a pose pushed to its owner (research/374b): as sendPose() and the
    // pose handler, on a channel of their own (another viewer's or Aurora's bridge never hears them). The send result
    // handler of reflect is called as setSendResultHandler()'s is for pose.
    bool sendReflect(PoseAction action, const LLUUID& nonce, const LLUUID& target);
    void setReflectHandler(PoseHandler handler);
    void setReflectResultHandler(SendResultHandler handler);

    // A pose pushed to its owner (research/374b): said to target only. One lane per target (the latest only, as for
    // sendShare, keyed by the target, 0.35 s after the end of the target's last post), 4 shares and pushes launched in
    // any second in all (the window of sendShare is this one), at most 8 targets on their way. An empty payload is the end.
    bool sendPush(const LLUUID& target, int seq, const std::string& payload);
    bool sendPushEnd(const LLUUID& target, int seq);
    // A push from another avatar to us: peer is the owner of the sending attachment, payload as above (empty: the end).
    using PushHandler = std::function<void(const LLUUID& peer, int seq, const std::string& payload)>;
    void setPushHandler(PushHandler handler);
}
