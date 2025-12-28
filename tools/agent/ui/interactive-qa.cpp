#include "interactive-qa.h"
#include "console.h"

#include <cstdio>
#include <string>

#if !defined(_WIN32)
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#define ANSI_COLOR_RESET "\x1b[0m"
#define ANSI_COLOR_CYAN "\x1b[36m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_GRAY "\x1b[90m"
#define ANSI_BOLD "\x1b[1m"
#define ANSI_CLEAR_LINE "\x1b[2K"
#define ANSI_CURSOR_UP "\x1b[A"
#define ANSI_CURSOR_HIDE "\x1b[?25l"
#define ANSI_CURSOR_SHOW "\x1b[?25h"

namespace planning {

#if !defined(_WIN32)
// RAII class to manage terminal raw mode
class terminal_raw_mode {
public:
  terminal_raw_mode() : active_(false) {
    if (tcgetattr(STDIN_FILENO, &original_) == 0) {
      struct termios raw = original_;
      // Disable canonical mode (line buffering) and echo
      raw.c_lflag &= ~(ICANON | ECHO);
      // Read returns after 1 character
      raw.c_cc[VMIN] = 1;
      raw.c_cc[VTIME] = 0;
      if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
        active_ = true;
      }
    }
  }

  ~terminal_raw_mode() { restore(); }

  void restore() {
    if (active_) {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
      active_ = false;
    }
  }

  bool is_active() const { return active_; }

private:
  struct termios original_;
  bool active_;
};
#endif

// Helper to get terminal width
static int get_terminal_width() {
#if defined(_WIN32)
  return 80; // Default for Windows
#else
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
    return w.ws_col > 0 ? w.ws_col : 80;
  }
  return 80;
#endif
}

// Helper to read a single character (requires raw mode to be set)
static char32_t read_char() {
#if defined(_WIN32)
  // Windows implementation would use ReadConsoleInput
  // For now, simplified approach
  return getchar();
#else
  unsigned char c;
  if (read(STDIN_FILENO, &c, 1) != 1) {
    return 0;
  }
  return c;
#endif
}

interactive_qa_ui::interactive_qa_ui(qa_session &session,
                                     std::atomic<bool> &is_interrupted)
    : session_(session), is_interrupted_(is_interrupted), first_render_(true) {

  if (!session_.questions.empty()) {
    // Initialize selection based on any pre-existing answers
    auto &q = session_.questions[session_.current_question_index];
    if (q.selected_option_index >= 0) {
      current_option_index_ = q.selected_option_index;
    } else if (q.is_custom) {
      current_option_index_ =
          static_cast<int>(q.options.size()); // Custom option
      in_custom_mode_ = true;
      custom_input_ = q.selected_answer;
    }
  }
}

qa_result interactive_qa_ui::run() {
  if (session_.questions.empty()) {
    return qa_result::COMPLETED;
  }

#if !defined(_WIN32)
  // Enter raw mode for character-by-character input
  terminal_raw_mode raw_mode;
  if (!raw_mode.is_active()) {
    console::error("Failed to enter raw terminal mode for Q&A UI\n");
    return qa_result::ABORTED;
  }
#endif

  // Initial render
  printf(ANSI_CURSOR_HIDE);
  fflush(stdout);
  render();

  qa_result result = qa_result::COMPLETED;

  while (true) {
    if (is_interrupted_.load()) {
      result = qa_result::INTERRUPTED;
      break;
    }

    if (!handle_input()) {
      break;
    }

    render();
  }

  printf(ANSI_CURSOR_SHOW);
  fflush(stdout);

#if !defined(_WIN32)
  // RAII class will restore terminal on destruction
  raw_mode.restore();
#endif

  if (session_.questions[session_.current_question_index]
          .selected_answer.empty() &&
      !all_answered()) {
    return qa_result::ABORTED;
  }

  return result;
}

qa_result interactive_qa_ui::show(qa_session &session,
                                  std::atomic<bool> &is_interrupted) {
  interactive_qa_ui ui(session, is_interrupted);
  return ui.run();
}

