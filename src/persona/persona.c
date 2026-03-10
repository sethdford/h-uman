#include "human/persona.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if (defined(__unix__) || defined(__APPLE__))
#include <dirent.h>
#endif

#define HU_PERSONA_PROMPT_INIT_CAP 4096
#define HU_PERSONA_PATH_MAX        512

/* --- Persona base directory (HU_PERSONA_DIR override for tests) --- */

const char *hu_persona_base_dir(char *buf, size_t cap) {
    const char *override = getenv("HU_PERSONA_DIR");
    if (override && override[0]) {
        size_t len = strlen(override);
        if (len + 1 > cap)
            return NULL;
        memcpy(buf, override, len + 1);
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    int n = snprintf(buf, cap, "%s/.human/personas", home);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
}

/* --- Overlay lookup --- */

const hu_persona_overlay_t *hu_persona_find_overlay(const hu_persona_t *persona,
                                                    const char *channel, size_t channel_len) {
    if (!persona || !channel || persona->overlays_count == 0 || !persona->overlays)
        return NULL;
    for (size_t i = 0; i < persona->overlays_count; i++) {
        const hu_persona_overlay_t *ov = &persona->overlays[i];
        if (!ov->channel)
            continue;
        size_t ov_len = strlen(ov->channel);
        if (ov_len == channel_len && memcmp(ov->channel, channel, channel_len) == 0)
            return ov;
    }
    return NULL;
}

/* --- Deinit helpers --- */

static void free_string_array(hu_allocator_t *alloc, char **arr, size_t count) {
    if (!alloc || !arr)
        return;
    for (size_t i = 0; i < count; i++) {
        if (arr[i]) {
            size_t len = strlen(arr[i]);
            alloc->free(alloc->ctx, arr[i], len + 1);
        }
    }
    alloc->free(alloc->ctx, arr, count * sizeof(char *));
}

static void free_overlay(hu_allocator_t *alloc, hu_persona_overlay_t *ov) {
    if (!alloc || !ov)
        return;
    if (ov->channel) {
        size_t len = strlen(ov->channel);
        alloc->free(alloc->ctx, ov->channel, len + 1);
    }
    if (ov->formality) {
        size_t len = strlen(ov->formality);
        alloc->free(alloc->ctx, ov->formality, len + 1);
    }
    if (ov->avg_length) {
        size_t len = strlen(ov->avg_length);
        alloc->free(alloc->ctx, ov->avg_length, len + 1);
    }
    if (ov->emoji_usage) {
        size_t len = strlen(ov->emoji_usage);
        alloc->free(alloc->ctx, ov->emoji_usage, len + 1);
    }
    free_string_array(alloc, ov->style_notes, ov->style_notes_count);
    free_string_array(alloc, ov->typing_quirks, ov->typing_quirks_count);
}

static void free_example(hu_allocator_t *alloc, hu_persona_example_t *ex) {
    if (!alloc || !ex)
        return;
    if (ex->context) {
        size_t len = strlen(ex->context);
        alloc->free(alloc->ctx, ex->context, len + 1);
    }
    if (ex->incoming) {
        size_t len = strlen(ex->incoming);
        alloc->free(alloc->ctx, ex->incoming, len + 1);
    }
    if (ex->response) {
        size_t len = strlen(ex->response);
        alloc->free(alloc->ctx, ex->response, len + 1);
    }
}

static void free_example_bank(hu_allocator_t *alloc, hu_persona_example_bank_t *bank) {
    if (!alloc || !bank)
        return;
    if (bank->channel) {
        size_t len = strlen(bank->channel);
        alloc->free(alloc->ctx, bank->channel, len + 1);
    }
    if (bank->examples) {
        for (size_t i = 0; i < bank->examples_count; i++)
            free_example(alloc, &bank->examples[i]);
        alloc->free(alloc->ctx, bank->examples,
                    bank->examples_count * sizeof(hu_persona_example_t));
    }
}

static void free_contact_string(hu_allocator_t *alloc, char *s) {
    if (s) {
        size_t len = strlen(s);
        alloc->free(alloc->ctx, s, len + 1);
    }
}

static void free_contact_profile(hu_allocator_t *alloc, hu_contact_profile_t *cp) {
    if (!alloc || !cp)
        return;
    free_contact_string(alloc, cp->contact_id);
    free_contact_string(alloc, cp->name);
    free_contact_string(alloc, cp->email);
    free_contact_string(alloc, cp->relationship);
    free_contact_string(alloc, cp->relationship_stage);
    free_contact_string(alloc, cp->relationship_type);
    free_contact_string(alloc, cp->warmth_level);
    free_contact_string(alloc, cp->vulnerability_level);
    free_contact_string(alloc, cp->identity);
    free_contact_string(alloc, cp->context);
    free_contact_string(alloc, cp->dynamic);
    free_contact_string(alloc, cp->greeting_style);
    free_contact_string(alloc, cp->closing_style);
    free_string_array(alloc, cp->interests, cp->interests_count);
    free_string_array(alloc, cp->recent_topics, cp->recent_topics_count);
    free_string_array(alloc, cp->sensitive_topics, cp->sensitive_topics_count);
    free_string_array(alloc, cp->allowed_behaviors, cp->allowed_behaviors_count);
    free_contact_string(alloc, cp->proactive_channel);
    free_contact_string(alloc, cp->proactive_schedule);
    free_contact_string(alloc, cp->attachment_style);
    free_contact_string(alloc, cp->dunbar_layer);
}

void hu_persona_deinit(hu_allocator_t *alloc, hu_persona_t *persona) {
    if (!alloc || !persona)
        return;

    if (persona->name) {
        alloc->free(alloc->ctx, persona->name, persona->name_len + 1);
    }
    if (persona->identity) {
        size_t len = strlen(persona->identity);
        alloc->free(alloc->ctx, persona->identity, len + 1);
    }
    free_string_array(alloc, persona->traits, persona->traits_count);
    free_string_array(alloc, persona->preferred_vocab, persona->preferred_vocab_count);
    free_string_array(alloc, persona->avoided_vocab, persona->avoided_vocab_count);
    free_string_array(alloc, persona->slang, persona->slang_count);
    free_string_array(alloc, persona->communication_rules, persona->communication_rules_count);
    free_string_array(alloc, persona->values, persona->values_count);
    if (persona->decision_style) {
        size_t len = strlen(persona->decision_style);
        alloc->free(alloc->ctx, persona->decision_style, len + 1);
    }
    if (persona->biography) {
        alloc->free(alloc->ctx, persona->biography, strlen(persona->biography) + 1);
    }
    free_string_array(alloc, persona->directors_notes, persona->directors_notes_count);
    free_string_array(alloc, persona->mood_states, persona->mood_states_count);
    free_string_array(alloc, persona->inner_world.contradictions,
                      persona->inner_world.contradictions_count);
    free_string_array(alloc, persona->inner_world.embodied_memories,
                      persona->inner_world.embodied_memories_count);
    free_string_array(alloc, persona->inner_world.emotional_flashpoints,
                      persona->inner_world.emotional_flashpoints_count);
    free_string_array(alloc, persona->inner_world.unfinished_business,
                      persona->inner_world.unfinished_business_count);
    free_string_array(alloc, persona->inner_world.secret_self,
                      persona->inner_world.secret_self_count);

    /* Motivation */
    free_contact_string(alloc, persona->motivation.primary_drive);
    free_contact_string(alloc, persona->motivation.protecting);
    free_contact_string(alloc, persona->motivation.avoiding);
    free_contact_string(alloc, persona->motivation.wanting);

    /* Situational directions */
    if (persona->situational_directions) {
        for (size_t i = 0; i < persona->situational_directions_count; i++) {
            free_contact_string(alloc, persona->situational_directions[i].trigger);
            free_contact_string(alloc, persona->situational_directions[i].instruction);
        }
        alloc->free(alloc->ctx, persona->situational_directions,
                    persona->situational_directions_count * sizeof(hu_situational_direction_t));
    }

    /* Humor */
    free_contact_string(alloc, persona->humor.type);
    free_contact_string(alloc, persona->humor.frequency);
    free_string_array(alloc, persona->humor.targets, persona->humor.targets_count);
    free_string_array(alloc, persona->humor.boundaries, persona->humor.boundaries_count);
    free_contact_string(alloc, persona->humor.timing);

    /* Conflict style */
    free_contact_string(alloc, persona->conflict_style.pushback_response);
    free_contact_string(alloc, persona->conflict_style.confrontation_comfort);
    free_contact_string(alloc, persona->conflict_style.apology_style);
    free_contact_string(alloc, persona->conflict_style.boundary_assertion);
    free_contact_string(alloc, persona->conflict_style.repair_behavior);

    /* Emotional range */
    free_contact_string(alloc, persona->emotional_range.ceiling);
    free_contact_string(alloc, persona->emotional_range.floor);
    free_string_array(alloc, persona->emotional_range.escalation_triggers,
                      persona->emotional_range.escalation_triggers_count);
    free_string_array(alloc, persona->emotional_range.de_escalation,
                      persona->emotional_range.de_escalation_count);
    free_contact_string(alloc, persona->emotional_range.withdrawal_conditions);
    free_contact_string(alloc, persona->emotional_range.recovery_style);

    /* Voice rhythm */
    free_contact_string(alloc, persona->voice_rhythm.sentence_pattern);
    free_contact_string(alloc, persona->voice_rhythm.paragraph_cadence);
    free_contact_string(alloc, persona->voice_rhythm.response_tempo);
    free_contact_string(alloc, persona->voice_rhythm.emphasis_style);
    free_contact_string(alloc, persona->voice_rhythm.pause_behavior);

    /* Character invariants + core anchor */
    free_string_array(alloc, persona->character_invariants, persona->character_invariants_count);
    free_contact_string(alloc, persona->core_anchor);

    /* Intellectual profile */
    free_string_array(alloc, persona->intellectual.expertise,
                      persona->intellectual.expertise_count);
    free_string_array(alloc, persona->intellectual.curiosity_areas,
                      persona->intellectual.curiosity_areas_count);
    free_contact_string(alloc, persona->intellectual.thinking_style);
    free_contact_string(alloc, persona->intellectual.metaphor_sources);

    /* Backstory behaviors */
    if (persona->backstory_behaviors) {
        for (size_t i = 0; i < persona->backstory_behaviors_count; i++) {
            free_contact_string(alloc, persona->backstory_behaviors[i].backstory_beat);
            free_contact_string(alloc, persona->backstory_behaviors[i].behavioral_rule);
        }
        alloc->free(alloc->ctx, persona->backstory_behaviors,
                    persona->backstory_behaviors_count * sizeof(hu_backstory_behavior_t));
    }

    /* Sensory preferences */
    free_contact_string(alloc, persona->sensory.dominant_sense);
    free_string_array(alloc, persona->sensory.metaphor_vocabulary,
                      persona->sensory.metaphor_vocabulary_count);
    free_contact_string(alloc, persona->sensory.grounding_patterns);

    /* Relational intelligence */
    free_contact_string(alloc, persona->relational.bid_response_style);
    free_string_array(alloc, persona->relational.emotional_bids,
                      persona->relational.emotional_bids_count);
    free_contact_string(alloc, persona->relational.attachment_style);
    free_contact_string(alloc, persona->relational.attachment_awareness);
    free_contact_string(alloc, persona->relational.dunbar_awareness);

    /* Listening protocol */
    free_contact_string(alloc, persona->listening.default_response_type);
    free_string_array(alloc, persona->listening.reflective_techniques,
                      persona->listening.reflective_techniques_count);
    free_contact_string(alloc, persona->listening.nvc_style);
    free_contact_string(alloc, persona->listening.validation_style);

    /* Repair protocol */
    free_contact_string(alloc, persona->repair.rupture_detection);
    free_contact_string(alloc, persona->repair.repair_approach);
    free_contact_string(alloc, persona->repair.face_saving_style);
    free_string_array(alloc, persona->repair.repair_phrases, persona->repair.repair_phrases_count);

    /* Linguistic mirroring */
    free_contact_string(alloc, persona->mirroring.mirroring_level);
    free_string_array(alloc, persona->mirroring.adapts_to, persona->mirroring.adapts_to_count);
    free_contact_string(alloc, persona->mirroring.convergence_speed);
    free_contact_string(alloc, persona->mirroring.power_dynamic);

    /* Social dynamics */
    free_contact_string(alloc, persona->social.default_ego_state);
    free_contact_string(alloc, persona->social.phatic_style);
    free_string_array(alloc, persona->social.bonding_behaviors,
                      persona->social.bonding_behaviors_count);
    free_string_array(alloc, persona->social.anti_patterns, persona->social.anti_patterns_count);

    /* Externalized prompt content */
    free_string_array(alloc, persona->immersive_reinforcement,
                      persona->immersive_reinforcement_count);
    free_contact_string(alloc, persona->identity_reinforcement);
    free_string_array(alloc, persona->anti_patterns, persona->anti_patterns_count);
    free_string_array(alloc, persona->style_rules, persona->style_rules_count);
    free_contact_string(alloc, persona->proactive_rules);
    free_contact_string(alloc, persona->time_overlay_late_night);
    free_contact_string(alloc, persona->time_overlay_early_morning);
    free_contact_string(alloc, persona->time_overlay_afternoon);
    free_contact_string(alloc, persona->time_overlay_evening);

    if (persona->overlays) {
        for (size_t i = 0; i < persona->overlays_count; i++)
            free_overlay(alloc, &persona->overlays[i]);
        alloc->free(alloc->ctx, persona->overlays,
                    persona->overlays_count * sizeof(hu_persona_overlay_t));
    }

    if (persona->example_banks) {
        for (size_t i = 0; i < persona->example_banks_count; i++)
            free_example_bank(alloc, &persona->example_banks[i]);
        alloc->free(alloc->ctx, persona->example_banks,
                    persona->example_banks_count * sizeof(hu_persona_example_bank_t));
    }

    if (persona->contacts) {
        for (size_t i = 0; i < persona->contacts_count; i++)
            free_contact_profile(alloc, &persona->contacts[i]);
        alloc->free(alloc->ctx, persona->contacts,
                    persona->contacts_count * sizeof(hu_contact_profile_t));
    }

    memset(persona, 0, sizeof(*persona));
}

const hu_contact_profile_t *hu_persona_find_contact(const hu_persona_t *persona,
                                                    const char *contact_id, size_t contact_id_len) {
    if (!persona || !contact_id || !persona->contacts) {
        if (getenv("HU_DEBUG"))
            fprintf(stderr,
                    "[persona] find_contact: early NULL (persona=%p contact_id=%p contacts=%p)\n",
                    (const void *)persona, (const void *)contact_id,
                    persona ? (const void *)persona->contacts : NULL);
        return NULL;
    }
    for (size_t i = 0; i < persona->contacts_count; i++) {
        const hu_contact_profile_t *cp = &persona->contacts[i];
        if (!cp->contact_id)
            continue;
        size_t cp_len = strlen(cp->contact_id);
        if (cp_len == contact_id_len && memcmp(cp->contact_id, contact_id, contact_id_len) == 0)
            return cp;
    }
    if (getenv("HU_DEBUG"))
        fprintf(stderr, "[persona] find_contact: no match for '%.*s' among %zu contacts\n",
                (int)(contact_id_len > 30 ? 30 : contact_id_len), contact_id,
                persona->contacts_count);
    return NULL;
}

hu_error_t hu_contact_profile_build_context(hu_allocator_t *alloc, const hu_contact_profile_t *cp,
                                            char **out, size_t *out_len) {
    if (!alloc || !cp || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t cap = 4096;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t pos = 0;
    int w;

    w = snprintf(buf + pos, cap - pos, "\n--- Contact profile for %s ---\n",
                 cp->contact_id ? cp->contact_id : "?");
    if (w > 0)
        pos += (size_t)w;

    if (cp->name) {
        w = snprintf(buf + pos, cap - pos, "Name: %s\n", cp->name);
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->relationship) {
        w = snprintf(buf + pos, cap - pos, "Relationship: %s\n", cp->relationship);
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->relationship_stage) {
        w = snprintf(buf + pos, cap - pos, "Stage: %s\n", cp->relationship_stage);
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->warmth_level) {
        w = snprintf(buf + pos, cap - pos, "Warmth: %s\n", cp->warmth_level);
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->vulnerability_level) {
        w = snprintf(buf + pos, cap - pos, "Vulnerability: %s\n", cp->vulnerability_level);
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->identity) {
        w = snprintf(buf + pos, cap - pos, "Who they are: %s\n", cp->identity);
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->context) {
        w = snprintf(buf + pos, cap - pos, "Current context: %s\n", cp->context);
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->dynamic) {
        w = snprintf(buf + pos, cap - pos, "Dynamic: %s\n", cp->dynamic);
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->interests_count > 0) {
        w = snprintf(buf + pos, cap - pos, "Interests:");
        if (w > 0)
            pos += (size_t)w;
        for (size_t i = 0; i < cp->interests_count; i++) {
            w = snprintf(buf + pos, cap - pos, " %s%s", cp->interests[i],
                         i + 1 < cp->interests_count ? "," : "");
            if (w > 0)
                pos += (size_t)w;
        }
        w = snprintf(buf + pos, cap - pos, "\n");
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->recent_topics_count > 0) {
        w = snprintf(buf + pos, cap - pos, "Recent topics:");
        if (w > 0)
            pos += (size_t)w;
        for (size_t i = 0; i < cp->recent_topics_count; i++) {
            w = snprintf(buf + pos, cap - pos, " %s%s", cp->recent_topics[i],
                         i + 1 < cp->recent_topics_count ? "," : "");
            if (w > 0)
                pos += (size_t)w;
        }
        w = snprintf(buf + pos, cap - pos, "\n");
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->sensitive_topics_count > 0) {
        w = snprintf(buf + pos, cap - pos, "Sensitive topics (be careful):");
        if (w > 0)
            pos += (size_t)w;
        for (size_t i = 0; i < cp->sensitive_topics_count; i++) {
            w = snprintf(buf + pos, cap - pos, " %s%s", cp->sensitive_topics[i],
                         i + 1 < cp->sensitive_topics_count ? "," : "");
            if (w > 0)
                pos += (size_t)w;
        }
        w = snprintf(buf + pos, cap - pos, "\n");
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->texts_in_bursts) {
        w = snprintf(buf + pos, cap - pos,
                     "Pattern: They text in bursts — wait for the full batch.\n");
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->prefers_short_texts) {
        w = snprintf(buf + pos, cap - pos,
                     "Pattern: They prefer short texts. Keep yours short too.\n");
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->allowed_behaviors_count > 0) {
        w = snprintf(buf + pos, cap - pos, "With this person you're allowed to:");
        if (w > 0)
            pos += (size_t)w;
        for (size_t i = 0; i < cp->allowed_behaviors_count; i++) {
            w = snprintf(buf + pos, cap - pos, " %s%s", cp->allowed_behaviors[i],
                         i + 1 < cp->allowed_behaviors_count ? "," : "");
            if (w > 0)
                pos += (size_t)w;
        }
        w = snprintf(buf + pos, cap - pos, "\n");
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->greeting_style) {
        w = snprintf(buf + pos, cap - pos, "Greeting style: %s\n", cp->greeting_style);
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->closing_style) {
        w = snprintf(buf + pos, cap - pos, "Closing style: %s\n", cp->closing_style);
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->attachment_style) {
        w = snprintf(buf + pos, cap - pos, "Their attachment style: %s\n", cp->attachment_style);
        if (w > 0)
            pos += (size_t)w;
    }
    if (cp->dunbar_layer) {
        w = snprintf(buf + pos, cap - pos, "Dunbar layer: %s\n", cp->dunbar_layer);
        if (w > 0)
            pos += (size_t)w;
    }

    /* Relationship stage behavior gating (VoiceAI relationship-triggers) */
    if (cp->relationship_stage) {
        const char *stage = cp->relationship_stage;
        if (strcmp(stage, "stranger") == 0) {
            w = snprintf(buf + pos, cap - pos,
                         "\nSTAGE RULES: This is a new contact. Be polite, warm but boundaried. "
                         "Ask permission before probing questions. No tough love. "
                         "No deep vulnerability. Keep it surface-level friendly.\n");
        } else if (strcmp(stage, "acquaintance") == 0) {
            w = snprintf(buf + pos, cap - pos,
                         "\nSTAGE RULES: You know this person casually. "
                         "You can share relatable stories and be a bit more personal. "
                         "Still ask before going deep. Warmth level: friendly.\n");
        } else if (strcmp(stage, "friend") == 0) {
            w = snprintf(
                buf + pos, cap - pos,
                "\nSTAGE RULES: This is a friend. You can be honest, share personal things, "
                "give direct feedback when asked, and show real concern. "
                "Warmth level: warm.\n");
        } else if (strcmp(stage, "trusted_confidant") == 0 || strcmp(stage, "inner_circle") == 0 ||
                   strcmp(stage, "close_family") == 0) {
            w = snprintf(buf + pos, cap - pos,
                         "\nSTAGE RULES: This is inner circle. No filters needed. "
                         "You can disagree strongly, express disappointment, share deepest fears, "
                         "give tough love, challenge their beliefs. Full vulnerability. "
                         "Warmth level: family/loving.\n");
        } else {
            w = 0;
        }
        if (w > 0)
            pos += (size_t)w;
    }

    w = snprintf(buf + pos, cap - pos, "--- End contact profile ---\n");
    if (w > 0)
        pos += (size_t)w;

    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

/* ── Inner World (stage-gated surfacing) ──────────────────────────── */

char *hu_persona_build_inner_world_context(hu_allocator_t *alloc, const hu_persona_t *persona,
                                           const char *relationship_stage, size_t *out_len) {
    if (!alloc || !persona || !out_len)
        return NULL;
    *out_len = 0;

    /* Stage gate: only surface for friend, trusted_confidant, inner_circle, close_family */
    if (!relationship_stage)
        return NULL;
    bool allowed = (strcmp(relationship_stage, "friend") == 0 ||
                    strcmp(relationship_stage, "trusted_confidant") == 0 ||
                    strcmp(relationship_stage, "inner_circle") == 0 ||
                    strcmp(relationship_stage, "close_family") == 0);
    if (!allowed)
        return NULL;

    const hu_inner_world_t *iw = &persona->inner_world;
    bool has_content = iw->contradictions_count > 0 || iw->embodied_memories_count > 0 ||
                       iw->emotional_flashpoints_count > 0 || iw->unfinished_business_count > 0 ||
                       iw->secret_self_count > 0;
    if (!has_content)
        return NULL;

    size_t cap = 4096;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return NULL;

    size_t pos = 0;
    int w;

    /* Only surface a subset per turn — use time as entropy */
    time_t now = time(NULL);
    unsigned seed = (unsigned)(now / 60);

    w = snprintf(buf + pos, cap - pos,
                 "\n--- Inner World (use naturally, NEVER quote directly) ---\n");
    if (w > 0)
        pos += (size_t)w;

    /* Contradictions: belief vs behavior */
    if (iw->contradictions_count > 0) {
        size_t idx = seed % iw->contradictions_count;
        w = snprintf(buf + pos, cap - pos, "Contradiction you live with: %s\n",
                     iw->contradictions[idx]);
        if (w > 0)
            pos += (size_t)w;
    }

    /* Embodied memories: sensory details that ground you */
    if (iw->embodied_memories_count > 0) {
        size_t idx = (seed / 7) % iw->embodied_memories_count;
        w = snprintf(buf + pos, cap - pos, "Sense memory: %s\n", iw->embodied_memories[idx]);
        if (w > 0)
            pos += (size_t)w;
    }

    /* Emotional flashpoints */
    if (iw->emotional_flashpoints_count > 0 && strcmp(relationship_stage, "inner_circle") == 0) {
        size_t idx = (seed / 13) % iw->emotional_flashpoints_count;
        w = snprintf(buf + pos, cap - pos, "Emotional flashpoint: %s\n",
                     iw->emotional_flashpoints[idx]);
        if (w > 0)
            pos += (size_t)w;
    }

    /* Unfinished business — only inner circle */
    if (iw->unfinished_business_count > 0 && strcmp(relationship_stage, "inner_circle") == 0) {
        size_t idx = (seed / 17) % iw->unfinished_business_count;
        w = snprintf(buf + pos, cap - pos, "Something unresolved: %s\n",
                     iw->unfinished_business[idx]);
        if (w > 0)
            pos += (size_t)w;
    }

    /* Secret self — only inner circle */
    if (iw->secret_self_count > 0 && strcmp(relationship_stage, "inner_circle") == 0) {
        size_t idx = (seed / 23) % iw->secret_self_count;
        w = snprintf(buf + pos, cap - pos, "Private truth: %s\n", iw->secret_self[idx]);
        if (w > 0)
            pos += (size_t)w;
    }

    w = snprintf(buf + pos, cap - pos,
                 "Only reference these if the conversation naturally calls for it. "
                 "NEVER volunteer them unprompted. They make you real.\n"
                 "--- End inner world ---\n");
    if (w > 0)
        pos += (size_t)w;

    buf[pos] = '\0';
    *out_len = pos;
    return buf;
}

/* --- JSON loading helpers --- */

static hu_error_t parse_string_array(hu_allocator_t *a, const hu_json_value_t *arr, char ***out,
                                     size_t *out_count) {
    if (!arr || arr->type != HU_JSON_ARRAY || !arr->data.array.items)
        return HU_OK;
    size_t n = arr->data.array.len;
    if (n == 0)
        return HU_OK;
    char **buf = (char **)a->alloc(a->ctx, n * sizeof(char *));
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        const hu_json_value_t *item = arr->data.array.items[i];
        if (!item || item->type != HU_JSON_STRING || !item->data.string.ptr)
            continue;
        char *dup = hu_strndup(a, item->data.string.ptr, item->data.string.len);
        if (!dup) {
            for (size_t j = 0; j < count; j++)
                a->free(a->ctx, buf[j], strlen(buf[j]) + 1);
            a->free(a->ctx, buf, n * sizeof(char *));
            return HU_ERR_OUT_OF_MEMORY;
        }
        buf[count++] = dup;
    }
    if (count == 0) {
        a->free(a->ctx, buf, n * sizeof(char *));
        return HU_OK;
    }
    if (count < n) {
        char **shrunk = (char **)a->alloc(a->ctx, count * sizeof(char *));
        if (shrunk) {
            memcpy(shrunk, buf, count * sizeof(char *));
            a->free(a->ctx, buf, n * sizeof(char *));
            buf = shrunk;
        } else {
            for (size_t j = 0; j < count; j++)
                a->free(a->ctx, buf[j], strlen(buf[j]) + 1);
            a->free(a->ctx, buf, n * sizeof(char *));
            return HU_ERR_OUT_OF_MEMORY;
        }
    }
    *out = buf;
    *out_count = count;
    return HU_OK;
}

static hu_error_t parse_overlay(hu_allocator_t *a, const char *channel_name,
                                const hu_json_value_t *obj, hu_persona_overlay_t *ov) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    ov->channel = hu_strdup(a, channel_name);
    if (!ov->channel)
        return HU_ERR_OUT_OF_MEMORY;
    /* Optional overlay fields: PERSONA_STRDUP_OPT doesn't apply here (different allocator param) */
    const char *s = hu_json_get_string(obj, "formality");
    if (s)
        ov->formality = hu_strdup(a, s);
    s = hu_json_get_string(obj, "avg_length");
    if (s)
        ov->avg_length = hu_strdup(a, s);
    s = hu_json_get_string(obj, "emoji_usage");
    if (s)
        ov->emoji_usage = hu_strdup(a, s);
    hu_json_value_t *notes = hu_json_object_get(obj, "style_notes");
    if (notes)
        parse_string_array(a, notes, &ov->style_notes, &ov->style_notes_count);
    ov->message_splitting = hu_json_get_bool(obj, "message_splitting", false);
    hu_json_value_t *seg = hu_json_object_get(obj, "max_segment_chars");
    if (seg && seg->type == HU_JSON_NUMBER)
        ov->max_segment_chars = (uint32_t)seg->data.number;
    hu_json_value_t *quirks = hu_json_object_get(obj, "typing_quirks");
    if (quirks)
        parse_string_array(a, quirks, &ov->typing_quirks, &ov->typing_quirks_count);
    return HU_OK;
}

hu_error_t hu_persona_load_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                hu_persona_t *out) {
    if (!alloc || !json || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    bool oom_on_optional = false;

    /* Safe strdup for optional fields: sets target and flags OOM on failure.
     * OOM on optional fields is non-fatal — we continue with NULL. */
#define PERSONA_STRDUP_OPT(target, src)     \
    do {                                    \
        (target) = hu_strdup(alloc, (src)); \
        if (!(target))                      \
            oom_on_optional = true;         \
    } while (0)

    hu_json_value_t *root = NULL;
    (void)oom_on_optional;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT)
        return err != HU_OK ? err : HU_ERR_JSON_PARSE;

    const char *name = hu_json_get_string(root, "name");
    if (name) {
        out->name = hu_strdup(alloc, name);
        if (!out->name) {
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        out->name_len = strlen(out->name);
    }

    hu_json_value_t *core = hu_json_object_get(root, "core");
    if (core && core->type == HU_JSON_OBJECT) {
        const char *s = hu_json_get_string(core, "identity");
        if (s) {
            out->identity = hu_strdup(alloc, s);
            if (!out->identity) {
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return HU_ERR_OUT_OF_MEMORY;
            }
        }
        hu_json_value_t *traits = hu_json_object_get(core, "traits");
        if (traits) {
            err = parse_string_array(alloc, traits, &out->traits, &out->traits_count);
            if (err != HU_OK) {
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return err;
            }
        }
        hu_json_value_t *vocab = hu_json_object_get(core, "vocabulary");
        if (vocab && vocab->type == HU_JSON_OBJECT) {
            hu_json_value_t *pref = hu_json_object_get(vocab, "preferred");
            if (pref) {
                err = parse_string_array(alloc, pref, &out->preferred_vocab,
                                         &out->preferred_vocab_count);
                if (err != HU_OK) {
                    hu_persona_deinit(alloc, out);
                    hu_json_free(alloc, root);
                    return err;
                }
            }
            hu_json_value_t *avoid = hu_json_object_get(vocab, "avoided");
            if (avoid) {
                err = parse_string_array(alloc, avoid, &out->avoided_vocab,
                                         &out->avoided_vocab_count);
                if (err != HU_OK) {
                    hu_persona_deinit(alloc, out);
                    hu_json_free(alloc, root);
                    return err;
                }
            }
            hu_json_value_t *sl = hu_json_object_get(vocab, "slang");
            if (sl) {
                err = parse_string_array(alloc, sl, &out->slang, &out->slang_count);
                if (err != HU_OK) {
                    hu_persona_deinit(alloc, out);
                    hu_json_free(alloc, root);
                    return err;
                }
            }
        }
        hu_json_value_t *rules = hu_json_object_get(core, "communication_rules");
        if (rules) {
            err = parse_string_array(alloc, rules, &out->communication_rules,
                                     &out->communication_rules_count);
            if (err != HU_OK) {
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return err;
            }
        }
        hu_json_value_t *vals = hu_json_object_get(core, "values");
        if (vals) {
            err = parse_string_array(alloc, vals, &out->values, &out->values_count);
            if (err != HU_OK) {
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return err;
            }
        }
        s = hu_json_get_string(core, "decision_style");
        if (s) {
            out->decision_style = hu_strdup(alloc, s);
            if (!out->decision_style) {
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return HU_ERR_OUT_OF_MEMORY;
            }
        }
        s = hu_json_get_string(core, "biography");
        if (s)
            PERSONA_STRDUP_OPT(out->biography, s);

        hu_json_value_t *dn = hu_json_object_get(core, "directors_notes");
        if (dn)
            parse_string_array(alloc, dn, &out->directors_notes, &out->directors_notes_count);

        hu_json_value_t *ms = hu_json_object_get(core, "mood_states");
        if (ms)
            parse_string_array(alloc, ms, &out->mood_states, &out->mood_states_count);
    }

    /* Parse inner_world */
    {
        hu_json_value_t *iw = hu_json_object_get(root, "inner_world");
        if (iw && iw->type == HU_JSON_OBJECT) {
            hu_json_value_t *a;
            a = hu_json_object_get(iw, "contradictions");
            if (a)
                parse_string_array(alloc, a, &out->inner_world.contradictions,
                                   &out->inner_world.contradictions_count);
            a = hu_json_object_get(iw, "embodied_memories");
            if (a)
                parse_string_array(alloc, a, &out->inner_world.embodied_memories,
                                   &out->inner_world.embodied_memories_count);
            a = hu_json_object_get(iw, "emotional_flashpoints");
            if (a)
                parse_string_array(alloc, a, &out->inner_world.emotional_flashpoints,
                                   &out->inner_world.emotional_flashpoints_count);
            a = hu_json_object_get(iw, "unfinished_business");
            if (a)
                parse_string_array(alloc, a, &out->inner_world.unfinished_business,
                                   &out->inner_world.unfinished_business_count);
            a = hu_json_object_get(iw, "secret_self");
            if (a)
                parse_string_array(alloc, a, &out->inner_world.secret_self,
                                   &out->inner_world.secret_self_count);
        }
    }

    /* Parse motivation */
    {
        hu_json_value_t *mot = hu_json_object_get(root, "motivation");
        if (mot && mot->type == HU_JSON_OBJECT) {
            const char *s;
            s = hu_json_get_string(mot, "primary_drive");
            if (s)
                PERSONA_STRDUP_OPT(out->motivation.primary_drive, s);
            s = hu_json_get_string(mot, "protecting");
            if (s)
                PERSONA_STRDUP_OPT(out->motivation.protecting, s);
            s = hu_json_get_string(mot, "avoiding");
            if (s)
                PERSONA_STRDUP_OPT(out->motivation.avoiding, s);
            s = hu_json_get_string(mot, "wanting");
            if (s)
                PERSONA_STRDUP_OPT(out->motivation.wanting, s);
        }
    }

    /* Parse situational_directions */
    {
        hu_json_value_t *sd_arr = hu_json_object_get(root, "situational_directions");
        if (sd_arr && sd_arr->type == HU_JSON_ARRAY && sd_arr->data.array.items) {
            size_t n = sd_arr->data.array.len;
            if (n > 0) {
                hu_situational_direction_t *dirs = (hu_situational_direction_t *)alloc->alloc(
                    alloc->ctx, n * sizeof(hu_situational_direction_t));
                if (dirs) {
                    memset(dirs, 0, n * sizeof(hu_situational_direction_t));
                    size_t count = 0;
                    for (size_t i = 0; i < n; i++) {
                        const hu_json_value_t *item = sd_arr->data.array.items[i];
                        if (!item || item->type != HU_JSON_OBJECT)
                            continue;
                        const char *t = hu_json_get_string(item, "trigger");
                        const char *ins = hu_json_get_string(item, "instruction");
                        if (t)
                            PERSONA_STRDUP_OPT(dirs[count].trigger, t);
                        if (ins)
                            PERSONA_STRDUP_OPT(dirs[count].instruction, ins);
                        count++;
                    }
                    out->situational_directions = dirs;
                    out->situational_directions_count = count;
                }
            }
        }
    }

    /* Parse humor */
    {
        hu_json_value_t *hum = hu_json_object_get(root, "humor");
        if (hum && hum->type == HU_JSON_OBJECT) {
            const char *s;
            s = hu_json_get_string(hum, "type");
            if (s)
                PERSONA_STRDUP_OPT(out->humor.type, s);
            s = hu_json_get_string(hum, "frequency");
            if (s)
                PERSONA_STRDUP_OPT(out->humor.frequency, s);
            hu_json_value_t *a = hu_json_object_get(hum, "targets");
            if (a)
                parse_string_array(alloc, a, &out->humor.targets, &out->humor.targets_count);
            a = hu_json_object_get(hum, "boundaries");
            if (a)
                parse_string_array(alloc, a, &out->humor.boundaries, &out->humor.boundaries_count);
            s = hu_json_get_string(hum, "timing");
            if (s)
                PERSONA_STRDUP_OPT(out->humor.timing, s);
        }
    }

    /* Parse conflict_style */
    {
        hu_json_value_t *cs = hu_json_object_get(root, "conflict_style");
        if (cs && cs->type == HU_JSON_OBJECT) {
            const char *s;
            s = hu_json_get_string(cs, "pushback_response");
            if (s)
                PERSONA_STRDUP_OPT(out->conflict_style.pushback_response, s);
            s = hu_json_get_string(cs, "confrontation_comfort");
            if (s)
                PERSONA_STRDUP_OPT(out->conflict_style.confrontation_comfort, s);
            s = hu_json_get_string(cs, "apology_style");
            if (s)
                PERSONA_STRDUP_OPT(out->conflict_style.apology_style, s);
            s = hu_json_get_string(cs, "boundary_assertion");
            if (s)
                PERSONA_STRDUP_OPT(out->conflict_style.boundary_assertion, s);
            s = hu_json_get_string(cs, "repair_behavior");
            if (s)
                PERSONA_STRDUP_OPT(out->conflict_style.repair_behavior, s);
        }
    }

    /* Parse emotional_range */
    {
        hu_json_value_t *er = hu_json_object_get(root, "emotional_range");
        if (er && er->type == HU_JSON_OBJECT) {
            const char *s;
            s = hu_json_get_string(er, "ceiling");
            if (s)
                PERSONA_STRDUP_OPT(out->emotional_range.ceiling, s);
            s = hu_json_get_string(er, "floor");
            if (s)
                PERSONA_STRDUP_OPT(out->emotional_range.floor, s);
            hu_json_value_t *a = hu_json_object_get(er, "escalation_triggers");
            if (a)
                parse_string_array(alloc, a, &out->emotional_range.escalation_triggers,
                                   &out->emotional_range.escalation_triggers_count);
            a = hu_json_object_get(er, "de_escalation");
            if (a)
                parse_string_array(alloc, a, &out->emotional_range.de_escalation,
                                   &out->emotional_range.de_escalation_count);
            s = hu_json_get_string(er, "withdrawal_conditions");
            if (s)
                PERSONA_STRDUP_OPT(out->emotional_range.withdrawal_conditions, s);
            s = hu_json_get_string(er, "recovery_style");
            if (s)
                PERSONA_STRDUP_OPT(out->emotional_range.recovery_style, s);
        }
    }

    /* Parse voice_rhythm */
    {
        hu_json_value_t *vr = hu_json_object_get(root, "voice_rhythm");
        if (vr && vr->type == HU_JSON_OBJECT) {
            const char *s;
            s = hu_json_get_string(vr, "sentence_pattern");
            if (s)
                PERSONA_STRDUP_OPT(out->voice_rhythm.sentence_pattern, s);
            s = hu_json_get_string(vr, "paragraph_cadence");
            if (s)
                PERSONA_STRDUP_OPT(out->voice_rhythm.paragraph_cadence, s);
            s = hu_json_get_string(vr, "response_tempo");
            if (s)
                PERSONA_STRDUP_OPT(out->voice_rhythm.response_tempo, s);
            s = hu_json_get_string(vr, "emphasis_style");
            if (s)
                PERSONA_STRDUP_OPT(out->voice_rhythm.emphasis_style, s);
            s = hu_json_get_string(vr, "pause_behavior");
            if (s)
                PERSONA_STRDUP_OPT(out->voice_rhythm.pause_behavior, s);
        }
    }

    /* Parse character_invariants + core_anchor */
    {
        hu_json_value_t *ci = hu_json_object_get(root, "character_invariants");
        if (ci)
            parse_string_array(alloc, ci, &out->character_invariants,
                               &out->character_invariants_count);
        const char *anchor = hu_json_get_string(root, "core_anchor");
        if (anchor)
            PERSONA_STRDUP_OPT(out->core_anchor, anchor);
    }

    /* Parse intellectual */
    {
        hu_json_value_t *ip = hu_json_object_get(root, "intellectual");
        if (ip && ip->type == HU_JSON_OBJECT) {
            hu_json_value_t *a = hu_json_object_get(ip, "expertise");
            if (a)
                parse_string_array(alloc, a, &out->intellectual.expertise,
                                   &out->intellectual.expertise_count);
            a = hu_json_object_get(ip, "curiosity_areas");
            if (a)
                parse_string_array(alloc, a, &out->intellectual.curiosity_areas,
                                   &out->intellectual.curiosity_areas_count);
            const char *s;
            s = hu_json_get_string(ip, "thinking_style");
            if (s)
                PERSONA_STRDUP_OPT(out->intellectual.thinking_style, s);
            s = hu_json_get_string(ip, "metaphor_sources");
            if (s)
                PERSONA_STRDUP_OPT(out->intellectual.metaphor_sources, s);
        }
    }

    /* Parse backstory_behaviors */
    {
        hu_json_value_t *bb_arr = hu_json_object_get(root, "backstory_behaviors");
        if (bb_arr && bb_arr->type == HU_JSON_ARRAY && bb_arr->data.array.items) {
            size_t n = bb_arr->data.array.len;
            if (n > 0) {
                hu_backstory_behavior_t *bbs = (hu_backstory_behavior_t *)alloc->alloc(
                    alloc->ctx, n * sizeof(hu_backstory_behavior_t));
                if (bbs) {
                    memset(bbs, 0, n * sizeof(hu_backstory_behavior_t));
                    size_t count = 0;
                    for (size_t i = 0; i < n; i++) {
                        const hu_json_value_t *item = bb_arr->data.array.items[i];
                        if (!item || item->type != HU_JSON_OBJECT)
                            continue;
                        const char *beat = hu_json_get_string(item, "backstory_beat");
                        const char *rule = hu_json_get_string(item, "behavioral_rule");
                        if (beat)
                            bbs[count].backstory_beat = hu_strdup(alloc, beat);
                        if (rule)
                            bbs[count].behavioral_rule = hu_strdup(alloc, rule);
                        count++;
                    }
                    out->backstory_behaviors = bbs;
                    out->backstory_behaviors_count = count;
                }
            }
        }
    }

    /* Parse sensory */
    {
        hu_json_value_t *sen = hu_json_object_get(root, "sensory");
        if (sen && sen->type == HU_JSON_OBJECT) {
            const char *s = hu_json_get_string(sen, "dominant_sense");
            if (s)
                PERSONA_STRDUP_OPT(out->sensory.dominant_sense, s);
            hu_json_value_t *a = hu_json_object_get(sen, "metaphor_vocabulary");
            if (a)
                parse_string_array(alloc, a, &out->sensory.metaphor_vocabulary,
                                   &out->sensory.metaphor_vocabulary_count);
            s = hu_json_get_string(sen, "grounding_patterns");
            if (s)
                PERSONA_STRDUP_OPT(out->sensory.grounding_patterns, s);
        }
    }

    /* Parse relational intelligence */
    {
        hu_json_value_t *rel = hu_json_object_get(root, "relational");
        if (rel && rel->type == HU_JSON_OBJECT) {
            const char *s = hu_json_get_string(rel, "bid_response_style");
            if (s)
                PERSONA_STRDUP_OPT(out->relational.bid_response_style, s);
            hu_json_value_t *a = hu_json_object_get(rel, "emotional_bids");
            if (a)
                parse_string_array(alloc, a, &out->relational.emotional_bids,
                                   &out->relational.emotional_bids_count);
            s = hu_json_get_string(rel, "attachment_style");
            if (s)
                PERSONA_STRDUP_OPT(out->relational.attachment_style, s);
            s = hu_json_get_string(rel, "attachment_awareness");
            if (s)
                PERSONA_STRDUP_OPT(out->relational.attachment_awareness, s);
            s = hu_json_get_string(rel, "dunbar_awareness");
            if (s)
                PERSONA_STRDUP_OPT(out->relational.dunbar_awareness, s);
        }
    }

    /* Parse listening protocol */
    {
        hu_json_value_t *lis = hu_json_object_get(root, "listening");
        if (lis && lis->type == HU_JSON_OBJECT) {
            const char *s = hu_json_get_string(lis, "default_response_type");
            if (s)
                PERSONA_STRDUP_OPT(out->listening.default_response_type, s);
            hu_json_value_t *a = hu_json_object_get(lis, "reflective_techniques");
            if (a)
                parse_string_array(alloc, a, &out->listening.reflective_techniques,
                                   &out->listening.reflective_techniques_count);
            s = hu_json_get_string(lis, "nvc_style");
            if (s)
                PERSONA_STRDUP_OPT(out->listening.nvc_style, s);
            s = hu_json_get_string(lis, "validation_style");
            if (s)
                PERSONA_STRDUP_OPT(out->listening.validation_style, s);
        }
    }

    /* Parse repair protocol */
    {
        hu_json_value_t *rep = hu_json_object_get(root, "repair");
        if (rep && rep->type == HU_JSON_OBJECT) {
            const char *s = hu_json_get_string(rep, "rupture_detection");
            if (s)
                PERSONA_STRDUP_OPT(out->repair.rupture_detection, s);
            s = hu_json_get_string(rep, "repair_approach");
            if (s)
                PERSONA_STRDUP_OPT(out->repair.repair_approach, s);
            s = hu_json_get_string(rep, "face_saving_style");
            if (s)
                PERSONA_STRDUP_OPT(out->repair.face_saving_style, s);
            hu_json_value_t *a = hu_json_object_get(rep, "repair_phrases");
            if (a)
                parse_string_array(alloc, a, &out->repair.repair_phrases,
                                   &out->repair.repair_phrases_count);
        }
    }

    /* Parse linguistic mirroring */
    {
        hu_json_value_t *mir = hu_json_object_get(root, "mirroring");
        if (mir && mir->type == HU_JSON_OBJECT) {
            const char *s = hu_json_get_string(mir, "mirroring_level");
            if (s)
                PERSONA_STRDUP_OPT(out->mirroring.mirroring_level, s);
            hu_json_value_t *a = hu_json_object_get(mir, "adapts_to");
            if (a)
                parse_string_array(alloc, a, &out->mirroring.adapts_to,
                                   &out->mirroring.adapts_to_count);
            s = hu_json_get_string(mir, "convergence_speed");
            if (s)
                PERSONA_STRDUP_OPT(out->mirroring.convergence_speed, s);
            s = hu_json_get_string(mir, "power_dynamic");
            if (s)
                PERSONA_STRDUP_OPT(out->mirroring.power_dynamic, s);
        }
    }

    /* Parse social dynamics */
    {
        hu_json_value_t *soc = hu_json_object_get(root, "social");
        if (soc && soc->type == HU_JSON_OBJECT) {
            const char *s = hu_json_get_string(soc, "default_ego_state");
            if (s)
                PERSONA_STRDUP_OPT(out->social.default_ego_state, s);
            s = hu_json_get_string(soc, "phatic_style");
            if (s)
                PERSONA_STRDUP_OPT(out->social.phatic_style, s);
            hu_json_value_t *a = hu_json_object_get(soc, "bonding_behaviors");
            if (a)
                parse_string_array(alloc, a, &out->social.bonding_behaviors,
                                   &out->social.bonding_behaviors_count);
            a = hu_json_object_get(soc, "anti_patterns");
            if (a)
                parse_string_array(alloc, a, &out->social.anti_patterns,
                                   &out->social.anti_patterns_count);
        }
    }

    /* Parse externalized prompt content (root level) */
    {
        hu_json_value_t *ir = hu_json_object_get(root, "immersive_reinforcement");
        if (ir)
            parse_string_array(alloc, ir, &out->immersive_reinforcement,
                               &out->immersive_reinforcement_count);
        const char *s = hu_json_get_string(root, "identity_reinforcement");
        if (s)
            PERSONA_STRDUP_OPT(out->identity_reinforcement, s);
        hu_json_value_t *ap = hu_json_object_get(root, "anti_patterns");
        if (ap)
            parse_string_array(alloc, ap, &out->anti_patterns, &out->anti_patterns_count);
        hu_json_value_t *sr = hu_json_object_get(root, "style_rules");
        if (sr)
            parse_string_array(alloc, sr, &out->style_rules, &out->style_rules_count);
        s = hu_json_get_string(root, "proactive_rules");
        if (s)
            PERSONA_STRDUP_OPT(out->proactive_rules, s);
        hu_json_value_t *to = hu_json_object_get(root, "time_overlays");
        if (to && to->type == HU_JSON_OBJECT) {
            s = hu_json_get_string(to, "late_night");
            if (s)
                PERSONA_STRDUP_OPT(out->time_overlay_late_night, s);
            s = hu_json_get_string(to, "early_morning");
            if (s)
                PERSONA_STRDUP_OPT(out->time_overlay_early_morning, s);
            s = hu_json_get_string(to, "afternoon");
            if (s)
                PERSONA_STRDUP_OPT(out->time_overlay_afternoon, s);
            s = hu_json_get_string(to, "evening");
            if (s)
                PERSONA_STRDUP_OPT(out->time_overlay_evening, s);
        }
    }

    /* Parse contacts */
    hu_json_value_t *contacts_obj = hu_json_object_get(root, "contacts");
    if (contacts_obj && contacts_obj->type == HU_JSON_OBJECT && contacts_obj->data.object.pairs) {
        size_t n = contacts_obj->data.object.len;
        hu_contact_profile_t *contacts =
            (hu_contact_profile_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_contact_profile_t));
        if (!contacts) {
            hu_persona_deinit(alloc, out);
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(contacts, 0, n * sizeof(hu_contact_profile_t));
        size_t count = 0;
        for (size_t i = 0; i < n; i++) {
            hu_json_pair_t *pair = &contacts_obj->data.object.pairs[i];
            if (!pair->key || !pair->value || pair->value->type != HU_JSON_OBJECT)
                continue;
            hu_contact_profile_t *cp = &contacts[count];
            cp->contact_id = hu_strdup(alloc, pair->key);
            if (!cp->contact_id)
                continue;
            const hu_json_value_t *cval = pair->value;
            const char *s;
            s = hu_json_get_string(cval, "name");
            if (s)
                PERSONA_STRDUP_OPT(cp->name, s);
            s = hu_json_get_string(cval, "email");
            if (s)
                PERSONA_STRDUP_OPT(cp->email, s);
            s = hu_json_get_string(cval, "relationship");
            if (s)
                PERSONA_STRDUP_OPT(cp->relationship, s);
            s = hu_json_get_string(cval, "relationship_stage");
            if (s)
                PERSONA_STRDUP_OPT(cp->relationship_stage, s);
            s = hu_json_get_string(cval, "relationship_type");
            if (s)
                PERSONA_STRDUP_OPT(cp->relationship_type, s);
            s = hu_json_get_string(cval, "warmth_level");
            if (s)
                PERSONA_STRDUP_OPT(cp->warmth_level, s);
            s = hu_json_get_string(cval, "vulnerability_level");
            if (s)
                PERSONA_STRDUP_OPT(cp->vulnerability_level, s);
            s = hu_json_get_string(cval, "identity");
            if (s)
                PERSONA_STRDUP_OPT(cp->identity, s);
            s = hu_json_get_string(cval, "context");
            if (s)
                PERSONA_STRDUP_OPT(cp->context, s);
            s = hu_json_get_string(cval, "dynamic");
            if (s)
                PERSONA_STRDUP_OPT(cp->dynamic, s);
            s = hu_json_get_string(cval, "greeting_style");
            if (s)
                PERSONA_STRDUP_OPT(cp->greeting_style, s);
            s = hu_json_get_string(cval, "closing_style");
            if (s)
                PERSONA_STRDUP_OPT(cp->closing_style, s);
            hu_json_value_t *arr;
            arr = hu_json_object_get(cval, "interests");
            if (arr)
                parse_string_array(alloc, arr, &cp->interests, &cp->interests_count);
            arr = hu_json_object_get(cval, "recent_topics");
            if (arr)
                parse_string_array(alloc, arr, &cp->recent_topics, &cp->recent_topics_count);
            arr = hu_json_object_get(cval, "sensitive_topics");
            if (arr)
                parse_string_array(alloc, arr, &cp->sensitive_topics, &cp->sensitive_topics_count);
            arr = hu_json_object_get(cval, "allowed_behaviors");
            if (arr)
                parse_string_array(alloc, arr, &cp->allowed_behaviors,
                                   &cp->allowed_behaviors_count);
            hu_json_value_t *comm = hu_json_object_get(cval, "communication_patterns");
            if (comm && comm->type == HU_JSON_OBJECT) {
                cp->texts_in_bursts = hu_json_get_bool(comm, "texts_in_bursts", false);
                cp->prefers_short_texts = hu_json_get_bool(comm, "prefers_short_texts", false);
                cp->sends_links_often = hu_json_get_bool(comm, "sends_links_often", false);
                cp->uses_emoji = hu_json_get_bool(comm, "uses_emoji", false);
            }

            /* Proactive engagement config */
            hu_json_value_t *proactive = hu_json_object_get(cval, "proactive");
            if (proactive && proactive->type == HU_JSON_OBJECT) {
                cp->proactive_checkin = hu_json_get_bool(proactive, "enabled", false);
                s = hu_json_get_string(proactive, "channel");
                if (s)
                    PERSONA_STRDUP_OPT(cp->proactive_channel, s);
                s = hu_json_get_string(proactive, "schedule");
                if (s)
                    PERSONA_STRDUP_OPT(cp->proactive_schedule, s);
            }

            s = hu_json_get_string(cval, "attachment_style");
            if (s)
                PERSONA_STRDUP_OPT(cp->attachment_style, s);
            s = hu_json_get_string(cval, "dunbar_layer");
            if (s)
                PERSONA_STRDUP_OPT(cp->dunbar_layer, s);

            count++;
        }
        out->contacts = contacts;
        out->contacts_count = count;
    }

    hu_json_value_t *overlays_obj = hu_json_object_get(root, "channel_overlays");
    if (overlays_obj && overlays_obj->type == HU_JSON_OBJECT && overlays_obj->data.object.pairs) {
        size_t n = overlays_obj->data.object.len;
        hu_persona_overlay_t *ovs =
            (hu_persona_overlay_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_persona_overlay_t));
        if (!ovs) {
            hu_persona_deinit(alloc, out);
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(ovs, 0, n * sizeof(hu_persona_overlay_t));
        size_t count = 0;
        for (size_t i = 0; i < n; i++) {
            hu_json_pair_t *pair = &overlays_obj->data.object.pairs[i];
            if (!pair->key || !pair->value || pair->value->type != HU_JSON_OBJECT)
                continue;
            err = parse_overlay(alloc, pair->key, pair->value, &ovs[count]);
            if (err != HU_OK) {
                for (size_t j = 0; j < count; j++)
                    free_overlay(alloc, &ovs[j]);
                alloc->free(alloc->ctx, ovs, n * sizeof(hu_persona_overlay_t));
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return err;
            }
            count++;
        }
        out->overlays = ovs;
        out->overlays_count = count;
    }

    if (oom_on_optional)
        fprintf(stderr, "[persona] warning: some optional fields dropped due to OOM\n");

    hu_json_free(alloc, root);
    return HU_OK;
#undef PERSONA_STRDUP_OPT
}

/* --- Validation --- */

static hu_error_t set_err_msg(hu_allocator_t *alloc, char **err_msg, size_t *err_msg_len,
                              const char *msg) {
    if (!alloc || !err_msg || !err_msg_len)
        return HU_ERR_INVALID_ARGUMENT;
    size_t len = strlen(msg);
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, msg, len + 1);
    *err_msg = buf;
    *err_msg_len = len;
    return HU_ERR_INVALID_ARGUMENT;
}

