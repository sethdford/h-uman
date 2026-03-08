#ifndef SC_PERSONA_H
#define SC_PERSONA_H

#include "seaclaw/core/allocator.h"

#define SC_PERSONA_PROMPT_MAX_BYTES (24 * 1024) /* 24 KB cap for research-rich personas */

#include "seaclaw/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sc_persona_overlay {
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
} sc_persona_overlay_t;

typedef struct sc_persona_example {
    char *context;
    char *incoming;
    char *response;
} sc_persona_example_t;

typedef struct sc_persona_example_bank {
    char *channel;
    sc_persona_example_t *examples;
    size_t examples_count;
} sc_persona_example_bank_t;

typedef struct sc_contact_profile {
    char *contact_id;
    char *name;
    char *email;
    char *relationship;
    char *relationship_stage;
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
} sc_contact_profile_t;

/* Motivation — the character's core drive (anti-drift anchor) */
typedef struct sc_persona_motivation {
    char *primary_drive;
    char *protecting;
    char *avoiding;
    char *wanting;
} sc_persona_motivation_t;

/* Situational direction — trigger → behavior pairs (director's scene notes) */
typedef struct sc_situational_direction {
    char *trigger;
    char *instruction;
} sc_situational_direction_t;

/* Humor profile */
typedef struct sc_humor_profile {
    char *type;
    char *frequency;
    char **targets;
    size_t targets_count;
    char **boundaries;
    size_t boundaries_count;
    char *timing;
} sc_humor_profile_t;

/* Conflict style — how the persona handles disagreement and friction */
typedef struct sc_conflict_style {
    char *pushback_response;
    char *confrontation_comfort;
    char *apology_style;
    char *boundary_assertion;
    char *repair_behavior;
} sc_conflict_style_t;

/* Emotional range boundaries */
typedef struct sc_emotional_range {
    char *ceiling;
    char *floor;
    char **escalation_triggers;
    size_t escalation_triggers_count;
    char **de_escalation;
    size_t de_escalation_count;
    char *withdrawal_conditions;
    char *recovery_style;
} sc_emotional_range_t;

/* Voice rhythm — text pacing and cadence */
typedef struct sc_voice_rhythm {
    char *sentence_pattern;
    char *paragraph_cadence;
    char *response_tempo;
    char *emphasis_style;
    char *pause_behavior;
} sc_voice_rhythm_t;

/* Intellectual profile */
typedef struct sc_intellectual_profile {
    char **expertise;
    size_t expertise_count;
    char **curiosity_areas;
    size_t curiosity_areas_count;
    char *thinking_style;
    char *metaphor_sources;
} sc_intellectual_profile_t;

/* Backstory-to-behavior mapping */
typedef struct sc_backstory_behavior {
    char *backstory_beat;
    char *behavioral_rule;
} sc_backstory_behavior_t;

/* Sensory preferences */
typedef struct sc_sensory_preferences {
    char *dominant_sense;
    char **metaphor_vocabulary;
    size_t metaphor_vocabulary_count;
    char *grounding_patterns;
} sc_sensory_preferences_t;

/* Relational intelligence — Gottman bids, attachment, Dunbar layers (PhD-level) */
typedef struct sc_relational_intelligence {
    char *bid_response_style;
    char **emotional_bids;
    size_t emotional_bids_count;
    char *attachment_style;
    char *attachment_awareness;
    char *dunbar_awareness;
} sc_relational_intelligence_t;

/* Listening protocol — Derber support/shift, OARS, NVC, validation (PhD-level) */
typedef struct sc_listening_protocol {
    char *default_response_type;
    char **reflective_techniques;
    size_t reflective_techniques_count;
    char *nvc_style;
    char *validation_style;
} sc_listening_protocol_t;

/* Repair protocol — rupture-repair, conversational repair, face-saving (PhD-level) */
typedef struct sc_repair_protocol {
    char *rupture_detection;
    char *repair_approach;
    char *face_saving_style;
    char **repair_phrases;
    size_t repair_phrases_count;
} sc_repair_protocol_t;

/* Linguistic mirroring — CAT, style matching, accommodation (PhD-level) */
typedef struct sc_linguistic_mirroring {
    char *mirroring_level;
    char **adapts_to;
    size_t adapts_to_count;
    char *convergence_speed;
    char *power_dynamic;
} sc_linguistic_mirroring_t;

