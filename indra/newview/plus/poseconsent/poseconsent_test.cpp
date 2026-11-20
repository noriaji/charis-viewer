// Stand-alone test of poseconsentcore.h/.cpp (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX poseconsent_test.cpp poseconsentcore.cpp && poseconsent_test.exe
// The cases follow the tables of research/336 (its appendix on the states of poseconsent, the one whose Japanese heading
// ends in "(implementation)"): every exit of each state, the nonce
// checks, the cooldowns, the limits, the ended marks, the policy, the time-outs and which exits send cancel.
#include "poseconsentcore.h"

#include <cstdio>
#include <set>
#include <utility>
#include <vector>

using namespace plus::poseconsent::core;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static Id id(std::uint64_t n) { return Id{ 0x1000, n }; }

struct Sent
{
    Action action;
    Id nonce;
    Id peer;
};

struct Key
{
    Id id;
    bool operator<(const Key& o) const { return id.hi != o.id.hi ? id.hi < o.id.hi : id.lo < o.id.lo; }
};

// Where a peer is: gone (or muted), in our region but further than the range, or near.
enum class Where { Gone, Far, Near, Muted };     // Muted: in our region, there, but muted (not present)

class FakeEnv : public Env
{
public:
    Id me = id(1);
    bool bridge = true;         // Running
    bool starting = false;      // Starting (getting ready again): there, but sends fail
    bool sendWorks = true;
    Policy pol = Policy::Friends;
    std::set<Key> presentSet, nearbySet, mutedSet, friends, blocked;
    std::uint64_t nextNonce = 1000;
    std::vector<Sent> sent;
    std::vector<Id> lost;
    std::vector<std::pair<Notice, Id>> notices;

    void place(const Id& peer, Where where)
    {
        presentSet.erase({ peer });
        nearbySet.erase({ peer });
        mutedSet.erase({ peer });
        if (where == Where::Far || where == Where::Near) presentSet.insert({ peer });
        if (where == Where::Near) nearbySet.insert({ peer });
        if (where == Where::Muted) mutedSet.insert({ peer });
    }
    void clearLog() { sent.clear(); lost.clear(); notices.clear(); }
    bool sentAction(Action a, const Id& peer) const
    {
        for (const Sent& s : sent) if (s.action == a && s.peer == peer) return true;
        return false;
    }
    bool noticed(Notice n, const Id& peer) const
    {
        for (const auto& p : notices) if (p.first == n && p.second == peer) return true;
        return false;
    }
    Id lastNonce() const { return Id{ 0x2000, nextNonce - 1 }; }

    Id self() const override { return me; }
    bool bridgeRunning() const override { return bridge; }
    bool bridgeThere() const override { return bridge || starting; }
    bool present(const Id& peer) const override { return presentSet.count({ peer }) != 0; }
    bool inRegion(const Id& peer) const override { return present(peer) || mutedSet.count({ peer }) != 0; }
    bool nearby(const Id& peer) const override { return nearbySet.count({ peer }) != 0; }
    bool isFriend(const Id& peer) const override { return friends.count({ peer }) != 0; }
    bool isBlocked(const Id& peer) const override { return blocked.count({ peer }) != 0; }
    Policy policy() const override { return pol; }
    Id newNonce() override { return Id{ 0x2000, nextNonce++ }; }
    bool send(Action action, const Id& nonce, const Id& peer) override
    {
        sent.push_back({ action, nonce, peer });
        return bridge && sendWorks;
    }
    void consentLost(const Id& peer) override { lost.push_back(peer); }
    void notice(Notice n, const Id& peer) override { notices.push_back({ n, peer }); }
};

// How many times `notice` was given about `who` (for the "once" of a notice).
static int countNotices(const FakeEnv& env, Notice notice, const Id& who)
{
    int n = 0;
    for (const auto& p : env.notices) n += (p.first == notice && p.second == who) ? 1 : 0;
    return n;
}

// A peer near us, a friend.
static void setUp(FakeEnv& env, const Id& peer)
{
    env.place(peer, Where::Near);
    env.friends.insert({ peer });
}

