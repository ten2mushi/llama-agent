#pragma once

// Centralized constants for the agent module.
// All magic numbers should be defined here for consistency.

namespace agent::config {

// =============================================================================
// Tool output limits
// =============================================================================

// Maximum characters in bash command output before truncation
constexpr int MAX_BASH_OUTPUT_LENGTH = 30000;

// Maximum lines shown in truncated bash output (head + tail)
constexpr int MAX_BASH_OUTPUT_LINES = 50;

// Default number of lines to read from a file
constexpr int DEFAULT_READ_LIMIT = 2000;

// Maximum line length before truncation in file reads
constexpr int MAX_LINE_LENGTH = 2000;

// Maximum number of files returned by glob
constexpr int MAX_GLOB_RESULTS = 100;

// =============================================================================
// Timeouts (milliseconds)
// =============================================================================

// Default timeout for tool execution
constexpr int DEFAULT_TOOL_TIMEOUT_MS = 120000;

// Timeout for context compaction operations
constexpr int COMPACT_TOOL_TIMEOUT_MS = 60000;

// Timeout for MCP server operations
constexpr int MCP_TIMEOUT_MS = 60000;

// =============================================================================
// Iteration limits
// =============================================================================

// Default maximum iterations for agent loop
constexpr int DEFAULT_MAX_ITERATIONS = 50;

// Minimum allowed value for --max-iterations
constexpr int MIN_MAX_ITERATIONS = 1;

// Maximum allowed value for --max-iterations
constexpr int MAX_MAX_ITERATIONS = 1000;

// Maximum iterations allowed for subagents (stricter limit)
constexpr int SUBAGENT_MAX_ITERATIONS_LIMIT = 100;

// =============================================================================
// Directory traversal
// =============================================================================

// Maximum depth for directory tree traversal (prevent infinite loops)
constexpr int MAX_DIRECTORY_DEPTH = 100;

// =============================================================================
// Subagent spawning
// =============================================================================

// Maximum depth of nested subagent spawning
constexpr int MAX_SPAWN_DEPTH = 3;

} // namespace agent::config
