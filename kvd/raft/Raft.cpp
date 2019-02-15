#include <boost/algorithm/string.hpp>
#include <kvd/raft/Raft.h>
#include <kvd/common/log.h>

namespace kvd
{


static const std::string kCampaignPreElection = "CampaignPreElection";
static const std::string kCampaignElection = "CampaignElection";
static const std::string kCampaignTransfer = "CampaignTransfer";

static uint32_t num_of_pending_conf(const std::vector<proto::EntryPtr>& entries)
{
    uint32_t n = 0;
    for (const proto::EntryPtr& entry: entries) {
        if (entry->type == proto::EntryConfChange) {
            n++;
        }
    }
    return n;
}

Raft::Raft(const Config& c)
    : id_(c.id),
      max_msg_size_(c.max_size_per_msg),
      max_uncommitted_size_(c.max_uncommitted_entries_size),
      max_inflight_(c.max_inflight_msgs),
      is_learner_(false),
      lead_(0),
      lead_transferee_(0),
      pending_conf_index_(0),
      uncommitted_size_(0),
      read_only_(new ReadOnly(c.read_only_option)),
      election_elapsed_(0),
      heartbeat_elapsed_(0),
      check_quorum_(c.check_quorum),
      pre_vote_(c.pre_vote),
      heartbeat_timeout_(c.heartbeat_tick),
      election_timeout_(c.election_tick),
      randomized_election_timeout_(0),
      disable_proposal_forwarding_(c.disable_proposal_forwarding),
      random_device_(0, c.election_tick)
{
    raft_log_ = std::make_shared<RaftLog>(c.storage, c.max_committed_size_per_ready);
    proto::HardState hs;
    proto::ConfState cs;
    Status status = c.storage->initial_state(hs, cs);
    if (!status.is_ok()) {
        LOG_FATAL("%s", status.to_string().c_str());
    }


    std::vector<uint64_t> peers = c.peers;
    std::vector<uint64_t> learners = c.learners;

    if (!cs.nodes.empty() || !cs.learners.empty()) {
        if (!peers.empty() || !learners.empty()) {
            // tests; the argument should be removed and these tests should be
            // updated to specify their nodes through a snapshot.
            LOG_FATAL("cannot specify both newRaft(peers, learners) and ConfState.(Nodes, Learners)");
        }
        peers = cs.nodes;
        learners = cs.learners;
    }


    for (uint64_t peer : peers) {
        ProgressPtr p(new Progress(max_inflight_));
        p->next = 1;
        prs_[peer] = p;
    }

    for (uint64_t learner :  learners) {
        auto it = prs_.find(learner);
        if (it != prs_.end()) {
            LOG_FATAL("node %lu is in both learner and peer list", learner);
        }

        ProgressPtr p(new Progress(max_inflight_));
        p->next = 1;
        p->is_learner = true;

        learner_prs_[learner] = p;

        if (id_ == learner) {
            is_learner_ = true;
        }
    }

    if (!hs.is_empty_state()) {
        load_state(hs);
    }


    if (c.applied > 0) {
        raft_log_->applied_to(c.applied);

    }
    become_follower(term_, 0);

    std::string node_str;
    {
        std::vector<std::string> nodes_strs;
        std::vector<uint64_t> node;
        this->nodes(node);
        for (uint64_t n : node) {
            nodes_strs.push_back(std::to_string(n));
        }
        node_str = boost::join(nodes_strs, ",");
    }

    LOG_INFO("raft %lu [peers: [%s], term: %lu, commit: %lu, applied: %lu, last_index: %lu, last_term: %lu]",
             id_,
             node_str.c_str(),
             term_,
             raft_log_->committed(),
             raft_log_->applied(),
             raft_log_->last_index(),
             raft_log_->last_term());
}

Raft::~Raft()
{

}

void Raft::become_follower(uint64_t term, uint64_t lead)
{

    step_ = std::bind(&Raft::step_follower, this, std::placeholders::_1);
    reset(term);

    tick_ = [this]() {
        this->tick_election();
    };
    lead_ = lead;
    state_ = RaftState::Follower;

    LOG_INFO("%lu became follower at term %lu", id_, term_);
}

void Raft::become_candidate()
{
    LOG_WARN("no impl yet");
}

void Raft::become_pre_candidate()
{
    LOG_WARN("no impl yet");
}

void Raft::become_leader()
{
    LOG_WARN("no impl yet");
}

void Raft::campaign(const std::string& campaign_type)
{
    uint64_t term = 0;
    proto::MessageType vote_msg = 0;
    if (campaign_type == kCampaignPreElection) {
        become_pre_candidate();
        vote_msg = proto::MsgPreVote;
        // PreVote RPCs are sent for the next term before we've incremented r.Term.
        term = term_ + 1;
    }
    else {
        become_candidate();
        vote_msg = proto::MsgVote;
        term = term_;
    }

    if (quorum() == poll(id_, vote_msg, true)) {
        // We won the election after voting for ourselves (which must mean that
        // this is a single-node cluster). Advance to the next state.
        if (campaign_type == kCampaignPreElection) {
            campaign(kCampaignElection);
        }
        else {
            become_leader();
        }
        return;
    }

    for (auto it = prs_.begin(); it != prs_.end(); ++it) {
        if (it->first == id_) {
            continue;
        }

        LOG_INFO("%lu [logterm: %lu, index: %lu] sent %d request to %lu at term %lu",
                 id_, raft_log_->last_term(), raft_log_->last_index(), vote_msg, it->first, term_);

        std::vector<uint8_t> ctx;
        if (campaign_type == kCampaignTransfer) {
            ctx = std::vector<uint8_t>(kCampaignTransfer.begin(), kCampaignTransfer.end());
        }
        proto::MessagePtr msg(new proto::Message());
        msg->term = term;
        msg->to = it->first;
        msg->type = vote_msg;
        msg->index = raft_log_->last_index();
        msg->term = raft_log_->last_term();
        msg->context = std::move(ctx);

        send(std::move(msg));
    }
}

uint32_t Raft::poll(uint64_t id, proto::MessageType type, bool v)
{
    uint32_t granted = 0;
    if (v) {
        LOG_INFO("%lu received %d from %lu at term %lu", id_, type, id, term_);
    }
    else {
        LOG_INFO("%lu received %d rejection from %lu at term %lu", id_, type, id, term_);
    }

    auto it = votes_.find(id);
    if (it == votes_.end()) {
        votes_[id] = v;
    }

    for (it = votes_.begin(); it != votes_.end(); ++it) {
        if (it->second) {
            granted++;
        }
    }
    return granted;
}

Status Raft::step(proto::MessagePtr msg)
{
    if (term_ == 0) {
        LOG_INFO("local msg");
    }
    else if (msg->term > term_) {

    }
    else if (msg->term < term_) {

    }
    else {
        assert(term_ == msg->term);
        return Status::ok();
    }

    switch (msg->type) {
    case proto::MsgHup: {
        if (state_ != RaftState::Leader) {
            std::vector<proto::EntryPtr> entries;
            Status status =
                raft_log_->slice(raft_log_->applied() + 1, raft_log_->committed() + 1, RaftLog::unlimited(), entries);
            if (!status.is_ok()) {
                LOG_FATAL("unexpected error getting unapplied entries (%s)", status.to_string().c_str());
            }

            uint32_t pending = num_of_pending_conf(entries);
            if (pending > 0 && raft_log_->committed() > raft_log_->applied()) {
                LOG_WARN(
                    "%lu cannot campaign at term %lu since there are still %u pending configuration changes to apply",
                    id_,
                    term_,
                    pending);
                return Status::ok();
            }
            LOG_INFO("%lu is starting a new election at term %lu", id_, term_);
            if (pre_vote_) {
                campaign(kCampaignPreElection);
            }
            else {
                campaign(kCampaignElection);
            }
        }
        else {
            LOG_DEBUG("%lu ignoring MsgHup because already leader", id_);
        }
        break;
    }
    default: {
        return step_(msg);
    }
    }

    return Status::ok();
}

Status Raft::step_leader(proto::MessagePtr msg)
{
    LOG_WARN("no impl yet");
    return Status::ok();
}

Status Raft::step_candidate(proto::MessagePtr msg)
{
    LOG_WARN("no impl yet");
    return Status::ok();
}

void Raft::send(proto::MessagePtr msg)
{
    msg->from = id_;
    if (msg->type == proto::MsgVote || msg->type == proto::MsgVoteResp || msg->type == proto::MsgPreVote
        || msg->type == proto::MsgPreVoteResp) {
        if (msg->term == 0) {
            // All {pre-,}campaign messages need to have the term set when
            // sending.
            // - MsgVote: m.Term is the term the node is campaigning for,
            //   non-zero as we increment the term when campaigning.
            // - MsgVoteResp: m.Term is the new r.Term if the MsgVote was
            //   granted, non-zero for the same reason MsgVote is
            // - MsgPreVote: m.Term is the term the node will campaign,
            //   non-zero as we use m.Term to indicate the next term we'll be
            //   campaigning for
            // - MsgPreVoteResp: m.Term is the term received in the original
            //   MsgPreVote if the pre-vote was granted, non-zero for the
            //   same reasons MsgPreVote is
            LOG_FATAL("term should be set when sending %d", msg->type);
        }
    }
    else {
        if (msg->term != 0) {
            LOG_FATAL("term should not be set when sending %d (was %lu)", msg->type, msg->term);
        }
        // do not attach term to MsgProp, MsgReadIndex
        // proposals are a way to forward to the leader and
        // should be treated as local message.
        // MsgReadIndex is also forwarded to leader.
        if (msg->type != proto::MsgProp && msg->type != proto::MsgReadIndex) {
            msg->term = term_;
        }
    }
    msgs_.push_back(std::move(msg));
}

void Raft::restore_node(std::vector<uint64_t> nodes, bool is_learner)
{
    LOG_WARN("no impl yet");
}

bool Raft::promotable() const
{
    auto it = prs_.find(id_);
    return it != prs_.end();
}

void Raft::add_node_or_learner(uint64_t id, bool is_learner)
{
    ProgressPtr pr = get_progress(id);
    if (pr == nullptr) {
        set_progress(id, 0, raft_log_->last_index() + 1, is_learner);
    }
    else {

        if (is_learner && !pr->is_learner) {
            // can only change Learner to Voter
            LOG_INFO("%lu ignored addLearner: do not support changing %lu from raft peer to learner.", id_, id);
            return;
        }

        if (is_learner == pr->is_learner) {
            // Ignore any redundant addNode calls (which can happen because the
            // initial bootstrapping entries are applied twice).
            return;
        }

        // change Learner to Voter, use origin Learner progress
        learner_prs_.erase(id);
        pr->is_learner = false;
        prs_[id] = pr;
    }

    if (id_ == id) {
        is_learner_ = is_learner;
    }

    // When a node is first added, we should mark it as recently active.
    // Otherwise, CheckQuorum may cause us to step down if it is invoked
    // before the added node has a chance to communicate with us.
    get_progress(id)->recent_active = true;
}

void Raft::remove_node(uint64_t id)
{
    LOG_WARN("no impl yet");
}

Status Raft::step_follower(proto::MessagePtr msg)
{
    LOG_INFO("step follower");
    return Status::ok();
}

void Raft::handle_append_entries(proto::MessagePtr msg)
{
    LOG_WARN("no impl yet");
}

void Raft::handle_heartbeat(proto::MessagePtr msg)
{
    LOG_WARN("no impl yet");
}

bool Raft::restore(proto::SnapshotPtr snapshot)
{
    LOG_WARN("no impl yet");
    return true;
}

void Raft::tick()
{
    if (tick_) {
        tick_();
    }
    else {
        //LOG_WARN("tick function is not set");
    }
}

SoftStatePtr Raft::soft_state() const
{
    return std::make_shared<SoftState>(lead_, state_);
}

proto::HardState Raft::hard_state() const
{
    proto::HardState hs;
    hs.term = term_;
    hs.vote = vote_;
    hs.commit = raft_log_->committed();
    return hs;
}

void Raft::load_state(const proto::HardState& state)
{
    if (state.commit < raft_log_->committed() || state.commit > raft_log_->last_index()) {
        LOG_FATAL("%lu state.commit %lu is out of range [%lu, %lu]",
                  id_,
                  state.commit,
                  raft_log_->committed(),
                  raft_log_->last_index());
    }
    raft_log_->committed() = state.commit;
    term_ = state.term;
    vote_ = state.vote;
}

void Raft::nodes(std::vector<uint64_t>& node) const
{
    for (auto it = prs_.begin(); it != prs_.end(); ++it) {
        node.push_back(it->first);
    }
    std::sort(node.begin(), node.end());
}

void Raft::learner_nodes(std::vector<uint64_t>& learner) const
{
    for (auto it = learner_prs_.begin(); it != prs_.end(); ++it) {
        learner.push_back(it->first);
    }
    std::sort(learner.begin(), learner.end());
}

ProgressPtr Raft::get_progress(uint64_t id)
{
    auto it = prs_.find(id);
    if (it != prs_.end()) {
        return it->second;
    }

    it = learner_prs_.find(id);
    if (it != learner_prs_.end()) {
        return it->second;
    }
    return nullptr;
}

void Raft::set_progress(uint64_t id, uint64_t match, uint64_t next, bool is_learner)
{
    if (!is_learner) {
        learner_prs_.erase(id);
        ProgressPtr progress(new Progress(max_inflight_));
        progress->next = next;
        progress->match = match;
        prs_[id] = progress;
        return;
    }

    auto it = prs_.find(id);
    if (it != prs_.end()) {
        LOG_FATAL("%lu unexpected changing from voter to learner for %lu", id_, id);
    }

    ProgressPtr progress(new Progress(max_inflight_));
    progress->next = next;
    progress->match = match;
    progress->is_learner = true;

    learner_prs_[id] = progress;
}

void Raft::del_progress(uint64_t id)
{
    prs_.erase(id);
    learner_prs_.erase(id);
}

void Raft::send_append(uint64_t to)
{
    maybe_send_append(to, true);
}

bool Raft::maybe_send_append(uint64_t to, bool send_if_empty)
{
    LOG_WARN("no impl yet");
    return true;
}

void Raft::send_heartbeat(uint64_t to, std::vector<uint8_t> ctx)
{
    LOG_WARN("no impl yet");
}

void Raft::for_each_progress(const std::function<void(uint64_t, ProgressPtr&)>& callback)
{
    for (auto it = prs_.begin(); it != prs_.end(); ++it) {
        callback(it->first, it->second);
    }

    for (auto it = learner_prs_.begin(); it != learner_prs_.end(); ++it) {
        callback(it->first, it->second);
    }
}

void Raft::bcast_append()
{
    for_each_progress([this](uint64_t id, ProgressPtr& progress) {
        if (id == id_) {
            return;
        }

        this->send_append(id);
    });
}

void Raft::bcast_heartbeat()
{
    LOG_WARN("no impl yet");
}

void Raft::bcast_heartbeat_with_ctx(std::vector<uint8_t> ctx)
{
    for_each_progress([this, ctx](uint64_t id, ProgressPtr& progress) {
        if (id == id_) {
            return;
        }

        this->send_heartbeat(id, std::move(ctx));
    });
}

bool Raft::maybe_commit()
{
    LOG_WARN("no impl yet");
    return true;
}

void Raft::reset(uint64_t term)
{
    if (term_ != term) {
        term_ = term;
        vote_ = 0;
    }
    lead_ = 0;

    election_elapsed_ = 0;
    heartbeat_elapsed_ = 0;
    reset_randomized_election_timeout();

    abort_leader_transfer();

    votes_.clear();
    for_each_progress([this](uint64_t id, ProgressPtr& progress) {
        bool is_learner = progress->is_learner;
        progress = std::make_shared<Progress>(max_inflight_);
        progress->next = raft_log_->last_index();
        progress->is_learner = is_learner;

        if (id == id_) {
            progress->match = raft_log_->last_index();
        }

    });

    pending_conf_index_ = 0;
    uncommitted_size_ = 0;
    read_only_->pending_read_index.clear();
    read_only_->read_index_queue.clear();
}

void Raft::add_node(uint64_t id)
{
    add_node_or_learner(id, false);
}

bool Raft::append_entry(std::vector<proto::EntryPtr> entries)
{
    LOG_WARN("no impl yet");
    return true;
}

void Raft::tick_election()
{
    election_elapsed_++;

    if (promotable() && past_election_timeout()) {
        election_elapsed_ = 0;
        proto::MessagePtr msg(new proto::Message());
        msg->from = id_;
        msg->type = proto::MsgHup;
        step(std::move(msg));
    }
}

void Raft::tick_heartbeat()
{
    LOG_WARN("no impl yet");
}

bool Raft::past_election_timeout()
{
    return election_elapsed_ >= randomized_election_timeout_;
}

void Raft::reset_randomized_election_timeout()
{
    randomized_election_timeout_ = election_timeout_ + random_device_.gen();
    assert(randomized_election_timeout_ <= 2 * election_timeout_);
}

bool Raft::check_quorum_active() const
{
    LOG_WARN("no impl yet");
    return true;
}

void Raft::send_timeout_now(uint64_t to)
{
    LOG_WARN("no impl yet");
}

void Raft::abort_leader_transfer()
{
    lead_transferee_ = 0;
}

bool Raft::increase_uncommitted_size(std::vector<proto::EntryPtr> entries)
{
    LOG_WARN("no impl yet");
    return true;
}

void Raft::reduce_uncommitted_size(const std::vector<proto::EntryPtr>& entries)
{
    LOG_WARN("no impl yet");
}

}