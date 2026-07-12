/*
 * test_command_dispatcher.cpp — Google Test for CommandDispatcher.
 */
#include <gtest/gtest.h>
#include "../src/commands/CommandDispatcher.hpp"
#include "../src/store/KvStore.hpp"
#include "../src/protocol/RespParser.hpp"

using namespace commands;
using namespace store;
using namespace protocol;

class CommandTest : public ::testing::Test {
protected:
    KvStore            store_{4, nullptr, false};
    CommandDispatcher  dispatcher_;
};

// ─── PING ─────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Ping) {
    auto r = dispatcher_.dispatch({"PING"}, store_);
    EXPECT_EQ(r, "+PONG\r\n");
}

TEST_F(CommandTest, PingWithMessage) {
    auto r = dispatcher_.dispatch({"PING", "hello"}, store_);
    EXPECT_EQ(r, "$5\r\nhello\r\n");
}

// ─── SET / GET ─────────────────────────────────────────────────────────────────

TEST_F(CommandTest, SetAndGet) {
    auto set_r = dispatcher_.dispatch({"SET", "key", "value"}, store_);
    EXPECT_EQ(set_r, "+OK\r\n");

    auto get_r = dispatcher_.dispatch({"GET", "key"}, store_);
    EXPECT_EQ(get_r, "$5\r\nvalue\r\n");
}

TEST_F(CommandTest, GetMissing) {
    auto r = dispatcher_.dispatch({"GET", "nope"}, store_);
    EXPECT_EQ(r, "$-1\r\n");
}

TEST_F(CommandTest, SetWithEx) {
    auto r = dispatcher_.dispatch({"SET", "k", "v", "EX", "10"}, store_);
    EXPECT_EQ(r, "+OK\r\n");
    auto ttl_r = dispatcher_.dispatch({"TTL", "k"}, store_);
    // Should be positive integer
    EXPECT_EQ(ttl_r[0], ':');
}

// ─── DEL ──────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Del) {
    dispatcher_.dispatch({"SET", "k", "v"}, store_);
    auto r = dispatcher_.dispatch({"DEL", "k"}, store_);
    EXPECT_EQ(r, ":1\r\n");
    auto miss = dispatcher_.dispatch({"DEL", "k"}, store_);
    EXPECT_EQ(miss, ":0\r\n");
}

TEST_F(CommandTest, DelMultipleKeys) {
    dispatcher_.dispatch({"SET", "a", "1"}, store_);
    dispatcher_.dispatch({"SET", "b", "2"}, store_);
    auto r = dispatcher_.dispatch({"DEL", "a", "b", "nope"}, store_);
    EXPECT_EQ(r, ":2\r\n");
}

// ─── TTL / EXPIRE ─────────────────────────────────────────────────────────────

TEST_F(CommandTest, TtlNoExpiry) {
    dispatcher_.dispatch({"SET", "k", "v"}, store_);
    auto r = dispatcher_.dispatch({"TTL", "k"}, store_);
    EXPECT_EQ(r, ":-1\r\n");
}

TEST_F(CommandTest, TtlMissing) {
    auto r = dispatcher_.dispatch({"TTL", "nope"}, store_);
    EXPECT_EQ(r, ":-2\r\n");
}

TEST_F(CommandTest, Expire) {
    dispatcher_.dispatch({"SET", "k", "v"}, store_);
    auto r = dispatcher_.dispatch({"EXPIRE", "k", "10"}, store_);
    EXPECT_EQ(r, ":1\r\n");
    auto ttl = dispatcher_.dispatch({"TTL", "k"}, store_);
    EXPECT_NE(ttl, ":-1\r\n");
    EXPECT_NE(ttl, ":-2\r\n");
}

// ─── DBSIZE ───────────────────────────────────────────────────────────────────

TEST_F(CommandTest, DbSize) {
    auto r0 = dispatcher_.dispatch({"DBSIZE"}, store_);
    EXPECT_EQ(r0, ":0\r\n");
    dispatcher_.dispatch({"SET", "a", "1"}, store_);
    dispatcher_.dispatch({"SET", "b", "2"}, store_);
    auto r2 = dispatcher_.dispatch({"DBSIZE"}, store_);
    EXPECT_EQ(r2, ":2\r\n");
}

// ─── Error handling ───────────────────────────────────────────────────────────

TEST_F(CommandTest, UnknownCommand) {
    auto r = dispatcher_.dispatch({"FOOBAR"}, store_);
    EXPECT_EQ(r[0], '-'); // starts with RESP error
}

TEST_F(CommandTest, TooFewArgs) {
    auto r = dispatcher_.dispatch({"GET"}, store_);
    EXPECT_EQ(r[0], '-');
}

TEST_F(CommandTest, SetInvalidExpire) {
    auto r = dispatcher_.dispatch({"SET", "k", "v", "EX", "-5"}, store_);
    EXPECT_EQ(r[0], '-');
}
