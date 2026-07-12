#pragma once
/*
 * AofWriter.hpp — Append-Only File (AOF) persistence.
 *
 * Writes every mutating command (SET, DEL, EXPIRE) to a file so the
 * dataset can be restored after a crash.
 *
 * File format (human-readable, Redis-compatible subset):
 *   *3\r\n$3\r\nSET\r\n$<klen>\r\n<key>\r\n$<vlen>\r\n<val>\r\n
 *   *2\r\n$3\r\nDEL\r\n$<klen>\r\n<key>\r\n
 *   *5\r\n$3\r\nSET\r\n...$2\r\nEX\r\n$<slen>\r\n<secs>\r\n
 *
 * Recovery: replay the AOF with a provided KvStore reference.
 *
 * Durability mode (configurable):
 *   - SYNC_ALWAYS  : fsync after every write    (safest, slowest)
 *   - SYNC_EVERY_S : fsync once per second      (default)
 *   - SYNC_NEVER   : let OS decide              (fastest)
 */
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <atomic>

namespace store {

enum class AofSync { ALWAYS, EVERY_SECOND, NEVER };

class KvStore; // forward

class AofWriter {
public:
    AofWriter(std::string path, AofSync sync = AofSync::EVERY_SECOND);
    ~AofWriter();

    // Non-copyable
    AofWriter(const AofWriter&)            = delete;
    AofWriter& operator=(const AofWriter&) = delete;

    bool is_open() const { return file_.is_open(); }

    // Append a SET command
    void log_set(std::string_view key, std::string_view value);

    // Append a SET with EX (absolute expiration in milliseconds)
    void log_set_ex(std::string_view key, std::string_view value, int64_t expire_at_ms);

    // Append a DEL command
    void log_del(std::string_view key);

    // Append a PEXPIREAT command
    void log_expire(std::string_view key, int64_t expire_at_ms);

    // Replay the AOF file into a KvStore.
    // Called once at startup before accepting connections.
    // Returns the number of commands replayed, or -1 on error.
    using SetFn   = std::function<bool(std::string_view key,
                                       std::string_view value)>;
    using SetExFn = std::function<bool(std::string_view key,
                                       std::string_view value,
                                       int64_t expire_at_ms)>;
    using DelFn   = std::function<bool(std::string_view key)>;
    using ExpireFn = std::function<bool(std::string_view key,
                                        int64_t expire_at_ms)>;

    int64_t replay(const std::string& path,
                   SetFn set_fn, SetExFn set_ex_fn, DelFn del_fn, ExpireFn expire_fn);

private:
    void write_resp(const std::string& cmd);
    void maybe_fsync();

    // Background fsync thread (used when sync == EVERY_SECOND)
    void fsync_loop();

    std::string   path_;
    AofSync       sync_mode_;
    std::ofstream file_;
    std::mutex    mu_;

    // For EVERY_SECOND mode
    std::atomic<bool>   stop_{false};
    std::atomic<bool>   need_fsync_{false};
    std::thread         fsync_thread_;
};

} // namespace store
