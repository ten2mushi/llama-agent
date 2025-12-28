#pragma once

#include "../common/agent-common.h"

#include <optional>
#include <string>
#include <vector>

using agent::json;
namespace fs = agent::fs;

// Unique identifier for a conversation context
using context_id = std::string;

// Summary for listing contexts
struct context_summary {
    context_id id;
    std::string updated_at;
    std::string preview;       // First line of last user message
    int message_count;
};

// Archive reference for compacted history
struct archive_ref {
    std::string timestamp;
    std::string filepath;        // Path to conversation_<ts>.json
    std::string compact_filepath; // Path to compact_<ts>.json
    int message_count;
};

// Full conversation state
struct conversation_state {
    context_id id;
    std::string created_at;
    std::string updated_at;
    json messages;               // Full message history
    json metadata;               // Custom metadata (title, tags, archives, plan_ref)

    // Serialization
    json to_json() const;
    static conversation_state from_json(const json& j);
};

// Compacted history entry - hybrid of programmatic extraction + LLM summary
struct compact_entry {
    std::string timestamp;

    // Programmatically extracted (reliable, structured)
    std::vector<std::string> user_messages;   // All user messages in compacted segment
    std::vector<std::string> files_modified;  // Files that were modified
    std::vector<std::string> commands_run;    // Shell commands that were run
    std::string plan_ref;                     // Reference to plan.json if exists

    // LLM-generated (intelligent, contextual)
    std::string summary;                      // High-level summary from LLM
    json key_decisions;                       // Key decisions extracted by LLM
    std::string current_state;                // Where the work stands
    std::vector<std::string> pending_tasks;   // Unfinished tasks

    json to_json() const;
    static compact_entry from_json(const json& j);
};

// Manages conversation persistence
class context_manager {
public:
    explicit context_manager(const std::string& base_path);

    // Create a new context with fresh UUID
    context_id create_context();

    // Load an existing context
    std::optional<conversation_state> load_context(const context_id& id);

    // Save/update a context
    bool save_context(const conversation_state& state);

    // Append a single message to a context (loads and saves full file)
    // Note: For performance, prefer save_messages() for batch updates
    bool append_message(const context_id& id, const json& message);

    // Save all messages for a context (efficient batch update)
    bool save_messages(const context_id& id, const json& messages);

    // List all contexts, sorted by updated_at descending
    std::vector<context_summary> list_contexts();

    // Delete a context and all its files
    bool delete_context(const context_id& id);

    // Check if a context exists
    bool context_exists(const context_id& id);

    // Get the context directory path
    std::string context_path(const context_id& id) const;

    // Compact a context: archive current messages and create compact entry
    bool compact_context(const context_id& id, const compact_entry& entry);

    // Get all archive references for a context
    std::vector<archive_ref> get_archives(const context_id& id);

    // Save plan.md for a context (markdown content)
    bool save_plan_md(const context_id& id, const std::string& content);

    // Load plan.md for a context (returns empty string if no plan)
    std::string load_plan_md(const context_id& id);

    // Check if a context has a plan (plan.md)
    bool has_plan(const context_id& id);

    // Get the base path
    const std::string& base_path() const { return base_path_; }

private:
    std::string base_path_;

    // Generate a new UUID
    static std::string generate_uuid();

    // Get current ISO8601 timestamp
    static std::string iso8601_now();

    // Get filesystem-safe timestamp (for filenames)
    static std::string timestamp_now();

    // Extract preview from messages (first line of last user message)
    static std::string get_preview(const json& messages);

    // Ensure directory exists
    static bool ensure_directory(const std::string& path);

    // Read JSON from file
    static std::optional<json> read_json(const std::string& path);

    // Write JSON to file atomically
    static bool write_json(const std::string& path, const json& data);
};
