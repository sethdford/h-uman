#ifndef HU_M3_REWRITE_CAPTURE_H
#define HU_M3_REWRITE_CAPTURE_H

/* Phase D7 (2026-05-18) — REWRITE outcome capture for DPO training.
 *
 * When the response_guard chain detects a problem with the model's
 * raw output and rewrites it, the moment of rewrite is a PERFECT
 * preference pair: the original (rejected) response is bad, the
 * rewritten (accepted) response is what we want. DPO/IPO/KTO train
 * on exactly this shape.
 *
 * The outcome ring (m3_frontier_adapter.h) captures hashes only and
 * keeps the C-side struct privacy-preserving. For DPO we need actual
 * text — both the prompt AND both versions of the response — so this
 * module writes a SEPARATE JSONL file:
 *
 *   ~/.human/training-data/m3-rewrite-pairs.jsonl
 *
 * Each line is one preference pair:
 *   {
 *     "t":  unix_ms,
 *     "ph": prompt_hash,
 *     "prompt": "<user message>",
 *     "rejected": "<provider's raw response>",
 *     "accepted": "<guard-rewritten response>",
 *     "k": turn_kind (1=stream, 2=batch, 3=proactive)
 *   }
 *
 * Privacy stance:
 *   - This file lives under the same directory as m3-outcomes.jsonl
 *     and inherits its access controls (user-only by default).
 *   - The prompt + responses are stored as-is — no PII redaction at
 *     this layer. The training-side pipeline applies the same
 *     PII filters as lora-persona before any tensor pass.
 *   - The "ring stays hashes-only" invariant is preserved — this
 *     side-file is the ONE place raw content lives by design.
 *
 * The file is rotated by training_loop.py when it grows past 16 MB
 * (same pattern as the outcomes JSONL rotation in D6).
 */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Append a single REWRITE preference pair to the side-file.
 *
 * Best-effort: failures (allocator, file I/O, NULL inputs) are
 * non-fatal — the chat path MUST continue functioning even if the
 * pair file is unwriteable. The caller treats the return value as
 * informational, not actionable.
 *
 * `path` may be NULL or empty — in that case the default
 * ~/.human/training-data/m3-rewrite-pairs.jsonl is used. Passing
 * a path lets tests redirect to a fixture file.
 *
 * Thread-safety: the agent's chat path is single-threaded within a
 * turn. Multiple concurrent turns can call this; the underlying
 * append is `fopen("a")` which on POSIX is atomic for writes
 * smaller than PIPE_BUF (4096 bytes). Records over that size could
 * interleave — we mitigate by truncating prompt + rejected +
 * accepted to bounded lengths below. */
#ifdef HU_ENABLE_ML
hu_error_t hu_m3_rewrite_pair_record(hu_allocator_t *alloc, const char *path, const char *prompt,
                                     size_t prompt_len, const char *rejected, size_t rejected_len,
                                     const char *accepted, size_t accepted_len,
                                     unsigned char turn_kind);
#else
/* HU_ENABLE_ML=OFF stub. The pair-record file is M3 training data; without
 * the ML subsystem there is nothing to train, so the record is a no-op.
 * Defined inline so call sites in agent_turn.c / agent_stream.c link cleanly
 * in non-ML builds (per ~/.claude/rules/test-source-gate-symmetry.md). */
static inline hu_error_t hu_m3_rewrite_pair_record(hu_allocator_t *alloc, const char *path,
                                                   const char *prompt, size_t prompt_len,
                                                   const char *rejected, size_t rejected_len,
                                                   const char *accepted, size_t accepted_len,
                                                   unsigned char turn_kind) {
    (void)alloc;
    (void)path;
    (void)prompt;
    (void)prompt_len;
    (void)rejected;
    (void)rejected_len;
    (void)accepted;
    (void)accepted_len;
    (void)turn_kind;
    return HU_OK;
}
#endif

/* Max bytes per field in the JSONL record. Records exceeding the
 * cap are TRUNCATED, not rejected — DPO training prefers a truncated
 * sample to no sample. The cap keeps a single record under PIPE_BUF
 * for write atomicity. */
#define HU_M3_REWRITE_PAIR_MAX_FIELD_BYTES 1024

#ifdef __cplusplus
}
#endif

#endif /* HU_M3_REWRITE_CAPTURE_H */
