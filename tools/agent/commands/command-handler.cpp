#include "command-handler.h"
#include "../context/context-manager.h"

namespace agent {

void command_dispatcher::register_command(const std::string& name, command_handler handler) {
    handlers_[name] = std::move(handler);
}

command_result command_dispatcher::dispatch(const std::string& input, command_context& ctx) {
    // Find matching command
    for (const auto& [cmd_name, handler] : handlers_) {
        if (input == cmd_name) {
            // Command with no arguments
            return handler("", ctx);
        }
        if (input.rfind(cmd_name + " ", 0) == 0) {
            // Command with arguments
            std::string args = input.substr(cmd_name.length() + 1);
            return handler(args, ctx);
        }
    }

    // Not a recognized command
    return command_result::RUN_PROMPT;
}

bool command_dispatcher::is_command(const std::string& input) const {
    if (input.empty() || input[0] != '/') {
        return false;
    }

    for (const auto& [cmd_name, _] : handlers_) {
        if (input == cmd_name || input.rfind(cmd_name + " ", 0) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> command_dispatcher::get_command_names() const {
    std::vector<std::string> names;
    names.reserve(handlers_.size());
    for (const auto& [name, _] : handlers_) {
        names.push_back(name);
    }
    return names;
}

std::string find_context_by_prefix(
    context_manager& ctx_mgr,
    const std::string& prefix,
    std::string& error_msg
) {
    // Trim whitespace from prefix
    std::string target = prefix;
    while (!target.empty() && std::isspace(target.front())) {
        target.erase(0, 1);
    }
    while (!target.empty() && std::isspace(target.back())) {
        target.pop_back();
    }

    if (target.empty()) {
        error_msg = "No context ID specified.";
        return "";
    }

    // Find matching context(s)
    auto contexts = ctx_mgr.list_contexts();
    std::string matched_id;
    int match_count = 0;

    for (const auto& ctx : contexts) {
        if (ctx.id.rfind(target, 0) == 0) {
            matched_id = ctx.id;
            match_count++;
        }
    }

    if (match_count == 0) {
        error_msg = "No context matching '" + target + "' found.";
        return "";
    }
    if (match_count > 1) {
        error_msg = "Multiple contexts match '" + target + "'. Be more specific.";
        return "";
    }

    return matched_id;
}

} // namespace agent
