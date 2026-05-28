#ifndef HU_PERSONA_H
#define HU_PERSONA_H

#include "human/core/allocator.h"

#define HU_PERSONA_PROMPT_MAX_BYTES (24 * 1024) /* 24 KB cap for research-rich personas */

#include "human/agent/output_validator_chain.h"
#include "human/core/error.h"
#include "human/persona/circadian.h"
#include "human/persona/relationship.h"

#include <stdbool.h>

struct hu_json_value;
#include <stddef.h>
#include <stdint.h>

typedef struct hu_persona_overlay {
    char *channel;
    char *formality;
    char *avg_length;
    char *emoji_usage;
    char **style_notes;
    size_t style_notes_count;
    char **filler_bank; /* per-channel thinking-filler strings */
    size_t filler_bank_count;
    size_t filler_bank_cap; /* allocated capacity; soft cap 32 enforced at add */
    bool message_splitting;
    uint32_t max_segment_chars;
    char **typing_quirks;
    size_t typing_quirks_count;
    /* B15: explicit user-stated pragmatics only (never inferred). */
    char *directness;
    char *face_saving;
    char *disagreement_style;
    char *silence_tolerance;
    char *vulnerability_tier;
    float affect_mirror_ceiling; /* 0.0-1.0; caps emotional intensity mirroring. 0 = use default */
    uint8_t leave_on_read_pct;   /* 0-100; probability of leave-on-read. 0 = use default (10%) */
    /* Phase 1 uncertainty: per-confidence-level hedge phrase banks */
    char **hedge_phrases[4]; /* [HIGH, MEDIUM, LOW, VERY_LOW] */
    size_t hedge_phrase_counts[4];
} hu_persona_overlay_t;

typedef struct hu_persona_example {
    char *context;
    char *incoming;
    char *response;
} hu_persona_example_t;

typedef struct hu_persona_example_bank {
    char *channel;
    hu_persona_example_t *examples;
    size_t examples_count;
} hu_persona_example_bank_t;

typedef struct hu_contact_profile {
    char *contact_id;
    char *name;
    char *email;
    char *relationship;
    char *relationship_stage;
    char *relationship_type; /* "family", "friend", "coworker", "acquaintance", or NULL */
    char *warmth_level;
    char *vulnerability_level;
    char *identity;
    char *context;
    char *dynamic;
    char *greeting_style;
    char *closing_style;
    char **interests;
    size_t interests_count;
    char **recent_topics;
    size_t recent_topics_count;
    char **sensitive_topics;
    size_t sensitive_topics_count;
    char **allowed_behaviors;
    size_t allowed_behaviors_count;
    bool texts_in_bursts;
    bool prefers_short_texts;
    bool sends_links_often;
    bool uses_emoji;
    bool proactive_checkin;
    char *proactive_channel;
    char *proactive_schedule;
    char *attachment_style;
    char *dunbar_layer;
    float affect_mirror_ceiling; /* per-contact ceiling override. 0 = use stage default */
    uint8_t leave_on_read_pct;   /* per-contact override (0-100). 0 = use overlay/default */
} hu_contact_profile_t;

/* Motivation — the character's core drive (anti-drift anchor) */
typedef struct hu_persona_motivation {
    char *primary_drive;
    char *protecting;
    char *avoiding;
    char *wanting;
} hu_persona_motivation_t;

/* Situational direction — trigger → behavior pairs (director's scene notes) */
typedef struct hu_situational_direction {
    char *trigger;
    char *instruction;
} hu_situational_direction_t;

/* Phase 6 — daily routine block (time, activity, availability, mood modifier) */
typedef struct hu_routine_block {
    char time[8]; /* "05:30" */
    char activity[64];
    char availability[16]; /* "brief","unavailable","slow","available" */
    char mood_modifier[32];
} hu_routine_block_t;

/* Phase 6 — daily routine (weekday/weekend blocks, variance) */
typedef struct hu_daily_routine {
    hu_routine_block_t weekday[24];
    size_t weekday_count;
    hu_routine_block_t weekend[24];
    size_t weekend_count;
    float routine_variance; /* default 0.15 */
} hu_daily_routine_t;

/* Phase 6 — life chapter (theme, mood, key threads) */
typedef struct hu_life_chapter {
    char theme[256];
    char mood[64];
    int64_t started_at;
    char key_threads[8][128];
    size_t key_threads_count;
} hu_life_chapter_t;

/* Humor profile */
typedef struct hu_humor_profile {
    char *type;
    char *timing;
    char **targets;
    size_t targets_count;
    char **boundaries;
    size_t boundaries_count;
    char *frequency;
    /* Phase 6 additions — fixed-size arrays */
    char style[8][32];
    size_t style_count;
    char never_during[8][32];
    size_t never_during_count;
    char signature_phrases[8][64];
    size_t signature_phrases_count;
    char self_deprecation_topics[8][64];
    size_t self_deprecation_count;
} hu_humor_profile_t;