// The asking side: Ask -> Pending -> Accepted, and every way out.
static void testAsking()
{
    const Id bob = id(2);
    {
        FakeEnv env; Ledger l(env); setUp(env, bob);
        CHECK(l.status(bob, 0) == Status::Ask);
        CHECK(l.canAsk(bob, 0));
        CHECK(!l.canAsk(env.me, 0));                  // not ourselves
        CHECK(!l.canAsk(Id{}, 0));                     // not null
        CHECK(l.ask(bob, 0));
        CHECK(env.sentAction(Action::Request, bob));
        const Id nonce = env.lastNonce();
        CHECK(l.status(bob, 1) == Status::Pending);
        CHECK(!l.canAsk(bob, 1));                     // one at a time
        CHECK(!l.ask(bob, 1));
        CHECK(l.active());

        // an accept with another nonce, or from someone else, does nothing
        l.receive(id(999), Action::Accept, bob, 2);
        l.receive(nonce, Action::Accept, id(3), 2);
        CHECK(l.status(bob, 2) == Status::Pending);

        l.receive(nonce, Action::Accept, bob, 3);
        CHECK(l.status(bob, 3) == Status::Accepted);
        CHECK(l.canPose(bob));
        CHECK(env.noticed(Notice::Accepted, bob));

        // the person ends it: cancel is sent, consent lost, back to Ask (no cooldown)
        env.clearLog();
        l.cancel(bob, 10);
        CHECK(env.sentAction(Action::Cancel, bob));
        CHECK(env.lost.size() == 1 && env.lost[0] == bob);
        CHECK(l.status(bob, 10) == Status::Ask);
        CHECK(!l.canPose(bob));
        CHECK(l.canAsk(bob, 10));
        // a late accept of the ended nonce does nothing
        l.receive(nonce, Action::Accept, bob, 11);
        CHECK(l.status(bob, 11) == Status::Ask);
    }
    {   // the bridge is not running: no asking
        FakeEnv env; Ledger l(env); setUp(env, bob);
        env.bridge = false;
        CHECK(!l.canAsk(bob, 0));
        CHECK(!l.ask(bob, 0));
        CHECK(!l.active());
    }
    {   // too far: no asking
        FakeEnv env; Ledger l(env); setUp(env, bob);
        env.place(bob, Where::Far);
        CHECK(!l.canAsk(bob, 0));
    }
    {   // the request could not be sent: nothing held
        FakeEnv env; Ledger l(env); setUp(env, bob);
        env.sendWorks = false;
        CHECK(!l.ask(bob, 0));
        CHECK(l.status(bob, 0) == Status::Ask);
    }
    {   // refused: 10 minute cooldown, then Ask again
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        const Id nonce = env.lastNonce();
        l.receive(id(77), Action::Refuse, bob, 1);   // wrong nonce
        CHECK(l.status(bob, 1) == Status::Pending);
        l.receive(nonce, Action::Refuse, bob, 2);
        CHECK(l.status(bob, 2) == Status::Refused);
        CHECK(env.noticed(Notice::Refused, bob));
        CHECK(!env.sentAction(Action::Cancel, bob));
        CHECK(!l.canAsk(bob, 2 + COOLDOWN_SECONDS - 1));
        CHECK(l.status(bob, 2 + COOLDOWN_SECONDS) == Status::Ask);
        CHECK(l.canAsk(bob, 2 + COOLDOWN_SECONDS));
    }
    {   // no answer in 60 s: cancel sent, NoAnswer for 10 minutes
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 100);
        const Id nonce = env.lastNonce();
        l.idle(100 + TIMEOUT_SECONDS - 0.5);
        CHECK(l.status(bob, 100 + TIMEOUT_SECONDS - 0.5) == Status::Pending);
        // an accept that comes after the 60 s, before idle noticed, is not taken
        l.receive(nonce, Action::Accept, bob, 100 + TIMEOUT_SECONDS);
        CHECK(l.status(bob, 100 + TIMEOUT_SECONDS) == Status::Pending);
        l.idle(100 + TIMEOUT_SECONDS);
        CHECK(l.status(bob, 100 + TIMEOUT_SECONDS) == Status::NoAnswer);
        CHECK(env.sentAction(Action::Cancel, bob));
        CHECK(env.noticed(Notice::NoAnswer, bob));
        CHECK(!l.canAsk(bob, 100 + TIMEOUT_SECONDS + 1));
        CHECK(l.status(bob, 100 + TIMEOUT_SECONDS + COOLDOWN_SECONDS) == Status::Ask);
        // and a late accept of it is dropped (ended mark)
        l.receive(nonce, Action::Accept, bob, 100 + TIMEOUT_SECONDS + COOLDOWN_SECONDS);
        CHECK(l.status(bob, 100 + TIMEOUT_SECONDS + COOLDOWN_SECONDS) == Status::Ask);
    }
    {   // Pending, the peer goes out of range / leaves: NoAnswer + cancel (research/336 4.1)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        env.place(bob, Where::Far);
        l.idle(1);
        CHECK(l.status(bob, 1) == Status::NoAnswer);
        CHECK(env.sentAction(Action::Cancel, bob));
    }
    {   // Pending, the person cancels: cancel sent, then 10 minutes before asking them again (review 10: the other
        // side cools down on a cancelled request, so asking again at once would be dropped unanswered)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        l.cancel(bob, 1);
        CHECK(env.sentAction(Action::Cancel, bob));
        CHECK(l.status(bob, 1) == Status::NoAnswer);
        CHECK(!l.canAsk(bob, 1));
        CHECK(l.canAsk(bob, 1 + COOLDOWN_SECONDS));
        CHECK(env.lost.empty());
    }
    {   // Pending, the peer withdraws (nonce must match)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        const Id nonce = env.lastNonce();
        l.receive(id(5), Action::Withdraw, bob, 1);
        CHECK(l.status(bob, 1) == Status::Pending);
        l.receive(nonce, Action::Withdraw, bob, 1);
        CHECK(l.status(bob, 1) == Status::Ask);
        CHECK(!env.sentAction(Action::Cancel, bob));
    }
    {   // Accepted: withdraw from the peer -> Ask, consent lost, no cancel
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        const Id nonce = env.lastNonce();
        l.receive(nonce, Action::Accept, bob, 1);
        env.clearLog();
        l.receive(id(6), Action::Withdraw, bob, 2);  // wrong nonce
        CHECK(l.status(bob, 2) == Status::Accepted);
        l.receive(nonce, Action::Withdraw, bob, 2);
        CHECK(l.status(bob, 2) == Status::Ask);
        CHECK(env.lost.size() == 1);
        CHECK(env.noticed(Notice::Withdrawn, bob));
        CHECK(env.sent.empty());
    }
    {   // Accepted: out of range keeps Accepted but stops posing (consent lost once), back in range poses again
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        l.receive(env.lastNonce(), Action::Accept, bob, 1);
        env.clearLog();
        env.place(bob, Where::Far);
        l.idle(2);
        CHECK(l.status(bob, 2) == Status::Accepted);
        CHECK(!l.canPose(bob));
        CHECK(env.lost.size() == 1);
        CHECK(env.noticed(Notice::OutOfRange, bob));
        l.idle(3);
        CHECK(env.lost.size() == 1);                  // not again while still out
        env.place(bob, Where::Near);
        l.idle(4);
        CHECK(l.canPose(bob));
        CHECK(env.lost.size() == 1);
        CHECK(env.sent.empty());
    }
    {   // Accepted: the peer leaves (or is muted): cancel sent, Ask, consent lost
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        l.receive(env.lastNonce(), Action::Accept, bob, 1);
        env.clearLog();
        env.place(bob, Where::Gone);
        l.idle(2);
        CHECK(l.status(bob, 2) == Status::Ask);
        CHECK(env.sentAction(Action::Cancel, bob));
        CHECK(env.lost.size() == 1);
        CHECK(env.noticed(Notice::Ended, bob));
    }
    {   // Accepted / Pending: the bridge goes -> Ask, cancel tried, consent lost
        FakeEnv env; Ledger l(env); setUp(env, bob);
        const Id carol = id(3); setUp(env, carol);
        l.ask(bob, 0);
        l.receive(env.lastNonce(), Action::Accept, bob, 1);
        l.ask(carol, 1);
        env.clearLog();
        env.bridge = false;
        l.bridgeLost(2);
        CHECK(l.status(bob, 2) == Status::Ask);
        CHECK(l.status(carol, 2) == Status::NoAnswer);    // as a cancel (review 10)
        CHECK(env.sentAction(Action::Cancel, bob));
        CHECK(env.sentAction(Action::Cancel, carol));
        CHECK(env.lost.size() == 1 && env.lost[0] == bob);
        CHECK(!l.active());
    }
    {   // region change: everything goes, nothing is sent, consent lost for the accepted (and so at a logout, below)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        const Id carol = id(3); setUp(env, carol);
        l.ask(bob, 0);
        l.receive(env.lastNonce(), Action::Accept, bob, 1);
        l.ask(carol, 1);
        env.clearLog();
        l.clearAll(ClearReason::RegionChanged, 2);
        CHECK(env.sent.empty());
        CHECK(env.lost.size() == 1 && env.lost[0] == bob);
        CHECK(!l.active());
        CHECK(l.status(bob, 2) == Status::Ask);
    }
    {   // a cooldown lasts across a region change, not across a logout
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        l.receive(env.lastNonce(), Action::Refuse, bob, 1);
        l.clearAll(ClearReason::RegionChanged, 2);
        CHECK(l.status(bob, 2) == Status::Refused);
        l.clearAll(ClearReason::Logout, 2);
        CHECK(l.status(bob, 2) == Status::Ask);
    }
    {   // at most MAX_SESSIONS asked or accepted at once
        FakeEnv env; Ledger l(env);
        for (std::uint64_t n = 0; n < MAX_SESSIONS; ++n)
        {
            setUp(env, id(100 + n));
            CHECK(l.ask(id(100 + n), 0));
        }
        setUp(env, id(500));
        CHECK(!l.canAsk(id(500), 0));
        l.cancel(id(100), 1);
        CHECK(l.canAsk(id(500), 1));
    }
}

