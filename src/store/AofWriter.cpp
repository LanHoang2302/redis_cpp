/*
 * AofWriter.cpp — AOF persistence implementation.
 */
#include "AofWriter.hpp"

#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <fcntl.h>   // O_WRONLY, O_APPEND, open
#include <unistd.h>  // fsync, close
#include <cerrno>

namespace store {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

AofWriter::AofWriter(std::string path, AofSync sync)
    : path_(std::move(path)), sync_mode_(sync)
{
    if (!path_.empty()) {
        file_.open(path_, std::ios::binary | std::ios::app);
        if (!file_.is_open())
            std::cerr << "[AofWriter] Cannot open " << path_ << ": " << strerror(errno) << "\n";
    }

    if (sync_mode_ == AofSync::EVERY_SECOND && file_.is_open()) {
        fsync_thread_ = std::thread([this] { fsync_loop(); });
    }
}

AofWriter::~AofWriter() {
    stop_.store(true, std::memory_order_release);
    if (fsync_thread_.joinable()) fsync_thread_.join();

    std::lock_guard lock(mu_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// log_set
// ─────────────────────────────────────────────────────────────────────────────

static std::string bulk(std::string_view s) {
    return '$' + std::to_string(s.size()) + "\r\n" + std::string(s) + "\r\n";
}

void AofWriter::log_set(std::string_view key, std::string_view value) {
    // *3\r\n$3\r\nSET\r\n$K\r\n<key>\r\n$V\r\n<val>\r\n
    std::string cmd = "*3\r\n" + bulk("SET") + bulk(key) + bulk(value);
    write_resp(cmd);
}

void AofWriter::log_set_ex(std::string_view key, std::string_view value,
                            int64_t ttl_s)
{
    // *5\r\n$3\r\nSET\r\n...$2\r\nEX\r\n$<N>\r\n<secs>\r\n
    std::string secs = std::to_string(ttl_s);
    std::string cmd  = "*5\r\n"
                     + bulk("SET") + bulk(key) + bulk(value)
                     + bulk("EX") + bulk(secs);
    write_resp(cmd);
}

void AofWriter::log_del(std::string_view key) {
    // *2\r\n$3\r\nDEL\r\n$K\r\n<key>\r\n
    std::string cmd = "*2\r\n" + bulk("DEL") + bulk(key);
    write_resp(cmd);
}

// ─────────────────────────────────────────────────────────────────────────────
// write_resp (internal)
// ─────────────────────────────────────────────────────────────────────────────

void AofWriter::write_resp(const std::string& cmd) {
    if (!file_.is_open()) return;
    std::lock_guard lock(mu_);
    file_.write(cmd.data(), static_cast<std::streamsize>(cmd.size()));
    maybe_fsync();
}

void AofWriter::maybe_fsync() {
    // mu_ is held by caller
    switch (sync_mode_) {
    case AofSync::ALWAYS:
        file_.flush();
        {
            // Re-open in append mode to get an fd for fsync
            int fd = ::open(path_.c_str(), O_WRONLY | O_APPEND);
            if (fd >= 0) { ::fsync(fd); ::close(fd); }
        }
        break;
    case AofSync::EVERY_SECOND:
        need_fsync_.store(true, std::memory_order_release);
        break;
    case AofSync::NEVER:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// fsync_loop — background thread for EVERY_SECOND mode
// ─────────────────────────────────────────────────────────────────────────────

void AofWriter::fsync_loop() {
    using namespace std::chrono_literals;
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1s);
        if (need_fsync_.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard lock(mu_);
            if (file_.is_open()) {
                file_.flush();
                int fd = ::open(path_.c_str(), O_WRONLY | O_APPEND);
                if (fd >= 0) { ::fsync(fd); ::close(fd); }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// replay — parse AOF file and rebuild KvStore state
// ─────────────────────────────────────────────────────────────────────────────

int64_t AofWriter::replay(const std::string& path,
                           SetFn   set_fn,
                           SetExFn set_ex_fn,
                           DelFn   del_fn)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return 0; // No AOF yet is fine

    int64_t count = 0;

    // Simple RESP array reader (we only wrote valid RESP, so we can be strict)
    auto read_line = [&](std::string& line) -> bool {
        return static_cast<bool>(std::getline(f, line));
    };

    auto strip_cr = [](std::string& s) {
        if (!s.empty() && s.back() == '\r') s.pop_back();
    };

    auto read_bulk = [&](std::string& out) -> bool {
        std::string hdr;
        if (!read_line(hdr)) return false;
        strip_cr(hdr);
        if (hdr.empty() || hdr[0] != '$') return false;

        int64_t len = 0;
        try { len = std::stoll(hdr.substr(1)); } catch(...) { return false; }
        if (len < 0) { out = ""; return true; }

        out.resize(static_cast<size_t>(len));
        if (!f.read(out.data(), len)) return false;

        // Consume trailing \r\n
        std::string crlf;
        std::getline(f, crlf);
        return true;
    };

    while (f.peek() != EOF) {
        std::string hdr;
        if (!read_line(hdr)) break;
        strip_cr(hdr);
        if (hdr.empty()) continue;
        if (hdr[0] != '*') break; // malformed

        int nargs = 0;
        try { nargs = std::stoi(hdr.substr(1)); } catch(...) { break; }

        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(nargs));
        bool ok = true;
        for (int i = 0; i < nargs && ok; ++i) {
            std::string arg;
            ok = read_bulk(arg);
            if (ok) args.push_back(std::move(arg));
        }
        if (!ok || args.empty()) break;

        // Dispatch
        const std::string& cmd = args[0];
        if (cmd == "SET") {
            if (args.size() == 3) {
                set_fn(args[1], args[2]);
                ++count;
            } else if (args.size() == 5 && args[3] == "EX") {
                int64_t ttl = 0;
                try { ttl = std::stoll(args[4]); } catch(...) {}
                if (ttl > 0) { set_ex_fn(args[1], args[2], ttl); ++count; }
            }
        } else if (cmd == "DEL" && args.size() == 2) {
            del_fn(args[1]);
            ++count;
        }
    }

    std::cout << "[AofWriter] Replayed " << count << " commands from " << path << "\n";
    return count;
}

} // namespace store