/* Phase 6 — relationship entry */
typedef struct hu_relationship {
    char name[64];
    char role[32];
    char notes[256];
} hu_relationship_t;

/* Conflict style — how the persona handles disagreement and friction */
typedef struct hu_conflict_style {
    char *pushback_response;
    char *confrontation_comfort;
    char *apology_style;
    char *boundary_assertion;
    char *repair_behavior;
} hu_conflict_style_t;

/* Emotional range boundaries */
typedef struct hu_emotional_range {
    char *ceiling;
    char *floor;
    char **escalation_triggers;
    size_t escalation_triggers_count;
    char **de_escalation;
    size_t de_escalation_count;
    char *withdrawal_conditions;
    char *recovery_style;
} hu_emotional_range_t;

/* Voice rhythm — text pacing and cadence */
typedef struct hu_voice_rhythm {
    char *sentence_pattern;
    char *paragraph_cadence;
    char *response_tempo;
    char *emphasis_style;
    char *pause_behavior;
} hu_voice_rhythm_t;

/* Intellectual profile */
typedef struct hu_intellectual_profile {
    char **expertise;
    size_t expertise_count;
    char **curiosity_areas;
    size_t curiosity_areas_count;
    char *thinking_style;
    char *metaphor_sources;
} hu_intellectual_profile_t;

/* Backstory-to-behavior mapping */
typedef struct hu_backstory_behavior {
    char *backstory_beat;
    char *behavioral_rule;
} hu_backstory_behavior_t;

/* Sensory preferences */
typedef struct hu_sensory_preferences {
    char *dominant_sense;
    char **metaphor_vocabulary;
    size_t metaphor_vocabulary_count;
    char *grounding_patterns;
} hu_sensory_preferences_t;

/* Relational intelligence — Gottman bids, attachment, Dunbar layers (PhD-level) */
typedef struct hu_relational_intelligence {
    char *bid_response_style;
    char **emotional_bids;
    size_t emotional_bids_count;
    char *attachment_style;
    char *attachment_awareness;
    char *dunbar_awareness;
} hu_relational_intelligence_t;

/* Listening protocol — Derber support/shift, OARS, NVC, validation (PhD-level) */
typedef struct hu_listening_protocol {
    char *default_response_type;
    char **reflective_techniques;
    size_t reflective_techniques_count;
    char *nvc_style;
    char *validation_style;
} hu_listening_protocol_t;

/* Repair protocol — rupture-repair, conversational repair, face-saving (PhD-level) */
typedef struct hu_repair_protocol {
    char *rupture_detection;
    char *repair_approach;
    char *face_saving_style;
    char **repair_phrases;
    size_t repair_phrases_count;
} hu_repair_protocol_t;

/* Linguistic mirroring — CAT, style matching, accommodation (PhD-level) */
typedef struct hu_linguistic_mirroring {
    char *mirroring_level;
    char **adapts_to;
    size_t adapts_to_count;
    char *convergence_speed;
    char *power_dynamic;
} hu_linguistic_mirroring_t;

/* Social dynamics — ego states, phatic communication, conversation management */
typedef struct hu_social_dynamics {
    char *default_ego_state;
    char *phatic_style;
    char **bonding_behaviors;
    size_t bonding_behaviors_count;
    char **anti_patterns;
    size_t anti_patterns_count;
} hu_social_dynamics_t;

/* Follow-up style — delayed follow-ups, double-texting */
typedef struct hu_follow_up_style {
    float delayed_follow_up_probability; /* default 0.15 */
    int16_t min_delay_minutes;           /* default 20 */
    int16_t max_delay_hours;             /* default 4 */
} hu_follow_up_style_t;

/* Bookend messages — morning/evening check-ins */
typedef struct hu_bookend_config {
    bool enabled;                /* default false */
    uint8_t morning_window[2];   /* default {7, 9} */
    uint8_t evening_window[2];   /* default {22, 23} */
    float frequency_per_week;    /* default 2.5 */
    char phrases_morning[8][64]; /* fixed-size arrays */
    size_t phrases_morning_count;
    char phrases_evening[8][64];
    size_t phrases_evening_count;
} hu_bookend_config_t;

/* Humanization config — disfluency, backchannels, burst messages, double-text, GIFs */
typedef struct hu_humanization_config {
    float disfluency_frequency;      /* default 0.15 */
    float backchannel_probability;   /* default 0.3 */
    float burst_message_probability; /* default 0.03 */
    float double_text_probability;   /* default 0.08 */
    float gif_probability;           /* default 0.10 */
} hu_humanization_config_t;

/* Context modifiers — topic/emotion/turn-based boosts */
typedef struct hu_context_modifiers {
    float serious_topics_reduction;      /* default 0.4 */
    float personal_sharing_warmth_boost; /* default 1.6 */
    float high_emotion_breathing_boost;  /* default 1.5 */
    float early_turn_humanization_boost; /* default 1.4 */
} hu_context_modifiers_t;

