#pragma once

#include "agent-registry.h"
#include "../agent-loop.h"
#include "../context/context-manager.h"

#include <atomic>
#include <stack>
#include <string>
#include <vector>

// Maximum spawn depth to prevent runaway recursion
constexpr int MAX_SPAWN_DEPTH = 3;

// Request to spawn a subagent
struct subagent_request {
    std::string agent_name;            // Name of agent to spawn
    std::string task;                  // Task for the subagent
    json context;                      // Additional context to pass
    int max_iterations = 20;           // Max iterations (overrides agent default)
    bool persist = false;              // Whether to save subagent state
    int spawn_depth = 0;               // Current spawn depth (0 = main agent spawning)
    std::string working_dir;           // Override working directory (empty = inherit)
};

// Result from subagent execution
struct subagent_result {
    bool success = false;
    std::string output;                // Final response from subagent
    json artifacts;                    // Structured outputs (plan.json, etc.)
    int iterations = 0;
    session_stats stats;
    std::string error;                 // Error message if failed

    // Tracked modifications for parent context awareness
    std::vector<std::string> files_modified;   // Files written/edited by subagent
    std::vector<std::string> commands_run;     // Bash commands executed by subagent
};

// Forward declarations
struct server_context;
struct common_params;

// Forward declarations
class permission_manager;

// Manages subagent spawning and execution
class subagent_manager {
public:
    subagent_manager(server_context& server_ctx,
                     const common_params& params,
                     agent_registry& registry,
                     context_manager& ctx_mgr,
                     const std::string& working_dir,
                     permission_manager* parent_permission_mgr = nullptr);

    // Set permission manager (for late binding after agent_loop is created)
    void set_permission_manager(permission_manager* mgr) { permission_mgr_ = mgr; }

    // Spawn a subagent and wait for result
    // parent_messages: current messages from parent agent (for saving/restoring)
    subagent_result spawn(const subagent_request& request,
                          const json& parent_messages,
                          std::atomic<bool>& is_interrupted);

    // Get the last subagent's messages (for debugging)
    const json& get_last_messages() const { return last_messages_; }

    // Get current spawn depth (for nested spawning)
    int get_current_spawn_depth() const {
        return spawn_depth_stack_.empty() ? 0 : spawn_depth_stack_.top();
    }

    // Generate system prompt for an agent (useful for direct agent_loop usage)
    std::string generate_system_prompt(const agent_definition& agent);

private:
    server_context& server_ctx_;
    const common_params& params_;
    agent_registry& registry_;
    context_manager& ctx_mgr_;
    std::string working_dir_;
    permission_manager* permission_mgr_ = nullptr;

    json last_messages_;
    std::stack<int> spawn_depth_stack_;  // Stack-based spawn depth tracking

    // Save/restore parent KV cache state
    bool save_kv_state(std::vector<uint8_t>& out_state);
    bool restore_kv_state(const std::vector<uint8_t>& state);
    void clear_kv_cache();

    // Build filtered tool list based on agent's allowed_tools
    std::vector<std::string> filter_tools(const std::vector<std::string>& allowed_tools);

    // Extract artifacts from subagent messages
    json extract_artifacts(const json& messages);

    // Extract file modifications and commands from messages
    void extract_modifications(const json& messages,
                               std::vector<std::string>& files_modified,
                               std::vector<std::string>& commands_run);

    // Resolve path relative to base directory
    std::string resolve_path(const std::string& path, const std::string& base);
};
