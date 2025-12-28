#pragma once

#include "../common/agent-common.h"

#include <string>
#include <vector>

using agent::json;

namespace planning {

// A single question with multiple choice options
struct plan_question {
    int id = 0;
    std::string text;                        // The question text
    std::vector<std::string> options;        // Predefined answer options
    std::string selected_answer;             // User's selection or custom text
    bool is_custom = false;                  // True if user typed custom answer
    int selected_option_index = -1;          // Index of selected option (-1 if custom)

    json to_json() const;
    static plan_question from_json(const json& j);
};

// Session containing all questions for a planning iteration
struct qa_session {
    std::vector<plan_question> questions;
    int current_question_index = 0;

    // Check if all questions have been answered
    bool is_complete() const;

    // Get count of answered questions
    int answered_count() const;

    // Serialize for persistence
    json to_json() const;
    static qa_session from_json(const json& j);
};

// Parse questions from PlanningAgent output
// Expected format:
// {
//   "questions": [
//     {
//       "id": 1,
//       "text": "Which caching strategy?",
//       "options": ["Redis", "Memcached", "In-memory"]
//     },
//     ...
//   ]
// }
qa_session parse_questions_from_json(const json& agent_output);

// Format answered questions for plan refinement prompt
std::string format_answers_for_prompt(const qa_session& session);

} // namespace planning
