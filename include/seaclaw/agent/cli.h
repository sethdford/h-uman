#ifndef SC_AGENT_CLI_H
#define SC_AGENT_CLI_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/* Agent CLI: parse args, run loop. */
typedef struct sc_parsed_agent_args {
    const char *message;
    const char *session_id;
    const char *provider_override;
    const char *model_override;
    double temperature_override;
    int has_temperature;
    int use_tui;
} sc_parsed_agent_args_t;

sc_error_t sc_agent_cli_parse_args(const char *const *argv, size_t argc,
    sc_parsed_agent_args_t *out);

sc_error_t sc_agent_cli_run(sc_allocator_t *alloc, const char *const *argv, size_t argc);

#endif /* SC_AGENT_CLI_H */
