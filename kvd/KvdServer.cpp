#include "kvd/KvdServer.h"
#include "kvd/transport/AsioTransport.h"
#include "kvd/common/log.h"
#include <boost/algorithm/string.hpp>

namespace kvd
{

KvdServer::KvdServer(uint64_t id, const std::string &cluster, uint16_t port)
    : id_(id)
{
    boost::split(peers_, cluster, boost::is_any_of(","));
    if (peers_.empty()) {
        LOG_DEBUG("invalid args %s", cluster.c_str());
        exit(0);
    }
}

KvdServer::~KvdServer()
{
    LOG_DEBUG("stopped");
    if (transport_) {
        transport_->stop();
        transport_ = nullptr;
    }
}

void KvdServer::schedule()
{

}

Status KvdServer::process(proto::MessagePtr msg)
{
    LOG_DEBUG("no impl yet");
    return Status::ok();
}

bool KvdServer::is_id_removed(uint64_t id)
{
    LOG_DEBUG("no impl yet");
    return false;
}

void KvdServer::report_unreachable(uint64_t id)
{
    LOG_DEBUG("no impl yet");
}

void KvdServer::report_snapshot(uint64_t id, SnapshotStatus status)
{
    LOG_DEBUG("no impl yet");
}

static KvdServerPtr g_node = nullptr;

void on_signal(int)
{
    if (g_node) {
        g_node->stop();
    }
}

void KvdServer::main(uint64_t id, const std::string &cluster, uint16_t port)
{
    g_node = std::make_shared<KvdServer>(id, cluster, port);

    AsioTransport *transport = new AsioTransport(g_node, g_node->id_);

    TransporterPtr ptr((Transporter *) transport);
    ptr->start();
    g_node->transport_ = ptr;

    for (uint64_t i = 0; i < g_node->peers_.size(); ++i) {
        uint64_t peer = i + 1;
        if (peer == g_node->id_) {
            continue;
        }
        ptr->add_peer(peer, g_node->peers_[i]);
    }
    g_node->schedule();
    sleep(3);
}

void KvdServer::stop()
{
    LOG_DEBUG("stopping");
    if (transport_) {
        transport_->stop();
        transport_ = nullptr;
    }

    io_service_.stop();

}

}
