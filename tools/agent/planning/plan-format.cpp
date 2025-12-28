#include "plan-format.h"

#include <regex>
#include <sstream>

namespace planning {

std::string plan_formatter::generate(const plan_data& data) {
    std::stringstream ss;

    // Header
    ss << "# Implementation Plan: " << data.task_summary << "\n\n";

    // Metadata
    ss << "## Metadata\n";
    ss << "- Created: " << data.created_at << "\n";
    ss << "- Version: " << data.version << "\n";
    ss << "- Status: " << data.status << "\n\n";

    // Executive Summary
    if (!data.executive_summary.empty()) {
        ss << "## Executive Summary\n\n";
        ss << data.executive_summary << "\n\n";
    }

    // Design Decisions
    if (!data.design_decisions.empty()) {
        ss << "## Design Decisions\n\n";
        ss << "Based on the following user preferences:\n\n";
        for (const auto& [question, answer] : data.design_decisions) {
            ss << "- **" << question << "**: " << answer << "\n";
        }
        ss << "\n";
    }

    // Plan body (phases, risks, testing, etc.)
    if (!data.plan_body.empty()) {
        ss << data.plan_body;
        if (data.plan_body.back() != '\n') {
            ss << "\n";
        }
    }

    return ss.str();
}

std::string plan_formatter::generate_header(const std::string& task_summary,
                                            const std::string& timestamp,
                                            int version,
                                            const std::string& status) {
    std::stringstream ss;
    ss << "# Implementation Plan: " << task_summary << "\n\n";
    ss << "## Metadata\n";
    ss << "- Created: " << timestamp << "\n";
    ss << "- Version: " << version << "\n";
    ss << "- Status: " << status << "\n\n";
    return ss.str();
}

std::string plan_formatter::update_design_decisions(const std::string& markdown,
                                                     const qa_session& session) {
    std::stringstream decisions;
    decisions << "## Design Decisions\n\n";
    decisions << "Based on the following user preferences:\n\n";

    for (const auto& q : session.questions) {
        if (!q.selected_answer.empty()) {
            decisions << "- **" << q.text << "**: " << q.selected_answer;
            if (q.is_custom) {
                decisions << " *(custom)*";
            }
            decisions << "\n";
        }
    }
    decisions << "\n";

    // Try to replace existing Design Decisions section
    std::string result = replace_section(markdown, "## Design Decisions", decisions.str());

    // If section didn't exist, insert after Metadata
    if (result == markdown) {
        size_t metadata_end = markdown.find("## ", markdown.find("## Metadata") + 1);
        if (metadata_end == std::string::npos) {
            // No other sections, append
            result = markdown + decisions.str();
        } else {
            result = markdown.substr(0, metadata_end) + decisions.str() + markdown.substr(metadata_end);
        }
    }

    return result;
}

std::string plan_formatter::update_status(const std::string& markdown,
                                          const std::string& new_status) {
    std::regex status_regex(R"(- Status: \w+)");
    return std::regex_replace(markdown, status_regex, "- Status: " + new_status);
}

std::string plan_formatter::update_version(const std::string& markdown, int new_version) {
    std::regex version_regex(R"(- Version: \d+)");
    return std::regex_replace(markdown, version_regex, "- Version: " + std::to_string(new_version));
}

std::string plan_formatter::extract_section(const std::string& markdown,
                                            const std::string& section_header) {
    size_t start = markdown.find(section_header);
    if (start == std::string::npos) {
        return "";
    }

    // Find the end of this section (next ## header or end of document)
    size_t content_start = start + section_header.length();
    // Skip to end of header line
    size_t line_end = markdown.find('\n', content_start);
    if (line_end != std::string::npos) {
        content_start = line_end + 1;
    }

    // Find next ## header
    size_t end = markdown.find("\n## ", content_start);
    if (end == std::string::npos) {
        end = markdown.length();
    }

    return markdown.substr(content_start, end - content_start);
}

std::string plan_formatter::replace_section(const std::string& markdown,
                                            const std::string& section_header,
                                            const std::string& new_content) {
    size_t start = markdown.find(section_header);
    if (start == std::string::npos) {
        return markdown;  // Section not found
    }

    // Find the end of this section (next ## header or end of document)
    size_t end = markdown.find("\n## ", start + section_header.length());
    if (end == std::string::npos) {
        end = markdown.length();
    } else {
        end++;  // Include the newline before next section
    }

    return markdown.substr(0, start) + new_content + markdown.substr(end);
}

} // namespace planning
