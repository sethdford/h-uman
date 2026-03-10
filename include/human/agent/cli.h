#ifndef HU_AGENT_CLI_H
#define HU_AGENT_CLI_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* Agent CLI: parse args, run loop. */
typedef struct hu_parsed_agent_args {
    const char *message;
    const char *session_id;
    const char *provider_override;
    const char *model_override;
    double temperature_override;
    int has_temperature;
    int use_tui;
    int demo_mode;
} hu_parsed_agent_args_t;

hu_error_t hu_agent_cli_parse_args(const char *const *argv, size_t argc,
                                   hu_parsed_agent_args_t *out);

hu_error_t hu_agent_cli_run(hu_allocator_t *alloc, const char *const *argv, size_t argc);

#endif /* HU_AGENT_CLI_H */
