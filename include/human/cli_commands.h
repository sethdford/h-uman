#ifndef HU_CLI_COMMANDS_H
#define HU_CLI_COMMANDS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdio.h>

hu_error_t cmd_channel(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_hardware(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_memory(hu_allocator_t *alloc, int argc, char **argv);
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
/* Emits `human setup local-model` report to `out` (stdout from cmd_setup); used by tests. */
hu_error_t hu_cli_setup_local_model_emit(FILE *out);
#ifdef HU_ENABLE_FEEDS
hu_error_t cmd_feed(hu_allocator_t *alloc, int argc, char **argv);
#endif
hu_error_t cmd_research(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_calibrate(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_drafts(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_hula(hu_allocator_t *alloc, int argc, char **argv);

#endif /* HU_CLI_COMMANDS_H */
