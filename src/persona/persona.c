#include "seaclaw/persona.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_PERSONA_PROMPT_INIT_CAP 4096
#define SC_PERSONA_PATH_MAX        512

/* --- Overlay lookup --- */

const sc_persona_overlay_t *sc_persona_find_overlay(const sc_persona_t *persona,
                                                    const char *channel, size_t channel_len) {
    if (!persona || !channel || persona->overlays_count == 0 || !persona->overlays)
        return NULL;
    for (size_t i = 0; i < persona->overlays_count; i++) {
        const sc_persona_overlay_t *ov = &persona->overlays[i];
        if (!ov->channel)
            continue;
        size_t ov_len = strlen(ov->channel);
        if (ov_len == channel_len && memcmp(ov->channel, channel, channel_len) == 0)
            return ov;
    }
    return NULL;
}

/* --- Deinit helpers --- */

static void free_string_array(sc_allocator_t *alloc, char **arr, size_t count) {
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

static void free_overlay(sc_allocator_t *alloc, sc_persona_overlay_t *ov) {
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
}

static void free_example(sc_allocator_t *alloc, sc_persona_example_t *ex) {
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

static void free_example_bank(sc_allocator_t *alloc, sc_persona_example_bank_t *bank) {
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
                    bank->examples_count * sizeof(sc_persona_example_t));
    }
}

void sc_persona_deinit(sc_allocator_t *alloc, sc_persona_t *persona) {
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

    if (persona->overlays) {
        for (size_t i = 0; i < persona->overlays_count; i++)
            free_overlay(alloc, &persona->overlays[i]);
        alloc->free(alloc->ctx, persona->overlays,
                    persona->overlays_count * sizeof(sc_persona_overlay_t));
    }

    if (persona->example_banks) {
        for (size_t i = 0; i < persona->example_banks_count; i++)
            free_example_bank(alloc, &persona->example_banks[i]);
        alloc->free(alloc->ctx, persona->example_banks,
                    persona->example_banks_count * sizeof(sc_persona_example_bank_t));
    }

    memset(persona, 0, sizeof(*persona));
}

/* --- JSON loading helpers --- */

static sc_error_t parse_string_array(sc_allocator_t *a, const sc_json_value_t *arr, char ***out,
                                     size_t *out_count) {
    if (!arr || arr->type != SC_JSON_ARRAY || !arr->data.array.items)
        return SC_OK;
    size_t n = arr->data.array.len;
    if (n == 0)
        return SC_OK;
    char **buf = (char **)a->alloc(a->ctx, n * sizeof(char *));
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        const sc_json_value_t *item = arr->data.array.items[i];
        if (!item || item->type != SC_JSON_STRING || !item->data.string.ptr)
            continue;
        char *dup = sc_strndup(a, item->data.string.ptr, item->data.string.len);
        if (!dup) {
            for (size_t j = 0; j < count; j++)
                a->free(a->ctx, buf[j], strlen(buf[j]) + 1);
            a->free(a->ctx, buf, n * sizeof(char *));
            return SC_ERR_OUT_OF_MEMORY;
        }
        buf[count++] = dup;
    }
    *out = buf;
    *out_count = count;
    return SC_OK;
}

static sc_error_t parse_overlay(sc_allocator_t *a, const char *channel_name,
                                const sc_json_value_t *obj, sc_persona_overlay_t *ov) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    ov->channel = sc_strdup(a, channel_name);
    if (!ov->channel)
        return SC_ERR_OUT_OF_MEMORY;
    const char *s = sc_json_get_string(obj, "formality");
    if (s)
        ov->formality = sc_strdup(a, s);
    s = sc_json_get_string(obj, "avg_length");
    if (s)
        ov->avg_length = sc_strdup(a, s);
    s = sc_json_get_string(obj, "emoji_usage");
    if (s)
        ov->emoji_usage = sc_strdup(a, s);
    sc_json_value_t *notes = sc_json_object_get(obj, "style_notes");
    if (notes)
        parse_string_array(a, notes, &ov->style_notes, &ov->style_notes_count);
    return SC_OK;
}

