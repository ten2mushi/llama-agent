#pragma once

#include "../common/agent-common.h"
#include "../context/context-manager.h"

#include <string>
#include <vector>

using agent::json;

namespace planning {

// Planning workflow states
enum class planning_state {
    IDLE,                  // No planning in progress
    EXPLORING,             // PlanningAgent exploring codebase
    SYNTHESIZING,          // Creating initial plan
    QUESTIONING,           // Generating questions
    AWAITING_ANSWERS,      // Waiting for user input
    REFINING,              // Updating plan based on answers
    AWAITING_APPROVAL,     // Waiting for final approval
    APPROVED,              // Plan finalized
    ABORTED                // User cancelled
};

// Convert state to string for display/logging
const char* state_to_string(planning_state state);

// Planning session data - persisted to plan_state.json
struct planning_session {
    planning_state state = planning_state::IDLE;
    std::string context_id;
    std::string task;                    // Original user task
    std::string exploration_findings;    // Pre-exploration results from explorer-agent
    std::string plan_content;            // Current plan markdown
    json questions;                      // Current Q&A set (array of question objects)
    json answers;                        // User's answers (array matching questions)
    int iteration = 0;                   // Refinement iteration count
    std::string plan_path;               // Path to plan.md
    std::string created_at;
    std::string updated_at;

    json to_json() const;
    static planning_session from_json(const json& j);
};

// State machine for managing planning workflow
class planning_state_machine {
public:
    explicit planning_state_machine(context_manager& ctx_mgr);

    // Lifecycle
    bool start(const std::string& task, const std::string& context_id);
    bool abort();
    bool complete();

    // State transitions
    bool transition_to(planning_state new_state);

    // State queries
    planning_state current_state() const { return session_.state; }
    bool is_active() const;
    bool is_interactive() const;  // AWAITING_ANSWERS or AWAITING_APPROVAL
    int current_iteration() const { return session_.iteration; }

    // Session access
    planning_session& session() { return session_; }
    const planning_session& session() const { return session_; }

    // Plan content management
    void set_exploration_findings(const std::string& findings);
    void set_plan_content(const std::string& content);
    void set_questions(const json& questions);
    void set_answers(const json& answers);
    void increment_iteration();

    // Persistence
    bool save();
    bool load(const std::string& context_id);
    bool has_saved_session(const std::string& context_id);

    // Get plan file path for a context
    std::string get_plan_path() const;
    std::string get_state_path() const;

private:
    planning_session session_;
    context_manager& ctx_mgr_;

    bool validate_transition(planning_state from, planning_state to);
    std::string get_timestamp();
};

} // namespace planning
