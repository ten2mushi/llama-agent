#include "context-manager.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

// conversation_state serialization

json conversation_state::to_json() const {
    json j;
    j["id"] = id;
    j["created_at"] = created_at;
    j["updated_at"] = updated_at;
    j["messages"] = messages;
    if (!metadata.empty()) {
        j["metadata"] = metadata;
    }
    return j;
}

conversation_state conversation_state::from_json(const json& j) {
    conversation_state state;
    state.id = j.value("id", "");
    state.created_at = j.value("created_at", "");
    state.updated_at = j.value("updated_at", "");
    state.messages = j.value("messages", json::array());
    state.metadata = j.value("metadata", json::object());
    return state;
}

// compact_entry serialization

json compact_entry::to_json() const {
    json j;
    j["timestamp"] = timestamp;

    // Programmatically extracted
    j["user_messages"] = user_messages;
    j["files_modified"] = files_modified;
    j["commands_run"] = commands_run;
    if (!plan_ref.empty()) {
        j["plan_ref"] = plan_ref;
    }

    // LLM-generated
    j["summary"] = summary;
    j["key_decisions"] = key_decisions;
    if (!current_state.empty()) {
        j["current_state"] = current_state;
    }
    if (!pending_tasks.empty()) {
        j["pending_tasks"] = pending_tasks;
    }

    return j;
}

compact_entry compact_entry::from_json(const json& j) {
    compact_entry entry;
    entry.timestamp = j.value("timestamp", "");

    // Programmatically extracted
    if (j.contains("user_messages") && j["user_messages"].is_array()) {
        for (const auto& msg : j["user_messages"]) {
            entry.user_messages.push_back(msg.get<std::string>());
        }
    }
    if (j.contains("files_modified") && j["files_modified"].is_array()) {
        for (const auto& f : j["files_modified"]) {
            entry.files_modified.push_back(f.get<std::string>());
        }
    }
    if (j.contains("commands_run") && j["commands_run"].is_array()) {
        for (const auto& c : j["commands_run"]) {
            entry.commands_run.push_back(c.get<std::string>());
        }
    }
    entry.plan_ref = j.value("plan_ref", "");

    // LLM-generated
    entry.summary = j.value("summary", "");
    entry.key_decisions = j.value("key_decisions", json::object());
    entry.current_state = j.value("current_state", "");
    if (j.contains("pending_tasks") && j["pending_tasks"].is_array()) {
        for (const auto& t : j["pending_tasks"]) {
            entry.pending_tasks.push_back(t.get<std::string>());
        }
    }

    return entry;
}

// context_manager implementation

context_manager::context_manager(const std::string& base_path)
    : base_path_(base_path) {
    ensure_directory(base_path_ + "/contexts");
}

std::string context_manager::generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    std::stringstream ss;
    ss << std::hex << std::setfill('0');

    uint64_t ab = dis(gen);
    uint64_t cd = dis(gen);

    ss << std::setw(8) << (ab >> 32);
    ss << '-';
    ss << std::setw(4) << ((ab >> 16) & 0xFFFF);
    ss << '-';
    ss << std::setw(4) << (4 << 12 | (ab & 0x0FFF)); // Version 4
    ss << '-';
    ss << std::setw(4) << (0x8000 | ((cd >> 48) & 0x3FFF)); // Variant
    ss << '-';
    ss << std::setw(12) << (cd & 0xFFFFFFFFFFFF);

    return ss.str();
}

std::string context_manager::iso8601_now() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

std::string context_manager::timestamp_now() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

std::string context_manager::get_preview(const json& messages) {
    // Find last user message
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if ((*it).value("role", "") == "user") {
            std::string content = (*it).value("content", "");
            // Get first line, truncate if needed
            size_t newline = content.find('\n');
            if (newline != std::string::npos) {
                content = content.substr(0, newline);
            }
            if (content.length() > 80) {
                content = content.substr(0, 77) + "...";
            }
            return content;
        }
    }
    return "(empty)";
}

bool context_manager::ensure_directory(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    return !ec;
}

