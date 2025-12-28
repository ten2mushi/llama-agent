#include "../tool-registry.h"
#include "../common/agent-common.h"

#include <fstream>
#include <sstream>

namespace fs = agent::fs;

// Helper to find most recently modified plan in a contexts directory
static std::string find_most_recent_plan(const std::string& contexts_dir, std::string& out_context_id) {
    if (!fs::exists(contexts_dir) || !fs::is_directory(contexts_dir)) {
        return "";
    }

    std::string best_path;
    std::filesystem::file_time_type best_time;
    bool found = false;

    for (const auto& entry : fs::directory_iterator(contexts_dir)) {
        if (entry.is_directory()) {
            std::string candidate = entry.path().string() + "/plan.md";
            if (fs::exists(candidate)) {
                auto mtime = fs::last_write_time(candidate);
                if (!found || mtime > best_time) {
                    best_path = candidate;
                    best_time = mtime;
                    out_context_id = entry.path().filename().string();
                    found = true;
                }
            }
        }
    }

    return best_path;
}

static tool_result read_plan_execute(const json& args, const tool_context& ctx) {
    std::string context_id = args.value("context_id", "");

    // Use the canonical context_base_path from tool context
    // This is set from --data-dir CLI arg or defaults to working_dir/.llama-agent
    std::string base_path = ctx.context_base_path;

    // Fallback for legacy compatibility (shouldn't happen in practice)
    if (base_path.empty()) {
        base_path = ctx.working_dir + "/.llama-agent";
    }

    std::string contexts_dir = base_path + "/contexts";
    std::string plan_path;

    // Priority: explicit arg > current context > most recent
    if (context_id.empty() && !ctx.context_id.empty()) {
        // Use current conversation's context
        context_id = ctx.context_id;
    }

    if (context_id.empty()) {
        // No context_id from args or tool context - find most recent plan
        plan_path = find_most_recent_plan(contexts_dir, context_id);

        if (plan_path.empty()) {
            return {false, "", "No plans found in: " + contexts_dir + "\n"
                              "Use context_id parameter to specify a specific plan."};
        }
    } else {
        // Context ID provided (from args or current context) - look for plan in that context
        plan_path = contexts_dir + "/" + context_id + "/plan.md";
    }

    // Check if plan.md exists
    if (!fs::exists(plan_path)) {
        // Check for legacy plan.json format
        std::string json_path = contexts_dir + "/" + context_id + "/plan.json";
        if (fs::exists(json_path)) {
            std::ifstream f(json_path);
            if (!f) {
                return {false, "", "Failed to read legacy plan.json for context: " + context_id};
            }
            std::stringstream buffer;
            buffer << f.rdbuf();

            return {true, "Note: This is a legacy JSON plan format.\n\n" + buffer.str(), ""};
        }

        return {false, "", "No plan found for context: " + context_id + "\n"
                          "Expected path: " + plan_path};
    }

    // Read plan content
    std::ifstream f(plan_path);
    if (!f) {
        return {false, "", "Failed to read plan file: " + plan_path};
    }

    std::stringstream buffer;
    buffer << f.rdbuf();

    std::string content = buffer.str();
    if (content.empty()) {
        return {false, "", "Plan file is empty: " + plan_path};
    }

    // Return with metadata header
    std::string result = "# Plan from context: " + context_id + "\n";
    result += "# Path: " + plan_path + "\n\n";
    result += content;

    return {true, result, ""};
}

static tool_def read_plan_tool = {
    "read_plan",
    "Read the implementation plan for a context. Returns the plan.md content which contains "
    "the implementation strategy, phases, design decisions, and success criteria. "
    "If no context_id is provided, finds the most recent plan.",
    "read_plan(context_id?: string)",
    R"json({
        "type": "object",
        "properties": {
            "context_id": {
                "type": "string",
                "description": "The context ID to read the plan from. If omitted, finds the most recent plan."
            }
        }
    })json",
    read_plan_execute
};

REGISTER_TOOL(read_plan, read_plan_tool);
