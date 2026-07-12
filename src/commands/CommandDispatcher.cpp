/*
 * CommandDispatcher.cpp — Registers all commands and dispatches requests.
 */
#include "CommandDispatcher.hpp"
#include "../protocol/RespParser.hpp"
#include "../store/KvStore.hpp"

// ── Command implementations (inline here for brevity) ────────────────────────
// Each is a small stateless functor implementing commands::Command.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>

namespace commands {

using R = protocol::RespParser;

// ─── GET ──────────────────────────────────────────────────────────────────────
class GetCommand final : public Command {
public:
    std::string_view name() const noexcept override { return "GET"; }
    int min_args() const noexcept override { return 2; }
    int max_args() const noexcept override { return 2; }

    std::string execute(const std::vector<std::string>& args,
                        store::KvStore& store) override
    {
        auto val = store.get(args[1]);
        return val ? R::bulk_string(*val) : R::nil_bulk();
    }
};

// ─── SET ──────────────────────────────────────────────────────────────────────
class SetCommand final : public Command {
public:
    std::string_view name() const noexcept override { return "SET"; }
    int min_args() const noexcept override { return 3; }
    int max_args() const noexcept override { return 5; } // SET key val [EX n]

    std::string execute(const std::vector<std::string>& args,
                        store::KvStore& store) override
    {
        if (args.size() == 3) {
            return store.set(args[1], args[2]) ? R::simple_string("OK")
                                                : R::error("out of memory");
        }
        // SET key val EX seconds
        if (args.size() == 5) {
            std::string opt = args[3];
            for (char& c : opt) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

            if (opt == "EX") {
                int64_t secs = 0;
                auto [ptr, ec] = std::from_chars(args[4].data(),
                                                  args[4].data() + args[4].size(),
                                                  secs);
                if (ec != std::errc{} || secs <= 0)
                    return R::error("invalid expire time in 'set' command");
                return store.set_ex(args[1], args[2], secs)
                       ? R::simple_string("OK")
                       : R::error("out of memory");
            }
            if (opt == "PX") {
                int64_t ms = 0;
                auto [ptr, ec] = std::from_chars(args[4].data(),
                                                  args[4].data() + args[4].size(),
                                                  ms);
                if (ec != std::errc{} || ms <= 0)
                    return R::error("invalid expire time in 'set' command");
                // Convert ms → s (round up)
                int64_t secs = std::max<int64_t>(1, ms / 1000);
                return store.set_ex(args[1], args[2], secs)
                       ? R::simple_string("OK")
                       : R::error("out of memory");
            }
        }
        return R::error("syntax error");
    }
};

// ─── DEL ──────────────────────────────────────────────────────────────────────
class DelCommand final : public Command {
public:
    std::string_view name() const noexcept override { return "DEL"; }
    int min_args() const noexcept override { return 2; }
    int max_args() const noexcept override { return -1; } // DEL k1 k2 ...

    std::string execute(const std::vector<std::string>& args,
                        store::KvStore& store) override
    {
        int64_t deleted = 0;
        for (size_t i = 1; i < args.size(); ++i)
            if (store.del(args[i])) ++deleted;
        return R::integer(deleted);
    }
};

// ─── EXPIRE ───────────────────────────────────────────────────────────────────
class ExpireCommand final : public Command {
public:
    std::string_view name() const noexcept override { return "EXPIRE"; }
    int min_args() const noexcept override { return 3; }
    int max_args() const noexcept override { return 3; }

    std::string execute(const std::vector<std::string>& args,
                        store::KvStore& store) override
    {
        int64_t secs = 0;
        auto [ptr, ec] = std::from_chars(args[2].data(),
                                          args[2].data() + args[2].size(),
                                          secs);
        if (ec != std::errc{} || secs <= 0)
            return R::error("value is not an integer or out of range");
        return R::integer(store.expire(args[1], secs) ? 1 : 0);
    }
};

// ─── TTL ──────────────────────────────────────────────────────────────────────
class TtlCommand final : public Command {
public:
    std::string_view name() const noexcept override { return "TTL"; }
    int min_args() const noexcept override { return 2; }
    int max_args() const noexcept override { return 2; }