/* Important date — birthday, holiday, anniversary (MM-DD format) */
typedef struct hu_important_date {
    char date[8];      /* "07-15" MM-DD format */
    char type[32];     /* "birthday", "holiday", "anniversary" */
    char message[256]; /* "happy birthday min!" */
} hu_important_date_t;

/* Voice messages config — when to send voice vs text (per-contact) */
typedef struct hu_voice_messages_config {
    bool enabled;
    char frequency[16];     /* "rare", "occasional", "frequent" */
    char prefer_for[8][32]; /* "emotional", "late_night", "long_response", "comfort" */
    size_t prefer_for_count;
    char never_for[8][32]; /* "questions", "logistics", "quick_ack" */
    size_t never_for_count;
    uint32_t max_duration_sec; /* default 30 */
} hu_voice_messages_config_t;

/* Voice config — Cartesia TTS, cloned voice UUID, model, emotion */
typedef struct hu_persona_voice_config {
    char provider[32];         /* "cartesia" */
    char voice_id[64];         /* UUID */
    char model[64];            /* "sonic-3-2026-01-12" */
    char default_emotion[32];  /* "content" */
    float default_speed;       /* 0.95 */
    bool nonverbals;           /* true */
    float vulnerability_level; /* 0.0–1.0, EMA-tracked from content */
} hu_persona_voice_config_t;

/* Context awareness — calendar, weather, sports, news */
typedef struct hu_context_awareness {
    bool calendar_enabled;
    bool weather_enabled;
    char sports_teams[8][64];
    size_t sports_teams_count;
    char news_topics[8][64];
    size_t news_topics_count;
} hu_context_awareness_t;

/* Inner world — deep personality content surfaced by relationship stage */
typedef struct hu_inner_world {
    char **contradictions;
    size_t contradictions_count;
    char **embodied_memories;
    size_t embodied_memories_count;
    char **emotional_flashpoints;
    size_t emotional_flashpoints_count;
    char **unfinished_business;
    size_t unfinished_business_count;
    char **secret_self;
    size_t secret_self_count;
} hu_inner_world_t;

/* Cross-channel ACL: controls which relationship types can access facts from which origins */
typedef struct hu_xchan_acl_rule {
    char relationship_type[32]; /* "coworker", "family", etc */
    char **allow_list;          /* allowed relationship_types */
    size_t allow_count;
} hu_xchan_acl_rule_t;

typedef struct hu_xchan_acl {
    char default_policy[32]; /* "deny_unknown" | "allow_unknown" */
    hu_xchan_acl_rule_t *rules;
    size_t rule_count;
} hu_xchan_acl_t;

