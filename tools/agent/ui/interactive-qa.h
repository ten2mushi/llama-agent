#pragma once

#include "../planning/plan-questions.h"

#include <atomic>
#include <string>

namespace planning {

// Result of interactive Q&A session
enum class qa_result {
  COMPLETED,  // User answered all questions
  ABORTED,    // User pressed ESC to abort
  INTERRUPTED // External interrupt signal
};

// Interactive terminal UI for Q&A during planning workflow
//
// Terminal Layout:
// +------------------------------------------------------------------+
// | [Q1] [Q2] [Q3] [Q4*] [Q5]                                        |  <- Tabs
// +------------------------------------------------------------------+
// | Question 4: Which error handling strategy do you prefer?         |
// |                                                                   |
// | > [x] Option 1: Return error codes                               |
// |   [ ] Option 2: Throw exceptions                                 |
// |   [ ] Option 3: Result<T, E> pattern                             |
// |   [ ] Custom: _____________________________                      |
// |                                                                   |
// | <- -> tabs | up/down select | Enter confirm | Tab custom | ESC   |
// +------------------------------------------------------------------+
//
// Controls:
// - Left/Right arrows: Switch between question tabs
// - Up/Down arrows: Navigate answer options
// - Enter: Select highlighted option and move to next question
// - Tab: Toggle custom input mode
// - ESC: Abort (with confirmation)
// - Ctrl+D: Submit all answers (when all answered)
//
class interactive_qa_ui {
public:
  explicit interactive_qa_ui(qa_session &session,
                             std::atomic<bool> &is_interrupted);

  // Run the interactive UI
  // Returns result indicating how the session ended
  qa_result run();

  // Static convenience method
  static qa_result show(qa_session &session, std::atomic<bool> &is_interrupted);

private:
  qa_session &session_;
  std::atomic<bool> &is_interrupted_;

  int current_option_index_ = 0; // Currently highlighted option
  bool in_custom_mode_ = false;  // True when typing custom answer
  std::string custom_input_;     // Buffer for custom input
  bool first_render_ = true;     // Track initial render for this instance

  // Rendering
  void render();
  void render_tabs();
  void render_question();
  void render_options();
  void render_help();
  void clear_ui_area();

  // Input handling
  bool handle_input(); // Returns false to exit loop
  void next_tab();
  void prev_tab();
  void next_option();
  void prev_option();
  void select_current_option();
  void toggle_custom_mode();
  void handle_custom_input(char32_t ch);
  bool confirm_abort();

  // Utilities
  int option_count() const; // Including "Custom" option
  bool all_answered() const;
  void save_current_selection();
};

// Simple yes/no prompt for plan approval
// Returns true if user approves, false otherwise
bool prompt_approval(const std::string &prompt_text,
                     std::atomic<bool> &is_interrupted);

} // namespace planning
