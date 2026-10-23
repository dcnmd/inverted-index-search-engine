#include "Reactor.hpp"

#include "HttpParser.hpp"
#include "HttpRouter.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {
void reportSystemError(const char* operation, int error) {
    std::cerr << operation << ": "
              << std::error_code(error, std::generic_category()).message()
              << " (errno=" << error << ")\n";
}
}  // namespace

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

// ---------------------------------------------------------------- 单连接处理

Connection* Reactor::find(uint32_t id) {
    const std::unordered_map<uint32_t, std::shared_ptr<Connection>>::iterator found =
        conns_.find(id);
    return found == conns_.end() ? nullptr : found->second.get();
}

void Reactor::onReadable(Connection& connection) {
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

void Reactor::onWritable(Connection& connection) {
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

bool Reactor::flushOutput(Connection& connection) {
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

bool Reactor::updateEvents(Connection& connection) {
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

void Reactor::processBufferedRequests(Connection& connection) {
    while (!connection.close_after_write && !connection.busy) {
        http_protocol::Request request;
        size_t consumed = 0;
        const http_protocol::ParseResult result =
            http_protocol::HttpParser::tryParseRequest(connection.inbuf, request, consumed);

        if (result == http_protocol::ParseResult::NeedMore) return;
        if (result == http_protocol::ParseResult::TooLarge) {
            queueErrorAndClose(connection, 431, "Request Header Fields Too Large");
            return;
        }
        if (result == http_protocol::ParseResult::Bad) {
            queueErrorAndClose(connection, 400, "Bad Request");
            return;
        }

        connection.inbuf.erase(0, consumed);
        const bool close =
            request.close ||
            connection.requests_served + 1 >= kMaxRequestsPerConn ||
            (connection.eof && connection.inbuf.empty());

        const http_router::RouteResult routed = http_router::route(request, close);
        if (routed.need_search) {
            if (!search_) {
                connection.append(http_router::buildResponse(
                    503, "Service Unavailable", "text/plain",
                    "search backend not configured", true));
                connection.close_after_write = true;
                return;
            }
            submitSearch(connection, routed.word, routed.close);
            return;  // 请求已消费，inbuf 里剩下的等结果回来再处理
        }

        // 客户端只发不读时 outbuf 会一直长，超限就丢弃未发数据并断开
        if (connection.pendingBytes() > kMaxOutbuf) {
            connection.clearOut();
            connection.inbuf.clear();
            connection.close_after_write = true;
            return;
        }

        connection.append(routed.response);
        ++connection.requests_served;
        if (routed.close) connection.close_after_write = true;
    }
}

void Reactor::processAndFlush(Connection& connection) {
    processBufferedRequests(connection);

    if (!connection.busy && connection.eof && !connection.close_after_write) {
        if (!connection.inbuf.empty()) {
            queueErrorAndClose(connection, 400, "Bad Request");
        } else {
            connection.close_after_write = true;
        }
    }

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

void Reactor::queueErrorAndClose(Connection& connection, int status, const char* reason) {
    const std::string body = std::string(reason) + "\n";
    connection.append(http_router::buildResponse(
        status, reason, "text/plain; charset=utf-8", body, true));
    connection.close_after_write = true;
    connection.inbuf.clear();
}

void Reactor::submitSearch(Connection& connection, std::string word, bool close_after) {
    connection.busy = true;
    const uint32_t id = connection.id;
    Reactor* self = this;

    // ⚠️ 只捕获 id / word / self。绝不捕获 Connection* 或 conns_ 的引用：
    // 业务线程执行期间连接可能已被回收，传指针就是悬垂引用。
    pool_->submit([self, id, word, close_after]() {
        std::string body;
        try {
            body = self->search_(word);
        } catch (const std::exception& error) {
            const std::string payload = std::string("search failed: ") + error.what();
            self->postResult(id,
                             http_router::buildResponse(500, "Internal Server Error",
                                                        "text/plain; charset=utf-8",
                                                        payload, true),
                             true);
            return;
        }
        self->postResult(id,
                         http_router::buildResponse(200, "OK",
                                                    "application/json; charset=utf-8",
                                                    body, close_after),
                         close_after);
    });
}

// ---------------------------------------------------------------- 回收

void Reactor::closeConnection(uint32_t id, const char* reason) {
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