sc_error_t sc_persona_load_json(sc_allocator_t *alloc, const char *json, size_t json_len,
                                sc_persona_t *out) {
    if (!alloc || !json || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, json, json_len, &root);
    if (err != SC_OK || !root || root->type != SC_JSON_OBJECT)
        return err != SC_OK ? err : SC_ERR_JSON_PARSE;

    const char *name = sc_json_get_string(root, "name");
    if (name) {
        out->name = sc_strdup(alloc, name);
        if (!out->name) {
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        out->name_len = strlen(out->name);
    }

    sc_json_value_t *core = sc_json_object_get(root, "core");
    if (core && core->type == SC_JSON_OBJECT) {
        const char *s = sc_json_get_string(core, "identity");
        if (s) {
            out->identity = sc_strdup(alloc, s);
            if (!out->identity) {
                sc_persona_deinit(alloc, out);
                sc_json_free(alloc, root);
                return SC_ERR_OUT_OF_MEMORY;
            }
        }
        sc_json_value_t *traits = sc_json_object_get(core, "traits");
        if (traits) {
            err = parse_string_array(alloc, traits, &out->traits, &out->traits_count);
            if (err != SC_OK) {
                sc_persona_deinit(alloc, out);
                sc_json_free(alloc, root);
                return err;
            }
        }
        sc_json_value_t *vocab = sc_json_object_get(core, "vocabulary");
        if (vocab && vocab->type == SC_JSON_OBJECT) {
            sc_json_value_t *pref = sc_json_object_get(vocab, "preferred");
            if (pref) {
                err = parse_string_array(alloc, pref, &out->preferred_vocab,
                                         &out->preferred_vocab_count);
                if (err != SC_OK) {
                    sc_persona_deinit(alloc, out);
                    sc_json_free(alloc, root);
                    return err;
                }
            }
            sc_json_value_t *avoid = sc_json_object_get(vocab, "avoided");
            if (avoid) {
                err = parse_string_array(alloc, avoid, &out->avoided_vocab,
                                         &out->avoided_vocab_count);
                if (err != SC_OK) {
                    sc_persona_deinit(alloc, out);
                    sc_json_free(alloc, root);
                    return err;
                }
            }
            sc_json_value_t *sl = sc_json_object_get(vocab, "slang");
            if (sl) {
                err = parse_string_array(alloc, sl, &out->slang, &out->slang_count);
                if (err != SC_OK) {
                    sc_persona_deinit(alloc, out);
                    sc_json_free(alloc, root);
                    return err;
                }
            }
        }
        sc_json_value_t *rules = sc_json_object_get(core, "communication_rules");
        if (rules) {
            err = parse_string_array(alloc, rules, &out->communication_rules,
                                     &out->communication_rules_count);
            if (err != SC_OK) {
                sc_persona_deinit(alloc, out);
                sc_json_free(alloc, root);
                return err;
            }
        }
        sc_json_value_t *vals = sc_json_object_get(core, "values");
        if (vals) {
            err = parse_string_array(alloc, vals, &out->values, &out->values_count);
            if (err != SC_OK) {
                sc_persona_deinit(alloc, out);
                sc_json_free(alloc, root);
                return err;
            }
        }
        s = sc_json_get_string(core, "decision_style");
        if (s) {
            out->decision_style = sc_strdup(alloc, s);
            if (!out->decision_style) {
                sc_persona_deinit(alloc, out);
                sc_json_free(alloc, root);
                return SC_ERR_OUT_OF_MEMORY;
            }
        }
    }

    sc_json_value_t *overlays_obj = sc_json_object_get(root, "channel_overlays");
    if (overlays_obj && overlays_obj->type == SC_JSON_OBJECT && overlays_obj->data.object.pairs) {
        size_t n = overlays_obj->data.object.len;
        sc_persona_overlay_t *ovs =
            (sc_persona_overlay_t *)alloc->alloc(alloc->ctx, n * sizeof(sc_persona_overlay_t));
        if (!ovs) {
            sc_persona_deinit(alloc, out);
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memset(ovs, 0, n * sizeof(sc_persona_overlay_t));
        size_t count = 0;
        for (size_t i = 0; i < n; i++) {
            sc_json_pair_t *pair = &overlays_obj->data.object.pairs[i];
            if (!pair->key || !pair->value || pair->value->type != SC_JSON_OBJECT)
                continue;
            err = parse_overlay(alloc, pair->key, pair->value, &ovs[count]);
            if (err != SC_OK) {
                for (size_t j = 0; j < count; j++)
                    free_overlay(alloc, &ovs[j]);
                alloc->free(alloc->ctx, ovs, n * sizeof(sc_persona_overlay_t));
                sc_persona_deinit(alloc, out);
                sc_json_free(alloc, root);
                return err;
            }
            count++;
        }
        out->overlays = ovs;
        out->overlays_count = count;
    }

    sc_json_free(alloc, root);
    return SC_OK;
}

sc_error_t sc_persona_load(sc_allocator_t *alloc, const char *name, size_t name_len,
                           sc_persona_t *out) {
    if (!alloc || !name || !out)
        return SC_ERR_INVALID_ARGUMENT;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return SC_ERR_NOT_FOUND;
    char path[SC_PERSONA_PATH_MAX];
    int n =
        snprintf(path, sizeof(path), "%s/.seaclaw/personas/%.*s.json", home, (int)name_len, name);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return SC_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "rb");
    if (!f)
        return SC_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return SC_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > (long)(1024 * 1024)) {
        fclose(f);
        return sz < 0 ? SC_ERR_IO : SC_ERR_INVALID_ARGUMENT;
    }
    rewind(f);
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t read_len = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (read_len != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return SC_ERR_IO;
    }
    buf[read_len] = '\0';
    sc_error_t err = sc_persona_load_json(alloc, buf, read_len, out);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    return err;
}

