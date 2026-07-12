/*
 * TtlManager.cpp — Min-heap TTL expiry implementation.
 */
#include "TtlManager.hpp"
#include <algorithm>

namespace store {

// ─────────────────────────────────────────────────────────────────────────────

int64_t TtlManager::now_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// ─────────────────────────────────────────────────────────────────────────────

TtlManager::TtlManager(ExpiryCallback cb)
    : cb_(std::move(cb))
{
    thread_ = std::thread([this] { run(); });
}

TtlManager::~TtlManager() {
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

// ─────────────────────────────────────────────────────────────────────────────

void TtlManager::schedule(const std::string& key, int64_t expire_at_ms) {
    {
        std::lock_guard lock(mu_);
        heap_.push({expire_at_ms, key});
    }
    cv_.notify_one(); // wake background thread in case it's sleeping longer
}

// ─────────────────────────────────────────────────────────────────────────────

void TtlManager::run() {
    using namespace std::chrono_literals;

    while (!stop_.load(std::memory_order_acquire)) {
        std::unique_lock lock(mu_);

        if (heap_.empty()) {
            // No entries: sleep up to 100ms waiting for new entries
            cv_.wait_for(lock, 100ms, [this] {
                return stop_.load() || !heap_.empty();
            });
            continue;
        }

        // Calculate time until next expiry
        int64_t next_ms = heap_.top().expire_at_ms;
        int64_t diff_ms = next_ms - now_ms();

        if (diff_ms > 0) {
            // Sleep until next expiry (or interrupted)
            cv_.wait_for(lock, std::chrono::milliseconds(diff_ms), [this, next_ms] {
                return stop_.load() || heap_.empty() || heap_.top().expire_at_ms < next_ms;
            });
        }

        if (stop_.load()) break;

        // Pop all expired entries
        int64_t now = now_ms();
        while (!heap_.empty() && heap_.top().expire_at_ms <= now) {
            std::string key = heap_.top().key;
            heap_.pop();
            lock.unlock();
            // Notify KvStore — it will do a lazy check (key may have been
            // re-set with a new TTL or already deleted)
            cb_(key);
            lock.lock();
            now = now_ms(); // refresh after callback
        }
    }
}

} // namespace store
