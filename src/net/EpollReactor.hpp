#pragma once
/*
 * EpollReactor.hpp — Linux epoll-based Reactor pattern.
 *
 * The reactor owns the event loop and dispatches I/O events.
 * Business logic is handled by a MessageHandler callback
 * executed in a thread-pool worker (not on the reactor thread).
 *
 * Design:
 *  - Single reactor thread runs epoll_wait (edge-triggered, EPOLLET)
 *  - Accept new connections → create Connection objects
 *  - On EPOLLIN  → drain recv buffer → extract complete RESP frames
 *                  → enqueue jobs to ThreadPool
 *  - On EPOLLOUT → flush pending send buffer
 *  - Idle timeout → close stale connections
 *  - stop_fd pipe for graceful shutdown (thread-safe)
 */
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>

#include "Socket.hpp"
#include "Connection.hpp"

class ThreadPool; // forward declaration — ThreadPool.hpp

namespace net {

// Called by worker thread when a complete RESP command has been parsed.
// fd  : connection fd (use reactor.send() to reply)
// gen : generation guard (check before using fd)
// args: parsed tokens, e.g. {"SET", "key", "value"}
using MessageHandler = std::function<void(int fd, uint64_t gen,
                                          std::vector<std::string> args)>;

struct ReactorConfig {
    uint16_t     port            = 8080;
    int          max_connections = 4096;
    int          idle_timeout_s  = 60;   // 0 = no timeout
    size_t       max_recv_bytes  = 65536;
    MessageHandler on_message;
};

class EpollReactor {
public:
    explicit EpollReactor(ReactorConfig cfg, ThreadPool& pool);
    ~EpollReactor();

    // Non-copyable, non-movable
    EpollReactor(const EpollReactor&)            = delete;
    EpollReactor& operator=(const EpollReactor&) = delete;

    // Run event loop (blocking until stop() is called)
    void run();

    // Signal reactor to exit (thread-safe, called from any thread)
    void stop() noexcept;

    // Send data to a connection (thread-safe: callable from worker threads)
    // Returns false if connection is gone or buffer append failed
    bool send(int fd, uint64_t gen, const std::string& data);

    // Notify reactor that a command has completed execution (thread-safe)
    void notify_command_complete(int fd, uint64_t gen);

private:
    struct PendingWrite {
        int fd;
        uint64_t gen;
        std::string data;
    };

    struct CommandComplete {
        int fd;
        uint64_t gen;
    };

    // Epoll helpers
    void epoll_add(int fd, uint32_t events);
    void epoll_mod(int fd, uint32_t events);
    void epoll_del(int fd);

    // Event handlers
    void handle_accept();
    void handle_read(int fd);
    void handle_write(int fd);
    void handle_close(int fd);

    // Process thread-safe events (called only in reactor thread)
    void process_pending_writes();
    void process_completed_commands();
    void dispatch_next_command(Connection* conn);

    // Idle timeout scan
    void check_idle();

    // RESP framing: split raw bytes in recv_buf into complete frames
    // Puts each complete command's token list into out_cmds
    void extract_frames(Connection& conn,
                        std::vector<std::vector<std::string>>& out_cmds);

    // Connection table helpers
    Connection* get_conn(int fd) noexcept;

    ReactorConfig  cfg_;
    ThreadPool&    pool_;

    Socket         listen_sock_;
    int            epoll_fd_{-1};

    // Wakeup/Stop pipe (write 1 byte to [1] → reactor sees [0] readable → wakes up)
    int stop_pipe_[2]{-1, -1};

    // Thread-safe queues for worker thread communication
    std::mutex                 pending_writes_mu_;
    std::vector<PendingWrite>  pending_writes_;

    std::mutex                 completed_commands_mu_;
    std::vector<CommandComplete> completed_commands_;

    // Connection table: fd → Connection (fd can be sparse, use map)
    std::unordered_map<int, std::unique_ptr<Connection>> conns_;
    uint64_t gen_counter_{0};

    std::atomic<bool> running_{false};
};

} // namespace net
