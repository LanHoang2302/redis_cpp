#pragma once
/*
 * KvStore.hpp — Sharded, thread-safe key-value store.
 *
 * Design:
 *  - N shards (default 16, must be power-of-2)
 *  - Each shard: std::unordered_map<string,Entry> + std::shared_mutex
 *    → multiple concurrent GETs per shard, exclusive for SET/DEL
 *  - TTL stored as absolute epoch-ms timestamp (-1 = no expiry)
 *  - Lazy expiry on GET + proactive expiry via TtlManager callback
 *  - Stats tracked with std::atomic counters
 */
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace store {

class TtlManager;
class AofWriter;

struct Entry {
    std::string value;
    int64_t     expire_at_ms{-1}; // -1 = persistent
};

class KvStore {
public:
    static constexpr size_t DEFAULT_SHARDS = 16;

    explicit KvStore(size_t n_shards       = DEFAULT_SHARDS,
                     AofWriter* aof        = nullptr,
                     bool       enable_ttl = true);
    ~KvStore();

    // Non-copyable
    KvStore(const KvStore&)            = delete;
    KvStore& operator=(const KvStore&) = delete;

    // ── Core operations ──────────────────────────────────────────────

    // Returns true on success
    bool set(std::string_view key, std::string_view value);
    bool set_ex(std::string_view key, std::string_view value, int64_t ttl_s);

    // Returns value, or std::nullopt if key not found / expired
    std::optional<std::string> get(std::string_view key);

    // Returns true if key existed and was deleted
    bool del(std::string_view key);

    // Set AOF writer dynamically (e.g. to enable it after replay)
    void set_aof(AofWriter* aof) noexcept { aof_ = aof; }

    // Set key with absolute expiration time (in milliseconds since epoch)
    bool set_absolute(std::string_view key, std::string_view value, int64_t expire_at_ms);

    // Set TTL on existing key (Redis EXPIRE semantics)
    bool expire(std::string_view key, int64_t ttl_s);

    // Set absolute expiration time (Redis PEXPIREAT semantics)
    bool expire_at(std::string_view key, int64_t expire_at_ms);

    // Remove expiration from a key (Redis PERSIST semantics)
    bool persist(std::string_view key);

    // ── TTL query ────────────────────────────────────────────────────
    // -2 = key does not exist (or expired)
    // -1 = key exists, no expiry
    // >= 0 = seconds remaining
    int64_t ttl(std::string_view key);

    // ── Diagnostics ──────────────────────────────────────────────────
    size_t  count() const;  // total live keys
    std::string info() const;

    // ── Called by TtlManager when a key may have expired ─────────────
    void on_ttl_expired(const std::string& key);

    // ── Epoch time ───────────────────────────────────────────────────
    static int64_t epoch_ms() noexcept;

private:
    struct Shard {
        std::unordered_map<std::string, Entry> map;
        mutable std::shared_mutex               mu;
    };

    Shard& shard_for(std::string_view key) noexcept;
    size_t shard_index(std::string_view key) const noexcept;

    // Internal set without AOF (used during AOF replay)
    bool set_internal(std::string_view key, std::string_view value,
                      int64_t expire_at_ms);

    std::vector<Shard>        shards_;
    size_t                    n_shards_;
    std::unique_ptr<TtlManager> ttl_mgr_;
    AofWriter*                aof_{nullptr};
    mutable std::mutex        mutation_mutex_;

    // Stats
    mutable std::atomic<uint64_t> stat_set_{0};
    mutable std::atomic<uint64_t> stat_get_{0};
    mutable std::atomic<uint64_t> stat_del_{0};
    mutable std::atomic<uint64_t> stat_expired_{0};

    friend class AofWriter; // for replay helper access
};

} // namespace store
