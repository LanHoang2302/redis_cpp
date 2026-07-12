/*
 * RespParser.cpp — RESP parsing + encoding implementation.
 */
#include "RespParser.hpp"
#include <charconv>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace protocol {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

size_t RespParser::find_crlf(std::string_view v, size_t from) {
    for (size_t i = from; i + 1 < v.size(); ++i)
        if (v[i] == '\r' && v[i+1] == '\n') return i;
    return std::string_view::npos;
}

bool RespParser::parse_integer(std::string_view v, size_t from,
                                int64_t& out_val, size_t& next) {
    size_t crlf = find_crlf(v, from);
    if (crlf == std::string_view::npos) return false;

    std::string_view num = v.substr(from, crlf - from);
    auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), out_val);
    if (ec != std::errc{} || ptr != num.data() + num.size()) return false;

    next = crlf + 2; // skip \r\n
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// parse() — top-level dispatcher
// ─────────────────────────────────────────────────────────────────────────────

RespParser::Result RespParser::parse(std::string_view data,
                                     std::vector<std::vector<std::string>>& out_cmds,
                                     size_t& frame_len)
{
    if (data.empty()) return Result::INCOMPLETE;

    if (data[0] == '*') {
        return parse_multibulk(data, out_cmds, frame_len);
    } else {
        return parse_inline(data, out_cmds, frame_len);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_multibulk — *N\r\n$L\r\n<arg>\r\n...
// ─────────────────────────────────────────────────────────────────────────────

RespParser::Result RespParser::parse_multibulk(
        std::string_view data,
        std::vector<std::vector<std::string>>& out,
        size_t& consumed)
{
    // First line: *N\r\n
    int64_t nargs = 0;
    size_t  pos   = 0;
    if (!parse_integer(data, 1, nargs, pos)) return Result::INCOMPLETE;

    if (nargs < 0) return Result::ERROR;   // *-1 is only valid for arrays in replies
    if (nargs == 0) {
        consumed = pos;
        out.emplace_back(); // empty command
        return Result::COMPLETE;
    }

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(nargs));

    for (int64_t i = 0; i < nargs; ++i) {
        // Expect $L\r\n
        if (pos >= data.size()) return Result::INCOMPLETE;
        if (data[pos] != '$') return Result::ERROR;

        int64_t blen = 0;
        size_t  after_hdr = 0;
        if (!parse_integer(data, pos + 1, blen, after_hdr)) return Result::INCOMPLETE;
        if (blen < 0) return Result::ERROR; // nil bulk not valid in request

        // Need blen bytes + \r\n
        if (after_hdr + static_cast<size_t>(blen) + 2 > data.size())
            return Result::INCOMPLETE;

        args.emplace_back(data.substr(after_hdr, static_cast<size_t>(blen)));
        pos = after_hdr + static_cast<size_t>(blen) + 2; // skip data + \r\n
    }

    consumed = pos;
    out.push_back(std::move(args));
    return Result::COMPLETE;
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_inline — "PING\r\n" or "SET key val\r\n"
// ─────────────────────────────────────────────────────────────────────────────

RespParser::Result RespParser::parse_inline(
        std::string_view data,
        std::vector<std::vector<std::string>>& out,
        size_t& consumed)
{
    size_t crlf = find_crlf(data);
    if (crlf == std::string_view::npos) {
        // Try bare '\n' (some clients only send LF)
        size_t lf = data.find('\n');
        if (lf == std::string_view::npos) return Result::INCOMPLETE;
        crlf = (lf > 0 && data[lf-1] == '\r') ? lf - 1 : lf;
        consumed = lf + 1;
    } else {
        consumed = crlf + 2;
    }

    std::string_view line = data.substr(0, crlf);

    // Tokenize by whitespace, support double-quoted tokens
    std::vector<std::string> args;
    size_t i = 0;
    while (i < line.size()) {
        // Skip whitespace
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;

        std::string token;
        if (line[i] == '"') {
            // Quoted token
            ++i;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    ++i; // skip backslash
                    switch (line[i]) {
                        case 'n': token += '\n'; break;
                        case 'r': token += '\r'; break;
                        case 't': token += '\t'; break;
                        default:  token += line[i]; break;
                    }
                } else {
                    token += line[i];
                }
                ++i;
            }
            if (i < line.size()) ++i; // skip closing "
        } else {
            // Plain token
            size_t start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
            token = std::string(line.substr(start, i - start));
        }
        args.push_back(std::move(token));
    }

    if (!args.empty()) {
        // Normalize command to uppercase
        for (char& c : args[0]) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        out.push_back(std::move(args));
    }
    return Result::COMPLETE;
}

// ─────────────────────────────────────────────────────────────────────────────
// RESP Encoding helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string RespParser::simple_string(std::string_view s) {
    std::string r;
    r.reserve(s.size() + 3);
    r += '+';
    r.append(s.data(), s.size());
    r += "\r\n";
    return r;
}

std::string RespParser::error(std::string_view msg) {
    std::string r;
    r.reserve(msg.size() + 5);
    r += "-ERR ";
    r.append(msg.data(), msg.size());
    r += "\r\n";
    return r;
}

std::string RespParser::integer(int64_t n) {
    return ':' + std::to_string(n) + "\r\n";
}

std::string RespParser::bulk_string(std::string_view s) {
    return '$' + std::to_string(s.size()) + "\r\n" +
           std::string(s) + "\r\n";
}

std::string RespParser::nil_bulk() {
    return "$-1\r\n";
}

std::string RespParser::array(const std::vector<std::string>& items) {
    std::string r = '*' + std::to_string(items.size()) + "\r\n";
    for (const auto& item : items)
        r += bulk_string(item);
    return r;
}

} // namespace protocol