// The side that is asked: none -> Incoming -> Granted, and every way out.
static void testAsked()
{
    const Id bob = id(2);
    const Id n1 = id(901);
    {
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        CHECK(l.incoming().size() == 1);
        CHECK(l.active());
        // the same person again (another nonce): dropped, one at a time
        l.receive(id(902), Action::Request, bob, 1);
        CHECK(l.incoming().size() == 1);
        // the answer must name the request
        CHECK(l.answer(id(902), bob, true, 2) == Answer::Gone);
        CHECK(l.answer(n1, id(3), true, 2) == Answer::Gone);
        CHECK(l.incoming().size() == 1);
        const unsigned gen = l.grantedGeneration();
        CHECK(l.answer(n1, bob, true, 2) == Answer::Allowed);
        CHECK(env.sentAction(Action::Accept, bob));
        CHECK(l.incoming().empty());
        CHECK(l.isGranted(bob));
        CHECK(l.grantedCount() == 1 && l.grantedPeer(0) == bob);
        CHECK(l.grantedGeneration() != gen);
        // asked again while allowed: dropped
        l.receive(id(903), Action::Request, bob, 3);
        CHECK(l.incoming().empty());
        // we withdraw: withdraw sent, gone
        env.clearLog();
        l.withdraw(bob, 4);
        CHECK(env.sentAction(Action::Withdraw, bob));
        CHECK(!l.isGranted(bob));
        // the same nonce again (a replay) is dropped
        l.receive(n1, Action::Request, bob, 5);
        CHECK(l.incoming().empty());
        // a new request after a withdraw is shown (no cooldown for a withdraw)
        l.receive(id(904), Action::Request, bob, 6);
        CHECK(l.incoming().size() == 1);
    }
    {   // the policy and the block list
        FakeEnv env; Ledger l(env);
        env.place(bob, Where::Near);                    // not a friend
        l.receive(n1, Action::Request, bob, 0);
        CHECK(l.incoming().empty());                   // Friends (the default): dropped silently
        CHECK(env.sent.empty());
        env.pol = Policy::Everyone;
        l.receive(id(905), Action::Request, bob, 0);
        CHECK(l.incoming().size() == 1);
        FakeEnv env2; Ledger l2(env2); setUp(env2, bob);
        env2.pol = Policy::Nobody;
        l2.receive(n1, Action::Request, bob, 0);
        CHECK(l2.incoming().empty());
        env2.pol = Policy::Everyone;
        env2.blocked.insert({ bob });
        l2.receive(id(906), Action::Request, bob, 0);
        CHECK(l2.incoming().empty());
        CHECK(env2.sent.empty());
    }
    {   // not near, ourselves, null ids: dropped
        FakeEnv env; Ledger l(env); setUp(env, bob);
        env.place(bob, Where::Far);
        l.receive(n1, Action::Request, bob, 0);
        CHECK(l.incoming().empty());
        env.place(bob, Where::Near);
        l.receive(Id{}, Action::Request, bob, 0);
        l.receive(n1, Action::Request, Id{}, 0);
        l.receive(n1, Action::Request, env.me, 0);
        CHECK(l.incoming().empty());
    }
    {   // refused: refuse sent, then 10 minutes of silence from them
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        CHECK(l.answer(n1, bob, false, 1) == Answer::Refused);
        CHECK(env.sentAction(Action::Refuse, bob));
        CHECK(l.incoming().empty());
        l.receive(id(907), Action::Request, bob, 1 + COOLDOWN_SECONDS - 1);
        CHECK(l.incoming().empty());
        l.receive(id(908), Action::Request, bob, 1 + COOLDOWN_SECONDS);
        CHECK(l.incoming().size() == 1);
    }
    {   // not answered in 60 s: refuse sent (as Aurora), cooldown; a late answer does nothing
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        l.idle(TIMEOUT_SECONDS - 1);
        CHECK(l.incoming().size() == 1);
        // an answer after 60 s, before idle noticed: refused, not allowed
        CHECK(l.answer(n1, bob, true, TIMEOUT_SECONDS) == Answer::Expired);
        CHECK(env.sentAction(Action::Refuse, bob));
        CHECK(!l.isGranted(bob));
        FakeEnv env2; Ledger l2(env2); setUp(env2, bob);
        l2.receive(n1, Action::Request, bob, 0);
        l2.idle(TIMEOUT_SECONDS);
        CHECK(l2.incoming().empty());
        CHECK(env2.sentAction(Action::Refuse, bob));
        CHECK(l2.answer(n1, bob, true, TIMEOUT_SECONDS + 1) == Answer::Gone);
        CHECK(!l2.isGranted(bob));
        l2.receive(id(909), Action::Request, bob, TIMEOUT_SECONDS + 2);
        CHECK(l2.incoming().empty());                  // cooldown
    }
    {   // the asker leaves before the answer: refuse, cooldown; the answer then does nothing
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        env.place(bob, Where::Gone);
        l.idle(1);
        CHECK(l.incoming().empty());
        CHECK(env.sentAction(Action::Refuse, bob));
        env.place(bob, Where::Near);
        l.receive(id(910), Action::Request, bob, 2);
        CHECK(l.incoming().empty());
    }
    {   // the answer is checked again: out of range at that moment -> refused
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        env.place(bob, Where::Far);
        CHECK(l.answer(n1, bob, true, 1) == Answer::Expired);
        CHECK(!l.isGranted(bob));
        CHECK(env.sentAction(Action::Refuse, bob));
    }
    {   // the policy made stricter while asked: refused at the next idle
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        env.pol = Policy::Nobody;
        l.idle(1);
        CHECK(l.incoming().empty());
        CHECK(env.sentAction(Action::Refuse, bob));
    }
    {   // the asker cancels (nonce must match): gone, nothing sent, cooldown counts
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        l.receive(id(911), Action::Cancel, bob, 1);
        CHECK(l.incoming().size() == 1);
        l.receive(n1, Action::Cancel, bob, 1);
        CHECK(l.incoming().empty());
        CHECK(env.sent.empty());
        l.receive(id(912), Action::Request, bob, 2);
        CHECK(l.incoming().empty());
    }
    {   // Granted: the asker cancels (they are done) / leaves
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        l.answer(n1, bob, true, 1);
        l.receive(id(913), Action::Cancel, bob, 2);
        CHECK(l.isGranted(bob));
        env.clearLog();
        l.receive(n1, Action::Cancel, bob, 2);
        CHECK(!l.isGranted(bob));
        CHECK(env.sent.empty());

        const Id carol = id(3); setUp(env, carol);
        l.receive(id(914), Action::Request, carol, 3);
        l.answer(id(914), carol, true, 3);
        env.place(carol, Where::Gone);
        l.idle(4);
        CHECK(!l.isGranted(carol));
        CHECK(env.sentAction(Action::Withdraw, carol));
    }
    {   // Granted, out of range but still in the region: kept
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        l.answer(n1, bob, true, 1);
        env.place(bob, Where::Far);
        l.idle(2);
        CHECK(l.isGranted(bob));
    }
    {   // the accept could not be sent, no bridge: not granted, cooldown
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        env.bridge = false;
        CHECK(l.answer(n1, bob, true, 1) == Answer::NotSent);
        CHECK(!l.isGranted(bob));
        CHECK(l.incoming().empty());
        env.bridge = true;
        l.receive(id(918), Action::Request, bob, 2);
        CHECK(l.incoming().empty());                   // cooldown
    }
    {   // the accept could not be sent, the bridge busy: the request stays, to be answered again in its 60 s
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        env.sendWorks = false;
        CHECK(l.answer(n1, bob, true, 1) == Answer::Busy);
        CHECK(!l.isGranted(bob));
        CHECK(l.incoming().size() == 1);
        env.sendWorks = true;
        CHECK(l.answer(n1, bob, true, 2) == Answer::Allowed);
        CHECK(l.isGranted(bob));
        // and a busy request still times out
        FakeEnv env2; Ledger l2(env2); setUp(env2, bob);
        l2.receive(n1, Action::Request, bob, 0);
        env2.sendWorks = false;
        CHECK(l2.answer(n1, bob, true, 1) == Answer::Busy);
        env2.sendWorks = true;
        l2.idle(TIMEOUT_SECONDS);
        CHECK(l2.incoming().empty());
        CHECK(l2.answer(n1, bob, true, TIMEOUT_SECONDS) == Answer::Gone);
    }
    {   // withdraw without a bridge: kept and told, sent later
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(n1, Action::Request, bob, 0);
        l.answer(n1, bob, true, 1);
        env.bridge = false;
        CHECK(l.withdraw(bob, 2) == Withdrawal::NotSent);       // not sent: the caller tells the person
        CHECK(l.isGranted(bob));
        CHECK(l.withdraw(bob, 2) == Withdrawal::StillNotSent);  // pressed again: not told twice
        l.idle(3);                                      // no bridge: not sent again yet
        CHECK(l.isGranted(bob));
        env.bridge = true;
        l.idle(4);                                      // the bridge is back: idle sends it
        CHECK(!l.isGranted(bob));
        CHECK(env.sentAction(Action::Withdraw, bob));
        CHECK(l.withdraw(bob, 5) == Withdrawal::Withdrawn);     // nothing left to withdraw
    }
    {   // the bridge goes: requests in are dropped (no cooldown), permissions kept
        FakeEnv env; Ledger l(env); setUp(env, bob);
        const Id carol = id(3); setUp(env, carol);
        l.receive(n1, Action::Request, bob, 0);
        l.answer(n1, bob, true, 0);
        l.receive(id(915), Action::Request, carol, 0);
        l.bridgeLost(1);
        CHECK(l.incoming().empty());
        CHECK(l.isGranted(bob));
        l.receive(id(916), Action::Request, carol, 2);
        CHECK(l.incoming().size() == 1);
    }
    {   // region change: requests and permissions go
        FakeEnv env; Ledger l(env); setUp(env, bob);
        const Id carol = id(3); setUp(env, carol);
        l.receive(n1, Action::Request, bob, 0);
        l.answer(n1, bob, true, 0);
        l.receive(id(917), Action::Request, carol, 0);
        env.clearLog();
        const unsigned gen = l.grantedGeneration();
        l.clearAll(ClearReason::RegionChanged, 2);
        CHECK(l.incoming().empty());
        CHECK(!l.isGranted(bob));
        CHECK(l.grantedGeneration() != gen);
        CHECK(env.sent.empty());
        CHECK(env.lost.empty());
        CHECK(!l.active());
    }
    {   // at most MAX_SESSIONS asked or allowed at once
        FakeEnv env; Ledger l(env);
        env.pol = Policy::Everyone;
        for (std::uint64_t n = 0; n < MAX_SESSIONS; ++n)
        {
            env.place(id(100 + n), Where::Near);
            l.receive(id(2000 + n), Action::Request, id(100 + n), 0);
        }
        CHECK(l.incoming().size() == MAX_SESSIONS);
        env.place(id(500), Where::Near);
        l.receive(id(3000), Action::Request, id(500), 0);
        CHECK(l.incoming().size() == MAX_SESSIONS);
        CHECK(l.incoming().front().peer == id(100));  // in order of arrival
    }
    {   // the cooldowns and ended marks hold a bounded number; the oldest goes
        FakeEnv env; Ledger l(env);
        env.pol = Policy::Everyone;
        for (std::uint64_t n = 0; n < MAX_REMEMBERED + 10; ++n)
        {
            env.place(id(100 + n), Where::Near);
            l.receive(id(4000 + n), Action::Request, id(100 + n), double(n));
            l.answer(id(4000 + n), id(100 + n), false, double(n));
        }
        // the first ones were forgotten (their cooldown went to make room), the last ones are still held
        l.receive(id(5000), Action::Request, id(100), 100);
        CHECK(l.incoming().size() == 1);
        l.receive(id(5001), Action::Request, id(100 + MAX_REMEMBERED + 9), 100);
        CHECK(l.incoming().size() == 1);
    }
}

// idle() with nothing held does nothing: nothing sent, told or lost.
static void testIdleCheap()
{
    FakeEnv env; Ledger l(env);
    CHECK(!l.active());
    l.idle(1);
    CHECK(env.sent.empty() && env.notices.empty() && env.lost.empty());
}

