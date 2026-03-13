#ifndef HU_ML_TOKENIZER_H
#define HU_ML_TOKENIZER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * BPE tokenizer for ML training
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_bpe_tokenizer hu_bpe_tokenizer_t;

hu_error_t hu_bpe_tokenizer_create(hu_allocator_t *alloc, hu_bpe_tokenizer_t **out);

hu_error_t hu_bpe_tokenizer_load(hu_bpe_tokenizer_t *tok, const char *path);

hu_error_t hu_bpe_tokenizer_train(hu_bpe_tokenizer_t *tok, const char **texts,
                                  size_t texts_count, size_t vocab_size,
                                  const char *pattern);

hu_error_t hu_bpe_tokenizer_encode(const hu_bpe_tokenizer_t *tok, const char *text,
                                   size_t text_len, int32_t **ids_out,
                                   size_t *ids_count);

hu_error_t hu_bpe_tokenizer_decode(const hu_bpe_tokenizer_t *tok, const int32_t *ids,
                                   size_t ids_count, char **text_out,
                                   size_t *text_len_out);

size_t hu_bpe_tokenizer_vocab_size(const hu_bpe_tokenizer_t *tok);

size_t hu_bpe_tokenizer_token_byte_length(const hu_bpe_tokenizer_t *tok,
                                          int32_t token_id);

hu_error_t hu_bpe_tokenizer_save(const hu_bpe_tokenizer_t *tok, const char *path);

void hu_bpe_tokenizer_deinit(hu_bpe_tokenizer_t *tok);

#endif