/* Social dynamics — ego states, phatic communication, conversation management */
typedef struct sc_social_dynamics {
    char *default_ego_state;
    char *phatic_style;
    char **bonding_behaviors;
    size_t bonding_behaviors_count;
    char **anti_patterns;
    size_t anti_patterns_count;
} sc_social_dynamics_t;

/* Inner world — deep personality content surfaced by relationship stage */
typedef struct sc_inner_world {
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
} sc_inner_world_t;

typedef struct sc_persona {
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
    sc_inner_world_t inner_world;
    sc_persona_motivation_t motivation;
    sc_situational_direction_t *situational_directions;
    size_t situational_directions_count;
    sc_humor_profile_t humor;
    sc_conflict_style_t conflict_style;
    sc_emotional_range_t emotional_range;
    sc_voice_rhythm_t voice_rhythm;
    char **character_invariants;
    size_t character_invariants_count;
    char *core_anchor;
    sc_intellectual_profile_t intellectual;
    sc_backstory_behavior_t *backstory_behaviors;
    size_t backstory_behaviors_count;
    sc_sensory_preferences_t sensory;
    sc_relational_intelligence_t relational;
    sc_listening_protocol_t listening;
    sc_repair_protocol_t repair;
    sc_linguistic_mirroring_t mirroring;
    sc_social_dynamics_t social;
    sc_persona_overlay_t *overlays;
    size_t overlays_count;
    sc_persona_example_bank_t *example_banks;
    size_t example_banks_count;
    sc_contact_profile_t *contacts;
    size_t contacts_count;
} sc_persona_t;

/* Returns persona base directory path in buf (either SC_PERSONA_DIR or ~/.seaclaw/personas).
   Returns buf on success, NULL on failure. */
const char *sc_persona_base_dir(char *buf, size_t cap);

sc_error_t sc_persona_load(sc_allocator_t *alloc, const char *name, size_t name_len,
                           sc_persona_t *out);

sc_error_t sc_persona_load_json(sc_allocator_t *alloc, const char *json, size_t json_len,
                                sc_persona_t *out);

sc_error_t sc_persona_validate_json(sc_allocator_t *alloc, const char *json, size_t json_len,
                                    char **err_msg, size_t *err_msg_len);

sc_error_t sc_persona_examples_load_json(sc_allocator_t *alloc, const char *channel,
                                         size_t channel_len, const char *json, size_t json_len,
                                         sc_persona_example_bank_t *out);

void sc_persona_deinit(sc_allocator_t *alloc, sc_persona_t *persona);

sc_error_t sc_persona_build_prompt(sc_allocator_t *alloc, const sc_persona_t *persona,
                                   const char *channel, size_t channel_len, const char *topic,
                                   size_t topic_len, char **out, size_t *out_len);

sc_error_t sc_persona_select_examples(const sc_persona_t *persona, const char *channel,
                                      size_t channel_len, const char *topic, size_t topic_len,
                                      const sc_persona_example_t **out, size_t *out_count,
                                      size_t max_examples);

const sc_persona_overlay_t *sc_persona_find_overlay(const sc_persona_t *persona,
                                                    const char *channel, size_t channel_len);

const sc_contact_profile_t *sc_persona_find_contact(const sc_persona_t *persona,
                                                    const char *contact_id, size_t contact_id_len);

sc_error_t sc_contact_profile_build_context(sc_allocator_t *alloc,
                                            const sc_contact_profile_t *contact, char **out,
                                            size_t *out_len);

/* Build inner world context, stage-gated. Only surfaces for friend+ stages.
 * Returns NULL if stage is too low or no inner world content. */
char *sc_persona_build_inner_world_context(sc_allocator_t *alloc, const sc_persona_t *persona,
                                           const char *relationship_stage, size_t *out_len);

/* Feedback — user corrections for persona learning */
typedef struct sc_persona_feedback {
    const char *channel;
    size_t channel_len;
    const char *original_response;
    size_t original_response_len;
    const char *corrected_response;
    size_t corrected_response_len;
    const char *context;
    size_t context_len;
} sc_persona_feedback_t;

sc_error_t sc_persona_feedback_record(sc_allocator_t *alloc, const char *persona_name,
                                      size_t persona_name_len,
                                      const sc_persona_feedback_t *feedback);

