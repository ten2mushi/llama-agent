#include "../tool-registry.h"

#include <sstream>

static tool_result describe_tool_execute(const json& args, const tool_context& ctx) {
    (void)ctx;  // Unused

    std::string tool_name = args.value("tool_name", "");

    if (tool_name.empty()) {
        return {false, "", "tool_name is required"};
    }

    const tool_def* tool = tool_registry::instance().get_tool(tool_name);
    if (!tool) {
        // List available tools if not found
        std::ostringstream ss;
        ss << "Unknown tool: " << tool_name << "\n\nAvailable tools:\n";
        for (const auto* t : tool_registry::instance().get_all_tools()) {
            ss << "  - " << t->name << "\n";
        }
        return {false, "", ss.str()};
    }

    // Format full tool documentation
    std::ostringstream ss;
    ss << "# " << tool->name << "\n\n";
    ss << tool->description << "\n\n";
    ss << "## Signature\n\n";
    ss << "`" << tool->signature << "`\n\n";
    ss << "## JSON Schema\n\n";
    ss << "```json\n" << tool->parameters << "\n```\n";

    return {true, ss.str(), ""};
}

static tool_def describe_tool_def = {
    "describe_tool",
    "Get full JSON schema and documentation for a tool. Use this when you need detailed parameter information beyond the signature.",
    "describe_tool(tool_name: string)",
    R"json({
        "type": "object",
        "properties": {
            "tool_name": {
                "type": "string",
                "description": "Name of the tool to describe (e.g., 'bash', 'read', 'edit')"
            }
        },
        "required": ["tool_name"]
    })json",
    describe_tool_execute
};

REGISTER_TOOL(describe_tool, describe_tool_def);
