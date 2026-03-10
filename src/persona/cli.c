#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/persona.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#define HU_PERSONA_PATH_MAX 512

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static bool str_in_arr(const char *str, char **arr, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (arr[i] && strcmp(arr[i], str) == 0)
            return true;
    return false;
}

static void diff_scalar(const char *label, const char *old_val, const char *new_val) {
    const char *o = old_val && old_val[0] ? old_val : "(none)";
    const char *n = new_val && new_val[0] ? new_val : "(none)";
    if (strcmp(o, n) == 0)
        return;
    fprintf(stdout, "%s:\n  - \"%s\"\n  + \"%s\"\n", label, o, n);
}

static void diff_arr(const char *label, char **a_arr, size_t a_count, char **b_arr,
                     size_t b_count) {
    bool any = false;
    for (size_t i = 0; i < a_count; i++)
        if (a_arr[i] && !str_in_arr(a_arr[i], b_arr, b_count)) {
            if (!any) {
                fprintf(stdout, "%s:\n", label);
                any = true;
            }
            fprintf(stdout, "  - %s\n", a_arr[i]);
        }
    for (size_t i = 0; i < b_count; i++)
        if (b_arr[i] && !str_in_arr(b_arr[i], a_arr, a_count)) {
            if (!any) {
                fprintf(stdout, "%s:\n", label);
                any = true;
            }
            fprintf(stdout, "  + %s\n", b_arr[i]);
        }
}
#endif

static const char *persona_dir_path(char *buf, size_t cap) {
    return hu_persona_base_dir(buf, cap);
}

hu_error_t hu_persona_cli_parse(int argc, const char **argv, hu_persona_cli_args_t *out) {
    if (!argv || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (argc < 3)
        return HU_ERR_INVALID_ARGUMENT;
    if (strcmp(argv[1], "persona") != 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *action = argv[2];
    if (strcmp(action, "create") == 0) {
        out->action = HU_PERSONA_ACTION_CREATE;
        if (argc < 4)
            return HU_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--from-imessage") == 0)
                out->from_imessage = true;
            else if (strcmp(argv[i], "--from-gmail") == 0) {
                out->from_gmail = true;
                if (i + 1 < argc) {
                    out->gmail_export_path = argv[i + 1];
                    i++;
                }
            } else if (strcmp(argv[i], "--from-facebook") == 0) {
                out->from_facebook = true;
                if (i + 1 < argc) {
                    out->facebook_export_path = argv[i + 1];
                    i++;
                }
            } else if (strcmp(argv[i], "--from-response") == 0 && i + 1 < argc)
                out->response_file = argv[++i];
            else if (strcmp(argv[i], "--with-contact") == 0 && i + 1 < argc)
                out->with_contact = argv[++i];
            else if (strcmp(argv[i], "--interactive") == 0)
                out->interactive = true;
        }
    } else if (strcmp(action, "update") == 0) {
        out->action = HU_PERSONA_ACTION_UPDATE;
        if (argc < 4)
            return HU_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--from-imessage") == 0)
                out->from_imessage = true;
            else if (strcmp(argv[i], "--from-gmail") == 0) {
                out->from_gmail = true;
                if (i + 1 < argc) {
                    out->gmail_export_path = argv[i + 1];
                    i++;
                }
            } else if (strcmp(argv[i], "--from-facebook") == 0) {
                out->from_facebook = true;
                if (i + 1 < argc) {
                    out->facebook_export_path = argv[i + 1];
                    i++;
                }
            } else if (strcmp(argv[i], "--from-response") == 0 && i + 1 < argc)
                out->response_file = argv[++i];
            else if (strcmp(argv[i], "--with-contact") == 0 && i + 1 < argc)
                out->with_contact = argv[++i];
            else if (strcmp(argv[i], "--interactive") == 0)
                out->interactive = true;
        }
    } else if (strcmp(action, "show") == 0) {
        out->action = HU_PERSONA_ACTION_SHOW;
        if (argc < 4)
            return HU_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
    } else if (strcmp(action, "list") == 0) {
        out->action = HU_PERSONA_ACTION_LIST;
    } else if (strcmp(action, "delete") == 0) {
        out->action = HU_PERSONA_ACTION_DELETE;
        if (argc < 4)
            return HU_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
    } else if (strcmp(action, "validate") == 0) {
        out->action = HU_PERSONA_ACTION_VALIDATE;
        if (argc < 4)
            return HU_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
    } else if (strcmp(action, "feedback") == 0) {
        if (argc < 5 || strcmp(argv[3], "apply") != 0)
            return HU_ERR_INVALID_ARGUMENT;
        out->action = HU_PERSONA_ACTION_FEEDBACK_APPLY;
        out->name = argv[4];
    } else if (strcmp(action, "diff") == 0) {
        out->action = HU_PERSONA_ACTION_DIFF;
        if (argc < 5)
            return HU_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        out->diff_name = argv[4];
    } else if (strcmp(action, "export") == 0) {
        out->action = HU_PERSONA_ACTION_EXPORT;
        if (argc < 4)
            return HU_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
    } else if (strcmp(action, "merge") == 0) {
        out->action = HU_PERSONA_ACTION_MERGE;
        if (argc < 6)
            return HU_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        out->merge_sources = (const char **)(argv + 4);
        out->merge_sources_count = (size_t)(argc - 4);
    } else if (strcmp(action, "import") == 0) {
        out->action = HU_PERSONA_ACTION_IMPORT;
        if (argc < 4)
            return HU_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--from-stdin") == 0)
                out->import_file = NULL;
            else if (strcmp(argv[i], "--from-file") == 0 && i + 1 < argc) {
                out->import_file = argv[i + 1];
                i++;
            }
        }
    } else {
        return HU_ERR_INVALID_ARGUMENT;
    }
    return HU_OK;
}

