// epoll_echo.cc —— 阶段 3：单线程 epoll(ET) + BM25 HTTP/1.1 server
//
// 编译：make epoll_echo
// 运行：./epoll_echo [port]          （在仓库根目录运行，默认 8082）

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

        CommandServer server(port, [&searcher](const std::string& word) {
            std::string json;
            searcher.Search(word, &json);
            return json;
        });
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "[fatal] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
