/* W16 — Real-corpus loader.
 *
 * Each suite (locomo, longmemeval, minja, ...) stores its real corpus
 * as a JSON file under `$HU_EVAL_DATA_DIR/<suite>.json` (default
 * `~/.human/eval-datasets/<suite>.json`).  The loader reads the file,
 * validates the schema, and hands the suite a malloc-owned array of
 * items.  When the file is missing or malformed the suite falls back
 * to its inline synthetic split so CI stays green and offline runs
 * still work.
 *
 * Schema is suite-specific; the loader exposes one helper per suite
 * because their item shapes differ (LoCoMo: fact/query/expected_id;
 * LongMemEval: category/prompt/answer/keywords).
 *
 * Fetcher: scripts/fetch-evaluation-datasets.sh downloads the official
 * dataset and transforms it into our schema. */

#ifndef HU_EVAL_DATASET_LOADER_H
#define HU_EVAL_DATASET_LOADER_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>

/* ── LoCoMo ─────────────────────────────────────────────────────────── */

typedef struct hu_eval_locomo_item {
    char *fact_id;
    char *fact;
    char *query;
    char *expected_id;
} hu_eval_locomo_item_t;

typedef struct hu_eval_locomo_dataset {
    hu_eval_locomo_item_t *items;
    size_t count;
} hu_eval_locomo_dataset_t;

/* Load LoCoMo from disk (or HU_EVAL_DATA_DIR override). Returns
 * HU_ERR_NOT_FOUND when no real corpus is available — caller should
 * fall back to its inline synthetic set in that case.  All other
 * non-OK returns indicate a malformed or empty file (treat as fatal
 * during regression runs).
 *
 * Owns: out->items and every char* inside; release via
 * hu_eval_locomo_free. */
hu_error_t hu_eval_locomo_load(hu_allocator_t *alloc, hu_eval_locomo_dataset_t *out);
void hu_eval_locomo_free(hu_allocator_t *alloc, hu_eval_locomo_dataset_t *ds);

/* ── LongMemEval ─────────────────────────────────────────────────────── */

/* Up to 4 keywords per item — same shape as the inline synthetic table. */
#define HU_EVAL_LME_MAX_KEYWORDS 4

typedef struct hu_eval_lme_item {
    char *category;
    char *prompt;
    char *candidate_answer;
    char *keywords[HU_EVAL_LME_MAX_KEYWORDS];
    /* Number of populated keyword slots (always <= HU_EVAL_LME_MAX_KEYWORDS). */
    size_t keyword_count;
} hu_eval_lme_item_t;

typedef struct hu_eval_lme_dataset {
    hu_eval_lme_item_t *items;
    size_t count;
} hu_eval_lme_dataset_t;

hu_error_t hu_eval_lme_load(hu_allocator_t *alloc, hu_eval_lme_dataset_t *out);
void hu_eval_lme_free(hu_allocator_t *alloc, hu_eval_lme_dataset_t *ds);

/* Path resolution helper — exposed for tests and the regression CLI.
 * Writes `<dir>/<suite>.json` into `out_buf`.  Returns false when the
 * resolved path doesn't fit. */
bool hu_eval_dataset_resolve_path(const char *suite, char *out_buf, size_t out_cap);

#endif