hu_error_t hu_persona_cli_run(hu_allocator_t *alloc, const hu_persona_cli_args_t *args) {
    if (!alloc || !args)
        return HU_ERR_INVALID_ARGUMENT;

    switch (args->action) {
    case HU_PERSONA_ACTION_SHOW: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for show\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_persona_t p = {0};
        hu_error_t err = hu_persona_load(alloc, args->name, strlen(args->name), &p);
        if (err != HU_OK) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return err;
        }
        char *prompt = NULL;
        size_t prompt_len = 0;
        err = hu_persona_build_prompt(alloc, &p, NULL, 0, NULL, 0, &prompt, &prompt_len);
        if (err == HU_OK && prompt) {
            fprintf(stdout, "%s", prompt);
            alloc->free(alloc->ctx, prompt, prompt_len + 1);
        }
        hu_persona_deinit(alloc, &p);
        return err;
    }
    case HU_PERSONA_ACTION_LIST: {
#if defined(__unix__) || defined(__APPLE__)
        char dir_buf[HU_PERSONA_PATH_MAX];
        const char *dir = persona_dir_path(dir_buf, sizeof(dir_buf));
        if (!dir) {
            fprintf(stderr, "Could not resolve persona directory\n");
            return HU_ERR_NOT_FOUND;
        }
        DIR *d = opendir(dir);
        if (!d) {
            return HU_OK;
        }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '\0' || e->d_name[0] == '.')
                continue;
            size_t len = strlen(e->d_name);
            if (len < 6 || strcmp(e->d_name + len - 5, ".json") != 0)
                continue;
            char name[256];
            size_t name_len = len - 5;
            if (name_len >= sizeof(name))
                continue;
            memcpy(name, e->d_name, name_len);
            name[name_len] = '\0';
            fprintf(stdout, "%s\n", name);
        }
        closedir(d);
        return HU_OK;
#else
        fprintf(stderr, "Persona list requires POSIX (opendir/readdir)\n");
        return HU_ERR_NOT_SUPPORTED;
#endif
    }
    case HU_PERSONA_ACTION_DELETE: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for delete\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
#if defined(__unix__) || defined(__APPLE__)
        char path[HU_PERSONA_PATH_MAX];
        const char *dir = persona_dir_path(path, sizeof(path));
        if (!dir) {
            fprintf(stderr, "Could not resolve persona directory\n");
            return HU_ERR_NOT_FOUND;
        }
        int n = snprintf(path, sizeof(path), "%s/%s.json", dir, args->name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "Invalid persona name\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (unlink(path) != 0) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return HU_ERR_NOT_FOUND;
        }
        fprintf(stdout, "Persona deleted: %s\n", args->name);
        return HU_OK;
#else
        fprintf(stderr, "Persona delete requires POSIX (unlink)\n");
        return HU_ERR_NOT_SUPPORTED;
#endif
    }
    case HU_PERSONA_ACTION_VALIDATE: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for validate\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
#if defined(HU_IS_TEST) && HU_IS_TEST
        (void)alloc;
        fprintf(stdout, "Persona '%s' is valid.\n", args->name);
        return HU_OK;
#else
#if defined(__unix__) || defined(__APPLE__)
        char base[HU_PERSONA_PATH_MAX];
        if (!hu_persona_base_dir(base, sizeof(base))) {
            fprintf(stderr, "Could not resolve persona directory (HOME or HU_PERSONA_DIR)\n");
            return HU_ERR_NOT_FOUND;
        }
        char path[HU_PERSONA_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s.json", base, args->name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "Invalid persona name\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return HU_ERR_NOT_FOUND;
        }
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
        char *err_msg = NULL;
        size_t err_len = 0;
        hu_error_t err = hu_persona_validate_json(alloc, buf, read_len, &err_msg, &err_len);
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        if (err != HU_OK) {
            fprintf(stderr, "Persona '%s' is invalid: %s\n", args->name,
                    err_msg ? err_msg : "unknown");
            if (err_msg)
                alloc->free(alloc->ctx, err_msg, err_len + 1);
            return err;
        }
        fprintf(stdout, "Persona '%s' is valid.\n", args->name);
        return HU_OK;
#else
        fprintf(stderr, "Persona validate requires POSIX\n");
        return HU_ERR_NOT_SUPPORTED;
