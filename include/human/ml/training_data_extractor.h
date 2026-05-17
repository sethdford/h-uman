#ifndef HU_ML_TRAINING_DATA_EXTRACTOR_H
#define HU_ML_TRAINING_DATA_EXTRACTOR_H

/* Continuous learning loop — training data extraction.
 *
 * Reads recent conversations from the `messages` table in memory.db,
 * formats them as chat-format JSONL (system prompt + messages array),
 * writes the output to the training data directory, and marks messages
 * as "extracted" so they are not re-processed.
 *
 * Also provides auto-DPO pair generation: when a user sends a correction
 * immediately after the agent's response, the corrected exchange becomes
 * a (prompt, chosen, rejected) DPO pair in the `dpo_pairs` table.
 *
 * Gated behind HU_ENABLE_SQLITE — the messages table lives on the
 * SQLite memory backend. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Extract recent unprocessed conversations from memory.db and write
 * them as chat-format JSONL files suitable for LoRA fine-tuning.
 *
 * Each session in `messages` whose rows have not been extracted is
 * formatted as a single JSONL line containing:
 *   { "messages": [ {"role":"system","content":"..."}, {"role":"user","content":"..."}, ... ] }
 *
 * The persona_path (may be NULL) is used to inject a system prompt
 * matching the persona identity. If NULL, a default system prompt is
 * used.
 *
 * On success, `*extracted_count` is the number of new JSONL examples
 * written. Returns HU_OK even when no new data exists (count = 0). */
hu_error_t hu_training_data_extract(hu_allocator_t *alloc, const char *memory_db_path,
                                    const char *persona_path, const char *output_dir,
                                    size_t *extracted_count);

/* Minimum number of new training examples before the scheduler should
 * enqueue a LoRA retraining job. */
#define HU_TRAINING_DATA_RETRAIN_THRESHOLD 50

/* Scan recent messages for auto-DPO patterns: when a user immediately
 * corrects the agent (sends a message within `correction_window_sec`
 * of the agent's response), record a DPO pair with:
 *   prompt  = original user message
 *   chosen  = user's correction
 *   rejected = agent's original response
 *
 * Writes directly to the `dpo_pairs` table in the same database.
 * `*pairs_created` reports how many new pairs were recorded.
 * Returns HU_OK even when no corrections were found (count = 0). */
hu_error_t hu_training_data_extract_dpo(hu_allocator_t *alloc, const char *memory_db_path,
                                        int correction_window_sec, size_t *pairs_created);

/* Default correction window: 300 seconds. If a user sends a follow-up
 * within this window after an assistant response, it is treated as a
 * potential correction for DPO pair generation. Widened from the
 * original 120s to capture corrections where the user pauses to read
 * or think before correcting. */
#define HU_DPO_CORRECTION_WINDOW_SEC 300

#ifdef HU_ENABLE_SQLITE
/* Testable inner of hu_training_data_extract_dpo. Operates on an
 * already-open `sqlite3*` rather than a path, which lets integration
 * tests pass an in-memory database (":memory:") and exercise the
 * production SQL end-to-end WITHOUT the HU_IS_TEST short-circuit the
 * path-based caller observes.
 *
 * Same semantics as the path-based variant:
 *   - Detects (user → assistant → user-within-window) triples
 *   - Inserts each into dpo_pairs with chosen=correction, rejected=assistant
 *   - Marks the assistant message_id in dpo_auto_extractions so a re-run
 *     is idempotent
 *   - Returns count via *pairs_created
 *
 * Does NOT short-circuit under HU_IS_TEST. The test owns the DB and
 * is responsible for any cleanup. ensure_dpo_table + ensure_extraction_tracking
 * are called internally so the test fixture doesn't need to know the
 * tracking-table schema.
 *
 * Returns HU_ERR_INVALID_ARGUMENT on NULL args, HU_ERR_IO on SQL
 * preparation failure (schema mismatch, etc.). */
hu_error_t hu_training_data_extract_dpo_from_db(sqlite3 *db, int correction_window_sec,
                                                size_t *pairs_created);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_TRAINING_DATA_EXTRACTOR_H */
