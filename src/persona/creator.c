#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/persona.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

static bool string_in_array(const char *s, char **arr, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (arr[i] && strcmp(arr[i], s) == 0)
            return true;
    }
    return false;
}

/* Merge traits from all partials, deduplicating */
static hu_error_t merge_traits(hu_allocator_t *alloc, char ***out, size_t *out_count,
                               const hu_persona_t *partials, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += partials[i].traits_count;
    if (total == 0)
        return HU_OK;
    char **buf = (char **)alloc->alloc(alloc->ctx, total * sizeof(char *));
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < partials[i].traits_count; j++) {
            const char *t = partials[i].traits[j];
            if (!t || string_in_array(t, buf, n))
                continue;
            char *dup = hu_strdup(alloc, t);
            if (!dup) {
                for (size_t k = 0; k < n; k++)
                    alloc->free(alloc->ctx, buf[k], strlen(buf[k]) + 1);
                alloc->free(alloc->ctx, buf, total * sizeof(char *));
                return HU_ERR_OUT_OF_MEMORY;
            }
            buf[n++] = dup;
        }
    }
    *out = buf;
    *out_count = n;
    return HU_OK;
}

/* Merge preferred_vocab only */
static hu_error_t merge_preferred_vocab(hu_allocator_t *alloc, char ***out, size_t *out_count,
                                        const hu_persona_t *partials, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += partials[i].preferred_vocab_count;
    if (total == 0)
        return HU_OK;
    char **buf = (char **)alloc->alloc(alloc->ctx, total * sizeof(char *));
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < partials[i].preferred_vocab_count; j++) {
            const char *v = partials[i].preferred_vocab[j];
            if (!v || string_in_array(v, buf, n))
                continue;
            char *dup = hu_strdup(alloc, v);
            if (!dup) {
                for (size_t k = 0; k < n; k++)
                    alloc->free(alloc->ctx, buf[k], strlen(buf[k]) + 1);
                alloc->free(alloc->ctx, buf, total * sizeof(char *));
                return HU_ERR_OUT_OF_MEMORY;
            }
            buf[n++] = dup;
        }
    }
    *out = buf;
    *out_count = n;
    return HU_OK;
}

static hu_error_t merge_communication_rules(hu_allocator_t *alloc, char ***out, size_t *out_count,
                                            const hu_persona_t *partials, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += partials[i].communication_rules_count;
    if (total == 0)
        return HU_OK;
    char **buf = (char **)alloc->alloc(alloc->ctx, total * sizeof(char *));
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < partials[i].communication_rules_count; j++) {
            const char *r = partials[i].communication_rules[j];
            if (!r || string_in_array(r, buf, n))
                continue;
            char *dup = hu_strdup(alloc, r);
            if (!dup) {
                for (size_t k = 0; k < n; k++)
                    alloc->free(alloc->ctx, buf[k], strlen(buf[k]) + 1);
                alloc->free(alloc->ctx, buf, total * sizeof(char *));
                return HU_ERR_OUT_OF_MEMORY;
            }
            buf[n++] = dup;
        }
    }
    *out = buf;
    *out_count = n;
    return HU_OK;
}

static hu_error_t merge_overlays(hu_allocator_t *alloc, hu_persona_overlay_t **out,
                                 size_t *out_count, const hu_persona_t *partials, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += partials[i].overlays_count;
    if (total == 0)
        return HU_OK;
    hu_persona_overlay_t *buf =
        (hu_persona_overlay_t *)alloc->alloc(alloc->ctx, total * sizeof(hu_persona_overlay_t));
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memset(buf, 0, total * sizeof(hu_persona_overlay_t));
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < partials[i].overlays_count; j++) {
            const hu_persona_overlay_t *ov = &partials[i].overlays[j];
            bool found = false;
            for (size_t k = 0; k < n; k++) {
                if (ov->channel && buf[k].channel && strcmp(ov->channel, buf[k].channel) == 0) {
                    found = true;
                    break;
                }
            }
            if (found)
                continue;
            buf[n].channel = ov->channel ? hu_strdup(alloc, ov->channel) : NULL;
            if (ov->channel && !buf[n].channel)
                goto overlay_oom;
            buf[n].formality = ov->formality ? hu_strdup(alloc, ov->formality) : NULL;
            if (ov->formality && !buf[n].formality)
                goto overlay_oom;
            buf[n].avg_length = ov->avg_length ? hu_strdup(alloc, ov->avg_length) : NULL;
            if (ov->avg_length && !buf[n].avg_length)
                goto overlay_oom;
            buf[n].emoji_usage = ov->emoji_usage ? hu_strdup(alloc, ov->emoji_usage) : NULL;
            if (ov->emoji_usage && !buf[n].emoji_usage)
                goto overlay_oom;
            if (ov->style_notes_count > 0 && ov->style_notes) {
                buf[n].style_notes =
                    (char **)alloc->alloc(alloc->ctx, ov->style_notes_count * sizeof(char *));
                if (buf[n].style_notes) {
                    buf[n].style_notes_count = ov->style_notes_count;
                    for (size_t k = 0; k < ov->style_notes_count; k++) {
                        buf[n].style_notes[k] =
                            ov->style_notes[k] ? hu_strdup(alloc, ov->style_notes[k]) : NULL;
                        if (ov->style_notes[k] && !buf[n].style_notes[k])
                            goto overlay_oom;
                    }
                }
            }
            n++;
        }
    }
    *out = buf;
    *out_count = n;
    return HU_OK;