typedef struct hu_persona {
    char *name;
    size_t name_len;
    char *identity;
    char **traits;
    size_t traits_count;
    char **principles; /* Constitutional AI principles */
    size_t principles_count;
    char **preferred_vocab;
    size_t preferred_vocab_count;
    char **avoided_vocab;
    size_t avoided_vocab_count;
    char **slang;
    size_t slang_count;
    char **communication_rules;
    size_t communication_rules_count;
    char **values;
    size_t values_count;
    char *decision_style;
    char *biography;
    char **directors_notes;
    size_t directors_notes_count;
    char **mood_states;
    size_t mood_states_count;
    hu_inner_world_t inner_world;
    hu_persona_motivation_t motivation;
    hu_situational_direction_t *situational_directions;
    size_t situational_directions_count;
    hu_humor_profile_t humor;
    hu_conflict_style_t conflict_style;
    hu_emotional_range_t emotional_range;
    hu_voice_rhythm_t voice_rhythm;
    char **character_invariants;
    size_t character_invariants_count;
    char *core_anchor;
    hu_intellectual_profile_t intellectual;
    hu_backstory_behavior_t *backstory_behaviors;
    size_t backstory_behaviors_count;
    hu_sensory_preferences_t sensory;
    hu_relational_intelligence_t relational;
    hu_listening_protocol_t listening;
    hu_repair_protocol_t repair;
    hu_linguistic_mirroring_t mirroring;
    hu_social_dynamics_t social;
    hu_persona_overlay_t *overlays;
    size_t overlays_count;
    hu_persona_example_bank_t *example_banks;
    size_t example_banks_count;
    hu_contact_profile_t *contacts;
    size_t contacts_count;
    /* Externalized prompt content (loaded from JSON, avoids hardcoding in C) */
    char **immersive_reinforcement;
    size_t immersive_reinforcement_count;
    char *identity_reinforcement;
    char **anti_patterns;
    size_t anti_patterns_count;
    char **style_rules;
    size_t style_rules_count;
    char *proactive_rules;
    /* Time-of-day overlays */
    char *time_overlay_late_night;
    char *time_overlay_early_morning;
    char *time_overlay_afternoon;
    char *time_overlay_evening;
    hu_humanization_config_t humanization;
    hu_context_modifiers_t context_modifiers;
    hu_important_date_t *important_dates;
    size_t important_dates_count;
    hu_context_awareness_t context_awareness;
    /* Phase 4 — follow-ups, bookends, timezone, location, group behavior */
    hu_follow_up_style_t follow_up_style;
    hu_bookend_config_t bookend_messages;
    char timezone[64];
    char location[128];
    float group_response_rate; /* default 0.1 */
    /* Phase 5 — voice config (Cartesia TTS, cloned voice) */
    hu_persona_voice_config_t voice;
    hu_voice_messages_config_t voice_messages;
    /* Phase 6 — daily routine, life chapter, humor (extended), memory, values, relationships */
    hu_daily_routine_t daily_routine;
    hu_life_chapter_t current_chapter;
    float memory_degradation_rate; /* default 0.10 */
    char core_values[8][64];
    size_t core_values_count;
    hu_relationship_t relationships[16];
    size_t relationships_count;

    /* Reply pacing parameters (AC-7) — enforce human-like latency */
    int32_t min_reply_delay_ms;      /* default 250; minimum wall-clock before sending reply */
    int32_t reply_delay_variance_ms; /* default 600; adds ±variance ms jitter to pacing */

    /* Recent activity context (loaded from ~/.human/photos/recent_activity.json) */
    char *recent_activity;

    /* Behavioral calibration data (from behavioral_calibration JSON or hu_behavioral_clone) */
    double avg_message_length; /* 0 = not set */
    double emoji_frequency;    /* 0.0–1.0, fraction of messages with emoji */
    double avg_response_time_sec;
    char **signature_phrases;
    size_t signature_phrases_count;
    bool calibrated;
    /* B16: optional chronotype for JITAI / quiet-hours alignment (JSON `chronotype`). */
    hu_chronotype_t chronotype;
    /* When true, the agent sets response_format="json_schema" + response_schema on every
     * outbound chat request so the provider enforces the canonical reply schema (Layer 1
     * of the three-layer output defense). Defaults to false; opt-in via JSON key
     * "structured_output_enabled": true. */
    bool structured_output_enabled;
    /* Cached outbound validator chain built once at hu_persona_load_json() time.
     * Owned by the persona for its full lifetime; destroyed in hu_persona_deinit().
     * NULL only when chain build failed during persona load (e.g., out-of-memory).
     * An empty rule set still produces a non-NULL chain that executes no validators.
     * Persona name is treated as immutable post-load — mutating persona->name after
     * load does NOT re-derive the chain. */
    hu_output_validator_chain_t *outbound_chain;

    /* Global proactive-messaging kill switch.  When false, the daemon must NOT
     * generate or send ANY proactive message (F25 emotional check-ins, F30
     * curiosity, F31 callbacks, F23 topic absence, F12 bookend, scheduled
     * good-morning, etc.) regardless of per-contact `proactive_checkin` flags.
     *
     * Defaults to false (kill-switch ON) so a freshly loaded persona is safe
     * until the operator explicitly opts in via "proactive.master_enabled":
     * true in the persona JSON.  Pinned by the 2026-05-16 incident — see
     * docs/research/2026-05-16-proactive-audit/findings.md (P1-1). */
    bool proactive_master_enabled;

    /* Cross-channel ACL for privacy gating. Controls which relationship types
     * can access facts/patterns sourced from which origins. Safe defaults
     * ship in code; user can override per-persona via JSON.
     * AC-1: family facts MUST NEVER reach coworker turns. */
    hu_xchan_acl_t cross_channel_acl;
} hu_persona_t;

/* Returns persona base directory path in buf (either HU_PERSONA_DIR or ~/.human/personas).
   Returns buf on success, NULL on failure. */
const char *hu_persona_base_dir(char *buf, size_t cap);

hu_error_t hu_persona_load(hu_allocator_t *alloc, const char *name, size_t name_len,
                           hu_persona_t *out);

hu_error_t hu_persona_load_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                hu_persona_t *out);

hu_error_t hu_persona_validate_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                    char **err_msg, size_t *err_msg_len);

hu_error_t hu_persona_examples_load_json(hu_allocator_t *alloc, const char *channel,
                                         size_t channel_len, const char *json, size_t json_len,
                                         hu_persona_example_bank_t *out);

/* Populate one example bank from an already-parsed JSON "examples" array (persona file
 * "example_banks" entries). On success, out is filled; on failure, out is cleared. */
hu_error_t hu_persona_examples_bank_from_array(hu_allocator_t *alloc, const char *channel,
                                               size_t channel_len,
                                               const struct hu_json_value *examples_arr,
                                               hu_persona_example_bank_t *out);

void hu_persona_deinit(hu_allocator_t *alloc, hu_persona_t *persona);

/* Convenience alias for hu_persona_deinit (for test code symmetry) */
void hu_persona_free(hu_persona_t *persona);

/* Load a persona stub with safe-default ACL only (for tests) */
void hu_persona_load_defaults(hu_persona_t *out);

/* Returns true iff proactive messaging is GLOBALLY enabled for this persona.
 * Safe to call with persona == NULL (returns false).  Wraps the
 * `proactive_master_enabled` field so the daemon never bypasses the gate.
 * See 2026-05-16 incident — by default the gate is OFF, so a freshly loaded
 * persona will not send proactive messages until the operator explicitly
 * sets "proactive": { "master_enabled": true } in the persona JSON. */