/* --- Prompt builder --- */

static sc_error_t append_prompt(sc_allocator_t *alloc, char **buf, size_t *len, size_t *cap,
                                const char *s, size_t slen) {
    while (*len + slen + 1 > *cap) {
        size_t new_cap = *cap ? *cap * 2 : SC_PERSONA_PROMPT_INIT_CAP;
        char *nb = (char *)alloc->realloc(alloc->ctx, *buf, *cap, new_cap);
        if (!nb)
            return SC_ERR_OUT_OF_MEMORY;
        *buf = nb;
        *cap = new_cap;
    }
    memcpy(*buf + *len, s, slen);
    (*buf)[*len + slen] = '\0';
    *len += slen;
    return SC_OK;
}

sc_error_t sc_persona_build_prompt(sc_allocator_t *alloc, const sc_persona_t *persona,
                                   const char *channel, size_t channel_len, char **out,
                                   size_t *out_len) {
    if (!alloc || !persona || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    size_t cap = SC_PERSONA_PROMPT_INIT_CAP;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;
    size_t len = 0;
    buf[0] = '\0';

    const char *name = persona->name ? persona->name : "persona";
    size_t name_len = persona->name_len ? persona->name_len : strlen(name);
    char header[256];
    int n = snprintf(header, sizeof(header), "You are acting as %.*s.", (int)name_len, name);
    if (n > 0) {
        sc_error_t err = append_prompt(alloc, &buf, &len, &cap, header, (size_t)n);
        if (err != SC_OK) {
            alloc->free(alloc->ctx, buf, cap);
            return err;
        }
    }
    if (persona->identity && persona->identity[0]) {
        n = snprintf(header, sizeof(header), " %s", persona->identity);
        if (n > 0) {
            sc_error_t err = append_prompt(alloc, &buf, &len, &cap, header, (size_t)n);
            if (err != SC_OK) {
                alloc->free(alloc->ctx, buf, cap);
                return err;
            }
        }
    }
    sc_error_t err = append_prompt(alloc, &buf, &len, &cap, "\n\n", 2);
    if (err != SC_OK) {
        alloc->free(alloc->ctx, buf, cap);
        return err;
    }

    if (persona->traits && persona->traits_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Personality traits: ", 20);
        if (err != SC_OK)
            goto fail;
        for (size_t i = 0; i < persona->traits_count; i++) {
            if (i > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
                if (err != SC_OK)
                    goto fail;
            }
            const char *t = persona->traits[i];
            if (t)
                err = append_prompt(alloc, &buf, &len, &cap, t, strlen(t));
            if (err != SC_OK)
                goto fail;
        }
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != SC_OK)
            goto fail;
    }

    if (persona->preferred_vocab && persona->preferred_vocab_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Preferred vocabulary: ", 22);
        if (err != SC_OK)
            goto fail;
        for (size_t i = 0; i < persona->preferred_vocab_count; i++) {
            if (i > 0)
                err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
            else
                err = SC_OK;
            if (err != SC_OK)
                goto fail;
            const char *v = persona->preferred_vocab[i];
            if (v)
                err = append_prompt(alloc, &buf, &len, &cap, v, strlen(v));
            if (err != SC_OK)
                goto fail;
        }
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != SC_OK)
            goto fail;
    }

    if (persona->avoided_vocab && persona->avoided_vocab_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Avoid: ", 7);
        if (err != SC_OK)
            goto fail;
        for (size_t i = 0; i < persona->avoided_vocab_count; i++) {
            if (i > 0)
                err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
            else
                err = SC_OK;
            if (err != SC_OK)
                goto fail;
            const char *v = persona->avoided_vocab[i];
            if (v)
                err = append_prompt(alloc, &buf, &len, &cap, v, strlen(v));
            if (err != SC_OK)
                goto fail;
        }
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != SC_OK)
            goto fail;
    }

    if (persona->slang && persona->slang_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Slang: ", 7);
        if (err != SC_OK)
            goto fail;
        for (size_t i = 0; i < persona->slang_count; i++) {
            if (i > 0)
                err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
            else
                err = SC_OK;
            if (err != SC_OK)
                goto fail;
            const char *s = persona->slang[i];
            if (s)
                err = append_prompt(alloc, &buf, &len, &cap, s, strlen(s));
            if (err != SC_OK)
                goto fail;
        }
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != SC_OK)
            goto fail;
    }

    if (persona->communication_rules && persona->communication_rules_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Communication rules:\n", 21);
        if (err != SC_OK)
            goto fail;
        for (size_t i = 0; i < persona->communication_rules_count; i++) {
            const char *r = persona->communication_rules[i];
            if (r) {
                err = append_prompt(alloc, &buf, &len, &cap, "- ", 2);
                if (err != SC_OK)
                    goto fail;
                err = append_prompt(alloc, &buf, &len, &cap, r, strlen(r));
                if (err != SC_OK)
                    goto fail;
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != SC_OK)
                    goto fail;
            }
        }
    }

    if (persona->values && persona->values_count > 0) {
        err = append_prompt(alloc, &buf, &len, &cap, "Values: ", 8);
        if (err != SC_OK)
            goto fail;
        for (size_t i = 0; i < persona->values_count; i++) {
            if (i > 0)
                err = append_prompt(alloc, &buf, &len, &cap, ", ", 2);
            else
                err = SC_OK;
            if (err != SC_OK)
                goto fail;
            const char *v = persona->values[i];
            if (v)
                err = append_prompt(alloc, &buf, &len, &cap, v, strlen(v));
            if (err != SC_OK)
                goto fail;
        }
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != SC_OK)
            goto fail;
    }

    if (persona->decision_style && persona->decision_style[0]) {
        err = append_prompt(alloc, &buf, &len, &cap, "Decision style: ", 16);
        if (err != SC_OK)
            goto fail;
        err = append_prompt(alloc, &buf, &len, &cap, persona->decision_style,
                            strlen(persona->decision_style));
        if (err != SC_OK)
            goto fail;
        err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
        if (err != SC_OK)
            goto fail;
    }

    err = append_prompt(alloc, &buf, &len, &cap,
                        "Match this style naturally. Don't exaggerate traits — aim for "
                        "authenticity, not caricature.\n\n",
                        87);
    if (err != SC_OK)
        goto fail;

    if (channel && channel_len > 0) {
        const sc_persona_overlay_t *ov = sc_persona_find_overlay(persona, channel, channel_len);
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
            if (err != SC_OK)
                goto fail;
            if (ov->formality && ov->formality[0]) {
                n = snprintf(header, sizeof(header), "- Formality: %s\n", ov->formality);
                if (n > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, header, (size_t)n);
                    if (err != SC_OK)
                        goto fail;
                }
            }
            if (ov->avg_length && ov->avg_length[0]) {
                n = snprintf(header, sizeof(header), "- Avg length: %s\n", ov->avg_length);
                if (n > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, header, (size_t)n);
                    if (err != SC_OK)
                        goto fail;
                }
            }
            if (ov->emoji_usage && ov->emoji_usage[0]) {
                n = snprintf(header, sizeof(header), "- Emoji usage: %s\n", ov->emoji_usage);
                if (n > 0) {
                    err = append_prompt(alloc, &buf, &len, &cap, header, (size_t)n);
                    if (err != SC_OK)
                        goto fail;
                }
            }
            if (ov->style_notes && ov->style_notes_count > 0) {
                err = append_prompt(alloc, &buf, &len, &cap, "- Style notes: ", 15);
                if (err != SC_OK)
                    goto fail;
                for (size_t i = 0; i < ov->style_notes_count; i++) {
                    if (i > 0)
                        err = append_prompt(alloc, &buf, &len, &cap, "; ", 2);
                    else
                        err = SC_OK;
                    if (err != SC_OK)
                        goto fail;
                    const char *sn = ov->style_notes[i];
                    if (sn)
                        err = append_prompt(alloc, &buf, &len, &cap, sn, strlen(sn));
                    if (err != SC_OK)
                        goto fail;
                }
                err = append_prompt(alloc, &buf, &len, &cap, "\n", 1);
                if (err != SC_OK)
                    goto fail;
            }
        }
    }

    *out = buf;
    *out_len = len;
    return SC_OK;
fail:
    alloc->free(alloc->ctx, buf, cap);
    return err;
}
