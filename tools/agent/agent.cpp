#include "arg.h"
#include "common.h"
#include "console.h"
#include "llama.h"

#include "agent-loop.h"
#include "commands/command-handler.h"
#include "common/agent-common.h"
#include "common/constants.h"
#include "context/context-manager.h"
#include "permission.h"
#include "skills/skills-manager.h"
#include "subagents/agent-registry.h"
#include "subagents/subagent-manager.h"
#include "tool-registry.h"

#ifndef _WIN32
#include "mcp/mcp-server-manager.h"
#include "mcp/mcp-tool-wrapper.h"
#endif

#include <atomic>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = agent::fs;

// Get the user config directory for llama-agent
static std::string get_config_dir() {
#ifdef _WIN32
  const char *appdata = std::getenv("APPDATA");
  if (appdata) {
    return std::string(appdata) + "\\llama-agent";
  }
  return "";
#else
  const char *home = std::getenv("HOME");
  if (home) {
    return std::string(home) + "/.llama-agent";
  }
  return "";
#endif
}

const char *LLAMA_AGENT_LOGO = R"(
    ____                                                   __
   / / /___ _____ ___  ____ _      ____ _____ ____  ____  / /_
  / / / __ `/ __ `__ \/ __ `/_____/ __ `/ __ `/ _ \/ __ \/ __/
 / / / /_/ / / / / / / /_/ /_____/ /_/ / /_/ /  __/ / / / /_
/_/_/\__,_/_/ /_/ /_/\__,_/      \__,_/\__, /\___/_/ /_/\__/
                                      /____/
)";

static std::atomic<bool> g_is_interrupted = false;

static bool should_stop() { return g_is_interrupted.load(); }

static bool is_stdin_terminal() {
#ifdef _WIN32
  return _isatty(_fileno(stdin));
#else
  return isatty(fileno(stdin));
#endif
}

static std::string read_stdin_prompt() {
  std::string result;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!result.empty()) {
      result += "\n";
    }
    result += line;
  }
  return result;
}

// Load the compaction prompt template
static std::string load_compaction_prompt() {
  // Try to load from prompts folder relative to executable or working directory
  std::vector<std::string> search_paths = {
      "tools/agent/prompts/prompt_compaction.txt", // Development
      "./prompts/prompt_compaction.txt",           // Installed
  };

  for (const auto &path : search_paths) {
    std::ifstream f(path);
    if (f) {
      std::ostringstream ss;
      ss << f.rdbuf();
      return ss.str();
    }
  }

  // Fallback to embedded prompt
  return R"(# Context Compaction

Analyze the conversation and create a JSON summary:

```json
{
  "summary": "2-4 paragraph summary of what was accomplished",
  "key_decisions": {"architectural": [], "implementation": [], "rejected": []},
  "current_state": "Where the work stands now",
  "pending_tasks": ["Unfinished tasks"]
}
```

## Conversation
{{CONVERSATION}})";
}

// Helper struct for LLM compaction result
struct llm_compact_result {
  bool success = false;
  std::string summary;
  json key_decisions;
  std::string current_state;
  std::vector<std::string> pending_tasks;
  std::string error;
};

// Run LLM-based compaction using a temporary agent (context-isolated)
static llm_compact_result
run_llm_compaction(server_context &ctx_server, const common_params &params,
                   const std::string &working_dir, const json &messages,
                   const std::string &user_requirements,
                   std::atomic<bool> &is_interrupted) {

  llm_compact_result result;

  // Load prompt template
  std::string prompt_template = load_compaction_prompt();

  // Build conversation text from messages
  std::ostringstream conv_ss;
  for (const auto &msg : messages) {
    std::string role = msg.value("role", "");
    std::string content = msg.value("content", "");
    if (role == "user" || role == "assistant") {
      conv_ss << "**" << role << "**: " << content << "\n\n";
    }
  }

  // Substitute placeholders
  std::string prompt = prompt_template;
  size_t pos = prompt.find("{{CONVERSATION}}");
  if (pos != std::string::npos) {
    prompt.replace(pos, 16, conv_ss.str());
  }
  pos = prompt.find("{{USER_REQUIREMENTS}}");
  if (pos != std::string::npos) {
    if (!user_requirements.empty()) {
      prompt.replace(
          pos, 21, "\n## Additional Requirements\n" + user_requirements + "\n");
    } else {
      prompt.replace(pos, 21, "");
    }
  }

  // === CONTEXT ISOLATION: Clear slot for compaction agent ===
  // Note: Parent will re-process its context from messages afterwards
  ctx_server.clear_current_slot();

  // Create minimal agent config (no tools)
  agent_config compact_config;
  compact_config.working_dir = working_dir;
  compact_config.max_iterations = 1; // One-shot
  compact_config.tool_timeout_ms = agent::config::COMPACT_TOOL_TIMEOUT_MS;
  compact_config.verbose = false;
  compact_config.yolo_mode = true;
  compact_config.enable_skills = false;
  compact_config.allowed_tools = {}; // No tools - text generation only

  // Run compaction agent
  agent_loop compact_agent(ctx_server, params, compact_config, is_interrupted);
  agent_loop_result loop_result = compact_agent.run(prompt);

  // Parse the response
  std::string response = loop_result.final_response;

  // Extract JSON from response (look for ```json block)
  size_t json_start = response.find("```json");
  if (json_start != std::string::npos) {
    json_start += 7;
    // Skip newline
    if (json_start < response.size() && response[json_start] == '\n') {
      json_start++;
    }
    size_t json_end = response.find("```", json_start);
    if (json_end != std::string::npos) {
      std::string json_str = response.substr(json_start, json_end - json_start);
      try {
        json parsed = json::parse(json_str);
        result.success = true;
        result.summary = parsed.value("summary", "");
        result.key_decisions = parsed.value("key_decisions", json::object());
        result.current_state = parsed.value("current_state", "");

        if (parsed.contains("pending_tasks") &&
            parsed["pending_tasks"].is_array()) {
          for (const auto &task : parsed["pending_tasks"]) {
            result.pending_tasks.push_back(task.get<std::string>());
          }
        }
      } catch (const std::exception &e) {
        result.error = "Failed to parse JSON: " + std::string(e.what());
      }
    } else {
      result.error = "No closing ``` found for JSON block";
    }
  } else {
    // Try to parse the whole response as JSON
    try {
      json parsed = json::parse(response);
      result.success = true;
      result.summary = parsed.value("summary", "");
      result.key_decisions = parsed.value("key_decisions", json::object());
      result.current_state = parsed.value("current_state", "");

      if (parsed.contains("pending_tasks") &&
          parsed["pending_tasks"].is_array()) {
        for (const auto &task : parsed["pending_tasks"]) {
          result.pending_tasks.push_back(task.get<std::string>());
        }
      }
    } catch (...) {
      // Fall back to using raw response as summary
      result.success = true;
      result.summary = response;
    }
  }

  // === CONTEXT ISOLATION: Clear compaction agent's state ===
  ctx_server.clear_current_slot();

  return result;
}

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__)) ||          \
    defined(_WIN32)
