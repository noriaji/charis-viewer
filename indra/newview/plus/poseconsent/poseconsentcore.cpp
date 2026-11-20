/**
 * @file poseconsentcore.cpp
 * @brief The consent ledger for posing another avatar: see poseconsentcore.h and research/336
 *        (its appendix on the states of poseconsent, the one whose Japanese heading ends in "(implementation)").
 *
 * Every handler of Env (send, consentLost, notice) may call back into the ledger: each exit puts its own tables
 * right first and calls out afterwards, and the loops of idle() start again from the top after a call out
 * (the lists hold MAX_SESSIONS at most, so this stays small).
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

#include "poseconsentcore.h"

#include <algorithm>

namespace plus::poseconsent::core
{
    namespace
    {
        // Drops the entries older than `keep` seconds (Remembered and Ended both have peer and at).
        template <class T>
        void pruneOlderThan(std::vector<T>& list, double now, double keep)
        {
            list.erase(std::remove_if(list.begin(), list.end(), [&](const T& entry) { return now - entry.at >= keep; }),
                       list.end());
        }

        // Makes room for one more: the oldest entry goes while the list is full.
        template <class T>
        void makeRoom(std::vector<T>& list, std::size_t limit)
        {
            while (!list.empty() && list.size() >= limit)
            {
                list.erase(std::min_element(list.begin(), list.end(),
                                            [](const T& a, const T& b) { return a.at < b.at; }));
            }
        }
    }

    Ledger::Ledger(Env& env)
        : mEnv(env)
    {
        mOutgoing.reserve(MAX_SESSIONS);
        mAccepted.reserve(MAX_SESSIONS);
        mRefused.reserve(MAX_REMEMBERED);
        mIncoming.reserve(MAX_SESSIONS);
        mGranted.reserve(MAX_SESSIONS);
        mInboundCooldown.reserve(MAX_REMEMBERED);
        mEnded.reserve(MAX_ENDED);
    }

    template <class T>
    std::size_t Ledger::indexOf(const std::vector<T>& list, const Id& peer)
    {
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            if (list[i].peer == peer)
            {
                return i;
            }
        }
        return NONE;
    }

    template <class T>
    std::size_t Ledger::indexOf(const std::vector<T>& list, const Id& peer, const Id& nonce)
    {
        const std::size_t i = indexOf(list, peer);
        return (i != NONE && list[i].nonce == nonce) ? i : NONE;
    }

    template <class T>
    T* Ledger::find(std::vector<T>& list, const Id& peer)
    {
        const std::size_t i = indexOf(list, peer);
        return i == NONE ? nullptr : &list[i];
    }

    template <class T>
    const T* Ledger::find(const std::vector<T>& list, const Id& peer)
    {
        const std::size_t i = indexOf(list, peer);
        return i == NONE ? nullptr : &list[i];
    }

    const Ledger::Remembered* Ledger::findLive(const std::vector<Remembered>& list, const Id& peer, double now)
    {
        const Remembered* entry = find(list, peer);
        return (entry && now - entry->at < COOLDOWN_SECONDS) ? entry : nullptr;
    }

    void Ledger::remember(std::vector<Remembered>& list, const Id& peer, double now, bool no_answer)
    {
        pruneOlderThan(list, now, COOLDOWN_SECONDS);
        if (Remembered* entry = find(list, peer))
        {
            entry->at = now;
            entry->no_answer = no_answer;
            return;
        }
        makeRoom(list, MAX_REMEMBERED);
        list.push_back({ peer, now, no_answer });
    }

    void Ledger::markEnded(const Id& peer, const Id& nonce, double now)
    {
        pruneOlderThan(mEnded, now, ENDED_SECONDS);
        makeRoom(mEnded, MAX_ENDED);
        mEnded.push_back({ peer, nonce, now });
    }

    void Ledger::unmarkEnded(const Id& peer, const Id& nonce)
    {
        mEnded.erase(std::remove_if(mEnded.begin(), mEnded.end(),
                                    [&](const Ended& e) { return e.peer == peer && e.nonce == nonce; }),
                     mEnded.end());
    }

    bool Ledger::isEnded(const Id& peer, const Id& nonce, double now) const
    {
        for (const Ended& entry : mEnded)
        {
            if (entry.peer == peer && entry.nonce == nonce && now - entry.at < ENDED_SECONDS)
            {
                return true;
            }
        }
        return false;
    }

    bool Ledger::allowedToAsk(const Id& peer) const
    {
        switch (mEnv.policy())
        {
            case Policy::Everyone:
                break;
            case Policy::Friends:
                if (!mEnv.isFriend(peer))
                {
                    return false;
                }
                break;
            case Policy::Nobody:
            default:
                return false;
        }
        return !mEnv.isBlocked(peer);
    }

    // ---- the side that poses -------------------------------------------------------------------------------

    Status Ledger::status(const Id& peer, double now) const
    {
        if (find(mOutgoing, peer))
        {
            return Status::Pending;
        }
        if (find(mAccepted, peer))
        {
            return Status::Accepted;
        }
        if (const Remembered* refused = findLive(mRefused, peer, now))
        {
            return refused->no_answer ? Status::NoAnswer : Status::Refused;
        }
        return Status::Ask;
    }

    bool Ledger::canAsk(const Id& peer, double now) const
    {
        return !peer.isNull() && peer != mEnv.self() && mEnv.bridgeRunning() && status(peer, now) == Status::Ask
               && mOutgoing.size() + mAccepted.size() < MAX_SESSIONS && mEnv.nearby(peer);
    }

    bool Ledger::ask(const Id& peer, double now)
    {
        if (!canAsk(peer, now))
        {
            return false;
        }
        const Id nonce = mEnv.newNonce();
        if (nonce.isNull() || !mEnv.send(Action::Request, nonce, peer))
        {
            return false;
        }
        mOutgoing.push_back({ peer, nonce, now });
        ++mAskedGeneration;
        return true;
    }

    void Ledger::cancel(const Id& peer, double now)
    {
        std::size_t i = indexOf(mOutgoing, peer);
        if (i != NONE)
        {
            const Outgoing out = takeOutgoing(i, now);
            remember(mRefused, out.peer, now, true);   // a Charis on the receiving side cools down on a cancelled request
            mEnv.send(Action::Cancel, out.nonce, out.peer);
            return;
        }
        i = indexOf(mAccepted, peer);
        if (i != NONE)
        {
            endAccepted(i, true, nullptr, now);
        }
    }

    Ledger::Outgoing Ledger::takeOutgoing(std::size_t i, double now)
    {
        const Outgoing out = mOutgoing[i];
        mOutgoing.erase(mOutgoing.begin() + i);
        ++mAskedGeneration;
        markEnded(out.peer, out.nonce, now);
        return out;
    }

    bool Ledger::canPose(const Id& peer) const
    {
        const Accepted* accepted = find(mAccepted, peer);
        return accepted && accepted->usable && mEnv.nearby(peer);
    }

    void Ledger::asked(std::vector<Asked>& out) const
    {
        out.clear();
        for (const Outgoing& entry : mOutgoing)
        {
            out.push_back({ entry.peer, Status::Pending });
        }
        for (const Accepted& entry : mAccepted)
        {
            out.push_back({ entry.peer, Status::Accepted });
        }
    }

    void Ledger::endAccepted(std::size_t i, bool send_cancel, const Notice* notice, double now)
    {
        const Accepted ended = mAccepted[i];
        mAccepted.erase(mAccepted.begin() + i);
        ++mAskedGeneration;
        markEnded(ended.peer, ended.nonce, now);
        if (send_cancel)
        {
            mEnv.send(Action::Cancel, ended.nonce, ended.peer);
        }
        mEnv.consentLost(ended.peer);
        if (notice)
        {
            mEnv.notice(*notice, ended.peer);
        }
    }

    // ---- the side that is posed ----------------------------------------------------------------------------

    bool Ledger::isGranted(const Id& peer) const
    {
        return find(mGranted, peer) != nullptr;
    }

    void Ledger::dropIncoming(std::size_t i, DropIncoming how, double now)
    {
        const Incoming dropped = mIncoming[i];
        mIncoming.erase(mIncoming.begin() + i);
        markEnded(dropped.peer, dropped.nonce, now);
        if (how != DropIncoming::BridgeLost)
        {
            remember(mInboundCooldown, dropped.peer, now, false);
        }
        if (how != DropIncoming::Cancelled)
        {
            mEnv.send(Action::Refuse, dropped.nonce, dropped.peer);
        }
    }

    void Ledger::dropGranted(std::size_t i, double now)
    {
        const Granted dropped = mGranted[i];
        mGranted.erase(mGranted.begin() + i);
        ++mGrantedGeneration;
        markEnded(dropped.peer, dropped.nonce, now);
    }

    Answer Ledger::answer(const Id& nonce, const Id& peer, bool allow, double now)
    {
        const std::size_t i = indexOf(mIncoming, peer, nonce);
        if (i == NONE)
        {
            return Answer::Gone;
        }
        if (!allow)
        {
            dropIncoming(i, DropIncoming::Refused, now);
            return Answer::Refused;
        }
        const bool still_valid = now - mIncoming[i].received_at < TIMEOUT_SECONDS && mEnv.nearby(peer) && allowedToAsk(peer);
        if (!still_valid)
        {
            dropIncoming(i, DropIncoming::Refused, now);   // refused, as a refusal: the person is told it expired
            return Answer::Expired;
        }
        const bool sent = mEnv.send(Action::Accept, nonce, peer);
        const std::size_t j = indexOf(mIncoming, peer, nonce);     // the send may have called back into the ledger
        if (sent)
        {
            if (j != NONE)
            {
                mIncoming.erase(mIncoming.begin() + j);
            }
            if (!find(mGranted, peer))
            {
                mGranted.push_back({ peer, nonce });
                ++mGrantedGeneration;
            }
            return Answer::Allowed;
        }
        if (mEnv.bridgeThere())
        {
            // The bridge getting ready, or too many sends in flight: the request stays (its 60 s go on), marked busy, to
            // be answered again once the bridge runs. Not marked ended: the asker's cancel of it still counts.
            if (j == NONE)
            {
                return Answer::Gone;    // the send's handler took it away
            }
            mIncoming[j].busy = true;
            return Answer::Busy;
        }
        // No bridge to say yes with: as if it timed out on their side.
        if (j != NONE)
        {
            mIncoming.erase(mIncoming.begin() + j);
        }
        markEnded(peer, nonce, now);
        remember(mInboundCooldown, peer, now, false);
        return Answer::NotSent;
    }

    Withdrawal Ledger::withdraw(const Id& peer, double now)
    {
        std::size_t i = indexOf(mIncoming, peer);
        if (i != NONE)
        {
            dropIncoming(i, DropIncoming::Refused, now);
        }
        i = indexOf(mGranted, peer);
        if (i == NONE)
        {
            return Withdrawal::Withdrawn;
        }
        const Granted granted = mGranted[i];
        const bool sent = mEnv.send(Action::Withdraw, granted.nonce, peer);
        i = indexOf(mGranted, peer, granted.nonce);    // the send may have called back into the ledger: look again
        if (i == NONE)
        {
            return Withdrawal::Withdrawn;
        }
        if (sent)
        {
            dropGranted(i, now);
            return Withdrawal::Withdrawn;
        }
        // Kept and listed (the peer still has it), marked for idle() to send again; pressing again starts the waits anew.
        Granted& kept = mGranted[i];
        kept.wait = RESEND_FIRST_SECONDS;
        kept.next_try_at = now + RESEND_FIRST_SECONDS;
        if (kept.unsent)
        {
            return Withdrawal::StillNotSent;
        }
        kept.unsent = true;
        ++mGrantedGeneration;   // the reminder shows it as being withdrawn
        return Withdrawal::NotSent;
    }

    void Ledger::endWhereParentGone(const std::function<bool(const Id&)>& poserHolds, const std::function<bool(const Id&)>& posedHolds,
                                    double now)
    {
        std::vector<Id> cancels;
        for (const Outgoing& out : mOutgoing)
        {
            if (!poserHolds(out.peer))
            {
                cancels.push_back(out.peer);
            }
        }
        for (const Accepted& accepted : mAccepted)
        {
            if (!poserHolds(accepted.peer))
            {
                cancels.push_back(accepted.peer);
            }
        }
        std::vector<Id> withdraws;
        for (const Granted& granted : mGranted)
        {
            if (!granted.unsent && !posedHolds(granted.peer))
            {
                withdraws.push_back(granted.peer);
            }
        }
        for (const Id& peer : cancels)
        {
            cancel(peer, now);
        }
        for (const Id& peer : withdraws)
        {
            withdraw(peer, now);
        }
    }

    void Ledger::resendOneWithdraw(double now)
    {
        if (!mEnv.bridgeRunning())
        {
            return;     // sent again once the bridge is back
        }
        for (std::size_t i = 0; i < mGranted.size(); ++i)
        {
            if (!mGranted[i].unsent || now < mGranted[i].next_try_at)
            {
                continue;
            }
            const Granted granted = mGranted[i];
            const bool sent = mEnv.send(Action::Withdraw, granted.nonce, granted.peer);
            const std::size_t j = indexOf(mGranted, granted.peer, granted.nonce);  // may have changed in the call
            if (j == NONE)
            {
                return;
            }
            if (sent)
            {
                dropGranted(j, now);
            }
            else
            {
                // Not given up: tried again while the entry is there (it goes with the peer), at longer waits.
                Granted& waiting = mGranted[j];
                waiting.wait = std::min(waiting.wait * 2.0, RESEND_MOST_SECONDS);
                waiting.next_try_at = now + waiting.wait;
            }
            return;     // one a second at most
        }
    }

    // ---- both ----------------------------------------------------------------------------------------------

    void Ledger::receiveRequest(const Id& nonce, const Id& peer, double now)
    {
        // Each of these drops the request without an answer (Aurora: a request that does not qualify is ignored). No
        // bridge to answer with (not running, not getting ready) is one of them (review 11): its allow could never go.
        if (!mEnv.bridgeThere())
        {
            return;
        }
        if (find(mGranted, peer) || find(mIncoming, peer) || findLive(mInboundCooldown, peer, now))
        {
            return;
        }
        if (!allowedToAsk(peer) || !mEnv.nearby(peer))
        {
            return;
        }
        if (mIncoming.size() + mGranted.size() >= MAX_SESSIONS)
        {
            return;
        }
        mIncoming.push_back({ peer, nonce, now });
    }

    void Ledger::receive(const Id& nonce, Action action, const Id& peer, double now)
    {
        if (nonce.isNull() || peer.isNull() || peer == mEnv.self() || isEnded(peer, nonce, now))
        {
            return;
        }
        switch (action)
        {
            case Action::Request:
                receiveRequest(nonce, peer, now);
                return;

            case Action::Accept:
            {
                // Taken only in time and from someone still here (not muted, in our region); otherwise idle()
                // ends the request as not answered.
                const std::size_t i = indexOf(mOutgoing, peer, nonce);
                if (i != NONE && now - mOutgoing[i].sent_at < TIMEOUT_SECONDS && mEnv.present(peer))
                {
                    mOutgoing.erase(mOutgoing.begin() + i);
                    mAccepted.push_back({ peer, nonce, mEnv.nearby(peer) });
                    ++mAskedGeneration;
                    mEnv.notice(Notice::Accepted, peer);
                }
                return;
            }

            case Action::Refuse:
            {
                // As Aurora: a refusal after the time-out is ignored (idle() ends the request as not answered).
                const std::size_t i = indexOf(mOutgoing, peer, nonce);
                if (i != NONE && now - mOutgoing[i].sent_at < TIMEOUT_SECONDS)
                {
                    takeOutgoing(i, now);
                    remember(mRefused, peer, now, false);
                    mEnv.notice(Notice::Refused, peer);
                }
                return;
            }

            case Action::Cancel:    // the person who asked us stops: their request, or their permission
            {
                std::size_t i = indexOf(mIncoming, peer, nonce);
                if (i != NONE)
                {
                    const bool allowed_unsent = mIncoming[i].busy;
                    dropIncoming(i, DropIncoming::Cancelled, now);
                    if (allowed_unsent)
                    {
                        mEnv.notice(Notice::AllowNeverSent, peer);   // allowed, never sent: the asker took it back
                    }
                    return;
                }
                i = indexOf(mGranted, peer, nonce);
                if (i != NONE)
                {
                    dropGranted(i, now);
                }
                return;
            }

            case Action::Withdraw:  // the person we asked stops allowing us
            {
                static constexpr Notice WITHDRAWN = Notice::Withdrawn;
                std::size_t i = indexOf(mAccepted, peer, nonce);
                if (i != NONE)
                {
                    endAccepted(i, false, &WITHDRAWN, now);
                    return;
                }
                i = indexOf(mOutgoing, peer, nonce);
                if (i != NONE)
                {
                    takeOutgoing(i, now);
                    mEnv.notice(Notice::WithdrawnWhileAsking, peer);
                }
                return;
            }
        }
    }

    void Ledger::idle(double now)
    {
        if (!active())
        {
            return;
        }
        static constexpr Notice ENDED = Notice::Ended;

        // Each exit calls out (send, consentLost, notice) after its table is right; the scan then starts again,
        // as a handler may have changed the lists.
        for (std::size_t i = 0; i < mOutgoing.size();)
        {
            if (now - mOutgoing[i].sent_at >= TIMEOUT_SECONDS || !mEnv.nearby(mOutgoing[i].peer))
            {
                const Outgoing out = takeOutgoing(i, now);
                remember(mRefused, out.peer, now, true);
                mEnv.send(Action::Cancel, out.nonce, out.peer);
                mEnv.notice(Notice::NoAnswer, out.peer);
                i = 0;
                continue;
            }
            ++i;
        }

        for (std::size_t i = 0; i < mAccepted.size();)
        {
            Accepted& accepted = mAccepted[i];
            if (!mEnv.present(accepted.peer))
            {
                endAccepted(i, true, &ENDED, now);
                i = 0;
                continue;
            }
            const bool in_range = mEnv.nearby(accepted.peer);
            if (accepted.usable && !in_range)
            {
                accepted.usable = false;
                const Id peer = accepted.peer;  // the reference may not survive the calls
                mEnv.consentLost(peer);
                mEnv.notice(Notice::OutOfRange, peer);
                i = 0;
                continue;
            }
            accepted.usable = in_range;
            ++i;
        }

        for (std::size_t i = 0; i < mIncoming.size();)
        {
            const Incoming& in = mIncoming[i];
            if (now - in.received_at >= TIMEOUT_SECONDS || !mEnv.nearby(in.peer) || !allowedToAsk(in.peer))
            {
                const bool allowed_unsent = in.busy;
                const Id peer = in.peer;    // the reference does not survive the drop
                dropIncoming(i, DropIncoming::Refused, now);
                if (allowed_unsent)
                {
                    mEnv.notice(Notice::AllowNeverSent, peer);   // once: the entry is gone
                }
                i = 0;
                continue;
            }
            ++i;
        }

        for (std::size_t i = 0; i < mGranted.size();)
        {
            const Granted granted = mGranted[i];
            if (!mEnv.inRegion(granted.peer))
            {
                // Gone from the region: the permission ends with it there too. Withdrawn once (they may still hear it).
                dropGranted(i, now);
                mEnv.send(Action::Withdraw, granted.nonce, granted.peer);
                i = 0;
                continue;
            }
            if (!mEnv.present(granted.peer) && !granted.unsent)
            {
                // Muted but still here: on their side we are still present and they could go on posing us, so the
                // withdraw must get through (integration review 3, L2). Sent; one that does not go stays marked and
                // resendOneWithdraw() sends it again (muted or not).
                const bool sent = mEnv.send(Action::Withdraw, granted.nonce, granted.peer);
                const std::size_t j = indexOf(mGranted, granted.peer, granted.nonce);     // may have changed in the call
                if (j != NONE)
                {
                    if (sent)
                    {
                        dropGranted(j, now);
                    }
                    else
                    {
                        Granted& kept = mGranted[j];
                        kept.unsent = true;
                        kept.wait = RESEND_FIRST_SECONDS;
                        kept.next_try_at = now + RESEND_FIRST_SECONDS;
                        ++mGrantedGeneration;
                    }
                }
                i = 0;
                continue;
            }
            ++i;
        }

        resendOneWithdraw(now);
    }

    void Ledger::bridgeLost(double now)
    {
        static constexpr Notice ENDED = Notice::Ended;
        while (!mOutgoing.empty())
        {
            const Outgoing out = takeOutgoing(mOutgoing.size() - 1, now);
            remember(mRefused, out.peer, now, true);    // as cancel(): a Charis on the receiving side cools down on it
            mEnv.send(Action::Cancel, out.nonce, out.peer);     // tried (the bridge may already be gone)
            mEnv.notice(Notice::AskCancelledBridgeLost, out.peer);
        }
        while (!mAccepted.empty())
        {
            endAccepted(mAccepted.size() - 1, true, &ENDED, now);
        }
        while (!mIncoming.empty())
        {
            const bool allowed_unsent = mIncoming.back().busy;
            const Id peer = mIncoming.back().peer;
            dropIncoming(mIncoming.size() - 1, DropIncoming::BridgeLost, now);
            if (allowed_unsent)
            {
                mEnv.notice(Notice::AllowNeverSent, peer);
            }
        }
        // mGranted stays: a withdraw can be sent once the bridge is back (the ones marked unsent are sent again
        // by idle(), at once when it is back: their waits start anew), and the reminder keeps showing them.
        for (Granted& granted : mGranted)
        {
            if (granted.unsent)
            {
                granted.wait = RESEND_FIRST_SECONDS;
                granted.next_try_at = now;
            }
        }
    }

    void Ledger::sendFailed(Action action, const Id& nonce, const Id& peer, double now)
    {
        switch (action)
        {
            case Action::Withdraw:
            {
                if (find(mGranted, peer) || !mEnv.inRegion(peer))
                {
                    return;     // already listed again (pressed again, re-sent) or gone: nothing left to take back
                }                   // (muted but here counts as here: they could still pose us; integration review 3)
                // Back, marked: the session is live again (its ended mark goes, so the peer's cancel still counts).
                unmarkEnded(peer, nonce);
                Granted back{ peer, nonce };
                back.unsent = true;
                back.next_try_at = now + RESEND_FIRST_SECONDS;
                mGranted.push_back(back);
                ++mGrantedGeneration;
                return;
            }
            case Action::Accept:
            {
                // Kept: the post failed, yet the asker may have got it and may pose us. Listed, so it can be withdrawn.
                if (indexOf(mGranted, peer, nonce) != NONE)
                {
                    mEnv.notice(Notice::AllowUnsure, peer);
                }
                return;
            }
            case Action::Request:
            case Action::Cancel:
            case Action::Refuse:
                return;
        }
    }

    void Ledger::clearAll(ClearReason reason, double now)
    {
        std::vector<Id> lost;
        lost.reserve(mAccepted.size());
        for (const Accepted& accepted : mAccepted)
        {
            lost.push_back(accepted.peer);
        }
        // A teleport that has only started (it may fail, or stay in this region) leaves us where we are: everyone our
        // sessions concern is told, so that nothing is left on the other side that we could no longer take back
        // (reviews 4 and 5). Tried only (the bridge still runs at the start).
        // - The people we asked: a cancel, and COOLDOWN_SECONDS before asking them again, as cancel() (a Charis there
        //   cools down on it). The person is not told (review 6: the teleport is their own doing, and the list shows
        //   "no answer, 10 minutes").
        // - Those we may pose: a cancel. One that does not go is dropped all the same (their side keeps the permission
        //   until we are no longer present to them: another region, gone, muted; not when only out of range).
        // - Those we allowed: a withdraw. One that does not go stays, marked to be sent again (a teleport that gets
        //   through ends it at the region change that follows).
        // Copied first, sent after the tables are empty (a send may call back into the ledger).
        std::vector<Tell> tell;
        if (reason == ClearReason::Teleport)
        {
            tell.reserve(mOutgoing.size() + mAccepted.size() + mGranted.size());
            for (const Outgoing& out : mOutgoing)
            {
                tell.push_back({ Action::Cancel, out.peer, out.nonce });
                remember(mRefused, out.peer, now, true);
            }
            for (const Accepted& accepted : mAccepted)
            {
                tell.push_back({ Action::Cancel, accepted.peer, accepted.nonce });
            }
            for (const Granted& granted : mGranted)
            {
                tell.push_back({ Action::Withdraw, granted.peer, granted.nonce });
            }
        }
        if (!mGranted.empty())
        {
            ++mGrantedGeneration;
        }
        if (!mOutgoing.empty() || !mAccepted.empty())
        {
            ++mAskedGeneration;
        }
        // Requests we allowed while the bridge could not send them (busy) end unsent here: the person is told (review
        // 12), except at a logout (nothing is shown then).
        std::vector<Id> allowed_unsent;
        if (reason != ClearReason::Logout)
        {
            for (const Incoming& in : mIncoming)
            {
                if (in.busy)
                {
                    allowed_unsent.push_back(in.peer);
                }
            }
        }
        mOutgoing.clear();
        mAccepted.clear();
        mIncoming.clear();
        mGranted.clear();
        if (reason == ClearReason::Logout)
        {
            mRefused.clear();
            mInboundCooldown.clear();
            mEnded.clear();
        }
        for (const Tell& told : tell)
        {
            const bool sent = mEnv.send(told.action, told.nonce, told.peer);
            if (!sent && told.action == Action::Withdraw && !find(mGranted, told.peer))
            {
                // Kept, marked: sent again by idle() as a withdraw the person pressed (shown as waiting).
                Granted kept{ told.peer, told.nonce };
                kept.unsent = true;
                kept.next_try_at = now + RESEND_FIRST_SECONDS;
                mGranted.push_back(kept);
                ++mGrantedGeneration;
            }
        }
        for (const Id& peer : lost)
        {
            mEnv.consentLost(peer);
            mEnv.notice(Notice::Ended, peer);
        }
        for (const Id& peer : allowed_unsent)
        {
            mEnv.notice(Notice::AllowNeverSent, peer);
        }
    }
}