bool hu_persona_proactive_is_enabled(const hu_persona_t *persona);

hu_error_t hu_persona_build_prompt(hu_allocator_t *alloc, const hu_persona_t *persona,
                                   const char *channel, size_t channel_len, const char *topic,
                                   size_t topic_len, char **out, size_t *out_len);

/* 2026-05-18: compact variant for throughput-sensitive callers (eval
 * framework, short-form chat). Produces ~2-3 KB instead of 16 KB by
 * including only: identity (truncated 600 chars), the requested channel
 * overlay, top-N communication_rules and avoided_vocab, humor style +
 * signature phrases, and up to 5 example shots from the matching bank.
 *
 * Empirical motivation from the 2026-05-18 audit chain: the 16 KB
 * prompt produced 60-95 s end-to-end latencies on a 31B MLX model,
 * triggering provider-timeout-driven NULL responses on ~75% of eval
 * tasks. The 2 KB compact form completes in ~13 s and produces
 * identical-in-voice output (proven via
 * scripts/persona_eval_comparison.py). Same in-voice quality,
 * 6x throughput.
 *
 * For callers that need the full prompt (production agent_turn,
 * long-form planning), use hu_persona_build_prompt. For
 * short-form eval / chat where every msec counts, use this. */
hu_error_t hu_persona_build_prompt_compact(hu_allocator_t *alloc, const hu_persona_t *persona,
                                           const char *channel, size_t channel_len, char **out,
                                           size_t *out_len);

/* P6-5: shared absolute-rules block. Writes the highest-weight
 * formatting/identity instructions ("You are HUMAN", lowercase, no
 * markdown, etc.) into the caller's buffer. Called from BOTH the
 * reactive path (src/agent/agent_stream.c) and the proactive path
 * (src/daemon_proactive.c) so the two paths cannot drift.
 *
 * `persona` is currently unused but accepted so future per-persona
 * overrides don't break the call site.
 *
 * Returns HU_OK on success with *out_len set; HU_ERR_INVALID_ARGUMENT
 * on NULL buf or zero cap; HU_ERR_OUT_OF_MEMORY if the static block
 * would exceed cap. */
hu_error_t hu_persona_build_absolute_rules(const hu_persona_t *persona, char *buf, size_t cap,
                                           size_t *out_len);

hu_error_t hu_persona_select_examples(const hu_persona_t *persona, const char *channel,
                                      size_t channel_len, const char *topic, size_t topic_len,
                                      const hu_persona_example_t **out, size_t *out_count,
                                      size_t max_examples);

/* M3 Bridge A.0 — export the persona's example banks to JSONL in the
 * Alpaca shape ({"instruction": ..., "input": ..., "output": ...}).
 *
 * Compatible with llama.cpp/finetune, axolotl, unsloth, mlx-lm.lora,
 * and most open-weight fine-tuning toolchains. The instruction string
 * carries the channel and optional context: "On <channel>: <context>".
 * Examples missing either incoming or response are skipped.
 *
 * On success, *exported_count is the number of rows actually written.
 * The file at `path` is truncated and rewritten; the parent directory
 * must already exist. Returns HU_ERR_IO on file-system failure,
 * HU_ERR_INVALID_ARGUMENT on NULL inputs.
 *
 * This is the "export side" of the frontier-model fine-tune bridge —
 * users run the exporter, feed the JSONL to their preferred finetune
 * toolchain, and configure the resulting adapter back into the daemon
 * via `personalization.lora_adapter_path`. Closes the loop end-to-end
 * without requiring llama.cpp to be vendored in-tree. See
 * docs/plans/2026-05-10-m3-frontier-model-bridge.md for the full
 * Bridge A plan. */
hu_error_t hu_persona_bank_export_jsonl(const hu_persona_t *persona, const char *path,
                                        size_t path_len, size_t *exported_count);

