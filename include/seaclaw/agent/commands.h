#ifndef SC_AGENT_COMMANDS_H
#define SC_AGENT_COMMANDS_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
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
sc_error_t sc_agent_commands_bare_session_reset_prompt(sc_allocator_t *alloc,
    const char *msg, size_t len, char **out_prompt);

#endif /* SC_AGENT_COMMANDS_H */
