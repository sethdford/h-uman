#ifndef HU_AGENT_COMMANDS_H
#define HU_AGENT_COMMANDS_H

#include "human/agent/planner.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>

/* Slash command parsing. */
typedef struct hu_slash_cmd {
    const char *name;
    size_t name_len;
    const char *arg;
    size_t arg_len;
} hu_slash_cmd_t;

/* Parse slash command from message. Returns NULL if not a slash command. */
const hu_slash_cmd_t *hu_agent_commands_parse(const char *msg, size_t len);

/* Bare session reset prompt for /new or /reset. Caller must free. */
hu_error_t hu_agent_commands_bare_session_reset_prompt(hu_allocator_t *alloc, const char *msg,
                                                       size_t len, char **out_prompt);

/* Free owned tool calls (used by agent history and slash retry). */
void hu_agent_commands_free_owned_tool_calls(hu_allocator_t *alloc, hu_tool_call_t *tcs,
                                             size_t count);

/* Execute plan steps (used by /goal and hu_agent_execute_plan). Implemented in agent_plan.c. */
typedef struct hu_agent hu_agent_t;
hu_error_t hu_agent_commands_execute_plan_steps(hu_agent_t *agent, hu_plan_t *plan,
                                                char **summary_out, size_t *summary_len_out,
                                                const char *original_goal,
                                                size_t original_goal_len);

#endif /* HU_AGENT_COMMANDS_H */
