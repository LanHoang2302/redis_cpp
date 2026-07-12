#pragma once
/*
 * Socket.hpp — RAII wrapper for a POSIX file descriptor (socket or pipe).
 *
 * Move-only: cannot be copied.  On destruction, the fd is closed
 * automatically unless release() has been called first.
 */
#include <unistd.h>
#include <utility>
#include <stdexcept>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstring>

namespace net {

class Socket {
public:
    // Construct from an already-open file descriptor
    explicit Socket(int fd = -1) noexcept : fd_(fd) {}

    ~Socket() noexcept { close_fd(); }

    // Move-only
    Socket(const Socket&)            = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            close_fd();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }

    // Factory: create a non-blocking, CLOEXEC TCP listen socket
    static Socket make_tcp_listen(uint16_t port, int backlog = 4096) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error(strerror(errno));

        Socket s(fd);
        s.set_reuseaddr(true);
        s.set_reuseport(true);
        s.set_nonblocking();
        s.set_cloexec();

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error(std::string("bind: ") + strerror(errno));
        if (::listen(fd, backlog) < 0)
            throw std::runtime_error(std::string("listen: ") + strerror(errno));

        return s;
    }

    // ── Accessors ────────────────────────────────────────────────────
    int  fd()    const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

    // Give up ownership (caller is now responsible for closing)
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    // ── Configration helpers ─────────────────────────────────────────
    void set_nonblocking() const {
        int fl = ::fcntl(fd_, F_GETFL, 0);
        if (fl < 0 || ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK) < 0)
            throw std::runtime_error(strerror(errno));
    }

    void set_cloexec() const {
        int fl = ::fcntl(fd_, F_GETFD, 0);
        if (fl < 0 || ::fcntl(fd_, F_SETFD, fl | FD_CLOEXEC) < 0)
            throw std::runtime_error(strerror(errno));
    }

    void set_reuseaddr(bool on) const {
        int v = on ? 1 : 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v));
    }

    void set_reuseport(bool on) const {
#ifdef SO_REUSEPORT
        int v = on ? 1 : 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &v, sizeof(v));
#else
        (void)on;
#endif
    }

    void set_tcp_nodelay(bool on) const {
        int v = on ? 1 : 0;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
    }

private:
    void close_fd() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    int fd_;
};

} // namespace net
