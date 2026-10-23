// epoll_echo.cc —— 阶段 4：主从 Reactor + 业务线程池 + 空闲连接超时
//
// 编译：make epoll_echo
// 运行：./epoll_echo [port]          （在仓库根目录运行，默认 8082）
//
// 线程数 = main reactor(1) + sub reactor(2) + 业务线程池(4) = 7

#include "CommandServer.hpp"
#include "Ns_Util.hpp"
#include "sercher.hpp"

#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

cppjieba::Jieba NsUtil::jiebautil::jieba(
    "cppjieba/dict/jieba.dict.utf8",
    "cppjieba/dict/hmm_model.utf8",
    "cppjieba/dict/user.dict.utf8",
    "cppjieba/dict/idf.utf8",
    "cppjieba/dict/stop_words.utf8");

namespace {
const char* const kIndexInput = "data/pow_html/output.txt";
const size_t kSubReactors = 2;      // sub reactor 数量：按 CPU 核数取（这台机器 2 核）
const size_t kBusinessThreads = 4;  // 业务线程池：检索在池里做，不占 IO 线程
}  // namespace

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
        std::cerr << "[init] building index from " << kIndexInput << " ...\n";
        ns_search::Searcher searcher;
        searcher.InitSearcher(kIndexInput);

        BusinessPool pool(kBusinessThreads);
        SearchHandler search_handler = [&searcher](const std::string& word) {
            std::string json;
            searcher.Search(word, &json);
            return json;
        };

        // pool 的生命周期必须长于 server：CommandServer 析构时会先等池里的任务回投完
        CommandServer server(port, kSubReactors, &pool, search_handler);
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "[fatal] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
