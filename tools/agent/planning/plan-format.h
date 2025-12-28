#pragma once

#include "../common/agent-common.h"
#include "plan-questions.h"

#include <string>
#include <vector>

using agent::json;

namespace planning {

// Structured plan data for generation
struct plan_data {
    std::string task_summary;
    std::string created_at;
    int version = 1;
    std::string status;  // "draft" or "approved"

    std::string executive_summary;

    // Design decisions from Q&A
    std::vector<std::pair<std::string, std::string>> design_decisions;  // question -> answer

    // Raw plan content (phases, risks, testing, etc.)
    std::string plan_body;
};

// Plan format utilities
class plan_formatter {
public:
    // Generate plan markdown from structured data
    static std::string generate(const plan_data& data);

    // Generate minimal header for a plan
    static std::string generate_header(const std::string& task_summary,
                                       const std::string& timestamp,
                                       int version,
                                       const std::string& status);

    // Update the Design Decisions section with Q&A results
    static std::string update_design_decisions(const std::string& markdown,
                                                const qa_session& session);

    // Update plan status (draft -> approved)
    static std::string update_status(const std::string& markdown,
                                     const std::string& new_status);

    // Update version number
    static std::string update_version(const std::string& markdown, int new_version);

    // Extract a specific section from markdown
    static std::string extract_section(const std::string& markdown,
                                       const std::string& section_header);

    // Replace a specific section in markdown
    static std::string replace_section(const std::string& markdown,
                                       const std::string& section_header,
                                       const std::string& new_content);
};

} // namespace planning