void interactive_qa_ui::render() {
  // Move cursor up to overwrite previous render
  // For first render, this is a no-op
  if (!first_render_) {
    clear_ui_area();
  }
  first_render_ = false;

  render_tabs();
  printf("\n");
  render_question();
  render_options();
  render_help();

  fflush(stdout);
}

void interactive_qa_ui::render_tabs() {
  int width = get_terminal_width();
  std::string line = "+";
  for (int i = 0; i < width - 2; i++)
    line += "-";
  line += "+";
  printf("%s\n", line.c_str());

  printf("| ");
  for (size_t i = 0; i < session_.questions.size(); i++) {
    bool is_current =
        (i == static_cast<size_t>(session_.current_question_index));
    bool is_answered = !session_.questions[i].selected_answer.empty();

    if (is_current) {
      printf(ANSI_BOLD ANSI_COLOR_CYAN);
    } else if (is_answered) {
      printf(ANSI_COLOR_GREEN);
    }

    printf("[Q%zu%s]", i + 1, is_current ? "*" : (is_answered ? "+" : ""));
    printf(ANSI_COLOR_RESET " ");
  }

  // Pad to width
  printf("\n");
  printf("%s\n", line.c_str());
}

void interactive_qa_ui::render_question() {
  if (session_.current_question_index >=
      static_cast<int>(session_.questions.size())) {
    return;
  }

  const auto &q = session_.questions[session_.current_question_index];
  printf(ANSI_BOLD "Q%d: %s" ANSI_COLOR_RESET "\n\n", q.id, q.text.c_str());
}

void interactive_qa_ui::render_options() {
  if (session_.current_question_index >=
      static_cast<int>(session_.questions.size())) {
    return;
  }

  const auto &q = session_.questions[session_.current_question_index];

  for (size_t i = 0; i < q.options.size(); i++) {
    bool is_selected =
        (static_cast<int>(i) == current_option_index_) && !in_custom_mode_;
    bool is_answered = (static_cast<int>(i) == q.selected_option_index);

    if (is_selected) {
      printf(ANSI_COLOR_CYAN " > ");
    } else {
      printf("   ");
    }

    if (is_answered) {
      printf(ANSI_COLOR_GREEN "[x]" ANSI_COLOR_RESET);
    } else {
      printf("[ ]");
    }

    printf(" %s\n", q.options[i].c_str());
  }

  // Custom option
  bool custom_selected =
      (current_option_index_ == static_cast<int>(q.options.size())) ||
      in_custom_mode_;
  bool custom_answered = q.is_custom && !q.selected_answer.empty();

  if (custom_selected) {
    printf(ANSI_COLOR_CYAN " > ");
  } else {
    printf("   ");
  }

  if (custom_answered) {
    printf(ANSI_COLOR_GREEN "[x]" ANSI_COLOR_RESET);
  } else {
    printf("[ ]");
  }

  printf(" Custom: ");
  if (in_custom_mode_) {
    printf(ANSI_COLOR_YELLOW "%s_" ANSI_COLOR_RESET, custom_input_.c_str());
  } else if (q.is_custom && !q.selected_answer.empty()) {
    printf(ANSI_COLOR_GREEN "%s" ANSI_COLOR_RESET, q.selected_answer.c_str());
  } else {
    printf(ANSI_COLOR_GRAY "_______________" ANSI_COLOR_RESET);
  }
  printf("\n");
}

void interactive_qa_ui::render_help() {
  printf("\n");
  int width = get_terminal_width();
  std::string line = "+";
  for (int i = 0; i < width - 2; i++)
    line += "-";
  line += "+";
  printf("%s\n", line.c_str());

  printf(ANSI_COLOR_GRAY);
  if (in_custom_mode_) {
    printf("| Type answer, Enter to confirm, ESC to cancel custom input");
  } else {
    printf("| <- -> tabs | up/down select | Enter confirm | Tab custom | ESC "
           "abort");
  }

  if (all_answered()) {
    printf(" | " ANSI_COLOR_GREEN
           "Ctrl+D submit" ANSI_COLOR_RESET ANSI_COLOR_GRAY);
  }
  printf(ANSI_COLOR_RESET "\n");
  printf("%s\n", line.c_str());
}

