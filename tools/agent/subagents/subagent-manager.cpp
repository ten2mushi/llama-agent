#include "subagent-manager.h"
#include "../tool-registry.h"
#include "../common/constants.h"
#include "console.h"
#include "llama.h"

#include <chrono>
#include <sstream>

namespace fs = agent::fs;

subagent_manager::subagent_manager(server_context& server_ctx,
                                   const common_params& params,
                                   agent_registry& registry,
                                   context_manager& ctx_mgr,
                                   const std::string& working_dir,
                                   permission_manager* parent_permission_mgr)
    : server_ctx_(server_ctx)
    , params_(params)
    , registry_(registry)
    , ctx_mgr_(ctx_mgr)
    , working_dir_(working_dir)
    , permission_mgr_(parent_permission_mgr) {
}

bool subagent_manager::save_kv_state(std::vector<uint8_t>& out_state) {
    llama_context* ctx = server_ctx_.get_llama_context();
    if (!ctx) {
        return false;
    }

    // Get size needed for sequence 0 (main sequence)
    const llama_seq_id seq_id = 0;
    size_t state_size = llama_state_seq_get_size(ctx, seq_id);
    if (state_size == 0) {
        // No state to save (empty context)
        out_state.clear();
        return true;
    }

    out_state.resize(state_size);
    size_t written = llama_state_seq_get_data(ctx, out_state.data(), state_size, seq_id);
    if (written != state_size) {
        out_state.clear();
        return false;
    }

    return true;
}

bool subagent_manager::restore_kv_state(const std::vector<uint8_t>& state) {
    if (state.empty()) {
        // Nothing to restore
        return true;
    }

    llama_context* ctx = server_ctx_.get_llama_context();
    if (!ctx) {
        return false;
    }

    const llama_seq_id seq_id = 0;
    size_t read = llama_state_seq_set_data(ctx, state.data(), state.size(), seq_id);
    return read > 0;
}

void subagent_manager::clear_kv_cache() {
    // Use the proper slot clearing method that syncs both
    // the KV cache and the slot's internal token tracking
    server_ctx_.clear_current_slot();
}

std::vector<std::string> subagent_manager::filter_tools(
    const std::vector<std::string>& allowed_tools) {

    std::vector<std::string> result;

    if (allowed_tools.empty()) {
        return result;  // No tools allowed
    }

    // Get all available tools
    auto all_tools = tool_registry::instance().get_all_tools();

    // Filter to only allowed tools
    for (const auto* tool : all_tools) {
        for (const auto& allowed : allowed_tools) {
            if (tool->name == allowed) {
                result.push_back(tool->name);
                break;
            }
        }
    }

    return result;
}

std::string subagent_manager::generate_system_prompt(const agent_definition& agent) {
    std::ostringstream ss;

    ss << "You are " << agent.name << ", a specialized subagent.\n\n";

    // Add agent-specific instructions
    if (!agent.instructions.empty()) {
        ss << agent.instructions << "\n\n";
    }

    // Add tool information (progressive disclosure - signatures only)
    if (!agent.allowed_tools.empty()) {
        ss << "# Available Tools\n\n";
        ss << "| Tool | Signature | Description |\n";
        ss << "|------|-----------|-------------|\n";
        for (const auto& tool_name : agent.allowed_tools) {
            const auto* tool = tool_registry::instance().get_tool(tool_name);
            if (tool) {
                // Get first sentence of description for compact display
                std::string short_desc = tool->description;
                size_t period_pos = short_desc.find('.');
                if (period_pos != std::string::npos && period_pos < 100) {
                    short_desc = short_desc.substr(0, period_pos + 1);
                } else if (short_desc.length() > 80) {
                    short_desc = short_desc.substr(0, 77) + "...";
                }
                ss << "| " << tool->name << " | `" << tool->signature << "` | " << short_desc << " |\n";
            }
        }
        ss << "\nUse `describe_tool(tool_name)` for full parameter documentation.\n\n";
    } else {
        ss << "# No Tools Available\n\n";
        ss << "You do not have access to any tools. Please provide your analysis and response based on the context provided.\n\n";
    }

    // Add guidelines
    ss << "# Guidelines\n\n";
    ss << "- Focus on completing the task efficiently\n";
    ss << "- Be concise in your responses\n";
    ss << "- When finished, provide a clear summary of what you accomplished\n";

    return ss.str();
}

