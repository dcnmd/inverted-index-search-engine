#ifndef COMMAND_SERVER_HPP
#define COMMAND_SERVER_HPP

#include "BusinessPool.hpp"
#include "Connection.hpp"
#include "Reactor.hpp"
#include "TcpSocket.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// 阶段 4 · main reactor：
//   - 只监听 listen fd、accept 新连接、round-robin 分发给 sub reactor
//   - 不读不写任何客户端数据，因此这里没有慢查询阻塞的可能
class CommandServer {
public:
    CommandServer(int port, size_t reactor_count, BusinessPool* pool,
                  SearchHandler search_handler);
    ~CommandServer();

    CommandServer(const CommandServer&) = delete;
    CommandServer& operator=(const CommandServer&) = delete;

    void run();

private:
    void acceptConnections();
    void closeResources();

    int port_;
    TcpSocket listener_;
    int epoll_fd_;
    BusinessPool* pool_;
    uint32_t next_conn_id_;
    size_t next_reactor_;
    std::vector<std::unique_ptr<Reactor>> reactors_;
};

#endif  // COMMAND_SERVER_HPP
