#ifndef HTTP_ROUTER_HPP
#define HTTP_ROUTER_HPP

#include "Connection.hpp"
#include "HttpParser.hpp"

#include <string>
#include <unordered_map>

// 把「HTTP 语义」从「事件循环」里拆出来：纯函数，任何线程都能调。
// Reactor 在 IO 线程里调 route()，业务线程只调 buildResponse() 拼响应。
namespace http_router {

struct RouteResult {
    bool need_search = false;  // true 表示要把 word 丢给业务线程池
    bool close = false;        // 响应发完就关连接
    std::string word;          // need_search 时的检索词
    std::string response;      // need_search == false 时的完整响应
};

inline std::string buildResponse(int status,
                                 const char* reason,
                                 const std::string& content_type,
                                 const std::string& body,
                                 bool close,
                                 const std::string& extra_headers = std::string()) {
    const bool persistent = !close && status < 400;
    std::string response;
    response.reserve(body.size() + extra_headers.size() + 160);
    response += "HTTP/1.1 ";
    response += std::to_string(status);
    response += ' ';
    response += reason;
    response += "\r\nContent-Type: ";
    response += content_type;
    response += "\r\nContent-Length: ";
    response += std::to_string(body.size());
    if (persistent) {
        response += "\r\nKeep-Alive: timeout=";
        response += std::to_string(kIdleTimeoutSec);
        response += ", max=";
        response += std::to_string(kMaxRequestsPerConn);
    } else {
        response += "\r\nConnection: close";
    }
    response += "\r\n";
    response += extra_headers;
    response += "\r\n";
    response += body;
    return response;
}

// 只做「不需要检索」的路由判定；/s 返回 need_search 交给线程池。
inline RouteResult route(const http_protocol::Request& request, bool close) {
    RouteResult out;
    out.close = close;

    if (request.method != "GET") {
        out.close = true;
        out.response = buildResponse(405, "Method Not Allowed",
                                     "text/plain; charset=utf-8",
                                     "only GET supported\n", true, "Allow: GET\r\n");
        return out;
    }

    std::string path;
    std::string query;
    http_protocol::HttpParser::splitTarget(request.target, path, query);
    std::unordered_map<std::string, std::string> params;
    if (!http_protocol::HttpParser::parseQuery(query, params)) {
        out.close = true;
        out.response = buildResponse(400, "Bad Request", "text/plain; charset=utf-8",
                                     "invalid query encoding\n", true);
        return out;
    }

#ifdef COMMAND_SERVER_TEST_LARGE_RESPONSE
    if (path == "/__test/large") {
        out.response = buildResponse(200, "OK", "application/octet-stream",
                                     std::string(2 * 1024 * 1024, 'x'), out.close);
        return out;
    }
#endif

    if (path == "/s") {
        const std::unordered_map<std::string, std::string>::const_iterator word =
            params.find("word");
        if (word == params.end()) {
            out.close = true;
            out.response = buildResponse(400, "Bad Request", "text/plain",
                                         "missing word parameter", true);
            return out;
        }
        out.need_search = true;
        out.word = word->second;
        return out;
    }

    if (path != "/") {
        out.close = true;
        out.response = buildResponse(404, "Not Found", "text/plain; charset=utf-8",
                                     "not found\n", true);
        return out;
    }

    std::string body;
    body += "{\"method\":\"";
    body += http_protocol::HttpParser::jsonEscape(request.method);
    body += "\",\"path\":\"";
    body += http_protocol::HttpParser::jsonEscape(path);
    body += "\",\"raw_query\":\"";
    body += http_protocol::HttpParser::jsonEscape(query);
    body += "\",\"word\":\"";
    const std::unordered_map<std::string, std::string>::const_iterator word =
        params.find("word");
    if (word != params.end()) {
        body += http_protocol::HttpParser::jsonEscape(word->second);
    }
    body += "\"}\n";
    out.response = buildResponse(200, "OK", "application/json; charset=utf-8",
                                 body, out.close);
    return out;
}

}  // namespace http_router

#endif  // HTTP_ROUTER_HPP
