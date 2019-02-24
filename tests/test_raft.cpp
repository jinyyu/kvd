#include <gtest/gtest.h>
#include <kvd/raft/Raft.h>
#include <kvd/common/log.h>
#include <kvd/raft/util.h>
#include "network.hpp"


using namespace kvd;


std::vector<uint8_t> str_to_vector(const char* str)
{
    size_t len = strlen(str);
    std::vector<uint8_t> data(str, str + len);
    return data;
}

static RaftPtr newTestRaft(uint64_t id,
                           std::vector<uint64_t> peers,
                           uint64_t election,
                           uint64_t heartbeat,
                           StoragePtr storage)
{
    Config c = newTestConfig(id, peers, election, heartbeat, storage);
    c.max_inflight_msgs = 256;
    Status status = c.validate();
    assert(status.is_ok());
    return std::make_shared<Raft>(c);
}

static RaftPtr newTestLearnerRaft(uint64_t id,
                                  std::vector<uint64_t> peers,
                                  std::vector<uint64_t> learners,
                                  uint64_t election,
                                  uint64_t heartbeat,
                                  StoragePtr storage)
{
    Config c = newTestConfig(id, peers, election, heartbeat, storage);
    c.learners = learners;
    c.max_inflight_msgs = 256;
    Status status = c.validate();
    assert(status.is_ok());
    return std::make_shared<Raft>(c);
}

TEST(raft, ProgressLeader)
{
    auto r = newTestRaft(1, {1, 2}, 5, 1, std::make_shared<MemoryStorage>());
    r->become_candidate();
    r->become_leader();
    r->get_progress(2)->become_replicate();

    proto::MessagePtr propMsg(new proto::Message());
    propMsg->from = 1;
    propMsg->to = 1;
    propMsg->type = proto::MsgProp;
    proto::Entry e;
    e.data = std::vector<uint8_t>{'f', 'o', 'o'};
    propMsg->entries.push_back(e);


    // Send proposals to r1. The first 5 entries should be appended to the log.
    for (uint32_t i = 0; i < 5; i++) {
        LOG_INFO("ProgressLeader %u", i);
        auto pr = r->get_progress(r->id());
        ASSERT_TRUE(pr->state == ProgressStateReplicate);
        ASSERT_TRUE(pr->match = i + 1);
        ASSERT_TRUE(pr->next == pr->match + 1);
        Status status = r->step(propMsg);
        if (!status.is_ok()) {
            LOG_ERROR("proposal resulted in error: %s", status.to_string().c_str());
        }
        ASSERT_TRUE(status.is_ok());
    }
}

TEST(raft, ProgressResumeByHeartbeatResp)
{
    // ensures raft.heartbeat reset progress.paused by heartbeat response.
    auto r = newTestRaft(1, {1, 2}, 5, 1, std::make_shared<MemoryStorage>());
    r->become_candidate();
    r->become_leader();

    r->get_progress(2)->paused = true;

    proto::MessagePtr msg(new proto::Message());
    msg->from = 1;
    msg->to = 1;
    msg->type = proto::MsgBeat;
    Status status = r->step(msg);
    ASSERT_TRUE(status.is_ok());

    ASSERT_TRUE(r->get_progress(2)->paused);

    r->get_progress(2)->become_replicate();

    proto::MessagePtr m2(new proto::Message());
    m2->from = 2;
    m2->to = 1;
    m2->type = proto::MsgHeartbeatResp;
    status = r->step(m2);
    ASSERT_FALSE(r->get_progress(2)->paused);
}

TEST(raft, ProgressPaused)
{
    auto r = newTestRaft(1, {1, 2}, 5, 1, std::make_shared<MemoryStorage>());
    r->become_candidate();
    r->become_leader();

    proto::MessagePtr msg(new proto::Message());
    msg->from = 1;
    msg->to = 1;
    msg->type = proto::MsgProp;
    proto::Entry e;
    e.data = std::vector<uint8_t>{'f', 'o', 'o'};
    msg->entries.push_back(e);
    r->step(msg);
    r->step(msg);
    r->step(msg);

    auto msgs = r->msgs();
    ASSERT_TRUE(r->msgs().size() == 1);
}

