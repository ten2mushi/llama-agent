#include "command-handler.h"

namespace agent {

void register_exit_commands(command_dispatcher& dispatcher) {
    // /exit - Exit the agent
    dispatcher.register_command("/exit", [](const std::string&, command_context&) {
        return command_result::EXIT;
    });

    // /quit - Alias for /exit
    dispatcher.register_command("/quit", [](const std::string&, command_context&) {
        return command_result::EXIT;
    });
}

} // namespace agent