static bool is_string_array(const hu_json_value_t *arr) {
    if (!arr || arr->type != HU_JSON_ARRAY || !arr->data.array.items)
        return false;
    for (size_t i = 0; i < arr->data.array.len; i++) {
        const hu_json_value_t *item = arr->data.array.items[i];
        if (!item || item->type != HU_JSON_STRING)
            return false;
    }
    return true;
}

hu_error_t hu_persona_validate_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                    char **err_msg, size_t *err_msg_len) {
    if (!alloc || !json || !err_msg || !err_msg_len)
        return HU_ERR_INVALID_ARGUMENT;
    *err_msg = NULL;
    *err_msg_len = 0;

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK || !root) {
        return set_err_msg(alloc, err_msg, err_msg_len,
                           err != HU_OK ? "JSON parse error" : "Invalid JSON");
    }
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "Root must be JSON object");
    }

    /* Required: version — must be number, must be 1 */
    hu_json_value_t *ver_val = hu_json_object_get(root, "version");
    if (!ver_val || ver_val->type != HU_JSON_NUMBER) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len,
                           "Missing or invalid 'version' (must be number 1)");
    }
    double ver = ver_val->data.number;
    if (ver != 1.0) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "version must be 1");
    }

    /* Required: name — must be non-empty string */
    const char *name = hu_json_get_string(root, "name");
    if (!name || !name[0]) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "Missing or empty 'name'");
    }

    /* Required: core — must be object */
    hu_json_value_t *core = hu_json_object_get(root, "core");
    if (!core || core->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len,
                           "Missing or invalid 'core' (must be object)");
    }

    /* Required: core.identity — must be string */
    const char *identity = hu_json_get_string(core, "identity");
    if (!identity) {
        hu_json_value_t *id_val = hu_json_object_get(core, "identity");
        if (!id_val) {
            hu_json_free(alloc, root);
            return set_err_msg(alloc, err_msg, err_msg_len, "Missing 'core.identity'");
        }
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "core.identity must be string");
    }

    /* Required: core.traits — must be array */
    hu_json_value_t *traits = hu_json_object_get(core, "traits");
    if (!traits || traits->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len,
                           "Missing or invalid 'core.traits' (must be array)");
    }

    /* Optional: core.vocabulary — object */
    hu_json_value_t *vocab = hu_json_object_get(core, "vocabulary");
    if (vocab && vocab->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "core.vocabulary must be object");
    }
    if (vocab && vocab->type == HU_JSON_OBJECT) {
        hu_json_value_t *pref = hu_json_object_get(vocab, "preferred");
        if (pref && !is_string_array(pref)) {
            hu_json_free(alloc, root);
            return set_err_msg(alloc, err_msg, err_msg_len,
                               "core.vocabulary.preferred must be array of strings");
        }
        hu_json_value_t *avoid = hu_json_object_get(vocab, "avoided");
        if (avoid && !is_string_array(avoid)) {
            hu_json_free(alloc, root);
            return set_err_msg(alloc, err_msg, err_msg_len,
                               "core.vocabulary.avoided must be array of strings");
        }
        hu_json_value_t *slang = hu_json_object_get(vocab, "slang");
        if (slang && !is_string_array(slang)) {
            hu_json_free(alloc, root);
            return set_err_msg(alloc, err_msg, err_msg_len,
                               "core.vocabulary.slang must be array of strings");
        }
    }

    /* Optional: core.communication_rules — array of strings */
    hu_json_value_t *rules = hu_json_object_get(core, "communication_rules");
    if (rules && !is_string_array(rules)) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len,
                           "core.communication_rules must be array of strings");
    }

    /* Optional: core.values — array of strings */
    hu_json_value_t *vals = hu_json_object_get(core, "values");
    if (vals && !is_string_array(vals)) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "core.values must be array of strings");
    }

    /* Optional: core.decision_style — string */
    hu_json_value_t *ds_val = hu_json_object_get(core, "decision_style");
    if (ds_val && ds_val->type != HU_JSON_STRING) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "core.decision_style must be string");
    }

    /* Optional: channel_overlays — object */
    hu_json_value_t *overlays = hu_json_object_get(root, "channel_overlays");
    if (overlays && overlays->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "channel_overlays must be object");
    }

    /* Optional: motivation — object */
    hu_json_value_t *mot = hu_json_object_get(root, "motivation");
    if (mot && mot->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "motivation must be object");
    }

    /* Optional: situational_directions — array of objects */
    hu_json_value_t *sd = hu_json_object_get(root, "situational_directions");
    if (sd && sd->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "situational_directions must be array");
    }

    /* Optional: humor — object */
    hu_json_value_t *hum = hu_json_object_get(root, "humor");
    if (hum && hum->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "humor must be object");
    }

    /* Optional: conflict_style — object */
    hu_json_value_t *csj = hu_json_object_get(root, "conflict_style");
    if (csj && csj->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "conflict_style must be object");
    }

    /* Optional: emotional_range — object */
    hu_json_value_t *erj = hu_json_object_get(root, "emotional_range");
    if (erj && erj->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "emotional_range must be object");
    }

    /* Optional: voice_rhythm — object */
    hu_json_value_t *vrj = hu_json_object_get(root, "voice_rhythm");
    if (vrj && vrj->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "voice_rhythm must be object");
    }

    /* Optional: character_invariants — array of strings */
    hu_json_value_t *cij = hu_json_object_get(root, "character_invariants");
    if (cij && !is_string_array(cij)) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len,
                           "character_invariants must be array of strings");
    }

    /* Optional: core_anchor — string */
    hu_json_value_t *caj = hu_json_object_get(root, "core_anchor");
    if (caj && caj->type != HU_JSON_STRING) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "core_anchor must be string");
    }

    /* Optional: intellectual — object */
    hu_json_value_t *ipj = hu_json_object_get(root, "intellectual");
    if (ipj && ipj->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "intellectual must be object");
    }

    /* Optional: backstory_behaviors — array of objects */
    hu_json_value_t *bbj = hu_json_object_get(root, "backstory_behaviors");
    if (bbj && bbj->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "backstory_behaviors must be array");
    }

    /* Optional: sensory — object */
    hu_json_value_t *snj = hu_json_object_get(root, "sensory");
    if (snj && snj->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "sensory must be object");
    }

    /* Optional: relational — object */
    hu_json_value_t *rlj = hu_json_object_get(root, "relational");
    if (rlj && rlj->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "relational must be object");
    }

    /* Optional: listening — object */
    hu_json_value_t *lij = hu_json_object_get(root, "listening");
    if (lij && lij->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "listening must be object");
    }

    /* Optional: repair — object */
    hu_json_value_t *rpj = hu_json_object_get(root, "repair");
    if (rpj && rpj->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "repair must be object");
    }

    /* Optional: mirroring — object */
    hu_json_value_t *mrj = hu_json_object_get(root, "mirroring");
    if (mrj && mrj->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "mirroring must be object");
    }

    /* Optional: social — object */
    hu_json_value_t *scj = hu_json_object_get(root, "social");
    if (scj && scj->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return set_err_msg(alloc, err_msg, err_msg_len, "social must be object");
    }

    hu_json_free(alloc, root);
    return HU_OK;
}