void interactive_qa_ui::clear_ui_area() {
  // Calculate total lines to clear:
  // 1. Tabs padding/headers: 3 lines
  // 2. Initial spacing (render newline): 1 line
  // 3. Question (render_question): 2 lines (text + blank)
  // 4. Options (render_options): option_count() lines (includes custom)
  // 5. Help spacing (render_help newline): 1 line
  // 6. Help box (render_help): 3 lines
  // Total = 10 + option_count()
  int lines_to_clear = 10 + option_count();

  for (int i = 0; i < lines_to_clear; i++) {
    printf(ANSI_CURSOR_UP ANSI_CLEAR_LINE);
  }
}

bool interactive_qa_ui::handle_input() {
  char32_t ch = read_char();

  if (ch == 0) {
    return true; // No input, continue
  }

  // Handle escape sequences
  if (ch == 27) { // ESC
    char32_t next = read_char();
    if (next == '[') {
      char32_t code = read_char();
      switch (code) {
      case 'A': // Up arrow
        if (!in_custom_mode_)
          prev_option();
        break;
      case 'B': // Down arrow
        if (!in_custom_mode_)
          next_option();
        break;
      case 'C': // Right arrow
        if (!in_custom_mode_)
          next_tab();
        break;
      case 'D': // Left arrow
        if (!in_custom_mode_)
          prev_tab();
        break;
      }
    } else if (next == 0 || next == 27) {
      // Plain ESC
      if (in_custom_mode_) {
        in_custom_mode_ = false;
        custom_input_.clear();
      } else {
        if (confirm_abort()) {
          return false; // Exit loop
        }
      }
    }
    return true;
  }

  if (in_custom_mode_) {
    handle_custom_input(ch);
    return true;
  }

  switch (ch) {
  case '\r':
  case '\n': // Enter
    select_current_option();
    if (all_answered()) {
      return false; // Complete - all questions answered
    }
    // Move to next unanswered question (or wrap to first unanswered)
    {
      int start = session_.current_question_index;
      int n = static_cast<int>(session_.questions.size());
      for (int i = 1; i <= n; i++) {
        int idx = (start + i) % n;
        if (session_.questions[idx].selected_answer.empty()) {
          session_.current_question_index = idx;
          // Restore selection state for new question
          const auto &q = session_.questions[idx];
          if (q.is_custom) {
            current_option_index_ = static_cast<int>(q.options.size());
            custom_input_ = q.selected_answer;
          } else if (q.selected_option_index >= 0) {
            current_option_index_ = q.selected_option_index;
          } else {
            current_option_index_ = 0;
          }
          in_custom_mode_ = false;
          break;
        }
      }
    }
    break;

  case '\t': // Tab - toggle custom mode
    toggle_custom_mode();
    break;

  case 4: // Ctrl+D - submit if all answered
    if (all_answered()) {
      return false;
    }
    break;

  case 'j': // vim-style down
    next_option();
    break;

  case 'k': // vim-style up
    prev_option();
    break;

  case 'h': // vim-style left
    prev_tab();
    break;

  case 'l': // vim-style right
    next_tab();
    break;

  case 'q': // Quit
    if (confirm_abort()) {
      return false;
    }
    break;
  }

  return true;
}

void interactive_qa_ui::next_tab() {
  save_current_selection();
  if (session_.current_question_index <
      static_cast<int>(session_.questions.size()) - 1) {
    session_.current_question_index++;
    // Restore selection state for new question
    const auto &q = session_.questions[session_.current_question_index];
    if (q.is_custom) {
      current_option_index_ = static_cast<int>(q.options.size());
      custom_input_ = q.selected_answer;
    } else if (q.selected_option_index >= 0) {
      current_option_index_ = q.selected_option_index;
    } else {
      current_option_index_ = 0;
    }
    in_custom_mode_ = false;
  }
}

