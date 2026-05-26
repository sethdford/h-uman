/* response_guard_dpo.h — capture response_guard REJECTs as DPO negative
 * pairs for the next LoRA training run.
 *
 * Sprint 41 follow-up (2026-05-26): the Jordan "tbh morning" incident was
 * caught by the new G9 detector, but the underlying LoRA adapter STILL
 * wants to produce that text — we're now just refusing to send it. The
 * proper fix is to retrain the adapter with these rejections as DPO
 * negative pairs so the model learns not to emit them. This module
 * provides the capture half; the consume half lives in the existing
 * scripts/lora_*  pipeline that already reads
 * ~/.human/training-data/m3-*.jsonl.
 *
 * Schema (matches the existing m3-combined-dpo-*.jsonl shape):
 *
 *   {"prompt":"<what user said>","chosen":null,
 *    "rejected":"<what response_guard refused>",
 *    "_source":"response_guard_g9",
 *    "_detector":"naked_discourse_opener",
 *    "_channel":"imessage","_ts_unix":1779800000}
 *
 * `chosen` is null at capture time — a downstream pairing step (or the
 * DPO trainer itself) can opportunistically attach the successful retry
 * reply when one exists. Capturing the rejection alone is the lossless,
 * non-coupling shape: even rejections without retries are training
 * signal.
 *
 * Two layers:
 *   - hu_response_guard_format_dpo_negative_jsonl: pure formatter,
 *     trivially unit-testable, no I/O. Returns bytes written.
 *   - hu_response_guard_log_dpo_negative: I/O wrapper that appends a
 *     newline-terminated JSONL line to
 *     ~/.human/training-data/m3-dpo-rejections.jsonl. NO-OP under
 *     HU_IS_TEST so tests don't write to disk. Returns HU_OK on
 *     successful append, HU_ERR_* on file error. */
#ifndef HU_AGENT_RESPONSE_GUARD_DPO_H
#define HU_AGENT_RESPONSE_GUARD_DPO_H

#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure formatter — writes a single newline-terminated JSONL object into
 * the caller's buffer. Returns the number of bytes written (excluding
 * the final NUL). On out_cap=0 returns 0. On overflow, the object is
 * truncated and still NUL-terminated; caller can detect truncation by
 * comparing the return value against out_cap-1.
 *
 * All string inputs are JSON-escaped (quotes, backslashes, control
 * chars). NULL strings are emitted as JSON null. Empty strings are
 * emitted as "".
 *
 * Field order is stable: prompt, chosen, rejected, _source, _detector,
 * _channel, _ts_unix. */
size_t hu_response_guard_format_dpo_negative_jsonl(const char *prompt, size_t prompt_len,
                                                   const char *rejected, size_t rejected_len,
                                                   const char *detector, const char *channel,
                                                   int64_t ts_unix, char *out, size_t out_cap);

/* I/O wrapper — formats the JSONL line and appends it (with a final \n)
 * to ~/.human/training-data/m3-dpo-rejections.jsonl, creating the file
 * if it doesn't exist. NO-OP returning HU_OK under HU_IS_TEST so tests
 * don't write to disk. Returns HU_ERR_IO on file errors. */
hu_error_t hu_response_guard_log_dpo_negative(const char *prompt, size_t prompt_len,
                                              const char *rejected, size_t rejected_len,
                                              const char *detector, const char *channel,
                                              int64_t ts_unix);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_RESPONSE_GUARD_DPO_H */
