#ifndef HU_CLI_COMMANDS_H
#define HU_CLI_COMMANDS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

hu_error_t cmd_channel(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_hardware(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_memory(hu_allocator_t *alloc, int argc, char **argv);
/* human prospective init|probe|backfill (src/app/cli_prospective.c). */
hu_error_t cmd_prospective(hu_allocator_t *alloc, int argc, char **argv);
/* Emits the `human memory search --semantic|--hybrid` result lines to `out`:
 *   "  [<rank>] <key> (<score>): <content>"   (content truncated to 2000 bytes)
 * One line per entry; scripts/eval_memory_benchmarks.py parses <key> out of
 * this exact shape. Does not free `res`. Exposed for tests. */
struct hu_retrieval_result;
void hu_cli_memory_search_emit(FILE *out, const struct hu_retrieval_result *res);
/* Pure: bytes of content[0, len) that `human memory search` prints for one hit.
 * Caps at 2000 bytes, backed off over UTF-8 continuation bytes so the cut never
 * splits a multi-byte sequence. Returns len when len <= 2000; 0 on NULL. */
size_t hu_cli_memory_print_len(const char *content, size_t len);
hu_error_t cmd_workspace(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_config(hu_allocator_t *alloc, int argc, char **argv);
/** Prints top-level config key documentation to `out` (used by `human config schema` and tests). */
hu_error_t hu_cli_config_schema_emit(FILE *out);
hu_error_t cmd_capabilities(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_models(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_auth(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_update(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_sandbox(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_eval(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_evaluation(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_init(hu_allocator_t *alloc, int argc, char **argv);
/* What `human init` does about an existing config. Prompting when stdin is
 * not a terminal would block scripts and CI forever, so that case refuses
 * and names --force instead. */
typedef enum hu_init_decision {
    HU_INIT_PROCEED = 0, /* write the config */
    HU_INIT_PROMPT,      /* ask "Overwrite? [y/N]" on the terminal */
    HU_INIT_REFUSE,      /* leave it alone; tell the user about --force */
} hu_init_decision_t;
hu_init_decision_t hu_init_overwrite_decision(bool config_exists, bool force, bool stdin_is_tty);
hu_error_t cmd_setup(hu_allocator_t *alloc, int argc, char **argv);
/* `human initiative <log|status>` — read-only views of the JSONL written
 * by the init_proposer subsystem. Impl in src/agent/init_outcome.c. */
hu_error_t cmd_initiative(hu_allocator_t *alloc, int argc, char **argv);
/* Emits `human setup local-model` report to `out` (stdout from cmd_setup); used by tests. */
hu_error_t hu_cli_setup_local_model_emit(FILE *out);
#ifdef HU_ENABLE_FEEDS
hu_error_t cmd_feed(hu_allocator_t *alloc, int argc, char **argv);
#endif
hu_error_t cmd_research(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_calibrate(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_drafts(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_narrate(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_reply_prompt(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_autoresponder(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_export_dpo(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_export_kto(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_hula(hu_allocator_t *alloc, int argc, char **argv);

/* Argument parser for `human memory reindex [--limit N] [--full]`; pure so a
 * test can pin that a trailing `--full` is honoured (cli_commands.c). */
void hu_cli_parse_reindex_args(int argc, char **argv, size_t *limit_out, bool *full_out);

/* `human memory ground [--full] <contact> <message>` (argv[3..]). False when
 * the contact is missing/empty or the message is missing. Pure. */
bool hu_cli_parse_ground_args(int argc, char **argv, const char **contact_out, const char **msg_out,
                              bool *full_out);

struct hu_graph_ground_turn_stats;
/* `human memory ground` output. Plain (lexical compose only, unchanged):
 *   "matched=<n> bytes=<b>"
 * --full (hu_graph_ground_compose_turn, the live turn's composition):
 *   "matched=<n> bytes=<b> fallback=<0|1> self=<0|1> names=<typed lines>"
 * then the block, if any. scripts/eval_name_grounding.py parses this shape. */
void hu_cli_memory_ground_emit(FILE *out, bool full, size_t matched,
                               const struct hu_graph_ground_turn_stats *stats, const char *ctx,
                               size_t ctx_len);

#endif /* HU_CLI_COMMANDS_H */