void interactive_qa_ui::prev_tab() {
  save_current_selection();
  if (session_.current_question_index > 0) {
    session_.current_question_index--;
    // Restore selection state
    const auto &q = session_.questions[session_.current_question_index];
    if (q.is_custom) {
      current_option_index_ = static_cast<int>(q.options.size());
      custom_input_ = q.selected_answer;
    } else if (q.selected_option_index >= 0) {
      current_option_index_ = q.selected_option_index;
    } else {
      current_option_index_ = 0;
    }
    in_custom_mode_ = false;
  }
}

void interactive_qa_ui::next_option() {
  if (current_option_index_ < option_count() - 1) {
    current_option_index_++;
  }
}

void interactive_qa_ui::prev_option() {
  if (current_option_index_ > 0) {
    current_option_index_--;
  }
}

void interactive_qa_ui::select_current_option() {
  if (session_.current_question_index >=
      static_cast<int>(session_.questions.size())) {
    return;
  }

  auto &q = session_.questions[session_.current_question_index];

  if (current_option_index_ < static_cast<int>(q.options.size())) {
    // Selected a predefined option
    q.selected_answer = q.options[current_option_index_];
    q.selected_option_index = current_option_index_;
    q.is_custom = false;
  } else if (in_custom_mode_ && !custom_input_.empty()) {
    // Custom answer
    q.selected_answer = custom_input_;
    q.selected_option_index = -1;
    q.is_custom = true;
  }
}

void interactive_qa_ui::toggle_custom_mode() {
  const auto &q = session_.questions[session_.current_question_index];
  in_custom_mode_ = !in_custom_mode_;
  if (in_custom_mode_) {
    current_option_index_ = static_cast<int>(q.options.size());
    if (q.is_custom) {
      custom_input_ = q.selected_answer;
    } else {
      custom_input_.clear();
    }
  }
}

void interactive_qa_ui::handle_custom_input(char32_t ch) {
  if (ch == '\r' || ch == '\n') {
    // Confirm custom input
    if (!custom_input_.empty()) {
      select_current_option();
      in_custom_mode_ = false;
    }
  } else if (ch == 127 || ch == 8) {
    // Backspace
    if (!custom_input_.empty()) {
      custom_input_.pop_back();
    }
  } else if (ch >= 32 && ch < 127) {
    // Printable character
    custom_input_ += static_cast<char>(ch);
  }
}

bool interactive_qa_ui::confirm_abort() {
  printf("\n" ANSI_COLOR_YELLOW "Abort planning? (y/n): " ANSI_COLOR_RESET);
  fflush(stdout);

  char32_t ch = read_char();
  return (ch == 'y' || ch == 'Y');
}

int interactive_qa_ui::option_count() const {
  if (session_.current_question_index >=
      static_cast<int>(session_.questions.size())) {
    return 0;
  }
  return static_cast<int>(session_.questions[session_.current_question_index]
                              .options.size()) +
         1; // +1 for custom
}

bool interactive_qa_ui::all_answered() const { return session_.is_complete(); }

void interactive_qa_ui::save_current_selection() {
  // Save any in-progress custom input when switching tabs
  if (in_custom_mode_ && !custom_input_.empty()) {
    auto &q = session_.questions[session_.current_question_index];
    q.selected_answer = custom_input_;
    q.is_custom = true;
    q.selected_option_index = -1;
  }
}

// Simple approval prompt

bool prompt_approval(const std::string &prompt_text,
                     std::atomic<bool> &is_interrupted) {
  printf("\n%s (y/n): ", prompt_text.c_str());
  fflush(stdout);

#if !defined(_WIN32)
  // Enter raw mode for single character input
  terminal_raw_mode raw_mode;
  if (!raw_mode.is_active()) {
    // Fallback to line-buffered input
    std::string response;
    if (console::readline(response, false)) {
      return !response.empty() && (response[0] == 'y' || response[0] == 'Y');
    }
    return false;
  }
#endif

  while (!is_interrupted.load()) {
    char32_t ch = read_char();
    if (ch == 'y' || ch == 'Y') {
      printf("y\n");
      fflush(stdout);
      return true;
    }
    if (ch == 'n' || ch == 'N' || ch == 27) { // n, N, or ESC
      printf("n\n");
      fflush(stdout);
      return false;
    }
  }

  return false;
}

} // namespace planning
