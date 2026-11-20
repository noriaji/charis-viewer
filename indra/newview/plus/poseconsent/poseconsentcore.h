/**
 * @file poseconsentcore.h
 * @brief The consent ledger for posing another avatar (research/336): the state machine of both sides,
 *        free of the viewer's types so that it can be tested alone (poseconsent_test.cpp).
 *
 * The wire is Aurora's (research/336 2): "pose1|<nonce>|<action>|<target>" between the worn bridges, handed to
 * the viewer as (nonce, action, peer). The values are Aurora's too: an answer within TIMEOUT_SECONDS, then
 * COOLDOWN_SECONDS before the same person is asked again (and, on the receiving side, before their requests
 * are shown again), MAX_SESSIONS on each side. The tables of research/336 (its appendix on the states of poseconsent, the
 * one whose Japanese heading ends in "(implementation)") are what this file does; every exit named there is a branch here.
 *
 * Only the main thread calls. The clock is passed in (seconds, F64, one that never goes back). The viewer side
 * (poseconsent.cpp) gives the world through Env: who is near, who is a friend, the block list, the bridge.
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

#ifndef PLUS_POSECONSENTCORE_H
#define PLUS_POSECONSENTCORE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace plus::poseconsent::core
{
    // A 128-bit id (an agent or a nonce), the bytes of an LLUUID.
    struct Id
    {
        std::uint64_t hi = 0;
        std::uint64_t lo = 0;

        bool isNull() const { return hi == 0 && lo == 0; }
        bool operator==(const Id& other) const { return hi == other.hi && lo == other.lo; }
        bool operator!=(const Id& other) const { return !(*this == other); }
    };

    enum class Action { Request, Accept, Refuse, Cancel, Withdraw };
    enum class Status { Ask, Pending, Accepted, Refused, NoAnswer };
    enum class Policy { Everyone = 0, Friends = 1, Nobody = 2 };

    // What the person posing is told (the viewer turns it into a string of strings_plus.xml).
    enum class Notice
    {
        Accepted,       // the peer allowed us
        Refused,        // the peer said no
        NoAnswer,       // no answer in time, or the peer went away before answering
        AskCancelledBridgeLost, // our bridge stopped while we waited for the answer: we took the request back
        Withdrawn,      // the peer withdrew the permission
        WithdrawnWhileAsking,   // the peer withdrew while we waited for the answer (nothing was allowed yet)
        Ended,          // the permission ended: the peer left, was muted, the bridge went, a region change
        OutOfRange,     // the peer is further than the range: posing paused until they come back
        // Our own side's: the post of an accept failed, so the asker may or may not have it. The permission stays
        // listed (they may pose us), for the person to withdraw (integration review 2, M1)
        AllowUnsure,
        // Our own side's (not the poser's): a request we allowed while the bridge could not send it (Busy) ended
        // before it went (timed out, the asker left or cancelled, out of range, the policy or block list, the bridge
        // went, a teleport or region change; not at a logout, nor at the person's own withdraw()): nothing was allowed
        // (reviews 11, 12).
        AllowNeverSent
    };

    // What became of the answer to a confirmation.
    enum class Answer
    {
        Allowed,        // accept sent: the peer may pose us
        Refused,        // refuse sent: the person said no
        Expired,        // the person allowed, but the request no longer held (60 s past, out of range, the policy or
                        // the block list): refuse sent, as a refusal
        Busy,           // the person allowed, but the bridge, there but not able to send right now, did not: it is getting
                        // ready (Starting: a new URL and its hello, wearing it again, making it again, taking it off; a
                        // few minutes at most, every step has its time limit, then Running, Silent or Failed), or it
                        // runs with 16 sends in flight. The request is kept (marked busy), to be answered again
        NotSent,        // the person allowed, but there is no bridge to send it with (off, taken off, failed, silent):
                        // nothing allowed
        Gone            // no such request any more (timed out, cancelled, answered): nothing done
    };

    // What became of a withdraw.
    enum class Withdrawal
    {
        Withdrawn,      // withdraw sent (or nothing was allowed): no permission left
        NotSent,        // could not be sent: the permission stays listed, marked to be sent again by idle()
        StillNotSent    // as NotSent, for a permission already so marked: by an earlier withdraw (the person was told),
                        // or with no one told: at the start of a teleport, by the withdraw sent on its own when the
                        // peer was muted, or when sendFailed() put a withdraw back
    };

    // An entry of the side that poses: someone we asked (Pending) or may pose (Accepted).
    struct Asked
    {
        Id peer;
        Status status = Status::Pending;
    };

    // Why the session tables are emptied. RegionChanged and Logout send nothing to the other side (research/336 4.1:
    // no cancel is sent then); the person may still be told (Ended for those we may pose, AllowNeverSent for requests
    // allowed but never sent, the latter not at a logout). Teleport (its start: it may fail or stay in the region)
    // tries a cancel to each person we asked (then COOLDOWN_SECONDS before asking them again) and to each we may
    // pose, and a withdraw to each we allowed (reviews 4 and 5); a withdraw that does not go stays, marked to be sent
    // again, the rest is emptied either way.
    enum class ClearReason { RegionChanged, Teleport, Logout };

    constexpr double TIMEOUT_SECONDS  = 60.0;
    constexpr double COOLDOWN_SECONDS = 30.0;    // after a refusal / no answer (both sides); short so a mishap (crash, wrong click) costs little
    constexpr double ENDED_SECONDS    = 600.0;   // how long the ended marks last (late / replayed messages are dropped)
    constexpr std::size_t MAX_SESSIONS = 64;     // per side: Pending + Accepted, and Incoming + Granted
    constexpr std::size_t MAX_REMEMBERED = 64;   // Refused / NoAnswer, and the receiving side's cooldowns
    constexpr std::size_t MAX_ENDED = 128;       // (peer, nonce) of ended sessions: late or replayed messages are dropped
    // A withdraw that could not be sent (the person's, one tried at the start of a teleport, one sent on its own for a
    // peer we muted, or one sendFailed() put back) is sent again by idle(), one a second at most, while the bridge runs
    // and the entry is there: the peer in our region (inRegion, muted too: a muted peer's entry stays until they leave
    // the region; it goes at a region change or a logout; the start of a teleport keeps it marked). The wait between
    // the tries of one entry starts at the first and doubles up to the most (pressing Withdraw again starts it anew).
    // A try that fails costs nothing: the bridge answers false at once.
    constexpr double RESEND_FIRST_SECONDS = 1.0;
    constexpr double RESEND_MOST_SECONDS = 10.0;

    class Env
    {
    public:
        virtual ~Env() = default;
        virtual Id self() const = 0;
        virtual bool bridgeRunning() const = 0;
        // Running, or getting ready (Starting: a new URL and its hello, wearing it again, making it again; it can last a
        // few minutes, and ends in Running, Silent or Failed). A send that fails while this holds is "busy": tried again,
        // not taken as no bridge (review 10). Requests are taken only while this holds (review 11).
        virtual bool bridgeThere() const = 0;
        // In our region, an avatar there and not muted.
        virtual bool present(const Id& peer) const = 0;
        // In our region, an avatar there, muted or not: someone who can still pose us with a permission we gave
        // (integration review 3, L2). The permissions given are judged by this, not by present().
        virtual bool inRegion(const Id& peer) const = 0;
        // present() and within the range (50 m).
        virtual bool nearby(const Id& peer) const = 0;
        virtual bool isFriend(const Id& peer) const = 0;
        // On the block list of pose requests ("block future requests").
        virtual bool isBlocked(const Id& peer) const = 0;
        virtual Policy policy() const = 0;
        virtual Id newNonce() = 0;
        // Through our worn bridge. False when the bridge is not running (getting ready counts: Starting, as above), or
        // when it cannot send right now (16 sends in flight); nothing is queued. The ledger treats bridgeThere() + false
        // as "busy" and tries again.
        virtual bool send(Action action, const Id& nonce, const Id& peer) = 0;
        // An Accepted peer may no longer be posed (every exit of Accepted, and going out of range).
        virtual void consentLost(const Id& peer) = 0;
        virtual void notice(Notice notice, const Id& peer) = 0;
    };

    class Ledger
    {
    public:
        struct Incoming
        {
            Id peer;
            Id nonce;
            double received_at = 0.0;
            bool busy = false;      // allowed once, but the bridge could not send it (Answer::Busy): not shown again
                                    // until the bridge runs; told AllowNeverSent if it ends unsent (any exit but
                                    // being answered again, the person's own withdraw(), and a logout)
        };

        explicit Ledger(Env& env);

        // ---- the side that poses
        Status status(const Id& peer, double now) const;
        bool canAsk(const Id& peer, double now) const;
        bool ask(const Id& peer, double now);
        // Pending: cancel sent, then COOLDOWN_SECONDS before asking them again (status NoAnswer): a Charis on the
        // receiving side cools down on a cancelled request too, so asking it again at once would be dropped
        // unanswered (stricter than an Aurora on that side needs). Accepted: cancel sent, Ask.
        void cancel(const Id& peer, double now);
        // Accepted, in range at the last check (usable) and in range now. The last check is what consentLost()
        // follows: a peer who walked away is not posed at once, and is told lost at the next idle().
        bool canPose(const Id& peer) const;
        // Pending in the order they were asked, then Accepted in the order they accepted; filled into `out`
        // (cleared first).
        void asked(std::vector<Asked>& out) const;
        // Bumped whenever the people asked or accepted change (who, or their status).
        unsigned askedGeneration() const { return mAskedGeneration; }

        // ---- the side that is posed
        // The answer to the confirmation of `nonce` (block: refuse and put the peer on the block list, which the
        // viewer does). Checked again: the request must still be there, in time, the peer near and allowed.
        Answer answer(const Id& nonce, const Id& peer, bool allow, double now);
        // Refuses a request in from `peer` and withdraws the permission given to them (see Withdrawal).
        Withdrawal withdraw(const Id& peer, double now);
        const std::vector<Incoming>& incoming() const { return mIncoming; }   // in order of arrival
        std::size_t grantedCount() const { return mGranted.size(); }
        const Id& grantedPeer(std::size_t i) const { return mGranted[i].peer; }
        // Withdrawn (by the person, at the start of a teleport, on its own for a muted peer, or put back by sendFailed()),
        // but the withdraw is not sent yet (idle() tries again).
        bool grantedWithdrawPending(std::size_t i) const { return mGranted[i].unsent; }
        bool isGranted(const Id& peer) const;

        // ---- a ledger that follows another (research/374b: stage 2 follows stage 1)
        // Ends what the parent no longer holds: each Pending or Accepted peer (whom we asked, or may pose) for whom
        // poserHolds() is false, as cancel() (a request out: cancel sent, the cooldown, no notice); each permission we gave (not marked withdrawn already) for whom posedHolds() is false, as
        // withdraw(). The two answers are looked at before anything is ended (the ends call out).
        void endWhereParentGone(const std::function<bool(const Id&)>& poserHolds, const std::function<bool(const Id&)>& posedHolds,
                                double now);

        // ---- both
        // A message our bridge passed on (already authenticated as coming from `peer`'s attachment).
        void receive(const Id& nonce, Action action, const Id& peer, double now);
        // Once a second: time-outs, who left, who went out of range, a policy made stricter, a withdraw to send again.
        void idle(double now);
        void bridgeLost(double now);
        // A send the bridge took (send() true) did not get through after all: its HTTP post failed (integration review
        // L2; the result is the post's own, whatever the bridge or its address did since). Withdraw: the permission
        // comes back, marked to be sent again (as a withdraw not sent), while the peer is in our region (inRegion, muted
        // too). Accept: the
        // permission stays listed, not marked (the asker may have it: the person withdraws it if they want), and the
        // person is told AllowUnsure (integration review 2, M1). Request: nothing (the request times out here as no
        // answer). Cancel, Refuse: nothing (the other side ends them by time or presence).
        void sendFailed(Action action, const Id& nonce, const Id& peer, double now);
        void clearAll(ClearReason reason, double now);

        // Something is going on that idle() must watch (a request out, a permission, a request in).
        bool active() const { return !mOutgoing.empty() || !mAccepted.empty() || !mIncoming.empty() || !mGranted.empty(); }
        // Bumped whenever the list of people we allowed changes, or one of them is marked to be withdrawn (the
        // reminder window redraws then).
        unsigned grantedGeneration() const { return mGrantedGeneration; }

    private:
        struct Outgoing
        {
            Id peer;
            Id nonce;
            double sent_at = 0.0;
        };
        struct Accepted
        {
            Id peer;
            Id nonce;
            bool usable = true;     // in range at the last check
        };
        struct Remembered           // Refused / NoAnswer, and the receiving side's cooldowns
        {
            Id peer;
            double at = 0.0;
            bool no_answer = false;
        };
        struct Granted
        {
            Id peer;
            Id nonce;
            bool unsent = false;    // withdrawn (by the person, at a teleport's start, for a muted peer, or put back by
                                    // sendFailed()), the withdraw not sent yet
            double next_try_at = 0.0;   // when idle() tries again (seconds, the ledger's clock)
            double wait = RESEND_FIRST_SECONDS;     // the wait before next_try_at: doubled at each failed try,
                                                    // up to RESEND_MOST_SECONDS
        };
        struct Tell                 // clearAll(Teleport): what to send to whom, once the tables are empty
        {
            Action action;
            Id peer;
            Id nonce;
        };
        struct Ended
        {
            Id peer;
            Id nonce;
            double at = 0.0;
        };

        static constexpr std::size_t NONE = static_cast<std::size_t>(-1);
        // The entry of `peer` (each table holds one per peer), or of the session (peer, nonce); NONE if none.
        template <class T> static std::size_t indexOf(const std::vector<T>& list, const Id& peer);
        template <class T> static std::size_t indexOf(const std::vector<T>& list, const Id& peer, const Id& nonce);
        template <class T> static T* find(std::vector<T>& list, const Id& peer);
        template <class T> static const T* find(const std::vector<T>& list, const Id& peer);
        // Takes a request out of mOutgoing (marked ended) and returns it.
        Outgoing takeOutgoing(std::size_t i, double now);
        static const Remembered* findLive(const std::vector<Remembered>& list, const Id& peer, double now);
        static void remember(std::vector<Remembered>& list, const Id& peer, double now, bool no_answer);
        void markEnded(const Id& peer, const Id& nonce, double now);
        bool isEnded(const Id& peer, const Id& nonce, double now) const;
        void unmarkEnded(const Id& peer, const Id& nonce);
        bool allowedToAsk(const Id& peer) const;    // the policy, the friends, the block list
        void receiveRequest(const Id& nonce, const Id& peer, double now);
        // notice: what the person posing is told, null for nothing (they ended it themselves).
        void endAccepted(std::size_t i, bool send_cancel, const Notice* notice, double now);
        // How a request in ends. Refused: refuse sent, cooldown (the person said no, it timed out, the asker left,
        // the policy changed). Cancelled: the asker took it back, nothing sent, cooldown (a request taken back
        // counts too). BridgeLost: refuse tried (the bridge may be gone), no cooldown (not the asker's doing).
        enum class DropIncoming { Refused, Cancelled, BridgeLost };
        void dropIncoming(std::size_t i, DropIncoming how, double now);
        void dropGranted(std::size_t i, double now);
        // idle(): sends one withdraw marked unsent again while the bridge runs (see RESEND_FIRST_SECONDS).
        void resendOneWithdraw(double now);

        Env& mEnv;
        std::vector<Outgoing>   mOutgoing;
        std::vector<Accepted>   mAccepted;
        std::vector<Remembered> mRefused;
        std::vector<Incoming>   mIncoming;
        std::vector<Granted>    mGranted;
        std::vector<Remembered> mInboundCooldown;
        std::vector<Ended>      mEnded;
        unsigned mGrantedGeneration = 0;
        unsigned mAskedGeneration = 0;
    };
}

#endif // PLUS_POSECONSENTCORE_H
