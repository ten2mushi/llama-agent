// /plan command implementation - Enhanced interactive planning workflow
//
// Architecture:
// - Explorer-agent: One-shot spawn for codebase exploration
// - Planning-agent: Persistent conversational agent for planning + Q&A refinement
//
// Workflow:
// 1. Spawn explorer-agent (one-shot) for codebase exploration
// 2. Create persistent planning-agent with exploration findings
// 3. Planning-agent synthesizes plan + generates design questions
// 4. Interactive Q&A UI (planning-agent stays warm - no inference during UI)
// 5. Continue same planning-agent with user answers (full context preserved)
// 6. Refinement loop until no more questions
// 7. Final approval → save plan.md

#include "command-handler.h"
#include "../planning/plan-state.h"
#include "../planning/plan-questions.h"
#include "../planning/plan-format.h"
#include "../ui/interactive-qa.h"
#include "../subagents/subagent-manager.h"
#include "../subagents/agent-registry.h"
#include "../agent-loop.h"
#include "../common/agent-common.h"
#include "../common/constants.h"
#include "console.h"

#include <fstream>

namespace agent {

// Forward declarations
static command_result run_planning_workflow(
    planning::planning_state_machine& psm,
    command_context& ctx
);

static command_result resume_planning_session(
    planning::planning_state_machine& psm,
    command_context& ctx
);

// Build task-aware exploration prompt for explorer-agent
static std::string build_task_aware_exploration_prompt(const std::string& task) {
    return "## User Task\n\n" + task + "\n\n"
           "## Your Mission\n\n"
           "Explore the codebase to understand what exists and how the user's task should integrate.\n\n"
           "## Deliverables\n\n"
           "1. **Relevant Files**: List files directly related to the task with brief descriptions\n"
           "2. **Architecture Overview**: How does this codebase organize code?\n"
           "3. **Integration Points**: Where should the new functionality hook in?\n"
           "4. **Existing Patterns**: What conventions/patterns are already in use?\n"
           "5. **Dependencies**: What systems/modules would this task touch?\n\n"
           "Use glob for structure, read for content. Be thorough - your findings will be used to create an implementation plan.";
}

// Build planning prompt with exploration findings for planning-agent
static std::string build_planning_prompt(const std::string& task, const std::string& exploration_findings) {
    return "## User Task\n\n" + task + "\n\n"
           "## Codebase Exploration Results\n\n" + exploration_findings + "\n\n"
           "## Your Mission\n\n"
           "Create a comprehensive implementation plan based on the exploration findings above.\n\n"
           "You do NOT need to explore the codebase - findings are provided above.\n"
           "Focus entirely on strategic planning and design decisions.\n\n"
           "## Required Output\n\n"
           "1. A markdown implementation plan with phases, files to modify, and steps\n"
           "2. **5-7 design decision questions** to align with user intent\n\n"
           "Output questions in JSON format:\n"
           "```json\n"
           "{\n"
           "  \"questions\": [\n"
           "    {\n"
           "      \"id\": 1,\n"
           "      \"text\": \"Which approach do you prefer?\",\n"
           "      \"options\": [\"Option A\", \"Option B\", \"Option C\"]\n"
           "    }\n"
           "  ]\n"
           "}\n"
           "```\n\n"
           "Remember: Ask many thoughtful questions to ensure alignment with user intent.";
}

// Build refinement prompt with user answers
static std::string build_refinement_prompt(
    const std::string& current_plan,
    const planning::qa_session& qa
) {
    std::string prompt = "Based on the user's design decisions, please refine the implementation plan.\n\n";
    prompt += planning::format_answers_for_prompt(qa);
    prompt += "\nCurrent plan:\n" + current_plan + "\n\n";
    prompt += "Please update the plan to reflect these decisions and output:\n";
    prompt += "1. The refined markdown plan\n";
    prompt += "2. Any follow-up questions (if needed) in JSON format\n";
    prompt += "If no more questions are needed, omit the questions JSON block.";
    return prompt;
}

// Extract questions JSON from agent output
static planning::qa_session extract_questions(const std::string& agent_output) {
    std::string json_str;

    // Strategy 1: Look for markdown code fence ```json ... ```
    size_t fence_start = agent_output.find("```json");
    if (fence_start == std::string::npos) {
        // Try case variations
        fence_start = agent_output.find("```JSON");
    }

    if (fence_start != std::string::npos) {
        // Find content start (after ```json and any whitespace)
        size_t content_start = fence_start + 7;
        while (content_start < agent_output.size() &&
               (agent_output[content_start] == '\n' ||
                agent_output[content_start] == '\r' ||
                agent_output[content_start] == ' ')) {
            content_start++;
        }

        // Find closing fence
        size_t content_end = agent_output.find("```", content_start);
        if (content_end != std::string::npos) {
            json_str = agent_output.substr(content_start, content_end - content_start);
            // Trim trailing whitespace
            while (!json_str.empty() && (json_str.back() == '\n' ||
                                          json_str.back() == '\r' ||
                                          json_str.back() == ' ')) {
                json_str.pop_back();
            }
        }
    }

    // Strategy 2: Fallback to finding {"questions" directly if no fence found
    if (json_str.empty()) {
        size_t direct_start = agent_output.find("{\"questions\"");
        if (direct_start != std::string::npos) {
            // Try to find the end by looking for newlines after last ]
            // This is a heuristic fallback
            size_t search_pos = direct_start;
            int brace_count = 0;
            bool in_string = false;
            bool escape_next = false;

            for (size_t i = search_pos; i < agent_output.size(); i++) {
                char c = agent_output[i];

                if (escape_next) {
                    escape_next = false;
                    continue;
                }

                if (c == '\\' && in_string) {
                    escape_next = true;
                    continue;
                }

                if (c == '"') {
                    in_string = !in_string;
                    continue;
                }

                if (!in_string) {
                    if (c == '{') brace_count++;
                    if (c == '}') {
                        brace_count--;
                        if (brace_count == 0) {
                            json_str = agent_output.substr(direct_start, i - direct_start + 1);
                            break;
                        }
                    }
                }
            }
        }
    }

    // Parse the extracted JSON
    if (!json_str.empty()) {
        try {
            json j = json::parse(json_str);
            return planning::parse_questions_from_json(j);
        } catch (const json::exception&) {
            // Parse failed - questions JSON is malformed
        }
    }

    return planning::qa_session{};
}

// Extract plan markdown from agent output (everything before JSON block)
static std::string extract_plan_content(const std::string& agent_output) {
    size_t json_start = agent_output.find("```json");
    if (json_start != std::string::npos) {
        return agent_output.substr(0, json_start);
    }

    // Check for inline JSON
    size_t inline_json = agent_output.find("{\"questions\"");
    if (inline_json != std::string::npos) {
        return agent_output.substr(0, inline_json);
    }

    return agent_output;
}

// Save plan to file
static bool save_plan_file(
    const std::string& plan_path,
    const std::string& content
) {
    // Ensure directory exists
    auto parent = agent::fs::path(plan_path).parent_path();
    if (!agent::fs::exists(parent)) {
        agent::fs::create_directories(parent);
    }

    std::ofstream f(plan_path);
    if (!f) {
        return false;
    }
    f << content;
    return f.good();
}

// Helper to create agent_config for persistent planning agent
static agent_config create_planning_agent_config(
    const agent_definition* agent_def,
    const std::string& working_dir,
    permission_manager* parent_perm_mgr,
    subagent_manager* subagent_mgr,
    const std::string& context_base_path,
    const std::string& custom_system_prompt = ""
) {
    agent_config cfg;
    cfg.working_dir = working_dir;
    cfg.max_iterations = agent_def->max_iterations;
    cfg.tool_timeout_ms = agent::config::DEFAULT_TOOL_TIMEOUT_MS;
    cfg.verbose = false;
    cfg.yolo_mode = false;
    cfg.parent_permission_mgr = parent_perm_mgr;
    cfg.subagent_mgr = subagent_mgr;
    cfg.context_base_path = context_base_path;
    cfg.allowed_tools = agent_def->allowed_tools;
    cfg.custom_system_prompt = custom_system_prompt;  // Override default system prompt
    return cfg;
}

// Main /plan command handler
static command_result handle_plan_impl(const std::string& args, command_context& ctx) {
    std::string task = args;

    // Check if planning-agent is available
    const agent_definition* planning_agent = ctx.agent_reg.get_agent("planning-agent");
    if (!planning_agent) {
        console::error("planning-agent not found.\n");
        console::log("Create ~/.llama-agent/agents/planning-agent/AGENT.md to enable planning.\n");
        return command_result::CONTINUE;
    }

    // Initialize state machine
    planning::planning_state_machine psm(ctx.ctx_mgr);

    // Check for existing session
    if (psm.has_saved_session(ctx.current_context_id)) {
        if (psm.load(ctx.current_context_id)) {
            if (psm.is_active()) {
                console::log("Found existing planning session (state: %s).\n",
                             planning::state_to_string(psm.current_state()));

                // Ask user if they want to resume or start fresh
                console::log("Resume existing session? (y/n): ");
                console::flush();

                std::string response;
                if (console::readline(response, false)) {
                    if (response.find('y') != std::string::npos ||
                        response.find('Y') != std::string::npos) {
                        return resume_planning_session(psm, ctx);
                    }
                }

                console::log("Starting fresh planning session...\n");
            }
        }
    }

    // Need a task for new sessions
    if (task.empty()) {
        console::error("Usage: /plan <task description>\n");
        return command_result::CONTINUE;
    }

    // Start new planning session
    if (!psm.start(task, ctx.current_context_id)) {
        console::error("Failed to start planning session.\n");
        return command_result::CONTINUE;
    }

    return run_planning_workflow(psm, ctx);
}

static command_result run_planning_workflow(
    planning::planning_state_machine& psm,
    command_context& ctx
) {
    const agent_definition* explorer_agent = ctx.agent_reg.get_agent("explorer-agent");
    const agent_definition* planning_agent = ctx.agent_reg.get_agent("planning-agent");
    if (!explorer_agent || !planning_agent) {
        console::error("Required agents not found (explorer-agent, planning-agent).\n");
        psm.abort();
        return command_result::CONTINUE;
    }

    console::log("\n");
    console::set_display(DISPLAY_TYPE_INFO);
    console::log("Starting planning workflow for: %s\n", psm.session().task.c_str());
    console::set_display(DISPLAY_TYPE_RESET);

    // === STEP 1: Exploration (explorer-agent - one-shot) ===
    psm.transition_to(planning::planning_state::EXPLORING);
    console::log("\n[Step 1/5: Exploring codebase...]\n\n");

    subagent_request explore_req;
    explore_req.agent_name = "explorer-agent";
    explore_req.task = build_task_aware_exploration_prompt(psm.session().task);
    explore_req.max_iterations = explorer_agent->max_iterations;

    subagent_result explore_result = ctx.subagent_mgr.spawn(
        explore_req, ctx.agent.get_messages(), ctx.is_interrupted);

    if (!explore_result.success) {
        console::error("Exploration failed: %s\n", explore_result.error.c_str());
        psm.abort();
        return command_result::CONTINUE;
    }

    std::string exploration_findings = explore_result.output;
    psm.set_exploration_findings(exploration_findings);

    // === STEP 2: Create persistent planning agent ===
    // This agent stays alive for the entire Q&A loop, preserving full context
    psm.transition_to(planning::planning_state::SYNTHESIZING);
    console::log("\n[Step 2/5: Synthesizing plan...]\n\n");

    // Generate the planning agent's specialized system prompt
    // This will be used as the SYSTEM message (not buried in USER message)
    std::string planning_system_prompt = ctx.subagent_mgr.generate_system_prompt(*planning_agent);

    // Configure the persistent planning agent with custom system prompt
    agent_config planning_config = create_planning_agent_config(
        planning_agent,
        ctx.working_dir,
        ctx.agent.get_permission_manager(),
        &ctx.subagent_mgr,
        ctx.ctx_mgr.base_path(),
        planning_system_prompt  // Custom system prompt for planning agent
    );

    // Clear KV cache for fresh planning context
    ctx.server_ctx.clear_current_slot();

    // Create the persistent planning agent (lives for entire Q&A workflow)
    agent_loop planning_loop(ctx.server_ctx, ctx.params, planning_config, ctx.is_interrupted);

    // Build user prompt with ONLY task + exploration findings
    // (System instructions are now in the SYSTEM message via custom_system_prompt)
    std::string user_prompt = build_planning_prompt(psm.session().task, exploration_findings);

    // Show planning agent context (visual indicator)
    console::subagent::push_depth("planning-agent", planning_agent->max_iterations);

    // First turn: get initial plan + questions
    agent_loop_result plan_result = planning_loop.run(user_prompt);

    if (plan_result.stop_reason != agent_stop_reason::COMPLETED) {
        console::subagent::pop_depth(plan_result.iterations, 0);
        console::error("Planning failed.\n");
        psm.abort();
        ctx.server_ctx.clear_current_slot();
        return command_result::CONTINUE;
    }

    std::string plan_content = extract_plan_content(plan_result.final_response);
    psm.set_plan_content(plan_content);

    // Extract questions from planning-agent output
    planning::qa_session qa = extract_questions(plan_result.final_response);

    // === STEP 3 & 4: Interactive Q&A loop (same agent, multiple turns) ===
    while (!qa.questions.empty()) {
        psm.transition_to(planning::planning_state::QUESTIONING);
        psm.set_questions(qa.to_json());
        psm.save();

        console::log("\n[Step 3/5: Design decisions needed]\n");
        console::log("Found %zu questions for you to answer.\n\n", qa.questions.size());

        // Show interactive Q&A UI (planning agent stays warm - KV cache preserved)
        psm.transition_to(planning::planning_state::AWAITING_ANSWERS);

        planning::qa_result ui_result = planning::interactive_qa_ui::show(qa, ctx.is_interrupted);

        if (ui_result == planning::qa_result::ABORTED) {
            console::log("\nPlanning aborted by user.\n");
            console::subagent::pop_depth(plan_result.iterations, 0);
            psm.abort();
            ctx.server_ctx.clear_current_slot();
            return command_result::CONTINUE;
        }

        if (ui_result == planning::qa_result::INTERRUPTED) {
            console::log("\nPlanning interrupted. Session saved for later resume.\n");
            console::subagent::pop_depth(plan_result.iterations, 0);
            psm.save();
            ctx.server_ctx.clear_current_slot();
            return command_result::CONTINUE;
        }

        // Save answers
        psm.set_answers(qa.to_json());

        // === Continue same agent with user answers (full context preserved!) ===
        psm.transition_to(planning::planning_state::REFINING);
        psm.increment_iteration();
        console::log("\n[Step 4/5: Refining plan based on your decisions (iteration %d)...]\n\n",
                     psm.current_iteration());

        // Build continuation prompt (just the answers - context already established)
        std::string continuation = planning::format_answers_for_prompt(qa);
        continuation += "\n\nPlease refine the plan based on these decisions. "
                        "If any critical design decisions remain unclear, generate follow-up questions.";

        // Continue the SAME planning agent (preserves full context)
        agent_loop_result refine_result = planning_loop.run(continuation);

        if (refine_result.stop_reason != agent_stop_reason::COMPLETED) {
            console::error("Refinement failed.\n");
            // Continue with current plan
            break;
        }

        // Update plan
        plan_content = extract_plan_content(refine_result.final_response);
        psm.set_plan_content(plan_content);

        // Check for follow-up questions
        qa = extract_questions(refine_result.final_response);
    }

    // Clean up planning agent visual context
    console::subagent::pop_depth(planning_loop.get_stats().total_output, 0);

    // Clear planning agent's KV cache
    ctx.server_ctx.clear_current_slot();

    // === STEP 5: Approval ===
    psm.transition_to(planning::planning_state::AWAITING_APPROVAL);
    console::log("\n[Step 5/5: Plan ready for approval]\n\n");

    // Show final plan summary
    console::set_display(DISPLAY_TYPE_INFO);
    console::log("=== Final Plan ===\n");
    console::set_display(DISPLAY_TYPE_RESET);

    // Show first ~50 lines of plan
    std::istringstream plan_stream(plan_content);
    std::string line;
    int line_count = 0;
    while (std::getline(plan_stream, line) && line_count < 50) {
        console::log("%s\n", line.c_str());
        line_count++;
    }
    if (plan_stream.good()) {
        console::log("\n... (truncated, full plan will be saved to file)\n");
    }

    console::log("\n");

    if (planning::prompt_approval("Approve this plan?", ctx.is_interrupted)) {
        psm.transition_to(planning::planning_state::APPROVED);

        // Add metadata header to plan
        planning::plan_data data;
        data.task_summary = psm.session().task;
        data.created_at = psm.session().created_at;
        data.version = psm.current_iteration() + 1;
        data.status = "approved";
        data.plan_body = plan_content;

        std::string final_plan = planning::plan_formatter::generate(data);

        // Save to file
        std::string plan_path = psm.get_plan_path();
        if (save_plan_file(plan_path, final_plan)) {
            psm.session().plan_path = plan_path;
            psm.save();

            console::set_display(DISPLAY_TYPE_INFO);
            console::log("\nPlan approved and saved to: %s\n", plan_path.c_str());
            console::log("Context ID: %s\n", psm.session().context_id.c_str());
            console::log("\nTo implement this plan, you can:\n");
            console::log("  - Ask: \"read the plan and implement each phase\"\n");
            console::log("  - Or use: read_plan (will find the most recent plan)\n");
            console::set_display(DISPLAY_TYPE_RESET);
        } else {
            console::error("Failed to save plan to: %s\n", plan_path.c_str());
        }
    } else {
        console::log("\nPlan not approved. Session saved for later.\n");
        psm.save();
    }

    return command_result::CONTINUE;
}

static command_result resume_planning_session(
    planning::planning_state_machine& psm,
    command_context& ctx
) {
    console::log("Resuming planning session from state: %s\n",
                 planning::state_to_string(psm.current_state()));

    // Based on current state, resume at appropriate step
    switch (psm.current_state()) {
        case planning::planning_state::EXPLORING:
        case planning::planning_state::SYNTHESIZING:
            // Restart from beginning
            return run_planning_workflow(psm, ctx);

        case planning::planning_state::QUESTIONING:
        case planning::planning_state::AWAITING_ANSWERS: {
            // Resume Q&A
            planning::qa_session qa = planning::qa_session::from_json(psm.session().questions);
            if (qa.questions.empty()) {
                // No questions, go to approval
                psm.transition_to(planning::planning_state::AWAITING_APPROVAL);
            } else {
                planning::qa_result result = planning::interactive_qa_ui::show(qa, ctx.is_interrupted);
                if (result != planning::qa_result::COMPLETED) {
                    psm.save();
                    return command_result::CONTINUE;
                }
                psm.set_answers(qa.to_json());
            }
            // Fall through to approval
        }
        [[fallthrough]];

        case planning::planning_state::REFINING:
        case planning::planning_state::AWAITING_APPROVAL: {
            psm.transition_to(planning::planning_state::AWAITING_APPROVAL);

            console::log("\n=== Current Plan ===\n%s\n", psm.session().plan_content.c_str());

            if (planning::prompt_approval("Approve this plan?", ctx.is_interrupted)) {
                psm.transition_to(planning::planning_state::APPROVED);
                console::log("Plan approved!\n");
            } else {
                console::log("Plan not approved.\n");
            }
            break;
        }

        case planning::planning_state::APPROVED:
            console::log("Plan already approved. Path: %s\n", psm.session().plan_path.c_str());
            break;

        case planning::planning_state::ABORTED:
            console::log("Previous session was aborted. Starting fresh...\n");
            psm.start(psm.session().task, psm.session().context_id);
            return run_planning_workflow(psm, ctx);

        default:
            break;
    }

    return command_result::CONTINUE;
}

void register_plan_command(command_dispatcher& dispatcher) {
    dispatcher.register_command("/plan", [](const std::string& args, command_context& ctx) {
        return handle_plan_impl(args, ctx);
    });
}

} // namespace agent