#endif
#endif
    }
    case HU_PERSONA_ACTION_FEEDBACK_APPLY: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for feedback apply\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_error_t err = hu_persona_feedback_apply(alloc, args->name, strlen(args->name));
        if (err != HU_OK) {
            fprintf(stderr, "Failed to apply feedback for persona: %s\n", args->name);
            return err;
        }
        fprintf(stdout, "Feedback applied to persona: %s\n", args->name);
        return HU_OK;
    }
    case HU_PERSONA_ACTION_DIFF: {
        if (!args->name || !args->name[0] || !args->diff_name || !args->diff_name[0]) {
            fprintf(stderr, "Persona diff requires two persona names\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
#if defined(HU_IS_TEST) && HU_IS_TEST
        (void)alloc;
        return HU_OK;
#else
        hu_persona_t a = {0}, b = {0};
        hu_error_t err = hu_persona_load(alloc, args->name, strlen(args->name), &a);
        if (err != HU_OK) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return err;
        }
        err = hu_persona_load(alloc, args->diff_name, strlen(args->diff_name), &b);
        if (err != HU_OK) {
            fprintf(stderr, "Persona not found: %s\n", args->diff_name);
            hu_persona_deinit(alloc, &a);
            return err;
        }

        diff_scalar("identity", a.identity, b.identity);
        diff_arr("traits", a.traits, a.traits_count, b.traits, b.traits_count);
        diff_arr("preferred_vocab", a.preferred_vocab, a.preferred_vocab_count, b.preferred_vocab,
                 b.preferred_vocab_count);
        diff_arr("avoided_vocab", a.avoided_vocab, a.avoided_vocab_count, b.avoided_vocab,
                 b.avoided_vocab_count);
        diff_arr("slang", a.slang, a.slang_count, b.slang, b.slang_count);
        diff_arr("communication_rules", a.communication_rules, a.communication_rules_count,
                 b.communication_rules, b.communication_rules_count);
        diff_arr("values", a.values, a.values_count, b.values, b.values_count);
        diff_scalar("decision_style", a.decision_style, b.decision_style);
        diff_scalar("biography", a.biography, b.biography);
        diff_scalar("core_anchor", a.core_anchor, b.core_anchor);
        diff_arr("character_invariants", a.character_invariants, a.character_invariants_count,
                 b.character_invariants, b.character_invariants_count);
        diff_arr("directors_notes", a.directors_notes, a.directors_notes_count, b.directors_notes,
                 b.directors_notes_count);

        diff_scalar("motivation.primary_drive", a.motivation.primary_drive,
                    b.motivation.primary_drive);
        diff_scalar("motivation.protecting", a.motivation.protecting, b.motivation.protecting);
        diff_scalar("motivation.avoiding", a.motivation.avoiding, b.motivation.avoiding);
        diff_scalar("motivation.wanting", a.motivation.wanting, b.motivation.wanting);

        diff_scalar("humor.type", a.humor.type, b.humor.type);
        diff_scalar("humor.frequency", a.humor.frequency, b.humor.frequency);
        diff_scalar("humor.timing", a.humor.timing, b.humor.timing);
        diff_arr("humor.targets", a.humor.targets, a.humor.targets_count, b.humor.targets,
                 b.humor.targets_count);
        diff_arr("humor.boundaries", a.humor.boundaries, a.humor.boundaries_count,
                 b.humor.boundaries, b.humor.boundaries_count);

        diff_scalar("conflict_style.pushback_response", a.conflict_style.pushback_response,
                    b.conflict_style.pushback_response);
        diff_scalar("conflict_style.apology_style", a.conflict_style.apology_style,
                    b.conflict_style.apology_style);
        diff_scalar("conflict_style.confrontation_comfort", a.conflict_style.confrontation_comfort,
                    b.conflict_style.confrontation_comfort);
        diff_scalar("conflict_style.boundary_assertion", a.conflict_style.boundary_assertion,
                    b.conflict_style.boundary_assertion);
        diff_scalar("conflict_style.repair_behavior", a.conflict_style.repair_behavior,
                    b.conflict_style.repair_behavior);

        diff_scalar("emotional_range.ceiling", a.emotional_range.ceiling,
                    b.emotional_range.ceiling);
        diff_scalar("emotional_range.floor", a.emotional_range.floor, b.emotional_range.floor);
        diff_scalar("emotional_range.withdrawal_conditions",
                    a.emotional_range.withdrawal_conditions,
                    b.emotional_range.withdrawal_conditions);
        diff_scalar("emotional_range.recovery_style", a.emotional_range.recovery_style,
                    b.emotional_range.recovery_style);
        diff_arr("emotional_range.escalation_triggers", a.emotional_range.escalation_triggers,
                 a.emotional_range.escalation_triggers_count, b.emotional_range.escalation_triggers,
                 b.emotional_range.escalation_triggers_count);
        diff_arr("emotional_range.de_escalation", a.emotional_range.de_escalation,
                 a.emotional_range.de_escalation_count, b.emotional_range.de_escalation,
                 b.emotional_range.de_escalation_count);

        diff_scalar("voice_rhythm.sentence_pattern", a.voice_rhythm.sentence_pattern,
                    b.voice_rhythm.sentence_pattern);
        diff_scalar("voice_rhythm.paragraph_cadence", a.voice_rhythm.paragraph_cadence,
                    b.voice_rhythm.paragraph_cadence);
        diff_scalar("voice_rhythm.response_tempo", a.voice_rhythm.response_tempo,
                    b.voice_rhythm.response_tempo);
        diff_scalar("voice_rhythm.emphasis_style", a.voice_rhythm.emphasis_style,
                    b.voice_rhythm.emphasis_style);
        diff_scalar("voice_rhythm.pause_behavior", a.voice_rhythm.pause_behavior,
                    b.voice_rhythm.pause_behavior);

        diff_scalar("sensory.dominant_sense", a.sensory.dominant_sense, b.sensory.dominant_sense);
        diff_scalar("sensory.grounding_patterns", a.sensory.grounding_patterns,
                    b.sensory.grounding_patterns);
        diff_arr("sensory.metaphor_vocabulary", a.sensory.metaphor_vocabulary,
                 a.sensory.metaphor_vocabulary_count, b.sensory.metaphor_vocabulary,
                 b.sensory.metaphor_vocabulary_count);

        diff_scalar("intellectual.thinking_style", a.intellectual.thinking_style,
                    b.intellectual.thinking_style);
        diff_arr("intellectual.expertise", a.intellectual.expertise, a.intellectual.expertise_count,
                 b.intellectual.expertise, b.intellectual.expertise_count);
        diff_arr("intellectual.curiosity_areas", a.intellectual.curiosity_areas,
                 a.intellectual.curiosity_areas_count, b.intellectual.curiosity_areas,
                 b.intellectual.curiosity_areas_count);

        diff_scalar("relational.bid_response_style", a.relational.bid_response_style,
                    b.relational.bid_response_style);
        diff_scalar("relational.attachment_style", a.relational.attachment_style,
                    b.relational.attachment_style);
        diff_scalar("relational.attachment_awareness", a.relational.attachment_awareness,
                    b.relational.attachment_awareness);
        diff_scalar("relational.dunbar_awareness", a.relational.dunbar_awareness,
                    b.relational.dunbar_awareness);
        diff_arr("relational.emotional_bids", a.relational.emotional_bids,
                 a.relational.emotional_bids_count, b.relational.emotional_bids,
                 b.relational.emotional_bids_count);

        diff_scalar("listening.default_response_type", a.listening.default_response_type,
                    b.listening.default_response_type);
        diff_scalar("listening.nvc_style", a.listening.nvc_style, b.listening.nvc_style);
        diff_scalar("listening.validation_style", a.listening.validation_style,
                    b.listening.validation_style);
        diff_arr("listening.reflective_techniques", a.listening.reflective_techniques,
                 a.listening.reflective_techniques_count, b.listening.reflective_techniques,
                 b.listening.reflective_techniques_count);

        diff_scalar("repair.rupture_detection", a.repair.rupture_detection,
                    b.repair.rupture_detection);
        diff_scalar("repair.repair_approach", a.repair.repair_approach, b.repair.repair_approach);
        diff_scalar("repair.face_saving_style", a.repair.face_saving_style,
                    b.repair.face_saving_style);
        diff_arr("repair.repair_phrases", a.repair.repair_phrases, a.repair.repair_phrases_count,
                 b.repair.repair_phrases, b.repair.repair_phrases_count);

        diff_scalar("mirroring.mirroring_level", a.mirroring.mirroring_level,
                    b.mirroring.mirroring_level);
        diff_scalar("mirroring.convergence_speed", a.mirroring.convergence_speed,
                    b.mirroring.convergence_speed);
        diff_scalar("mirroring.power_dynamic", a.mirroring.power_dynamic,
                    b.mirroring.power_dynamic);
        diff_arr("mirroring.adapts_to", a.mirroring.adapts_to, a.mirroring.adapts_to_count,
                 b.mirroring.adapts_to, b.mirroring.adapts_to_count);

        diff_scalar("social.default_ego_state", a.social.default_ego_state,
                    b.social.default_ego_state);
        diff_scalar("social.phatic_style", a.social.phatic_style, b.social.phatic_style);
        diff_arr("social.bonding_behaviors", a.social.bonding_behaviors,
                 a.social.bonding_behaviors_count, b.social.bonding_behaviors,
                 b.social.bonding_behaviors_count);
        diff_arr("social.anti_patterns", a.social.anti_patterns, a.social.anti_patterns_count,
                 b.social.anti_patterns, b.social.anti_patterns_count);

        /* Overlays: compare by channel */
        bool overlay_any = false;
        for (size_t i = 0; i < a.overlays_count; i++) {
            const hu_persona_overlay_t *oa = &a.overlays[i];
            size_t ch_len = oa->channel ? strlen(oa->channel) : 0;
            const hu_persona_overlay_t *ob = hu_persona_find_overlay(&b, oa->channel, ch_len);
            if (!ob) {
                if (!overlay_any) {
                    fprintf(stdout, "overlays:\n");
                    overlay_any = true;
                }
                fprintf(stdout, "  - %s\n", oa->channel ? oa->channel : "(unknown)");
            } else {
                const char *af = oa->formality ? oa->formality : "";
                const char *bf = ob->formality ? ob->formality : "";
                const char *aa = oa->avg_length ? oa->avg_length : "";
                const char *ba = ob->avg_length ? ob->avg_length : "";
                const char *ae = oa->emoji_usage ? oa->emoji_usage : "";
                const char *be = ob->emoji_usage ? ob->emoji_usage : "";
                if (strcmp(af, bf) != 0 || strcmp(aa, ba) != 0 || strcmp(ae, be) != 0) {
                    if (!overlay_any) {
                        fprintf(stdout, "overlays:\n");
                        overlay_any = true;
                    }
                    fprintf(stdout, "  ~ %s (formality/avg_length/emoji changed)\n",
                            oa->channel ? oa->channel : "(unknown)");
                }
            }
        }
        for (size_t i = 0; i < b.overlays_count; i++) {
            const hu_persona_overlay_t *ob = &b.overlays[i];
            size_t ch_len = ob->channel ? strlen(ob->channel) : 0;
            const hu_persona_overlay_t *oa = hu_persona_find_overlay(&a, ob->channel, ch_len);
            if (!oa) {
                if (!overlay_any) {
                    fprintf(stdout, "overlays:\n");
                    overlay_any = true;
                }
                fprintf(stdout, "  + %s\n", ob->channel ? ob->channel : "(unknown)");
            }
        }

        hu_persona_deinit(alloc, &a);
        hu_persona_deinit(alloc, &b);
        return HU_OK;
#endif
    }
    case HU_PERSONA_ACTION_EXPORT: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for export\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
#if defined(__unix__) || defined(__APPLE__)
        char path[HU_PERSONA_PATH_MAX];
        const char *dir = persona_dir_path(path, sizeof(path));
        if (!dir) {
            fprintf(stderr, "Could not resolve persona directory\n");
            return HU_ERR_NOT_FOUND;
        }
        int n = snprintf(path, sizeof(path), "%s/%s.json", dir, args->name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "Invalid persona name\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return HU_ERR_NOT_FOUND;
        }
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
        fprintf(stdout, "%s", buf);
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_OK;
#else
        fprintf(stderr, "Persona export requires POSIX\n");
        return HU_ERR_NOT_SUPPORTED;
#endif
    }
    case HU_PERSONA_ACTION_MERGE: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Output persona name required for merge\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (!args->merge_sources || args->merge_sources_count < 2) {
            fprintf(stderr, "Merge requires at least 2 source personas\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_persona_t *partials = (hu_persona_t *)alloc->alloc(
            alloc->ctx, args->merge_sources_count * sizeof(hu_persona_t));
        if (!partials)
            return HU_ERR_OUT_OF_MEMORY;
        memset(partials, 0, args->merge_sources_count * sizeof(hu_persona_t));
        hu_error_t err = HU_OK;
        for (size_t i = 0; i < args->merge_sources_count; i++) {
            err = hu_persona_load(alloc, args->merge_sources[i], strlen(args->merge_sources[i]),
                                  &partials[i]);
            if (err != HU_OK) {
                fprintf(stderr, "Persona not found: %s\n", args->merge_sources[i]);
                for (size_t j = 0; j < i; j++)
                    hu_persona_deinit(alloc, &partials[j]);
                alloc->free(alloc->ctx, partials, args->merge_sources_count * sizeof(hu_persona_t));
                return err;
            }
        }
        hu_persona_t merged = {0};
        err = hu_persona_creator_synthesize(alloc, partials, args->merge_sources_count, args->name,
                                            strlen(args->name), &merged);
        for (size_t i = 0; i < args->merge_sources_count; i++)
            hu_persona_deinit(alloc, &partials[i]);
        alloc->free(alloc->ctx, partials, args->merge_sources_count * sizeof(hu_persona_t));
        if (err != HU_OK) {
            fprintf(stderr, "Failed to merge personas\n");
            return err;
        }
        err = hu_persona_creator_write(alloc, &merged);
        hu_persona_deinit(alloc, &merged);
        if (err != HU_OK)
            return err;
        fprintf(stdout, "Merged persona created: %s\n", args->name);
        return HU_OK;
    }
    case HU_PERSONA_ACTION_IMPORT: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for import\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        char *json = NULL;
        size_t json_len = 0;
        if (args->import_file) {
            FILE *f = fopen(args->import_file, "rb");
            if (!f) {
                fprintf(stderr, "Could not open file: %s\n", args->import_file);
                return HU_ERR_IO;
            }
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
            json = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
            if (!json) {
                fclose(f);
                return HU_ERR_OUT_OF_MEMORY;
            }
            json_len = fread(json, 1, (size_t)sz, f);
            fclose(f);
            json[json_len] = '\0';
        } else {
            size_t cap = 64 * 1024;
            json = (char *)alloc->alloc(alloc->ctx, cap);
            if (!json)
                return HU_ERR_OUT_OF_MEMORY;
            size_t pos = 0;
            int c;
            while (pos < cap - 1 && (c = getchar()) != EOF)
                json[pos++] = (char)c;
            json[pos] = '\0';
            json_len = pos;
        }
        char *err_msg = NULL;
        size_t err_len = 0;
        hu_error_t err = hu_persona_validate_json(alloc, json, json_len, &err_msg, &err_len);
        if (err != HU_OK) {
            fprintf(stderr, "Invalid persona JSON: %s\n", err_msg ? err_msg : "unknown");
            if (err_msg)
                alloc->free(alloc->ctx, err_msg, err_len + 1);
            alloc->free(alloc->ctx, json, json_len + 1);
            return err;
        }
