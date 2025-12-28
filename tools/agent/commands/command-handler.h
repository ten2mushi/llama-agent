#pragma once

// Command handler infrastructure for slash commands.
// Provides a map-based dispatch pattern for clean separation of concerns.

#include "../common/agent-common.h"

#include <atomic>
#include <functional>
#include <map>
#include <string>

// Forward declarations to avoid circular includes
class agent_loop;
class context_manager;
class skills_manager;
class agent_registry;
class subagent_manager;
struct server_context;
struct common_params;

namespace agent {

// Context passed to each command handler
struct command_context {
    agent_loop& agent;
    context_manager& ctx_mgr;
    skills_manager& skills_mgr;
    agent_registry& agent_reg;
    subagent_manager& subagent_mgr;
    server_context& server_ctx;
    const common_params& params;
    std::string& current_context_id;
    const std::string& working_dir;
    std::atomic<bool>& is_interrupted;
};

// Result of command execution
enum class command_result {
    CONTINUE,       // Continue main loop, get next input
    EXIT,           // Exit the agent
    RUN_PROMPT,     // Input is not a command, run as prompt
};

// Command handler function signature
using command_handler = std::function<command_result(
    const std::string& args,
    command_context& ctx
)>;

// Command dispatcher with map-based routing
class command_dispatcher {
public:
    // Register a command handler
    void register_command(const std::string& name, command_handler handler);

    // Dispatch a command (input should include the leading /)
    // Returns RUN_PROMPT if input is not a registered command
    command_result dispatch(const std::string& input, command_context& ctx);

    // Check if input starts with a registered command
    bool is_command(const std::string& input) const;

    // Get list of all registered command names
    std::vector<std::string> get_command_names() const;

private:
    std::map<std::string, command_handler> handlers_;
};

// Utility: Find context by prefix with error handling
// Returns matched context ID or empty string on error
// Sets error_msg on failure (no match or multiple matches)
std::string find_context_by_prefix(
    context_manager& ctx_mgr,
    const std::string& prefix,
    std::string& error_msg
);

// Registration functions - call these at startup
void register_exit_commands(command_dispatcher& dispatcher);
void register_context_commands(command_dispatcher& dispatcher);
void register_compact_command(command_dispatcher& dispatcher);
void register_plan_command(command_dispatcher& dispatcher);
void register_info_commands(command_dispatcher& dispatcher);

} // namespace agent