TEST(raft, ProgressFlowControl)
{
    auto c = newTestConfig(1, {1, 2}, 5, 1, std::make_shared<MemoryStorage>());
    c.max_inflight_msgs = 3;
    c.max_size_per_msg = 2048;
    RaftPtr r(new Raft(c));
    r->become_candidate();
    r->become_leader();

    // Throw away all the messages relating to the initial election.
    r->msgs().clear();

    // While node 2 is in probe state, propose a bunch of entries.
    r->get_progress(2)->become_probe();


    for (size_t i = 0; i < 10; i++) {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 1;
        msg->to = 1;
        msg->type = proto::MsgProp;
        proto::Entry e;
        e.data.resize(1000, 'a');
        msg->entries.push_back(e);
        r->step(msg);
    }
    auto ms = r->msgs();
    r->msgs().clear();

    // First append has two entries: the empty entry to confirm the
    // election, and the first proposal (only one proposal gets sent
    // because we're in probe state).
    ASSERT_TRUE(ms.size() == 1);
    ASSERT_TRUE(ms[0]->type == proto::MsgApp);

    ASSERT_TRUE(ms[0]->entries.size() == 2);
    ASSERT_TRUE(ms[0]->entries[0].data.empty());
    ASSERT_TRUE(ms[0]->entries[1].data.size() == 1000);

    // When this append is acked, we change to replicate state and can
    // send multiple messages at once.
    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 2;
        msg->to = 1;
        msg->type = proto::MsgAppResp;
        msg->index = ms[0]->entries[1].index;
        r->step(msg);
    }
    ms = r->msgs();
    r->msgs().clear();
    ASSERT_TRUE(ms.size() == 3);

    for (size_t i = 0; i < ms.size(); ++i) {
        ASSERT_TRUE(ms[i]->type == proto::MsgApp);
        ASSERT_TRUE(ms[i]->entries.size() == 2);
    }

    // Ack all three of those messages together and get the last two
    // messages (containing three entries).
    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 2;
        msg->to = 1;
        msg->type = proto::MsgAppResp;
        msg->index = ms[2]->entries[1].index;
        r->step(msg);
    }

    ms = r->msgs();
    r->msgs().clear();

    ASSERT_TRUE(ms.size() == 2);
    for (size_t i = 0; i < ms.size(); ++i) {
        ASSERT_TRUE(ms[i]->type == proto::MsgApp);
    }

    ASSERT_TRUE(ms[0]->entries.size() == 2);
    ASSERT_TRUE(ms[1]->entries.size() == 1);
}

TEST(raft, UncommittedEntryLimit)
{
    // Use a relatively large number of entries here to prevent regression of a
    // bug which computed the size before it was fixed. This test would fail
    // with the bug, either because we'd get dropped proposals earlier than we
    // expect them, or because the final tally ends up nonzero. (At the time of
    // writing, the former).
    uint64_t maxEntries = 1024;
    proto::Entry testEntry;
    testEntry.data.resize(8, 'a');
    uint64_t maxEntrySize = maxEntries * testEntry.payload_size();

    auto cfg = newTestConfig(1, {1, 2, 3}, 5, 1, std::make_shared<MemoryStorage>());
    cfg.max_uncommitted_entries_size = maxEntrySize;
    cfg.max_inflight_msgs = 2 * 1024; // avoid interference
    RaftPtr r(new Raft(cfg));
    r->become_candidate();
    r->become_leader();

    ASSERT_TRUE(r->uncommitted_size() == 0);

    // Set the two followers to the replicate state. Commit to tail of log.
    uint64_t numFollowers = 2;
    r->get_progress(2)->become_replicate();
    r->get_progress(3)->become_replicate();
    r->uncommitted_size() = 0;

    proto::MessagePtr propMsg(new proto::Message());
    propMsg->from = 1;
    propMsg->to = 1;
    propMsg->type = proto::MsgProp;
    propMsg->entries.push_back(testEntry);

    // Send proposals to r1. The first 5 entries should be appended to the log.
    std::vector<proto::EntryPtr> propEnts;

    for (uint64_t i = 0; i < maxEntries; i++) {
        Status status = r->step_leader(propMsg);
        ASSERT_TRUE(status.is_ok());
        propEnts.push_back(std::make_shared<proto::Entry>(testEntry));
    }

    // Send one more proposal to r1. It should be rejected.
    Status status = r->step(propMsg);
    ASSERT_FALSE(status.is_ok());
    fprintf(stderr, "status :%s\n", status.to_string().c_str());

    auto ms = r->msgs();
    r->msgs().clear();
    // Read messages and reduce the uncommitted size as if we had committed
    // these entries.

    ASSERT_TRUE(ms.size() == maxEntries * numFollowers);

    r->reduce_uncommitted_size(propEnts);
    ASSERT_TRUE(r->uncommitted_size() == 0);

    // Send a single large proposal to r1. Should be accepted even though it
    // pushes us above the limit because we were beneath it before the proposal.

    propEnts.resize(2 * maxEntries);

    for (size_t i = 0; i < propEnts.size(); ++i) {
        propEnts[i] = std::make_shared<proto::Entry>(testEntry);
    }

    proto::MessagePtr propMsgLarge(new proto::Message());
    propMsgLarge->from = 1;
    propMsgLarge->to = 1;
    propMsgLarge->type = proto::MsgProp;
    for (size_t i = 0; i < propEnts.size(); ++i) {
        propMsgLarge->entries.push_back(*propEnts[i]);
    }

    status = r->step(propMsgLarge);
    ASSERT_TRUE(status.is_ok());


    // Send one more proposal to r1. It should be rejected, again.
    status = r->step(propMsg);
    ASSERT_FALSE(status.is_ok());
    fprintf(stderr, "status :%s\n", status.to_string().c_str());


    // Read messages and reduce the uncommitted size as if we had committed
    // these entries.
    ms = r->msgs();
    r->msgs().clear();
    ASSERT_TRUE(ms.size() == numFollowers * 1);
    r->reduce_uncommitted_size(propEnts);
    ASSERT_TRUE(r->uncommitted_size() == 0);
}

