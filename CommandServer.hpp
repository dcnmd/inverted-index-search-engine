#ifndef COMMAND_SERVER_HPP
#define COMMAND_SERVER_HPP

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>

namespace {
const int kMaxEvents = 1024;
const int kBacklog = 128;
const size_t kReadChunk = 65536;

inline void throwSystemError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

inline void reportSystemError(const char* operation, int error) {
    std::cerr << operation << ": "
              << std::error_code(error, std::generic_category()).message()
              << " (errno=" << error << ")\n";
}
}  // namespace

class CommandServer {
public:
    explicit CommandServer(int port) {
        try {
            openListener(port);
            epoll_fd_ = epoll_create1(0);
            if (epoll_fd_ < 0) {
                throwSystemError("epoll_create1");
            }
            addEpollEvent(listen_fd_, EventKind::Listener);
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

    void run() {
        std::cerr << "[epoll_echo] listening on 0.0.0.0:" << port_
                  << " (epoll ET, single thread)\n";

        epoll_event events[kMaxEvents];
        for (;;) {
            const int ready = epoll_wait(epoll_fd_, events, kMaxEvents, -1);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throwSystemError("epoll_wait");
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
    enum class EventKind : uint32_t {
        Listener = 1,
        Client = 2
    };

    int port_ = 0;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    std::unordered_map<int, std::string> inbuf_;

    static uint64_t makeEventTag(EventKind kind, int fd) {
        return (static_cast<uint64_t>(kind) << 32) |
               static_cast<uint32_t>(fd);
    }

    void openListener(int port) {
        port_ = port;
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (listen_fd_ < 0) {
            throwSystemError("socket");
        }

        int one = 1;
        if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
            throwSystemError("setsockopt(SO_REUSEADDR)");
        }

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(port_));

        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throwSystemError("bind");
        }
        if (listen(listen_fd_, kBacklog) < 0) {
            throwSystemError("listen");
        }
    }

    void addEpollEvent(int fd, EventKind kind) {
        epoll_event event{};
        event.events = EPOLLIN | EPOLLET;
        if (kind == EventKind::Client) {
            event.events |= EPOLLRDHUP;
        }
        event.data.u64 = makeEventTag(kind, fd);
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) < 0) {
            throwSystemError("epoll_ctl(ADD)");
        }
    }

    void acceptConnections() {
        for (;;) {
            sockaddr_in peer{};
            socklen_t peer_length = sizeof(peer);
            const int conn = accept4(
                listen_fd_,
                reinterpret_cast<sockaddr*>(&peer),
                &peer_length,
                SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (conn < 0) {
                const int error = errno;
                if (error == EAGAIN || error == EWOULDBLOCK) {
                    return;
                }
                if (error == EINTR || error == ECONNABORTED) {
                    continue;
                }
                throw std::system_error(error, std::generic_category(), "accept4");
            }

            try {
                inbuf_.emplace(conn, std::string());
                addEpollEvent(conn, EventKind::Client);
            } catch (...) {
                inbuf_.erase(conn);
                close(conn);
                throw;
            }

            char address[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address)) == nullptr) {
                std::strncpy(address, "unknown", sizeof(address) - 1);
            }
            std::cerr << "[accept] fd=" << conn << " " << address << ":"
                      << ntohs(peer.sin_port) << " (conns=" << inbuf_.size() << ")\n";
        }
    }

    void handleClient(int fd, uint32_t events) {
        const auto found = inbuf_.find(fd);
        if (found == inbuf_.end()) {
            std::cerr << "[warning] event for unknown client fd=" << fd << '\n';
            return;
        }

        if (events & EPOLLERR) {
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) < 0) {
                reportSystemError("getsockopt(SO_ERROR)", errno);
            } else if (socket_error != 0) {
                reportSystemError("client socket", socket_error);
            }
            closeClient(fd, "EPOLLERR");
            return;
        }

        std::string& pending = found->second;
        char buffer[kReadChunk];
        bool eof = false;

        for (;;) {
            const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
            if (count > 0) {
                pending.append(buffer, static_cast<size_t>(count));
                continue;
            }
            if (count == 0) {
                eof = true;
                break;
            }

            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK) {
                break;
            }
            if (error == EINTR) {
                continue;
            }
            reportSystemError("recv", error);
            closeClient(fd, "recv error");
            return;
        }

        while (!pending.empty()) {
            const ssize_t count = send(fd, pending.data(), pending.size(), MSG_NOSIGNAL);
            if (count > 0) {
                pending.erase(0, static_cast<size_t>(count));
                continue;
            }
            if (count == 0) {
                break;
            }

            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            if (error == EAGAIN || error == EWOULDBLOCK) {
                break;
            }
            reportSystemError("send", error);
            closeClient(fd, "send error");
            return;
        }

        if (eof && pending.empty()) {
            closeClient(fd, "peer closed");
        }
    }

    void closeClient(int fd, const char* reason) {
        const int result = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        const int error = errno;
        if (result < 0 && error != ENOENT && error != EBADF) {
            reportSystemError("epoll_ctl(DEL)", error);
        }

        if (close(fd) < 0) {
            reportSystemError("close(client)", errno);
        }
        inbuf_.erase(fd);
        std::cerr << "[close] fd=" << fd << " (" << reason << ")\n";
    }

    void closeResources() {
        for (const auto& client : inbuf_) {
            close(client.first);
        }
        inbuf_.clear();
        if (epoll_fd_ >= 0) {
            close(epoll_fd_);
            epoll_fd_ = -1;
        }
        if (listen_fd_ >= 0) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
    }
};

#endif  // COMMAND_SERVER_HPP