// A handler that calls back into the ledger (the poser ending its other poses when one is lost).
class ReentrantEnv : public FakeEnv
{
public:
    Ledger* ledger = nullptr;
    void consentLost(const Id& peer) override
    {
        FakeEnv::consentLost(peer);
        if (ledger)
        {
            ledger->cancel(id(3), 50);   // ends another session from inside the call
            (void)ledger->canPose(peer);
        }
    }
};

static void testReentrant()
{
    ReentrantEnv env; Ledger l(env); env.ledger = &l;
    const Id bob = id(2), carol = id(3);
    setUp(env, bob); setUp(env, carol);
    l.ask(bob, 0);
    l.receive(env.lastNonce(), Action::Accept, bob, 1);
    l.ask(carol, 1);
    l.receive(env.lastNonce(), Action::Accept, carol, 2);
    env.place(bob, Where::Gone);
    l.idle(3);
    CHECK(l.status(bob, 3) == Status::Ask);
    CHECK(l.status(carol, 3) == Status::Ask);
    CHECK(!l.active());
}

// A send() that calls back into the ledger (a bridge that reports at once).
class ReentrantSendEnv : public FakeEnv
{
public:
    Ledger* ledger = nullptr;
    bool inside = false;
    bool send(Action action, const Id& nonce, const Id& peer) override
    {
        const bool went = FakeEnv::send(action, nonce, peer);
        if (ledger && !inside)
        {
            inside = true;
            (void)ledger->canPose(peer);
            (void)ledger->status(peer, 50);
            ledger->cancel(id(3), 50);
            ledger->withdraw(id(4), 50);
            inside = false;
        }
        return went;
    }
};

static void testReentrantSend()
{
    ReentrantSendEnv env; Ledger l(env);
    const Id bob = id(2), carol = id(3), dave = id(4);
    setUp(env, bob); setUp(env, carol); setUp(env, dave);
    l.ask(carol, 0);                                   // a Pending the callbacks cancel
    l.receive(id(950), Action::Request, dave, 0);
    l.answer(id(950), dave, true, 0);                  // a permission the callbacks withdraw
    env.ledger = &l;
    l.ask(bob, 1);
    CHECK(l.status(bob, 1) == Status::Pending);
    CHECK(l.status(carol, 1) == Status::NoAnswer);
    CHECK(!l.isGranted(dave));
    // the time-out of bob calls send() from inside idle()
    l.idle(1 + TIMEOUT_SECONDS);
    CHECK(l.status(bob, 1 + TIMEOUT_SECONDS) == Status::NoAnswer);
    CHECK(!l.active());
}

// Review 2026-11-13 (poseconsent, separate opus): the cases that were missing.
static void testReviewCases()
{
    const Id bob = id(2), carol = id(3);
    {   // a refusal that comes after the 60 s is ignored; idle ends the request as not answered
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        const Id nonce = env.lastNonce();
        l.receive(nonce, Action::Refuse, bob, TIMEOUT_SECONDS);
        CHECK(l.status(bob, TIMEOUT_SECONDS) == Status::Pending);
        CHECK(!env.noticed(Notice::Refused, bob));
        l.idle(TIMEOUT_SECONDS);
        CHECK(l.status(bob, TIMEOUT_SECONDS) == Status::NoAnswer);
    }
    {   // an accept from someone further than the range: Accepted, but not posed until they come near
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        env.place(bob, Where::Far);
        l.receive(env.lastNonce(), Action::Accept, bob, 1);
        CHECK(l.status(bob, 1) == Status::Accepted);
        CHECK(!l.canPose(bob));
        l.idle(2);
        CHECK(env.lost.empty());                       // never usable: nothing to stop
        env.place(bob, Where::Near);
        l.idle(3);
        CHECK(l.canPose(bob));
    }
    {   // an accept from someone gone or muted is not taken (review 3)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        env.place(bob, Where::Gone);
        l.receive(env.lastNonce(), Action::Accept, bob, 1);
        CHECK(l.status(bob, 1) == Status::Pending);
        CHECK(!env.noticed(Notice::Accepted, bob));
    }
    {   // canPose follows the range at once; consentLost comes once, at the next idle (review 4)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        l.receive(env.lastNonce(), Action::Accept, bob, 1);
        CHECK(l.canPose(bob));
        env.place(bob, Where::Far);
        CHECK(!l.canPose(bob));
        CHECK(env.lost.empty());
        l.idle(2);
        CHECK(env.lost.size() == 1);
        l.idle(3);
        CHECK(env.lost.size() == 1);
        // then the bridge goes: lost once more (the poser's stop is safe twice), and Ask
        env.bridge = false;
        l.bridgeLost(4);
        CHECK(env.lost.size() == 2);
        CHECK(l.status(bob, 4) == Status::Ask);
    }
    {   // withdraw() on a request in: refused, with the cooldown
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(960), Action::Request, bob, 0);
        CHECK(l.withdraw(bob, 1) == Withdrawal::Withdrawn);
        CHECK(l.incoming().empty());
        CHECK(env.sentAction(Action::Refuse, bob));
        l.receive(id(961), Action::Request, bob, 2);
        CHECK(l.incoming().empty());
    }
    {   // a withdraw while we wait for the answer has its own notice (nothing was allowed yet)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        l.receive(env.lastNonce(), Action::Withdraw, bob, 1);
        CHECK(env.noticed(Notice::WithdrawnWhileAsking, bob));
        CHECK(!env.noticed(Notice::Withdrawn, bob));
        CHECK(env.lost.empty());
    }
    {   // put on the block list while asked: refused at the next idle
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(962), Action::Request, bob, 0);
        env.blocked.insert({ bob });
        l.idle(1);
        CHECK(l.incoming().empty());
        CHECK(env.sentAction(Action::Refuse, bob));
    }
    {   // no nonce to be had: nothing asked
        class NullNonceEnv : public FakeEnv
        {
        public:
            Id newNonce() override { return Id{}; }
        };
        NullNonceEnv env; Ledger l(env); setUp(env, bob);
        CHECK(!l.ask(bob, 0));
        CHECK(env.sent.empty());
        CHECK(!l.active());
    }
    {   // the ended marks keep MAX_ENDED: the oldest is forgotten (its replay is taken again), the newer are not
        FakeEnv env; Ledger l(env);
        env.pol = Policy::Everyone;
        const std::uint64_t sessions = MAX_ENDED + 1;
        for (std::uint64_t n = 0; n < sessions; ++n)
        {
            env.place(id(100 + n), Where::Near);
            l.receive(id(6000 + n), Action::Request, id(100 + n), 1);
            l.answer(id(6000 + n), id(100 + n), true, 1);
            l.withdraw(id(100 + n), 1);                // ends it (no cooldown): only the ended mark holds a replay
        }
        l.receive(id(6000 + 1), Action::Request, id(100 + 1), 2);
        CHECK(l.incoming().empty());                   // still marked
        l.receive(id(6000), Action::Request, id(100), 2);
        CHECK(l.incoming().size() == 1);               // the first mark went to make room
    }
    {   // both ask each other at once: two independent sessions
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        const Id ours = env.lastNonce();
        l.receive(id(970), Action::Request, bob, 0);
        CHECK(l.incoming().size() == 1);
        CHECK(l.answer(id(970), bob, true, 1) == Answer::Allowed);
        l.receive(ours, Action::Accept, bob, 1);
        CHECK(l.isGranted(bob));
        CHECK(l.canPose(bob));
        l.cancel(bob, 2);                              // ending ours leaves theirs
        CHECK(l.isGranted(bob));
        CHECK(l.status(bob, 2) == Status::Ask);
    }
    {   // the people asked: Pending, then Accepted, with a generation that moves on every change
        FakeEnv env; Ledger l(env); setUp(env, bob); setUp(env, carol);
        std::vector<Asked> list;
        l.asked(list);
        CHECK(list.empty());
        unsigned gen = l.askedGeneration();
        l.ask(bob, 0);
        const Id bobNonce = env.lastNonce();
        l.ask(carol, 0);
        CHECK(l.askedGeneration() != gen);
        l.asked(list);
        CHECK(list.size() == 2 && list[0].peer == bob && list[0].status == Status::Pending);
        gen = l.askedGeneration();
        l.receive(bobNonce, Action::Accept, bob, 1);
        CHECK(l.askedGeneration() != gen);
        l.asked(list);
        CHECK(list.size() == 2 && list[0].peer == carol && list[0].status == Status::Pending
              && list[1].peer == bob && list[1].status == Status::Accepted);
        gen = l.askedGeneration();
        env.place(bob, Where::Far);                    // out of range: still listed, no change
        l.idle(2);
        CHECK(l.askedGeneration() == gen);
        l.clearAll(ClearReason::Teleport, 2);
        CHECK(l.askedGeneration() != gen);
        l.asked(list);
        CHECK(list.empty());
    }
}