void testLeaderElection(bool preVote)
{
    ConfigFunc cfg = [](Config&) {};
    RaftState candState = RaftState::Candidate;
    uint64_t candTerm = 1;
    if (preVote) {
        cfg = preVoteConfig;
        // In pre-vote mode, an election that fails to complete
        // leaves the node in pre-candidate state without advancing
        // the term.
        candState = RaftState::PreCandidate;
        candTerm = 0;
    }

    struct Test
    {
        NetworkPtr network;
        RaftState state;
        uint64_t expTerm;
    };

    std::vector<Test> tests;
    std::shared_ptr<BlackHole> nopStepper(new BlackHole());

    {
        std::vector<RaftPtr> peers{nullptr, nullptr, nullptr};
        Test t{.network = std::make_shared<Network>(cfg, peers), .state = RaftState::Leader, .expTerm = 1};
        tests.push_back(t);
    }

    {
        std::vector<RaftPtr> peers{nullptr, nullptr, nopStepper};
        Test t{.network = std::make_shared<Network>(cfg, peers), .state = RaftState::Leader, .expTerm = 1};
        tests.push_back(t);
    }

    {
        std::vector<RaftPtr> peers{nullptr, nopStepper, nopStepper};
        Test t{.network = std::make_shared<Network>(cfg, peers), .state = candState, .expTerm = candTerm};
        tests.push_back(t);
    }
    {
        std::vector<RaftPtr> peers{nullptr, nopStepper, nopStepper, nullptr};
        Test t{.network = std::make_shared<Network>(cfg, peers), .state = candState, .expTerm = candTerm};
        tests.push_back(t);
    }
    {
        std::vector<RaftPtr> peers{nullptr, nopStepper, nopStepper, nullptr, nullptr};
        Test t{.network = std::make_shared<Network>(cfg, peers), .state = RaftState::Leader, .expTerm = 1};
        tests.push_back(t);
    }

    {
        // three logs further along than 0, but in the same term so rejections
        // are returned instead of the votes being ignored.
        std::vector<RaftPtr> peers{nullptr,
                                   entsWithConfig(cfg, std::vector<uint64_t>{1}),
                                   entsWithConfig(cfg, std::vector<uint64_t>{1}),
                                   entsWithConfig(cfg, std::vector<uint64_t>{1, 1}),
                                   nullptr};
        Test t{.network = std::make_shared<Network>(cfg, peers), .state = RaftState::Follower, .expTerm = 1};
        tests.push_back(t);
    }


    for (size_t i = 0; i < tests.size(); ++i) {
        Test& test = tests[i];
        std::vector<proto::MessagePtr> msgs;
        proto::MessagePtr m(new proto::Message());
        m->from = 1;
        m->to = 1;
        m->type = proto::MsgHup;
        msgs.push_back(m);
        test.network->send(msgs);

        auto sm = test.network->peers[1];
        ASSERT_TRUE(sm->state_ == test.state);
        ASSERT_TRUE(sm->term_ == test.expTerm);
    }
}

