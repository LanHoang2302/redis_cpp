#pragma once
/*
 * CommandDispatcher.hpp — Maps command name → Command implementation.
 * Validates argument counts and delegates execution.
 */
#include "Command.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace store { class KvStore; }

namespace commands {

class CommandDispatcher {
public:
    CommandDispatcher();

    // Dispatch a parsed command to its handler.
    // args[0] = command name (upper-cased), args[1..] = arguments.
    // Returns a RESP-encoded response string.
    std::string dispatch(const std::vector<std::string>& args,
                         store::KvStore& store) const;

    // Register a new command (for extensibility)
    void register_command(std::unique_ptr<Command> cmd);

private:
    std::unordered_map<std::string, std::unique_ptr<Command>> commands_;
};

} // namespace commands
