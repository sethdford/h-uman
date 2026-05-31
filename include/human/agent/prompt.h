#ifndef HU_AGENT_PROMPT_H
#define HU_AGENT_PROMPT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_persona; /* forward declaration; avoid circular deps */

/* ──────────────────────────────────────────────────────────────────────────
 * System prompt builder — identity, tools, memory, datetime, constraints
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_prompt_config {
    const char *provider_name;
    size_t provider_name_len;
    const char *model_name;
    size_t model_name_len;
    const char *workspace_dir;
    size_t workspace_dir_len;
    hu_tool_t *tools;
    size_t tools_count;
    const char *memory_context;
    size_t memory_context_len;
    const char *stm_context;
    size_t stm_context_len;
    const char *commitment_context;
    size_t commitment_context_len;
    const char *pattern_context;
    size_t pattern_context_len;
    const char *adaptive_persona_context;
    size_t adaptive_persona_context_len;
    const char *proactive_context;
    size_t proactive_context_len;
    const char *superhuman_context;
    size_t superhuman_context_len;
    uint8_t autonomy_level; /* 0=readonly, 1=supervised, 2=full */
    const char *custom_instructions;
    size_t custom_instructions_len;
    const char *persona_prompt; /* overrides default identity when set */
    size_t persona_prompt_len;
    const char *preferences; /* user preference rules */
    size_t preferences_len;
    bool chain_of_thought; /* inject reasoning instructions */
    const char *tone_hint; /* adaptive tone directive */
    size_t tone_hint_len;
    const char *awareness_context; /* situational awareness (channels, errors, health) */
    size_t awareness_context_len;
    const char *outcome_context; /* outcome tracker summary (tool success rates, corrections) */
    size_t outcome_context_len;
    bool persona_immersive;      /* suppress AI-assistant framing for deep persona mode */
    const char *contact_context; /* per-contact profile context (from persona contacts) */
    size_t contact_context_len;
    const char *conversation_context; /* conversation history + awareness (from channel history) */
    size_t conversation_context_len;
    uint32_t max_response_chars;      /* 0 = unlimited */
    const struct hu_persona *persona; /* persona struct for externalized prompt fields */
    const char *safety_rules;
    size_t safety_rules_len;
    const char *autonomy_rules;
    size_t autonomy_rules_len;
    const char *reasoning_instruction;
    size_t reasoning_instruction_len;
    const char
        *intelligence_context; /* AGI frontier context: goals, values, learning, self-improvement */
    size_t intelligence_context_len;
    const char *skills_context; /* available SkillForge skills for this agent */
    size_t skills_context_len;
    bool native_tools;             /* provider supports structured tool calls */
    bool hula_program_protocol;    /* teach <hula_program> JSON in system prompt */
    const char *emotional_context; /* from hu_emotional_cognition_build_prompt */
    size_t emotional_context_len;
    const char *cognition_mode; /* "fast", "slow", "emotional"; NULL = unset */
    size_t cognition_mode_len;
    const char *episodic_replay; /* cognitive replay from episodic patterns */
    size_t episodic_replay_len;
    const char *constitutional_principles; /* formatted principles for prompt injection */
    size_t constitutional_principles_len;
    const char *humanness_context; /* shared refs, curiosity, absence, opinions */
    size_t humanness_context_len;
    const char *imperfect_delivery; /* certainty/uncertainty framing directive */
    size_t imperfect_delivery_len;
    const char *residue_carryover; /* emotional carryover from prior conversations */
    size_t residue_carryover_len;
    const char *replay_context; /* replay learning insights from prior conversations */
    size_t replay_context_len;
    const char *contact_turing_hint; /* per-contact weak-dimension hints from Turing history */
    size_t contact_turing_hint_len;
    const char *instruction_context; /* discovered .human.md / HUMAN.md instructions */
    size_t instruction_context_len;
    const char *somatic_context;
    size_t somatic_context_len;
    const char *narrative_self_context;
    size_t narrative_self_context_len;
    const char *presence_context;
    size_t presence_context_len;
    const char *micro_expression_context;
    size_t micro_expression_context_len;
    const char *creative_voice_context;
    size_t creative_voice_context_len;
    const char *novelty_context;
    size_t novelty_context_len;
    const char *attachment_context;
    size_t attachment_context_len;
    const char *rupture_context;
    size_t rupture_context_len;
    const char *growth_context;
    size_t growth_context_len;
    const char *boundary_context;
    size_t boundary_context_len;
    const char *relational_episode_context;
    size_t relational_episode_context_len;
    const char *trust_context;
    size_t trust_context_len;
    const char *humor_directive;
    size_t humor_directive_len;
    const char *sycophancy_friction;
    size_t sycophancy_friction_len;
    const char *conv_goals_context;
    size_t conv_goals_context_len;
    /* Personal model summary (identity, facts, topics, goals, learned style).
     * Built per-turn from the agent's accumulating hu_personal_model_t.
     * NULL/empty when the model has no signal yet — see
     * hu_personal_model_has_content() in human/memory/personal_model.h. */
    const char *personal_model_context;
    size_t personal_model_context_len;
    /* Moment-context decision-layer fragment — rendered from hu_moment_t
     * via hu_moment_render_prompt. ~256 chars max. Tells the model what
     * time/style/rhythm signals the current turn is operating under.
     * NULL/empty when compose returned an empty moment (new contact,
     * no signals). See include/human/moment.h. */
    const char *moment_context;
    size_t moment_context_len;
    /* Self-exemplars — up to N verbatim outbound messages we've sent to
     * THIS contact, formatted as in-context style anchors. Rendered via
     * hu_moment_render_self_exemplars. NULL/empty when no outbound
     * history exists for the contact. */
    const char *self_exemplars_context;
    size_t self_exemplars_context_len;
    /* W9 world-model snapshot (FIX 12): goals, negatives, theory-of-mind,
     * recent topics for the active contact. Rendered per-turn via the W7
     * bridge (src/agent/world_model_bridge.c). NULL when the load returns
     * an empty model (callers skip injection). */
    const char *world_model_context;
    size_t world_model_context_len;
    /* Sprint 6 US-14: Voice maturity stage directive.
     * A concise [VOICE STAGE: ...] tag emitted by hu_voice_maturity_build_directive.
     * NULL/empty when voice profile is uninitialised or persona is disabled. */
    const char *voice_maturity_directive;
    size_t voice_maturity_directive_len;
    /* GraphRAG: per-contact community summaries for relationship context */
    const char *graph_context;
    size_t graph_context_len;
    /* Phase 2 prompt-budget trim — populated from hu_config_t.prompt_budget
     * by the caller (agent_turn.c / agent_stream.c). When trim_enabled is
     * true AND the builder is invoked with a non-NULL budget pointer that
     * has tagged a field DEAD, that field's appender block is skipped.
     * Defaults (zero values) leave behavior unchanged from Phase 1b. */
    bool prompt_budget_trim_enabled;
    int prompt_budget_dead_field_min_bytes;   /* default 16 if 0 */
    int prompt_budget_min_samples_before_tag; /* default 100 if 0 */
    const char **
        prompt_budget_field_allowlist; /* field names to keep even if DEAD (borrowed from config) */
    size_t prompt_budget_field_allowlist_count; /* number of allowlisted fields */
    /* 2026-05 audit follow-up — suppress the "trim gate disabled" one-shot
     * diagnostic when invoked from the static-cache path (hu_prompt_build_
     * static). The static cache is built at agent_from_config time before
     * any turn observations exist, so the warning fires misleadingly there
     * even when the operator's config has the trim enabled. Per-turn paths
     * leave this false and the diagnostic still fires correctly. */
    bool suppress_prompt_budget_diagnostic;
    /* Calibrated-uncertainty (Task 3): when true, hu_prompt_build_system
     * appends the verbalized confidence-tagging addendum ([conf=0.X]) so the
     * model self-reports confidence on factual claims. Defaults false; the
     * caller (agent_turn.c) sets it from query classification. Leaving it
     * false suppresses the addendum on casual/non-factual turns. */
    bool is_factual_query;
} hu_prompt_config_t;

