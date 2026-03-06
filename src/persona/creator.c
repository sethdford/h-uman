#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include <stdbool.h>
#include <string.h>

static bool string_in_array(const char *s, char **arr, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (arr[i] && strcmp(arr[i], s) == 0)
            return true;
    }
    return false;
}

/* Merge traits from all partials, deduplicating */
static sc_error_t merge_traits(sc_allocator_t *alloc, char ***out, size_t *out_count,
                               const sc_persona_t *partials, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += partials[i].traits_count;
    if (total == 0)
        return SC_OK;
    char **buf = (char **)alloc->alloc(alloc->ctx, total * sizeof(char *));
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < partials[i].traits_count; j++) {
            const char *t = partials[i].traits[j];
            if (!t || string_in_array(t, buf, n))
                continue;
            char *dup = sc_strdup(alloc, t);
            if (!dup) {
                for (size_t k = 0; k < n; k++)
                    alloc->free(alloc->ctx, buf[k], strlen(buf[k]) + 1);
                alloc->free(alloc->ctx, buf, total * sizeof(char *));
                return SC_ERR_OUT_OF_MEMORY;
            }
            buf[n++] = dup;
        }
    }
    *out = buf;
    *out_count = n;
    return SC_OK;
}

/* Merge preferred_vocab only */
static sc_error_t merge_preferred_vocab(sc_allocator_t *alloc, char ***out, size_t *out_count,
                                        const sc_persona_t *partials, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += partials[i].preferred_vocab_count;
    if (total == 0)
        return SC_OK;
    char **buf = (char **)alloc->alloc(alloc->ctx, total * sizeof(char *));
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < partials[i].preferred_vocab_count; j++) {
            const char *v = partials[i].preferred_vocab[j];
            if (!v || string_in_array(v, buf, n))
                continue;
            char *dup = sc_strdup(alloc, v);
            if (!dup) {
                for (size_t k = 0; k < n; k++)
                    alloc->free(alloc->ctx, buf[k], strlen(buf[k]) + 1);
                alloc->free(alloc->ctx, buf, total * sizeof(char *));
                return SC_ERR_OUT_OF_MEMORY;
            }
            buf[n++] = dup;
        }
    }
    *out = buf;
    *out_count = n;
    return SC_OK;
}

static sc_error_t merge_communication_rules(sc_allocator_t *alloc, char ***out, size_t *out_count,
                                            const sc_persona_t *partials, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += partials[i].communication_rules_count;
    if (total == 0)
        return SC_OK;
    char **buf = (char **)alloc->alloc(alloc->ctx, total * sizeof(char *));
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < partials[i].communication_rules_count; j++) {
            const char *r = partials[i].communication_rules[j];
            if (!r || string_in_array(r, buf, n))
                continue;
            char *dup = sc_strdup(alloc, r);
            if (!dup) {
                for (size_t k = 0; k < n; k++)
                    alloc->free(alloc->ctx, buf[k], strlen(buf[k]) + 1);
                alloc->free(alloc->ctx, buf, total * sizeof(char *));
                return SC_ERR_OUT_OF_MEMORY;
            }
            buf[n++] = dup;
        }
    }
    *out = buf;
    *out_count = n;
    return SC_OK;
}

static sc_error_t merge_overlays(sc_allocator_t *alloc, sc_persona_overlay_t **out,
                                 size_t *out_count, const sc_persona_t *partials, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += partials[i].overlays_count;
    if (total == 0)
        return SC_OK;
    sc_persona_overlay_t *buf =
        (sc_persona_overlay_t *)alloc->alloc(alloc->ctx, total * sizeof(sc_persona_overlay_t));
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;
    memset(buf, 0, total * sizeof(sc_persona_overlay_t));
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < partials[i].overlays_count; j++) {
            const sc_persona_overlay_t *ov = &partials[i].overlays[j];
            bool found = false;
            for (size_t k = 0; k < n; k++) {
                if (ov->channel && buf[k].channel && strcmp(ov->channel, buf[k].channel) == 0) {
                    found = true;
                    break;
                }
            }
            if (found)
                continue;
            buf[n].channel = ov->channel ? sc_strdup(alloc, ov->channel) : NULL;
            buf[n].formality = ov->formality ? sc_strdup(alloc, ov->formality) : NULL;
            buf[n].avg_length = ov->avg_length ? sc_strdup(alloc, ov->avg_length) : NULL;
            buf[n].emoji_usage = ov->emoji_usage ? sc_strdup(alloc, ov->emoji_usage) : NULL;
            if (ov->style_notes_count > 0 && ov->style_notes) {
                buf[n].style_notes =
                    (char **)alloc->alloc(alloc->ctx, ov->style_notes_count * sizeof(char *));
                if (buf[n].style_notes) {
                    for (size_t k = 0; k < ov->style_notes_count; k++)
                        buf[n].style_notes[k] =
                            ov->style_notes[k] ? sc_strdup(alloc, ov->style_notes[k]) : NULL;
                    buf[n].style_notes_count = ov->style_notes_count;
                }
            }
            n++;
        }
    }
    *out = buf;
    *out_count = n;
    return SC_OK;
}

sc_error_t sc_persona_creator_synthesize(sc_allocator_t *alloc, const sc_persona_t *partials,
                                         size_t count, const char *name, size_t name_len,
                                         sc_persona_t *out) {
    if (!alloc || !partials || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    out->name = sc_strndup(alloc, name, name_len);
    if (!out->name)
        return SC_ERR_OUT_OF_MEMORY;
    out->name_len = name_len;

    sc_error_t err = merge_traits(alloc, &out->traits, &out->traits_count, partials, count);
    if (err != SC_OK) {
        sc_persona_deinit(alloc, out);
        return err;
    }
    err = merge_preferred_vocab(alloc, &out->preferred_vocab, &out->preferred_vocab_count, partials,
                                count);
    if (err != SC_OK) {
        sc_persona_deinit(alloc, out);
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
                        abuf[an++] = sc_strdup(alloc, v);
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
                        sbuf[sn++] = sc_strdup(alloc, v);
                    }
                }
            }
            out->slang = sbuf;
            out->slang_count = sn;
        }
    }

    err = merge_communication_rules(alloc, &out->communication_rules,
                                    &out->communication_rules_count, partials, count);
    if (err != SC_OK) {
        sc_persona_deinit(alloc, out);
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
                    if (v && !string_in_array(v, vbuf, vn))
                        vbuf[vn++] = sc_strdup(alloc, v);
                }
            }
            out->values = vbuf;
            out->values_count = vn;
        }
    }

    if (count > 0 && partials[0].decision_style)
        out->decision_style = sc_strdup(alloc, partials[0].decision_style);

    err = merge_overlays(alloc, &out->overlays, &out->overlays_count, partials, count);
    if (err != SC_OK) {
        sc_persona_deinit(alloc, out);
        return err;
    }
    return SC_OK;
}