json subagent_manager::extract_artifacts(const json& messages) {
    json artifacts = json::object();

    // Look for structured JSON outputs in assistant messages
    // Note: Plans are now saved as markdown via /plan command, not extracted as JSON artifacts
    for (const auto& msg : messages) {
        if (msg.value("role", "") == "assistant") {
            std::string content = msg.value("content", "");

            // Find JSON code blocks (for generic structured data, not plans)
            size_t start = content.find("```json");
            if (start != std::string::npos) {
                size_t json_start = start + 7;
                if (json_start < content.size() && content[json_start] == '\n') {
                    json_start++;
                }
                size_t json_end = content.find("```", json_start);
                if (json_end != std::string::npos) {
                    std::string json_str = content.substr(json_start, json_end - json_start);
                    try {
                        json parsed = json::parse(json_str);
                        // Store as generic data artifact (not for plans)
                        if (!parsed.contains("questions")) {  // Skip Q&A blocks from planning
                            artifacts["data"] = parsed;
                        }
                    } catch (...) {
                        // Not valid JSON, ignore
                    }
                }
            }
        }
    }

    return artifacts;
}

void subagent_manager::extract_modifications(const json& messages,
                                              std::vector<std::string>& files_modified,
                                              std::vector<std::string>& commands_run) {
    files_modified.clear();
    commands_run.clear();

    for (const auto& msg : messages) {
        std::string role = msg.value("role", "");

        if (role == "assistant" && msg.contains("tool_calls")) {
            for (const auto& tc : msg["tool_calls"]) {
                std::string tool_name = tc.value("function", json::object()).value("name", "");
                std::string args_str = tc.value("function", json::object()).value("arguments", "");

                try {
                    json args = json::parse(args_str);

                    if (tool_name == "write" || tool_name == "edit") {
                        std::string path = args.value("file_path", "");
                        if (!path.empty()) {
                            // Deduplicate
                            bool found = false;
                            for (const auto& f : files_modified) {
                                if (f == path) { found = true; break; }
                            }
                            if (!found) files_modified.push_back(path);
                        }
                    } else if (tool_name == "bash") {
                        std::string cmd = args.value("command", "");
                        if (!cmd.empty()) {
                            // Truncate long commands
                            if (cmd.size() > 200) cmd = cmd.substr(0, 197) + "...";
                            commands_run.push_back(cmd);
                        }
                    } else if (tool_name == "spawn_agent") {
                        // Recursively include files from nested subagent results
                        // Look in the tool result message for this call
                        std::string call_id = tc.value("id", "");
                        for (const auto& result_msg : messages) {
                            if (result_msg.value("role", "") == "tool" &&
                                result_msg.value("tool_call_id", "") == call_id) {
                                std::string content = result_msg.value("content", "");
                                try {
                                    json result_json = json::parse(content);
                                    if (result_json.contains("files_modified")) {
                                        for (const auto& f : result_json["files_modified"]) {
                                            std::string path = f.get<std::string>();
                                            bool found = false;
                                            for (const auto& existing : files_modified) {
                                                if (existing == path) { found = true; break; }
                                            }
                                            if (!found) files_modified.push_back(path);
                                        }
                                    }
                                    if (result_json.contains("commands_run")) {
                                        for (const auto& c : result_json["commands_run"]) {
                                            commands_run.push_back(c.get<std::string>());
                                        }
                                    }
                                } catch (...) {
                                    // Not valid JSON or missing fields, skip
                                }
                                break;
                            }
                        }
                    }
                } catch (...) {
                    // JSON parse error, skip this tool call
                }
            }
        }
    }
}