overlay_oom:
    for (size_t i = 0; i <= n; i++) {
        if (buf[i].channel)
            alloc->free(alloc->ctx, buf[i].channel, strlen(buf[i].channel) + 1);
        if (buf[i].formality)
            alloc->free(alloc->ctx, buf[i].formality, strlen(buf[i].formality) + 1);
        if (buf[i].avg_length)
            alloc->free(alloc->ctx, buf[i].avg_length, strlen(buf[i].avg_length) + 1);
        if (buf[i].emoji_usage)
            alloc->free(alloc->ctx, buf[i].emoji_usage, strlen(buf[i].emoji_usage) + 1);
        if (buf[i].style_notes) {
            for (size_t k = 0; k < buf[i].style_notes_count; k++)
                if (buf[i].style_notes[k])
                    alloc->free(alloc->ctx, buf[i].style_notes[k],
                                strlen(buf[i].style_notes[k]) + 1);
            alloc->free(alloc->ctx, buf[i].style_notes, buf[i].style_notes_count * sizeof(char *));
        }
    }
    alloc->free(alloc->ctx, buf, total * sizeof(hu_persona_overlay_t));
    return HU_ERR_OUT_OF_MEMORY;
}

hu_error_t hu_persona_creator_synthesize(hu_allocator_t *alloc, const hu_persona_t *partials,
                                         size_t count, const char *name, size_t name_len,
                                         hu_persona_t *out) {
    if (!alloc || !partials || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    out->name = hu_strndup(alloc, name, name_len);
    if (!out->name)
        return HU_ERR_OUT_OF_MEMORY;
    out->name_len = name_len;

    hu_error_t err = merge_traits(alloc, &out->traits, &out->traits_count, partials, count);
    if (err != HU_OK) {
        hu_persona_deinit(alloc, out);
        return err;
    }
    err = merge_preferred_vocab(alloc, &out->preferred_vocab, &out->preferred_vocab_count, partials,
                                count);
    if (err != HU_OK) {
        hu_persona_deinit(alloc, out);
        return err;
    }
    /* Merge avoided and slang similarly - for simplicity use merge_vocab for all, but test only
       checks traits. Use separate merges for avoided/slang. */
    size_t total_avoided = 0, total_slang = 0;
    for (size_t i = 0; i < count; i++) {
        total_avoided += partials[i].avoided_vocab_count;
        total_slang += partials[i].slang_count;
    }
    if (total_avoided > 0) {
        char **abuf = (char **)alloc->alloc(alloc->ctx, total_avoided * sizeof(char *));
        if (abuf) {
            size_t an = 0;
            for (size_t i = 0; i < count; i++) {
                for (size_t j = 0; j < partials[i].avoided_vocab_count; j++) {
                    const char *v = partials[i].avoided_vocab[j];
                    if (v && !string_in_array(v, abuf, an)) {
                        char *dup = hu_strdup(alloc, v);
                        if (!dup) {
                            for (size_t k = 0; k < an; k++)
                                alloc->free(alloc->ctx, abuf[k], strlen(abuf[k]) + 1);
                            alloc->free(alloc->ctx, abuf, total_avoided * sizeof(char *));
                            hu_persona_deinit(alloc, out);
                            return HU_ERR_OUT_OF_MEMORY;
                        }
                        abuf[an++] = dup;
                    }
                }
            }
            out->avoided_vocab = abuf;
            out->avoided_vocab_count = an;
        }
    }
    if (total_slang > 0) {
        char **sbuf = (char **)alloc->alloc(alloc->ctx, total_slang * sizeof(char *));
        if (sbuf) {
            size_t sn = 0;
            for (size_t i = 0; i < count; i++) {
                for (size_t j = 0; j < partials[i].slang_count; j++) {
                    const char *v = partials[i].slang[j];
                    if (v && !string_in_array(v, sbuf, sn)) {
                        char *dup = hu_strdup(alloc, v);
                        if (!dup) {
                            for (size_t k = 0; k < sn; k++)
                                alloc->free(alloc->ctx, sbuf[k], strlen(sbuf[k]) + 1);
                            alloc->free(alloc->ctx, sbuf, total_slang * sizeof(char *));
                            hu_persona_deinit(alloc, out);
                            return HU_ERR_OUT_OF_MEMORY;
                        }
                        sbuf[sn++] = dup;
                    }
                }
            }
            out->slang = sbuf;
            out->slang_count = sn;
        }
    }

    err = merge_communication_rules(alloc, &out->communication_rules,
                                    &out->communication_rules_count, partials, count);
    if (err != HU_OK) {
        hu_persona_deinit(alloc, out);
        return err;
    }

    /* Merge values (dedup) */
    size_t vtotal = 0;
    for (size_t i = 0; i < count; i++)
        vtotal += partials[i].values_count;
    if (vtotal > 0) {
        char **vbuf = (char **)alloc->alloc(alloc->ctx, vtotal * sizeof(char *));
        if (vbuf) {
            size_t vn = 0;
            for (size_t i = 0; i < count; i++) {
                for (size_t k = 0; k < partials[i].values_count; k++) {
                    const char *v = partials[i].values[k];
                    if (v && !string_in_array(v, vbuf, vn)) {
                        char *dup = hu_strdup(alloc, v);
                        if (!dup) {
                            for (size_t j = 0; j < vn; j++)
                                alloc->free(alloc->ctx, vbuf[j], strlen(vbuf[j]) + 1);
                            alloc->free(alloc->ctx, vbuf, vtotal * sizeof(char *));
                            hu_persona_deinit(alloc, out);
                            return HU_ERR_OUT_OF_MEMORY;
                        }
                        vbuf[vn++] = dup;
                    }
                }
            }
            out->values = vbuf;
            out->values_count = vn;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (partials[i].identity && !out->identity) {
            out->identity = hu_strdup(alloc, partials[i].identity);
            if (!out->identity) {
                hu_persona_deinit(alloc, out);
                return HU_ERR_OUT_OF_MEMORY;
            }
        }
        if (partials[i].decision_style && !out->decision_style) {
            out->decision_style = hu_strdup(alloc, partials[i].decision_style);
            if (!out->decision_style) {
                hu_persona_deinit(alloc, out);
                return HU_ERR_OUT_OF_MEMORY;
            }
        }
    }

    err = merge_overlays(alloc, &out->overlays, &out->overlays_count, partials, count);
    if (err != HU_OK) {
        hu_persona_deinit(alloc, out);
        return err;
    }

    /* Merge deep fields: first non-null wins */
    for (size_t i = 0; i < count; i++) {
        if (partials[i].biography && !out->biography)
            out->biography = hu_strdup(alloc, partials[i].biography);
        if (partials[i].core_anchor && !out->core_anchor)
            out->core_anchor = hu_strdup(alloc, partials[i].core_anchor);

        if (partials[i].motivation.primary_drive && !out->motivation.primary_drive)
            out->motivation.primary_drive = hu_strdup(alloc, partials[i].motivation.primary_drive);
        if (partials[i].motivation.protecting && !out->motivation.protecting)
            out->motivation.protecting = hu_strdup(alloc, partials[i].motivation.protecting);
        if (partials[i].motivation.avoiding && !out->motivation.avoiding)
            out->motivation.avoiding = hu_strdup(alloc, partials[i].motivation.avoiding);
        if (partials[i].motivation.wanting && !out->motivation.wanting)
            out->motivation.wanting = hu_strdup(alloc, partials[i].motivation.wanting);

        if (partials[i].humor.type && !out->humor.type)
            out->humor.type = hu_strdup(alloc, partials[i].humor.type);
        if (partials[i].humor.frequency && !out->humor.frequency)
            out->humor.frequency = hu_strdup(alloc, partials[i].humor.frequency);
        if (partials[i].humor.timing && !out->humor.timing)
            out->humor.timing = hu_strdup(alloc, partials[i].humor.timing);

        if (partials[i].conflict_style.pushback_response &&
            !out->conflict_style.pushback_response)
            out->conflict_style.pushback_response =
                hu_strdup(alloc, partials[i].conflict_style.pushback_response);
        if (partials[i].conflict_style.apology_style && !out->conflict_style.apology_style)
            out->conflict_style.apology_style =
                hu_strdup(alloc, partials[i].conflict_style.apology_style);
        if (partials[i].conflict_style.confrontation_comfort &&
            !out->conflict_style.confrontation_comfort)
            out->conflict_style.confrontation_comfort =
                hu_strdup(alloc, partials[i].conflict_style.confrontation_comfort);
        if (partials[i].conflict_style.boundary_assertion &&
            !out->conflict_style.boundary_assertion)
            out->conflict_style.boundary_assertion =
                hu_strdup(alloc, partials[i].conflict_style.boundary_assertion);
        if (partials[i].conflict_style.repair_behavior && !out->conflict_style.repair_behavior)
            out->conflict_style.repair_behavior =
                hu_strdup(alloc, partials[i].conflict_style.repair_behavior);

        if (partials[i].emotional_range.ceiling && !out->emotional_range.ceiling)
            out->emotional_range.ceiling = hu_strdup(alloc, partials[i].emotional_range.ceiling);
        if (partials[i].emotional_range.floor && !out->emotional_range.floor)
            out->emotional_range.floor = hu_strdup(alloc, partials[i].emotional_range.floor);
        if (partials[i].emotional_range.withdrawal_conditions &&
            !out->emotional_range.withdrawal_conditions)
            out->emotional_range.withdrawal_conditions =
                hu_strdup(alloc, partials[i].emotional_range.withdrawal_conditions);
        if (partials[i].emotional_range.recovery_style && !out->emotional_range.recovery_style)
            out->emotional_range.recovery_style =
                hu_strdup(alloc, partials[i].emotional_range.recovery_style);

        if (partials[i].voice_rhythm.sentence_pattern && !out->voice_rhythm.sentence_pattern)
            out->voice_rhythm.sentence_pattern =
                hu_strdup(alloc, partials[i].voice_rhythm.sentence_pattern);
        if (partials[i].voice_rhythm.paragraph_cadence && !out->voice_rhythm.paragraph_cadence)
            out->voice_rhythm.paragraph_cadence =
                hu_strdup(alloc, partials[i].voice_rhythm.paragraph_cadence);
        if (partials[i].voice_rhythm.response_tempo && !out->voice_rhythm.response_tempo)
            out->voice_rhythm.response_tempo =
                hu_strdup(alloc, partials[i].voice_rhythm.response_tempo);
        if (partials[i].voice_rhythm.emphasis_style && !out->voice_rhythm.emphasis_style)
            out->voice_rhythm.emphasis_style =
                hu_strdup(alloc, partials[i].voice_rhythm.emphasis_style);
        if (partials[i].voice_rhythm.pause_behavior && !out->voice_rhythm.pause_behavior)
            out->voice_rhythm.pause_behavior =
                hu_strdup(alloc, partials[i].voice_rhythm.pause_behavior);

        /* Relational intelligence */
        if (partials[i].relational.bid_response_style && !out->relational.bid_response_style)
            out->relational.bid_response_style =
                hu_strdup(alloc, partials[i].relational.bid_response_style);
        if (partials[i].relational.attachment_style && !out->relational.attachment_style)
            out->relational.attachment_style =
                hu_strdup(alloc, partials[i].relational.attachment_style);
        if (partials[i].relational.attachment_awareness && !out->relational.attachment_awareness)
            out->relational.attachment_awareness =
                hu_strdup(alloc, partials[i].relational.attachment_awareness);
        if (partials[i].relational.dunbar_awareness && !out->relational.dunbar_awareness)
            out->relational.dunbar_awareness =
                hu_strdup(alloc, partials[i].relational.dunbar_awareness);

        /* Listening protocol */
        if (partials[i].listening.default_response_type && !out->listening.default_response_type)
            out->listening.default_response_type =
                hu_strdup(alloc, partials[i].listening.default_response_type);
        if (partials[i].listening.nvc_style && !out->listening.nvc_style)
            out->listening.nvc_style = hu_strdup(alloc, partials[i].listening.nvc_style);
        if (partials[i].listening.validation_style && !out->listening.validation_style)
            out->listening.validation_style =
                hu_strdup(alloc, partials[i].listening.validation_style);

        /* Repair protocol */
        if (partials[i].repair.rupture_detection && !out->repair.rupture_detection)
            out->repair.rupture_detection =
                hu_strdup(alloc, partials[i].repair.rupture_detection);
        if (partials[i].repair.repair_approach && !out->repair.repair_approach)
            out->repair.repair_approach = hu_strdup(alloc, partials[i].repair.repair_approach);
        if (partials[i].repair.face_saving_style && !out->repair.face_saving_style)
            out->repair.face_saving_style =
                hu_strdup(alloc, partials[i].repair.face_saving_style);

        /* Linguistic mirroring */
        if (partials[i].mirroring.mirroring_level && !out->mirroring.mirroring_level)
            out->mirroring.mirroring_level =
                hu_strdup(alloc, partials[i].mirroring.mirroring_level);
        if (partials[i].mirroring.convergence_speed && !out->mirroring.convergence_speed)
            out->mirroring.convergence_speed =
                hu_strdup(alloc, partials[i].mirroring.convergence_speed);
        if (partials[i].mirroring.power_dynamic && !out->mirroring.power_dynamic)
            out->mirroring.power_dynamic =
                hu_strdup(alloc, partials[i].mirroring.power_dynamic);

        /* Social dynamics */
        if (partials[i].social.default_ego_state && !out->social.default_ego_state)
            out->social.default_ego_state =
                hu_strdup(alloc, partials[i].social.default_ego_state);
        if (partials[i].social.phatic_style && !out->social.phatic_style)
            out->social.phatic_style = hu_strdup(alloc, partials[i].social.phatic_style);
    }

    return HU_OK;
}

#define HU_PERSONA_CREATOR_PATH_MAX 512

static hu_error_t write_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", f);
            break;
        case '\\':
            fputs("\\\\", f);
            break;
        case '\n':
            fputs("\\n", f);
            break;
        case '\r':
            fputs("\\r", f);
            break;
        case '\t':
            fputs("\\t", f);
            break;
        default:
            fputc(*p, f);
            break;
        }
    }
    fputc('"', f);
    return HU_OK;
}

