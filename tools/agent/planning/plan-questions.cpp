#include "plan-questions.h"

#include <sstream>

namespace planning {

// plan_question serialization

json plan_question::to_json() const {
    return json{
        {"id", id},
        {"text", text},
        {"options", options},
        {"selected_answer", selected_answer},
        {"is_custom", is_custom},
        {"selected_option_index", selected_option_index}
    };
}

plan_question plan_question::from_json(const json& j) {
    plan_question q;
    q.id = j.value("id", 0);
    q.text = j.value("text", "");
    q.options = j.value("options", std::vector<std::string>{});
    q.selected_answer = j.value("selected_answer", "");
    q.is_custom = j.value("is_custom", false);
    q.selected_option_index = j.value("selected_option_index", -1);
    return q;
}

// qa_session implementation

bool qa_session::is_complete() const {
    for (const auto& q : questions) {
        if (q.selected_answer.empty()) {
            return false;
        }
    }
    return !questions.empty();
}

int qa_session::answered_count() const {
    int count = 0;
    for (const auto& q : questions) {
        if (!q.selected_answer.empty()) {
            count++;
        }
    }
    return count;
}

json qa_session::to_json() const {
    json arr = json::array();
    for (const auto& q : questions) {
        arr.push_back(q.to_json());
    }
    return json{
        {"questions", arr},
        {"current_question_index", current_question_index}
    };
}

qa_session qa_session::from_json(const json& j) {
    qa_session session;
    session.current_question_index = j.value("current_question_index", 0);

    if (j.contains("questions") && j["questions"].is_array()) {
        for (const auto& qj : j["questions"]) {
            session.questions.push_back(plan_question::from_json(qj));
        }
    }

    return session;
}

qa_session parse_questions_from_json(const json& agent_output) {
    qa_session session;

    // Handle different output formats from PlanningAgent
    json questions_array;

    if (agent_output.contains("questions") && agent_output["questions"].is_array()) {
        questions_array = agent_output["questions"];
    } else if (agent_output.is_array()) {
        questions_array = agent_output;
    } else {
        // No questions found
        return session;
    }

    int id = 1;
    for (const auto& qj : questions_array) {
        plan_question q;
        q.id = qj.value("id", id);

        // Support both "text" and "question" keys
        if (qj.contains("text")) {
            q.text = qj["text"].get<std::string>();
        } else if (qj.contains("question")) {
            q.text = qj["question"].get<std::string>();
        }

        // Support both "options" and "answers" keys
        if (qj.contains("options") && qj["options"].is_array()) {
            for (const auto& opt : qj["options"]) {
                if (opt.is_string()) {
                    q.options.push_back(opt.get<std::string>());
                }
            }
        } else if (qj.contains("answers") && qj["answers"].is_array()) {
            for (const auto& opt : qj["answers"]) {
                if (opt.is_string()) {
                    q.options.push_back(opt.get<std::string>());
                }
            }
        }

        if (!q.text.empty() && !q.options.empty()) {
            session.questions.push_back(q);
        }

        id++;
    }

    return session;
}

std::string format_answers_for_prompt(const qa_session& session) {
    std::stringstream ss;

    ss << "User's design decisions:\n\n";

    for (const auto& q : session.questions) {
        ss << "Q" << q.id << ": " << q.text << "\n";
        ss << "Answer: " << q.selected_answer;
        if (q.is_custom) {
            ss << " (custom response)";
        }
        ss << "\n\n";
    }

    return ss.str();
}

} // namespace planning