subagent_result subagent_manager::spawn(const subagent_request& request,
                                         const json& /* parent_messages */,
                                         std::atomic<bool>& is_interrupted) {
    subagent_result result;

    // Check spawn depth limit to prevent runaway recursion
    if (request.spawn_depth >= MAX_SPAWN_DEPTH) {
        result.error = "Maximum spawn depth (" + std::to_string(MAX_SPAWN_DEPTH) +
                       ") exceeded. Cannot spawn more subagents.";
        return result;
    }

    // Find the agent definition
    const agent_definition* agent_def = registry_.get_agent(request.agent_name);
    if (!agent_def) {
        result.error = "Unknown agent: " + request.agent_name;
        return result;
    }

    // Track timing for display
    auto spawn_start = std::chrono::steady_clock::now();

    // Push into subagent visual context (shows framed header)
    int max_iter = request.max_iterations > 0 ?
                   request.max_iterations : agent_def->max_iterations;
    console::subagent::push_depth(request.agent_name, max_iter);

    // === CONTEXT ISOLATION: Clear slot for fresh subagent context ===
    // Note: We don't save/restore parent KV cache - the parent will need to
    // re-process its prompt after subagent returns. This is simpler and safer
    // than trying to serialize/deserialize KV state which has sync issues.
    clear_kv_cache();

    // === SPAWN DEPTH TRACKING: Push onto stack ===
    spawn_depth_stack_.push(request.spawn_depth + 1);

    // Create subagent context for persistence (optional)
    context_id sub_ctx_id;
    if (request.persist) {
        sub_ctx_id = ctx_mgr_.create_context();
    }

    // Configure subagent
    agent_config sub_config;

    // Determine subagent working directory
    std::string sub_working_dir;
    if (!request.working_dir.empty()) {
        sub_working_dir = resolve_path(request.working_dir, working_dir_);
        if (sub_working_dir.empty()) {
            result.error = "working_dir does not exist or is not a directory: " + request.working_dir;
            spawn_depth_stack_.pop();
            console::subagent::pop_depth(0, 0);
            return result;
        }
    } else {
        sub_working_dir = working_dir_;  // Inherit from parent
    }
    sub_config.working_dir = sub_working_dir;

    sub_config.max_iterations = request.max_iterations > 0 ?
                                 request.max_iterations : agent_def->max_iterations;
    sub_config.tool_timeout_ms = agent::config::DEFAULT_TOOL_TIMEOUT_MS;
    sub_config.verbose = false;
    sub_config.yolo_mode = false;

    // Inherit parent's permission manager for shared session state
    sub_config.parent_permission_mgr = permission_mgr_;

    // === NESTED SUBAGENT SUPPORT: Pass ourselves for spawn_agent tool ===
    sub_config.subagent_mgr = this;

    // === CONTEXT BASE PATH: Propagate for tools like read_plan ===
    sub_config.context_base_path = ctx_mgr_.base_path();

    // === TOOL FILTERING: Apply allowed_tools from AGENT.md ===
    sub_config.allowed_tools = agent_def->allowed_tools;

    // Set up persistence callback if enabled
    if (request.persist && !sub_ctx_id.empty()) {
        sub_config.ctx_manager = &ctx_mgr_;
        sub_config.context_id = sub_ctx_id;
        sub_config.on_message = [this, sub_ctx_id](const json& msg) {
            ctx_mgr_.append_message(sub_ctx_id, msg);
        };
    }

    // Create subagent loop
    agent_loop subagent(server_ctx_, params_, sub_config, is_interrupted);

    // Build the task prompt
    std::string task_prompt = request.task;
    if (!request.context.empty()) {
        task_prompt += "\n\n## Context\n\n```json\n";
        task_prompt += request.context.dump(2);
        task_prompt += "\n```";
    }

    // Run the subagent
    // Note: The system prompt is built into agent_loop constructor
    // For now, we prepend the agent instructions to the task
    std::string full_prompt = generate_system_prompt(*agent_def) + "\n\n# Task\n\n" + task_prompt;

    agent_loop_result loop_result = subagent.run(full_prompt);

    // Capture results
    result.success = (loop_result.stop_reason == agent_stop_reason::COMPLETED);
    result.output = loop_result.final_response;
    result.iterations = loop_result.iterations;
    result.stats = subagent.get_stats();
    last_messages_ = subagent.get_messages();

    // Extract any artifacts
    result.artifacts = extract_artifacts(last_messages_);

    // Extract file modifications and commands for parent context awareness
    extract_modifications(last_messages_, result.files_modified, result.commands_run);

    // Set error message if failed
    if (!result.success) {
        switch (loop_result.stop_reason) {
            case agent_stop_reason::MAX_ITERATIONS:
                result.error = "Subagent reached max iterations";
                break;
            case agent_stop_reason::USER_CANCELLED:
                result.error = "Subagent was cancelled";
                break;
            case agent_stop_reason::AGENT_ERROR:
                result.error = "Subagent encountered an error";
                break;
            default:
                break;
        }
    }

    // === SPAWN DEPTH TRACKING: Pop from stack ===
    if (!spawn_depth_stack_.empty()) {
        spawn_depth_stack_.pop();
    }

    // === CONTEXT ISOLATION: Clear subagent's state ===
    // Parent will re-process its context from messages on next completion
    clear_kv_cache();

    // Calculate elapsed time and pop from visual context (shows framed footer)
    auto spawn_end = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(spawn_end - spawn_start).count();
    console::subagent::pop_depth(result.iterations, elapsed_ms);

    return result;
}

std::string subagent_manager::resolve_path(const std::string& path,
                                            const std::string& base) {
    fs::path p(path);
    if (p.is_relative()) {
        p = fs::path(base) / p;
    }

    std::error_code ec;
    fs::path canonical = fs::canonical(p, ec);
    if (ec || !fs::is_directory(canonical)) {
        return "";  // Signal error - path doesn't exist or isn't a directory
    }

    return canonical.string();
}
