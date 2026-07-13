#ifndef HU_AGENT_PROMPT_TRIM_H
#define HU_AGENT_PROMPT_TRIM_H

#include <stddef.h>

/* Value-aware system-prompt trim — replaces the positional 16 KB tail
 * truncation on the immersive persona path.
 *
 * The positional cap (agent_turn.c) cuts whatever was assembled LAST —
 * which on the immersive path is the anti-AI-tell guard (texting shape
 * rules, CRITICAL REMINDER, persona reinforcement), deliberately placed
 * last for recency salience. This module trims MIDDLE sections instead
 * (self-exemplars, GraphRAG grounding, memory context — in that priority
 * order), protecting the persona head and the guard tail.
 *
 * Gated by HU_PROMPT_TRIM per feature-gate-requires-measurement.md:
 *   off (default) — no behavior change; the positional cut stays in charge
 *   shadow        — compute + log the plan; emitted prompt unchanged
 *   live          — apply the plan; positional cut remains the safety net
 *
 * See docs/research/2026-07-11-prompt-composition-shrink-plan.md. */

/* System-prompt hard cap in bytes. Mirrors the MLX-safe positional cap
 * enforced in agent_turn.c (empty responses observed above ~28 KB;
 * 16 KB keeps a wide margin — 2026-05-19 finding). */
#define HU_PROMPT_TRIM_BUDGET_BYTES 16384

typedef enum hu_prompt_trim_mode {
    HU_PROMPT_TRIM_OFF = 0,
    HU_PROMPT_TRIM_SHADOW,
    HU_PROMPT_TRIM_LIVE,
} hu_prompt_trim_mode_t;

/* Pure parse of an HU_PROMPT_TRIM value. NULL, empty, "off", and any
 * unrecognized value map to OFF (fail-closed: unknown input must not
 * activate new behavior). "shadow" -> SHADOW; "live", "on", "1" -> LIVE. */
hu_prompt_trim_mode_t hu_prompt_trim_mode_parse(const char *value);

/* getenv("HU_PROMPT_TRIM") -> hu_prompt_trim_mode_parse. */
hu_prompt_trim_mode_t hu_prompt_trim_mode(void);

/* One trimmable middle-section span in the assembled prompt buffer.
 * A zero-length span marks an absent section and is skipped. Spans must
 * be disjoint. The ORDER of the spans array is the trim priority:
 * index 0 is trimmed first. */
typedef struct hu_prompt_trim_span {
    size_t offset; /* byte offset of the section start in the buffer */
    size_t length; /* section length in bytes, including its trailing
                    * separator; 0 = section absent */
} hu_prompt_trim_span_t;

/* Pure planner. When len > budget, decides how many bytes to cut from
 * the HEAD of each span (head-first = oldest-first for the memory
 * section, whose facts are appended in retrieval order), walking spans
 * in priority order until the overage is covered or every span is
 * exhausted. Partial cuts are extended forward to the next newline so
 * the surviving section content starts at a line boundary.
 *
 * cuts_out must hold span_count entries; entry i is the byte count to
 * remove from spans[i].offset. Returns the total planned cut. Returns 0
 * (all cuts zero) when len <= budget or on NULL/degenerate input. */
size_t hu_prompt_trim_plan(const char *buf, size_t len, size_t budget,
                           const hu_prompt_trim_span_t *spans, size_t span_count,
                           size_t *cuts_out);

/* Applies a plan from hu_prompt_trim_plan: removes [offset, offset+cut)
 * for every span with a non-zero cut, compacts the buffer, and returns
 * the new length. The buffer is NUL-terminated at the new length.
 * Out-of-range spans/cuts are clamped defensively. */
size_t hu_prompt_trim_apply(char *buf, size_t len, const hu_prompt_trim_span_t *spans,
                            size_t span_count, const size_t *cuts);

/* Positional tail-cap cut point — the pre-trim safety net shared by BOTH
 * turn paths (agent_turn.c and agent_stream.c previously carried identical
 * inline copies; 2026-07-12 review). Returns len when len <= budget.
 * Otherwise returns the cut point at the last newline within the budget,
 * falling back to the hard budget when no newline lies in its upper half
 * (a clean cut that far back would delete too much). Pure: no logging,
 * no mutation — callers NUL-terminate and log. */
size_t hu_prompt_positional_cap_point(const char *buf, size_t len, size_t budget);

#endif /* HU_AGENT_PROMPT_TRIM_H */