// Review 2 (2026-11-13): a withdraw the bridge (running) could not send is sent again by idle, one a second, until it
// goes (no limit since review 3: waits of 1, 2, 4, 8, then 10 s); a request we allowed but could not answer; the
// bridge lost while asking.
static void testReview2Cases()
{
    const Id bob = id(2), carol = id(3);
    {   // busy bridge: marked, sent again one a second, both of them in turn
        FakeEnv env; Ledger l(env); setUp(env, bob); setUp(env, carol);
        l.receive(id(980), Action::Request, bob, 0);
        l.answer(id(980), bob, true, 0);
        l.receive(id(981), Action::Request, carol, 0);
        l.answer(id(981), carol, true, 0);
        env.sendWorks = false;
        CHECK(l.withdraw(bob, 1) == Withdrawal::NotSent);
        CHECK(l.withdraw(carol, 1) == Withdrawal::NotSent);
        env.sendWorks = true;
        env.clearLog();
        l.idle(2);
        CHECK(env.sent.size() == 1);                   // one a second
        CHECK(!l.isGranted(bob) && l.isGranted(carol));
        l.idle(3);
        CHECK(!l.isGranted(carol));
    }
    {   // review 9: one a second whatever the outcome, with three marked (a loop that went on after one try would send
        // more: the next ones after a send that went, all of them after tries that fail)
        const Id dave = id(4);
        FakeEnv env; Ledger l(env); setUp(env, bob); setUp(env, carol); setUp(env, dave);
        for (const Id& who : { bob, carol, dave })
        {
            const Id nonce{ 0x3000, who.lo };
            l.receive(nonce, Action::Request, who, 0);
            l.answer(nonce, who, true, 0);
        }
        env.sendWorks = false;
        l.withdraw(bob, 1);
        l.withdraw(carol, 1);
        l.withdraw(dave, 1);
        env.clearLog();
        l.idle(2);                                     // all three due: tries that fail
        CHECK(env.sent.size() == 1);
        env.sendWorks = true;
        env.clearLog();
        l.idle(3);                                     // the two still due at their first wait: one goes
        CHECK(env.sent.size() == 1);
        CHECK(l.grantedCount() == 2);
    }
    {   // review 3: still busy for more than 30 s (a bridge waiting for its hello or URL): never given up, the waits
        // grow 1, 2, 4, 8, then 10 s; sent once the bridge can
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(982), Action::Request, bob, 0);
        l.answer(id(982), bob, true, 0);
        env.sendWorks = false;
        CHECK(l.withdraw(bob, 1) == Withdrawal::NotSent);
        CHECK(l.grantedWithdrawPending(0));
        env.clearLog();
        std::vector<double> tries;
        for (int s = 2; s <= 65; ++s)
        {
            const std::size_t before = env.sent.size();
            l.idle(double(s));
            if (env.sent.size() != before)
            {
                tries.push_back(double(s));
            }
        }
        const std::vector<double> expected = { 2, 4, 8, 16, 26, 36, 46, 56 };
        CHECK(tries == expected);
        CHECK(l.isGranted(bob));                       // still listed, still marked
        CHECK(l.grantedWithdrawPending(0));
        env.sendWorks = true;                          // the bridge can send now
        l.idle(66);
        CHECK(!l.isGranted(bob));
        CHECK(!l.active());
    }
    {   // pressing Withdraw again starts the waits anew (and is not told twice)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(986), Action::Request, bob, 0);
        l.answer(id(986), bob, true, 0);
        env.sendWorks = false;
        l.withdraw(bob, 1);
        for (int s = 2; s <= 20; ++s)
        {
            l.idle(double(s));                         // the wait has grown to 10 s
        }
        CHECK(l.withdraw(bob, 21) == Withdrawal::StillNotSent);
        env.sendWorks = true;
        l.idle(22);                                    // 1 s after the press, not 10
        CHECK(!l.isGranted(bob));
    }
    {   // review 9: the press starts the waits themselves anew, not only the next try: still failing after the press,
        // the tries come at 22 and 24 (1 s, then 2 s), not at 22 and 32 (the grown 10 s kept)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(988), Action::Request, bob, 0);
        l.answer(id(988), bob, true, 0);
        env.sendWorks = false;
        l.withdraw(bob, 1);
        for (int s = 2; s <= 20; ++s)
        {
            l.idle(double(s));                         // the wait has grown to 10 s
        }
        l.withdraw(bob, 21);
        env.clearLog();
        std::vector<double> tries;
        for (int s = 22; s <= 31; ++s)
        {
            const std::size_t before = env.sent.size();
            l.idle(double(s));
            if (env.sent.size() != before)
            {
                tries.push_back(double(s));
            }
        }
        const std::vector<double> expected = { 22, 24, 28 };
        CHECK(tries == expected);
    }
    {   // the mark shows in the list's generation (the reminder redraws with "being withdrawn")
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(987), Action::Request, bob, 0);
        l.answer(id(987), bob, true, 0);
        const unsigned gen = l.grantedGeneration();
        env.sendWorks = false;
        l.withdraw(bob, 1);
        CHECK(l.grantedGeneration() != gen);
    }
    {   // marked, then the peer leaves: dropped (withdraw tried once), no more tries
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(983), Action::Request, bob, 0);
        l.answer(id(983), bob, true, 0);
        env.sendWorks = false;
        l.withdraw(bob, 1);
        env.place(bob, Where::Gone);
        env.clearLog();
        l.idle(2);
        CHECK(!l.isGranted(bob));
        CHECK(env.sent.size() == 1);
        l.idle(3);
        CHECK(env.sent.size() == 1);
        CHECK(!l.active());
    }
    {   // the bridge goes while we wait for an answer: NoAnswer, and the person is told we took the request back
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        env.bridge = false;
        l.bridgeLost(1);
        CHECK(l.status(bob, 1) == Status::NoAnswer);
        CHECK(env.noticed(Notice::AskCancelledBridgeLost, bob));
        CHECK(!env.noticed(Notice::NoAnswer, bob));
    }
    {   // allowed after the request stopped holding: Expired (refuse sent), not Refused
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(984), Action::Request, bob, 0);
        env.pol = Policy::Nobody;
        CHECK(l.answer(id(984), bob, true, 1) == Answer::Expired);
        CHECK(env.sentAction(Action::Refuse, bob));
        FakeEnv env2; Ledger l2(env2); setUp(env2, bob);
        l2.receive(id(985), Action::Request, bob, 0);
        env2.pol = Policy::Nobody;
        CHECK(l2.answer(id(985), bob, false, 1) == Answer::Refused);   // a no stays a no
    }
}

