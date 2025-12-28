// /compact command implementation
// NOTE: This is a placeholder. The complex compact logic remains in agent.cpp
// for now and can be migrated in a future iteration.

#include "command-handler.h"

namespace agent {

void register_compact_command(command_dispatcher& /* dispatcher */) {
    // /compact is handled inline in agent.cpp for now due to its complexity:
    // - Needs access to run_llm_compaction() function
    // - Requires server_context for LLM calls
    // - Complex message analysis logic
    //
    // Future: Move run_llm_compaction() to a separate module and migrate this command
}

} // namespace agent