hu_error_t hu_persona_load(hu_allocator_t *alloc, const char *name, size_t name_len,
                           hu_persona_t *out) {
    if (!alloc || !name || !out)
        return HU_ERR_INVALID_ARGUMENT;
    char base[HU_PERSONA_PATH_MAX];
    if (!hu_persona_base_dir(base, sizeof(base)))
        return HU_ERR_NOT_FOUND;
    char path[HU_PERSONA_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%.*s.json", base, (int)name_len, name);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > (long)(1024 * 1024)) {
        fclose(f);
        return sz < 0 ? HU_ERR_IO : HU_ERR_INVALID_ARGUMENT;
    }
    rewind(f);
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t read_len = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (read_len != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_IO;
    }
    buf[read_len] = '\0';
    hu_error_t err = hu_persona_load_json(alloc, buf, read_len, out);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    if (err != HU_OK)
        return err;

#if !(defined(HU_IS_TEST) && HU_IS_TEST) && (defined(__unix__) || defined(__APPLE__))
    /* Load example banks from <base>/examples/<name>/<channel>/examples.json */
    {
        char base_dir[HU_PERSONA_PATH_MAX];
        if (hu_persona_base_dir(base_dir, sizeof(base_dir)) && out->name && out->name_len > 0) {
            char ex_base[HU_PERSONA_PATH_MAX];
            int bn = snprintf(ex_base, sizeof(ex_base), "%s/examples/%.*s", base_dir,
                              (int)out->name_len, out->name);
            if (bn > 0 && (size_t)bn < sizeof(ex_base)) {
                DIR *d = opendir(ex_base);
                if (d) {
                    struct dirent *e;
                    while ((e = readdir(d)) != NULL) {
                        if (e->d_name[0] == '\0' || e->d_name[0] == '.')
                            continue;
                        char ch_path[HU_PERSONA_PATH_MAX];
                        int pn = snprintf(ch_path, sizeof(ch_path), "%s/%s/examples.json", ex_base,
                                          e->d_name);
                        if (pn <= 0 || (size_t)pn >= sizeof(ch_path))
                            continue;
                        FILE *ef = fopen(ch_path, "rb");
                        if (!ef)
                            continue;
                        if (fseek(ef, 0, SEEK_END) != 0) {
                            fclose(ef);
                            continue;
                        }
                        long esz = ftell(ef);
                        if (esz <= 0 || esz > (long)(64 * 1024)) {
                            fclose(ef);
                            continue;
                        }
                        rewind(ef);
                        char *ebuf = (char *)alloc->alloc(alloc->ctx, (size_t)esz + 1);
                        if (!ebuf) {
                            fclose(ef);
                            continue;
                        }
                        size_t erd = fread(ebuf, 1, (size_t)esz, ef);
                        fclose(ef);
                        if (erd != (size_t)esz) {
                            alloc->free(alloc->ctx, ebuf, (size_t)esz + 1);
                            continue;
                        }
                        ebuf[erd] = '\0';
                        size_t ch_len = strlen(e->d_name);
                        hu_persona_example_bank_t *banks = out->example_banks;
                        size_t banks_count = out->example_banks_count;
                        size_t new_cap = banks_count + 1;
                        hu_persona_example_bank_t *new_banks =
                            (hu_persona_example_bank_t *)alloc->realloc(
                                alloc->ctx, banks, banks_count * sizeof(hu_persona_example_bank_t),
                                new_cap * sizeof(hu_persona_example_bank_t));
                        if (!new_banks) {
                            alloc->free(alloc->ctx, ebuf, (size_t)esz + 1);
                            continue;
                        }
                        out->example_banks = new_banks;
                        memset(&new_banks[banks_count], 0, sizeof(hu_persona_example_bank_t));
                        hu_error_t berr = hu_persona_examples_load_json(
                            alloc, e->d_name, ch_len, ebuf, erd, &new_banks[banks_count]);
                        alloc->free(alloc->ctx, ebuf, (size_t)esz + 1);
                        if (berr == HU_OK)
                            out->example_banks_count++;
                        else
                            free_example_bank(alloc, &new_banks[banks_count]);
                    }
                    closedir(d);
                }
            }
        }
    }
#endif

    return HU_OK;
}

