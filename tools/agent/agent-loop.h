#pragma once

#include "tool-registry.h"
#include "permission.h"
#include "chat.h"
#include "common/agent-common.h"

#include "server-context.h"
#include "server-task.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

using agent::json;

// Forward declaration
class context_manager;

// Stop reasons for the agent loop
enum class agent_stop_reason {
    COMPLETED,        // Model finished without tool calls
    MAX_ITERATIONS,   // Hit iteration limit
    USER_CANCELLED,   // User interrupted
    AGENT_ERROR,      // Error occurred (not ERROR due to Windows macro conflict)
};

// Callback type for message persistence
using message_callback = std::function<void(const json& message)>;

/**
 * Configuration for the agent.
 *
 * All fields have sensible defaults. The minimum required configuration is:
 * - working_dir: The directory for file operations and path resolution
 *
 * Optional features:
 * - skills/agents_md: Enable project-specific behavior
 * - ctx_manager: Enable conversation persistence
 * - parent_permission_mgr: Share permissions with parent agent (for subagents)
 */
struct agent_config {
    int max_iterations = 50;          // Max tool execution rounds per run()
    int tool_timeout_ms = 120000;     // Timeout for individual tool calls (2 min)
    std::string working_dir;          // Base directory for file operations
    bool verbose = false;             // Enable verbose logging
    bool yolo_mode = false;           // Skip all permission prompts (dangerous!)

    // Skills configuration (agentskills.io spec)
    bool enable_skills = true;
    std::vector<std::string> skills_search_paths;  // Additional search paths
    std::string skills_prompt_section;             // Pre-generated XML for prompt injection

    // Persistence configuration
    context_manager* ctx_manager = nullptr;        // Optional: context manager for persistence
    std::string context_id;                        // Current conversation context ID
    std::string context_base_path;                 // Base path for context persistence (e.g., .llama-agent)
    message_callback on_message;                   // Callback when message is added

    // Permission inheritance for subagents
    permission_manager* parent_permission_mgr = nullptr;  // If set, use parent's permission state

    // Tool filtering for subagents (empty = all tools allowed)
    std::vector<std::string> allowed_tools;  // Whitelist of tool names

    // Subagent manager for spawn_agent tool (optional, allows nested spawning)
    class subagent_manager* subagent_mgr = nullptr;

    // Custom system prompt override for specialized agents
    // If non-empty, replaces the default system prompt entirely
    // Tool table is still appended automatically unless skip_tool_table is true
    std::string custom_system_prompt;
    bool skip_tool_table = false;  // If true, don't append tool table to system prompt
};

// Result from running the agent loop
struct agent_loop_result {
    agent_stop_reason stop_reason;
    std::string final_response;
    int iterations = 0;
};

// Session-level statistics for token tracking
struct session_stats {
    int32_t total_input = 0;       // Total prompt tokens processed
    int32_t total_output = 0;      // Total tokens generated
    int32_t total_cached = 0;      // Total tokens served from KV cache
    double total_prompt_ms = 0;    // Total prompt evaluation time
    double total_predicted_ms = 0; // Total generation time

    // Context usage tracking
    int32_t current_context_tokens = 0;  // Current context usage (prompt + output last turn)
    int32_t n_ctx = 0;                   // Total context size (set from server)
    bool warned_70 = false;              // Already warned at 70%
    bool warned_80 = false;              // Already warned at 80%
};

/**
 * The main agent loop class.
 *
 * Thread safety: NOT thread-safe. All methods must be called from the same thread.
 * The is_interrupted flag can be set from another thread to signal cancellation.
 *
 * Lifecycle:
 * 1. Construct with server context, params, and config
 * 2. Optionally call set_subagent_manager() for spawn_agent support
 * 3. Call run() with user prompts
 * 4. Call clear() to reset conversation state
 */
class agent_loop {
public:
    /**
     * Construct an agent loop.
     *
     * @param server_ctx The server context for LLM inference
     * @param params Common parameters (sampling, speculative, etc.)
     * @param config Agent configuration (working_dir, max_iterations, etc.)
     * @param is_interrupted Atomic flag for cancellation from other threads
     */
    agent_loop(server_context & server_ctx,
               const common_params & params,
               const agent_config & config,
               std::atomic<bool> & is_interrupted);

    /**
     * Run the agent loop with a user prompt.
     *
     * @param user_prompt Non-empty user message to process
     * @return Result with stop reason and final response
     *
     * Preconditions:
     * - user_prompt should not be empty (will still work but wastes a turn)
     * - is_interrupted should be false at start
     *
     * The agent will:
     * 1. Add user message to conversation
     * 2. Generate LLM response
     * 3. Execute any tool calls
     * 4. Repeat until completion, max iterations, or interruption
     */
    agent_loop_result run(const std::string & user_prompt);

    // Clear conversation history (optionally create new context)
    void clear();

    // Get current messages (for debugging/persistence)
    const json & get_messages() const { return messages_; }

    // Set messages (for loading from persistence)
    void set_messages(const json & messages);

    // Get session statistics
    const session_stats & get_stats() const { return stats_; }

    // Get current context ID
    const std::string & get_context_id() const { return config_.context_id; }

    // Set context ID (for switching contexts)
    void set_context_id(const std::string & id) {
        config_.context_id = id;
        tool_ctx_.context_id = id;
    }

    // Update the message callback (for context switching)
    void set_message_callback(const message_callback & callback) { config_.on_message = callback; }

    // Set subagent manager (for late binding after permission manager is available)
    void set_subagent_manager(class subagent_manager * mgr) {
        tool_ctx_.subagent_mgr = mgr;
        config_.subagent_mgr = mgr;
    }

private:
    // Generate a completion and get the parsed response with tool calls
    common_chat_msg generate_completion(result_timings & out_timings);

    // Execute a single tool call
    tool_result execute_tool_call(const common_chat_tool_call & call);

    // Format tool result as message
    void add_tool_result_message(const std::string & tool_name,
                                  const std::string & call_id,
                                  const tool_result & result);

    // Add a message and trigger persistence callback
    void add_message(const json & message);

    // Check if a tool is allowed (always true if allowed_tools is empty)
    bool is_tool_allowed(const std::string & tool_name) const;

    // Get filtered tools for completion (respects allowed_tools)
    std::vector<const tool_def*> get_allowed_tools() const;

    // Generate compact tool signature table for system prompt
    std::string generate_tool_table() const;

    server_context & server_ctx_;
    agent_config config_;
    std::atomic<bool> & is_interrupted_;

    json messages_;
    task_params task_defaults_;
    permission_manager owned_permission_mgr_;     // Our own permission manager (used if no parent)
    permission_manager* permission_mgr_ = nullptr; // Points to either owned or parent's
    tool_context tool_ctx_;
    session_stats stats_;

public:
    // Get permission manager (for passing to subagents)
    permission_manager* get_permission_manager() { return permission_mgr_; }
};