std::optional<json> context_manager::read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        return std::nullopt;
    }
    try {
        json j;
        f >> j;
        return j;
    } catch (...) {
        return std::nullopt;
    }
}

bool context_manager::write_json(const std::string& path, const json& data) {
    // Write to temp file first, then rename for atomicity
    std::string temp_path = path + ".tmp";
    {
        std::ofstream f(temp_path);
        if (!f) {
            return false;
        }
        f << data.dump(2);
        if (!f) {
            return false;
        }
    }

    std::error_code ec;
    fs::rename(temp_path, path, ec);
    if (ec) {
        fs::remove(temp_path, ec);
        return false;
    }
    return true;
}

context_id context_manager::create_context() {
    context_id id = generate_uuid();
    std::string path = context_path(id);

    if (!ensure_directory(path)) {
        return "";
    }

    conversation_state state;
    state.id = id;
    state.created_at = iso8601_now();
    state.updated_at = state.created_at;
    state.messages = json::array();
    state.metadata = json::object();

    if (!save_context(state)) {
        return "";
    }

    return id;
}

std::optional<conversation_state> context_manager::load_context(const context_id& id) {
    std::string path = context_path(id) + "/conversation.json";
    auto j = read_json(path);
    if (!j) {
        return std::nullopt;
    }
    return conversation_state::from_json(*j);
}

bool context_manager::save_context(const conversation_state& state) {
    std::string path = context_path(state.id);
    if (!ensure_directory(path)) {
        return false;
    }
    return write_json(path + "/conversation.json", state.to_json());
}

bool context_manager::append_message(const context_id& id, const json& message) {
    auto state_opt = load_context(id);
    if (!state_opt) {
        return false;
    }

    auto& state = *state_opt;
    state.messages.push_back(message);
    state.updated_at = iso8601_now();

    return save_context(state);
}

bool context_manager::save_messages(const context_id& id, const json& messages) {
    auto state_opt = load_context(id);
    if (!state_opt) {
        return false;
    }

    auto& state = *state_opt;
    state.messages = messages;
    state.updated_at = iso8601_now();

    return save_context(state);
}

std::vector<context_summary> context_manager::list_contexts() {
    std::vector<context_summary> result;

    std::string contexts_dir = base_path_ + "/contexts";
    if (!fs::exists(contexts_dir)) {
        return result;
    }

    for (const auto& entry : fs::directory_iterator(contexts_dir)) {
        if (!entry.is_directory()) {
            continue;
        }

        context_id id = entry.path().filename().string();
        auto state_opt = load_context(id);
        if (!state_opt) {
            continue;
        }

        const auto& state = *state_opt;
        context_summary summary;
        summary.id = state.id;
        summary.updated_at = state.updated_at;
        summary.preview = get_preview(state.messages);
        summary.message_count = static_cast<int>(state.messages.size());

        result.push_back(summary);
    }

    // Sort by updated_at descending
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) {
                  return a.updated_at > b.updated_at;
              });

    return result;
}

bool context_manager::delete_context(const context_id& id) {
    std::string path = context_path(id);
    if (!fs::exists(path)) {
        return false;
    }

    std::error_code ec;
    fs::remove_all(path, ec);
    return !ec;
}

bool context_manager::context_exists(const context_id& id) {
    std::string path = context_path(id) + "/conversation.json";
    return fs::exists(path);
}

std::string context_manager::context_path(const context_id& id) const {
    return base_path_ + "/contexts/" + id;
}