#if defined(__unix__) || defined(__APPLE__)
        char dir_buf[HU_PERSONA_PATH_MAX];
        const char *dir = persona_dir_path(dir_buf, sizeof(dir_buf));
        if (!dir) {
            fprintf(stderr, "Could not resolve persona directory\n");
            alloc->free(alloc->ctx, json, json_len + 1);
            return HU_ERR_NOT_FOUND;
        }
#if defined(__unix__) || defined(__APPLE__)
        {
            const char *override = getenv("HU_PERSONA_DIR");
            if (!override || !override[0]) {
                const char *home = getenv("HOME");
                if (home && home[0]) {
                    char parent[HU_PERSONA_PATH_MAX];
                    int pn = snprintf(parent, sizeof(parent), "%s/.human", home);
                    if (pn > 0 && (size_t)pn < sizeof(parent))
                        (void)mkdir(parent, 0755);
                }
            }
            if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "Could not create persona directory\n");
                alloc->free(alloc->ctx, json, json_len + 1);
                return HU_ERR_IO;
            }
        }
#endif
        char out_path[HU_PERSONA_PATH_MAX];
        int on = snprintf(out_path, sizeof(out_path), "%s/%s.json", dir, args->name);
        if (on <= 0 || (size_t)on >= sizeof(out_path)) {
            alloc->free(alloc->ctx, json, json_len + 1);
            return HU_ERR_INVALID_ARGUMENT;
        }
        FILE *out = fopen(out_path, "wb");
        if (!out) {
            fprintf(stderr, "Could not write persona: %s\n", args->name);
            alloc->free(alloc->ctx, json, json_len + 1);
            return HU_ERR_IO;
        }
        size_t written = fwrite(json, 1, json_len, out);
        fclose(out);
        alloc->free(alloc->ctx, json, json_len + 1);
        if (written != json_len)
            return HU_ERR_IO;
        fprintf(stdout, "Persona imported: %s\n", args->name);
        return HU_OK;
