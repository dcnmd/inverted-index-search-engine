#ifndef REACTOR_IO_HPP
#define REACTOR_IO_HPP

#include "Reactor.hpp"

#include <sys/epoll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <string>

// 单连接层面的 IO：读、写、事件注册、关闭连接。
// 这些函数只会在所属 reactor 线程里执行，所以不需要任何锁。
// 定义成 inline 是为了让 Reactor.cc 保持「只讲事件循环」，
// 同时不给构建系统增加额外的编译单元。

inline void Reactor::onReadable(Connection& connection) {
    char buffer[kReadChunk];
    bool eof = false;

    for (;;) {
        if (connection.inbuf.size() >= kMaxInbuf) {
            if (connection.busy) break;  // 等业务线程回投结果腾出空间
            processBufferedRequests(connection);
            if (connection.close_after_write) break;
            if (connection.inbuf.size() >= kMaxInbuf) {
                queueErrorAndClose(connection, 431, "Request Header Fields Too Large");
                break;
            }
        }

        const size_t available = kMaxInbuf - connection.inbuf.size();
        if (available == 0) break;
        const size_t want = std::min(available, sizeof(buffer));
        const ssize_t count = connection.socket.receive(buffer, want);
        if (count > 0) {
            connection.inbuf.append(buffer, static_cast<size_t>(count));
            connection.last_active = std::chrono::steady_clock::now();
            continue;
        }
        if (count == 0) {
            eof = true;
            break;
        }

        const int error = errno;
        if (error == EAGAIN || error == EWOULDBLOCK) break;
        if (error == EINTR) continue;
        reportSystemError("recv", error);
        closeConnection(connection.id, "recv error");
        return;
    }

    if (eof) connection.eof = true;
    processAndFlush(connection);
}

inline void Reactor::onWritable(Connection& connection) {
    if (!flushOutput(connection)) {
        closeConnection(connection.id, "send failed");
        return;
    }
    if (connection.close_after_write && !connection.wantWrite()) {
        closeConnection(connection.id, "response done, closing");
        return;
    }
    if (!updateEvents(connection)) {
        closeConnection(connection.id, "epoll_ctl(MOD) failed");
    }
}

inline bool Reactor::flushOutput(Connection& connection) {
    while (connection.wantWrite()) {
        const ssize_t count = connection.socket.send(
            connection.outbuf.data() + connection.outpos,
            connection.outbuf.size() - connection.outpos);
        if (count > 0) {
            connection.outpos += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 这里不断开：单个大响应（如 2MB 测试用例）在 ET + 小 SO_SNDBUF 下
            // 本来就会先攒满 outbuf 再逐步发出去，按“一次 EAGAIN”判死会误杀。
            // OOM 的防线在排入新响应的地方（pendingBytes() > kMaxOutbuf）。
            return true;
        }
        if (count < 0) reportSystemError("send", errno);
        return false;
    }
    connection.clearOut();
    return true;
}

inline bool Reactor::updateEvents(Connection& connection) {
    epoll_event event{};
    event.events = EPOLLET | EPOLLRDHUP;
    // busy 期间不再读（不重新 arm），等结果回投后恢复，避免 ET 下反复空转
    if (!connection.close_after_write && !connection.busy) event.events |= EPOLLIN;
    if (connection.wantWrite()) event.events |= EPOLLOUT;
    event.data.u32 = connection.id;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, connection.fd(), &event) < 0) {
        reportSystemError("epoll_ctl(MOD)", errno);
        return false;
    }
    return true;
}

inline void Reactor::closeConnection(uint32_t id, const char* reason) {
    const std::unordered_map<uint32_t, std::shared_ptr<Connection>>::iterator found =
        conns_.find(id);
    if (found == conns_.end()) return;

    const int fd = found->second->fd();
    const std::string peer = found->second->peer;

    const int result = ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    const int error = errno;
    if (result < 0 && error != ENOENT && error != EBADF) {
        reportSystemError("epoll_ctl(DEL)", error);
    }
    if (found->second->socket.close() < 0) {
        reportSystemError("close(client)", errno);
    }
    conns_.erase(found);
    conn_count_.fetch_sub(1);
    std::cerr << "[close] fd=" << fd << " " << peer << " (" << reason
              << ", conns=" << conn_count_.load() << ")\n";
}

#endif  // REACTOR_IO_HPP
