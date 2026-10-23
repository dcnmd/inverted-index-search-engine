#include "Reactor.hpp"

#include "HttpPipeline.hpp"
#include "ReactorIo.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

// 本文件只负责三件事：
//   1. reactor 自身的生命周期（epoll / eventfd / 线程）
//   2. 跨线程投递队列（handoff / postResult）与 eventfd 唤醒
//   3. 事件循环与定时器（loop / drain* / expireIdle）
// 单连接的读写见 ReactorIo.hpp，请求解析与派活见 HttpPipeline.hpp。

// ---------------------------------------------------------------- 生命周期

Reactor::Reactor(size_t index, BusinessPool* pool, SearchHandler search)
    : index_(index),
      pool_(pool),
      search_(std::move(search)),
      epoll_fd_(-1),
      wakeup_fd_(-1),
      running_(true),
      conn_count_(0) {
    if (pool_ == nullptr) {
        throw std::invalid_argument("Reactor: pool must not be null");
    }

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw socket_detail::makeSystemError("epoll_create1", errno);
    }

    wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0) {
        const int error = errno;
        ::close(epoll_fd_);
        epoll_fd_ = -1;
        throw socket_detail::makeSystemError("eventfd", error);
    }

    epoll_event event{};
    event.events = EPOLLIN;  // eventfd 用水平触发，写满也不会漏
    event.data.u32 = kWakeupTag;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &event) < 0) {
        const int error = errno;
        ::close(wakeup_fd_);
        wakeup_fd_ = -1;
        ::close(epoll_fd_);
        epoll_fd_ = -1;
        throw socket_detail::makeSystemError("epoll_ctl(ADD wakeup)", error);
    }
}

Reactor::~Reactor() {
    stop();
    if (wakeup_fd_ >= 0) {
        ::close(wakeup_fd_);
        wakeup_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

void Reactor::start() {
    if (thread_.joinable()) return;
    thread_ = std::thread([this]() { loop(); });
}

void Reactor::stop() {
    running_.store(false);
    wakeUp();
    if (thread_.joinable()) thread_.join();
}

// ---------------------------------------------------------------- 跨线程入口

void Reactor::handoff(std::shared_ptr<Connection> conn) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        handoffs_.push_back(std::move(conn));
    }
    wakeUp();
}

void Reactor::postResult(uint32_t conn_id, std::string response, bool close_after) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        PendingResult result;
        result.id = conn_id;
        result.close_after = close_after;
        result.response = std::move(response);
        results_.push_back(std::move(result));
    }
    wakeUp();
}

void Reactor::wakeUp() {
    const uint64_t one = 1;
    const ssize_t written = ::write(wakeup_fd_, &one, sizeof(one));
    (void)written;
}

// ---------------------------------------------------------------- 事件循环

void Reactor::loop() {
    epoll_event events[kMaxEpollEvents];

    while (running_.load()) {
        const int ready = ::epoll_wait(epoll_fd_, events, kMaxEpollEvents, kTickMs);
        if (ready < 0) {
            if (errno == EINTR) continue;
            reportSystemError("epoll_wait", errno);
            break;
        }

        for (int i = 0; i < ready; ++i) {
            const uint32_t id = events[i].data.u32;
            if (id == kWakeupTag) {
                drainWakeup();
                continue;
            }

            const uint32_t flags = events[i].events;
            Connection* connection = find(id);
            if (connection == nullptr) continue;

            if (flags & EPOLLERR) {
                try {
                    const int socket_error = connection->socket.pendingError();
                    if (socket_error != 0) reportSystemError("client socket", socket_error);
                } catch (const std::system_error& error) {
                    reportSystemError("getsockopt(SO_ERROR)", error.code().value());
                }
                closeConnection(id, "EPOLLERR");
                continue;
            }

            if (flags & EPOLLOUT) {
                onWritable(*connection);
                connection = find(id);  // 可能已被关闭，重新查一次
                if (connection == nullptr) continue;
            }
            if (flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
                onReadable(*connection);
            }
        }

        expireIdle(std::chrono::steady_clock::now());
    }
}

void Reactor::drainWakeup() {
    uint64_t value = 0;
    for (;;) {
        const ssize_t count = ::read(wakeup_fd_, &value, sizeof(value));
        if (count > 0) continue;
        if (count < 0 && errno == EINTR) continue;
        break;  // EAGAIN：计数已经读干净
    }
    drainHandoffs();
    drainResults();
}

void Reactor::drainHandoffs() {
    std::vector<std::shared_ptr<Connection>> batch;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        batch.swap(handoffs_);
    }

    for (size_t i = 0; i < batch.size(); ++i) {
        std::shared_ptr<Connection>& conn = batch[i];
        // ⚠️ 只在这里 EPOLL_CTL_ADD。绝不能在 main reactor 线程里提前 ADD：
        // 否则 fd 可能先变可读，sub reactor 醒来时 conns_ 里还没有它，事件就丢了。
        epoll_event event{};
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        event.data.u32 = conn->id;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, conn->fd(), &event) < 0) {
            reportSystemError("epoll_ctl(ADD client)", errno);
            continue;  // shared_ptr 析构会关掉 fd
        }
        conn->last_active = std::chrono::steady_clock::now();
        conn_count_.fetch_add(1);
        conns_[conn->id] = conn;
    }
}

void Reactor::drainResults() {
    std::vector<PendingResult> batch;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        batch.swap(results_);
    }

    for (size_t i = 0; i < batch.size(); ++i) {
        std::unordered_map<uint32_t, std::shared_ptr<Connection>>::iterator found =
            conns_.find(batch[i].id);
        if (found == conns_.end()) continue;  // 连接已经关了，结果直接丢弃

        Connection& connection = *found->second;
        connection.busy = false;
        // 对端不读：未发数据已经积压超过上限，不再排入新响应，直接断开
        if (connection.pendingBytes() > kMaxOutbuf) {
            closeConnection(connection.id, "output buffer overflow");
            continue;
        }
        connection.append(batch[i].response);
        ++connection.requests_served;
        if (batch[i].close_after) connection.close_after_write = true;
        connection.last_active = std::chrono::steady_clock::now();
        processAndFlush(connection);
    }
}

Connection* Reactor::find(uint32_t id) {
    const std::unordered_map<uint32_t, std::shared_ptr<Connection>>::iterator found =
        conns_.find(id);
    return found == conns_.end() ? nullptr : found->second.get();
}

// ---------------------------------------------------------------- 定时器

void Reactor::expireIdle(std::chrono::steady_clock::time_point now) {
    std::vector<uint32_t> dead;
    for (std::unordered_map<uint32_t, std::shared_ptr<Connection>>::const_iterator it =
             conns_.begin();
         it != conns_.end(); ++it) {
        const Connection& connection = *it->second;
        if (connection.busy) continue;        // 正在算，不算空闲
        if (connection.wantWrite()) continue;  // 还有数据没发完
        const long long idle =
            std::chrono::duration_cast<std::chrono::seconds>(now - connection.last_active)
                .count();
        if (idle >= kIdleTimeoutSec) dead.push_back(it->first);
    }
    // ⚠️ 必须先收集再关：closeConnection 会 erase，range-for 里 erase 是 UB
    for (size_t i = 0; i < dead.size(); ++i) {
        closeConnection(dead[i], "idle timeout");
    }
}

// ---------------------------------------------------------------- 工具

void Reactor::reportSystemError(const char* operation, int error) {
    std::cerr << operation << ": "
              << std::error_code(error, std::generic_category()).message()
              << " (errno=" << error << ")\n";
}
