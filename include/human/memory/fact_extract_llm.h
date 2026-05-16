#ifndef HU_FACT_EXTRACT_LLM_H
#define HU_FACT_EXTRACT_LLM_H

/*
 * fact_extract_llm — LLM-based personal-fact extractor.
 *
 * Complements (does NOT replace) the regex-based `hu_fact_extract` in
 * fact_extract.h. The regex extractor matches 43 first-person prefixes
 * ("i like ", "i live in ", …) and is the per-turn fast-path —
 * deterministic, allocator-free, microsecond cost. Its recall on
 * natural English is ~15-25% (audit 2026-05-16): "Rock climbing is my
 * passion" → no match; "I really enjoy rock climbing" → no match.
 *
 * This extractor calls a frontier LLM with a focused prompt and parses
 * the JSON response into the same `hu_fact_extract_result_t` struct.
 * Intended cadence: every N turns (where N is set by the caller's
 * scheduler), not per-turn — the provider call is expensive relative
 * to the regex pass. The two extractors compose naturally:
 *
 *   per-turn:     hu_fact_extract(text, ..., &batch)
 *                 hu_personal_model_merge_facts(&model, &batch)
 *   every N turns: hu_fact_extract_llm(text_buffer, ..., &batch)
 *                 hu_personal_model_merge_facts(&model, &batch)
 *
 * The LLM-extracted batch will frequently overlap the regex-extracted
 * batch — that's fine: `hu_fact_dedup` collapses duplicates by
 * subject+predicate before merge.
 *
 * Scope today (2026-05-16):
 *   - Header + implementation + recording-stub test. Calls provider's
 *     chat_with_system with a focused JSON-output prompt and parses
 *     the response.
 *   - NOT YET wired into the agent turn loop. The integration decision
 *     (cadence, which provider, off-band scheduling) is out of scope
 *     for this skeleton — caller drives.
 *   - NOT YET measured against natural-English benchmark corpus. The
 *     recall improvement claim is theoretical; calibrate with an eval
 *     suite once a corpus exists.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/fact_extract.h"
#include "human/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Extract facts from `text` by issuing one provider call.
 *
 * `provider`      — non-NULL; must implement chat_with_system
 * `model`         — model identifier passed to the provider; "" is allowed
 *                    (provider-dependent default)
 * `text`/`text_len` — the message to extract from
 * `now_ts`        — Unix seconds, stamped onto each extracted fact's
 *                    last_seen_at + provenance
 * `result`        — populated on HU_OK; max HU_FACT_EXTRACT_MAX facts
 *
 * Provider's response is expected to be a JSON object of the shape:
 *   {"facts": [
 *     {"subject":"user","predicate":"likes","object":"climbing","confidence":0.85},
 *     ...
 *   ]}
 * or a bare array of fact objects. Both shapes are accepted.
 *
 * Soft errors (malformed JSON, no facts in response, response truncated)
 * return HU_OK with `result->fact_count == 0` — the caller should
 * treat that as "LLM declined" rather than a programmer error, since
 * provider behavior varies and the regex fast-path is the fallback.
 *
 * Hard errors (NULL args, provider returned non-OK, OOM during parse)
 * propagate as their respective error codes.
 *
 * Returns HU_OK on success or soft failure; HU_ERR_INVALID_ARGUMENT,
 * HU_ERR_PROVIDER_RESPONSE, or HU_ERR_OUT_OF_MEMORY otherwise. */
hu_error_t hu_fact_extract_llm(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                               size_t model_len, const char *text, size_t text_len, int64_t now_ts,
                               hu_fact_extract_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* HU_FACT_EXTRACT_LLM_H */
