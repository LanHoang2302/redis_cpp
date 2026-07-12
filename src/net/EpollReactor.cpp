/*
 * EpollReactor.cpp — Implementation of the Linux epoll Reactor.
 *
 * Protocol framing: supports both RESP inline commands and
 * RESP multi-bulk arrays (*N\r\n$L\r\n<arg>\r\n...).
 */
#include "EpollReactor.hpp"
#include "../ThreadPool.hpp"
#include "../protocol/RespParser.hpp"

#ifdef __APPLE__
#  include <sys/event.h>
#  include <sys/time.h>
#else
#  include <sys/epoll.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <cassert>

#ifdef __APPLE__
#  ifndef EPOLLIN
#    define EPOLLIN 0x001
#  endif
#  ifndef EPOLLOUT
#    define EPOLLOUT 0x004
#  endif
#  ifndef EPOLLET
#    define EPOLLET 0x010
#  endif
#  ifndef EPOLLRDHUP
#    define EPOLLRDHUP 0x020
#  endif
#endif

namespace net {

static constexpr int MAX_EVENTS = 64;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

EpollReactor::EpollReactor(ReactorConfig cfg, ThreadPool& pool)
    : cfg_(std::move(cfg)), pool_(pool)
{
    // Ignore SIGPIPE — we handle write errors explicitly
    ::signal(SIGPIPE, SIG_IGN);

    // Create listen socket
    listen_sock_ = Socket::make_tcp_listen(cfg_.port, cfg_.max_connections);

    // Create epoll/kqueue instance
#ifdef __APPLE__
    epoll_fd_ = ::kqueue();
    if (epoll_fd_ >= 0) {
        int flags = ::fcntl(epoll_fd_, F_GETFD, 0);
        ::fcntl(epoll_fd_, F_SETFD, flags | FD_CLOEXEC);
    }
#else
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
#endif
    if (epoll_fd_ < 0) throw std::runtime_error(strerror(errno));

    // Create stop pipe
    if (::pipe(stop_pipe_) < 0) throw std::runtime_error(strerror(errno));
    {
        int fl0 = ::fcntl(stop_pipe_[0], F_GETFL, 0);
        ::fcntl(stop_pipe_[0], F_SETFL, fl0 | O_NONBLOCK);
        int fl1 = ::fcntl(stop_pipe_[1], F_GETFL, 0);
        ::fcntl(stop_pipe_[1], F_SETFL, fl1 | O_NONBLOCK);
    }

    // Register listen socket and stop pipe read-end
    epoll_add(listen_sock_.fd(), EPOLLIN | EPOLLET);
    epoll_add(stop_pipe_[0],    EPOLLIN);

    std::cout << "[EpollReactor] Listening on port " << cfg_.port << "\n";
}

EpollReactor::~EpollReactor() {
    if (epoll_fd_ >= 0)    ::close(epoll_fd_);
    if (stop_pipe_[0] >= 0) ::close(stop_pipe_[0]);
    if (stop_pipe_[1] >= 0) ::close(stop_pipe_[1]);
}

// ─────────────────────────────────────────────────────────────────────────────
// stop() — thread-safe
// ─────────────────────────────────────────────────────────────────────────────

void EpollReactor::stop() noexcept {
    if (stop_pipe_[1] >= 0) {
        char b = 'S';
        (void)::write(stop_pipe_[1], &b, 1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// run() — blocking event loop
// ─────────────────────────────────────────────────────────────────────────────

void EpollReactor::run() {
    running_.store(true, std::memory_order_release);

#ifdef __APPLE__
    struct kevent events[MAX_EVENTS];
#else
    epoll_event events[MAX_EVENTS];
#endif
    time_t last_idle_check = std::time(nullptr);

    while (running_.load(std::memory_order_acquire)) {
        // 1-second timeout so we can run idle checks
#ifdef __APPLE__
        struct timespec timeout{1, 0}; // 1.0 second
        int n = ::kevent(epoll_fd_, nullptr, 0, events, MAX_EVENTS, &timeout);
#else
        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, 1000);
#endif

        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[EpollReactor] wait error: " << strerror(errno) << "\n";
            break;
        }

        for (int i = 0; i < n; ++i) {
#ifdef __APPLE__
            int fd = static_cast<int>(events[i].ident);
#else
            int fd = events[i].data.fd;
#endif

            // Wakeup / Stop pipe notification
            if (fd == stop_pipe_[0]) {
                char buf[128];
                bool stop_loop = false;
                for (;;) {
                    ssize_t bytes_read = ::read(stop_pipe_[0], buf, sizeof(buf));
                    if (bytes_read > 0) {
                        for (ssize_t j = 0; j < bytes_read; ++j) {
                            if (buf[j] == 'S') {
                                stop_loop = true;
                            } else if (buf[j] == 'W') {
                                process_pending_writes();
                            } else if (buf[j] == 'C') {
                                process_completed_commands();
                            }
                        }
                    } else {
                        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            break; // drained completely
                        }
                        if (bytes_read < 0 && errno == EINTR) {
                            continue; // retry
                        }
                        break; // EOF or error
                    }
                }
                if (stop_loop) {
                    running_.store(false, std::memory_order_release);
                    break;
                }
                continue;
            }

            // New connection
            if (fd == listen_sock_.fd()) {
                handle_accept();
                continue;
            }

#ifdef __APPLE__
            bool is_error = (events[i].flags & EV_ERROR);
            bool is_hup   = (events[i].flags & EV_EOF);
            bool is_read  = (events[i].filter == EVFILT_READ);
            bool is_write = (events[i].filter == EVFILT_WRITE);
#else
            uint32_t ev = events[i].events;
            bool is_error = (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP));
            bool is_hup   = false;
            bool is_read  = (ev & EPOLLIN);
            bool is_write = (ev & EPOLLOUT);
#endif

            if (is_error || is_hup) {
                handle_close(fd);
                continue;
            }
            if (is_read)  handle_read(fd);
            if (is_write) handle_write(fd);
        }

        // Idle timeout scan (at most once per second)
        if (cfg_.idle_timeout_s > 0) {
            time_t now = std::time(nullptr);
            if (now - last_idle_check >= 1) {
                check_idle();
                last_idle_check = now;
            }
        }
    }