/* Phase A1.3 — derive per-channel persona example banks from
 * conversation history.
 *
 * Reads the `messages` table in the SQLite database at `db_path`,
 * walks adjacent user→assistant message pairs in id-ascending order
 * within each session, and groups the surviving pairs into one
 * example bank per detected channel.
 *
 * Channel detection: the per-message `session_id` follows the
 * "<channel>:<contact-or-thread-id>" convention used everywhere in
 * the codebase (e.g. "telegram:123", "imessage:thread-7"). The
 * substring before the first ':' is the channel; sessions without
 * a ':' fall under "default".
 *
 * Quality gates (applied to every candidate pair, in order):
 *   1. Both halves must be non-empty after PII redaction
 *      (hu_pii_redact — emails, phones, SSNs, CC, IPs, secrets).
 *   2. Concatenated user+assistant text must pass
 *      hu_quality_check with default thresholds (length / Shannon
 *      entropy / unique-byte ratio).
 *   3. The pair must not collide with a previously-emitted pair
 *      under hu_dedup_set_check_and_add (lowercased,
 *      whitespace-collapsed FNV-1a). Dedup is global across the
 *      whole extraction, not per channel — the same exchange
 *      crossed by both Telegram and iMessage shows up once.
 *
 * Per-channel bank cap: at most `max_per_channel` examples; the
 * earliest qualifying pairs win (deterministic with id-asc ordering
 * in the messages table). `max_per_channel == 0` uses the default
 * (32). The number of channels is internally bounded at 32 — the
 * 33rd novel channel encountered is dropped silently rather than
 * crashing or growing without limit.
 *
 * Memory: on success, *out_banks is an allocator-owned array of
 * populated banks (channels with ≥1 surviving example) and
 * *out_count is its length. Free with hu_persona_example_banks_free.
 * On failure, *out_banks is NULL and *out_count is 0.
 *
 * Returns:
 *   HU_OK                    on success (0 banks is valid)
 *   HU_ERR_INVALID_ARGUMENT  on NULL inputs
 *   HU_ERR_NOT_SUPPORTED     when the build lacks SQLite
 *   HU_ERR_IO                on database open / query failure
 *   HU_ERR_OUT_OF_MEMORY     on allocation failure
 *
 * This is the missing input side of the LoRA personalization
 * pipeline: the agent already records every conversation, but
 * persona example banks were previously hand-authored. With this
 * function the operator can run `human persona learn-banks` (or any
 * equivalent) once on their existing message history, get a
 * channel-segregated bank with the obvious garbage filtered out,
 * and feed it straight to hu_persona_bank_export_jsonl for fine-
 * tuning. Closes the M3 Bridge A loop end-to-end. */
hu_error_t hu_persona_banks_extract_from_history(hu_allocator_t *alloc, const char *db_path,
                                                 size_t max_per_channel,
                                                 hu_persona_example_bank_t **out_banks,
                                                 size_t *out_count);

/* Free an array of example banks allocated by
 * hu_persona_banks_extract_from_history. Frees every channel string,
 * every example field, every per-bank examples array, and finally
 * the banks array itself. Safe to call with banks==NULL or count==0;
 * a zero-initialized bank within the array is also safe (NULL fields
 * are skipped). */
void hu_persona_example_banks_free(hu_allocator_t *alloc, hu_persona_example_bank_t *banks,
                                   size_t banks_count);

const hu_persona_overlay_t *hu_persona_find_overlay(const hu_persona_t *persona,
                                                    const char *channel, size_t channel_len);

/* Render outbound text for a channel by applying its persona overlay.
 *
 * Behavior (in fixed order):
 *   1. If overlay->emoji_usage is "none" / "no" / "off" / "never", strip emoji.
 *   2. If overlay->formality contains "formal" or "professional", apply
 *      casual-to-formal lexical swaps ("hey" -> "hello", "yeah" -> "yes",
 *      "gonna" -> "going to", etc.) and capitalize the first letter.
 *      If overlay->formality contains "casual" or "informal", apply the
 *      reverse swaps and lowercase the first letter.
 *   3. If overlay->avg_length is "short" or has the form "max_chars=NNN",
 *      truncate the output to at most NNN bytes (default short = 200).
 *      For length=="short", truncates at the last sentence boundary
 *      within 200 bytes when possible.
 *
 * When overlay is NULL, the output is a heap-allocated copy of raw_text
 * (no transforms). When raw_text is NULL or raw_len == 0, returns OK with
 * an empty string ("").
 *
 * Caller owns *out_rendered and *out_rendered_len; free with
 * alloc->free(alloc->ctx, *out_rendered, *out_rendered_len + 1).
 *
 * Returns HU_OK on success. HU_ERR_INVALID_ARGUMENT if alloc or
 * out_rendered is NULL. HU_ERR_OUT_OF_MEMORY on allocation failure. */
hu_error_t hu_persona_render_for_channel(const hu_persona_overlay_t *overlay, const char *raw_text,
                                         size_t raw_len, hu_allocator_t *alloc, char **out_rendered,
                                         size_t *out_rendered_len);

/* Pure predicate: returns the effective formality string for rendering given
 * an overlay's configured formality and (optionally) a contact's warmth_level.
 *
 * Rules:
 *   - contact_warmth indicates closeness ("close", "high", "warm") AND the
 *     overlay's formality is unset OR formal-leaning ("formal" / "professional")
 *     → return "casual" (the contact relationship overrides a stiff overlay).
 *   - Any other combination → return overlay_formality unchanged (NULL is OK).
 *
 * Returned pointer is either overlay_formality itself or a static "casual"
 * string. Do NOT free. Pure; safe for tests.
 *
 * This is the predicate that wires the persona's "warmth_level" field
 * (previously parsed-but-never-read in the send path) into deterministic
 * render-time behavior — extending the compiled-persona-architecture pattern
 * pioneered by hu_followup_compute_send_time. */
