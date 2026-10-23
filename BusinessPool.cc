#include "BusinessPool.hpp"

#include <utility>

BusinessPool::BusinessPool(size_t threads) : active_(0), stop_(false) {
    threads_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
        threads_.emplace_back([this]() { worker(); });
    }
}

BusinessPool::~BusinessPool() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    for (size_t i = 0; i < threads_.size(); ++i) {
        if (threads_[i].joinable()) threads_[i].join();
    }
}

void BusinessPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

size_t BusinessPool::pending() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return tasks_.size();
}

bool BusinessPool::waitIdle(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mtx_);
    return cv_idle_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this]() { return tasks_.empty() && active_ == 0; });
}

void BusinessPool::worker() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
            ++active_;
        }
        // ⚠️ 必须在锁外执行，否则整个池退化成单线程
        task();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            --active_;
        }
        cv_idle_.notify_all();
    }
}
