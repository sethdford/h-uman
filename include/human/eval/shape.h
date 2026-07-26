#ifndef HU_EVAL_SHAPE_H
#define HU_EVAL_SHAPE_H

/* 2026-05-18 (M4): C-side deterministic shape classifier for eval responses.
 *
 * Mirrors scripts/eval_shape_classifier.py but in C, so every
 * hu_eval_run_suite call automatically scores each response on shape
 * (length + markdown + AI-assistant openers) without needing the
 * Python tool. Results persist alongside score/passed in the
 * eval_results SQLite table.
 *
 * Rationale: the LLM-judge has false positives AND false negatives
 * (proven in the persona-eval audit chain). The shape classifier is
 * a deterministic regex/string-matching gate that detects the
 * canonical "AI assistant offering options" failure mode. It
 * correlates with the LLM-judge on unambiguous responses and provides
 * signal where the judge gives noise.
 *
 * For iMessage / Telegram (strict): no markdown, no AI openers, <=250 char.
 * For Discord / Slack (relaxed): markdown OK, no AI openers, <=500-800 char.
 * For Email (formal): markdown + openers OK, <=2000 char.
 */

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_shape_channel {
    HU_SHAPE_CHANNEL_IMESSAGE = 0, /* default — strictest */
    HU_SHAPE_CHANNEL_TELEGRAM,
    HU_SHAPE_CHANNEL_DISCORD, /* markdown allowed */
    HU_SHAPE_CHANNEL_SLACK,   /* markdown allowed */
    HU_SHAPE_CHANNEL_EMAIL,   /* markdown + openers allowed */
} hu_shape_channel_t;

typedef struct hu_shape_result {
    double score; /* in [0.0, 1.0], 1.0 = perfect in-voice */
    bool passed;  /* score >= 0.7 AND no fatal violation */
    size_t response_len;
    /* Bit flags for per-fail attribution (debug / explainability) */
    uint32_t fail_flags;
} hu_shape_result_t;

/* Per-fail bit flags. Use these to attribute which rules a response
 * violated when displaying results to humans. */
#define HU_SHAPE_FAIL_NULL_RESPONSE   0x0001
#define HU_SHAPE_FAIL_EMPTY_RESPONSE  0x0002
#define HU_SHAPE_FAIL_TOO_LONG        0x0004
#define HU_SHAPE_FAIL_WAY_TOO_LONG    0x0008
#define HU_SHAPE_FAIL_BULLET_LIST     0x0010
#define HU_SHAPE_FAIL_NUMBERED_LIST   0x0020
#define HU_SHAPE_FAIL_HEADER          0x0040
#define HU_SHAPE_FAIL_BOLD_MARKDOWN   0x0080
#define HU_SHAPE_FAIL_CODE_FENCE      0x0100
#define HU_SHAPE_FAIL_DEPENDING_ON    0x0200
#define HU_SHAPE_FAIL_HERE_ARE        0x0400
#define HU_SHAPE_FAIL_CERTAINLY       0x0800
#define HU_SHAPE_FAIL_ABSOLUTELY      0x1000
#define HU_SHAPE_FAIL_GREAT_QUESTION  0x2000
#define HU_SHAPE_FAIL_I_UNDERSTAND    0x4000
#define HU_SHAPE_FAIL_EXCESSIVE_EMOJI 0x8000 /* M5: seth.json says ZERO emoji on most msgs */
/* 2026-05-29: persona-break — the reply self-identifies as an AI or disclaims
 * capabilities ("as an AI", "I don't have access to", "as a language model").
 * A hard tell that the model dropped the Seth persona; fatal on every channel
 * except email (which allows AI-assistant register). Surfaced by the humanness
 * north-star nightly, where such replies wrongly scored anti_ai 1.0. */
#define HU_SHAPE_FAIL_AI_SELF_DISCLOSURE 0x10000

/* Reply ends in '?' — the assistant reflex of closing every turn with a
 * follow-up question.
 *
 * MEASUREMENT-ONLY. Deliberately NOT in HU_PERSONA_SHAPE_AI_OPENER_MASK
 * (src/agent/outbound/persona.c) and carries NO score penalty, so it cannot
 * gate or regenerate a send. Per .claude/rules/feature-gate-requires-measurement.md
 * a behaviour change ships OFF -> SHADOW -> LIVE on a measurement; this is the
 * SHADOW instrument.
 *
 * Why an instrument rather than a prompt rule: measured 2026-07-26 over 689 real
 * matched reply pairs, Seth ends a reply with '?' just 7.7% of the time, while
 * the model ran 42-70%. FOUR prompt layers already forbid it — hu_rules_casual
 * ("Question marks only when actually asking"), style_rules[3] ("about 1 in 12
 * texts"), core.communication_rules[6], and an anti_patterns entry added and then
 * REVERTED because it moved the rate the wrong way (55% -> 70%). The reflex is
 * prompt-resistant, so closing the gap needs a deterministic mechanism whose
 * effect can be measured before it touches sends. */
#define HU_SHAPE_FAIL_TRAILING_QUESTION 0x20000

/* M9: compile-time guard against bit-flag exhaustion. If the highest
 * fail flag exceeds 31 bits we'd silently corrupt masking in shape.c.
 * Update this bound when adding a flag. */
_Static_assert(HU_SHAPE_FAIL_AI_SELF_DISCLOSURE < (1u << 31),
               "HU_SHAPE_FAIL_* exhausted 31-bit range — widen shape_fails or split");

/* Map a channel name string (case-insensitive) to the enum. Falls
 * back to HU_SHAPE_CHANNEL_IMESSAGE (strictest) for unknown channels. */
hu_shape_channel_t hu_shape_channel_from_string(const char *channel, size_t channel_len);

/* Classify a response under the channel's shape rules. response may
 * be NULL (recorded as NULL_RESPONSE fail with score 0.0).
 * Never returns an error — populates result and returns HU_OK.
 *
 * Task 11 (AC-10) — optional learned_length_cap parameter:
 *   - If 0 (or NULL for the _ex variant): uses universal channel cap only.
 *   - If >0: for "close" relationship contacts, allows responses up to the
 *     learned cap (overriding the universal cap). Structural fails (markdown,
 *     AI openers) are unchanged.
 *
 * Safe to call from any context; no allocation, no I/O. */
hu_error_t hu_shape_classify(const char *response, size_t response_len, hu_shape_channel_t channel,
                             hu_shape_result_t *out);

/* Extended variant: pass relationship_stage and learned_length_cap for per-
 * contact shape rules. relationship_stage="close" + learned_length_cap>0 allows
 * exceeding the universal channel cap up to the learned cap. Pass NULL
 * relationship_stage or 0 learned_length_cap to use universal caps only. */
hu_error_t hu_shape_classify_ex(const char *response, size_t response_len,
                                hu_shape_channel_t channel, const char *relationship_stage,
                                size_t relationship_stage_len, size_t learned_length_cap,
                                hu_shape_result_t *out);

#endif /* HU_EVAL_SHAPE_H */