#else
        alloc->free(alloc->ctx, json, json_len + 1);
        fprintf(stderr, "Persona import requires POSIX\n");
        return HU_ERR_NOT_SUPPORTED;
#endif
    }
    case HU_PERSONA_ACTION_CREATE:
    case HU_PERSONA_ACTION_UPDATE: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for create/update\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (!args->from_imessage && !args->from_gmail && !args->from_facebook &&
            !args->response_file) {
            fprintf(stderr, "No source specified. Use --from-imessage, --from-gmail, "
                            "--from-facebook, or --from-response <path>.\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (args->from_facebook && !args->facebook_export_path) {
            fprintf(stderr, "Facebook export requires a file path. Use: --from-facebook "
                            "/path/to/export.json\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (args->from_gmail && !args->gmail_export_path) {
            fprintf(stderr, "Gmail export requires a file path. Use: --from-gmail "
                            "/path/to/export.json\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
#if defined(HU_IS_TEST) && HU_IS_TEST
        (void)alloc;
        return HU_OK;
#else
        /* Step 2: --from-response — read AI response, parse, write persona */
        if (args->response_file) {
            FILE *rf = fopen(args->response_file, "rb");
            if (!rf) {
                fprintf(stderr, "Could not open response file: %s\n", args->response_file);
                return HU_ERR_IO;
            }
            if (fseek(rf, 0, SEEK_END) != 0) {
                fclose(rf);
                return HU_ERR_IO;
            }
            long rsz = ftell(rf);
            if (rsz < 0 || rsz > (long)(1024 * 1024)) {
                fclose(rf);
                fprintf(stderr, "Response file too large or invalid\n");
                return HU_ERR_INVALID_ARGUMENT;
            }
            rewind(rf);
            char *response = (char *)alloc->alloc(alloc->ctx, (size_t)rsz + 1);
            if (!response) {
                fclose(rf);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t rlen = fread(response, 1, (size_t)rsz, rf);
            fclose(rf);
            if (rlen != (size_t)rsz) {
                alloc->free(alloc->ctx, response, (size_t)rsz + 1);
                return HU_ERR_IO;
            }
            response[rlen] = '\0';

            hu_persona_t partial = {0};
            hu_error_t perr =
                hu_persona_analyzer_parse_response(alloc, response, rlen, "unknown", 7, &partial);
            alloc->free(alloc->ctx, response, (size_t)rsz + 1);
            if (perr != HU_OK) {
                fprintf(stderr, "Failed to parse AI response\n");
                return perr;
            }
            partial.name = hu_strdup(alloc, args->name);
            if (!partial.name) {
                hu_persona_deinit(alloc, &partial);
                return HU_ERR_OUT_OF_MEMORY;
            }
            partial.name_len = strlen(args->name);

            hu_error_t werr = hu_persona_creator_write(alloc, &partial);
            hu_persona_deinit(alloc, &partial);
            if (werr != HU_OK)
                return werr;
            char dir_buf[HU_PERSONA_PATH_MAX];
            const char *dir = persona_dir_path(dir_buf, sizeof(dir_buf));
            if (dir)
                fprintf(stdout, "Persona created at %s/%s.json\n", dir, args->name);
            else
                fprintf(stdout, "Persona created at ~/.human/personas/%s.json\n", args->name);
            return HU_OK;
        }

        /* Step 1: extract messages, build prompt, write to .pending */
        char base[HU_PERSONA_PATH_MAX];
        if (!hu_persona_base_dir(base, sizeof(base))) {
            fprintf(stderr, "Could not resolve persona directory (HOME or HU_PERSONA_DIR)\n");
            return HU_ERR_NOT_FOUND;
        }
        char pending_dir[HU_PERSONA_PATH_MAX];
        int pn = snprintf(pending_dir, sizeof(pending_dir), "%s/.pending", base);
        if (pn <= 0 || (size_t)pn >= sizeof(pending_dir))
            return HU_ERR_INVALID_ARGUMENT;
#if defined(__unix__) || defined(__APPLE__)
        {
            const char *override = getenv("HU_PERSONA_DIR");
            if (!override || !override[0]) {
                const char *home = getenv("HOME");
                if (home && home[0]) {
                    char parent[HU_PERSONA_PATH_MAX];
                    int pp = snprintf(parent, sizeof(parent), "%s/.human", home);
                    if (pp > 0 && (size_t)pp < sizeof(parent))
                        (void)mkdir(parent, 0755);
                }
            }
            if (mkdir(base, 0755) != 0 && errno != EEXIST)
                return HU_ERR_IO;
            if (mkdir(pending_dir, 0755) != 0 && errno != EEXIST)
                return HU_ERR_IO;
        }
#endif
        bool wrote_prompt = false;
        hu_allocator_t sys = hu_system_allocator();

        if (args->from_imessage) {
#if defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
            const char *home = getenv("HOME");
            if (!home || !home[0]) {
                fprintf(stderr, "HOME not set\n");
                return HU_ERR_NOT_FOUND;
            }
            char db_path[HU_PERSONA_PATH_MAX];
            int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
            if (n <= 0 || (size_t)n >= sizeof(db_path))
                return HU_ERR_INVALID_ARGUMENT;
            sqlite3 *db = NULL;
            if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
                if (db)
                    sqlite3_close(db);
                fprintf(stderr, "Could not open iMessage chat.db (Full Disk Access required)\n");
                return HU_ERR_IO;
            }

            /* Contact-scoped conversation extraction */
            if (args->with_contact) {
                char query[512];
                size_t query_len = 0;
                hu_error_t qerr = hu_persona_sampler_imessage_conversation_query(
                    args->with_contact, strlen(args->with_contact), query, sizeof(query),
                    &query_len, 1000);
                if (qerr != HU_OK) {
                    sqlite3_close(db);
                    return qerr;
                }
                sqlite3_stmt *stmt = NULL;
                if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
                    sqlite3_close(db);
                    return HU_ERR_IO;
                }
                size_t raw_cap = 1000;
                hu_sampler_raw_msg_t *raw =
                    (hu_sampler_raw_msg_t *)alloc->alloc(alloc->ctx, raw_cap * sizeof(*raw));
                char **text_bufs = (char **)alloc->alloc(alloc->ctx, raw_cap * sizeof(char *));
                if (!raw || !text_bufs) {
                    if (raw)
                        alloc->free(alloc->ctx, raw, raw_cap * sizeof(*raw));
                    if (text_bufs)
                        alloc->free(alloc->ctx, text_bufs, raw_cap * sizeof(char *));
                    sqlite3_finalize(stmt);
                    sqlite3_close(db);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                memset(raw, 0, raw_cap * sizeof(*raw));
                memset(text_bufs, 0, raw_cap * sizeof(char *));
                size_t raw_count = 0;
                while (sqlite3_step(stmt) == SQLITE_ROW && raw_count < raw_cap) {
                    const char *text = (const char *)sqlite3_column_text(stmt, 0);
                    int from_me = sqlite3_column_int(stmt, 1);
                    int64_t date = sqlite3_column_int64(stmt, 2);
                    if (text && text[0]) {
                        text_bufs[raw_count] = hu_strdup(alloc, text);
                        if (!text_bufs[raw_count])
                            break;
                        raw[raw_count].text = text_bufs[raw_count];
                        raw[raw_count].text_len = strlen(text);
                        raw[raw_count].is_from_me = (from_me != 0);
                        raw[raw_count].timestamp = date / 1000000000;
                        raw_count++;
                    }
                }
                sqlite3_finalize(stmt);
                sqlite3_close(db);
                fprintf(stdout, "Found %zu messages with contact %s\n", raw_count,
                        args->with_contact);

                hu_persona_example_t *examples = NULL;
                size_t example_count = 0;
                hu_error_t eerr = hu_persona_sampler_build_examples(alloc, raw, raw_count,
                                                                    &examples, &example_count);
                hu_sampler_contact_stats_t stats;
                (void)hu_persona_sampler_detect_contact(alloc, raw, raw_count, &stats);

                for (size_t ri = 0; ri < raw_count; ri++) {
                    if (text_bufs[ri])
                        alloc->free(alloc->ctx, text_bufs[ri], strlen(text_bufs[ri]) + 1);
                }
                alloc->free(alloc->ctx, text_bufs, raw_cap * sizeof(char *));
                alloc->free(alloc->ctx, raw, raw_cap * sizeof(*raw));

                if (eerr == HU_OK && example_count > 0) {
                    char examples_path[HU_PERSONA_PATH_MAX];
                    int en =
                        snprintf(examples_path, sizeof(examples_path), "%s/%s_examples_%s.json",
                                 pending_dir, args->name, args->with_contact);
                    if (en > 0 && (size_t)en < sizeof(examples_path)) {
                        FILE *ef = fopen(examples_path, "wb");
                        if (ef) {
                            fputs("[\n", ef);
                            for (size_t ei = 0; ei < example_count; ei++) {
                                if (ei > 0)
                                    fputs(",\n", ef);
                                fprintf(ef, "  {\"context\": \"%s\", \"incoming\": \"",
                                        examples[ei].context ? examples[ei].context : "");
                                for (const char *p = examples[ei].incoming; p && *p; p++) {
                                    if (*p == '"')
                                        fputs("\\\"", ef);
                                    else if (*p == '\n')
                                        fputs("\\n", ef);
                                    else if (*p == '\\')
                                        fputs("\\\\", ef);
                                    else
                                        fputc(*p, ef);
                                }
                                fputs("\", \"response\": \"", ef);
                                for (const char *p = examples[ei].response; p && *p; p++) {
                                    if (*p == '"')
                                        fputs("\\\"", ef);
                                    else if (*p == '\n')
                                        fputs("\\n", ef);
                                    else if (*p == '\\')
                                        fputs("\\\\", ef);
                                    else
                                        fputc(*p, ef);
                                }
                                fputs("\"}", ef);
                            }
                            fputs("\n]\n", ef);
                            fclose(ef);
                            fprintf(stdout, "Wrote %zu example conversations to %s\n",
                                    example_count, examples_path);
                        }
                    }
                    for (size_t ei = 0; ei < example_count; ei++) {
                        if (examples[ei].context)
                            alloc->free(alloc->ctx, examples[ei].context,
                                        strlen(examples[ei].context) + 1);
                        if (examples[ei].incoming)
                            alloc->free(alloc->ctx, examples[ei].incoming,
                                        strlen(examples[ei].incoming) + 1);
                        if (examples[ei].response)
                            alloc->free(alloc->ctx, examples[ei].response,
                                        strlen(examples[ei].response) + 1);
                    }
                    alloc->free(alloc->ctx, examples, example_count * sizeof(*examples));
                }
                fprintf(stdout,
                        "Contact stats: %zu their msgs, %zu my msgs, "
                        "avg_their=%zu, avg_mine=%zu, emoji=%s, links=%s, "
                        "bursts=%s, short=%s\n",
                        stats.their_msg_count, stats.my_msg_count, stats.avg_their_len,
                        stats.avg_my_len, stats.uses_emoji ? "yes" : "no",
                        stats.sends_links ? "yes" : "no", stats.texts_in_bursts ? "yes" : "no",
                        stats.prefers_short ? "yes" : "no");
                wrote_prompt = true;
                return HU_OK;
            }

            char query[512];
            size_t query_len = 0;
            hu_error_t qerr =
                hu_persona_sampler_imessage_query(query, sizeof(query), &query_len, 500);
            if (qerr != HU_OK) {
                sqlite3_close(db);
                return qerr;
            }
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
                sqlite3_close(db);
                return HU_ERR_IO;
            }
            char **messages = (char **)alloc->alloc(alloc->ctx, 500 * sizeof(char *));
            if (!messages) {
                sqlite3_finalize(stmt);
                sqlite3_close(db);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t msg_count = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW && msg_count < 500) {
                const char *text = (const char *)sqlite3_column_text(stmt, 0);
                if (text && text[0]) {
                    messages[msg_count] = hu_strdup(alloc, text);
                    if (!messages[msg_count]) {
                        for (size_t i = 0; i < msg_count; i++)
                            alloc->free(alloc->ctx, messages[i], strlen(messages[i]) + 1);
                        alloc->free(alloc->ctx, messages, 500 * sizeof(char *));
                        sqlite3_finalize(stmt);
                        sqlite3_close(db);
                        return HU_ERR_OUT_OF_MEMORY;
                    }
                    msg_count++;
                }
            }
            sqlite3_finalize(stmt);
            sqlite3_close(db);

            if (msg_count > 0) {
                size_t prompt_cap = 1024 * 1024;
                char *prompt_buf = (char *)alloc->alloc(alloc->ctx, prompt_cap);
                if (!prompt_buf) {
                    for (size_t i = 0; i < msg_count; i++)
                        alloc->free(alloc->ctx, messages[i], strlen(messages[i]) + 1);
                    alloc->free(alloc->ctx, messages, 500 * sizeof(char *));
                    return HU_ERR_OUT_OF_MEMORY;
                }
                size_t prompt_len = 0;
                const char **msg_ptrs =
                    (const char **)alloc->alloc(alloc->ctx, msg_count * sizeof(const char *));
                if (!msg_ptrs) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    for (size_t i = 0; i < msg_count; i++)
                        alloc->free(alloc->ctx, messages[i], strlen(messages[i]) + 1);
                    alloc->free(alloc->ctx, messages, 500 * sizeof(char *));
                    return HU_ERR_OUT_OF_MEMORY;
                }
                for (size_t i = 0; i < msg_count; i++)
                    msg_ptrs[i] = messages[i];
                hu_error_t berr = hu_persona_analyzer_build_prompt(
                    msg_ptrs, msg_count, "imessage", prompt_buf, prompt_cap, &prompt_len);
                alloc->free(alloc->ctx, msg_ptrs, msg_count * sizeof(const char *));
                for (size_t i = 0; i < msg_count; i++)
                    alloc->free(alloc->ctx, messages[i], strlen(messages[i]) + 1);
                alloc->free(alloc->ctx, messages, 500 * sizeof(char *));
                if (berr != HU_OK) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return berr;
                }
                char prompt_path[HU_PERSONA_PATH_MAX];
                int path_n = snprintf(prompt_path, sizeof(prompt_path), "%s/%s_imessage_prompt.txt",
                                      pending_dir, args->name);
                if (path_n <= 0 || (size_t)path_n >= sizeof(prompt_path)) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return HU_ERR_INVALID_ARGUMENT;
                }
                FILE *pf = fopen(prompt_path, "wb");
                if (!pf) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return HU_ERR_IO;
                }
                size_t written = fwrite(prompt_buf, 1, prompt_len, pf);
                fclose(pf);
                alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                if (written != prompt_len)
                    return HU_ERR_IO;
                wrote_prompt = true;
                fprintf(stdout, "Found %zu messages from iMessage\n", msg_count);
            }