const char *hu_persona_effective_formality(const char *overlay_formality,
                                           const char *contact_warmth);

/* Render variant that takes a contact's warmth_level string. The renderer
 * uses hu_persona_effective_formality(overlay->formality, contact_warmth)
 * as the effective formality, so a close-relationship contact gets casual
 * rendering even when the channel overlay is configured formal.
 *
 * Callers without contact context should pass NULL for contact_warmth;
 * behavior then matches the original hu_persona_render_for_channel exactly.
 *
 * Pinned by tests/test_persona_render.c (warmth-override cases). */
hu_error_t hu_persona_render_for_channel_with_warmth(const hu_persona_overlay_t *overlay,
                                                     const char *contact_warmth,
                                                     const char *raw_text, size_t raw_len,
                                                     hu_allocator_t *alloc, char **out_rendered,
                                                     size_t *out_rendered_len);

const hu_contact_profile_t *hu_persona_find_contact(const hu_persona_t *persona,
                                                    const char *contact_id, size_t contact_id_len);

hu_error_t hu_contact_profile_build_context(hu_allocator_t *alloc,
                                            const hu_contact_profile_t *contact, char **out,
                                            size_t *out_len);

/* Affect mirror ceiling: get the effective ceiling for emotional mirroring.
 * Priority: contact override > overlay override > stage default.
 * Stage defaults: acquaintance=0.7, friend=0.85, close_friend/family=0.9.
 * Returns 0.7 if no stage is set. */
float hu_affect_mirror_ceiling(const hu_contact_profile_t *contact,
                               const hu_persona_overlay_t *overlay);

/* Effective leave-on-read percentage: per-contact > per-channel-overlay > 0.
 * 0 signals "use classifier default (10%)" — callers pass the return value to
 * hu_conversation_should_leave_on_read where 0 triggers the default. NULL-safe
 * for both parameters. Mirrors hu_affect_mirror_ceiling's layered-lookup
 * shape so the per-contact override behaves the same way across overlay axes. */
uint8_t hu_leave_on_read_pct_effective(const hu_contact_profile_t *contact,
                                       const hu_persona_overlay_t *overlay);

/* Apply affect mirror ceiling to emotional intensity.
 * If intensity > ceiling, returns the ceiling value.
 * Also returns a dampening directive in *directive (stack buffer, may be "").
 * directive_cap: capacity of directive buffer. */
float hu_affect_mirror_apply(float intensity, float ceiling, char *directive, size_t directive_cap);

/* Build inner world context, stage-gated. Only surfaces for friend+ stages.
 * Returns NULL if stage is too low or no inner world content. */
char *hu_persona_build_inner_world_context(hu_allocator_t *alloc, const hu_persona_t *persona,
                                           const char *relationship_stage, size_t *out_len);

/* Return a filtered view of inner world content for a relationship stage.
 * Graduated disclosure per category:
 *   embodied_memories: any stage (NEW+)
 *   contradictions: FAMILIAR+
 *   emotional_flashpoints: TRUSTED+
 *   unfinished_business: TRUSTED+
 *   secret_self: DEEP only
 * Returns shallow copy — pointers reference persona's data, do NOT free. */
hu_inner_world_t hu_persona_inner_world_for_stage(const hu_persona_t *persona,
                                                  hu_relationship_stage_t stage);

/* Build inner world context with graduated stage gating (enum-based).
 * Uses hu_persona_inner_world_for_stage internally for per-category filtering.
 * Returns NULL if no content available at this stage. */
char *hu_persona_build_inner_world_graduated(hu_allocator_t *alloc, const hu_persona_t *persona,
                                             hu_relationship_stage_t stage, size_t *out_len);

/* Feedback — user corrections for persona learning */
typedef struct hu_persona_feedback {
    const char *channel;
    size_t channel_len;
    const char *original_response;
    size_t original_response_len;
    const char *corrected_response;
    size_t corrected_response_len;
    const char *context;
    size_t context_len;
} hu_persona_feedback_t;

hu_error_t hu_persona_feedback_record(hu_allocator_t *alloc, const char *persona_name,
                                      size_t persona_name_len,
                                      const hu_persona_feedback_t *feedback);

hu_error_t hu_persona_feedback_apply(hu_allocator_t *alloc, const char *persona_name,
                                     size_t persona_name_len);

/* Message sampler — builds SQL / parses exports for persona creation pipeline */
hu_error_t hu_persona_sampler_imessage_query(char *buf, size_t cap, size_t *out_len, size_t limit);
hu_error_t hu_persona_sampler_imessage_conversation_query(const char *handle_id,
                                                          size_t handle_id_len, char *buf,
                                                          size_t cap, size_t *out_len,
                                                          size_t limit);

/* Raw message from a conversation sampler (used by example bank builder) */
typedef struct hu_sampler_raw_msg {
    const char *text;
    size_t text_len;
    int64_t timestamp;
    bool is_from_me;
} hu_sampler_raw_msg_t;

