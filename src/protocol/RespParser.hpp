#pragma once
/*
 * RespParser.hpp — RESP (Redis Serialization Protocol) parser.
 *
 * Supports:
 *  1. Multi-bulk array: *N\r\n$L\r\n<arg>\r\n...
 *  2. Inline commands:  PING\r\n  /  SET key val\r\n
 *     (sent by redis-cli in non-pipeline mode)
 *
 * Usage:
 *   std::vector<std::vector<std::string>> cmds;
 *   size_t frame_len = 0;
 *   auto r = RespParser::parse(data, cmds, frame_len);
 *   // cmds may get a new entry appended on COMPLETE
 *   // frame_len = bytes consumed from data
 */
#include <string>
#include <string_view>
#include <vector>

namespace protocol {

class RespParser {
public:
    enum class Result { COMPLETE, INCOMPLETE, ERROR };

    // Parse one frame from data.
    // On COMPLETE: appends one command to out_cmds, sets frame_len.
    // On INCOMPLETE: out_cmds unchanged, frame_len = 0.
    // On ERROR: caller should close the connection.
    static Result parse(std::string_view data,
                        std::vector<std::vector<std::string>>& out_cmds,
                        size_t& frame_len);

    // Encode RESP Simple String: "+OK\r\n"
    static std::string simple_string(std::string_view s);

    // Encode RESP Error: "-ERR msg\r\n"
    static std::string error(std::string_view msg);

    // Encode RESP Integer: ":42\r\n"
    static std::string integer(int64_t n);

    // Encode RESP Bulk String: "$5\r\nhello\r\n"  or  "$-1\r\n" for nil
    static std::string bulk_string(std::string_view s);
    static std::string nil_bulk();

    // Encode RESP Array of bulk strings
    static std::string array(const std::vector<std::string>& items);

private:
    // Parse multi-bulk frame starting with '*'
    static Result parse_multibulk(std::string_view data,
                                  std::vector<std::vector<std::string>>& out,
                                  size_t& consumed);

    // Parse inline command (no leading '*')
    static Result parse_inline(std::string_view data,
                                std::vector<std::vector<std::string>>& out,
                                size_t& consumed);

    // Find "\r\n" in view; returns position or npos
    static size_t find_crlf(std::string_view v, size_t from = 0);

    // Parse integer from view[from..end_of_line); sets next to after \r\n
    static bool parse_integer(std::string_view v, size_t from,
                               int64_t& out_val, size_t& next);
};

} // namespace protocol