bool context_manager::compact_context(const context_id& id, const compact_entry& entry) {
    auto state_opt = load_context(id);
    if (!state_opt) {
        return false;
    }

    auto& state = *state_opt;
    std::string ts = timestamp_now();
    std::string ctx_path = context_path(id);

    // Archive current messages
    std::string archive_path = ctx_path + "/conversation_" + ts + ".json";
    if (!write_json(archive_path, state.messages)) {
        return false;
    }

    // Create compact entry with timestamp
    compact_entry compact = entry;
    compact.timestamp = ts;

    std::string compact_path = ctx_path + "/compact_" + ts + ".json";
    if (!write_json(compact_path, compact.to_json())) {
        return false;
    }

    // Update metadata with archive reference
    if (!state.metadata.contains("archives")) {
        state.metadata["archives"] = json::array();
    }
    state.metadata["archives"].push_back({
        {"timestamp", ts},
        {"message_count", static_cast<int>(state.messages.size())},
        {"compact_ref", "compact_" + ts + ".json"}
    });

    // Reset messages with compact summary as context
    state.messages = json::array();

    // Build context restoration message from compact entry
    std::ostringstream context_ss;
    context_ss << "# Previous Context Summary\n\n";
    context_ss << compact.summary << "\n";

    if (!compact.current_state.empty()) {
        context_ss << "\n## Current State\n" << compact.current_state << "\n";
    }

    if (!compact.pending_tasks.empty()) {
        context_ss << "\n## Pending Tasks\n";
        for (const auto& task : compact.pending_tasks) {
            context_ss << "- " << task << "\n";
        }
    }

    if (!compact.files_modified.empty()) {
        context_ss << "\n## Files Modified\n";
        for (const auto& f : compact.files_modified) {
            context_ss << "- " << f << "\n";
        }
    }

    // Include plan reference if exists
    if (!compact.plan_ref.empty() || fs::exists(ctx_path + "/plan.md")) {
        context_ss << "\n## Active Plan\n";
        context_ss << "plan.md exists - use read_plan tool to review if needed\n";
        state.metadata["plan_ref"] = "plan.md";
    }

    state.messages.push_back({
        {"role", "system"},
        {"content", context_ss.str()}
    });
    state.updated_at = iso8601_now();

    return save_context(state);
}

std::vector<archive_ref> context_manager::get_archives(const context_id& id) {
    std::vector<archive_ref> result;
    std::string ctx_path = context_path(id);

    if (!fs::exists(ctx_path)) {
        return result;
    }

    // Find all conversation_*.json files
    for (const auto& entry : fs::directory_iterator(ctx_path)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        std::string filename = entry.path().filename().string();
        // Check if filename starts with "conversation_" and ends with ".json"
        if (filename.rfind("conversation_", 0) == 0 &&
            filename != "conversation.json" &&
            filename.size() >= 5 &&
            filename.compare(filename.size() - 5, 5, ".json") == 0) {

            // Extract timestamp from filename
            std::string ts = filename.substr(13, filename.length() - 18);

            archive_ref ref;
            ref.timestamp = ts;
            ref.filepath = entry.path().string();
            ref.compact_filepath = ctx_path + "/compact_" + ts + ".json";

            // Get message count from the archive
            auto archive_json = read_json(ref.filepath);
            if (archive_json && archive_json->is_array()) {
                ref.message_count = static_cast<int>(archive_json->size());
            }

            result.push_back(ref);
        }
    }

    // Sort by timestamp ascending (oldest first)
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) {
                  return a.timestamp < b.timestamp;
              });

    return result;
}

bool context_manager::save_plan_md(const context_id& id, const std::string& content) {
    std::string path = context_path(id);
    if (!ensure_directory(path)) {
        return false;
    }

    // Save plan.md (markdown format)
    std::string plan_path = path + "/plan.md";
    std::ofstream f(plan_path);
    if (!f) {
        return false;
    }
    f << content;
    if (!f) {
        return false;
    }
    f.close();

    // Update metadata to reference the plan
    auto state_opt = load_context(id);
    if (state_opt) {
        state_opt->metadata["plan_ref"] = "plan.md";
        state_opt->updated_at = iso8601_now();
        save_context(*state_opt);
    }

    return true;
}

std::string context_manager::load_plan_md(const context_id& id) {
    std::string path = context_path(id) + "/plan.md";
    if (!fs::exists(path)) {
        return "";
    }

    std::ifstream f(path);
    if (!f) {
        return "";
    }

    std::stringstream buffer;
    buffer << f.rdbuf();
    return buffer.str();
}

bool context_manager::has_plan(const context_id& id) {
    std::string path = context_path(id) + "/plan.md";
    return fs::exists(path);
}