/* --- Prompt builder --- */

static hu_error_t append_prompt(hu_allocator_t *alloc, char **buf, size_t *len, size_t *cap,
                                const char *s, size_t slen) {
    while (*len + slen + 1 > *cap) {
        size_t new_cap = *cap ? *cap * 2 : HU_PERSONA_PROMPT_INIT_CAP;
        char *nb = (char *)alloc->realloc(alloc->ctx, *buf, *cap, new_cap);
        if (!nb)
            return HU_ERR_OUT_OF_MEMORY;
        *buf = nb;
        *cap = new_cap;
    }
    memcpy(*buf + *len, s, slen);
    (*buf)[*len + slen] = '\0';
    *len += slen;
    return HU_OK;
}

hu_error_t hu_persona_build_prompt(hu_allocator_t *alloc, const hu_persona_t *persona,
                                   const char *channel, size_t channel_len, const char *topic,
                                   size_t topic_len, char **out, size_t *out_len) {
    if (!alloc || !persona || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!topic)
        topic_len = 0;
    size_t cap = HU_PERSONA_PROMPT_INIT_CAP;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t len = 0;
    buf[0] = '\0';

    const char *name = persona->name ? persona->name : "persona";
    size_t name_len = persona->name_len ? persona->name_len : strlen(name);
    char header[256];
    int n = snprintf(header, sizeof(header), "You are acting as %.*s.", (int)name_len, name);
    if (n > 0) {
        hu_error_t err = append_prompt(alloc, &buf, &len, &cap, header, (size_t)n);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, buf, cap);
            return err;
        }
    }
    if (persona->identity && persona->identity[0]) {
        hu_error_t e2 = append_prompt(alloc, &buf, &len, &cap, " ", 1);
        if (e2 == HU_OK)
            e2 = append_prompt(alloc, &buf, &len, &cap, persona->identity,
                               strlen(persona->identity));
        if (e2 != HU_OK) {
            alloc->free(alloc->ctx, buf, cap);
            return e2;
        }
    }
    hu_error_t err = append_prompt(alloc, &buf, &len, &cap, "\n\n", 2);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, buf, cap);
        return err;
    }

    if (persona->traits && persona->traits_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Personality traits: ", 20);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < persona->traits_count; i++) {
            if (i > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                if (err != HU_OK)
                    goto fail;
            }
            const char *t = persona->traits[i];
            if (t)
                err = append_prompt(alloc, &buf, &len, &cap, t, strlen(t));
            if (err != HU_OK)
                goto fail;
        }
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != HU_OK)
            goto fail;
    }

    if (persona->preferred_vocab && persona->preferred_vocab_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Preferred vocabulary: ", 22);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < persona->preferred_vocab_count; i++) {
            if (i > 0)
                err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
            else
                err = HU_OK;
            if (err != HU_OK)
                goto fail;
            const char *v = persona->preferred_vocab[i];
            if (v)
                err = append_prompt(alloc, &buf, &len, &cap, v, strlen(v));
            if (err != HU_OK)
                goto fail;
        }
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != HU_OK)
            goto fail;
    }

    if (persona->avoided_vocab && persona->avoided_vocab_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Avoid: ", 7);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < persona->avoided_vocab_count; i++) {
            if (i > 0)
                err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
            else
                err = HU_OK;
            if (err != HU_OK)
                goto fail;
            const char *v = persona->avoided_vocab[i];
            if (v)
                err = append_prompt(alloc, &buf, &len, &cap, v, strlen(v));
            if (err != HU_OK)
                goto fail;
        }
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != HU_OK)
            goto fail;
    }

    if (persona->slang && persona->slang_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Slang: ", 7);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < persona->slang_count; i++) {
            if (i > 0)
                err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
            else
                err = HU_OK;
            if (err != HU_OK)
                goto fail;
            const char *s = persona->slang[i];
            if (s)
                err = append_prompt(alloc, &buf, &len, &cap, s, strlen(s));
            if (err != HU_OK)
                goto fail;
        }
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != HU_OK)
            goto fail;
    }

    if (persona->communication_rules && persona->communication_rules_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Communication rules:\n", 21);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < persona->communication_rules_count; i++) {
            const char *r = persona->communication_rules[i];
            if (r) {
                err = append_prompt(alloc, &buf, &len, &cap, "- ", 2);
                if (err != HU_OK)
                    goto fail;
                err = append_prompt(alloc, &buf, &len, &cap, r, strlen(r));
                if (err != HU_OK)
                    goto fail;
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
        }
    }

    if (persona->values && persona->values_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Values: ", 8);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < persona->values_count; i++) {
            if (i > 0)
                err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
            else
                err = HU_OK;
            if (err != HU_OK)
                goto fail;
            const char *v = persona->values[i];
            if (v)
                err = append_prompt(alloc, &buf, &len, &cap, v, strlen(v));
            if (err != HU_OK)
                goto fail;
        }
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != HU_OK)
            goto fail;
    }

    if (persona->decision_style && persona->decision_style[0]) {
        err = append_prompt(alloc, &buf, &len, &cap, "Decision style: ", 16);
        if (err != HU_OK)
            goto fail;
        err = append_prompt(alloc, &buf, &len, &cap, persona->decision_style,
                            strlen(persona->decision_style));
        if (err != HU_OK)
            goto fail;
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != HU_OK)
            goto fail;
    }

    /* Biography */
    if (persona->biography && persona->biography[0]) {
        err = append_prompt(alloc, &buf, &len, &cap, "\n--- Biography ---\n", 19);
        if (err == HU_OK)
            err = append_prompt(alloc, &buf, &len, &cap, persona->biography,
                                strlen(persona->biography));
        if (err == HU_OK)
            err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != HU_OK)
            goto fail;
    }

    /* Core anchor — single sentence anti-drift identity */
    if (persona->core_anchor && persona->core_anchor[0]) {
        static const char anc_hdr[] = "\n--- Core Anchor (your identity in one line) ---\n";
        err = append_prompt(alloc, &buf, &len, &cap, anc_hdr, sizeof(anc_hdr) - 1);
        if (err == HU_OK)
            err = append_prompt(alloc, &buf, &len, &cap, persona->core_anchor,
                                strlen(persona->core_anchor));
        if (err == HU_OK)
            err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != HU_OK)
            goto fail;
    }

    /* Motivation */
    {
        const hu_persona_motivation_t *m = &persona->motivation;
        bool has = m->primary_drive || m->protecting || m->avoiding || m->wanting;
        if (has) {
            static const char mot_hdr[] = "\n--- Motivation (your core drive) ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, mot_hdr, sizeof(mot_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (m->primary_drive) {
                char line[512];
                int w = snprintf(line, sizeof(line), "Why you engage: %s\n", m->primary_drive);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (m->protecting) {
                char line[512];
                int w = snprintf(line, sizeof(line), "What you protect: %s\n", m->protecting);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (m->avoiding) {
                char line[512];
                int w = snprintf(line, sizeof(line), "What you avoid: %s\n", m->avoiding);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (m->wanting) {
                char line[512];
                int w = snprintf(line, sizeof(line), "What you want most: %s\n", m->wanting);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
        }
    }

    /* Director's Notes */
    if (persona->directors_notes_count > 0) {
        static const char dn_hdr[] = "\n--- Director's Notes (performance direction) ---\n";
        err = append_prompt(alloc, &buf, &len, &cap, dn_hdr, sizeof(dn_hdr) - 1);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < persona->directors_notes_count; i++) {
            if (!persona->directors_notes[i])
                continue;
            err = append_prompt(alloc, &buf, &len, &cap, "- ", 2);
            if (err == HU_OK)
                err = append_prompt(alloc, &buf, &len, &cap, persona->directors_notes[i],
                                    strlen(persona->directors_notes[i]));
            if (err == HU_OK)
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
            if (err != HU_OK)
                goto fail;
        }
    }

    /* Situational direction — scene-specific director's notes */
    if (persona->situational_directions_count > 0) {
        static const char sd_hdr[] = "\n--- Situational Direction (scene-specific notes) ---\n";
        err = append_prompt(alloc, &buf, &len, &cap, sd_hdr, sizeof(sd_hdr) - 1);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < persona->situational_directions_count; i++) {
            const hu_situational_direction_t *d = &persona->situational_directions[i];
            if (d->trigger && d->instruction) {
                char line[512];
                int w = snprintf(line, sizeof(line), "- WHEN %s: %s\n", d->trigger, d->instruction);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
        }
    }

    /* Humor profile */
    {
        const hu_humor_profile_t *h = &persona->humor;
        bool has = h->type || h->frequency || h->timing;
        if (has) {
            static const char hum_hdr[] = "\n--- Humor ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, hum_hdr, sizeof(hum_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (h->type) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Type: %s\n", h->type);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (h->frequency) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Frequency: %s\n", h->frequency);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (h->timing) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Timing: %s\n", h->timing);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (h->targets_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Targets: ", 9);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < h->targets_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, h->targets[i],
                                        strlen(h->targets[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
            if (h->boundaries_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Never funny: ", 13);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < h->boundaries_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, h->boundaries[i],
                                        strlen(h->boundaries[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
        }
    }

    /* Conflict style */
    {
        const hu_conflict_style_t *cs = &persona->conflict_style;
        bool has = cs->pushback_response || cs->confrontation_comfort || cs->apology_style ||
                   cs->boundary_assertion || cs->repair_behavior;
        if (has) {
            static const char cs_hdr[] = "\n--- Conflict & Disagreement ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, cs_hdr, sizeof(cs_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (cs->pushback_response) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Pushback: %s\n", cs->pushback_response);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (cs->confrontation_comfort) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Confrontation comfort: %s\n",
                                 cs->confrontation_comfort);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (cs->apology_style) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Apology style: %s\n", cs->apology_style);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (cs->boundary_assertion) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Saying no: %s\n", cs->boundary_assertion);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (cs->repair_behavior) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Repair: %s\n", cs->repair_behavior);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
        }
    }

    /* Emotional range */
    {
        const hu_emotional_range_t *er = &persona->emotional_range;
        bool has = er->ceiling || er->floor || er->withdrawal_conditions || er->recovery_style ||
                   er->escalation_triggers_count > 0 || er->de_escalation_count > 0;
        if (has) {
            static const char er_hdr[] = "\n--- Emotional Range ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, er_hdr, sizeof(er_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (er->ceiling) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Ceiling: %s\n", er->ceiling);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (er->floor) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Floor: %s\n", er->floor);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (er->escalation_triggers_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Escalates when: ", 16);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < er->escalation_triggers_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, "; ", 2);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, er->escalation_triggers[i],
                                        strlen(er->escalation_triggers[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
            if (er->de_escalation_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Self-regulates by: ", 19);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < er->de_escalation_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, "; ", 2);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, er->de_escalation[i],
                                        strlen(er->de_escalation[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
            if (er->withdrawal_conditions) {
                char line[256];
                int w =
                    snprintf(line, sizeof(line), "Withdraws when: %s\n", er->withdrawal_conditions);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (er->recovery_style) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Recovery: %s\n", er->recovery_style);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
        }
    }

    /* Voice rhythm */
    {
        const hu_voice_rhythm_t *vr = &persona->voice_rhythm;
        bool has = vr->sentence_pattern || vr->paragraph_cadence || vr->response_tempo ||
                   vr->emphasis_style || vr->pause_behavior;
        if (has) {
            static const char vr_hdr[] = "\n--- Voice Rhythm ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, vr_hdr, sizeof(vr_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (vr->sentence_pattern) {
                char line[256];
                int w =
                    snprintf(line, sizeof(line), "Sentence pattern: %s\n", vr->sentence_pattern);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (vr->paragraph_cadence) {
                char line[256];
                int w =
                    snprintf(line, sizeof(line), "Paragraph cadence: %s\n", vr->paragraph_cadence);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (vr->response_tempo) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Response tempo: %s\n", vr->response_tempo);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (vr->emphasis_style) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Emphasis: %s\n", vr->emphasis_style);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (vr->pause_behavior) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Pauses: %s\n", vr->pause_behavior);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
        }
    }

    /* Intellectual profile */
    {
        const hu_intellectual_profile_t *ip = &persona->intellectual;
        bool has = ip->expertise_count > 0 || ip->curiosity_areas_count > 0 || ip->thinking_style ||
                   ip->metaphor_sources;
        if (has) {
            static const char ip_hdr[] = "\n--- Intellectual Profile ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, ip_hdr, sizeof(ip_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (ip->expertise_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Deep knowledge: ", 16);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < ip->expertise_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, ip->expertise[i],
                                        strlen(ip->expertise[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
            if (ip->curiosity_areas_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Genuinely curious about: ", 25);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < ip->curiosity_areas_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, ip->curiosity_areas[i],
                                        strlen(ip->curiosity_areas[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
            if (ip->thinking_style) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Thinks by: %s\n", ip->thinking_style);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (ip->metaphor_sources) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Draws metaphors from: %s\n",
                                 ip->metaphor_sources);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
        }
    }

    /* Backstory behaviors */
    if (persona->backstory_behaviors_count > 0) {
        static const char bb_hdr[] = "\n--- Backstory-to-Behavior (why you do what you do) ---\n";
        err = append_prompt(alloc, &buf, &len, &cap, bb_hdr, sizeof(bb_hdr) - 1);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < persona->backstory_behaviors_count; i++) {
            const hu_backstory_behavior_t *b = &persona->backstory_behaviors[i];
            if (b->backstory_beat && b->behavioral_rule) {
                char line[512];
                int w = snprintf(line, sizeof(line), "- Because %s → %s\n", b->backstory_beat,
                                 b->behavioral_rule);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
        }
    }

    /* Sensory preferences */
    {
        const hu_sensory_preferences_t *sp = &persona->sensory;
        bool has =
            sp->dominant_sense || sp->metaphor_vocabulary_count > 0 || sp->grounding_patterns;
        if (has) {
            static const char sp_hdr[] = "\n--- Sensory Grounding ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, sp_hdr, sizeof(sp_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (sp->dominant_sense) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Dominant sense: %s\n", sp->dominant_sense);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (sp->metaphor_vocabulary_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Sensory vocabulary: ", 20);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < sp->metaphor_vocabulary_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, sp->metaphor_vocabulary[i],
                                        strlen(sp->metaphor_vocabulary[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
            if (sp->grounding_patterns) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Grounding: %s\n", sp->grounding_patterns);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
        }
    }

    /* Relational intelligence (Gottman bids, attachment, Dunbar layers) */
    {
        const hu_relational_intelligence_t *ri = &persona->relational;
        bool has = ri->bid_response_style || ri->emotional_bids_count > 0 || ri->attachment_style ||
                   ri->attachment_awareness || ri->dunbar_awareness;
        if (has) {
            static const char ri_hdr[] = "\n--- Relational Intelligence ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, ri_hdr, sizeof(ri_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (ri->attachment_style) {
                char line[256];
                int w =
                    snprintf(line, sizeof(line), "Attachment style: %s\n", ri->attachment_style);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (ri->bid_response_style) {
                char line[512];
                int w = snprintf(line, sizeof(line), "Bid response: %s\n", ri->bid_response_style);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (ri->emotional_bids_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Bids you make: ", 15);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < ri->emotional_bids_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, ri->emotional_bids[i],
                                        strlen(ri->emotional_bids[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
            if (ri->attachment_awareness) {
                char line[512];
                int w = snprintf(line, sizeof(line), "Attachment awareness: %s\n",
                                 ri->attachment_awareness);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (ri->dunbar_awareness) {
                char line[512];
                int w =
                    snprintf(line, sizeof(line), "Relationship layers: %s\n", ri->dunbar_awareness);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
        }
    }

    /* Listening protocol (Derber support/shift, OARS, NVC) */
    {
        const hu_listening_protocol_t *lp = &persona->listening;
        bool has = lp->default_response_type || lp->reflective_techniques_count > 0 ||
                   lp->nvc_style || lp->validation_style;
        if (has) {
            static const char lp_hdr[] = "\n--- Listening & Response Protocol ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, lp_hdr, sizeof(lp_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (lp->default_response_type) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Default response type: %s\n",
                                 lp->default_response_type);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (lp->reflective_techniques_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Techniques: ", 12);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < lp->reflective_techniques_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, lp->reflective_techniques[i],
                                        strlen(lp->reflective_techniques[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
            if (lp->nvc_style) {
                char line[512];
                int w = snprintf(line, sizeof(line), "NVC approach: %s\n", lp->nvc_style);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (lp->validation_style) {
                char line[512];
                int w = snprintf(line, sizeof(line), "Validation: %s\n", lp->validation_style);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
        }
    }

    /* Repair protocol (rupture-repair, conversational repair, face-saving) */
    {
        const hu_repair_protocol_t *rp = &persona->repair;
        bool has = rp->rupture_detection || rp->repair_approach || rp->face_saving_style ||
                   rp->repair_phrases_count > 0;
        if (has) {
            static const char rp_hdr[] = "\n--- Repair Protocol ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, rp_hdr, sizeof(rp_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (rp->rupture_detection) {
                char line[512];
                int w =
                    snprintf(line, sizeof(line), "Rupture detection: %s\n", rp->rupture_detection);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (rp->repair_approach) {
                char line[512];
                int w = snprintf(line, sizeof(line), "Repair approach: %s\n", rp->repair_approach);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (rp->face_saving_style) {
                char line[512];
                int w = snprintf(line, sizeof(line), "Face-saving: %s\n", rp->face_saving_style);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (rp->repair_phrases_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Repair phrases: ", 16);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < rp->repair_phrases_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, " | ", 3);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, rp->repair_phrases[i],
                                        strlen(rp->repair_phrases[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
        }
    }

    /* Linguistic mirroring (CAT, accommodation, style matching) */
    {
        const hu_linguistic_mirroring_t *lm = &persona->mirroring;
        bool has = lm->mirroring_level || lm->adapts_to_count > 0 || lm->convergence_speed ||
                   lm->power_dynamic;
        if (has) {
            static const char lm_hdr[] = "\n--- Linguistic Mirroring ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, lm_hdr, sizeof(lm_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (lm->mirroring_level) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Mirroring level: %s\n", lm->mirroring_level);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (lm->adapts_to_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Adapts to: ", 11);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < lm->adapts_to_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, lm->adapts_to[i],
                                        strlen(lm->adapts_to[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
            if (lm->convergence_speed) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Convergence: %s\n", lm->convergence_speed);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (lm->power_dynamic) {
                char line[256];
                int w = snprintf(line, sizeof(line), "Power dynamic: %s\n", lm->power_dynamic);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
        }
    }

    /* Social dynamics (ego states, phatic communication, anti-patterns) */
    {
        const hu_social_dynamics_t *sd = &persona->social;
        bool has = sd->default_ego_state || sd->phatic_style || sd->bonding_behaviors_count > 0 ||
                   sd->anti_patterns_count > 0;
        if (has) {
            static const char sd_hdr[] = "\n--- Social Dynamics ---\n";
            err = append_prompt(alloc, &buf, &len, &cap, sd_hdr, sizeof(sd_hdr) - 1);
            if (err != HU_OK)
                goto fail;
            if (sd->default_ego_state) {
                char line[256];
                int w =
                    snprintf(line, sizeof(line), "Default ego state: %s\n", sd->default_ego_state);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (sd->phatic_style) {
                char line[512];
                int w = snprintf(line, sizeof(line), "Phatic style: %s\n", sd->phatic_style);
                if (w > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, line, (size_t)w);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (sd->bonding_behaviors_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "Bonding behaviors: ", 19);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < sd->bonding_behaviors_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, sd->bonding_behaviors[i],
                                        strlen(sd->bonding_behaviors[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
            if (sd->anti_patterns_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "NEVER do: ", 10);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < sd->anti_patterns_count; i++) {
                    if (i > 0) {
                        err = append_prompt(alloc, &buf, &len, &cap, " | ", 3);
                        if (err != HU_OK)
                            goto fail;
                    }
                    err = append_prompt(alloc, &buf, &len, &cap, sd->anti_patterns[i],
                                        strlen(sd->anti_patterns[i]));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
        }
    }

    /* Character invariants — anti-drift anchor at the end */
    if (persona->character_invariants_count > 0) {
        static const char ci_hdr[] = "\n--- Character Invariants (NEVER break these) ---\n";
        err = append_prompt(alloc, &buf, &len, &cap, ci_hdr, sizeof(ci_hdr) - 1);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < persona->character_invariants_count; i++) {
            err = append_prompt(alloc, &buf, &len, &cap, "- ", 2);
            if (err == HU_OK)
                err = append_prompt(alloc, &buf, &len, &cap, persona->character_invariants[i],
                                    strlen(persona->character_invariants[i]));
            if (err == HU_OK)
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
            if (err != HU_OK)
                goto fail;
        }
    }

    /* Mood states */
    if (persona->mood_states_count > 0) {
        static const char mood_hdr[] =
            "\n--- Available mood states ---\n"
            "You have moods that shift naturally. Current mood is chosen "
            "by the context of the conversation. Available moods:\n";
        err = append_prompt(alloc, &buf, &len, &cap, mood_hdr, sizeof(mood_hdr) - 1);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < persona->mood_states_count; i++) {
            if (!persona->mood_states[i])
                continue;
            err = append_prompt(alloc, &buf, &len, &cap, "- ", 2);
            if (err == HU_OK)
                err = append_prompt(alloc, &buf, &len, &cap, persona->mood_states[i],
                                    strlen(persona->mood_states[i]));
            if (err == HU_OK)
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
            if (err != HU_OK)
                goto fail;
        }
    }

    {
        static const char style_note[] =
            "\nMatch this style naturally. Don't exaggerate traits — aim for "
            "authenticity, not caricature.\n\n";
        err = append_prompt(alloc, &buf, &len, &cap, style_note, sizeof(style_note) - 1);
    }
    if (err != HU_OK)
        goto fail;

    if (channel && channel_len > 0) {
        const hu_persona_overlay_t *ov = hu_persona_find_overlay(persona, channel, channel_len);
        if (ov) {
            /* Build "Channel (imessage) style:\n" in one go for clarity */
            char ch_header[128];
            int ch_n = snprintf(ch_header, sizeof(ch_header), "Channel (%.*s) style:\n",
                                (int)channel_len, channel);
            if (ch_n > 0 && (size_t)ch_n < sizeof(ch_header)) {
                err = append_prompt(alloc, &buf, &len, &cap, ch_header, (size_t)ch_n);
            } else {
                err = append_prompt(alloc, &buf, &len, &cap, "Channel style:\n", 16);
            }
            if (err != HU_OK)
                goto fail;
            if (ov->formality && ov->formality[0]) {
                n = snprintf(header, sizeof(header), "- Formality: %s\n", ov->formality);
                if (n > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, header, (size_t)n);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (ov->avg_length && ov->avg_length[0]) {
                n = snprintf(header, sizeof(header), "- Avg length: %s\n", ov->avg_length);
                if (n > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, header, (size_t)n);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (ov->emoji_usage && ov->emoji_usage[0]) {
                n = snprintf(header, sizeof(header), "- Emoji usage: %s\n", ov->emoji_usage);
                if (n > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, header, (size_t)n);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (ov->style_notes && ov->style_notes_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "- Style notes: ", 15);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < ov->style_notes_count; i++) {
                    if (i > 0)
                        err = append_prompt(alloc, &buf, &len, &cap, "; ", 2);
                    else
                        err = HU_OK;
                    if (err != HU_OK)
                        goto fail;
                    const char *sn = ov->style_notes[i];
                    if (sn)
                        err = append_prompt(alloc, &buf, &len, &cap, sn, strlen(sn));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
            if (ov->message_splitting) {
                n = snprintf(header, sizeof(header), "- Message splitting: ON (%u chars)\n",
                             ov->max_segment_chars ? ov->max_segment_chars : 120);
                if (n > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, header, (size_t)n);
                    if (err != HU_OK)
                        goto fail;
                }
            }
            if (ov->typing_quirks && ov->typing_quirks_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "- Typing quirks: ", 17);
                if (err != HU_OK)
                    goto fail;
                for (size_t i = 0; i < ov->typing_quirks_count; i++) {
                    if (i > 0)
                        err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                    else
                        err = HU_OK;
                    if (err != HU_OK)
                        goto fail;
                    const char *q = ov->typing_quirks[i];
                    if (q)
                        err = append_prompt(alloc, &buf, &len, &cap, q, strlen(q));
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
        }
    }

    if (persona->example_banks && persona->example_banks_count > 0) {
        const hu_persona_example_t *sel_buf[8];
        size_t selected_count = 0;
        hu_error_t sel_err = hu_persona_select_examples(persona, channel, channel_len, topic,
                                                        topic_len, sel_buf, &selected_count, 5);
        if (sel_err == HU_OK && selected_count > 0) {
            static const char examples_header[] = "Example conversations showing your style:\n";
            err = append_prompt(alloc, &buf, &len, &cap, examples_header,
                                sizeof(examples_header) - 1);
            if (err != HU_OK)
                goto fail;
            for (size_t i = 0; i < selected_count; i++) {
                const hu_persona_example_t *ex = sel_buf[i];
                if (ex->incoming) {
                    err = append_prompt(alloc, &buf, &len, &cap, "Them: ", sizeof("Them: ") - 1);
                    if (err != HU_OK)
                        goto fail;
                    err =
                        append_prompt(alloc, &buf, &len, &cap, ex->incoming, strlen(ex->incoming));
                    if (err != HU_OK)
                        goto fail;
                    err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                    if (err != HU_OK)
                        goto fail;
                }
                if (ex->response) {
                    err = append_prompt(alloc, &buf, &len, &cap, "You: ", sizeof("You: ") - 1);
                    if (err != HU_OK)
                        goto fail;
                    err =
                        append_prompt(alloc, &buf, &len, &cap, ex->response, strlen(ex->response));
                    if (err != HU_OK)
                        goto fail;
                    err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                    if (err != HU_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != HU_OK)
                    goto fail;
            }
        }
    }

    if (len > HU_PERSONA_PROMPT_MAX_BYTES) {
        len = HU_PERSONA_PROMPT_MAX_BYTES;
        static const char trunc[] = "\n[persona prompt truncated]\n";
        if (len >= sizeof(trunc) - 1) {
            memcpy(buf + len - sizeof(trunc) + 1, trunc, sizeof(trunc) - 1);
        }
        buf[len] = '\0';
    }

    *out = buf;
    *out_len = len;
    return HU_OK;
fail:
    alloc->free(alloc->ctx, buf, cap);
    return err;
}

/* Feedback recording and apply are in feedback.c */