static void signal_handler(int) {
  if (g_is_interrupted.load()) {
    fprintf(stdout, "\033[0m\n");
    fflush(stdout);
    std::exit(130);
  }
  g_is_interrupted.store(true);
}
#endif

int main(int argc, char **argv) {
  common_params params;

  params.verbosity = LOG_LEVEL_ERROR;

  // Check for custom flags before common_params_parse
  bool yolo_mode = false;
  int max_iterations = agent::config::DEFAULT_MAX_ITERATIONS;
  bool enable_skills = true;
  std::vector<std::string> extra_skills_paths;
  std::string data_dir; // Empty = use default (project-local .llama-agent/)
  std::string cli_working_dir; // Empty = use current directory

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--yolo") {
      yolo_mode = true;
      // Remove from argv
      for (int j = i; j < argc - 1; j++) {
        argv[j] = argv[j + 1];
      }
      argc--;
      i--; // Re-check this position
    } else if (arg == "--no-skills") {
      enable_skills = false;
      // Remove from argv
      for (int j = i; j < argc - 1; j++) {
        argv[j] = argv[j + 1];
      }
      argc--;
      i--; // Re-check this position
    } else if (arg == "--skills-path") {
      if (i + 1 < argc) {
        extra_skills_paths.push_back(argv[i + 1]);
        // Remove both the flag and its value
        for (int j = i; j < argc - 2; j++) {
          argv[j] = argv[j + 2];
        }
        argc -= 2;
        i--; // Re-check this position
      } else {
        fprintf(stderr, "--skills-path requires a value\n");
        return 1;
      }
    } else if (arg == "--max-iterations" || arg == "-mi") {
      if (i + 1 < argc) {
        try {
          max_iterations = std::stoi(argv[i + 1]);
          if (max_iterations < agent::config::MIN_MAX_ITERATIONS) {
            max_iterations = agent::config::MIN_MAX_ITERATIONS;
          }
          if (max_iterations > agent::config::MAX_MAX_ITERATIONS) {
            max_iterations = agent::config::MAX_MAX_ITERATIONS;
          }
        } catch (...) {
          fprintf(stderr, "Invalid --max-iterations value: %s\n", argv[i + 1]);
          return 1;
        }
        // Remove both the flag and its value
        for (int j = i; j < argc - 2; j++) {
          argv[j] = argv[j + 2];
        }
        argc -= 2;
        i--; // Re-check this position
      } else {
        fprintf(stderr, "--max-iterations requires a value\n");
        return 1;
      }
    } else if (arg == "--data-dir" || arg == "-dd") {
      if (i + 1 < argc) {
        data_dir = argv[i + 1];
        // Remove both the flag and its value
        for (int j = i; j < argc - 2; j++) {
          argv[j] = argv[j + 2];
        }
        argc -= 2;
        i--; // Re-check this position
      } else {
        fprintf(stderr, "--data-dir requires a path\n");
        return 1;
      }
    } else if (arg == "--working-dir" || arg == "-C") {
      if (i + 1 < argc) {
        cli_working_dir = argv[i + 1];
        // Remove both the flag and its value
        for (int j = i; j < argc - 2; j++) {
          argv[j] = argv[j + 2];
        }
        argc -= 2;
        i--; // Re-check this position
      } else {
        fprintf(stderr, "--working-dir requires a path\n");
        return 1;
      }
    }
  }

  if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_CLI)) {
    return 1;
  }

  if (params.conversation_mode == COMMON_CONVERSATION_MODE_DISABLED) {
    console::error("--no-conversation is not supported by llama-agent\n");
    return 1;
  }

  common_init();

  llama_backend_init();
  llama_numa_init(params.numa);

  console::init(params.simple_io, params.use_color);
  atexit([]() { console::cleanup(); });

  console::set_display(DISPLAY_TYPE_RESET);

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
  struct sigaction sigint_action;
  sigint_action.sa_handler = signal_handler;
  sigemptyset(&sigint_action.sa_mask);
  sigint_action.sa_flags = 0;
  sigaction(SIGINT, &sigint_action, NULL);
  sigaction(SIGTERM, &sigint_action, NULL);
