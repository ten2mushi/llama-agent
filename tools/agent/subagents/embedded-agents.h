#pragma once

// Embedded built-in agent definitions.
// These agents ship with the binary and provide core functionality.
// Users can override them by creating AGENT.md files with the same name.

namespace agent {
namespace embedded {

// Built-in planning-agent AGENT.md content
// Used by /plan command for codebase exploration and implementation planning
extern const char* PLANNING_AGENT_MD;

// Built-in explorer-agent AGENT.md content
// Used by planning-agent for deep dives into specific code areas
extern const char* EXPLORER_AGENT_MD;

} // namespace embedded
} // namespace agent
