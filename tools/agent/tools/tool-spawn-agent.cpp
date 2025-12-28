#include "../tool-registry.h"
#include "../subagents/subagent-manager.h"

#include <sstream>

static tool_result spawn_agent_execute(const json& args, const tool_context& ctx) {
    if (!ctx.subagent_mgr) {
        return {false, "", "Subagent manager not available in this context"};
    }

    // Parse arguments
    std::string agent_name = args.value("agent_name", "");
    std::string task = args.value("task", "");
    json context = args.value("context", json::object());
    int max_iterations = args.value("max_iterations", 20);

    if (agent_name.empty()) {
        return {false, "", "agent_name is required"};
    }
    if (task.empty()) {
        return {false, "", "task is required"};
    }

    // Build request
    subagent_request req;
    req.agent_name = agent_name;
    req.task = task;
    req.context = context;
    req.max_iterations = max_iterations;
    req.persist = false;  // Don't persist subagent state by default
    req.spawn_depth = ctx.subagent_mgr->get_current_spawn_depth();  // Inherit current depth
    req.working_dir = args.value("working_dir", "");  // Optional: scope to specific directory

    // Spawn the subagent
    std::atomic<bool> local_interrupted{false};
    if (ctx.is_interrupted) {
        // Use parent's interrupt flag if available
        subagent_result result = ctx.subagent_mgr->spawn(req, json::array(), *ctx.is_interrupted);

        if (!result.success) {
            // Include any output along with error
            std::string error_msg = result.error;
            if (!result.output.empty()) {
                error_msg = result.output + "\n\nError: " + result.error;
            }
            return {false, "", error_msg};
        }

        // Format success result
        json output;
        output["agent"] = agent_name;
        output["result"] = result.output;
        output["iterations"] = result.iterations;
        output["stats"] = {
            {"input_tokens", result.stats.total_input},
            {"output_tokens", result.stats.total_output}
        };

        if (!result.artifacts.empty()) {
            output["artifacts"] = result.artifacts;
        }

        // Include file modifications for parent context tracking
        if (!result.files_modified.empty()) {
            output["files_modified"] = result.files_modified;
        }
        if (!result.commands_run.empty()) {
            output["commands_run"] = result.commands_run;
        }

        return {true, output.dump(2), ""};
    } else {
        subagent_result result = ctx.subagent_mgr->spawn(req, json::array(), local_interrupted);

        if (!result.success) {
            return {false, "", result.error.empty() ? "Subagent failed" : result.error};
        }

        json output;
        output["agent"] = agent_name;
        output["result"] = result.output;
        output["iterations"] = result.iterations;

        if (!result.artifacts.empty()) {
            output["artifacts"] = result.artifacts;
        }

        // Include file modifications for parent context tracking
        if (!result.files_modified.empty()) {
            output["files_modified"] = result.files_modified;
        }
        if (!result.commands_run.empty()) {
            output["commands_run"] = result.commands_run;
        }

        return {true, output.dump(2), ""};
    }
}

static tool_def spawn_agent_tool = {
    "spawn_agent",
    R"(Spawn a subagent to perform a specialized task with a fresh context.

The subagent runs with its own context window, preventing pollution of the main agent's context.
Results are returned to the main agent upon completion.

Use this when:
- A task requires deep exploration that would pollute main context
- Specialized behavior (planning, code review, etc.) is needed
- You want to delegate a focused subtask

Available agents can be discovered from AGENT.md files in:
- ./.llama-agent/agents/ (project-local)
- ~/.llama-agent/agents/ (user-global))",
    "spawn_agent(agent_name: string, task: string, context?: object, max_iterations?: int, working_dir?: string)",
    R"json({
        "type": "object",
        "properties": {
            "agent_name": {
                "type": "string",
                "description": "Name of the agent to spawn (e.g., 'explorer-agent', 'planning-agent')"
            },
            "task": {
                "type": "string",
                "description": "The task for the subagent to perform"
            },
            "context": {
                "type": "object",
                "description": "Additional context to pass to the subagent (optional)"
            },
            "max_iterations": {
                "type": "integer",
                "description": "Maximum iterations for the subagent (default: 20)"
            },
            "working_dir": {
                "type": "string",
                "description": "Scope subagent to this directory (relative to current or absolute). Access outside triggers user permission."
            }
        },
        "required": ["agent_name", "task"]
    })json",
    spawn_agent_execute
};

REGISTER_TOOL(spawn_agent, spawn_agent_tool);