/* Build example bank entries from raw two-sided conversation messages */
hu_error_t hu_persona_sampler_build_examples(hu_allocator_t *alloc,
                                             const hu_sampler_raw_msg_t *msgs, size_t msg_count,
                                             hu_persona_example_t **out, size_t *out_count);

/* Auto-detect contact profile stats from conversation messages */
typedef struct hu_sampler_contact_stats {
    size_t their_msg_count;
    size_t my_msg_count;
    size_t avg_their_len;
    size_t avg_my_len;
    bool uses_emoji;
    bool sends_links;
    bool texts_in_bursts;
    bool prefers_short;
} hu_sampler_contact_stats_t;

hu_error_t hu_persona_sampler_detect_contact(hu_allocator_t *alloc,
                                             const hu_sampler_raw_msg_t *msgs, size_t msg_count,
                                             hu_sampler_contact_stats_t *out);

hu_error_t hu_persona_sampler_facebook_parse(const char *json, size_t json_len, char ***out,
                                             size_t *out_count);
hu_error_t hu_persona_sampler_gmail_parse(const char *json, size_t json_len, char ***out,
                                          size_t *out_count);

/* Provider analyzer — builds extraction prompt, parses provider JSON into partial persona */
hu_error_t hu_persona_analyzer_build_prompt(const char **messages, size_t msg_count,
                                            const char *channel, char *buf, size_t cap,
                                            size_t *out_len);
hu_error_t hu_persona_analyzer_parse_response(hu_allocator_t *alloc, const char *response,
                                              size_t resp_len, const char *channel,
                                              size_t channel_len, hu_persona_t *out);

/* Creator pipeline — merges partial personas into one */
hu_error_t hu_persona_creator_synthesize(hu_allocator_t *alloc, const hu_persona_t *partials,
                                         size_t count, const char *name, size_t name_len,
                                         hu_persona_t *out);
hu_error_t hu_persona_creator_write(hu_allocator_t *alloc, const hu_persona_t *persona);

/* CLI types and commands */
typedef enum {
    HU_PERSONA_ACTION_CREATE,
    HU_PERSONA_ACTION_UPDATE,
    HU_PERSONA_ACTION_SHOW,
    HU_PERSONA_ACTION_LIST,
    HU_PERSONA_ACTION_DELETE,
    HU_PERSONA_ACTION_VALIDATE,
    HU_PERSONA_ACTION_FEEDBACK_APPLY,
    HU_PERSONA_ACTION_DIFF,
    HU_PERSONA_ACTION_EXPORT,
    HU_PERSONA_ACTION_MERGE,
    HU_PERSONA_ACTION_IMPORT,
    HU_PERSONA_ACTION_EVAL,
    HU_PERSONA_ACTION_EXPORT_BANK,  /* human persona export-bank <name> [--output <path>] */
    HU_PERSONA_ACTION_FILLER_ADD,   /* human persona filler add --channel <ch> "<text>" */
    HU_PERSONA_ACTION_FILLER_LIST,  /* human persona filler list --channel <ch> */
    HU_PERSONA_ACTION_FILLER_REMOVE /* human persona filler remove --channel <ch> --index N */
} hu_persona_action_t;

typedef struct hu_persona_cli_args {
    hu_persona_action_t action;
    const char *name;
    const char *diff_name; /* second persona for diff action */
    bool from_imessage;
    bool from_gmail;
    bool from_facebook;
    bool interactive;
    const char *facebook_export_path;
    const char *gmail_export_path;
    const char *response_file; /* --from-response <path> */
    const char *with_contact;  /* --with-contact <handle_id> for conversation extraction */
    const char **merge_sources;
    size_t merge_sources_count;
    const char *import_file;    /* --from-file <path> or NULL for --from-stdin */
    const char *filler_channel; /* --channel <name> for filler subcommands */
    const char *filler_text;    /* positional "<text>" for filler add */
    int filler_index;           /* --index <n> for filler remove; -1 = not set */
    /* --output <path> for export-bank action; NULL means stdout. The
     * underlying exporter (`hu_persona_bank_export_jsonl`) requires a
     * path, so a NULL value resolves to an in-process temp file that
     * we read back and stream to stdout. */
    const char *output_path;
} hu_persona_cli_args_t;

hu_error_t hu_persona_cli_parse(int argc, const char **argv, hu_persona_cli_args_t *out);
hu_error_t hu_persona_cli_run(hu_allocator_t *alloc, const hu_persona_cli_args_t *args);

/* Style learning loop: re-analyze recent conversations to refine persona.
 * Call periodically (e.g. every 50 turns) to close the style feedback loop. */
struct hu_legacy_memory;
struct hu_provider;
hu_error_t hu_persona_style_reanalyze(hu_allocator_t *alloc, struct hu_provider *provider,
                                      const char *model, size_t model_len,
                                      struct hu_legacy_memory *memory, const char *persona_name,
                                      size_t persona_name_len, const char *channel,
                                      size_t channel_len, const char *contact_id,
                                      size_t contact_id_len);

#endif /* HU_PERSONA_H */
