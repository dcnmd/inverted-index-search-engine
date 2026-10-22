#ifndef COMMAND_SERVER_HPP
#define COMMAND_SERVER_HPP

#include "HttpParser.hpp"
#include "TcpSocket.hpp"

#include <sys/epoll.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace command_server_detail {
struct Connection {
    TcpSocket socket;
    std::string inbuf;
    std::string outbuf;
    size_t outpos = 0;
    std::string peer;
    bool close_after_write = false;
    size_t req_count = 0;

    Connection(TcpSocket&& client_socket, const std::string& peer_address)
        : socket(std::move(client_socket)), peer(peer_address) {}

    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;

    bool wantWrite() const {
        return outpos < outbuf.size();
    }

    void clearOut() {
        outbuf.clear();
        outpos = 0;
    }
};
}  // namespace command_server_detail

namespace {
const int kMaxEvents = 1024;
const int kBacklog = 128;
const size_t kReadChunk = 65536;
const size_t kMaxInbuf = 16384;
const size_t kMaxRequestsPerConn = 100;

inline void reportSystemError(const char* operation, int error) {
    std::cerr << operation << ": "
              << std::error_code(error, std::generic_category()).message()
              << " (errno=" << error << ")\n";
}
}  // namespace

class CommandServer {
public:
    typedef std::function<std::string(const std::string& word)> SearchHandler;

    explicit CommandServer(int port, SearchHandler search_handler = SearchHandler())
        : search_handler_(std::move(search_handler)) {
        try {
            listener_ = TcpSocket::listenOn(port, kBacklog);
            port_ = port;
            epoll_fd_ = epoll_create1(0);
            if (epoll_fd_ < 0) {
                throw socket_detail::makeSystemError("epoll_create1", errno);
            }
            addEpollEvent(listener_.nativeHandle(), EventKind::Listener);
        } catch (...) {
            closeResources();
            throw;
        }
    }

    ~CommandServer() {
        closeResources();
    }

    CommandServer(const CommandServer&) = delete;
    CommandServer& operator=(const CommandServer&) = delete;
    // epoll 主事件循环
    void run() {
        std::cerr << "[epoll_http] listening on 0.0.0.0:" << port_
                  << " (HTTP/1.1, epoll ET, single thread)\n";

        epoll_event events[kMaxEvents];
        for (;;) {
            const int ready = epoll_wait(epoll_fd_, events, kMaxEvents, -1);
            if (ready < 0) {
                if (errno == EINTR) continue;
                throw socket_detail::makeSystemError("epoll_wait", errno);
            }

            for (int i = 0; i < ready; ++i) {
                const uint64_t tag = events[i].data.u64;
                const int fd = static_cast<int>(tag & 0xffffffffu);
                const EventKind kind = static_cast<EventKind>(tag >> 32);

                if (kind == EventKind::Listener) {
                    acceptConnections();
                } else if (kind == EventKind::Client) {
                    handleClient(fd, events[i].events);
                } else {
                    throw std::runtime_error("epoll returned an unknown event type");
                }
            }
        }
    }

private:
    typedef command_server_detail::Connection Connection;

    enum class EventKind : uint32_t {
        Listener = 1,
        Client = 2
    };

    int port_ = 0;
    TcpSocket listener_;
    int epoll_fd_ = -1;
    std::unordered_map<int, Connection> conns_;
    SearchHandler search_handler_;

