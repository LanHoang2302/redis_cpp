/*
 * test_resp_parser.cpp — Google Test unit tests for RespParser.
 */
#include <gtest/gtest.h>
#include "../src/protocol/RespParser.hpp"

using namespace protocol;

// ─── Parse helpers ────────────────────────────────────────────────────────────

static std::vector<std::vector<std::string>> parse_all(std::string_view data) {
    std::vector<std::vector<std::string>> cmds;
    size_t offset = 0;
    while (offset < data.size()) {
        std::string_view view(data.data() + offset, data.size() - offset);
        size_t frame_len = 0;
        auto r = RespParser::parse(view, cmds, frame_len);
        if (r != RespParser::Result::COMPLETE) break;
        offset += frame_len;
    }
    return cmds;
}

// ─── Multi-bulk (inline from redis-cli) ───────────────────────────────────────

TEST(RespParser, ParseMultibulkSet) {
    std::string data = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    auto cmds = parse_all(data);
    ASSERT_EQ(cmds.size(), 1u);
    ASSERT_EQ(cmds[0].size(), 3u);
    EXPECT_EQ(cmds[0][0], "SET");
    EXPECT_EQ(cmds[0][1], "foo");
    EXPECT_EQ(cmds[0][2], "bar");
}

TEST(RespParser, ParseMultibulkGet) {
    std::string data = "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n";
    auto cmds = parse_all(data);
    ASSERT_EQ(cmds.size(), 1u);
    ASSERT_EQ(cmds[0].size(), 2u);
    EXPECT_EQ(cmds[0][0], "GET");
    EXPECT_EQ(cmds[0][1], "foo");
}

TEST(RespParser, ParseMultibulkPing) {
    std::string data = "*1\r\n$4\r\nPING\r\n";
    auto cmds = parse_all(data);
    ASSERT_EQ(cmds.size(), 1u);
    ASSERT_EQ(cmds[0].size(), 1u);
    EXPECT_EQ(cmds[0][0], "PING");
}

TEST(RespParser, ParsePipeline) {
    std::string data =
        "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
        "*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
        "*1\r\n$4\r\nPING\r\n";
    auto cmds = parse_all(data);
    ASSERT_EQ(cmds.size(), 3u);
    EXPECT_EQ(cmds[0][0], "SET");
    EXPECT_EQ(cmds[1][0], "GET");
    EXPECT_EQ(cmds[2][0], "PING");
}

// ─── Inline format ─────────────────────────────────────────────────────────────

TEST(RespParser, InlinePing) {
    std::string data = "PING\r\n";
    auto cmds = parse_all(data);
    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0][0], "PING");
}

TEST(RespParser, InlineSetQuoted) {
    std::string data = "SET key \"hello world\"\r\n";
    auto cmds = parse_all(data);
    ASSERT_EQ(cmds.size(), 1u);
    ASSERT_EQ(cmds[0].size(), 3u);
    EXPECT_EQ(cmds[0][0], "SET");
    EXPECT_EQ(cmds[0][1], "key");
    EXPECT_EQ(cmds[0][2], "hello world");
}

TEST(RespParser, InlineUppercasesCommand) {
    std::string data = "get mykey\r\n";
    auto cmds = parse_all(data);
    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0][0], "GET");  // normalized to upper
}

// ─── Incomplete frames ─────────────────────────────────────────────────────────

TEST(RespParser, IncompleteMultibulk) {
    std::string data = "*3\r\n$3\r\nSET\r\n"; // missing key and value
    std::vector<std::vector<std::string>> cmds;
    size_t frame_len = 0;
    auto r = RespParser::parse(data, cmds, frame_len);
    EXPECT_EQ(r, RespParser::Result::INCOMPLETE);
    EXPECT_TRUE(cmds.empty());
}

TEST(RespParser, IncompleteInline) {
    std::string data = "PING"; // no \r\n
    std::vector<std::vector<std::string>> cmds;
    size_t frame_len = 0;
    auto r = RespParser::parse(data, cmds, frame_len);
    EXPECT_EQ(r, RespParser::Result::INCOMPLETE);
}

// ─── Value with spaces (bulk string) ──────────────────────────────────────────

TEST(RespParser, BulkStringWithSpaces) {
    std::string val = "hello world foo bar";
    std::string data = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$"
                     + std::to_string(val.size()) + "\r\n" + val + "\r\n";
    auto cmds = parse_all(data);
    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0][2], val);
}

// ─── RESP Encoder ─────────────────────────────────────────────────────────────

TEST(RespEncoder, SimpleString) {
    EXPECT_EQ(RespParser::simple_string("OK"), "+OK\r\n");
}

TEST(RespEncoder, Error) {
    EXPECT_EQ(RespParser::error("bad input"), "-ERR bad input\r\n");
}

TEST(RespEncoder, Integer) {
    EXPECT_EQ(RespParser::integer(42),  ":42\r\n");
    EXPECT_EQ(RespParser::integer(-1), ":-1\r\n");
    EXPECT_EQ(RespParser::integer(0),  ":0\r\n");
}

TEST(RespEncoder, BulkString) {
    EXPECT_EQ(RespParser::bulk_string("hello"), "$5\r\nhello\r\n");
    EXPECT_EQ(RespParser::bulk_string(""),      "$0\r\n\r\n");
}

TEST(RespEncoder, NilBulk) {
    EXPECT_EQ(RespParser::nil_bulk(), "$-1\r\n");
}
