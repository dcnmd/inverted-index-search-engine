#ifndef TCP_SOCKET_HPP
#define TCP_SOCKET_HPP

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

namespace socket_detail {
inline std::system_error makeSystemError(const char* operation, int error) {
    return std::system_error(error, std::generic_category(), operation);
}

// 预留一个 fd：真撞到 EMFILE 时先关掉它腾出位置，accept 后立刻 close，再补回来。
// 没有这个兜底，EMFILE 会以异常形式冒泡到 main loop，直接打挂服务器。
inline int& spareFdSlot() {
    static int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    return fd;
}

inline void releaseSpareFd() {
    int& spare = spareFdSlot();
    if (spare >= 0) {
        ::close(spare);
        spare = -1;
    }
}

inline void reserveSpareFd() {
    int& spare = spareFdSlot();
    if (spare < 0) {
        spare = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
}
}  // namespace socket_detail

class TcpSocket {
public:
    TcpSocket() = default;
    explicit TcpSocket(int fd) : fd_(fd) {}

    ~TcpSocket() {
        close();
    }

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    TcpSocket(TcpSocket&& other) noexcept : fd_(other.release()) {}

    TcpSocket& operator=(TcpSocket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.release();
        }
        return *this;
    }

    static TcpSocket listenOn(int port, int backlog) {
        TcpSocket listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
        if (listener.fd_ < 0) {
            throw socket_detail::makeSystemError("socket", errno);
        }

        int one = 1;
        if (::setsockopt(listener.fd_, SOL_SOCKET, SO_REUSEADDR,
                         &one, sizeof(one)) < 0) {
            throw socket_detail::makeSystemError("setsockopt(SO_REUSEADDR)", errno);
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<uint16_t>(port));

        if (::bind(listener.fd_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) < 0) {
            throw socket_detail::makeSystemError("bind", errno);
        }
        if (::listen(listener.fd_, backlog) < 0) {
            throw socket_detail::makeSystemError("listen", errno);
        }
        return listener;
    }

    bool accept(TcpSocket& client, sockaddr_in& peer) const {
        int emfile_retries = 0;
        for (;;) {
            socklen_t peer_length = sizeof(peer);
            const int accepted_fd = ::accept4(
                fd_,
                reinterpret_cast<sockaddr*>(&peer),
                &peer_length,
                SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (accepted_fd >= 0) {
                client = TcpSocket(accepted_fd);
                return true;
            }

            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK) {
                return false;
            }
            if (error == EINTR || error == ECONNABORTED) {
                continue;
            }
            if (error == EMFILE || error == ENFILE) {
                // fd 用尽：腾出预留 fd -> accept 后立刻拒绝 -> 补回预留 fd
                if (++emfile_retries > 16) return false;
                socket_detail::releaseSpareFd();
                const int rejected = ::accept(fd_, nullptr, nullptr);
                if (rejected >= 0) ::close(rejected);
                socket_detail::reserveSpareFd();
                continue;
            }
            throw socket_detail::makeSystemError("accept4", error);
        }
    }

    ssize_t receive(void* buffer, size_t size) const {
        return ::recv(fd_, buffer, size, 0);
    }

    ssize_t send(const void* buffer, size_t size) const {
        return ::send(fd_, buffer, size, MSG_NOSIGNAL);
    }

    void setSendBufferSize(int size) const {
        if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0) {
            throw socket_detail::makeSystemError("setsockopt(SO_SNDBUF)", errno);
        }
    }

    int pendingError() const {
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
            throw socket_detail::makeSystemError("getsockopt(SO_ERROR)", errno);
        }
        return error;
    }

    int nativeHandle() const {
        return fd_;
    }

    int close() noexcept {
        if (fd_ < 0) {
            return 0;
        }
        const int fd = release();
        return ::close(fd);
    }

private:
    int fd_ = -1;

    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
};

#endif  // TCP_SOCKET_HPP