#elif defined(_WIN32)
  auto console_ctrl_handler = +[](DWORD ctrl_type) -> BOOL {
    return (ctrl_type == CTRL_C_EVENT) ? (signal_handler(SIGINT), true) : false;
  };
  SetConsoleCtrlHandler(
      reinterpret_cast<PHANDLER_ROUTINE>(console_ctrl_handler), true);
#endif

  // Create server context
  server_context ctx_server;

  console::log("\nLoading model... ");
  console::spinner::start();
  if (!ctx_server.load_model(params)) {
    console::spinner::stop();
    console::error("\nFailed to load the model\n");
    return 1;
  }

  console::spinner::stop();
  console::log("\n");

  // Start inference thread
  std::thread inference_thread([&ctx_server]() { ctx_server.start_loop(); });

  auto inf = ctx_server.get_info();

  // Get working directory
  std::string working_dir;
  if (!cli_working_dir.empty()) {
    // Use CLI-specified directory (resolve to absolute)
    fs::path cli_path(cli_working_dir);
    if (cli_path.is_relative()) {
      cli_path = fs::current_path() / cli_path;
    }
    std::error_code ec;
    fs::path canonical_path = fs::canonical(cli_path, ec);
    if (ec || !fs::is_directory(canonical_path)) {
      console::error(
          "--working-dir path does not exist or is not a directory: %s\n",
          cli_working_dir.c_str());
      return 1;
    }
    working_dir = canonical_path.string();
  } else {
    working_dir = fs::current_path().string();
  }

