#ifndef HTTP_PIPELINE_HPP
#define HTTP_PIPELINE_HPP

#include "HttpParser.hpp"
#include "HttpRouter.hpp"
#include "Reactor.hpp"
#include "ReactorIo.hpp"

#include <exception>
#include <string>

// 一个连接上的「请求流水线」：
//   解析 inbuf → 路由 → 要么本地直接回，要么丢进业务池 → 结果回来后拼进 outbuf
// 同样只会在所属 reactor 线程里执行。

inline void Reactor::processBufferedRequests(Connection& connection) {
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

inline void Reactor::processAndFlush(Connection& connection) {
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

inline void Reactor::queueErrorAndClose(Connection& connection, int status,
                                        const char* reason) {
    const std::string body = std::string(reason) + "\n";
    connection.append(http_router::buildResponse(
        status, reason, "text/plain; charset=utf-8", body, true));
    connection.close_after_write = true;
    connection.inbuf.clear();
}

inline void Reactor::submitSearch(Connection& connection, std::string word,
                                  bool close_after) {
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

#endif  // HTTP_PIPELINE_HPP
