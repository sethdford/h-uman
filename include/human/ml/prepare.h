#ifndef HU_ML_PREPARE_H
#define HU_ML_PREPARE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/tokenizer_ml.h"
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Data preparation utilities
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_ml_prepare_tokenize_file(hu_allocator_t *alloc, hu_bpe_tokenizer_t *tok,
                                       const char *input_path, const char *output_path);

hu_error_t hu_ml_prepare_tokenize_dir(hu_allocator_t *alloc, hu_bpe_tokenizer_t *tok,
                                      const char *input_dir, const char *output_dir);

hu_error_t hu_ml_prepare_token_bytes(hu_allocator_t *alloc, hu_bpe_tokenizer_t *tok,
                                     int32_t **token_bytes_out, size_t *count);

/* Load a BPE tokenizer using the project convention (`data_dir/tokenizer.vocab`
 * → `~/.human/models/tokenizer.vocab` → default 256-byte byte-level BPE) and
 * derive the `token_bytes` lookup table needed by `hu_ml_train` to compute
 * bits-per-byte against arbitrary vocabularies.
 *
 * On success, ownership transfers to the caller:
 *   - free `*out_tok` with `hu_bpe_tokenizer_deinit`
 *   - free `*out_token_bytes` with `alloc->free(alloc->ctx, ptr, *out_count * sizeof(int32_t))`
 * `*out_count` is the tokenizer's vocab size. Callers should align their
 * model's vocab dimension to this value before training so the CE loss
 * matches the data. See cli.c::hu_ml_cli_train and experiment.c::run_single_experiment
 * for canonical use. */
hu_error_t hu_ml_prepare_load_default_tokenizer(hu_allocator_t *alloc, const char *data_dir,
                                                hu_bpe_tokenizer_t **out_tok,
                                                int32_t **out_token_bytes, size_t *out_count);

/* Prepare conversation data from chat.db + memory.db into tokenized .bin files.
 * Reads iMessage conversations and memory entries, formats as turn-delimited text,
 * tokenizes via BPE, and writes train/val .bin splits to output_dir. */
hu_error_t hu_ml_prepare_conversations(hu_allocator_t *alloc, hu_bpe_tokenizer_t *tok,
                                       const char *chat_db_path, const char *memory_db_path,
                                       const char *output_dir, size_t *messages_processed);

#endif
