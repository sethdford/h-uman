/* include/human/ml/lora_export.h
 *
 * LoRA-training-data export — Sprint B C-loop (2026-05-24).
 *
 * Sprint A's reaction-ingest pipeline has been writing dpo_pairs to
 * the daemon's SQLite collector for months. The lora-persona trainer
 * consumes JSONL with {prompt, chosen, rejected} rows. This module
 * bridges the two — extracting collector rows into a JSONL file that
 * `human ml lora-persona --pairs <file>` can read.
 *
 * Why this is the M3-bridge unlock: until now, lora-persona trained
 * on synthetic persona example banks. With this exporter, it trains
 * on REAL preference pairs derived from the user's actual reactions —
 * which is the whole point of M3 ("Private Learning").
 *
 * Pure layers (mirror earlier B-series modules):
 *
 *   1. SQL scanner — reads dpo_pairs from a sqlite3 db handle into a
 *      bounded array of in-memory records.
 *   2. JSONL writer — pure escaper + writer that takes records and
 *      emits one JSON object per line.
 *   3. CLI glue — `human ml export-dpo --db <path> --out <jsonl>
 *      [--since-days N]` wires them together with a file-handle.
 */
#ifndef HU_ML_LORA_EXPORT_H
#define HU_ML_LORA_EXPORT_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_LORA_EXPORT_PROMPT_MAX 2048
#define HU_LORA_EXPORT_RESP_MAX   2048

typedef struct hu_lora_export_pair {
    char prompt[HU_LORA_EXPORT_PROMPT_MAX];
    char chosen[HU_LORA_EXPORT_RESP_MAX];
    char rejected[HU_LORA_EXPORT_RESP_MAX];
    int64_t timestamp;
} hu_lora_export_pair_t;

/* Pure: JSON-escape `src` into `dst` (NUL-terminated, bounded). Quotes,
 * backslashes, and control chars are escaped. Returns bytes written
 * (excluding NUL). Exposed for unit testing. */
size_t hu_lora_export_json_escape(const char *src, char *dst, size_t cap);

/* Pure: render one pair as a single JSONL line into `out` (no trailing
 * newline; caller appends or uses fputc). Returns bytes written, or 0
 * when out is too small or any required field is empty (an empty
 * chosen or prompt is unusable for DPO training and is silently
 * dropped — same gate the writer applies). */
size_t hu_lora_export_render_jsonl_line(const hu_lora_export_pair_t *pair, char *out, size_t cap);

/* Read dpo_pairs from a SQLite database, filter by timestamp window,
 * and write JSONL to `out_file_path`. Returns the number of rows
 * written via *out_count. Returns HU_ERR_NOT_SUPPORTED on builds
 * without SQLite. */
hu_error_t hu_lora_export_dpo_pairs(hu_allocator_t *alloc, const char *db_path,
                                    const char *out_file_path, int64_t since_unix,
                                    size_t *out_count);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_LORA_EXPORT_H */