/* Build the full system prompt. Caller owns returned string; free with alloc.
 *
 * `stats` is an OPTIONAL per-field byte-accounting array (NULL = legacy
 * zero-overhead path). When non-NULL, it MUST point to an array of at
 * least HU_PROMPT_FIELD_COUNT entries; the builder populates each
 * entry's `name` (static string) and `bytes_contributed` (delta this
 * field added to the prompt). Empty fields report 0 bytes — operators
 * see WHICH slots are unwired, not just which are dead. See
 * include/human/agent/prompt_budget.h for the field enum + accumulator.
 *
 * `budget` is an OPTIONAL accumulated dead-field history (NULL = no trim).
 * When non-NULL AND `config->prompt_budget.enabled` is true, the builder
 * checks each wrapped field via hu_prompt_budget_field_is_dead and SKIPS
 * the appender block for fields that have been observed dead. Mean bytes
 * < cfg->prompt_budget.dead_field_min_bytes after >= cfg->prompt_budget.
 * min_samples_before_tag observations is the dead criterion. Skipped
 * fields contribute 0 bytes to the prompt AND to the stats array. */
struct hu_prompt_field_stat; /* forward decl — full def in prompt_budget.h */
struct hu_prompt_budget;     /* forward decl — opaque from prompt_budget.h */
hu_error_t hu_prompt_build_system(hu_allocator_t *alloc, const hu_prompt_config_t *config,
                                  struct hu_prompt_field_stat *stats,
                                  const struct hu_prompt_budget *budget, char **out,
                                  size_t *out_len);

/* Build only the static parts (identity, tools, autonomy, safety, custom).
 * The result can be cached and reused across turns. Caller owns returned string. */
hu_error_t hu_prompt_build_static(hu_allocator_t *alloc, const hu_prompt_config_t *config,
                                  char **out, size_t *out_len);

/* Build full prompt by combining cached static part with dynamic memory context.
 * Avoids rebuilding the static portion each turn. Caller owns returned string. */
hu_error_t hu_prompt_build_with_cache(hu_allocator_t *alloc, const char *static_prompt,
                                      size_t static_prompt_len, const char *memory_context,
                                      size_t memory_context_len, char **out, size_t *out_len);

/* Tone detection — analyze recent user messages to detect communication style.
 * Returns a static string hint suitable for tone_hint field. */
typedef enum hu_tone {
    HU_TONE_NEUTRAL,
    HU_TONE_CASUAL,
    HU_TONE_TECHNICAL,
    HU_TONE_FORMAL,
} hu_tone_t;

hu_tone_t hu_detect_tone(const char *const *user_messages, const size_t *message_lens,
                         size_t count);

const char *hu_tone_hint_string(hu_tone_t tone, size_t *out_len);

#endif /* HU_AGENT_PROMPT_H */