// Review 4 (2026-11-13): the start of a teleport tells the other side; the marks of withdraws not sent.
static void testReview4Cases()
{
    const Id bob = id(2), carol = id(3), dave = id(4);
    {   // the start of a teleport: a withdraw to whom we allowed, a cancel to whom we may pose, then all goes
        FakeEnv env; Ledger l(env); setUp(env, bob); setUp(env, carol); setUp(env, dave);
        l.receive(id(990), Action::Request, bob, 0);
        l.answer(id(990), bob, true, 0);               // we allowed bob
        l.ask(carol, 0);
        l.receive(env.lastNonce(), Action::Accept, carol, 1);   // carol allowed us
        l.ask(dave, 1);                                // dave still asked: cancelled too (review 5), and 10 minutes
        env.clearLog();
        l.clearAll(ClearReason::Teleport, 2);
        CHECK(env.sentAction(Action::Withdraw, bob));
        CHECK(env.sentAction(Action::Cancel, carol));
        CHECK(env.sentAction(Action::Cancel, dave));
        CHECK(env.sent.size() == 3);
        CHECK(env.lost.size() == 1 && env.lost[0] == carol);
        CHECK(env.noticed(Notice::Ended, carol));
        CHECK(!env.noticed(Notice::NoAnswer, dave));   // review 7: the person is not told of the request taken back
        CHECK(env.notices.size() == 1);                // carol's Ended only
        CHECK(l.status(dave, 2) == Status::NoAnswer);
        CHECK(!l.canAsk(dave, 2));
        CHECK(l.canAsk(dave, 2 + COOLDOWN_SECONDS));
        CHECK(!l.active());
    }
    {   // the same without a bridge: tried; the cancel not sent is dropped, the withdraw not sent stays marked (review 5)
        FakeEnv env; Ledger l(env); setUp(env, bob); setUp(env, carol);
        l.receive(id(991), Action::Request, bob, 0);
        l.answer(id(991), bob, true, 0);
        l.ask(carol, 0);
        l.receive(env.lastNonce(), Action::Accept, carol, 1);
        env.bridge = false;
        env.clearLog();
        const unsigned gen = l.grantedGeneration();
        l.clearAll(ClearReason::Teleport, 2);
        CHECK(env.sent.size() == 2);
        CHECK(l.status(carol, 2) == Status::Ask);
        CHECK(l.isGranted(bob));
        CHECK(l.grantedCount() == 1 && l.grantedWithdrawPending(0));
        CHECK(l.grantedGeneration() != gen);
        // the teleport failed or stayed here, the bridge is back: the withdraw goes
        env.bridge = true;
        l.idle(3);
        CHECK(!l.isGranted(bob));
        CHECK(!l.active());
        // the teleport got through: the region change that follows ends the marked one, nothing sent
        FakeEnv env2; Ledger l2(env2); setUp(env2, bob);
        l2.receive(id(999), Action::Request, bob, 0);
        l2.answer(id(999), bob, true, 0);
        env2.bridge = false;
        l2.clearAll(ClearReason::Teleport, 1);
        CHECK(l2.isGranted(bob));
        env2.clearLog();
        l2.clearAll(ClearReason::RegionChanged, 2);
        CHECK(env2.sent.empty());
        CHECK(!l2.active());
    }
    {   // the start of a teleport sends no refuse to a request in (the asker's side times it out), and drops it
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(998), Action::Request, bob, 0);
        env.clearLog();
        l.clearAll(ClearReason::Teleport, 1);
        CHECK(env.sent.empty());
        CHECK(l.incoming().empty());
    }
    {   // a logout with permissions both ways: nothing is sent
        FakeEnv env; Ledger l(env); setUp(env, bob); setUp(env, carol);
        l.receive(id(997), Action::Request, bob, 0);
        l.answer(id(997), bob, true, 0);
        l.ask(carol, 0);
        l.receive(env.lastNonce(), Action::Accept, carol, 1);
        env.clearLog();
        l.clearAll(ClearReason::Logout, 2);
        CHECK(env.sent.empty());
        CHECK(env.noticed(Notice::Ended, carol));
        CHECK(!l.active());
    }
    {   // a send that calls back into the ledger during the teleport's clear
        ReentrantSendEnv env; Ledger l(env); setUp(env, bob); setUp(env, carol); setUp(env, dave);
        l.receive(id(992), Action::Request, bob, 0);
        l.answer(id(992), bob, true, 0);
        l.receive(id(993), Action::Request, dave, 0);
        l.answer(id(993), dave, true, 0);
        env.ledger = &l;
        l.clearAll(ClearReason::Teleport, 2);
        CHECK(!l.isGranted(bob) && !l.isGranted(dave));
        CHECK(!l.active());
    }
    {   // a failed resend does not move the list's generation (the reminder is not rebuilt every try)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(994), Action::Request, bob, 0);
        l.answer(id(994), bob, true, 0);
        env.sendWorks = false;
        l.withdraw(bob, 1);
        const unsigned gen = l.grantedGeneration();
        env.clearLog();
        l.idle(2);
        l.idle(4);
        CHECK(env.sent.size() == 2);
        CHECK(l.grantedGeneration() == gen);
    }
    {   // a marked permission the other side cancels: gone, no more tries
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(995), Action::Request, bob, 0);
        l.answer(id(995), bob, true, 0);
        env.sendWorks = false;
        l.withdraw(bob, 1);
        l.receive(id(995), Action::Cancel, bob, 1);
        CHECK(!l.isGranted(bob));
        env.clearLog();
        l.idle(2);
        CHECK(env.sent.empty());
        CHECK(!l.active());
    }
    {   // a marked permission at a region change: gone, nothing sent, no more tries
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(996), Action::Request, bob, 0);
        l.answer(id(996), bob, true, 0);
        env.sendWorks = false;
        l.withdraw(bob, 1);
        env.clearLog();
        l.clearAll(ClearReason::RegionChanged, 2);
        l.idle(2);
        CHECK(env.sent.empty());
        CHECK(!l.isGranted(bob));
    }
    {   // the wait grown to 10 s, then the bridge goes and comes back: tried at once (the waits start anew)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(997), Action::Request, bob, 0);
        l.answer(id(997), bob, true, 0);
        env.sendWorks = false;
        l.withdraw(bob, 1);
        for (int s = 2; s <= 17; ++s)
        {
            l.idle(double(s));                         // tries at 2, 4, 8, 16: the next wait is 10 s (at 26)
        }
        env.bridge = false;
        l.bridgeLost(18);
        CHECK(l.isGranted(bob));
        env.bridge = true;
        env.sendWorks = true;
        l.idle(19);
        CHECK(!l.isGranted(bob));
    }
}

// Review 6 (2026-11-13): the start of a teleport with a running bridge that cannot send, and two starts in a row.
static void testReview6Cases()
{
    const Id bob = id(2), carol = id(3), dave = id(4), erin = id(5);
    {   // running but unable to send: the withdraw stays marked, the cancels (Accepted and Pending) are dropped
        FakeEnv env; Ledger l(env); setUp(env, bob); setUp(env, carol); setUp(env, dave);
        l.receive(id(1010), Action::Request, bob, 0);
        l.answer(id(1010), bob, true, 0);
        l.ask(carol, 0);
        l.receive(env.lastNonce(), Action::Accept, carol, 1);
        l.ask(dave, 1);
        env.sendWorks = false;                         // bridgeRunning() stays true
        env.clearLog();
        l.clearAll(ClearReason::Teleport, 2);
        CHECK(env.sent.size() == 3);
        CHECK(l.isGranted(bob) && l.grantedWithdrawPending(0));
        CHECK(l.status(carol, 2) == Status::Ask);
        CHECK(l.status(dave, 2) == Status::NoAnswer);
        CHECK(env.lost.size() == 1 && env.lost[0] == carol);
        env.sendWorks = true;
        l.idle(3);
        CHECK(!l.isGranted(bob));
    }
    {   // two teleport starts in a row: the second tries only the marked one; what went the first time is not sent again
        FakeEnv env; Ledger l(env); setUp(env, bob); setUp(env, erin);
        l.receive(id(1011), Action::Request, bob, 0);
        l.answer(id(1011), bob, true, 0);
        l.receive(id(1012), Action::Request, erin, 0);
        l.answer(id(1012), erin, true, 0);
        env.clearLog();
        env.sendWorks = false;
        l.clearAll(ClearReason::Teleport, 1);          // neither goes: both marked
        CHECK(l.grantedCount() == 2);
        env.sendWorks = true;
        l.withdraw(erin, 1);                           // erin's goes now
        CHECK(l.grantedCount() == 1 && l.grantedPeer(0) == bob);
        env.clearLog();
        l.clearAll(ClearReason::Teleport, 2);          // the second start: bob's at once, erin not again
        CHECK(env.sent.size() == 1 && env.sentAction(Action::Withdraw, bob));
        CHECK(!env.sentAction(Action::Withdraw, erin));
        CHECK(!l.active());
    }
}

// Review 10 (2026-11-13): a bridge getting ready again (Starting) is "busy", not "no bridge".
static void testReview10Cases()
{
    const Id bob = id(2);
    {   // allowed while the bridge is Starting: the request stays, to be answered again; no cooldown on the asker
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1020), Action::Request, bob, 0);
        env.bridge = false;
        env.starting = true;
        CHECK(l.answer(id(1020), bob, true, 1) == Answer::Busy);
        CHECK(l.incoming().size() == 1);
        CHECK(!l.isGranted(bob));
        env.bridge = true;
        env.starting = false;
        CHECK(l.answer(id(1020), bob, true, 3) == Answer::Allowed);
        CHECK(l.isGranted(bob));
    }
    {   // the same with no bridge at all: NotSent, the request gone (as before)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1021), Action::Request, bob, 0);
        env.bridge = false;
        CHECK(l.answer(id(1021), bob, true, 1) == Answer::NotSent);
        CHECK(l.incoming().empty());
    }
    {   // withdrawn while Starting: marked, not tried while it gets ready, sent once it runs
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1022), Action::Request, bob, 0);
        l.answer(id(1022), bob, true, 0);
        env.bridge = false;
        env.starting = true;
        CHECK(l.withdraw(bob, 1) == Withdrawal::NotSent);
        CHECK(l.grantedWithdrawPending(0));
        env.clearLog();
        l.idle(2);
        CHECK(env.sent.empty());
        env.bridge = true;
        env.starting = false;
        l.idle(3);
        CHECK(!l.isGranted(bob));
    }
}

// A send of an accept whose handler takes the request away (the asker's cancel arriving inside it), the bridge Starting.
class CancelInsideSendEnv : public FakeEnv
{
public:
    Ledger* ledger = nullptr;
    bool send(Action action, const Id& nonce, const Id& peer) override
    {
        FakeEnv::send(action, nonce, peer);
        if (ledger && action == Action::Accept)
        {
            ledger->receive(nonce, Action::Cancel, peer, 1);
        }
        return false;
    }
};

