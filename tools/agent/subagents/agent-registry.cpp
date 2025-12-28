#include "agent-registry.h"
#include "embedded-agents.h"
#include "../common/constants.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace fs = agent::fs;
using agent::escape_xml;

bool agent_registry::validate_name(const std::string& name) {
    // 1-64 characters, lowercase letters, numbers, hyphens
    // Cannot start or end with hyphen, no consecutive hyphens
    if (name.empty() || name.size() > 64) return false;
    if (name.front() == '-' || name.back() == '-') return false;

    bool prev_hyphen = false;
    for (char c : name) {
        if (c == '-') {
            if (prev_hyphen) return false;  // No consecutive hyphens
            prev_hyphen = true;
        } else if (std::islower(c) || std::isdigit(c)) {
            prev_hyphen = false;
        } else {
            return false;  // Invalid character
        }
    }
    return true;
}

void agent_registry::register_embedded_agents() {
    // Parse and register planning-agent
    auto planning = parse_frontmatter(agent::embedded::PLANNING_AGENT_MD, "<embedded>/planning-agent");
    if (planning) {
        planning->agent_dir = "<embedded>";
        embedded_agents_["planning-agent"] = *planning;
    }

    // Parse and register explorer-agent
    auto explorer = parse_frontmatter(agent::embedded::EXPLORER_AGENT_MD, "<embedded>/explorer-agent");
    if (explorer) {
        explorer->agent_dir = "<embedded>";
        embedded_agents_["explorer-agent"] = *explorer;
    }
}

std::optional<agent_definition> agent_registry::parse_frontmatter(
    const std::string& content, const std::string& path) {

    // Check for frontmatter delimiter
    if (content.substr(0, 4) != "---\n" && content.substr(0, 3) != "---") {
        return std::nullopt;
    }

    // Find end of frontmatter
    size_t start = (content[3] == '\n') ? 4 : 3;
    size_t end = content.find("\n---", start);
    if (end == std::string::npos) {
        return std::nullopt;
    }

    std::string frontmatter = content.substr(start, end - start);
    std::string body = content.substr(end + 4);  // Skip the closing ---

    // Trim leading newlines from body
    while (!body.empty() && body.front() == '\n') {
        body.erase(0, 1);
    }

    agent_definition agent;
    agent.path = path;
    agent.instructions = body;

    // Parse YAML-like frontmatter (simple key: value format)
    std::istringstream iss(frontmatter);
    std::string line;

    while (std::getline(iss, line)) {
        // Trim leading whitespace
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        line = line.substr(first);

        // Skip comments
        if (line.empty() || line[0] == '#') continue;

        // Find key-value separator
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // Trim whitespace from key and value
        while (!key.empty() && std::isspace(key.back())) key.pop_back();
        while (!value.empty() && std::isspace(value.front())) value.erase(0, 1);
        while (!value.empty() && std::isspace(value.back())) value.pop_back();

        if (key == "name") {
            agent.name = value;
        } else if (key == "description") {
            agent.description = value;
        } else if (key == "allowed-tools") {
            // Parse space-separated list of tools
            std::istringstream tools_iss(value);
            std::string tool;
            while (tools_iss >> tool) {
                agent.allowed_tools.push_back(tool);
            }
        } else if (key == "max-iterations") {
            try {
                agent.max_iterations = std::stoi(value);
                if (agent.max_iterations < agent::config::MIN_MAX_ITERATIONS) {
                    agent.max_iterations = agent::config::MIN_MAX_ITERATIONS;
                }
                if (agent.max_iterations > agent::config::SUBAGENT_MAX_ITERATIONS_LIMIT) {
                    agent.max_iterations = agent::config::SUBAGENT_MAX_ITERATIONS_LIMIT;
                }
            } catch (...) {
                // Keep default
            }
        } else {
            // Store in metadata
            agent.metadata[key] = value;
        }
    }

    // Validate required fields
    if (agent.name.empty() || !validate_name(agent.name)) {
        return std::nullopt;
    }
    if (agent.description.empty()) {
        return std::nullopt;
    }

    return agent;
}

std::optional<agent_definition> agent_registry::parse_agent(const std::string& agent_dir) {
    std::string agent_md_path = agent_dir + "/AGENT.md";

    if (!fs::exists(agent_md_path)) {
        return std::nullopt;
    }

    // Read file content
    std::ifstream file(agent_md_path);
    if (!file) {
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    auto agent = parse_frontmatter(content, agent_md_path);
    if (agent) {
        agent->agent_dir = agent_dir;
    }
    return agent;
}

int agent_registry::discover(const std::vector<std::string>& search_paths) {
    // Use map to implement precedence: later entries override earlier entries
    // Precedence order (lowest to highest):
    //   1. User-global (~/.llama-agent/agents/) - lowest precedence
    //   2. Project-local (.llama-agent/agents/) - overrides global
    //   3. Embedded agents (built into binary) - HIGHEST, cannot be overridden
    //
    // This ensures embedded agents (core functionality) are always used,
    // while still allowing users to create custom agents with different names.
    std::map<std::string, agent_definition> agents_by_name;

    // First load from disk paths (lower precedence)
    // search_paths order: [data-dir, project-local, user-global]
    // We reverse iterate so user-global is loaded first, then overwritten by project-local
    for (auto it = search_paths.rbegin(); it != search_paths.rend(); ++it) {
        const auto& search_path = *it;
        if (!fs::exists(search_path)) {
            continue;
        }

        // Iterate through subdirectories
        for (const auto& entry : fs::directory_iterator(search_path)) {
            if (!entry.is_directory()) {
                continue;
            }

            auto agent = parse_agent(entry.path().string());
            if (agent) {
                // Skip disk agents that conflict with embedded agents
                // Embedded agents are core functionality and cannot be overridden
                if (embedded_agents_.find(agent->name) != embedded_agents_.end()) {
                    // Silently skip - embedded agent takes precedence
                    continue;
                }
                // Later entries (project-local) overwrite earlier (global)
                agents_by_name[agent->name] = *agent;
            }
        }
    }

    // Finally apply embedded agents (highest precedence - cannot be overridden)
    for (const auto& [name, def] : embedded_agents_) {
        agents_by_name[name] = def;
    }

    // Convert map to vector
    agents_.clear();
    for (const auto& [name, def] : agents_by_name) {
        agents_.push_back(def);
    }

    // Sort by name for consistent ordering
    std::sort(agents_.begin(), agents_.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });

    return static_cast<int>(agents_.size());
}

const agent_definition* agent_registry::get_agent(const std::string& name) const {
    for (const auto& agent : agents_) {
        if (agent.name == name) {
            return &agent;
        }
    }
    return nullptr;
}

std::string agent_registry::generate_prompt_section() const {
    if (agents_.empty()) {
        return "";
    }

    std::ostringstream ss;
    ss << "<available_agents>\n";

    for (const auto& agent : agents_) {
        ss << "<agent>\n";
        ss << "  <name>" << escape_xml(agent.name) << "</name>\n";
        ss << "  <description>" << escape_xml(agent.description) << "</description>\n";
        if (!agent.allowed_tools.empty()) {
            ss << "  <tools>";
            for (size_t i = 0; i < agent.allowed_tools.size(); i++) {
                if (i > 0) ss << " ";
                ss << agent.allowed_tools[i];
            }
            ss << "</tools>\n";
        }
        ss << "</agent>\n";
    }

    ss << "</available_agents>\n";
    return ss.str();
}
