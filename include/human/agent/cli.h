#ifndef HU_AGENT_CLI_H
#define HU_AGENT_CLI_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* Agent CLI: parse args, run loop. */
typedef struct hu_parsed_agent_args {
    const char *config_path;
    const char *message;
    const char *session_id;
    const char *contact_id;   /* --contact: binds agent->memory_session_id so
                               * per-contact memory recall + GraphRAG community-
                               * summary grounding fire on a one-shot CLI turn.
                               * The daemon binds this from the channel contact;
                               * the CLI previously had no equivalent seam. */
    const char *history_file; /* --history-file: JSONL of preceding turns,
                               * {"from":"them"|"seth","text":"..."} one per
                               * line, oldest first, seeded into agent->history
                               * BEFORE a -m one-shot turn. The daemon always
                               * has thread history; the CLI had no seam for it,
                               * so eval harnesses compared a context-free model
                               * reply against a context-rich human one and the
                               * judge simply detected the missing memory. A
                               * file (not an argv string) keeps arbitrary
                               * message text away from shell quoting. */
    const char *provider_override;
    const char *model_override;
    double temperature_override;
    int has_temperature;
    int use_tui;
    int demo_mode;
    const char *prompt;
    const char *channel;
    int once;
} hu_parsed_agent_args_t;

hu_error_t hu_agent_cli_parse_args(const char *const *argv, size_t argc,
                                   hu_parsed_agent_args_t *out);

hu_error_t hu_agent_cli_run(hu_allocator_t *alloc, const char *const *argv, size_t argc);

#endif /* HU_AGENT_CLI_H */
