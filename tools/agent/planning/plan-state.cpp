#include "plan-state.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace planning {

const char* state_to_string(planning_state state) {
    switch (state) {
        case planning_state::IDLE:              return "idle";
        case planning_state::EXPLORING:         return "exploring";
        case planning_state::SYNTHESIZING:      return "synthesizing";
        case planning_state::QUESTIONING:       return "questioning";
        case planning_state::AWAITING_ANSWERS:  return "awaiting_answers";
        case planning_state::REFINING:          return "refining";
        case planning_state::AWAITING_APPROVAL: return "awaiting_approval";
        case planning_state::APPROVED:          return "approved";
        case planning_state::ABORTED:           return "aborted";
        default:                                return "unknown";
    }
}

static planning_state string_to_state(const std::string& s) {
    if (s == "idle")              return planning_state::IDLE;
    if (s == "exploring")         return planning_state::EXPLORING;
    if (s == "synthesizing")      return planning_state::SYNTHESIZING;
    if (s == "questioning")       return planning_state::QUESTIONING;
    if (s == "awaiting_answers")  return planning_state::AWAITING_ANSWERS;
    if (s == "refining")          return planning_state::REFINING;
    if (s == "awaiting_approval") return planning_state::AWAITING_APPROVAL;
    if (s == "approved")          return planning_state::APPROVED;
    if (s == "aborted")           return planning_state::ABORTED;
    return planning_state::IDLE;
}

// planning_session serialization

json planning_session::to_json() const {
    return json{
        {"state", state_to_string(state)},
        {"context_id", context_id},
        {"task", task},
        {"exploration_findings", exploration_findings},
        {"plan_content", plan_content},
        {"questions", questions},
        {"answers", answers},
        {"iteration", iteration},
        {"plan_path", plan_path},
        {"created_at", created_at},
        {"updated_at", updated_at}
    };
}

planning_session planning_session::from_json(const json& j) {
    planning_session s;
    s.state = string_to_state(j.value("state", "idle"));
    s.context_id = j.value("context_id", "");
    s.task = j.value("task", "");
    s.exploration_findings = j.value("exploration_findings", "");
    s.plan_content = j.value("plan_content", "");
    s.questions = j.value("questions", json::array());
    s.answers = j.value("answers", json::array());
    s.iteration = j.value("iteration", 0);
    s.plan_path = j.value("plan_path", "");
    s.created_at = j.value("created_at", "");
    s.updated_at = j.value("updated_at", "");
    return s;
}

// planning_state_machine implementation

planning_state_machine::planning_state_machine(context_manager& ctx_mgr)
    : ctx_mgr_(ctx_mgr) {}

std::string planning_state_machine::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

bool planning_state_machine::start(const std::string& task, const std::string& context_id) {
    if (session_.state != planning_state::IDLE) {
        return false;  // Already active
    }

    session_ = planning_session{};
    session_.state = planning_state::EXPLORING;
    session_.context_id = context_id;
    session_.task = task;
    session_.iteration = 0;
    session_.created_at = get_timestamp();
    session_.updated_at = session_.created_at;
    session_.plan_path = get_plan_path();

    return save();
}

bool planning_state_machine::abort() {
    session_.state = planning_state::ABORTED;
    session_.updated_at = get_timestamp();
    return save();
}

bool planning_state_machine::complete() {
    session_.state = planning_state::APPROVED;
    session_.updated_at = get_timestamp();
    return save();
}

bool planning_state_machine::validate_transition(planning_state from, planning_state to) {
    // Define valid state transitions
    switch (from) {
        case planning_state::IDLE:
            return to == planning_state::EXPLORING;

        case planning_state::EXPLORING:
            return to == planning_state::SYNTHESIZING ||
                   to == planning_state::ABORTED;

        case planning_state::SYNTHESIZING:
            return to == planning_state::QUESTIONING ||
                   to == planning_state::AWAITING_APPROVAL ||  // No questions needed
                   to == planning_state::ABORTED;

        case planning_state::QUESTIONING:
            return to == planning_state::AWAITING_ANSWERS ||
                   to == planning_state::ABORTED;

        case planning_state::AWAITING_ANSWERS:
            return to == planning_state::REFINING ||
                   to == planning_state::ABORTED;

        case planning_state::REFINING:
            return to == planning_state::QUESTIONING ||     // More questions
                   to == planning_state::AWAITING_APPROVAL || // No more questions
                   to == planning_state::ABORTED;

        case planning_state::AWAITING_APPROVAL:
            return to == planning_state::APPROVED ||
                   to == planning_state::REFINING ||  // User wants changes
                   to == planning_state::ABORTED;

        case planning_state::APPROVED:
        case planning_state::ABORTED:
            return to == planning_state::IDLE;  // Reset for new session

        default:
            return false;
    }
}

bool planning_state_machine::transition_to(planning_state new_state) {
    if (!validate_transition(session_.state, new_state)) {
        return false;
    }

    session_.state = new_state;
    session_.updated_at = get_timestamp();
    return save();
}

bool planning_state_machine::is_active() const {
    return session_.state != planning_state::IDLE &&
           session_.state != planning_state::APPROVED &&
           session_.state != planning_state::ABORTED;
}

bool planning_state_machine::is_interactive() const {
    return session_.state == planning_state::AWAITING_ANSWERS ||
           session_.state == planning_state::AWAITING_APPROVAL;
}

void planning_state_machine::set_exploration_findings(const std::string& findings) {
    session_.exploration_findings = findings;
    session_.updated_at = get_timestamp();
}

void planning_state_machine::set_plan_content(const std::string& content) {
    session_.plan_content = content;
    session_.updated_at = get_timestamp();
}

void planning_state_machine::set_questions(const json& questions) {
    session_.questions = questions;
    session_.updated_at = get_timestamp();
}

void planning_state_machine::set_answers(const json& answers) {
    session_.answers = answers;
    session_.updated_at = get_timestamp();
}

void planning_state_machine::increment_iteration() {
    session_.iteration++;
    session_.updated_at = get_timestamp();
}

std::string planning_state_machine::get_plan_path() const {
    return ctx_mgr_.context_path(session_.context_id) + "/plan.md";
}

std::string planning_state_machine::get_state_path() const {
    return ctx_mgr_.context_path(session_.context_id) + "/plan_state.json";
}

bool planning_state_machine::save() {
    std::string path = get_state_path();

    // Ensure directory exists
    auto parent = agent::fs::path(path).parent_path();
    if (!agent::fs::exists(parent)) {
        agent::fs::create_directories(parent);
    }

    // Write atomically via temp file
    std::string temp_path = path + ".tmp";
    std::ofstream f(temp_path);
    if (!f) {
        return false;
    }

    f << session_.to_json().dump(2);
    f.close();

    if (f.fail()) {
        agent::fs::remove(temp_path);
        return false;
    }

    // Atomic rename
    agent::fs::rename(temp_path, path);
    return true;
}

bool planning_state_machine::load(const std::string& context_id) {
    session_.context_id = context_id;
    std::string path = get_state_path();

    if (!agent::fs::exists(path)) {
        session_ = planning_session{};
        session_.context_id = context_id;
        return false;
    }

    std::ifstream f(path);
    if (!f) {
        return false;
    }

    try {
        json j = json::parse(f);
        session_ = planning_session::from_json(j);
        session_.context_id = context_id;  // Ensure consistency
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool planning_state_machine::has_saved_session(const std::string& context_id) {
    std::string path = ctx_mgr_.context_path(context_id) + "/plan_state.json";
    return agent::fs::exists(path);
}

} // namespace planning
