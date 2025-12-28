#include "command-handler.h"
#include "../agent-loop.h"
#include "../tool-registry.h"
#include "../skills/skills-manager.h"
#include "../subagents/agent-registry.h"
#include "console.h"

namespace agent {

void register_info_commands(command_dispatcher& dispatcher) {
    // /tools - List available tools
    dispatcher.register_command("/tools", [](const std::string&, command_context&) {
        console::log("\nAvailable tools:\n");
        for (const auto* tool : tool_registry::instance().get_all_tools()) {
            console::log("  %s:\n", tool->name.c_str());
            console::log("    %s\n", tool->description.c_str());
        }
        return command_result::CONTINUE;
    });

    // /stats - Show token usage statistics
    dispatcher.register_command("/stats", [](const std::string&, command_context& ctx) {
        const auto& stats = ctx.agent.get_stats();
        console::log("\nSession Statistics:\n");
        console::log("  Prompt tokens:  %d\n", stats.total_input);
        console::log("  Output tokens:  %d\n", stats.total_output);
        if (stats.total_cached > 0) {
            console::log("  Cached tokens:  %d\n", stats.total_cached);
        }
        console::log("  Total tokens:   %d\n", stats.total_input + stats.total_output);
        if (stats.total_prompt_ms > 0) {
            console::log("  Prompt time:    %.2fs\n", stats.total_prompt_ms / 1000.0);
        }
        if (stats.total_predicted_ms > 0) {
            console::log("  Gen time:       %.2fs\n", stats.total_predicted_ms / 1000.0);
            double avg_speed = stats.total_output * 1000.0 / stats.total_predicted_ms;
            console::log("  Avg speed:      %.1f tok/s\n", avg_speed);
        }
        return command_result::CONTINUE;
    });

    // /skills - List available skills
    dispatcher.register_command("/skills", [](const std::string&, command_context& ctx) {
        const auto& skills = ctx.skills_mgr.get_skills();
        if (skills.empty()) {
            console::log("\nNo skills discovered.\n");
            console::log("Skills are loaded from:\n");
            console::log("  ./.llama-agent/skills/  (project-local)\n");
            console::log("  ~/.llama-agent/skills/  (user-global)\n");
        } else {
            console::log("\nAvailable skills:\n");
            for (const auto& skill : skills) {
                console::log("  %s:\n", skill.name.c_str());
                console::log("    %s\n", skill.description.c_str());
                console::log("    Path: %s\n", skill.path.c_str());
            }
        }
        return command_result::CONTINUE;
    });

    // /subagents - List available subagents
    dispatcher.register_command("/subagents", [](const std::string&, command_context& ctx) {
        const auto& agents = ctx.agent_reg.get_agents();
        if (agents.empty()) {
            console::log("\nNo subagents discovered.\n");
            console::log("Subagents are loaded from:\n");
            console::log("  ./.llama-agent/agents/  (project-local)\n");
            console::log("  ~/.llama-agent/agents/  (user-global)\n");
            console::log("\nCreate an AGENT.md file in a subdirectory to define a subagent.\n");
        } else {
            console::log("\nAvailable subagents:\n");
            for (const auto& ag : agents) {
                console::log("  %s:\n", ag.name.c_str());
                console::log("    %s\n", ag.description.c_str());
                if (!ag.allowed_tools.empty()) {
                    console::log("    Tools: ");
                    for (size_t i = 0; i < ag.allowed_tools.size(); i++) {
                        if (i > 0) console::log(", ");
                        console::log("%s", ag.allowed_tools[i].c_str());
                    }
                    console::log("\n");
                }
                console::log("    Path: %s\n", ag.path.c_str());
            }
        }
        return command_result::CONTINUE;
    });
}

} // namespace agent