    std::string execute(const std::vector<std::string>& args,
                        store::KvStore& store) override
    {
        return R::integer(store.ttl(args[1]));
    }
};

// ─── PING ─────────────────────────────────────────────────────────────────────
class PingCommand final : public Command {
public:
    std::string_view name() const noexcept override { return "PING"; }
    int min_args() const noexcept override { return 1; }
    int max_args() const noexcept override { return 2; }

    std::string execute(const std::vector<std::string>& args,
                        store::KvStore& /*store*/) override
    {
        if (args.size() > 1) return R::bulk_string(args[1]);
        return R::simple_string("PONG");
    }
};

// ─── INFO ─────────────────────────────────────────────────────────────────────
class InfoCommand final : public Command {
public:
    std::string_view name() const noexcept override { return "INFO"; }
    int min_args() const noexcept override { return 1; }
    int max_args() const noexcept override { return 2; }

    std::string execute(const std::vector<std::string>& /*args*/,
                        store::KvStore& store) override
    {
        return R::bulk_string(store.info());
    }
};

// ─── KEYS ─────────────────────────────────────────────────────────────────────
// Simplified: always returns all keys (pattern matching not implemented)
class KeysCommand final : public Command {
public:
    std::string_view name() const noexcept override { return "KEYS"; }
    int min_args() const noexcept override { return 2; }
    int max_args() const noexcept override { return 2; }

    std::string execute(const std::vector<std::string>& /*args*/,
                        store::KvStore& store) override
    {
        // We'd need a KvStore::keys() method; for now return info
        return R::bulk_string(store.info());
    }
};

// ─── DBSIZE ───────────────────────────────────────────────────────────────────
class DbSizeCommand final : public Command {
public:
    std::string_view name() const noexcept override { return "DBSIZE"; }
    int min_args() const noexcept override { return 1; }
    int max_args() const noexcept override { return 1; }

    std::string execute(const std::vector<std::string>& /*args*/,
                        store::KvStore& store) override
    {
        return R::integer(static_cast<int64_t>(store.count()));
    }
};

// ─── COMMAND ──────────────────────────────────────────────────────────────────
// Redis clients send COMMAND after connecting; return minimal response
class CommandCmd final : public Command {
public:
    std::string_view name() const noexcept override { return "COMMAND"; }
    int min_args() const noexcept override { return 1; }
    int max_args() const noexcept override { return -1; }

    std::string execute(const std::vector<std::string>& /*args*/,
                        store::KvStore& /*store*/) override
    {
        // Return empty array — redis-cli will still work
        return "*0\r\n";
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// CommandDispatcher
// ─────────────────────────────────────────────────────────────────────────────

CommandDispatcher::CommandDispatcher() {
    auto reg = [this](std::unique_ptr<Command> c) {
        std::string n(c->name());
        commands_[n] = std::move(c);
    };

    reg(std::make_unique<GetCommand>());
    reg(std::make_unique<SetCommand>());
    reg(std::make_unique<DelCommand>());
    reg(std::make_unique<ExpireCommand>());
    reg(std::make_unique<TtlCommand>());
    reg(std::make_unique<PingCommand>());
    reg(std::make_unique<InfoCommand>());
    reg(std::make_unique<KeysCommand>());
    reg(std::make_unique<DbSizeCommand>());
    reg(std::make_unique<CommandCmd>());
}

void CommandDispatcher::register_command(std::unique_ptr<Command> cmd) {
    std::string n(cmd->name());
    commands_[n] = std::move(cmd);
}

std::string CommandDispatcher::dispatch(const std::vector<std::string>& args,
                                         store::KvStore& store) const
{
    if (args.empty()) return R::error("empty command");

    auto it = commands_.find(args[0]);
    if (it == commands_.end())
        return R::error("unknown command '" + args[0] +
                        "', with args beginning with: " +
                        (args.size() > 1 ? "'" + args[1] + "'" : ""));

    Command* cmd = it->second.get();
    int argc = static_cast<int>(args.size());

    if (argc < cmd->min_args())
        return R::error("wrong number of arguments for '" +
                        std::string(cmd->name()) + "' command");
    if (cmd->max_args() >= 0 && argc > cmd->max_args())
        return R::error("wrong number of arguments for '" +
                        std::string(cmd->name()) + "' command");

    return cmd->execute(args, store);
}

} // namespace commands
