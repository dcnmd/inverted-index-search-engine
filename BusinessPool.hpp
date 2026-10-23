#ifndef BUSINESS_POOL_HPP
#define BUSINESS_POOL_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// 最简固定大小线程池：只有 submit()，没有动态扩缩容。
// 任务函数里禁止碰 socket fd / epoll fd，只允许纯计算 + 回投结果。
class BusinessPool {
public:
    explicit BusinessPool(size_t threads);
    ~BusinessPool();

    BusinessPool(const BusinessPool&) = delete;
    BusinessPool& operator=(const BusinessPool&) = delete;

    void submit(std::function<void()> task);
    size_t pending() const;
    // 等待队列任务全部执行完（带超时），用于关停时避免任务持有已销毁的 Reactor*
    bool waitIdle(int timeout_ms);

private:
    void worker();

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::condition_variable cv_idle_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> threads_;
    size_t active_;
    bool stop_;
};

#endif  // BUSINESS_POOL_HPP