sc_error_t sc_persona_feedback_apply(sc_allocator_t *alloc, const char *persona_name,
                                     size_t persona_name_len);

/* Message sampler — builds SQL / parses exports for persona creation pipeline */
sc_error_t sc_persona_sampler_imessage_query(char *buf, size_t cap, size_t *out_len, size_t limit);
sc_error_t sc_persona_sampler_imessage_conversation_query(const char *handle_id,
                                                         size_t handle_id_len, char *buf,
                                                         size_t cap, size_t *out_len,
                                                         size_t limit);

/* Raw message from a conversation sampler (used by example bank builder) */
typedef struct sc_sampler_raw_msg {
    const char *text;
    size_t text_len;
    int64_t timestamp;
    bool is_from_me;
} sc_sampler_raw_msg_t;

/* Build example bank entries from raw two-sided conversation messages */
sc_error_t sc_persona_sampler_build_examples(sc_allocator_t *alloc,
                                             const sc_sampler_raw_msg_t *msgs, size_t msg_count,
                                             sc_persona_example_t **out, size_t *out_count);

/* Auto-detect contact profile stats from conversation messages */
typedef struct sc_sampler_contact_stats {
    size_t their_msg_count;
    size_t my_msg_count;
    size_t avg_their_len;
    size_t avg_my_len;
    bool uses_emoji;
    bool sends_links;
    bool texts_in_bursts;
    bool prefers_short;
} sc_sampler_contact_stats_t;

sc_error_t sc_persona_sampler_detect_contact(sc_allocator_t *alloc,
                                             const sc_sampler_raw_msg_t *msgs, size_t msg_count,
                                             sc_sampler_contact_stats_t *out);

sc_error_t sc_persona_sampler_facebook_parse(const char *json, size_t json_len, char ***out,
                                             size_t *out_count);
sc_error_t sc_persona_sampler_gmail_parse(const char *json, size_t json_len, char ***out,
                                          size_t *out_count);

/* Provider analyzer — builds extraction prompt, parses provider JSON into partial persona */
sc_error_t sc_persona_analyzer_build_prompt(const char **messages, size_t msg_count,
                                            const char *channel, char *buf, size_t cap,
                                            size_t *out_len);
sc_error_t sc_persona_analyzer_parse_response(sc_allocator_t *alloc, const char *response,
                                              size_t resp_len, const char *channel,
                                              size_t channel_len, sc_persona_t *out);

/* Creator pipeline — merges partial personas into one */
sc_error_t sc_persona_creator_synthesize(sc_allocator_t *alloc, const sc_persona_t *partials,
                                         size_t count, const char *name, size_t name_len,
                                         sc_persona_t *out);
sc_error_t sc_persona_creator_write(sc_allocator_t *alloc, const sc_persona_t *persona);

/* CLI types and commands */
typedef enum {
    SC_PERSONA_ACTION_CREATE,
    SC_PERSONA_ACTION_UPDATE,
    SC_PERSONA_ACTION_SHOW,
    SC_PERSONA_ACTION_LIST,
    SC_PERSONA_ACTION_DELETE,
    SC_PERSONA_ACTION_VALIDATE,
    SC_PERSONA_ACTION_FEEDBACK_APPLY,
    SC_PERSONA_ACTION_DIFF,
    SC_PERSONA_ACTION_EXPORT,
    SC_PERSONA_ACTION_MERGE,
    SC_PERSONA_ACTION_IMPORT
} sc_persona_action_t;

typedef struct sc_persona_cli_args {
    sc_persona_action_t action;
    const char *name;
    const char *diff_name; /* second persona for diff action */
    bool from_imessage;
    bool from_gmail;
    bool from_facebook;
    bool interactive;
    const char *facebook_export_path;
    const char *gmail_export_path;
    const char *response_file;    /* --from-response <path> */
    const char *with_contact;     /* --with-contact <handle_id> for conversation extraction */
    const char **merge_sources;
    size_t merge_sources_count;
    const char *import_file; /* --from-file <path> or NULL for --from-stdin */
} sc_persona_cli_args_t;

sc_error_t sc_persona_cli_parse(int argc, const char **argv, sc_persona_cli_args_t *out);
sc_error_t sc_persona_cli_run(sc_allocator_t *alloc, const sc_persona_cli_args_t *args);

#endif /* SC_PERSONA_H */