TEST(raft, LeaderElection)
{
    testLeaderElection(false);
}

TEST(raft, LeaderElectionPreVote)
{
    testLeaderElection(true);
}

// TestLearnerElectionTimeout verfies that the leader should not start election even
// when times out.
TEST(raft, LearnerElectionTimeout)
{
    auto n1 = newTestLearnerRaft(1,
                                 std::vector<uint64_t>{1},
                                 std::vector<uint64_t>{2},
                                 10,
                                 1,
                                 std::make_shared<MemoryStorage>());
    auto n2 = newTestLearnerRaft(2,
                                 std::vector<uint64_t>{1},
                                 std::vector<uint64_t>{2},
                                 10,
                                 1,
                                 std::make_shared<MemoryStorage>());

    n1->become_follower(1, 0);
    n2->become_follower(1, 0);

    // n2 is learner. Learner should not start election even when times out.
    n2->randomized_election_timeout_ = n2->election_timeout_;
    for (uint64_t i = 0; i < n2->election_timeout_; i++) {
        n2->tick();
    }

    ASSERT_TRUE(n2->state_ == RaftState::Follower);
}

// TestLearnerPromotion verifies that the learner should not election until
// it is promoted to a normal peer.
TEST(raft, LearnerPromotion)
{
    auto n1 = newTestLearnerRaft(1,
                                 std::vector<uint64_t>{1},
                                 std::vector<uint64_t>{2},
                                 10,
                                 1,
                                 std::make_shared<MemoryStorage>());
    auto n2 = newTestLearnerRaft(2,
                                 std::vector<uint64_t>{1},
                                 std::vector<uint64_t>{2},
                                 10,
                                 1,
                                 std::make_shared<MemoryStorage>());

    n1->become_follower(1, 0);
    n2->become_follower(1, 0);

    Network nt(std::vector<RaftPtr>{n1, n2});

    ASSERT_FALSE(n1->state_ == RaftState::Leader);

    n1->randomized_election_timeout_ = n1->election_timeout_;
    // n1 should become leader

    for (uint64_t i = 0; i < n1->election_timeout_; i++) {
        n1->tick();
    }

    ASSERT_TRUE(n1->state_ == RaftState::Leader);
    ASSERT_TRUE(n2->state_ == RaftState::Follower);

    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 1;
        msg->to = 1;
        msg->type = proto::MsgBeat;
        std::vector<proto::MessagePtr> msgs{msg};
        nt.send(msgs);
    }


    n1->add_node(2);
    n2->add_node(2);
    ASSERT_TRUE(!n2->is_learner_);

    // n2 start election, should become leader
    n2->randomized_election_timeout_ = n2->election_timeout_;
    for (uint64_t i = 0; i < n2->election_timeout_; i++) {
        n2->tick();
    }

    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 2;
        msg->to = 2;
        msg->type = proto::MsgBeat;
        std::vector<proto::MessagePtr> msgs{msg};
        nt.send(msgs);
    }

    ASSERT_TRUE(n1->state_ == RaftState::Follower);
    ASSERT_TRUE(n2->state_ == RaftState::Leader);
}

// TestLearnerCannotVote checks that a learner can't vote even it receives a valid Vote request.
TEST(raft, LearnerCannotVote)
{
    auto n2 = newTestLearnerRaft(2,
                                 std::vector<uint64_t>{1},
                                 std::vector<uint64_t>{2},
                                 10,
                                 1,
                                 std::make_shared<MemoryStorage>());

    n2->become_follower(1, 0);

    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 1;
        msg->to = 2;
        msg->term = 2;
        msg->type = proto::MsgVote;
        msg->log_term = 1;
        msg->index = 11;
        n2->step(msg);
    }

    ASSERT_TRUE(n2->msgs_.empty());
}