#ifndef _WIN32
  // Load MCP servers (Unix only - requires fork/pipe)
  mcp_server_manager mcp_mgr;
  int mcp_tools_count = 0;
  std::string mcp_config = find_mcp_config(working_dir);
  if (!mcp_config.empty()) {
    if (mcp_mgr.load_config(mcp_config)) {
      int started = mcp_mgr.start_servers();
      if (started > 0) {
        register_mcp_tools(mcp_mgr);
        mcp_tools_count = (int)mcp_mgr.list_all_tools().size();
      }
    }
  }
#else
  int mcp_tools_count = 0;
#endif

  // Discover skills (agentskills.io spec)
  skills_manager skills_mgr;
  int skills_count = 0;
  if (enable_skills) {
    std::vector<std::string> skill_paths;

    // Project-local skills (highest priority)
    skill_paths.push_back(working_dir + "/.llama-agent/skills");

    // User-global skills
    std::string config_dir = get_config_dir();
    if (!config_dir.empty()) {
      skill_paths.push_back(config_dir + "/skills");
    }

    // Extra paths from --skills-path flags
    skill_paths.insert(skill_paths.end(), extra_skills_paths.begin(),
                       extra_skills_paths.end());

    skills_count = skills_mgr.discover(skill_paths);
  }

  // Determine data directory for all agent outputs (contexts, plans, etc.)
  // Default: project-local .llama-agent/ directory
  // Override with --data-dir or -dd
  std::string agent_data_dir =
      data_dir.empty() ? (working_dir + "/.llama-agent") : data_dir;

  // Initialize context manager for persistence
  context_manager ctx_mgr(agent_data_dir);
  context_id current_context_id = ctx_mgr.create_context();

  // Discover agents (AGENT.md files)
  agent_registry agent_reg;
  int agent_count = 0;
  {
    // Register built-in agents (these have highest precedence, cannot be overridden)
    agent_reg.register_embedded_agents();

    std::vector<std::string> agent_paths;
    // Data-dir agents (highest priority when --data-dir is used)
    agent_paths.push_back(agent_data_dir + "/agents");
    // Project-local agents (if different from data_dir)
    if (agent_data_dir != working_dir + "/.llama-agent") {
      agent_paths.push_back(working_dir + "/.llama-agent/agents");
    }
    // User-global agents
    std::string user_config_dir = get_config_dir();
    if (!user_config_dir.empty()) {
      agent_paths.push_back(user_config_dir + "/agents");
    }
    // Discover disk-based agents (embedded agents always take precedence)
    agent_count = agent_reg.discover(agent_paths);
  }

  // Configure agent
  agent_config config;
  config.working_dir = working_dir;
  config.max_iterations = max_iterations;
  config.tool_timeout_ms = 120000;
  config.verbose = (params.verbosity >= LOG_LEVEL_INFO);
  config.yolo_mode = yolo_mode;
  config.enable_skills = enable_skills;
  config.skills_search_paths = extra_skills_paths;
  config.skills_prompt_section = skills_mgr.generate_prompt_section();

  // Persistence configuration
  // Note: We save messages after each interaction (when run() completes),
  // not per-message, to avoid O(n²) I/O. The on_message callback is kept
  // for future use cases (e.g., streaming to external systems).
  config.ctx_manager = &ctx_mgr;
  config.context_id = current_context_id;
  config.context_base_path =
      agent_data_dir;          // Canonical path for all context operations
  config.on_message = nullptr; // Don't persist per-message; save after run()

  // Create agent loop
  agent_loop agent(ctx_server, params, config, g_is_interrupted);

  // Create subagent manager and inject into agent via tool_context
  // Pass the agent's permission manager so subagents inherit permission state
  subagent_manager subagent_mgr(ctx_server, params, agent_reg, ctx_mgr,
                                working_dir, agent.get_permission_manager());
  agent.set_subagent_manager(&subagent_mgr);

  // Display startup info
  console::log("\n");
  console::log("%s\n", LLAMA_AGENT_LOGO);
  console::log("build      : %s\n", inf.build_info.c_str());
  console::log("model      : %s\n", inf.model_name.c_str());
  console::log("working dir: %s\n", working_dir.c_str());
  console::log("data dir   : %s\n", agent_data_dir.c_str());
  if (yolo_mode) {
    console::set_display(DISPLAY_TYPE_ERROR);
    console::log("mode       : YOLO (all permissions auto-approved)\n");
    console::set_display(DISPLAY_TYPE_RESET);
  }
  if (mcp_tools_count > 0) {
    console::log("mcp tools  : %d\n", mcp_tools_count);
  }
  if (skills_count > 0) {
    console::log("skills     : %d\n", skills_count);
  }
  if (agent_count > 0) {
    console::log("subagents  : %d\n", agent_count);
  }
  console::log("context    : %s\n", current_context_id.substr(0, 8).c_str());
  console::log("\n");

  // Resolve initial prompt from -p/--prompt flag or stdin
  std::string initial_prompt;
  if (!params.prompt.empty()) {
    initial_prompt = params.prompt;
    params.prompt.clear(); // Only use once
  } else if (!is_stdin_terminal()) {
    initial_prompt = read_stdin_prompt();
    // Trim trailing whitespace
    while (!initial_prompt.empty() &&
           (initial_prompt.back() == '\n' || initial_prompt.back() == '\r')) {
      initial_prompt.pop_back();
    }
    // When reading from stdin pipe, always use single-turn mode
    // (stdin is at EOF, so interactive input would spin forever)
    params.single_turn = true;
  }

  // Non-interactive mode: if we have a prompt and single_turn, skip the help
  // text
  if (initial_prompt.empty() || !params.single_turn) {
    console::log("commands:\n");
    console::log("  /exit         exit the agent\n");
    console::log("  /clear        clear and start new conversation\n");
    console::log("  /list         list saved conversations\n");
    console::log("  /switch <id>  switch to a saved conversation\n");
    console::log("  /delete <id>  delete a saved conversation\n");
    console::log("  /compact      compact current context with summary\n");
    console::log("  /plan <task>  spawn planning-agent to create a plan\n");
    console::log("  /stats        show token usage statistics\n");
    console::log("  /tools        list available tools\n");
    console::log("  /skills       list available skills\n");
    console::log("  /subagents    list available subagents\n");
    console::log("  ESC/Ctrl+C    abort generation\n");
    console::log("\n");
  }

  // Set up command dispatcher
  agent::command_dispatcher cmd_dispatcher;
  agent::register_exit_commands(cmd_dispatcher);
  agent::register_context_commands(cmd_dispatcher);
  agent::register_info_commands(cmd_dispatcher);
  agent::register_compact_command(cmd_dispatcher);
  agent::register_plan_command(cmd_dispatcher);

  // Create command context (references to all managers)
  agent::command_context cmd_ctx{
      agent,           ctx_mgr,    skills_mgr, agent_reg,
      subagent_mgr,    ctx_server, params,     current_context_id, working_dir,
      g_is_interrupted};

  // Track if we have an initial prompt to process
  bool first_turn = !initial_prompt.empty();

  // Main loop
  while (true) {
    std::string buffer;

    if (first_turn) {
      // Use the initial prompt
      buffer = initial_prompt;
      first_turn = false;
      console::set_display(DISPLAY_TYPE_USER_INPUT);
      console::log("\n› %s\n", buffer.c_str());
      console::set_display(DISPLAY_TYPE_RESET);
    } else {
      // Interactive input
      console::set_display(DISPLAY_TYPE_USER_INPUT);
      console::log("\n› ");

      std::string line;
      bool another_line = true;
      do {
        another_line = console::readline(line, params.multiline_input);
        buffer += line;
      } while (another_line);

      console::set_display(DISPLAY_TYPE_RESET);

      if (should_stop()) {
        g_is_interrupted.store(false);
        break;
      }

      // Remove trailing newline
      if (!buffer.empty() && buffer.back() == '\n') {
        buffer.pop_back();
      }

      // Skip empty input
      if (buffer.empty()) {
        continue;
      }

      // Dispatch commands through the command handler
      agent::command_result cmd_result =
          cmd_dispatcher.dispatch(buffer, cmd_ctx);
      if (cmd_result == agent::command_result::EXIT) {
        break;
      }
      if (cmd_result == agent::command_result::CONTINUE) {
        continue;
      }

      // Commands not handled by dispatcher (complex commands kept inline for
      // now)
      if (buffer == "/compact" || buffer.rfind("/compact ", 0) == 0) {
        // Get optional user directive for compaction
        std::string user_requirements;
        if (buffer.size() > 9) {
          user_requirements = buffer.substr(9);
        }

        const auto &messages = agent.get_messages();

        // === PHASE 1: Programmatic extraction (reliable, structured) ===
        std::vector<std::string> user_messages_list;
        std::vector<std::string> files_modified;
        std::vector<std::string> commands_run;

        for (const auto &msg : messages) {
          std::string role = msg.value("role", "");

          if (role == "user") {
            std::string content = msg.value("content", "");
            if (!content.empty()) {
              // Truncate very long messages
              if (content.size() > 1000) {
                content = content.substr(0, 997) + "...";
              }
              user_messages_list.push_back(content);
            }
          } else if (role == "assistant") {
            // Extract tool calls
            if (msg.contains("tool_calls")) {
              for (const auto &tc : msg["tool_calls"]) {
                std::string tool_name =
                    tc.value("function", json::object()).value("name", "");
                std::string args_str =
                    tc.value("function", json::object()).value("arguments", "");
                try {
                  json args = json::parse(args_str);
                  if (tool_name == "write" || tool_name == "edit") {
                    std::string path = args.value("file_path", "");
                    if (!path.empty()) {
                      // Deduplicate
                      bool found = false;
                      for (const auto &f : files_modified) {
                        if (f == path) {
                          found = true;
                          break;
                        }
                      }
                      if (!found)
                        files_modified.push_back(path);
                    }
                  } else if (tool_name == "bash") {
                    std::string cmd = args.value("command", "");
                    if (!cmd.empty()) {
                      if (cmd.size() > 200)
                        cmd = cmd.substr(0, 197) + "...";
                      commands_run.push_back(cmd);
                    }
                  } else if (tool_name == "spawn_agent") {
                    // Extract files_modified from subagent result
                    std::string call_id = tc.value("id", "");
                    for (const auto &result_msg : messages) {
                      if (result_msg.value("role", "") == "tool" &&
                          result_msg.value("tool_call_id", "") == call_id) {
                        std::string content = result_msg.value("content", "");
                        try {
                          json result_json = json::parse(content);
                          if (result_json.contains("files_modified")) {
                            for (const auto &f :
                                 result_json["files_modified"]) {
                              std::string path = f.get<std::string>();
                              bool found = false;
                              for (const auto &existing : files_modified) {
                                if (existing == path) {
                                  found = true;
                                  break;
                                }
                              }
                              if (!found)
                                files_modified.push_back(path);
                            }
                          }
                          if (result_json.contains("commands_run")) {
                            for (const auto &c : result_json["commands_run"]) {
                              commands_run.push_back(c.get<std::string>());
                            }
                          }
                        } catch (...) {
                        }
                        break;
                      }
                    }
                  }
                } catch (...) {
                }
              }
            }
          }
        }

        // Check for plan reference
        std::string plan_ref;
        if (ctx_mgr.has_plan(current_context_id)) {
          plan_ref = "plan.md";
        }

        // === PHASE 2: LLM-based summarization (intelligent, contextual) ===
        console::log("\nGenerating summary...\n");
        console::spinner::start();

        llm_compact_result llm_result =
            run_llm_compaction(ctx_server, params, working_dir, messages,
                               user_requirements, g_is_interrupted);

        console::spinner::stop();

        // === PHASE 3: Build hybrid compact_entry ===
        compact_entry entry;

        // Programmatic fields
        entry.user_messages = user_messages_list;
        entry.files_modified = files_modified;
        entry.commands_run = commands_run;
        entry.plan_ref = plan_ref;

        // LLM-generated fields
        if (llm_result.success) {
          entry.summary = llm_result.summary;
          entry.key_decisions = llm_result.key_decisions;
          entry.current_state = llm_result.current_state;
          entry.pending_tasks = llm_result.pending_tasks;
        } else {
          // Fallback summary if LLM failed
          std::ostringstream fallback_ss;
          fallback_ss << "Conversation with " << user_messages_list.size()
                      << " user messages. ";
          if (!files_modified.empty()) {
            fallback_ss << "Modified " << files_modified.size() << " files. ";
          }
          if (!commands_run.empty()) {
            fallback_ss << "Ran " << commands_run.size() << " commands.";
          }
          entry.summary = fallback_ss.str();
          console::set_display(DISPLAY_TYPE_ERROR);
          console::log("LLM summary failed: %s\n", llm_result.error.c_str());
          console::set_display(DISPLAY_TYPE_RESET);
        }

        // === PHASE 4: Save and reload ===
        if (ctx_mgr.compact_context(current_context_id, entry)) {
          console::log("\nContext compacted.\n");

          // Show summary
          console::set_display(DISPLAY_TYPE_INFO);
          console::log("\n--- Summary ---\n%s\n", entry.summary.c_str());
          if (!entry.current_state.empty()) {
            console::log("\n--- Current State ---\n%s\n",
                         entry.current_state.c_str());
          }
          if (!entry.pending_tasks.empty()) {
            console::log("\n--- Pending Tasks ---\n");
            for (const auto &task : entry.pending_tasks) {
              console::log("- %s\n", task.c_str());
            }
          }
          console::set_display(DISPLAY_TYPE_RESET);

          // Reload the compacted context
          auto state_opt = ctx_mgr.load_context(current_context_id);
          if (state_opt) {
            agent.set_messages(state_opt->messages);
          }
        } else {
          console::error("Failed to compact context.\n");
        }
        continue;
      }
      // Note: /plan is now handled by the command dispatcher (cmd-plan.cpp)
      // Note: /tools, /stats, /skills, /subagents are now handled by
      // the dispatcher
    }

    console::log("\n");

    // Run agent loop
    agent_loop_result result = agent.run(buffer);

    // Save conversation after each interaction (batch save for efficiency)
    ctx_mgr.save_messages(current_context_id, agent.get_messages());

    console::log("\n");

    // Display result
    switch (result.stop_reason) {
    case agent_stop_reason::COMPLETED:
      console::set_display(DISPLAY_TYPE_INFO);
      console::log("[Completed in %d iteration(s)]\n", result.iterations);
      console::set_display(DISPLAY_TYPE_RESET);
      break;
    case agent_stop_reason::MAX_ITERATIONS:
      console::set_display(DISPLAY_TYPE_ERROR);
      console::log("[Stopped: max iterations reached (%d)]\n",
                   result.iterations);
      console::set_display(DISPLAY_TYPE_RESET);
      break;
    case agent_stop_reason::USER_CANCELLED:
      console::log("[Cancelled by user]\n");
      g_is_interrupted.store(false);
      break;
    case agent_stop_reason::AGENT_ERROR:
      console::error("[Error occurred]\n");
      break;
    }

    if (params.single_turn) {
      break;
    }
  }

  console::set_display(DISPLAY_TYPE_RESET);
  console::log("\nExiting...\n");

#ifndef _WIN32
  // Shutdown MCP servers
  mcp_mgr.shutdown_all();
#endif

  ctx_server.terminate();
  inference_thread.join();

  common_log_set_verbosity_thold(LOG_LEVEL_INFO);
  llama_memory_breakdown_print(ctx_server.get_llama_context());

  return 0;
}
