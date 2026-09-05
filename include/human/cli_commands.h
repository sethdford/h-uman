#ifndef HU_CLI_COMMANDS_H
#define HU_CLI_COMMANDS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdio.h>

hu_error_t cmd_channel(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_hardware(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_memory(hu_allocator_t *alloc, int argc, char **argv);
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
hu_error_t cmd_autoresponder(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_export_dpo(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_export_kto(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_hula(hu_allocator_t *alloc, int argc, char **argv);

#endif /* HU_CLI_COMMANDS_H */