// testLeaderCycle verifies that each node in a cluster can campaign
// and be elected in turn. This ensures that elections (including
// pre-vote) work when not starting from a clean slate (as they do in
// TestLeaderElection)
void testLeaderCycle(bool preVote)
{
    ConfigFunc cfg = [](Config& c) {};
    if (preVote) {
        cfg = preVoteConfig;
    }

    Network n(cfg, std::vector<RaftPtr>{nullptr, nullptr, nullptr});
    for (uint64_t campaignerID = 1; campaignerID <= 1; campaignerID++) {

        {
            proto::MessagePtr msg(new proto::Message());
            msg->from = campaignerID;
            msg->to = campaignerID;
            msg->type = proto::MsgHup;
            std::vector<proto::MessagePtr> msgs{msg};
            n.send(msgs);
        }

        for (auto it = n.peers.begin(); it != n.peers.end(); ++it) {
            auto sm = it->second;

            if (sm->id_ == campaignerID && sm->state_ != RaftState::Leader) {
                ASSERT_FALSE(true);
            }
            else if (sm->id_ != campaignerID && sm->state_ != RaftState::Follower) {
                ASSERT_FALSE(true);
            }
        }
    }
}

TEST(raft, LeaderCycle)
{
    testLeaderCycle(false);
}

TEST(raft, LeaderCyclePreVote)
{
    testLeaderCycle(true);
}

void testLeaderElectionOverwriteNewerLogs(bool preVote)
{
    ConfigFunc cfg = [](Config& c) {};
    if (preVote) {
        cfg = preVoteConfig;
    }
    // This network represents the results of the following sequence of
    // events:
    // - Node 1 won the election in term 1.
    // - Node 1 replicated a log entry to node 2 but died before sending
    //   it to other nodes.
    // - Node 3 won the second election in term 2.
    // - Node 3 wrote an entry to its logs but died without sending it
    //   to any other nodes.
    //
    // At this point, nodes 1, 2, and 3 all have uncommitted entries in
    // their logs and could win an election at term 3. The winner's log
    // entry overwrites the losers'. (TestLeaderSyncFollowerLog tests
    // the case where older log entries are overwritten, so this test
    // focuses on the case where the newer entries are lost).
    std::vector<RaftPtr> peers;
    peers.push_back(entsWithConfig(cfg, std::vector<uint64_t>{1}));// Node 1: Won first election
    peers.push_back(entsWithConfig(cfg, std::vector<uint64_t>{1}));// Node 2: Got logs from node 1
    peers.push_back(entsWithConfig(cfg, std::vector<uint64_t>{2})); // Node 3: Won second election
    peers.push_back(votedWithConfig(cfg, 3, 2)); // Node 4: Voted but didn't get logs
    peers.push_back(votedWithConfig(cfg, 3, 2)); // Node 5: Voted but didn't get logs
    Network n(cfg, peers);

    // Node 1 campaigns. The election fails because a quorum of nodes
    // know about the election that already happened at term 2. Node 1's
    // term is pushed ahead to 2.
    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 1;
        msg->to = 1;
        msg->type = proto::MsgHup;
        std::vector<proto::MessagePtr> msgs{msg};
        n.send(msgs);
    };

    auto sm1 = n.peers[1];
    ASSERT_TRUE(sm1->state_ == RaftState::Follower);
    ASSERT_TRUE(sm1->term_ = 2);

    // Node 1 campaigns again with a higher term. This time it succeeds.
    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 1;
        msg->to = 1;
        msg->type = proto::MsgHup;
        std::vector<proto::MessagePtr> msgs{msg};
        n.send(msgs);
    }

    ASSERT_TRUE(sm1->state_ == RaftState::Leader);
    ASSERT_TRUE(sm1->term_ = 3);

    // Now all nodes agree on a log entry with term 1 at index 1 (and
    // term 3 at index 2).

    for (auto it = n.peers.begin(); it != n.peers.end(); ++it) {
        auto sm = it->second;
        std::vector<proto::EntryPtr> entries;
        sm->raft_log_->all_entries(entries);
        ASSERT_TRUE(entries.size() == 2);
        ASSERT_TRUE(entries[0]->term = 1);
        ASSERT_TRUE(entries[1]->term = 3);
    }
}

// LeaderElectionOverwriteNewerLogs tests a scenario in which a
// newly-elected leader does *not* have the newest (i.e. highest term)
// log entries, and must overwrite higher-term log entries with
// lower-term ones.
TEST(raft, LeaderElectionOverwriteNewerLogs)
{
    testLeaderElectionOverwriteNewerLogs(false);
}