    std::cout << "[EpollReactor] Event loop exited.\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// handle_accept
// ─────────────────────────────────────────────────────────────────────────────

void EpollReactor::handle_accept() {
    sockaddr_in addr{};
    socklen_t   addrlen = sizeof(addr);

    for (;;) {
#ifdef __APPLE__
        int fd = ::accept(listen_sock_.fd(),
                          reinterpret_cast<sockaddr*>(&addr), &addrlen);
        if (fd >= 0) {
            // Set non-blocking
            int fl = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            // Set cloexec
            int fdfl = ::fcntl(fd, F_GETFD, 0);
            ::fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
        }
#else
        int fd = ::accept4(listen_sock_.fd(),
                           reinterpret_cast<sockaddr*>(&addr), &addrlen,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR)  continue;
            std::cerr << "[EpollReactor] accept4 error: " << strerror(errno) << "\n";
            break;
        }

        // Apply TCP_NODELAY
        {
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }

        uint64_t gen = ++gen_counter_;
        conns_[fd]   = std::make_unique<Connection>(fd, gen);

        epoll_add(fd, EPOLLIN | EPOLLET | EPOLLRDHUP);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// handle_read
// ─────────────────────────────────────────────────────────────────────────────

void EpollReactor::handle_read(int fd) {
    Connection* conn = get_conn(fd);
    if (!conn || !conn->alive()) return;

    auto& buf = conn->recv_buf();

    // Drain kernel buffer (edge-triggered → must read until EAGAIN)
    for (;;) {
        // Ensure at least 4096 bytes of space
        size_t old_size = buf.size();
        buf.resize(old_size + 4096);

        ssize_t n = ::read(fd, buf.data() + old_size, 4096);
        if (n > 0) {
            buf.resize(old_size + static_cast<size_t>(n));
            conn->touch();

            // Guard against runaway clients
            if (buf.size() > cfg_.max_recv_bytes) {
                handle_close(fd);
                return;
            }
        } else if (n == 0) {
            // Peer closed connection
            buf.resize(old_size);
            handle_close(fd);
            return;
        } else {
            buf.resize(old_size);
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR)  continue;
            handle_close(fd);
            return;
        }
    }

    // Parse complete RESP frames from buffer
    std::vector<std::vector<std::string>> cmds;
    if (!extract_frames(*conn, cmds)) {
        return; // Connection was closed during parsing, return immediately!
    }

    // Queue commands for sequential execution
    for (auto& args : cmds) {
        if (args.empty()) continue;
        conn->command_queue().push_back(std::move(args));
    }

    if (!conn->command_queue().empty() && !conn->processing()) {
        dispatch_next_command(conn);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// handle_write — flush pending send buffer
// ─────────────────────────────────────────────────────────────────────────────

void EpollReactor::handle_write(int fd) {
    Connection* conn = get_conn(fd);
    if (!conn || !conn->alive()) return;

    int r = conn->flush(fd);
    if (r == -1) {
        handle_close(fd);
    } else if (r == 0) {
        // Send buffer fully drained → disable EPOLLOUT
        epoll_mod(fd, EPOLLIN | EPOLLET | EPOLLRDHUP);
    }
    // r == 1 → more data pending, keep EPOLLOUT armed
}

// ─────────────────────────────────────────────────────────────────────────────
// handle_close
// ─────────────────────────────────────────────────────────────────────────────

void EpollReactor::handle_close(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;

    it->second->mark_dead();
    epoll_del(fd);
    ::close(fd);
    conns_.erase(it);
}

// ─────────────────────────────────────────────────────────────────────────────
// send() — thread-safe, called from worker threads
// ─────────────────────────────────────────────────────────────────────────────

bool EpollReactor::send(int fd, uint64_t gen, const std::string& data) {
    {
        std::lock_guard lock(pending_writes_mu_);
        pending_writes_.push_back({fd, gen, data});
    }
    char b = 'W';
    (void)::write(stop_pipe_[1], &b, 1);
    return true;
}

void EpollReactor::notify_command_complete(int fd, uint64_t gen) {
    {
        std::lock_guard lock(completed_commands_mu_);
        completed_commands_.push_back({fd, gen});
    }
    char b = 'C';
    (void)::write(stop_pipe_[1], &b, 1);
}

void EpollReactor::process_pending_writes() {
    std::vector<PendingWrite> writes;
    {
        std::lock_guard lock(pending_writes_mu_);
        writes.swap(pending_writes_);
    }

    for (const auto& w : writes) {
        Connection* conn = get_conn(w.fd);
        if (!conn || !conn->alive() || conn->generation() != w.gen) continue;

        bool ok = conn->send_enqueue(w.data);
        if (!ok) continue;

        int r = conn->flush(w.fd);
        if (r == 1) {
            epoll_mod(w.fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP);
        } else if (r == -1) {
            handle_close(w.fd);
        }
    }
}

void EpollReactor::process_completed_commands() {
    std::vector<CommandComplete> completes;
    {
        std::lock_guard lock(completed_commands_mu_);
        completes.swap(completed_commands_);
    }

    for (const auto& c : completes) {
        Connection* conn = get_conn(c.fd);
        if (!conn || !conn->alive() || conn->generation() != c.gen) continue;

        conn->processing() = false;
        if (!conn->command_queue().empty()) {
            dispatch_next_command(conn);
        }
    }
}

void EpollReactor::dispatch_next_command(Connection* conn) {
    if (conn->command_queue().empty()) return;

    conn->processing() = true;
    auto args = std::move(conn->command_queue().front());
    conn->command_queue().pop_front();

    pool_.enqueue([this, fd = conn->fd(), gen = conn->generation(), args = std::move(args)]() mutable {
        cfg_.on_message(fd, gen, std::move(args));
        notify_command_complete(fd, gen);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// check_idle — close connections idle for too long
// ─────────────────────────────────────────────────────────────────────────────

void EpollReactor::check_idle() {
    time_t now = std::time(nullptr);
    // Collect fds to close (avoid modifying map while iterating)
    std::vector<int> to_close;
    for (auto& [fd, conn] : conns_) {
        if (conn->alive() &&
            (now - conn->last_active()) >= cfg_.idle_timeout_s) {
            to_close.push_back(fd);
        }
    }
    for (int fd : to_close) handle_close(fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// extract_frames — parse RESP frames from recv buffer
// ─────────────────────────────────────────────────────────────────────────────

bool EpollReactor::extract_frames(Connection& conn,
                                  std::vector<std::vector<std::string>>& out_cmds)
{
    auto& buf = conn.recv_buf();

    size_t consumed = 0;
    bool alive = true;
    while (consumed < buf.size()) {
        std::string_view data(buf.data() + consumed, buf.size() - consumed);

        size_t frame_len = 0;
        auto result = protocol::RespParser::parse(data, out_cmds, frame_len);

        if (result == protocol::RespParser::Result::COMPLETE) {
            consumed += frame_len;
        } else if (result == protocol::RespParser::Result::INCOMPLETE) {
            break; // Wait for more data
        } else {
            // Parse error → close connection
            handle_close(conn.fd());
            alive = false;
            break;
        }
    }

    // Compact buffer: remove consumed bytes if connection is still alive
    if (alive && consumed > 0) {
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    return alive;
}

// ─────────────────────────────────────────────────────────────────────────────
// Epoll helpers
// ─────────────────────────────────────────────────────────────────────────────

void EpollReactor::epoll_add(int fd, uint32_t events) {
#ifdef __APPLE__
    struct kevent kev[2];
    int n = 0;
    if (events & EPOLLIN) {
        EV_SET(&kev[n++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    }
    if (events & EPOLLOUT) {
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    }
    if (n > 0) {
        ::kevent(epoll_fd_, kev, n, nullptr, 0, nullptr);
    }
#else
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        std::cerr << "[EpollReactor] epoll_add(" << fd << "): " << strerror(errno) << "\n";
#endif
}

void EpollReactor::epoll_mod(int fd, uint32_t events) {
#ifdef __APPLE__
    struct kevent kev[2];
    int n = 0;
    EV_SET(&kev[n++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (events & EPOLLOUT) {
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    } else {
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    ::kevent(epoll_fd_, kev, n, nullptr, 0, nullptr);
#else
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
        std::cerr << "[EpollReactor] epoll_mod(" << fd << "): " << strerror(errno) << "\n";
#endif
}

void EpollReactor::epoll_del(int fd) {
#ifdef __APPLE__
    struct kevent kev[2];
    EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(epoll_fd_, kev, 2, nullptr, 0, nullptr);
#else
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
}

Connection* EpollReactor::get_conn(int fd) noexcept {
    auto it = conns_.find(fd);
    return (it != conns_.end()) ? it->second.get() : nullptr;
}

} // namespace net
