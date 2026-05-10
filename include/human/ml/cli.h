#ifndef HU_ML_CLI_H
#define HU_ML_CLI_H

#include "human/core/allocator.h"
#include "human/core/error.h"

hu_error_t hu_ml_cli_train(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_experiment(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_prepare(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_status(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_dpo_train(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_prepare_conversations(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_lora_persona(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_train_feed_predictor(hu_allocator_t *alloc, int argc, const char **argv);
/* W13 — End-to-end adapter loading: open the local huml provider, call
 * hu_provider_load_adapter, print the active_adapter id. Closes the loop
 * "trained adapter on disk → loaded by provider runtime" so the W13
 * pipeline is verifiable from a single CLI command. */
hu_error_t hu_ml_cli_apply_adapter(hu_allocator_t *alloc, int argc, const char **argv);

#endif
