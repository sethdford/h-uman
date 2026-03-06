#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include <stdio.h>
#include <string.h>

static sc_error_t parse_string_array_from_json(sc_allocator_t *a, const sc_json_value_t *arr,
                                               char ***out, size_t *out_count) {
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

sc_error_t sc_persona_analyzer_build_prompt(const char **messages, size_t msg_count,
                                            const char *channel, char *buf, size_t cap,
                                            size_t *out_len) {
    if (!messages || !buf || !out_len || cap < 128)
        return SC_ERR_INVALID_ARGUMENT;

    size_t n = 0;
    n += (size_t)snprintf(buf + n, cap - n,
                          "Analyze these %zu message samples from channel \"%s\" and extract "
                          "personality traits, vocabulary preferences, and communication style.\n\n"
                          "Return valid JSON with: traits (array of strings), vocabulary "
                          "{preferred, avoided, slang}, communication_rules (array), "
                          "formality, avg_length, emoji_usage.\n\n"
                          "Messages:\n",
                          msg_count, channel ? channel : "unknown");
    if ((size_t)n >= cap)
        return SC_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < msg_count && n < cap; i++) {
        if (messages[i]) {
            size_t len = strlen(messages[i]);
            if (n + len + 4 > cap)
                break;
            n += (size_t)snprintf(buf + n, cap - n, "%zu. %s\n", i + 1, messages[i]);
        }
    }
    *out_len = n;
    return SC_OK;
}

sc_error_t sc_persona_analyzer_parse_response(sc_allocator_t *alloc, const char *response,
                                              size_t resp_len, const char *channel,
                                              size_t channel_len, sc_persona_t *out) {
    if (!alloc || !response || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, response, resp_len, &root);
    if (err != SC_OK || !root || root->type != SC_JSON_OBJECT) {
        if (root)
            sc_json_free(alloc, root);
        return err != SC_OK ? err : SC_ERR_JSON_PARSE;
    }

    sc_json_value_t *traits = sc_json_object_get(root, "traits");
    if (traits) {
        err = parse_string_array_from_json(alloc, traits, &out->traits, &out->traits_count);
        if (err != SC_OK) {
            sc_json_free(alloc, root);
            return err;
        }
    }

    sc_json_value_t *vocab = sc_json_object_get(root, "vocabulary");
    if (vocab && vocab->type == SC_JSON_OBJECT) {
        sc_json_value_t *pref = sc_json_object_get(vocab, "preferred");
        if (pref)
            parse_string_array_from_json(alloc, pref, &out->preferred_vocab,
                                         &out->preferred_vocab_count);
        sc_json_value_t *avoid = sc_json_object_get(vocab, "avoided");
        if (avoid)
            parse_string_array_from_json(alloc, avoid, &out->avoided_vocab,
                                         &out->avoided_vocab_count);
        sc_json_value_t *sl = sc_json_object_get(vocab, "slang");
        if (sl)
            parse_string_array_from_json(alloc, sl, &out->slang, &out->slang_count);
    }

    sc_json_value_t *rules = sc_json_object_get(root, "communication_rules");
    if (rules)
        parse_string_array_from_json(alloc, rules, &out->communication_rules,
                                     &out->communication_rules_count);

    const char *formality = sc_json_get_string(root, "formality");
    const char *avg_length = sc_json_get_string(root, "avg_length");
    const char *emoji_usage = sc_json_get_string(root, "emoji_usage");
    if (channel && channel_len > 0 && (formality || avg_length || emoji_usage)) {
        out->overlays = alloc->alloc(alloc->ctx, sizeof(sc_persona_overlay_t));
        if (!out->overlays) {
            sc_persona_deinit(alloc, out);
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memset(out->overlays, 0, sizeof(sc_persona_overlay_t));
        out->overlays_count = 1;
        out->overlays[0].channel = sc_strndup(alloc, channel, channel_len);
        if (!out->overlays[0].channel) {
            sc_persona_deinit(alloc, out);
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        if (formality) {
            out->overlays[0].formality = sc_strdup(alloc, formality);
            if (!out->overlays[0].formality) {
                sc_persona_deinit(alloc, out);
                sc_json_free(alloc, root);
                return SC_ERR_OUT_OF_MEMORY;
            }
        }
        if (avg_length) {
            out->overlays[0].avg_length = sc_strdup(alloc, avg_length);
            if (!out->overlays[0].avg_length) {
                sc_persona_deinit(alloc, out);
                sc_json_free(alloc, root);
                return SC_ERR_OUT_OF_MEMORY;
            }
        }
        if (emoji_usage) {
            out->overlays[0].emoji_usage = sc_strdup(alloc, emoji_usage);
            if (!out->overlays[0].emoji_usage) {
                sc_persona_deinit(alloc, out);
                sc_json_free(alloc, root);
                return SC_ERR_OUT_OF_MEMORY;
            }
        }
    }

    sc_json_free(alloc, root);
    return SC_OK;
}
