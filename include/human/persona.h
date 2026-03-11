#ifndef HU_PERSONA_H
#define HU_PERSONA_H

#include "human/core/allocator.h"

#define HU_PERSONA_PROMPT_MAX_BYTES (24 * 1024) /* 24 KB cap for research-rich personas */

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_persona_overlay {
    char *channel;
    char *formality;
    char *avg_length;
    char *emoji_usage;
    char **style_notes;
    size_t style_notes_count;
    bool message_splitting;
    uint32_t max_segment_chars;
    char **typing_quirks;
    size_t typing_quirks_count;
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

/* Humor profile */
typedef struct hu_humor_profile {
    char *type;
    char *frequency;
    char **targets;
    size_t targets_count;
    char **boundaries;
    size_t boundaries_count;
    char *timing;
} hu_humor_profile_t;

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
    bool enabled;                   /* default false */
    uint8_t morning_window[2];       /* default {7, 9} */
    uint8_t evening_window[2];       /* default {22, 23} */
    float frequency_per_week;        /* default 2.5 */
    char phrases_morning[8][64];    /* fixed-size arrays */
    size_t phrases_morning_count;
    char phrases_evening[8][64];
    size_t phrases_evening_count;
} hu_bookend_config_t;

/* Humanization config — disfluency, backchannels, burst messages, double-text */
typedef struct hu_humanization_config {
    float disfluency_frequency;      /* default 0.15 */
    float backchannel_probability;   /* default 0.3 */
    float burst_message_probability; /* default 0.03 */
    float double_text_probability;  /* default 0.08 */
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
    char frequency[16];           /* "rare", "occasional", "frequent" */
    char prefer_for[8][32];        /* "emotional", "late_night", "long_response", "comfort" */
    size_t prefer_for_count;
    char never_for[8][32];         /* "questions", "logistics", "quick_ack" */
    size_t never_for_count;
    uint32_t max_duration_sec;     /* default 30 */
} hu_voice_messages_config_t;

/* Voice config — Cartesia TTS, cloned voice UUID, model, emotion */
typedef struct hu_persona_voice_config {
    char provider[32];        /* "cartesia" */
    char voice_id[64];        /* UUID */
    char model[64];           /* "sonic-3-2026-01-12" */
    char default_emotion[32];  /* "content" */
    float default_speed;      /* 0.95 */
    bool nonverbals;          /* true */
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

typedef struct hu_persona {
    char *name;
    size_t name_len;
    char *identity;
    char **traits;
    size_t traits_count;
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

void hu_persona_deinit(hu_allocator_t *alloc, hu_persona_t *persona);

hu_error_t hu_persona_build_prompt(hu_allocator_t *alloc, const hu_persona_t *persona,
                                   const char *channel, size_t channel_len, const char *topic,
                                   size_t topic_len, char **out, size_t *out_len);

hu_error_t hu_persona_select_examples(const hu_persona_t *persona, const char *channel,
                                      size_t channel_len, const char *topic, size_t topic_len,
                                      const hu_persona_example_t **out, size_t *out_count,
                                      size_t max_examples);

const hu_persona_overlay_t *hu_persona_find_overlay(const hu_persona_t *persona,
                                                    const char *channel, size_t channel_len);

const hu_contact_profile_t *hu_persona_find_contact(const hu_persona_t *persona,
                                                    const char *contact_id, size_t contact_id_len);

hu_error_t hu_contact_profile_build_context(hu_allocator_t *alloc,
                                            const hu_contact_profile_t *contact, char **out,
                                            size_t *out_len);

/* Build inner world context, stage-gated. Only surfaces for friend+ stages.
 * Returns NULL if stage is too low or no inner world content. */
char *hu_persona_build_inner_world_context(hu_allocator_t *alloc, const hu_persona_t *persona,
                                           const char *relationship_stage, size_t *out_len);

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
    HU_PERSONA_ACTION_IMPORT
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
    const char *import_file; /* --from-file <path> or NULL for --from-stdin */
} hu_persona_cli_args_t;

hu_error_t hu_persona_cli_parse(int argc, const char **argv, hu_persona_cli_args_t *out);
hu_error_t hu_persona_cli_run(hu_allocator_t *alloc, const hu_persona_cli_args_t *args);

#endif /* HU_PERSONA_H */
