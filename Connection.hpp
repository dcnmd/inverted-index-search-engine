#ifndef CONNECTION_HPP
#define CONNECTION_HPP

#include "TcpSocket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// 阶段 4：所有超时/上限使用同一组常量，响应头里的 Keep-Alive 声明与定时器保持一致
constexpr size_t kMaxInbuf = 16384;         // 单连接待解析数据上限
constexpr size_t kMaxOutbuf = 1024 * 1024;  // 未发出数据上限，超了就主动断开
constexpr int kIdleTimeoutSec = 5;          // 空闲回收阈值（秒）
constexpr uint32_t kMaxRequestsPerConn = 100;

// 业务函数：只做检索并返回 JSON 正文，绝不碰 socket / epoll
typedef std::function<std::string(const std::string& word)> SearchHandler;

// 一个连接从 accept 起就固定属于某个 sub reactor，其读写只在该 reactor 线程完成，
// 因此字段本身不需要加锁；跨线程只传 id。
struct Connection {
    uint32_t id = 0;
    TcpSocket socket;
    std::string inbuf;
    std::string outbuf;
    size_t outpos = 0;
    std::string peer;
    uint32_t requests_served = 0;
    bool close_after_write = false;
    bool eof = false;
    bool busy = false;  // 已有一个检索在 BusinessPool 里跑
    std::chrono::steady_clock::time_point last_active;

    int fd() const { return socket.nativeHandle(); }

    size_t pendingBytes() const { return outbuf.size() - outpos; }
    bool wantWrite() const { return outpos < outbuf.size(); }

    void append(const std::string& data) { outbuf += data; }
    void clearOut() {
        outbuf.clear();
        outpos = 0;
    }
};

#endif  // CONNECTION_HPP
