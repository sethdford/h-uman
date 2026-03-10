#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include <string.h>
#include <strings.h>

#define SC_PERSONA_EXAMPLES_MAX 256

/* Parse example bank from JSON. Format: {"examples":[{context,incoming,response},...]} */
sc_error_t sc_persona_examples_load_json(sc_allocator_t *alloc, const char *channel,
                                         size_t channel_len, const char *json, size_t json_len,
                                         sc_persona_example_bank_t *out) {
    if (!alloc || !channel || !json || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    out->channel = sc_strndup(alloc, channel, channel_len);
    if (!out->channel)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, json, json_len, &root);
    if (err != SC_OK || !root || root->type != SC_JSON_OBJECT) {
        alloc->free(alloc->ctx, out->channel, channel_len + 1);
        out->channel = NULL;
        return err != SC_OK ? err : SC_ERR_JSON_PARSE;
    }

    sc_json_value_t *arr = sc_json_object_get(root, "examples");
    if (!arr || arr->type != SC_JSON_ARRAY || !arr->data.array.items) {
        alloc->free(alloc->ctx, out->channel, channel_len + 1);
        out->channel = NULL;
        sc_json_free(alloc, root);
        return SC_OK;
    }

    size_t n = arr->data.array.len;
    sc_persona_example_t *examples =
        (sc_persona_example_t *)alloc->alloc(alloc->ctx, n * sizeof(sc_persona_example_t));
    if (!examples) {
        sc_json_free(alloc, root);
        alloc->free(alloc->ctx, out->channel, channel_len + 1);
        out->channel = NULL;
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(examples, 0, n * sizeof(sc_persona_example_t));
    size_t count = 0;

    for (size_t i = 0; i < n; i++) {
        sc_json_value_t *item = arr->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT)
            continue;
        const char *ctx = sc_json_get_string(item, "context");
        const char *inc = sc_json_get_string(item, "incoming");
        const char *resp = sc_json_get_string(item, "response");
        /* Accept input/output as alternate field names */
        if (!inc)
            inc = sc_json_get_string(item, "input");
        if (!resp)
            resp = sc_json_get_string(item, "output");
        if (!ctx)
            ctx = "";
        if (!inc || !resp)
            continue;
        examples[count].context = sc_strdup(alloc, ctx);
        examples[count].incoming = sc_strdup(alloc, inc);
        examples[count].response = sc_strdup(alloc, resp);
        if (!examples[count].context || !examples[count].incoming || !examples[count].response) {
            if (examples[count].context)
                alloc->free(alloc->ctx, examples[count].context,
                            strlen(examples[count].context) + 1);
            if (examples[count].incoming)
                alloc->free(alloc->ctx, examples[count].incoming,
                            strlen(examples[count].incoming) + 1);
            if (examples[count].response)
                alloc->free(alloc->ctx, examples[count].response,
                            strlen(examples[count].response) + 1);
            for (size_t j = 0; j < count; j++) {
                if (examples[j].context)
                    alloc->free(alloc->ctx, examples[j].context, strlen(examples[j].context) + 1);
                if (examples[j].incoming)
                    alloc->free(alloc->ctx, examples[j].incoming, strlen(examples[j].incoming) + 1);
                if (examples[j].response)
                    alloc->free(alloc->ctx, examples[j].response, strlen(examples[j].response) + 1);
            }
            alloc->free(alloc->ctx, examples, n * sizeof(sc_persona_example_t));
            sc_json_free(alloc, root);
            alloc->free(alloc->ctx, out->channel, channel_len + 1);
            out->channel = NULL;
            return SC_ERR_OUT_OF_MEMORY;
        }
        count++;
    }

    out->examples = examples;
    out->examples_count = count;
    sc_json_free(alloc, root);
    return SC_OK;
}

/* Count how many words from topic appear in context (case-insensitive, space-separated) */
static size_t keyword_overlap(const char *topic, size_t topic_len, const char *context) {
    if (!topic || topic_len == 0 || !context)
        return 0;
    size_t score = 0;
    const char *p = topic;
    const char *end = topic + topic_len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (p >= end)
            break;
        const char *word_start = p;
        while (p < end && *p != ' ' && *p != '\t')
            p++;
        size_t word_len = (size_t)(p - word_start);
        if (word_len == 0)
            continue;
        /* Check if this word appears in context (substring, case-insensitive) */
        const char *c = context;
        while (c[0] && (size_t)(strlen(c)) >= word_len) {
            if (strncasecmp(c, word_start, word_len) == 0) {
                char next = c[word_len];
                if (next == '\0' || next == ' ' || next == '\t' || next == ',' || next == '.' ||
                    next == ';') {
                    score++;
                    break;
                }
            }
            c++;
        }
    }
    return score;
}

sc_error_t sc_persona_select_examples(const sc_persona_t *persona, const char *channel,
                                      size_t channel_len, const char *topic, size_t topic_len,
                                      const sc_persona_example_t **out, size_t *out_count,
                                      size_t max_examples) {
    if (!persona || !out || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    if (!persona->example_banks || persona->example_banks_count == 0)
        return SC_OK;
    if (!channel || channel_len == 0)
        return SC_OK;

    /* Find matching bank */
    sc_persona_example_bank_t *bank = NULL;
    for (size_t i = 0; i < persona->example_banks_count; i++) {
        if (persona->example_banks[i].channel &&
            strlen(persona->example_banks[i].channel) == channel_len &&
            memcmp(persona->example_banks[i].channel, channel, channel_len) == 0) {
            bank = &persona->example_banks[i];
            break;
        }
    }
    if (!bank || !bank->examples || bank->examples_count == 0)
        return SC_OK;

    /* Score each example by keyword overlap */
    size_t n = bank->examples_count;
    if (n > SC_PERSONA_EXAMPLES_MAX)
        n = SC_PERSONA_EXAMPLES_MAX;
    struct {
        size_t idx;
        size_t score;
    } scores[SC_PERSONA_EXAMPLES_MAX];

    for (size_t i = 0; i < n; i++) {
        scores[i].idx = i;
        scores[i].score = 0;
        if (topic && topic_len > 0 && bank->examples[i].context)
            scores[i].score = keyword_overlap(topic, topic_len, bank->examples[i].context);
        else
            scores[i].score = 1; /* No topic: give all equal weight so we still return some */
    }

    /* Sort by score descending (simple bubble for small n) */
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (scores[j].score > scores[i].score) {
                size_t tmp_idx = scores[i].idx;
                size_t tmp_sc = scores[i].score;
                scores[i].idx = scores[j].idx;
                scores[i].score = scores[j].score;
                scores[j].idx = tmp_idx;
                scores[j].score = tmp_sc;
            }
        }
    }

    size_t take = max_examples < n ? max_examples : n;
    for (size_t i = 0; i < take; i++)
        out[i] = &bank->examples[scores[i].idx];
    *out_count = take;
    return SC_OK;
}
