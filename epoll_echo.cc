// epoll_echo.cc —— 阶段 1：单线程 epoll(ET) + 非阻塞 IO 的 echo server
//
// 编译：g++ -std=c++11 -O2 -Wall -o epoll_echo epoll_echo.cc
// 运行：./epoll_echo [port]          （默认 8082）
//
// 已知缺陷（阶段 2 来修）：非阻塞 send 部分写后尚无 EPOLLOUT 写缓冲。

#include "CommandServer.hpp"

#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    int port = 8082;
    if (argc > 1) {
        char* end = nullptr;
        const long parsed = std::strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || parsed < 1 || parsed > 65535) {
            std::cerr << "invalid port: " << argv[1] << " (expected 1-65535)\n";
            return 2;
        }
        port = static_cast<int>(parsed);
    }

    if (::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        std::cerr << "failed to ignore SIGPIPE\n";
        return 1;
    }

    try {
        CommandServer server(port);
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "[fatal] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
