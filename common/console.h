// Console functions

#pragma once

#include "common.h"

#include <string>

enum display_type {
    DISPLAY_TYPE_RESET = 0,
    DISPLAY_TYPE_INFO,
    DISPLAY_TYPE_PROMPT,
    DISPLAY_TYPE_REASONING,
    DISPLAY_TYPE_USER_INPUT,
    DISPLAY_TYPE_ERROR
};

namespace console {
    void init(bool use_simple_io, bool use_advanced_display);
    void cleanup();
    void set_display(display_type display);
    bool readline(std::string & line, bool multiline_input);

    namespace spinner {
        void start();
        void stop();
    }

    // Subagent depth visualization
    // Creates visual "rails" to show nested agent context
    namespace subagent {
        // Push/pop depth when entering/exiting subagent context
        void push_depth(const std::string& agent_name, int max_iterations = 20);
        void pop_depth(int final_iterations, double elapsed_ms);
        int get_depth();

        // Update status during execution (iteration count, tool calls)
        void update_status(int iteration, int tool_calls = 0);

        // Viewport mode for compact display (last N lines)
        void set_viewport_lines(int max_lines);  // 0 = disabled (default)
        int get_viewport_lines();
    }

    // note: the logging API below output directly to stdout
    // it can negatively impact performance if used on inference thread
    // only use in in a dedicated CLI thread
    // for logging in inference thread, use log.h instead

    LLAMA_COMMON_ATTRIBUTE_FORMAT(1, 2)
    void log(const char * fmt, ...);

    LLAMA_COMMON_ATTRIBUTE_FORMAT(1, 2)
    void error(const char * fmt, ...);

    void flush();
}
