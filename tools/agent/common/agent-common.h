#pragma once

// Common utilities and type aliases for the agent module.
// This header consolidates duplicated declarations across the codebase.

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace agent {

// Filesystem namespace alias - use this instead of declaring locally
namespace fs = std::filesystem;

// JSON type alias - use this instead of declaring locally
using json = nlohmann::ordered_json;

// XML escaping for safe embedding in XML/HTML content.
// Handles: & < > " '
inline std::string escape_xml(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        switch (c) {
            case '&':  result += "&amp;";  break;
            case '<':  result += "&lt;";   break;
            case '>':  result += "&gt;";   break;
            case '"':  result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default:   result += c;        break;
        }
    }
    return result;
}

// Resolve a path relative to a working directory.
// Returns weakly canonical path (resolves .., symlinks where possible).
inline fs::path resolve_path(const std::string& path, const std::string& working_dir) {
    fs::path p(path);
    if (p.is_relative()) {
        p = fs::path(working_dir) / p;
    }
    return fs::weakly_canonical(p);
}

// Format error messages consistently.
// Pattern: "<action> failed: <reason> (<context>)"
inline std::string format_error(const std::string& action,
                                const std::string& reason,
                                const std::string& context = "") {
    std::string msg = action + " failed: " + reason;
    if (!context.empty()) {
        msg += " (" + context + ")";
    }
    return msg;
}

// Trim whitespace from both ends of a string
inline std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

} // namespace agent
