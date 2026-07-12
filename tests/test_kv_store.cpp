/*
 * test_kv_store.cpp — Google Test unit tests for KvStore.
 */
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

#include "../src/store/KvStore.hpp"

using namespace store;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Basic CRUD
// ─────────────────────────────────────────────────────────────────────────────

TEST(KvStore, SetAndGet) {
    KvStore store(4, nullptr, false);
    EXPECT_TRUE(store.set("key", "value"));
    auto val = store.get("key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "value");
}

TEST(KvStore, GetMissingKey) {
    KvStore store(4, nullptr, false);
    EXPECT_FALSE(store.get("nosuchkey").has_value());
}

TEST(KvStore, OverwriteKey) {
    KvStore store(4, nullptr, false);
    store.set("k", "v1");
    store.set("k", "v2");
    EXPECT_EQ(*store.get("k"), "v2");
}

TEST(KvStore, Del) {
    KvStore store(4, nullptr, false);
    store.set("k", "v");
    EXPECT_TRUE(store.del("k"));
    EXPECT_FALSE(store.get("k").has_value());
    EXPECT_FALSE(store.del("k")); // second del returns false
}

TEST(KvStore, Count) {
    KvStore store(4, nullptr, false);
    EXPECT_EQ(store.count(), 0u);
    store.set("a", "1");
    store.set("b", "2");
    EXPECT_EQ(store.count(), 2u);
    store.del("a");
    EXPECT_EQ(store.count(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// TTL / Expiry
// ─────────────────────────────────────────────────────────────────────────────

TEST(KvStore, TtlNoExpiry) {
    KvStore store(4, nullptr, false);
    store.set("k", "v");
    EXPECT_EQ(store.ttl("k"), -1); // no expiry
}

TEST(KvStore, TtlMissingKey) {
    KvStore store(4, nullptr, false);
    EXPECT_EQ(store.ttl("nope"), -2);
}

TEST(KvStore, SetEx) {
    KvStore store(4, nullptr, false);
    EXPECT_TRUE(store.set_ex("k", "v", 2)); // 2 second TTL
    auto ttl = store.ttl("k");
    EXPECT_GE(ttl, 0);
    EXPECT_LE(ttl, 2);
    EXPECT_TRUE(store.get("k").has_value());
}

TEST(KvStore, SetExExpires) {
    KvStore store(4, nullptr, true); // enable TTL manager
    EXPECT_TRUE(store.set_ex("k", "v", 1)); // 1 second TTL
    // Should be present immediately
    EXPECT_TRUE(store.get("k").has_value());
    // Wait for expiry
    std::this_thread::sleep_for(1500ms);
    EXPECT_FALSE(store.get("k").has_value());
    EXPECT_EQ(store.ttl("k"), -2);
}

TEST(KvStore, Expire) {
    KvStore store(4, nullptr, false);
    store.set("k", "v");
    EXPECT_TRUE(store.expire("k", 10));
    auto ttl = store.ttl("k");
    EXPECT_GT(ttl, 0);
    EXPECT_LE(ttl, 10);
}

TEST(KvStore, ExpireMissingKey) {
    KvStore store(4, nullptr, false);
    EXPECT_FALSE(store.expire("nope", 10));
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrent access
// ─────────────────────────────────────────────────────────────────────────────

TEST(KvStore, ConcurrentWrites) {
    KvStore store(16, nullptr, false);
    constexpr int THREADS = 8;
    constexpr int OPS     = 1000;

    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < OPS; ++i) {
                std::string key = "key_" + std::to_string(t) + "_" + std::to_string(i);
                store.set(key, "value_" + std::to_string(i));
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(store.count(), THREADS * OPS);
}

TEST(KvStore, ConcurrentReadWrite) {
    KvStore store(16, nullptr, false);
    store.set("shared", "initial");

    std::atomic<bool> done{false};
    std::atomic<int>  read_count{0};

    // Writer thread
    std::thread writer([&] {
        for (int i = 0; i < 500; ++i)
            store.set("shared", std::to_string(i));
        done = true;
    });

    // Multiple reader threads
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!done || read_count < 100) {
                auto v = store.get("shared");
                if (v) ++read_count;
                std::this_thread::yield();
            }
        });
    }

    writer.join();
    for (auto& r : readers) r.join();

    EXPECT_GT(read_count.load(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Info / Count
// ─────────────────────────────────────────────────────────────────────────────

TEST(KvStore, InfoContainsKeys) {
    KvStore store(4, nullptr, false);
    store.set("a", "1");
    store.set("b", "2");
    std::string info = store.info();
    EXPECT_NE(info.find("keys:2"), std::string::npos);
}
