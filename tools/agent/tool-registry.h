#pragma once

#include "chat.h"
#include "common/agent-common.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

using agent::json;

// Forward declaration
class subagent_manager;

// Tool execution context passed to each tool
struct tool_context {
    std::string working_dir;
    std::atomic<bool> * is_interrupted = nullptr;
    int timeout_ms = 120000;

    // Context persistence base path (set from --data-dir or defaults to working_dir/.llama-agent)
    // This is the canonical path for all context CRUD operations
    std::string context_base_path;

    // Current conversation context ID (for tools that need to access context-specific data)
    std::string context_id;

    // Optional: subagent manager for spawn_agent tool
    subagent_manager * subagent_mgr = nullptr;
};

/**
 * Result of a tool execution.
 *
 * Contract:
 * - success=true: output contains result, error should be empty
 * - success=false: error contains message (required), output may contain partial result
 *
 * Callers should check success first, then use either output or error accordingly.
 */
struct tool_result {
    bool success = true;
    std::string output;  // Result data (valid when success=true)
    std::string error;   // Error message (valid when success=false)
};

// Tool definition
struct tool_def {
    std::string name;
    std::string description;
    std::string signature;   // Compact signature: "bash(command: string, timeout?: int)"
    std::string parameters;  // JSON schema string

    std::function<tool_result(const json &, const tool_context &)> execute;

    // Convert to common_chat_tool for llama.cpp infrastructure
    common_chat_tool to_chat_tool() const {
        return {name, description, parameters};
    }
};

// Singleton tool registry
class tool_registry {
public:
    static tool_registry & instance();

    // Register a tool
    void register_tool(const tool_def & tool);

    // Get tool by name
    const tool_def * get_tool(const std::string & name) const;

    // Get all registered tools
    std::vector<const tool_def *> get_all_tools() const;

    // Convert all tools to common_chat_tool format
    std::vector<common_chat_tool> to_chat_tools() const;

    // Execute a tool by name
    tool_result execute(const std::string & name, const json & args, const tool_context & ctx) const;

private:
    tool_registry() = default;
    std::map<std::string, tool_def> tools_;
};

// Helper macro for tool auto-registration
struct tool_registrar {
    tool_registrar(const tool_def & tool) {
        tool_registry::instance().register_tool(tool);
    }
};

#define REGISTER_TOOL(name, tool_instance) \
    static tool_registrar _tool_reg_##name(tool_instance)
