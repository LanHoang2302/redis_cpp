/*
 * test_ttl_manager.cpp — Google Test unit tests for TtlManager.
 */
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

#include "../src/store/TtlManager.hpp"

using namespace store;
using namespace std::chrono_literals;

TEST(TtlManager, FiresOnExpiry) {
    std::atomic<int> count{0};
    std::string fired_key;

    TtlManager mgr([&](const std::string& key) {
        fired_key = key;
        ++count;
    });

    int64_t expire_at = TtlManager::now_ms() + 200; // 200ms from now
    mgr.schedule("mykey", expire_at);

    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(count.load(), 1);
    EXPECT_EQ(fired_key, "mykey");
}

TEST(TtlManager, MultipleKeys) {
    std::atomic<int> count{0};
    std::vector<std::string> fired;
    std::mutex mu;

    TtlManager mgr([&](const std::string& key) {
        std::lock_guard lock(mu);
        fired.push_back(key);
        ++count;
    });

    int64_t now = TtlManager::now_ms();
    mgr.schedule("key1", now + 100);
    mgr.schedule("key2", now + 200);
    mgr.schedule("key3", now + 300);

    std::this_thread::sleep_for(600ms);
    EXPECT_EQ(count.load(), 3);
}

TEST(TtlManager, EarlierKeyFiresFirst) {
    std::vector<std::string> order;
    std::mutex mu;

    TtlManager mgr([&](const std::string& key) {
        std::lock_guard lock(mu);
        order.push_back(key);
    });

    int64_t now = TtlManager::now_ms();
    mgr.schedule("late",  now + 300);
    mgr.schedule("early", now + 100);

    std::this_thread::sleep_for(600ms);
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "early");
    EXPECT_EQ(order[1], "late");
}

TEST(TtlManager, NowMs) {
    int64_t a = TtlManager::now_ms();
    std::this_thread::sleep_for(10ms);
    int64_t b = TtlManager::now_ms();
    EXPECT_GT(b, a);
}
