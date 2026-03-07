#ifndef SC_AGENT_COMMANDS_H
#define SC_AGENT_COMMANDS_H

#include "seaclaw/agent/planner.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include <stddef.h>

/* Slash command parsing. */
typedef struct sc_slash_cmd {
    const char *name;
    size_t name_len;
    const char *arg;
    size_t arg_len;
} sc_slash_cmd_t;

/* Parse slash command from message. Returns NULL if not a slash command. */
const sc_slash_cmd_t *sc_agent_commands_parse(const char *msg, size_t len);

/* Bare session reset prompt for /new or /reset. Caller must free. */
sc_error_t sc_agent_commands_bare_session_reset_prompt(sc_allocator_t *alloc, const char *msg,
                                                       size_t len, char **out_prompt);

/* Free owned tool calls (used by agent history and slash retry). */
void sc_agent_commands_free_owned_tool_calls(sc_allocator_t *alloc, sc_tool_call_t *tcs,
                                             size_t count);

/* Execute plan steps (used by /goal and sc_agent_execute_plan). Implemented in agent_slash.c. */
typedef struct sc_agent sc_agent_t;
sc_error_t sc_agent_commands_execute_plan_steps(sc_agent_t *agent, sc_plan_t *plan,
                                                char **summary_out, size_t *summary_len_out,
                                                const char *original_goal,
                                                size_t original_goal_len);

#endif /* SC_AGENT_COMMANDS_H */
