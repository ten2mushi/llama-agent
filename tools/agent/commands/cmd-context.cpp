#include "command-handler.h"
#include "../agent-loop.h"
#include "../context/context-manager.h"
#include "console.h"

namespace agent {

void register_context_commands(command_dispatcher& dispatcher) {
    // /clear - Clear conversation and start new context
    dispatcher.register_command("/clear", [](const std::string&, command_context& ctx) {
        ctx.agent.clear();
        ctx.current_context_id = ctx.ctx_mgr.create_context();
        ctx.agent.set_context_id(ctx.current_context_id);
        console::log("Conversation cleared. New context: %s\n",
                    ctx.current_context_id.substr(0, 8).c_str());
        return command_result::CONTINUE;
    });

    // /list - List saved conversations
    dispatcher.register_command("/list", [](const std::string&, command_context& ctx) {
        auto contexts = ctx.ctx_mgr.list_contexts();
        if (contexts.empty()) {
            console::log("\nNo saved conversations.\n");
        } else {
            console::log("\nSaved conversations:\n");
            for (const auto& c : contexts) {
                std::string marker = (c.id == ctx.current_context_id) ? " *" : "";
                console::log("  %s%s  [%d msgs]  %s\n",
                            c.id.substr(0, 8).c_str(),
                            marker.c_str(),
                            c.message_count,
                            c.preview.c_str());
            }
            console::log("\n  * = current context\n");
        }
        return command_result::CONTINUE;
    });

    // /switch <id> - Switch to a saved conversation
    dispatcher.register_command("/switch", [](const std::string& args, command_context& ctx) {
        std::string error_msg;
        std::string matched_id = find_context_by_prefix(ctx.ctx_mgr, args, error_msg);

        if (matched_id.empty()) {
            console::error("%s\n", error_msg.c_str());
            return command_result::CONTINUE;
        }

        auto state_opt = ctx.ctx_mgr.load_context(matched_id);
        if (state_opt) {
            ctx.current_context_id = matched_id;
            ctx.agent.set_messages(state_opt->messages);
            ctx.agent.set_context_id(ctx.current_context_id);
            console::log("Switched to context %s (%d messages)\n",
                        ctx.current_context_id.substr(0, 8).c_str(),
                        (int)state_opt->messages.size());
        } else {
            console::error("Failed to load context.\n");
        }
        return command_result::CONTINUE;
    });

    // /delete <id> - Delete a saved conversation
    dispatcher.register_command("/delete", [](const std::string& args, command_context& ctx) {
        std::string error_msg;
        std::string matched_id = find_context_by_prefix(ctx.ctx_mgr, args, error_msg);

        if (matched_id.empty()) {
            console::error("%s\n", error_msg.c_str());
            return command_result::CONTINUE;
        }

        // Prevent deleting current context
        if (matched_id == ctx.current_context_id) {
            console::error("Cannot delete current context. Use /clear first.\n");
            return command_result::CONTINUE;
        }

        if (ctx.ctx_mgr.delete_context(matched_id)) {
            console::log("Deleted context %s\n", matched_id.substr(0, 8).c_str());
        } else {
            console::error("Failed to delete context.\n");
        }
        return command_result::CONTINUE;
    });
}

} // namespace agent
