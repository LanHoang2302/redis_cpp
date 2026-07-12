#pragma once
/*
 * TtlManager.hpp — Efficient TTL expiration using a min-heap.
 *
 * Design:
 *  - A std::priority_queue<TtlEntry, ..., std::greater<>> (min-heap)
 *    orders entries by their expiry timestamp (milliseconds since epoch).
 *  - A background thread wakes up every ~10ms and pops all entries
 *    whose expire_at <= now_ms().
 *  - Uses a "lazy deletion" approach: entries in the heap may be stale
 *    (key already re-set or deleted). The heap entry just triggers
 *    a check; KvStore performs the actual expiry (atomic compare).
 *  - Caller registers an expiry callback: void(const std::string& key).
 */
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <atomic>

namespace store {

using ExpiryCallback = std::function<void(const std::string& key)>;
using ClockMs = std::chrono::milliseconds;

class TtlManager {
public:
    explicit TtlManager(ExpiryCallback cb);
    ~TtlManager();

    // Non-copyable
    TtlManager(const TtlManager&)            = delete;
    TtlManager& operator=(const TtlManager&) = delete;

    // Schedule key for expiry at the given absolute epoch-ms timestamp.
    // Thread-safe.
    void schedule(const std::string& key, int64_t expire_at_ms);

    // Current epoch time in milliseconds
    static int64_t now_ms() noexcept;

private:
    struct Entry {
        int64_t     expire_at_ms;
        std::string key;

        // Min-heap: smallest expire_at_ms at top
        bool operator>(const Entry& o) const noexcept {
            return expire_at_ms > o.expire_at_ms;
        }
    };

    void run();

    using MinHeap = std::priority_queue<Entry,
                                        std::vector<Entry>,
                                        std::greater<Entry>>;

    ExpiryCallback  cb_;
    MinHeap         heap_;
    std::mutex      mu_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::thread      thread_;
};

} // namespace store
