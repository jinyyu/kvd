#pragma once
#include <string>
#include <stdint.h>
#include <memory>
#include <vector>
#include <raft-kv/transport/Transport.h>
#include <raft-kv/raft/Node.h>
#include <raft-kv/server/RedisServer.h>
#include <raft-kv/wal/WAL.h>

namespace kv
{

typedef std::function<void(const Status&)> StatusCallback;

class KvServer: public RaftServer, public std::enable_shared_from_this<KvServer>
{
public:
    static void main(uint64_t id, const std::string& cluster, uint16_t port);

    explicit KvServer(uint64_t id, const std::string& cluster, uint16_t port);

    virtual ~KvServer();

    void stop();

    void propose(std::shared_ptr<std::vector<uint8_t>> data, const StatusCallback& callback);

    virtual void process(proto::MessagePtr msg, const StatusCallback& callback);

    virtual void is_id_removed(uint64_t id, const std::function<void(bool)>& callback);

    virtual void report_unreachable(uint64_t id);

    virtual void report_snapshot(uint64_t id, SnapshotStatus status);

    bool publish_entries(const std::vector<proto::EntryPtr>& entries);
    void entries_to_apply(const std::vector<proto::EntryPtr>& entries, std::vector<proto::EntryPtr>& ents);
    void maybe_trigger_snapshot();

private:
    void start_timer();
    void check_raft_ready();
    void schedule();

    uint16_t port_;
    pthread_t pthread_id_;
    boost::asio::io_service io_servie_;
    boost::asio::deadline_timer timer_;
    uint64_t id_;
    std::vector<std::string> peers_;
    std::string wal_dir_;
    uint64_t last_index_;
    proto::ConfStatePtr conf_state_;
    uint64_t snapshot_index_;
    uint64_t applied_index_;

    RawNodePtr node_;
    TransporterPtr transport_;
    MemoryStoragePtr storage_;
    std::shared_ptr<RedisServer> redis_server_;
};
typedef std::shared_ptr<KvServer> KvdServerPtr;

}