// Review 11 (2026-11-13): a bridge getting ready for long; requests while there is no bridge.
static void testReview11Cases()
{
    const Id bob = id(2), carol = id(3);
    {   // Starting: no asking
        FakeEnv env; Ledger l(env); setUp(env, bob);
        env.bridge = false;
        env.starting = true;
        CHECK(!l.canAsk(bob, 0));
        CHECK(!l.ask(bob, 0));
        CHECK(env.sent.empty());
    }
    {   // a busy request is marked; the asker's cancel still takes it away (it is not marked ended), the answer is Gone
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1030), Action::Request, bob, 0);
        env.bridge = false;
        env.starting = true;
        CHECK(l.answer(id(1030), bob, true, 1) == Answer::Busy);
        CHECK(l.incoming().size() == 1 && l.incoming().front().busy);
        l.receive(id(1030), Action::Cancel, bob, 2);
        CHECK(l.incoming().empty());
        CHECK(l.answer(id(1030), bob, true, 3) == Answer::Gone);
        CHECK(countNotices(env, Notice::AllowNeverSent, bob) == 1);    // review 12: allowed, never sent, taken back: told
        // and only once (review 13): nothing more at the next looks or the clears that may follow
        l.idle(4);
        l.clearAll(ClearReason::Teleport, 5);
        l.clearAll(ClearReason::RegionChanged, 6);
        CHECK(countNotices(env, Notice::AllowNeverSent, bob) == 1);
    }
    {   // the send's handler took the request away: Gone, not Busy
        CancelInsideSendEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1031), Action::Request, bob, 0);
        env.bridge = false;
        env.starting = true;
        env.ledger = &l;
        CHECK(l.answer(id(1031), bob, true, 1) == Answer::Gone);
        CHECK(l.incoming().empty());
    }
    {   // busy, and the bridge still not running when the 60 s are up: dropped, the person told once
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1032), Action::Request, bob, 0);
        env.bridge = false;
        env.starting = true;
        l.answer(id(1032), bob, true, 1);
        l.idle(TIMEOUT_SECONDS - 1);
        CHECK(l.incoming().size() == 1);
        l.idle(TIMEOUT_SECONDS);
        CHECK(l.incoming().empty());
        int told = 0;
        for (const auto& n : env.notices) told += (n.first == Notice::AllowNeverSent && n.second == bob) ? 1 : 0;
        CHECK(told == 1);
        l.idle(TIMEOUT_SECONDS + 1);
        CHECK(env.notices.size() == 1);
    }
    {   // busy, then the bridge goes: dropped, the person told once; a request never allowed is not told
        FakeEnv env; Ledger l(env); setUp(env, bob); setUp(env, carol);
        l.receive(id(1033), Action::Request, bob, 0);
        l.receive(id(1034), Action::Request, carol, 0);
        env.bridge = false;
        env.starting = true;
        l.answer(id(1033), bob, true, 1);
        env.starting = false;
        l.bridgeLost(2);
        CHECK(l.incoming().empty());
        CHECK(countNotices(env, Notice::AllowNeverSent, bob) == 1);
        CHECK(countNotices(env, Notice::AllowNeverSent, carol) == 0);
        // and only once (review 13)
        l.idle(3);
        l.bridgeLost(4);
        l.clearAll(ClearReason::Teleport, 5);
        l.clearAll(ClearReason::RegionChanged, 6);
        CHECK(countNotices(env, Notice::AllowNeverSent, bob) == 1);
    }
    {   // no bridge (not running, not getting ready): a request is dropped unanswered; Starting takes it
        FakeEnv env; Ledger l(env); setUp(env, bob);
        env.bridge = false;
        l.receive(id(1035), Action::Request, bob, 0);
        CHECK(l.incoming().empty());
        CHECK(env.sent.empty());
        env.starting = true;
        l.receive(id(1036), Action::Request, bob, 1);
        CHECK(l.incoming().size() == 1);
    }
}

// Review 12 (2026-11-13): every way a busy request ends unsent tells once; a request never allowed is never told.
static void testReview12Cases()
{
    const Id bob = id(2), carol = id(3);
    // A busy request from bob (allowed while the bridge was Starting) and a plain one from carol.
    auto busyAndPlain = [&](FakeEnv& env, Ledger& l)
    {
        setUp(env, bob); setUp(env, carol);
        l.receive(id(1040), Action::Request, bob, 0);
        l.receive(id(1041), Action::Request, carol, 0);
        env.bridge = false;
        env.starting = true;
        CHECK(l.answer(id(1040), bob, true, 1) == Answer::Busy);
        env.clearLog();
    };
    auto toldCount = [](const FakeEnv& env, const Id& who)
    {
        int n = 0;
        for (const auto& p : env.notices) n += (p.first == Notice::AllowNeverSent && p.second == who) ? 1 : 0;
        return n;
    };
    for (ClearReason reason : { ClearReason::Teleport, ClearReason::RegionChanged })
    {   // a teleport's start or a region change: told for the busy one only
        FakeEnv env; Ledger l(env);
        busyAndPlain(env, l);
        l.clearAll(reason, 2);
        CHECK(toldCount(env, bob) == 1);
        CHECK(toldCount(env, carol) == 0);
    }
    {   // a logout: no one is told
        FakeEnv env; Ledger l(env);
        busyAndPlain(env, l);
        l.clearAll(ClearReason::Logout, 2);
        CHECK(env.notices.empty());
    }
    {   // carol's (never allowed) cancelled: not told
        FakeEnv env; Ledger l(env);
        busyAndPlain(env, l);
        l.receive(id(1041), Action::Cancel, carol, 2);
        CHECK(l.incoming().size() == 1);
        CHECK(toldCount(env, carol) == 0);
    }
    {   // requests never allowed that idle drops (60 s, out of range, the policy): never told
        FakeEnv env; Ledger l(env); setUp(env, bob); setUp(env, carol);
        const Id dave = id(4); setUp(env, dave);
        l.receive(id(1042), Action::Request, bob, 0);
        l.receive(id(1043), Action::Request, carol, 30);
        l.receive(id(1044), Action::Request, dave, 30);
        l.idle(TIMEOUT_SECONDS);                       // bob's 60 s
        env.place(carol, Where::Far);                  // carol out of range
        env.friends.erase({ dave });                   // dave no longer a friend (Friends policy)
        l.idle(TIMEOUT_SECONDS + 1);
        CHECK(l.incoming().empty());
        CHECK(toldCount(env, bob) == 0 && toldCount(env, carol) == 0 && toldCount(env, dave) == 0);
        CHECK(env.sentAction(Action::Refuse, bob) && env.sentAction(Action::Refuse, carol) && env.sentAction(Action::Refuse, dave));
    }
    {   // a busy one that idle drops for the policy: told once
        FakeEnv env; Ledger l(env);
        busyAndPlain(env, l);
        env.pol = Policy::Nobody;
        l.idle(2);
        CHECK(toldCount(env, bob) == 1);
        CHECK(toldCount(env, carol) == 0);
    }
}

// Integration review L2 (2026-11-14): a send the bridge took that did not get through (sendFailed).
static void testSendFailedCases()
{
    const Id bob = id(2), carol = id(3);
    {   // a withdraw that did not get through: back on the list, marked, sent again; the peer's cancel still counts
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1050), Action::Request, bob, 0);
        l.answer(id(1050), bob, true, 0);
        CHECK(l.withdraw(bob, 1) == Withdrawal::Withdrawn);
        CHECK(!l.isGranted(bob));
        const unsigned gen = l.grantedGeneration();
        l.sendFailed(Action::Withdraw, id(1050), bob, 2);
        CHECK(l.isGranted(bob) && l.grantedWithdrawPending(0));
        CHECK(l.grantedGeneration() != gen);
        env.clearLog();
        l.idle(3);                                     // sent again after the first wait
        CHECK(env.sentAction(Action::Withdraw, bob));
        CHECK(!l.isGranted(bob));
        // again failed, then the peer cancels it: gone (its ended mark was taken away when it came back)
        l.sendFailed(Action::Withdraw, id(1050), bob, 4);
        CHECK(l.isGranted(bob));
        l.receive(id(1050), Action::Cancel, bob, 5);
        CHECK(!l.isGranted(bob));
    }
    {   // a withdraw that did not get through to someone no longer here: nothing comes back
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1051), Action::Request, bob, 0);
        l.answer(id(1051), bob, true, 0);
        l.withdraw(bob, 1);
        env.place(bob, Where::Gone);
        l.sendFailed(Action::Withdraw, id(1051), bob, 2);
        CHECK(!l.isGranted(bob));
        CHECK(!l.active());
    }
    {   // an allow whose post failed (integration review 2, M1): the asker may have it, so the permission stays listed,
        // not marked to be withdrawn; the person is told it is unsure, and can withdraw it
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1052), Action::Request, bob, 0);
        CHECK(l.answer(id(1052), bob, true, 1) == Answer::Allowed);
        CHECK(l.isGranted(bob));
        l.sendFailed(Action::Accept, id(1052), bob, 2);
        CHECK(l.isGranted(bob));
        CHECK(!l.grantedWithdrawPending(0));
        CHECK(countNotices(env, Notice::AllowUnsure, bob) == 1);
        CHECK(countNotices(env, Notice::AllowNeverSent, bob) == 0);
        env.clearLog();
        l.idle(3);
        CHECK(env.sent.empty());                       // nothing sent by itself: withdrawing is the person's call
        CHECK(l.withdraw(bob, 4) == Withdrawal::Withdrawn);
        CHECK(env.sentAction(Action::Withdraw, bob));
        CHECK(!l.isGranted(bob));
    }
    {   // a request that did not get through: still Pending, then NoAnswer at its 60 s
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.ask(bob, 0);
        l.sendFailed(Action::Request, env.lastNonce(), bob, 1);
        CHECK(l.status(bob, 1) == Status::Pending);
        l.idle(TIMEOUT_SECONDS);
        CHECK(l.status(bob, TIMEOUT_SECONDS) == Status::NoAnswer);
    }
    {   // a cancel or refuse that did not get through: nothing changes
        FakeEnv env; Ledger l(env); setUp(env, bob); setUp(env, carol);
        l.ask(bob, 0);
        const Id nonce = env.lastNonce();
        l.cancel(bob, 1);
        l.receive(id(1053), Action::Request, carol, 1);
        l.answer(id(1053), carol, false, 1);
        env.clearLog();
        l.sendFailed(Action::Cancel, nonce, bob, 2);
        l.sendFailed(Action::Refuse, id(1053), carol, 2);
        CHECK(l.status(bob, 2) == Status::NoAnswer);
        CHECK(l.incoming().empty() && !l.isGranted(carol));
        CHECK(env.sent.empty() && env.notices.empty());
        CHECK(!l.active());
    }
}