TEST(raft, LeaderElectionOverwriteNewerLogsPreVote)
{
    testLeaderElectionOverwriteNewerLogs(true);
}

void testVoteFromAnyState(proto::MessageType vt)
{
    for (auto st = (size_t) RaftState::Follower; st <= RaftState::PreCandidate; st++) {
        auto r = newTestRaft(1, std::vector<uint64_t>{1, 2, 3}, 10, 1, std::make_shared<MemoryStorage>());
        r->term_ = 1;

        switch (st) {
            case RaftState::Follower: {
                r->become_follower(r->term_, 3);
                break;
            }
            case RaftState::PreCandidate: {
                r->become_pre_candidate();
                break;
            }

            case RaftState::Candidate: {
                r->become_candidate();
                break;
            }
            case RaftState::Leader: {
                r->become_candidate();
                r->become_leader();
            }
        }

        // Note that setting our state above may have advanced r.Term
        // past its initial value.
        auto origTerm = r->term_;
        auto newTerm = r->term_ + 1;

        proto::MessagePtr msg(new proto::Message());
        msg->from = 2;
        msg->to = 1;
        msg->type = vt;
        msg->term = newTerm;
        msg->log_term = newTerm;
        msg->index = 42;

        Status status = r->step(msg);
        ASSERT_TRUE(status.is_ok());
        ASSERT_TRUE(r->msgs_.size() == 1);

        auto resp = r->msgs_[0];
        ASSERT_TRUE(resp->type == vote_resp_msg_type(vt));
        ASSERT_FALSE(resp->reject);

        if (vt == proto::MsgVote) {
            ASSERT_TRUE(r->state_ == RaftState::Follower);
            ASSERT_TRUE(r->term_ == newTerm);
            ASSERT_TRUE(r->vote_ == 2);
        }
        else {
            // In a prevote, nothing changes.
            ASSERT_TRUE(r->state_ == st);
            ASSERT_TRUE(r->term_ == origTerm);
            // if st == StateFollower or StatePreCandidate, r hasn't voted yet.
            // In StateCandidate or StateLeader, it's voted for itself.
            ASSERT_FALSE(r->vote_ != 0 && r->vote_ != 1);
        }
    }
}

TEST(raft, VoteFromAnyState)
{
    testVoteFromAnyState(proto::MsgVote);
}

TEST(raft, PreVoteFromAnyState)
{
    testVoteFromAnyState(proto::MsgPreVote);
}

TEST(raft, LogReplication)
{
    struct Test
    {
        NetworkPtr network;
        std::vector<proto::MessagePtr> msgs;
        uint64_t wcommitted;
    };

    std::vector<Test> tests;

    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 1;
        msg->to = 1;
        msg->type = proto::MsgProp;
        proto::Entry e;
        e.data = str_to_vector("somedata");
        msg->entries.push_back(e);
        std::vector<proto::MessagePtr> msgs{msg};

        std::vector<RaftPtr> peers{nullptr, nullptr, nullptr};

        tests.push_back(Test{.network = std::make_shared<Network>(peers),
            .msgs = msgs,
            .wcommitted = 2,
        });
    }

    {
        std::vector<proto::MessagePtr> msgs;
        {
            proto::MessagePtr msg(new proto::Message());
            msg->from = 1;
            msg->to = 1;
            msg->type = proto::MsgProp;
            proto::Entry e;
            e.data = str_to_vector("somedata");
            msg->entries.push_back(e);
            msgs.push_back(msg);
        }
        {
            proto::MessagePtr msg(new proto::Message());
            msg->from = 1;
            msg->to = 2;
            msg->type = proto::MsgHup;;
            msgs.push_back(msg);
        }
        {
            proto::MessagePtr msg(new proto::Message());
            msg->from = 1;
            msg->to = 2;
            msg->type = proto::MsgProp;
            proto::Entry e;
            e.data = str_to_vector("somedata");
            msg->entries.push_back(e);
            msgs.push_back(msg);
        }
        std::vector<RaftPtr> peers{nullptr, nullptr, nullptr};
        tests.push_back(Test{.network = std::make_shared<Network>(peers),
            .msgs = msgs,
            .wcommitted = 4,
        });

    }

    for (size_t i = 0; i < tests.size(); ++i) {
        auto tt = tests[i];

        {
            proto::MessagePtr msg(new proto::Message());
            msg->from = 1;
            msg->to = 1;
            msg->type = proto::MsgHup;
            std::vector<proto::MessagePtr> msgs{msg};
            tt.network->send(msgs);
        }

        for (proto::MessagePtr msg : tt.msgs) {
            std::vector<proto::MessagePtr> msgs{msg};
            tt.network->send(msgs);
        }


        for (auto it = tt.network->peers.begin(); it != tt.network->peers.end(); ++it) {
            RaftPtr sm = it->second;

            ASSERT_TRUE(sm->raft_log_->committed() == tt.wcommitted);

            std::vector<proto::EntryPtr> ents;
            auto out = nextEnts(sm, tt.network->storage[it->first]);
            for (proto::EntryPtr ent : out) {
                if (!ent->data.empty()) {
                    ents.push_back(ent);
                }

            }

            std::vector<proto::MessagePtr> props;
            for (proto::MessagePtr msg : tt.msgs) {
                if (msg->type == proto::MsgProp) {
                    props.push_back(msg);
                }
            }

            for (size_t k = 0; k < props.size(); ++k) {
                proto::MessagePtr m = props[k];
                ASSERT_TRUE(ents[k]->data == m->entries[0].data);
            }
        }

    }
}

