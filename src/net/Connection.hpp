#pragma once
/*
 * Connection.hpp — per-connection state for the epoll reactor.
 *
 * Holds:
 *  - receive buffer (dynamically growing)
 *  - send buffer    (with offset for partial writes)
 *  - generation counter (FD-reuse guard)
 *  - idle-timeout timestamp
 */
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace net {

class Connection {
public:
    static constexpr size_t RECV_INIT = 4096;
    static constexpr size_t SEND_INIT = 4096;

    explicit Connection(int fd, uint64_t gen)
        : fd_(fd), generation_(gen), alive_(true),
          last_active_(std::time(nullptr))
    {
        recv_buf_.reserve(RECV_INIT);
        send_buf_.reserve(SEND_INIT);
    }

    // Non-copyable, non-movable (stored by pointer / map)
    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    // ── Identity ─────────────────────────────────────────────────────
    int      fd()         const noexcept { return fd_; }
    uint64_t generation() const noexcept { return generation_; }
    bool     alive()      const noexcept { return alive_; }
    void     mark_dead()        noexcept { alive_ = false; }
    void     touch()            noexcept { last_active_ = std::time(nullptr); }
    time_t   last_active() const noexcept { return last_active_; }

    // ── Receive buffer (single-threaded: accessed only by reactor thread) ──
    std::vector<char>& recv_buf() noexcept { return recv_buf_; }

    // ── Send buffer (multi-threaded: worker writes, reactor flushes) ──
    // Append data to the send queue; returns false on OOM
    bool send_enqueue(const char* data, size_t len) {
        std::lock_guard lock(send_mu_);
        try {
            send_buf_.insert(send_buf_.end(), data, data + len);
            return true;
        } catch (...) { return false; }
    }

    bool send_enqueue(const std::string& s) {
        return send_enqueue(s.data(), s.size());
    }

    // Attempt to drain send buffer to the socket (called by reactor on EPOLLOUT)
    // Returns: 0 = fully drained, 1 = more pending, -1 = fatal error
    int flush(int fd) {
        std::lock_guard lock(send_mu_);
        while (send_offset_ < send_buf_.size()) {
            ssize_t n = ::write(fd,
                                send_buf_.data() + send_offset_,
                                send_buf_.size()  - send_offset_);
            if (n > 0) {
                send_offset_ += static_cast<size_t>(n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return 1; // kernel buffer full, retry later
            } else {
                return -1; // connection error
            }
        }
        // Fully drained: compact buffer
        send_buf_.clear();
        send_offset_ = 0;
        return 0;
    }

    bool has_pending_send() const {
        std::lock_guard lock(send_mu_);
        return send_offset_ < send_buf_.size();
    }

    // ── Command queue for sequential execution ──────────────────────
    std::vector<std::vector<std::string>>& command_queue() noexcept { return command_queue_; }
    bool& processing() noexcept { return processing_; }

private:
    int      fd_;
    uint64_t generation_;
    bool     alive_;
    time_t   last_active_;

    // Receive buffer — owned by reactor thread, no lock needed
    std::vector<char> recv_buf_;

    // Send buffer — shared between worker threads and reactor thread
    mutable std::mutex send_mu_;
    std::vector<char>  send_buf_;
    size_t             send_offset_{0};

    // Command execution queue — reactor-only, no locks needed
    std::vector<std::vector<std::string>> command_queue_;
    bool                                  processing_{false};
};

} // namespace net
