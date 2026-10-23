#ifndef REACTOR_HPP
#define REACTOR_HPP

#include "BusinessPool.hpp"
#include "Connection.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

constexpr size_t kMaxEpollEvents = 1024;
constexpr int kTickMs = 500;         // 定时器 tick：每 500ms 扫一遍空闲连接
constexpr uint32_t kWakeupTag = 0;   // epoll data.u32 == 0 表示 eventfd，连接 id 从 1 开始
constexpr size_t kReadChunk = 65536;

// 从业务线程回投到 reactor 的结果：只带连接 id，绝不带 Connection*
struct PendingResult {
    uint32_t id;
    bool close_after;
    std::string response;
};

// sub reactor：一个线程 + 一个 epoll + 一张连接表。
// 对外的三个入口（handoff / postResult / stop）可以被任意线程调用，内部用 eventfd 唤醒。
class Reactor {
public:
    Reactor(size_t index, BusinessPool* pool, SearchHandler search);
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    void start();
    void stop();

    // ===== 可从任意线程调用 =====
    void handoff(std::shared_ptr<Connection> conn);             // main -> sub
    void postResult(uint32_t conn_id, std::string response,
                    bool close_after);                          // pool -> sub
    // ===========================

    size_t index() const { return index_; }
    size_t connectionCount() const { return conn_count_.load(); }

private:
    void loop();
    void wakeUp();
    void drainWakeup();
    void drainHandoffs();
    void drainResults();

    Connection* find(uint32_t id);

    void onReadable(Connection& connection);
    void onWritable(Connection& connection);
    bool flushOutput(Connection& connection);
    bool updateEvents(Connection& connection);

    void processBufferedRequests(Connection& connection);
    void processAndFlush(Connection& connection);
    void queueErrorAndClose(Connection& connection, int status, const char* reason);
    void submitSearch(Connection& connection, std::string word, bool close_after);

    void closeConnection(uint32_t id, const char* reason);
    void expireIdle(std::chrono::steady_clock::time_point now);

    size_t index_;
    BusinessPool* pool_;
    SearchHandler search_;
    int epoll_fd_;
    int wakeup_fd_;
    std::thread thread_;
    std::atomic<bool> running_;
    std::atomic<size_t> conn_count_;

    mutable std::mutex mtx_;
    std::vector<std::shared_ptr<Connection>> handoffs_;
    std::vector<PendingResult> results_;
    std::unordered_map<uint32_t, std::shared_ptr<Connection>> conns_;
};

#endif  // REACTOR_HPP
