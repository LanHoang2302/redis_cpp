/*
 * KvStore.cpp — Sharded KV store implementation.
 */
#include "KvStore.hpp"
#include "TtlManager.hpp"
#include "AofWriter.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <sstream>
#include <ctime>

namespace store {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

int64_t KvStore::epoch_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// DJB2 → shard index (power-of-2 mask)
size_t KvStore::shard_index(std::string_view key) const noexcept {
    uint64_t h = 5381;
    for (unsigned char c : key)
        h = ((h << 5) + h) ^ c;
    return static_cast<size_t>(h & (n_shards_ - 1));
}

KvStore::Shard& KvStore::shard_for(std::string_view key) noexcept {
    return shards_[shard_index(key)];
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

KvStore::KvStore(size_t n_shards, AofWriter* aof, bool enable_ttl)
    : shards_(n_shards), n_shards_(n_shards), aof_(aof)
{
    // n_shards must be power-of-2
    if (n_shards == 0 || (n_shards & (n_shards - 1)) != 0)
        throw std::invalid_argument("KvStore: n_shards must be a power of 2");

    if (enable_ttl) {
        ttl_mgr_ = std::make_unique<TtlManager>(
            [this](const std::string& key) { on_ttl_expired(key); });
    }
}

KvStore::~KvStore() = default;

// ─────────────────────────────────────────────────────────────────────────────
// set / set_ex
// ─────────────────────────────────────────────────────────────────────────────

bool KvStore::set_internal(std::string_view key, std::string_view value,
                            int64_t expire_at_ms)
{
    auto& shard = shard_for(key);
    {
        std::unique_lock lock(shard.mu);
        auto& entry = shard.map[std::string(key)];
        entry.value         = std::string(value);
        entry.expire_at_ms  = expire_at_ms;
    }
    if (expire_at_ms > 0 && ttl_mgr_)
        ttl_mgr_->schedule(std::string(key), expire_at_ms);
    return true;
}

bool KvStore::set(std::string_view key, std::string_view value) {
    stat_set_.fetch_add(1, std::memory_order_relaxed);
    if (aof_) aof_->log_set(key, value);
    return set_internal(key, value, -1);
}

bool KvStore::set_ex(std::string_view key, std::string_view value,
                     int64_t ttl_s)
{
    if (ttl_s <= 0) return false;
    int64_t expire_at = epoch_ms() + ttl_s * 1000;
    stat_set_.fetch_add(1, std::memory_order_relaxed);
    if (aof_) aof_->log_set_ex(key, value, expire_at);
    return set_internal(key, value, expire_at);
}

// ─────────────────────────────────────────────────────────────────────────────
// get
// ─────────────────────────────────────────────────────────────────────────────

std::optional<std::string> KvStore::get(std::string_view key) {
    stat_get_.fetch_add(1, std::memory_order_relaxed);
    auto& shard = shard_for(key);

    // Fast path: shared lock for read
    {
        std::shared_lock lock(shard.mu);
        auto it = shard.map.find(std::string(key));
        if (it == shard.map.end()) return std::nullopt;

        const Entry& e = it->second;
        if (e.expire_at_ms < 0 || epoch_ms() < e.expire_at_ms)
            return e.value;
    }

    // Lazy expiry: upgrade to exclusive lock and remove
    {
        std::unique_lock lock(shard.mu);
        auto it = shard.map.find(std::string(key));
        if (it != shard.map.end() &&
            it->second.expire_at_ms >= 0 &&
            epoch_ms() >= it->second.expire_at_ms)
        {
            shard.map.erase(it);
            stat_expired_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// del
// ─────────────────────────────────────────────────────────────────────────────

bool KvStore::del(std::string_view key) {
    stat_del_.fetch_add(1, std::memory_order_relaxed);
    if (aof_) aof_->log_del(key);

    auto& shard = shard_for(key);
    std::unique_lock lock(shard.mu);
    return shard.map.erase(std::string(key)) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// expire
// ─────────────────────────────────────────────────────────────────────────────

bool KvStore::expire(std::string_view key, int64_t ttl_s) {
    if (ttl_s <= 0) return false;

    auto& shard = shard_for(key);
    int64_t expire_at = epoch_ms() + ttl_s * 1000;

    std::unique_lock lock(shard.mu);
    auto it = shard.map.find(std::string(key));
    if (it == shard.map.end()) return false;

    // Check not already expired
    if (it->second.expire_at_ms >= 0 && epoch_ms() >= it->second.expire_at_ms) {
        shard.map.erase(it);
        return false;
    }

    it->second.expire_at_ms = expire_at;
    lock.unlock();

    if (aof_) aof_->log_expire(key, expire_at);

    if (ttl_mgr_) ttl_mgr_->schedule(std::string(key), expire_at);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ttl
// ─────────────────────────────────────────────────────────────────────────────

int64_t KvStore::ttl(std::string_view key) {
    auto& shard = shard_for(key);
    std::shared_lock lock(shard.mu);

    auto it = shard.map.find(std::string(key));
    if (it == shard.map.end()) return -2; // key not found

    const Entry& e = it->second;
    if (e.expire_at_ms < 0) return -1; // no expiry

    int64_t remaining_ms = e.expire_at_ms - epoch_ms();
    if (remaining_ms <= 0) return -2; // expired (will be lazily removed)

    return remaining_ms / 1000; // convert to seconds
}

// ─────────────────────────────────────────────────────────────────────────────
// on_ttl_expired — called by TtlManager background thread
// ─────────────────────────────────────────────────────────────────────────────

void KvStore::on_ttl_expired(const std::string& key) {
    auto& shard = shard_for(key);
    std::unique_lock lock(shard.mu);

    auto it = shard.map.find(key);
    if (it == shard.map.end()) return;

    const Entry& e = it->second;
    // Only remove if still expired (key may have been re-set with a new TTL)
    if (e.expire_at_ms >= 0 && epoch_ms() >= e.expire_at_ms) {
        shard.map.erase(it);
        stat_expired_.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// count / info
// ─────────────────────────────────────────────────────────────────────────────

size_t KvStore::count() const {
    size_t total = 0;
    int64_t now  = epoch_ms();
    for (const auto& shard : shards_) {
        std::shared_lock lock(shard.mu);
        for (const auto& [key, entry] : shard.map) {
            if (entry.expire_at_ms < 0 || now < entry.expire_at_ms)
                ++total;
        }
    }
    return total;
}

std::string KvStore::info() const {
    std::ostringstream oss;
    oss << "# Stats\r\n"
        << "keys:"        << count()                                 << "\r\n"
        << "total_set:"   << stat_set_.load(std::memory_order_relaxed)  << "\r\n"
        << "total_get:"   << stat_get_.load(std::memory_order_relaxed)  << "\r\n"
        << "total_del:"   << stat_del_.load(std::memory_order_relaxed)  << "\r\n"
        << "total_expired:" << stat_expired_.load(std::memory_order_relaxed) << "\r\n"
        << "shards:"      << n_shards_                               << "\r\n";
    return oss.str();
}

} // namespace store
