/* include/human/util/llm_json.h — locate the JSON payload inside an LLM reply.
 *
 * Structured-output call sites (reflection, fact extraction, planners) ask
 * the model for bare JSON and then hand the raw reply to hu_json_parse. Real
 * replies are rarely bare: reasoning models prepend <think>…</think> blocks,
 * chat models wrap the object in ```json fences, and both add a sentence of
 * prose on either side. Every caller that re-derived its own locator got a
 * different subset wrong (2026-09-04 audit: 16,056 reflection runs rejected
 * as schema_invalid, one accepted). This is the one locator they share.
 *
 * Contract:
 *   - Skips everything up to and including the LAST closing reasoning tag
 *     (</think> or </thought>, case-insensitive). An opening tag with no
 *     close means the reply was truncated mid-thought → not found.
 *   - Returns the first balanced {…} or […] slice after that point. Bracket
 *     matching is string-aware (quotes and backslash escapes inside JSON
 *     strings do not count). Fences and prose fall out naturally because the
 *     slice starts at the first bracket and ends at its match.
 *   - Never allocates; *out points into `s`.
 *   - Returns false on NULL/empty input, no bracket, or an unbalanced
 *     (truncated) payload. The caller decides what a false means (soft-fail,
 *     retry, log). The returned slice is a CANDIDATE — hu_json_parse still
 *     validates it. */
#ifndef HUMAN_UTIL_LLM_JSON_H
#define HUMAN_UTIL_LLM_JSON_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool hu_llm_json_locate(const char *s, size_t len, const char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_UTIL_LLM_JSON_H */