// TestLearnerLogReplication tests that a learner can receive entries from the leader.
TEST(raft, LearnerLogReplication)
{
    auto n1 = newTestLearnerRaft(1,
                                 std::vector<uint64_t>{1},
                                 std::vector<uint64_t>{2},
                                 10,
                                 1,
                                 std::make_shared<MemoryStorage>());
    auto n2 = newTestLearnerRaft(2,
                                 std::vector<uint64_t>{1},
                                 std::vector<uint64_t>{2},
                                 10,
                                 1,
                                 std::make_shared<MemoryStorage>());

    Network nt(std::vector<RaftPtr>{n1, n2});

    n1->become_follower(1, 0);
    n2->become_follower(1, 0);

    n1->randomized_election_timeout_ = n1->election_timeout_;

    for (size_t i = 0; i < n1->election_timeout_; i++) {
        n1->tick();
    }

    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 1;
        msg->to = 1;
        msg->type = proto::MsgBeat;
        std::vector<proto::MessagePtr> msgs{msg};
        nt.send(msgs);
    }


    // n1 is leader and n2 is learner
    ASSERT_TRUE(n1->state_ == RaftState::Leader);
    ASSERT_TRUE(n2->is_learner_);

    auto nextCommitted = n1->raft_log_->committed() + 1;
    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 1;
        msg->to = 1;
        msg->type = proto::MsgProp;
        proto::Entry e;
        e.data = str_to_vector("somedata");
        msg->entries.push_back(e);
        std::vector<proto::MessagePtr> msgs{msg};
        nt.send(msgs);
    }

    ASSERT_TRUE(n1->raft_log_->committed() == nextCommitted);
    ASSERT_TRUE(n1->raft_log_->committed() == n2->raft_log_->committed());


    auto match = n1->get_progress(2)->match;
    ASSERT_TRUE(match == n2->raft_log_->committed());
}

TEST(raft, SingleNodeCommit)
{
    std::vector<RaftPtr> peers{nullptr};
    Network tt(peers);
    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 1;
        msg->to = 1;
        msg->type = proto::MsgHup;
        std::vector<proto::MessagePtr> msgs{msg};
        tt.send(msgs);
    }

    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 1;
        msg->to = 1;
        msg->type = proto::MsgProp;
        proto::Entry e;
        e.data = str_to_vector("somedata");
        msg->entries.push_back(e);
        std::vector<proto::MessagePtr> msgs{msg};
        tt.send(msgs);
    }
    {
        proto::MessagePtr msg(new proto::Message());
        msg->from = 1;
        msg->to = 1;
        msg->type = proto::MsgProp;
        proto::Entry e;
        e.data = str_to_vector("somedata");
        msg->entries.push_back(e);
        std::vector<proto::MessagePtr> msgs{msg};
        tt.send(msgs);
    }

    RaftPtr r = tt.peers[1];
    ASSERT_TRUE(r->raft_log_->committed() == 3);

}

int main(int argc, char* argv[])
{
    //testing::GTEST_FLAG(filter) = "raft.LogReplication";
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
