#include "CommandServer.hpp"

#include <sys/epoll.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {
const int kMaxEvents = 512;
const int kBacklog = 128;
}  // namespace

CommandServer::CommandServer(int port, size_t reactor_count, BusinessPool* pool,
                             SearchHandler search_handler)
    : port_(port),
      epoll_fd_(-1),
      pool_(pool),
      next_conn_id_(1),
      next_reactor_(0) {
    if (reactor_count == 0) {
        throw std::invalid_argument("CommandServer: reactor_count must be >= 1");
    }
    if (pool_ == nullptr) {
        throw std::invalid_argument("CommandServer: pool must not be null");
    }

    try {
        listener_ = TcpSocket::listenOn(port, kBacklog);

        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            throw socket_detail::makeSystemError("epoll_create1", errno);
        }

        epoll_event event{};
        event.events = EPOLLIN | EPOLLET;
        event.data.u64 = 0;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listener_.nativeHandle(), &event) < 0) {
            throw socket_detail::makeSystemError("epoll_ctl(ADD listener)", errno);
        }

        for (size_t i = 0; i < reactor_count; ++i) {
            std::unique_ptr<Reactor> reactor(new Reactor(i, pool_, search_handler));
            reactor->start();
            reactors_.push_back(std::move(reactor));
        }
    } catch (...) {
        closeResources();
        throw;
    }
}

CommandServer::~CommandServer() {
    closeResources();
}

// main reactor 事件循环：只有 listener 一个 fd，阻塞等待即可
void CommandServer::run() {
    std::cerr << "[epoll_http] listening on 0.0.0.0:" << port_
              << " (HTTP/1.1, epoll ET, master + " << reactors_.size()
              << " sub reactors, business pool shared)\n";

    epoll_event events[kMaxEvents];
    for (;;) {
        const int ready = epoll_wait(epoll_fd_, events, kMaxEvents, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            throw socket_detail::makeSystemError("epoll_wait", errno);
        }
        for (int i = 0; i < ready; ++i) {
            acceptConnections();
        }
    }
}

// 接受新连接并交给某个 sub reactor；这里不做 epoll_ctl(ADD)（fd 一旦可读，
// sub reactor 必须先认识它，见 Reactor::drainHandoffs）
void CommandServer::acceptConnections() {
    for (;;) {
        TcpSocket client;
        sockaddr_in peer{};
        // accept 内部处理 EAGAIN（返回 false）和 EMFILE（腾 fd 兜底后继续）
        if (!listener_.accept(client, peer)) return;

#ifdef COMMAND_SERVER_TEST_LARGE_RESPONSE
        const int test_send_buffer = 4096;
        client.setSendBufferSize(test_send_buffer);
#endif

        char address[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address)) == nullptr) {
            std::strncpy(address, "unknown", sizeof(address) - 1);
        }

        std::shared_ptr<Connection> connection(new Connection());
        connection->id = next_conn_id_;
        ++next_conn_id_;
        if (next_conn_id_ == kWakeupTag) next_conn_id_ = 1;  // 0 保留给 eventfd
        connection->socket = std::move(client);
        connection->peer = std::string(address) + ":" + std::to_string(ntohs(peer.sin_port));
        connection->last_active = std::chrono::steady_clock::now();

        Reactor& target = *reactors_[next_reactor_];
        next_reactor_ = (next_reactor_ + 1) % reactors_.size();

        std::cerr << "[accept] fd=" << connection->fd() << " " << connection->peer
                  << " -> reactor " << target.index() << '\n';
        target.handoff(std::move(connection));
    }
}

void CommandServer::closeResources() {
    // 先等业务线程把手上的检索跑完（结果回投给仍然活着的 reactor），
    // 再停 reactor —— 否则队列里的任务会持有已销毁的 Reactor*
    if (pool_ != nullptr) pool_->waitIdle(2000);
    for (size_t i = 0; i < reactors_.size(); ++i) {
        reactors_[i]->stop();
    }
    reactors_.clear();
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
    listener_.close();
}