static hu_error_t write_json_string_array(FILE *f, char **arr, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (i > 0)
            fputc(',', f);
        if (write_json_string(f, arr[i] ? arr[i] : "") != HU_OK)
            return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_persona_creator_write(hu_allocator_t *alloc, const hu_persona_t *persona) {
    if (!alloc || !persona || !persona->name)
        return HU_ERR_INVALID_ARGUMENT;

    char dir_buf[HU_PERSONA_CREATOR_PATH_MAX];
    if (!hu_persona_base_dir(dir_buf, sizeof(dir_buf)))
        return HU_ERR_NOT_FOUND;
#if defined(__unix__) || defined(__APPLE__)
    {
        const char *override = getenv("HU_PERSONA_DIR");
        if (!override || !override[0]) {
            const char *home = getenv("HOME");
            if (home && home[0]) {
                char parent[HU_PERSONA_CREATOR_PATH_MAX];
                int pn = snprintf(parent, sizeof(parent), "%s/.human", home);
                if (pn > 0 && (size_t)pn < sizeof(parent))
                    (void)mkdir(parent, 0755);
            }
        }
        if (mkdir(dir_buf, 0755) != 0 && errno != EEXIST)
            return HU_ERR_IO;
    }
#endif

    char path[HU_PERSONA_CREATOR_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%.*s.json", dir_buf, (int)persona->name_len,
                     persona->name);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "wb");
    if (!f)
        return HU_ERR_IO;

    fputs("{\n  \"version\": 1,\n  \"name\": ", f);
    if (write_json_string(f, persona->name) != HU_OK)
        goto fail;
    fputs(",\n  \"core\": {\n", f);

    fputs("    \"identity\": ", f);
    if (write_json_string(f, persona->identity) != HU_OK)
        goto fail;
    fputs(",\n    \"traits\": [", f);
    if (write_json_string_array(f, persona->traits, persona->traits_count) != HU_OK)
        goto fail;
    fputs("],\n    \"vocabulary\": {\n      \"preferred\": [", f);
    if (write_json_string_array(f, persona->preferred_vocab, persona->preferred_vocab_count) !=
        HU_OK)
        goto fail;
    fputs("],\n      \"avoided\": [", f);
    if (write_json_string_array(f, persona->avoided_vocab, persona->avoided_vocab_count) != HU_OK)
        goto fail;
    fputs("],\n      \"slang\": [", f);
    if (write_json_string_array(f, persona->slang, persona->slang_count) != HU_OK)
        goto fail;
    fputs("]\n    },\n    \"communication_rules\": [", f);
    if (write_json_string_array(f, persona->communication_rules,
                                persona->communication_rules_count) != HU_OK)
        goto fail;
    fputs("],\n    \"values\": [", f);
    if (write_json_string_array(f, persona->values, persona->values_count) != HU_OK)
        goto fail;
    fputs("],\n    \"decision_style\": ", f);
    if (write_json_string(f, persona->decision_style) != HU_OK)
        goto fail;

    fputs("\n  },\n  \"channel_overlays\": {\n", f);
    for (size_t i = 0; i < persona->overlays_count; i++) {
        const hu_persona_overlay_t *ov = &persona->overlays[i];
        if (!ov->channel)
            continue;
        if (i > 0)
            fputs(",\n", f);
        fputs("    ", f);
        if (write_json_string(f, ov->channel) != HU_OK)
            goto fail;
        fputs(": {\n      \"formality\": ", f);
        if (write_json_string(f, ov->formality) != HU_OK)
            goto fail;
        fputs(",\n      \"avg_length\": ", f);
        if (write_json_string(f, ov->avg_length) != HU_OK)
            goto fail;
        fputs(",\n      \"emoji_usage\": ", f);
        if (write_json_string(f, ov->emoji_usage) != HU_OK)
            goto fail;
        fputs(",\n      \"style_notes\": [", f);
        if (write_json_string_array(f, ov->style_notes, ov->style_notes_count) != HU_OK)
            goto fail;
        fputs("]\n    }", f);
    }
    fputs("\n  }", f);

    /* Biography */
    if (persona->biography) {
        fputs(",\n  \"biography\": ", f);
        write_json_string(f, persona->biography);
    }

    /* Motivation */
    if (persona->motivation.primary_drive || persona->motivation.protecting ||
        persona->motivation.avoiding || persona->motivation.wanting) {
        fputs(",\n  \"motivation\": {", f);
        bool first = true;
        if (persona->motivation.primary_drive) {
            fputs("\n    \"primary_drive\": ", f);
            write_json_string(f, persona->motivation.primary_drive);
            first = false;
        }
        if (persona->motivation.protecting) {
            if (!first) fputc(',', f);
            fputs("\n    \"protecting\": ", f);
            write_json_string(f, persona->motivation.protecting);
            first = false;
        }
        if (persona->motivation.avoiding) {
            if (!first) fputc(',', f);
            fputs("\n    \"avoiding\": ", f);
            write_json_string(f, persona->motivation.avoiding);
            first = false;
        }
        if (persona->motivation.wanting) {
            if (!first) fputc(',', f);
            fputs("\n    \"wanting\": ", f);
            write_json_string(f, persona->motivation.wanting);
        }
        fputs("\n  }", f);
    }

    /* Humor */
    if (persona->humor.type || persona->humor.frequency) {
        fputs(",\n  \"humor\": {", f);
        bool first = true;
        if (persona->humor.type) {
            fputs("\n    \"type\": ", f);
            write_json_string(f, persona->humor.type);
            first = false;
        }
        if (persona->humor.frequency) {
            if (!first) fputc(',', f);
            fputs("\n    \"frequency\": ", f);
            write_json_string(f, persona->humor.frequency);
            first = false;
        }
        if (persona->humor.targets_count > 0) {
            if (!first) fputc(',', f);
            fputs("\n    \"targets\": [", f);
            write_json_string_array(f, persona->humor.targets, persona->humor.targets_count);
            fputc(']', f);
            first = false;
        }
        if (persona->humor.boundaries_count > 0) {
            if (!first) fputc(',', f);
            fputs("\n    \"boundaries\": [", f);
            write_json_string_array(f, persona->humor.boundaries, persona->humor.boundaries_count);
            fputc(']', f);
            first = false;
        }
        if (persona->humor.timing) {
            if (!first) fputc(',', f);
            fputs("\n    \"timing\": ", f);
            write_json_string(f, persona->humor.timing);
        }
        fputs("\n  }", f);
    }

    /* Conflict style */
    if (persona->conflict_style.pushback_response || persona->conflict_style.apology_style) {
        fputs(",\n  \"conflict_style\": {", f);
        bool first = true;
        const char *cs_fields[] = {"pushback_response", "confrontation_comfort", "apology_style",
                                   "boundary_assertion", "repair_behavior"};
        const char *cs_vals[] = {
            persona->conflict_style.pushback_response,
            persona->conflict_style.confrontation_comfort,
            persona->conflict_style.apology_style,
            persona->conflict_style.boundary_assertion,
            persona->conflict_style.repair_behavior,
        };
        for (int ci = 0; ci < 5; ci++) {
            if (cs_vals[ci]) {
                if (!first) fputc(',', f);
                fprintf(f, "\n    \"%s\": ", cs_fields[ci]);
                write_json_string(f, cs_vals[ci]);
                first = false;
            }
        }
        fputs("\n  }", f);
    }

    /* Emotional range */
    if (persona->emotional_range.ceiling || persona->emotional_range.floor) {
        fputs(",\n  \"emotional_range\": {", f);
        bool first = true;
        if (persona->emotional_range.ceiling) {
            fputs("\n    \"ceiling\": ", f);
            write_json_string(f, persona->emotional_range.ceiling);
            first = false;
        }
        if (persona->emotional_range.floor) {
            if (!first) fputc(',', f);
            fputs("\n    \"floor\": ", f);
            write_json_string(f, persona->emotional_range.floor);
            first = false;
        }
        if (persona->emotional_range.escalation_triggers_count > 0) {
            if (!first) fputc(',', f);
            fputs("\n    \"escalation_triggers\": [", f);
            write_json_string_array(f, persona->emotional_range.escalation_triggers,
                                    persona->emotional_range.escalation_triggers_count);
            fputc(']', f);
            first = false;
        }
        if (persona->emotional_range.de_escalation_count > 0) {
            if (!first) fputc(',', f);
            fputs("\n    \"de_escalation\": [", f);
            write_json_string_array(f, persona->emotional_range.de_escalation,
                                    persona->emotional_range.de_escalation_count);
            fputc(']', f);
            first = false;
        }
        if (persona->emotional_range.withdrawal_conditions) {
            if (!first) fputc(',', f);
            fputs("\n    \"withdrawal_conditions\": ", f);
            write_json_string(f, persona->emotional_range.withdrawal_conditions);
            first = false;
        }
        if (persona->emotional_range.recovery_style) {
            if (!first) fputc(',', f);
            fputs("\n    \"recovery_style\": ", f);
            write_json_string(f, persona->emotional_range.recovery_style);
        }
        fputs("\n  }", f);
    }

    /* Voice rhythm */
    if (persona->voice_rhythm.sentence_pattern || persona->voice_rhythm.response_tempo) {
        fputs(",\n  \"voice_rhythm\": {", f);
        bool first = true;
        const char *vr_fields[] = {"sentence_pattern", "paragraph_cadence", "response_tempo",
                                   "emphasis_style", "pause_behavior"};
        const char *vr_vals[] = {
            persona->voice_rhythm.sentence_pattern,
            persona->voice_rhythm.paragraph_cadence,
            persona->voice_rhythm.response_tempo,
            persona->voice_rhythm.emphasis_style,
            persona->voice_rhythm.pause_behavior,
        };
        for (int vi = 0; vi < 5; vi++) {
            if (vr_vals[vi]) {
                if (!first) fputc(',', f);
                fprintf(f, "\n    \"%s\": ", vr_fields[vi]);
                write_json_string(f, vr_vals[vi]);
                first = false;
            }
        }
        fputs("\n  }", f);
    }

    /* Inner world */
    if (persona->inner_world.contradictions_count > 0 ||
        persona->inner_world.embodied_memories_count > 0) {
        fputs(",\n  \"inner_world\": {", f);
        bool first = true;
        struct {
            const char *key;
            char **arr;
            size_t count;
        } iw[] = {
            {"contradictions", persona->inner_world.contradictions,
             persona->inner_world.contradictions_count},
            {"embodied_memories", persona->inner_world.embodied_memories,
             persona->inner_world.embodied_memories_count},
            {"emotional_flashpoints", persona->inner_world.emotional_flashpoints,
             persona->inner_world.emotional_flashpoints_count},
            {"unfinished_business", persona->inner_world.unfinished_business,
             persona->inner_world.unfinished_business_count},
            {"secret_self", persona->inner_world.secret_self,
             persona->inner_world.secret_self_count},
        };
        for (int wi = 0; wi < 5; wi++) {
            if (iw[wi].count > 0) {
                if (!first) fputc(',', f);
                fprintf(f, "\n    \"%s\": [", iw[wi].key);
                write_json_string_array(f, iw[wi].arr, iw[wi].count);
                fputc(']', f);
                first = false;
            }
        }
        fputs("\n  }", f);
    }

    /* Directors notes */
    if (persona->directors_notes_count > 0) {
        fputs(",\n  \"directors_notes\": [", f);
        write_json_string_array(f, persona->directors_notes, persona->directors_notes_count);
        fputc(']', f);
    }

    /* Character invariants */
    if (persona->character_invariants_count > 0) {
        fputs(",\n  \"character_invariants\": [", f);
        write_json_string_array(f, persona->character_invariants,
                                persona->character_invariants_count);
        fputc(']', f);
    }

    /* Core anchor */
    if (persona->core_anchor) {
        fputs(",\n  \"core_anchor\": ", f);
        write_json_string(f, persona->core_anchor);
    }

    /* Intellectual profile */
    if (persona->intellectual.thinking_style || persona->intellectual.expertise_count > 0) {
        fputs(",\n  \"intellectual\": {", f);
        bool first = true;
        if (persona->intellectual.thinking_style) {
            fputs("\n    \"thinking_style\": ", f);
            write_json_string(f, persona->intellectual.thinking_style);
            first = false;
        }
        if (persona->intellectual.expertise_count > 0) {
            if (!first) fputc(',', f);
            fputs("\n    \"expertise\": [", f);
            write_json_string_array(f, persona->intellectual.expertise,
                                    persona->intellectual.expertise_count);
            fputc(']', f);
            first = false;
        }
        if (persona->intellectual.curiosity_areas_count > 0) {
            if (!first) fputc(',', f);
            fputs("\n    \"curiosity_areas\": [", f);
            write_json_string_array(f, persona->intellectual.curiosity_areas,
                                    persona->intellectual.curiosity_areas_count);
            fputc(']', f);
            first = false;
        }
        if (persona->intellectual.metaphor_sources) {
            if (!first) fputc(',', f);
            fputs("\n    \"metaphor_sources\": ", f);
            write_json_string(f, persona->intellectual.metaphor_sources);
        }
        fputs("\n  }", f);
    }

    /* Sensory preferences */
    if (persona->sensory.dominant_sense || persona->sensory.metaphor_vocabulary_count > 0) {
        fputs(",\n  \"sensory\": {", f);
        bool first = true;
        if (persona->sensory.dominant_sense) {
            fputs("\n    \"dominant_sense\": ", f);
            write_json_string(f, persona->sensory.dominant_sense);
            first = false;
        }
        if (persona->sensory.metaphor_vocabulary_count > 0) {
            if (!first) fputc(',', f);
            fputs("\n    \"metaphor_vocabulary\": [", f);
            write_json_string_array(f, persona->sensory.metaphor_vocabulary,
                                    persona->sensory.metaphor_vocabulary_count);
            fputc(']', f);
            first = false;
        }
        if (persona->sensory.grounding_patterns) {
            if (!first) fputc(',', f);
            fputs("\n    \"grounding_patterns\": ", f);
            write_json_string(f, persona->sensory.grounding_patterns);
        }
        fputs("\n  }", f);
    }

    /* Backstory behaviors */
    if (persona->backstory_behaviors_count > 0) {
        fputs(",\n  \"backstory_behaviors\": [", f);
        for (size_t i = 0; i < persona->backstory_behaviors_count; i++) {
            if (i > 0) fputc(',', f);
            fputs("\n    {\"backstory_beat\": ", f);
            write_json_string(f, persona->backstory_behaviors[i].backstory_beat);
            fputs(", \"behavioral_rule\": ", f);
            write_json_string(f, persona->backstory_behaviors[i].behavioral_rule);
            fputc('}', f);
        }
        fputs("\n  ]", f);
    }

    /* Situational directions */
    if (persona->situational_directions_count > 0) {
        fputs(",\n  \"situational_directions\": [", f);
        for (size_t i = 0; i < persona->situational_directions_count; i++) {
            if (i > 0) fputc(',', f);
            fputs("\n    {\"trigger\": ", f);
            write_json_string(f, persona->situational_directions[i].trigger);
            fputs(", \"instruction\": ", f);
            write_json_string(f, persona->situational_directions[i].instruction);
            fputc('}', f);
        }
        fputs("\n  ]", f);
    }

    /* Relational intelligence */
    {
        const hu_relational_intelligence_t *ri = &persona->relational;
        if (ri->bid_response_style || ri->attachment_style || ri->attachment_awareness ||
            ri->dunbar_awareness || ri->emotional_bids_count > 0) {
            fputs(",\n  \"relational\": {", f);
            bool first = true;
            if (ri->bid_response_style) {
                fputs("\n    \"bid_response_style\": ", f);
                write_json_string(f, ri->bid_response_style);
                first = false;
            }
            if (ri->emotional_bids_count > 0) {
                if (!first) fputc(',', f);
                fputs("\n    \"emotional_bids\": [", f);
                write_json_string_array(f, ri->emotional_bids, ri->emotional_bids_count);
                fputc(']', f);
                first = false;
            }
            if (ri->attachment_style) {
                if (!first) fputc(',', f);
                fputs("\n    \"attachment_style\": ", f);
                write_json_string(f, ri->attachment_style);
                first = false;
            }
            if (ri->attachment_awareness) {
                if (!first) fputc(',', f);
                fputs("\n    \"attachment_awareness\": ", f);
                write_json_string(f, ri->attachment_awareness);
                first = false;
            }
            if (ri->dunbar_awareness) {
                if (!first) fputc(',', f);
                fputs("\n    \"dunbar_awareness\": ", f);
                write_json_string(f, ri->dunbar_awareness);
            }
            fputs("\n  }", f);
        }
    }

    /* Listening protocol */
    {
        const hu_listening_protocol_t *lp = &persona->listening;
        if (lp->default_response_type || lp->reflective_techniques_count > 0 ||
            lp->nvc_style || lp->validation_style) {
            fputs(",\n  \"listening\": {", f);
            bool first = true;
            if (lp->default_response_type) {
                fputs("\n    \"default_response_type\": ", f);
                write_json_string(f, lp->default_response_type);
                first = false;
            }
            if (lp->reflective_techniques_count > 0) {
                if (!first) fputc(',', f);
                fputs("\n    \"reflective_techniques\": [", f);
                write_json_string_array(f, lp->reflective_techniques,
                                        lp->reflective_techniques_count);
                fputc(']', f);
                first = false;
            }
            if (lp->nvc_style) {
                if (!first) fputc(',', f);
                fputs("\n    \"nvc_style\": ", f);
                write_json_string(f, lp->nvc_style);
                first = false;
            }
            if (lp->validation_style) {
                if (!first) fputc(',', f);
                fputs("\n    \"validation_style\": ", f);
                write_json_string(f, lp->validation_style);
            }
            fputs("\n  }", f);
        }
    }

    /* Repair protocol */
    {
        const hu_repair_protocol_t *rp = &persona->repair;
        if (rp->rupture_detection || rp->repair_approach || rp->face_saving_style ||
            rp->repair_phrases_count > 0) {
            fputs(",\n  \"repair\": {", f);
            bool first = true;
            if (rp->rupture_detection) {
                fputs("\n    \"rupture_detection\": ", f);
                write_json_string(f, rp->rupture_detection);
                first = false;
            }
            if (rp->repair_approach) {
                if (!first) fputc(',', f);
                fputs("\n    \"repair_approach\": ", f);
                write_json_string(f, rp->repair_approach);
                first = false;
            }
            if (rp->face_saving_style) {
                if (!first) fputc(',', f);
                fputs("\n    \"face_saving_style\": ", f);
                write_json_string(f, rp->face_saving_style);
                first = false;
            }
            if (rp->repair_phrases_count > 0) {
                if (!first) fputc(',', f);
                fputs("\n    \"repair_phrases\": [", f);
                write_json_string_array(f, rp->repair_phrases, rp->repair_phrases_count);
                fputc(']', f);
            }
            fputs("\n  }", f);
        }
    }

    /* Linguistic mirroring */
    {
        const hu_linguistic_mirroring_t *lm = &persona->mirroring;
        if (lm->mirroring_level || lm->adapts_to_count > 0 || lm->convergence_speed ||
            lm->power_dynamic) {
            fputs(",\n  \"mirroring\": {", f);
            bool first = true;
            if (lm->mirroring_level) {
                fputs("\n    \"mirroring_level\": ", f);
                write_json_string(f, lm->mirroring_level);
                first = false;
            }
            if (lm->adapts_to_count > 0) {
                if (!first) fputc(',', f);
                fputs("\n    \"adapts_to\": [", f);
                write_json_string_array(f, lm->adapts_to, lm->adapts_to_count);
                fputc(']', f);
                first = false;
            }
            if (lm->convergence_speed) {
                if (!first) fputc(',', f);
                fputs("\n    \"convergence_speed\": ", f);
                write_json_string(f, lm->convergence_speed);
                first = false;
            }
            if (lm->power_dynamic) {
                if (!first) fputc(',', f);
                fputs("\n    \"power_dynamic\": ", f);
                write_json_string(f, lm->power_dynamic);
            }
            fputs("\n  }", f);
        }
    }

    /* Social dynamics */
    {
        const hu_social_dynamics_t *sd = &persona->social;
        if (sd->default_ego_state || sd->phatic_style || sd->bonding_behaviors_count > 0 ||
            sd->anti_patterns_count > 0) {
            fputs(",\n  \"social\": {", f);
            bool first = true;
            if (sd->default_ego_state) {
                fputs("\n    \"default_ego_state\": ", f);
                write_json_string(f, sd->default_ego_state);
                first = false;
            }
            if (sd->phatic_style) {
                if (!first) fputc(',', f);
                fputs("\n    \"phatic_style\": ", f);
                write_json_string(f, sd->phatic_style);
                first = false;
            }
            if (sd->bonding_behaviors_count > 0) {
                if (!first) fputc(',', f);
                fputs("\n    \"bonding_behaviors\": [", f);
                write_json_string_array(f, sd->bonding_behaviors, sd->bonding_behaviors_count);
                fputc(']', f);
                first = false;
            }
            if (sd->anti_patterns_count > 0) {
                if (!first) fputc(',', f);
                fputs("\n    \"anti_patterns\": [", f);
                write_json_string_array(f, sd->anti_patterns, sd->anti_patterns_count);
                fputc(']', f);
            }
            fputs("\n  }", f);
        }
    }

    /* Example banks */
    if (persona->example_banks_count > 0) {
        fputs(",\n  \"example_banks\": {\n", f);
        for (size_t bi = 0; bi < persona->example_banks_count; bi++) {
            const hu_persona_example_bank_t *bank = &persona->example_banks[bi];
            if (!bank->channel)
                continue;
            if (bi > 0)
                fputs(",\n", f);
            fputs("    ", f);
            write_json_string(f, bank->channel);
            fputs(": [\n", f);
            for (size_t ei = 0; ei < bank->examples_count; ei++) {
                if (ei > 0)
                    fputs(",\n", f);
                fputs("      {\"context\": ", f);
                write_json_string(f, bank->examples[ei].context);
                fputs(", \"incoming\": ", f);
                write_json_string(f, bank->examples[ei].incoming);
                fputs(", \"response\": ", f);
                write_json_string(f, bank->examples[ei].response);
                fputc('}', f);
            }
            fputs("\n    ]", f);
        }
        fputs("\n  }", f);
    }

    /* Contacts */
    if (persona->contacts_count > 0) {
        fputs(",\n  \"contacts\": [\n", f);
        for (size_t ci = 0; ci < persona->contacts_count; ci++) {
            const hu_contact_profile_t *c = &persona->contacts[ci];
            if (ci > 0)
                fputs(",\n", f);
            fputs("    {\n", f);
            if (c->contact_id) {
                fputs("      \"contact_id\": ", f);
                write_json_string(f, c->contact_id);
            }
            if (c->name) {
                fputs(",\n      \"name\": ", f);
                write_json_string(f, c->name);
            }
            if (c->relationship) {
                fputs(",\n      \"relationship\": ", f);
                write_json_string(f, c->relationship);
            }
            if (c->relationship_stage) {
                fputs(",\n      \"relationship_stage\": ", f);
                write_json_string(f, c->relationship_stage);
            }
            if (c->warmth_level) {
                fputs(",\n      \"warmth_level\": ", f);
                write_json_string(f, c->warmth_level);
            }
            if (c->dynamic) {
                fputs(",\n      \"dynamic\": ", f);
                write_json_string(f, c->dynamic);
            }
            if (c->greeting_style) {
                fputs(",\n      \"greeting_style\": ", f);
                write_json_string(f, c->greeting_style);
            }
            if (c->closing_style) {
                fputs(",\n      \"closing_style\": ", f);
                write_json_string(f, c->closing_style);
            }
            fprintf(f, ",\n      \"texts_in_bursts\": %s", c->texts_in_bursts ? "true" : "false");
            fprintf(f, ",\n      \"prefers_short_texts\": %s",
                    c->prefers_short_texts ? "true" : "false");
            fprintf(f, ",\n      \"uses_emoji\": %s", c->uses_emoji ? "true" : "false");
            fprintf(f, ",\n      \"sends_links_often\": %s",
                    c->sends_links_often ? "true" : "false");
            if (c->proactive_checkin) {
                fputs(",\n      \"proactive_checkin\": true", f);
                if (c->proactive_channel) {
                    fputs(",\n      \"proactive_channel\": ", f);
                    write_json_string(f, c->proactive_channel);
                }
            }
            if (c->attachment_style) {
                fputs(",\n      \"attachment_style\": ", f);
                write_json_string(f, c->attachment_style);
            }
            if (c->dunbar_layer) {
                fputs(",\n      \"dunbar_layer\": ", f);
                write_json_string(f, c->dunbar_layer);
            }
            fputs("\n    }", f);
        }
        fputs("\n  ]", f);
    }

    fputs("\n}\n", f);

    if (ferror(f)) {
        fclose(f);
        return HU_ERR_IO;
    }
    fclose(f);
    return HU_OK;
fail:
    fclose(f);
    return HU_ERR_IO;
}
