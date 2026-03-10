#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/persona.h"
#include <stdio.h>
#include <string.h>

static hu_error_t parse_string_array_from_json(hu_allocator_t *a, const hu_json_value_t *arr,
                                               char ***out, size_t *out_count) {
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
    *out = buf;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_persona_analyzer_build_prompt(const char **messages, size_t msg_count,
                                            const char *channel, char *buf, size_t cap,
                                            size_t *out_len) {
    if (!messages || !buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    size_t n = 0;
    n += (size_t)snprintf(
        buf + n, cap - n,
        "Analyze these %zu message samples from channel \"%s\" and extract a deep "
        "personality profile.\n\n"
        "Return valid JSON with ALL of these fields:\n"
        "- identity (string: one-sentence description of who this person is)\n"
        "- traits (array of strings)\n"
        "- vocabulary {preferred (array), avoided (array), slang (array)}\n"
        "- communication_rules (array of strings)\n"
        "- values (array of strings)\n"
        "- decision_style (string)\n"
        "- formality (string), avg_length (string), emoji_usage (string)\n"
        "- humor {type (string), frequency (string), targets (array), "
        "boundaries (array), timing (string)}\n"
        "- conflict_style {pushback_response, confrontation_comfort, apology_style, "
        "boundary_assertion, repair_behavior} (all strings)\n"
        "- emotional_range {ceiling, floor (strings), escalation_triggers (array), "
        "de_escalation (array), withdrawal_conditions, recovery_style (strings)}\n"
        "- voice_rhythm {sentence_pattern, paragraph_cadence, response_tempo, "
        "emphasis_style, pause_behavior} (all strings)\n"
        "- motivation {primary_drive, protecting, avoiding, wanting} (all strings)\n"
        "- intellectual {thinking_style (string), expertise (array), curiosity_areas (array), "
        "metaphor_sources (string)}\n"
        "- sensory {dominant_sense (string), metaphor_vocabulary (array), "
        "grounding_patterns (string)}\n"
        "- relational {bid_response_style (string), emotional_bids (array), "
        "attachment_style (string: secure/anxious/avoidant/mixed), "
        "attachment_awareness (string), dunbar_awareness (string)}\n"
        "- listening {default_response_type (string: support or shift), "
        "reflective_techniques (array: e.g. open_questions, affirmations, "
        "reflective_listening, summary_reflections), "
        "nvc_style (string), validation_style (string)}\n"
        "- repair {rupture_detection (string), repair_approach (string), "
        "face_saving_style (string), repair_phrases (array)}\n"
        "- mirroring {mirroring_level (string: none/subtle/moderate/strong), "
        "adapts_to (array: e.g. message_length, formality, emoji, pacing), "
        "convergence_speed (string), power_dynamic (string)}\n"
        "- social {default_ego_state (string: adult/nurturing_parent/free_child), "
        "phatic_style (string), bonding_behaviors (array), anti_patterns (array)}\n"
        "\nMessages:\n",
        msg_count, channel ? channel : "unknown");
    if ((size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < msg_count && n < cap; i++) {
        if (messages[i]) {
            size_t len = strlen(messages[i]);
            if (n + len + 4 > cap)
                break;
            n += (size_t)snprintf(buf + n, cap - n, "%zu. %s\n", i + 1, messages[i]);
        }
    }
    *out_len = n;
    return HU_OK;
}

hu_error_t hu_persona_analyzer_parse_response(hu_allocator_t *alloc, const char *response,
                                              size_t resp_len, const char *channel,
                                              size_t channel_len, hu_persona_t *out) {
    if (!alloc || !response || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, response, resp_len, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(alloc, root);
        return err != HU_OK ? err : HU_ERR_JSON_PARSE;
    }

    hu_json_value_t *traits = hu_json_object_get(root, "traits");
    if (traits) {
        err = parse_string_array_from_json(alloc, traits, &out->traits, &out->traits_count);
        if (err != HU_OK) {
            hu_json_free(alloc, root);
            return err;
        }
    }

    hu_json_value_t *vocab = hu_json_object_get(root, "vocabulary");
    if (vocab && vocab->type == HU_JSON_OBJECT) {
        hu_json_value_t *pref = hu_json_object_get(vocab, "preferred");
        if (pref) {
            err = parse_string_array_from_json(alloc, pref, &out->preferred_vocab,
                                               &out->preferred_vocab_count);
            if (err != HU_OK) {
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return err;
            }
        }
        hu_json_value_t *avoid = hu_json_object_get(vocab, "avoided");
        if (avoid) {
            err = parse_string_array_from_json(alloc, avoid, &out->avoided_vocab,
                                               &out->avoided_vocab_count);
            if (err != HU_OK) {
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return err;
            }
        }
        hu_json_value_t *sl = hu_json_object_get(vocab, "slang");
        if (sl) {
            err = parse_string_array_from_json(alloc, sl, &out->slang, &out->slang_count);
            if (err != HU_OK) {
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return err;
            }
        }
    }

    hu_json_value_t *rules = hu_json_object_get(root, "communication_rules");
    if (rules) {
        err = parse_string_array_from_json(alloc, rules, &out->communication_rules,
                                           &out->communication_rules_count);
        if (err != HU_OK) {
            hu_persona_deinit(alloc, out);
            hu_json_free(alloc, root);
            return err;
        }
    }

    const char *formality = hu_json_get_string(root, "formality");
    const char *avg_length = hu_json_get_string(root, "avg_length");
    const char *emoji_usage = hu_json_get_string(root, "emoji_usage");
    if (channel && channel_len > 0 && (formality || avg_length || emoji_usage)) {
        out->overlays = alloc->alloc(alloc->ctx, sizeof(hu_persona_overlay_t));
        if (!out->overlays) {
            hu_persona_deinit(alloc, out);
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(out->overlays, 0, sizeof(hu_persona_overlay_t));
        out->overlays_count = 1;
        out->overlays[0].channel = hu_strndup(alloc, channel, channel_len);
        if (!out->overlays[0].channel) {
            hu_persona_deinit(alloc, out);
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        if (formality) {
            out->overlays[0].formality = hu_strdup(alloc, formality);
            if (!out->overlays[0].formality) {
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return HU_ERR_OUT_OF_MEMORY;
            }
        }
        if (avg_length) {
            out->overlays[0].avg_length = hu_strdup(alloc, avg_length);
            if (!out->overlays[0].avg_length) {
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return HU_ERR_OUT_OF_MEMORY;
            }
        }
        if (emoji_usage) {
            out->overlays[0].emoji_usage = hu_strdup(alloc, emoji_usage);
            if (!out->overlays[0].emoji_usage) {
                hu_persona_deinit(alloc, out);
                hu_json_free(alloc, root);
                return HU_ERR_OUT_OF_MEMORY;
            }
        }
    }

    const char *identity = hu_json_get_string(root, "identity");
    if (identity) {
        out->identity = hu_strdup(alloc, identity);
        if (!out->identity) {
            hu_persona_deinit(alloc, out);
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    const char *decision_style = hu_json_get_string(root, "decision_style");
    if (decision_style) {
        out->decision_style = hu_strdup(alloc, decision_style);
        if (!out->decision_style) {
            hu_persona_deinit(alloc, out);
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    hu_json_value_t *values = hu_json_object_get(root, "values");
    if (values) {
        err = parse_string_array_from_json(alloc, values, &out->values, &out->values_count);
        if (err != HU_OK) {
            hu_persona_deinit(alloc, out);
            hu_json_free(alloc, root);
            return err;
        }
    }

    /* Humor profile */
    hu_json_value_t *humor = hu_json_object_get(root, "humor");
    if (humor && humor->type == HU_JSON_OBJECT) {
        const char *ht = hu_json_get_string(humor, "type");
        if (ht)
            out->humor.type = hu_strdup(alloc, ht);
        const char *hf = hu_json_get_string(humor, "frequency");
        if (hf)
            out->humor.frequency = hu_strdup(alloc, hf);
        const char *htm = hu_json_get_string(humor, "timing");
        if (htm)
            out->humor.timing = hu_strdup(alloc, htm);
        hu_json_value_t *ht_arr = hu_json_object_get(humor, "targets");
        if (ht_arr)
            (void)parse_string_array_from_json(alloc, ht_arr, &out->humor.targets,
                                               &out->humor.targets_count);
        hu_json_value_t *hb_arr = hu_json_object_get(humor, "boundaries");
        if (hb_arr)
            (void)parse_string_array_from_json(alloc, hb_arr, &out->humor.boundaries,
                                               &out->humor.boundaries_count);
    }

    /* Conflict style */
    hu_json_value_t *conflict = hu_json_object_get(root, "conflict_style");
    if (conflict && conflict->type == HU_JSON_OBJECT) {
        const char *s;
        s = hu_json_get_string(conflict, "pushback_response");
        if (s)
            out->conflict_style.pushback_response = hu_strdup(alloc, s);
        s = hu_json_get_string(conflict, "confrontation_comfort");
        if (s)
            out->conflict_style.confrontation_comfort = hu_strdup(alloc, s);
        s = hu_json_get_string(conflict, "apology_style");
        if (s)
            out->conflict_style.apology_style = hu_strdup(alloc, s);
        s = hu_json_get_string(conflict, "boundary_assertion");
        if (s)
            out->conflict_style.boundary_assertion = hu_strdup(alloc, s);
        s = hu_json_get_string(conflict, "repair_behavior");
        if (s)
            out->conflict_style.repair_behavior = hu_strdup(alloc, s);
    }

    /* Emotional range */
    hu_json_value_t *emo = hu_json_object_get(root, "emotional_range");
    if (emo && emo->type == HU_JSON_OBJECT) {
        const char *s;
        s = hu_json_get_string(emo, "ceiling");
        if (s)
            out->emotional_range.ceiling = hu_strdup(alloc, s);
        s = hu_json_get_string(emo, "floor");
        if (s)
            out->emotional_range.floor = hu_strdup(alloc, s);
        s = hu_json_get_string(emo, "withdrawal_conditions");
        if (s)
            out->emotional_range.withdrawal_conditions = hu_strdup(alloc, s);
        s = hu_json_get_string(emo, "recovery_style");
        if (s)
            out->emotional_range.recovery_style = hu_strdup(alloc, s);
        hu_json_value_t *esc = hu_json_object_get(emo, "escalation_triggers");
        if (esc)
            (void)parse_string_array_from_json(alloc, esc,
                                               &out->emotional_range.escalation_triggers,
                                               &out->emotional_range.escalation_triggers_count);
        hu_json_value_t *de = hu_json_object_get(emo, "de_escalation");
        if (de)
            (void)parse_string_array_from_json(alloc, de, &out->emotional_range.de_escalation,
                                               &out->emotional_range.de_escalation_count);
    }

    /* Voice rhythm */
    hu_json_value_t *voice = hu_json_object_get(root, "voice_rhythm");
    if (voice && voice->type == HU_JSON_OBJECT) {
        const char *s;
        s = hu_json_get_string(voice, "sentence_pattern");
        if (s)
            out->voice_rhythm.sentence_pattern = hu_strdup(alloc, s);
        s = hu_json_get_string(voice, "paragraph_cadence");
        if (s)
            out->voice_rhythm.paragraph_cadence = hu_strdup(alloc, s);
        s = hu_json_get_string(voice, "response_tempo");
        if (s)
            out->voice_rhythm.response_tempo = hu_strdup(alloc, s);
        s = hu_json_get_string(voice, "emphasis_style");
        if (s)
            out->voice_rhythm.emphasis_style = hu_strdup(alloc, s);
        s = hu_json_get_string(voice, "pause_behavior");
        if (s)
            out->voice_rhythm.pause_behavior = hu_strdup(alloc, s);
    }

    /* Motivation */
    hu_json_value_t *mot = hu_json_object_get(root, "motivation");
    if (mot && mot->type == HU_JSON_OBJECT) {
        const char *s;
        s = hu_json_get_string(mot, "primary_drive");
        if (s)
            out->motivation.primary_drive = hu_strdup(alloc, s);
        s = hu_json_get_string(mot, "protecting");
        if (s)
            out->motivation.protecting = hu_strdup(alloc, s);
        s = hu_json_get_string(mot, "avoiding");
        if (s)
            out->motivation.avoiding = hu_strdup(alloc, s);
        s = hu_json_get_string(mot, "wanting");
        if (s)
            out->motivation.wanting = hu_strdup(alloc, s);
    }

    /* Intellectual profile */
    hu_json_value_t *intel = hu_json_object_get(root, "intellectual");
    if (intel && intel->type == HU_JSON_OBJECT) {
        const char *s = hu_json_get_string(intel, "thinking_style");
        if (s)
            out->intellectual.thinking_style = hu_strdup(alloc, s);
        hu_json_value_t *a = hu_json_object_get(intel, "expertise");
        if (a)
            (void)parse_string_array_from_json(alloc, a, &out->intellectual.expertise,
                                               &out->intellectual.expertise_count);
        a = hu_json_object_get(intel, "curiosity_areas");
        if (a)
            (void)parse_string_array_from_json(alloc, a, &out->intellectual.curiosity_areas,
                                               &out->intellectual.curiosity_areas_count);
        s = hu_json_get_string(intel, "metaphor_sources");
        if (s)
            out->intellectual.metaphor_sources = hu_strdup(alloc, s);
    }

    /* Sensory preferences */
    hu_json_value_t *sens = hu_json_object_get(root, "sensory");
    if (sens && sens->type == HU_JSON_OBJECT) {
        const char *s = hu_json_get_string(sens, "dominant_sense");
        if (s)
            out->sensory.dominant_sense = hu_strdup(alloc, s);
        hu_json_value_t *a = hu_json_object_get(sens, "metaphor_vocabulary");
        if (a)
            (void)parse_string_array_from_json(alloc, a, &out->sensory.metaphor_vocabulary,
                                               &out->sensory.metaphor_vocabulary_count);
        s = hu_json_get_string(sens, "grounding_patterns");
        if (s)
            out->sensory.grounding_patterns = hu_strdup(alloc, s);
    }

    /* Relational intelligence */
    hu_json_value_t *rel = hu_json_object_get(root, "relational");
    if (rel && rel->type == HU_JSON_OBJECT) {
        const char *s;
        s = hu_json_get_string(rel, "bid_response_style");
        if (s)
            out->relational.bid_response_style = hu_strdup(alloc, s);
        s = hu_json_get_string(rel, "attachment_style");
        if (s)
            out->relational.attachment_style = hu_strdup(alloc, s);
        s = hu_json_get_string(rel, "attachment_awareness");
        if (s)
            out->relational.attachment_awareness = hu_strdup(alloc, s);
        s = hu_json_get_string(rel, "dunbar_awareness");
        if (s)
            out->relational.dunbar_awareness = hu_strdup(alloc, s);
        hu_json_value_t *a = hu_json_object_get(rel, "emotional_bids");
        if (a)
            (void)parse_string_array_from_json(alloc, a, &out->relational.emotional_bids,
                                               &out->relational.emotional_bids_count);
    }

    /* Listening protocol */
    hu_json_value_t *lis = hu_json_object_get(root, "listening");
    if (lis && lis->type == HU_JSON_OBJECT) {
        const char *s;
        s = hu_json_get_string(lis, "default_response_type");
        if (s)
            out->listening.default_response_type = hu_strdup(alloc, s);
        s = hu_json_get_string(lis, "nvc_style");
        if (s)
            out->listening.nvc_style = hu_strdup(alloc, s);
        s = hu_json_get_string(lis, "validation_style");
        if (s)
            out->listening.validation_style = hu_strdup(alloc, s);
        hu_json_value_t *a = hu_json_object_get(lis, "reflective_techniques");
        if (a)
            (void)parse_string_array_from_json(alloc, a, &out->listening.reflective_techniques,
                                               &out->listening.reflective_techniques_count);
    }

    /* Repair protocol */
    hu_json_value_t *rep = hu_json_object_get(root, "repair");
    if (rep && rep->type == HU_JSON_OBJECT) {
        const char *s;
        s = hu_json_get_string(rep, "rupture_detection");
        if (s)
            out->repair.rupture_detection = hu_strdup(alloc, s);
        s = hu_json_get_string(rep, "repair_approach");
        if (s)
            out->repair.repair_approach = hu_strdup(alloc, s);
        s = hu_json_get_string(rep, "face_saving_style");
        if (s)
            out->repair.face_saving_style = hu_strdup(alloc, s);
        hu_json_value_t *a = hu_json_object_get(rep, "repair_phrases");
        if (a)
            (void)parse_string_array_from_json(alloc, a, &out->repair.repair_phrases,
                                               &out->repair.repair_phrases_count);
    }

    /* Linguistic mirroring */
    hu_json_value_t *mir = hu_json_object_get(root, "mirroring");
    if (mir && mir->type == HU_JSON_OBJECT) {
        const char *s;
        s = hu_json_get_string(mir, "mirroring_level");
        if (s)
            out->mirroring.mirroring_level = hu_strdup(alloc, s);
        s = hu_json_get_string(mir, "convergence_speed");
        if (s)
            out->mirroring.convergence_speed = hu_strdup(alloc, s);
        s = hu_json_get_string(mir, "power_dynamic");
        if (s)
            out->mirroring.power_dynamic = hu_strdup(alloc, s);
        hu_json_value_t *a = hu_json_object_get(mir, "adapts_to");
        if (a)
            (void)parse_string_array_from_json(alloc, a, &out->mirroring.adapts_to,
                                               &out->mirroring.adapts_to_count);
    }

    /* Social dynamics */
    hu_json_value_t *soc = hu_json_object_get(root, "social");
    if (soc && soc->type == HU_JSON_OBJECT) {
        const char *s;
        s = hu_json_get_string(soc, "default_ego_state");
        if (s)
            out->social.default_ego_state = hu_strdup(alloc, s);
        s = hu_json_get_string(soc, "phatic_style");
        if (s)
            out->social.phatic_style = hu_strdup(alloc, s);
        hu_json_value_t *a = hu_json_object_get(soc, "bonding_behaviors");
        if (a)
            (void)parse_string_array_from_json(alloc, a, &out->social.bonding_behaviors,
                                               &out->social.bonding_behaviors_count);
        a = hu_json_object_get(soc, "anti_patterns");
        if (a)
            (void)parse_string_array_from_json(alloc, a, &out->social.anti_patterns,
                                               &out->social.anti_patterns_count);
    }

    hu_json_free(alloc, root);
    return HU_OK;
}
