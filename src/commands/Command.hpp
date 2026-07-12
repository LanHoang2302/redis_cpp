#pragma once
/*
 * Command.hpp — Abstract Command interface (Command Pattern).
 *
 * Each Redis command (GET, SET, DEL, ...) implements this interface.
 * Commands are stateless objects; state lives in KvStore.
 */
#include <string>
#include <vector>

namespace store { class KvStore; }

namespace commands {

class Command {
public:
    virtual ~Command() = default;

    // Execute the command; returns a RESP-encoded response string.
    // args[0] is the command name (already upper-cased).
    virtual std::string execute(const std::vector<std::string>& args,
                                store::KvStore& store) = 0;

    // The command name in uppercase, e.g. "GET"
    virtual std::string_view name() const noexcept = 0;

    // Minimum and maximum argument counts (including command name)
    virtual int min_args() const noexcept = 0;
    virtual int max_args() const noexcept = 0; // -1 = unlimited
};

} // namespace commands