// Integration review 3 (2026-11-14): a permission given to someone we then mute.
static void testMutedGrantedCases()
{
    const Id bob = id(2);
    {   // muted, the withdraw cannot be sent: stays listed as waiting, sent again (muted still), then gone
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1060), Action::Request, bob, 0);
        l.answer(id(1060), bob, true, 0);
        env.place(bob, Where::Muted);
        env.sendWorks = false;
        env.clearLog();
        l.idle(1);
        CHECK(env.sentAction(Action::Withdraw, bob));
        CHECK(l.isGranted(bob) && l.grantedWithdrawPending(0));
        env.sendWorks = true;
        l.idle(2);                                     // resendOneWithdraw: due 1 s later
        CHECK(!l.isGranted(bob));
        CHECK(!l.active());
    }
    {   // muted, sent at once: gone; nothing more is sent
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1061), Action::Request, bob, 0);
        l.answer(id(1061), bob, true, 0);
        env.place(bob, Where::Muted);
        env.clearLog();
        l.idle(1);
        CHECK(!l.isGranted(bob));
        l.idle(2);
        CHECK(env.sent.size() == 1);
    }
    {   // muted, the withdraw's post failed: it comes back to be sent again (muted is still here)
        FakeEnv env; Ledger l(env); setUp(env, bob);
        l.receive(id(1062), Action::Request, bob, 0);
        l.answer(id(1062), bob, true, 0);
        env.place(bob, Where::Muted);
        l.idle(1);                                     // sent, dropped
        CHECK(!l.isGranted(bob));
        l.sendFailed(Action::Withdraw, id(1062), bob, 2);
        CHECK(l.isGranted(bob) && l.grantedWithdrawPending(0));
        env.clearLog();
        l.idle(3);
        CHECK(env.sentAction(Action::Withdraw, bob));
        CHECK(!l.isGranted(bob));
    }
}

// Stage 2 (research/374b, posereflect.cpp): a second ledger whose Env says "blocked" for a person stage 1 did not allow,
// Policy::Everyone, and endWhereParentGone() ending what stage 1 no longer holds.
class StageTwoEnv : public FakeEnv
{
public:
    std::set<Key> stageOne;     // the people stage 1 allows (as poseconsent::mayPoseUs / status Accepted)
    bool isBlocked(const Id& peer) const override { return blocked.count({ peer }) != 0 || stageOne.count({ peer }) == 0; }
};

static void testStageTwo()
{
    const Id bob = id(2);
    const Id eve = id(3);
    {   // T side: a request from a friend stage 1 allowed is shown; one from a person it did not allow, or from one who
        // is not a friend (stage 2 is for friends: the policy is Friends), is dropped without an answer
        StageTwoEnv env; Ledger l(env); setUp(env, bob); env.pol = Policy::Friends;
        env.place(eve, Where::Near);
        env.stageOne.insert({ bob });
        env.stageOne.insert({ eve });                  // stage 1 allowed, but not a friend
        const Id mal = id(4);
        setUp(env, mal);                                // a friend, but stage 1 did not allow
        l.receive(id(1500), Action::Request, bob, 0);
        l.receive(id(1501), Action::Request, eve, 0);
        l.receive(id(1502), Action::Request, mal, 0);
        CHECK(l.incoming().size() == 1 && l.incoming()[0].peer == bob);
        CHECK(!env.sentAction(Action::Refuse, mal));
        CHECK(!env.sentAction(Action::Refuse, eve));
        CHECK(l.answer(id(1500), bob, true, 1) == Answer::Allowed);
        CHECK(env.sentAction(Action::Accept, bob));
        CHECK(l.isGranted(bob));

        // stage 1 withdrawn: the permission is withdrawn too, once (a second look does not send again)
        env.clearLog();
        env.stageOne.clear();
        auto none = [](const Id&) { return false; };
        l.endWhereParentGone(none, [&](const Id& p) { return env.stageOne.count({ p }) != 0; }, 2);
        CHECK(env.sentAction(Action::Withdraw, bob));
        CHECK(!l.isGranted(bob));
        env.clearLog();
        l.endWhereParentGone(none, [&](const Id& p) { return env.stageOne.count({ p }) != 0; }, 3);
        CHECK(env.sent.empty());
    }
    {   // a request waiting when stage 1 goes: idle() refuses it (the Env now says blocked)
        StageTwoEnv env; Ledger l(env); setUp(env, bob); env.pol = Policy::Friends;
        env.stageOne.insert({ bob });
        l.receive(id(1510), Action::Request, bob, 0);
        CHECK(l.incoming().size() == 1);
        env.stageOne.clear();
        l.idle(1);
        CHECK(l.incoming().empty());
        CHECK(env.sentAction(Action::Refuse, bob));
    }
    {   // P side: Accepted ends (cancel sent, consent lost, back to Ask) when stage 1 no longer holds; kept while it does
        StageTwoEnv env; Ledger l(env); setUp(env, bob);
        CHECK(l.ask(bob, 0));
        l.receive(env.lastNonce(), Action::Accept, bob, 1);
        CHECK(l.canPose(bob));
        const auto holds = [](const Id&) { return true; };
        env.clearLog();
        l.endWhereParentGone(holds, holds, 2);
        CHECK(l.canPose(bob) && env.sent.empty());      // stage 1 holds: nothing ends
        l.endWhereParentGone([](const Id&) { return false; }, holds, 3);
        CHECK(env.sentAction(Action::Cancel, bob));
        CHECK(env.lost.size() == 1 && env.lost[0] == bob);
        CHECK(!l.canPose(bob));
        CHECK(l.status(bob, 3) == Status::Ask);
    }
    {   // P side: a request still out when stage 1 goes is taken back (cancel sent, no notice); the cooldown follows
        StageTwoEnv env; Ledger l(env); setUp(env, bob);
        CHECK(l.ask(bob, 0));
        CHECK(l.status(bob, 1) == Status::Pending);
        env.clearLog();
        l.endWhereParentGone([](const Id&) { return false; }, [](const Id&) { return true; }, 2);
        CHECK(env.sentAction(Action::Cancel, bob));
        CHECK(env.notices.empty());
        CHECK(l.status(bob, 2) == Status::NoAnswer);
    }
    {   // a permission whose withdraw is not sent yet is not withdrawn again by the look
        StageTwoEnv env; Ledger l(env); setUp(env, bob); env.pol = Policy::Everyone;
        env.stageOne.insert({ bob });
        l.receive(id(1520), Action::Request, bob, 0);
        l.answer(id(1520), bob, true, 0);
        env.sendWorks = false;
        CHECK(l.withdraw(bob, 1) == Withdrawal::NotSent);
        env.clearLog();
        l.endWhereParentGone([](const Id&) { return true; }, [](const Id&) { return false; }, 2);
        CHECK(env.sent.empty());
    }
}

int main()
{
    testStageTwo();
    testMutedGrantedCases();
    testSendFailedCases();
    testReview12Cases();
    testReview11Cases();
    testReview10Cases();
    testReview6Cases();
    testReview4Cases();
    testReview2Cases();
    testAsking();
    testAsked();
    testIdleCheap();
    testReentrant();
    testReentrantSend();
    testReviewCases();
    if (failures == 0)
    {
        std::printf("poseconsent_test: all passed\n");
        return 0;
    }
    std::printf("poseconsent_test: %d failed\n", failures);
    return 1;
}