    static uint64_t makeEventTag(EventKind kind, int fd) {
        return (static_cast<uint64_t>(kind) << 32) |
               static_cast<uint32_t>(fd);
    }
    //添加epoll事件和关心
    void addEpollEvent(int fd, EventKind kind) {
        epoll_event event{};
        event.events = EPOLLIN | EPOLLET;
        if (kind == EventKind::Client) event.events |= EPOLLRDHUP;
        event.data.u64 = makeEventTag(kind, fd);
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) < 0) {
            throw socket_detail::makeSystemError("epoll_ctl(ADD)", errno);
        }
    }

    bool updateEvents(int fd, const Connection& connection) {
        epoll_event event{};
        event.events = EPOLLET | EPOLLRDHUP;
        if (!connection.close_after_write) event.events |= EPOLLIN;
        if (connection.wantWrite()) event.events |= EPOLLOUT;
        event.data.u64 = makeEventTag(EventKind::Client, fd);
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
            reportSystemError("epoll_ctl(MOD)", errno);
            return false;
        }
        return true;
    }
    // 接受新客户端连接，并保存连接状态、注册到 epoll
    void acceptConnections() {
        for (;;) {
            TcpSocket client;
            sockaddr_in peer{};
            if (!listener_.accept(client, peer)) return;

#ifdef COMMAND_SERVER_TEST_LARGE_RESPONSE
            const int test_send_buffer = 4096;
            client.setSendBufferSize(test_send_buffer);
#endif

            char address[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address)) == nullptr) {
                std::strncpy(address, "unknown", sizeof(address) - 1);
            }
            const std::string peer_address =
                std::string(address) + ":" + std::to_string(ntohs(peer.sin_port));
            const int fd = client.nativeHandle();

            try {
                Connection connection(std::move(client), peer_address);
                conns_.emplace(fd, std::move(connection));
                addEpollEvent(fd, EventKind::Client);
            } catch (...) {
                conns_.erase(fd);
                throw;
            }
            std::cerr << "[accept] fd=" << fd << " " << peer_address
                      << " (conns=" << conns_.size() << ")\n";
        }
    }
    //组装响应的工具函数
    std::string buildResponse(int status,
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
            response += "\r\nKeep-Alive: timeout=5, max=100";
        } else {
            response += "\r\nConnection: close";
        }
        response += "\r\n";
        response += extra_headers;
        response += "\r\n";
        response += body;
        return response;
    }
    //查看请求和不合规,生成对应响应
    std::string routeRequest(const http_protocol::Request& request, bool& close) {
        if (request.method != "GET") {
            close = true;
            return buildResponse(405, "Method Not Allowed",
                                 "text/plain; charset=utf-8",
                                 "only GET supported\n", close,
                                 "Allow: GET\r\n");
        }

        std::string path;
        std::string query;
        http_protocol::HttpParser::splitTarget(request.target, path, query);
        std::unordered_map<std::string, std::string> params;
        if (!http_protocol::HttpParser::parseQuery(query, params)) {
            close = true;
            return buildResponse(400, "Bad Request", "text/plain; charset=utf-8",
                                 "invalid query encoding\n", close);
        }

#ifdef COMMAND_SERVER_TEST_LARGE_RESPONSE
        if (path == "/__test/large") {
            return buildResponse(200, "OK", "application/octet-stream",
                                 std::string(2 * 1024 * 1024, 'x'), close);
        }
#endif

        if (path == "/s") {
            const std::unordered_map<std::string, std::string>::const_iterator word =
                params.find("word");
            if (word == params.end()) {
                close = true;
                return buildResponse(400, "Bad Request", "text/plain",
                                     "missing word parameter", close);
            }
            if (!search_handler_) {
                close = true;
                return buildResponse(503, "Service Unavailable", "text/plain",
                                     "search backend not configured", close);
            }
            return buildResponse(200, "OK", "application/json; charset=utf-8",
                                 search_handler_(word->second), close);
        }

        if (path != "/") {
            close = true;
            return buildResponse(404, "Not Found", "text/plain; charset=utf-8",
                                 "not found\n", close);
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
        return buildResponse(200, "OK", "application/json; charset=utf-8",
                             body, close);
    }
    //发送数据,直到发完或遇到EAGAIN 
    bool flushOutput(Connection& connection) {
        while (connection.wantWrite()) {
            const ssize_t count = connection.socket.send(
                connection.outbuf.data() + connection.outpos,
                connection.outbuf.size() - connection.outpos);
            if (count > 0) {
                connection.outpos += static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            }
            if (count < 0) reportSystemError("send", errno);
            return false;
        }
        connection.clearOut();
        return true;
    }
    //对端输入数据太大了,或格式错误
    void queueErrorAndClose(Connection& connection,
                            int status,
                            const char* reason) {
        const std::string body = std::string(reason) + "\n";
        connection.outbuf += buildResponse(
            status, reason, "text/plain; charset=utf-8", body, true);
        connection.close_after_write = true;
        connection.inbuf.clear();
    }
    //解析输入缓冲区,放到输出缓冲区里面
    bool processBufferedRequests(Connection& connection, bool peer_eof = false) {
        while (!connection.close_after_write) {
            http_protocol::Request request;
            size_t consumed = 0;
            const http_protocol::ParseResult result =
                http_protocol::HttpParser::tryParseRequest(
                    connection.inbuf, request, consumed);

            if (result == http_protocol::ParseResult::NeedMore) return true;
            if (result == http_protocol::ParseResult::TooLarge) {
                queueErrorAndClose(connection, 431,
                                   "Request Header Fields Too Large");
                return false;
            }
            if (result == http_protocol::ParseResult::Bad) {
                queueErrorAndClose(connection, 400, "Bad Request");
                return false;
            }

            connection.inbuf.erase(0, consumed);
            ++connection.req_count;
            bool close =
                request.close || connection.req_count >= kMaxRequestsPerConn ||
                (peer_eof && connection.inbuf.empty());
            connection.outbuf += routeRequest(request, close);
            if (close) connection.close_after_write = true;
        }
        return true;
    }
    //读并处理客户端发来的数据
    void handleReadable(int fd) {
        std::unordered_map<int, Connection>::iterator found = conns_.find(fd);
        if (found == conns_.end()) return;
        Connection& connection = found->second;

        char buffer[kReadChunk];
        bool eof = false;
        for (;;) {
            if (connection.inbuf.size() >= kMaxInbuf) {
                if (!processBufferedRequests(connection)) break;
                if (connection.close_after_write) break;
                if (connection.inbuf.size() >= kMaxInbuf) {
                    queueErrorAndClose(connection, 431,
                                       "Request Header Fields Too Large");
                    break;
                }
            }

            const size_t available = kMaxInbuf - connection.inbuf.size();
            const size_t read_size = std::min(available, sizeof(buffer));
            const ssize_t count = connection.socket.receive(buffer, read_size);
            if (count > 0) {
                connection.inbuf.append(buffer, static_cast<size_t>(count));
                continue;
            }
            if (count == 0) {
                eof = true;
                break;
            }

            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK) break;
            if (error == EINTR) continue;
            reportSystemError("recv", error);
            closeClient(fd, "recv error");
            return;
        }

        processBufferedRequests(connection, eof);

        if (eof && !connection.close_after_write) {
            if (!connection.inbuf.empty()) {
                queueErrorAndClose(connection, 400, "Bad Request");
            } else {
                connection.close_after_write = true;
            }
        }

        if (!flushOutput(connection)) {
            closeClient(fd, "send failed");
            return;
        }
        if (connection.close_after_write && !connection.wantWrite()) {
            closeClient(fd, "response done, closing");
            return;
        }
        if (!updateEvents(fd, connection)) {
            closeClient(fd, "epoll_ctl(MOD) failed");
        }
    }
    //客户端写数据
    bool handleWritable(int fd) {
        std::unordered_map<int, Connection>::iterator found = conns_.find(fd);
        if (found == conns_.end()) return false;
        Connection& connection = found->second;

        if (!flushOutput(connection)) {
            closeClient(fd, "send failed");
            return false;
        }
        if (connection.close_after_write && !connection.wantWrite()) {
            closeClient(fd, "response done, closing");
            return false;
        }
        if (!updateEvents(fd, connection)) {
            closeClient(fd, "epoll_ctl(MOD) failed");
            return false;
        }
        return true;
    }
    //客户端事件分发
    void handleClient(int fd, uint32_t events) {
        if (events & EPOLLERR) {
            std::unordered_map<int, Connection>::const_iterator found =
                conns_.find(fd);
            try {
                const int socket_error =
                    found == conns_.end() ? 0 : found->second.socket.pendingError();
                if (socket_error != 0) {
                    reportSystemError("client socket", socket_error);
                }
            } catch (const std::system_error& error) {
                reportSystemError("getsockopt(SO_ERROR)", error.code().value());
            }
            closeClient(fd, "EPOLLERR");
            return;
        }

        if (events & EPOLLOUT) {
            if (!handleWritable(fd)) return;
        }
        if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
            handleReadable(fd);
        }
    }
    //关闭客户端连接
    void closeClient(int fd, const char* reason) {
        std::unordered_map<int, Connection>::iterator found = conns_.find(fd);
        const std::string peer =
            found == conns_.end() ? std::string("unknown") : found->second.peer;

        const int result = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        const int error = errno;
        if (result < 0 && error != ENOENT && error != EBADF) {
            reportSystemError("epoll_ctl(DEL)", error);
        }
        if (found != conns_.end() && found->second.socket.close() < 0) {
            reportSystemError("close(client)", errno);
        }
        conns_.erase(fd);
        std::cerr << "[close] fd=" << fd << " " << peer << " (" << reason
                  << ", conns=" << conns_.size() << ")\n";
    }
    //关闭服务端
    void closeResources() {
        conns_.clear();
        if (epoll_fd_ >= 0) {
            close(epoll_fd_);
            epoll_fd_ = -1;
        }
        listener_.close();
    }
};

#endif  // COMMAND_SERVER_HPP
