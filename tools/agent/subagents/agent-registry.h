#pragma once

#include "../common/agent-common.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

using agent::json;

// Agent definition parsed from AGENT.md
struct agent_definition {
    std::string name;                          // Required: agent name
    std::string description;                   // Required: when to use this agent
    std::string instructions;                  // Full markdown instructions (body after frontmatter)
    std::vector<std::string> allowed_tools;    // Whitelist of tools (empty = no tools)
    int max_iterations = 20;                   // Max iterations for this agent
    json metadata;                             // Optional: custom key-value pairs
    std::string path;                          // Absolute path to AGENT.md
    std::string agent_dir;                     // Directory containing AGENT.md
};

// Discovers and manages agent definitions from AGENT.md files
class agent_registry {
public:
    // Register built-in agents (call before discover)
    // These agents are embedded in the binary and serve as defaults
    void register_embedded_agents();

    // Discover agents from search paths
    // Returns number of agents discovered
    int discover(const std::vector<std::string>& search_paths);

    // Get all discovered agents
    const std::vector<agent_definition>& get_agents() const { return agents_; }

    // Get agent by name (returns nullptr if not found)
    const agent_definition* get_agent(const std::string& name) const;

    // Generate prompt section for system prompt (lists available agents)
    std::string generate_prompt_section() const;

    // Validate agent name according to spec
    static bool validate_name(const std::string& name);

private:
    std::vector<agent_definition> agents_;
    std::map<std::string, agent_definition> embedded_agents_;

    // Parse a single agent directory
    std::optional<agent_definition> parse_agent(const std::string& agent_dir);

    // Parse YAML frontmatter from AGENT.md content
    std::optional<agent_definition> parse_frontmatter(const std::string& content,
                                                        const std::string& path);

};
