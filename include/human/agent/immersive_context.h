#ifndef HU_AGENT_IMMERSIVE_CONTEXT_H
#define HU_AGENT_IMMERSIVE_CONTEXT_H

/* HU_IMMERSIVE_CONTEXT — a budgeted "## What you know right now" block for
 * the immersive (production) prompt.
 *
 * The immersive branch of hu_prompt_build_system returns before ~31 rendered
 * context fields are appended, so production never sends commitments,
 * episodic replay, emotional state, presence, proactive/superhuman insight,
 * conversation goals or residue. This composer picks the most useful of them
 * under a hard byte budget, in priority order:
 *   1. commitments / open loops            (commitment_context)
 *   2. episodic replay, one item           (episodic_replay)
 *   3. emotional state, presence           (emotional_context, presence_context)
 *   4. proactive + superhuman, one each    (proactive_context, superhuman_context)
 *   5. conversation goals, residue         (conv_goals_context, residue_carryover)
 * Each field is the existing builder's rendered output; the composer splits it
 * into items (lines), drops the builder's own headings and placeholders, and
 * keeps or drops WHOLE items — never cutting one mid-sentence.
 *
 * Gate (default OFF): OFF does no work and the prompt is byte-identical;
 * SHADOW composes and logs one aggregate line; LIVE appends the block. The
 * block is private (owner memory text): the reliable/degradation fallbacks
 * strip it before any non-primary attempt — see
 * include/human/providers/private_context.h. */

#include "human/agent/prompt.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>

#define HU_IMMERSIVE_CONTEXT_BUDGET_BYTES 1536

/* Bits of hu_immersive_context_stats_t.field_mask, in priority order. */
enum {
    HU_IMMERSIVE_CTX_COMMITMENT = 1u << 0,
    HU_IMMERSIVE_CTX_EPISODIC = 1u << 1,
    HU_IMMERSIVE_CTX_EMOTIONAL = 1u << 2,
    HU_IMMERSIVE_CTX_PRESENCE = 1u << 3,
    HU_IMMERSIVE_CTX_PROACTIVE = 1u << 4,
    HU_IMMERSIVE_CTX_SUPERHUMAN = 1u << 5,
    HU_IMMERSIVE_CTX_CONV_GOALS = 1u << 6,
    HU_IMMERSIVE_CTX_RESIDUE = 1u << 7,
};

typedef struct hu_immersive_context_stats {
    size_t fields_used;  /* source fields that contributed at least one item */
    unsigned field_mask; /* HU_IMMERSIVE_CTX_* of those fields */
    size_t items_used;
    size_t bytes;   /* length of the composed block, 0 when empty */
    bool truncated; /* at least one item dropped for the byte budget */
} hu_immersive_context_stats_t;

/* Pure composer. *out is NULL (and stats zero) when no field has an item. The
 * block starts with HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT, has no blank line in
 * its body and ends with "\n\n", so it is strippable byte-exactly. budget is
 * the hard ceiling on *out_len. */
hu_error_t hu_immersive_context_compose(hu_allocator_t *alloc, const hu_prompt_config_t *cfg,
                                        size_t budget, char **out, size_t *out_len,
                                        hu_immersive_context_stats_t *stats);

/* getenv("HU_IMMERSIVE_CONTEXT"), default OFF. */
hu_gate_mode_t hu_immersive_context_mode(void);

/* The prompt-builder hook: OFF → nothing (no work); SHADOW → compose, log
 * "[HU_IMMERSIVE_CONTEXT shadow] fields_used=.. bytes=.. truncated=..", return
 * nothing; LIVE → compose, log the same line tagged live, return the block in
 * *out (caller frees out_len + 1). */
hu_error_t hu_immersive_context_for_prompt(hu_allocator_t *alloc, const hu_prompt_config_t *cfg,
                                           char **out, size_t *out_len);

#endif /* HU_AGENT_IMMERSIVE_CONTEXT_H */