#else
            fprintf(stderr, "iMessage sampling requires macOS and SQLite\n");
            return HU_ERR_NOT_SUPPORTED;
#endif
        }
        if (args->from_facebook && args->facebook_export_path) {
            FILE *f = fopen(args->facebook_export_path, "rb");
            if (!f) {
                fprintf(stderr, "Could not open Facebook export: %s\n", args->facebook_export_path);
                return HU_ERR_IO;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0 || sz > (long)(10 * 1024 * 1024)) {
                fclose(f);
                fprintf(stderr, "Facebook export file too large or empty\n");
                return HU_ERR_INVALID_ARGUMENT;
            }
            char *json = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
            if (!json) {
                fclose(f);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t nr = fread(json, 1, (size_t)sz, f);
            fclose(f);
            json[nr] = '\0';

            char **messages = NULL;
            size_t msg_count = 0;
            hu_error_t perr = hu_persona_sampler_facebook_parse(json, nr, &messages, &msg_count);
            alloc->free(alloc->ctx, json, (size_t)sz + 1);
            if (perr != HU_OK) {
                fprintf(stderr, "Failed to parse Facebook export\n");
                return perr;
            }
            if (msg_count > 0 && messages) {
                size_t prompt_cap = 1024 * 1024;
                char *prompt_buf = (char *)alloc->alloc(alloc->ctx, prompt_cap);
                if (!prompt_buf) {
                    for (size_t i = 0; i < msg_count; i++)
                        sys.free(sys.ctx, messages[i], strlen(messages[i]) + 1);
                    sys.free(sys.ctx, messages, msg_count * sizeof(char *));
                    return HU_ERR_OUT_OF_MEMORY;
                }
                size_t prompt_len = 0;
                hu_error_t berr =
                    hu_persona_analyzer_build_prompt((const char **)messages, msg_count, "facebook",
                                                     prompt_buf, prompt_cap, &prompt_len);
                for (size_t i = 0; i < msg_count; i++)
                    sys.free(sys.ctx, messages[i], strlen(messages[i]) + 1);
                sys.free(sys.ctx, messages, msg_count * sizeof(char *));
                if (berr != HU_OK) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return berr;
                }
                char prompt_path[HU_PERSONA_PATH_MAX];
                int path_n = snprintf(prompt_path, sizeof(prompt_path), "%s/%s_facebook_prompt.txt",
                                      pending_dir, args->name);
                if (path_n <= 0 || (size_t)path_n >= sizeof(prompt_path)) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return HU_ERR_INVALID_ARGUMENT;
                }
                FILE *pf = fopen(prompt_path, "wb");
                if (!pf) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return HU_ERR_IO;
                }
                size_t written = fwrite(prompt_buf, 1, prompt_len, pf);
                fclose(pf);
                alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                if (written != prompt_len)
                    return HU_ERR_IO;
                wrote_prompt = true;
                fprintf(stdout, "Found %zu messages from Facebook\n", msg_count);
            }
        }
        if (args->from_gmail && args->gmail_export_path) {
            FILE *f = fopen(args->gmail_export_path, "rb");
            if (!f) {
                fprintf(stderr, "Could not open Gmail export: %s\n", args->gmail_export_path);
                return HU_ERR_IO;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0 || sz > (long)(10 * 1024 * 1024)) {
                fclose(f);
                fprintf(stderr, "Gmail export file too large or empty\n");
                return HU_ERR_INVALID_ARGUMENT;
            }
            char *json = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
            if (!json) {
                fclose(f);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t nr = fread(json, 1, (size_t)sz, f);
            fclose(f);
            json[nr] = '\0';

            char **messages = NULL;
            size_t msg_count = 0;
            hu_error_t perr = hu_persona_sampler_gmail_parse(json, nr, &messages, &msg_count);
            alloc->free(alloc->ctx, json, (size_t)sz + 1);
            if (perr != HU_OK) {
                fprintf(stderr, "Failed to parse Gmail export\n");
                return perr;
            }
            if (msg_count > 0 && messages) {
                size_t prompt_cap = 1024 * 1024;
                char *prompt_buf = (char *)alloc->alloc(alloc->ctx, prompt_cap);
                if (!prompt_buf) {
                    for (size_t i = 0; i < msg_count; i++)
                        sys.free(sys.ctx, messages[i], strlen(messages[i]) + 1);
                    sys.free(sys.ctx, messages, msg_count * sizeof(char *));
                    return HU_ERR_OUT_OF_MEMORY;
                }
                size_t prompt_len = 0;
                hu_error_t berr =
                    hu_persona_analyzer_build_prompt((const char **)messages, msg_count, "gmail",
                                                     prompt_buf, prompt_cap, &prompt_len);
                for (size_t i = 0; i < msg_count; i++)
                    sys.free(sys.ctx, messages[i], strlen(messages[i]) + 1);
                sys.free(sys.ctx, messages, msg_count * sizeof(char *));
                if (berr != HU_OK) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return berr;
                }
                char prompt_path[HU_PERSONA_PATH_MAX];
                int path_n = snprintf(prompt_path, sizeof(prompt_path), "%s/%s_gmail_prompt.txt",
                                      pending_dir, args->name);
                if (path_n <= 0 || (size_t)path_n >= sizeof(prompt_path)) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return HU_ERR_INVALID_ARGUMENT;
                }
                FILE *pf = fopen(prompt_path, "wb");
                if (!pf) {
                    alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                    return HU_ERR_IO;
                }
                size_t written = fwrite(prompt_buf, 1, prompt_len, pf);
                fclose(pf);
                alloc->free(alloc->ctx, prompt_buf, prompt_cap);
                if (written != prompt_len)
                    return HU_ERR_IO;
                wrote_prompt = true;
                fprintf(stdout, "Found %zu messages from Gmail\n", msg_count);
            }
        }

        if (!wrote_prompt) {
            fprintf(stderr, "No messages found from any source.\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        fprintf(stdout, "Analysis prompt written to %s\n", pending_dir);
        fprintf(stdout, "Run this prompt through your AI provider, save the response, then run:\n");
        fprintf(stdout, "  human persona create %s --from-response <path>\n", args->name);
        return HU_OK;
#endif
    }
    }
    return HU_ERR_INVALID_ARGUMENT;
}
