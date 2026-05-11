#include "human/core/json.h"
#include "human/core/string.h"
#include "human/persona.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define HU_PERSONA_EXAMPLES_MAX 256

/* Parse examples array items into a heap block; on zero valid rows frees the block. */
static hu_error_t persona_examples_fill_from_array(hu_allocator_t *alloc,
                                                   const hu_json_value_t *arr,
                                                   hu_persona_example_t **out_examples,
                                                   size_t *out_count) {
    *out_examples = NULL;
    *out_count = 0;
    if (!arr || arr->type != HU_JSON_ARRAY)
        return HU_OK;
    size_t n = arr->data.array.len;
    if (n == 0)
        return HU_OK;
    if (!arr->data.array.items)
        return HU_ERR_JSON_PARSE;
    if (n > 10000 || n > SIZE_MAX / sizeof(hu_persona_example_t))
        return HU_ERR_INVALID_ARGUMENT;

    hu_persona_example_t *examples =
        (hu_persona_example_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_persona_example_t));
    if (!examples)
        return HU_ERR_OUT_OF_MEMORY;
    memset(examples, 0, n * sizeof(hu_persona_example_t));
    size_t count = 0;

    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = arr->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        const char *ctx = hu_json_get_string(item, "context");
        const char *inc = hu_json_get_string(item, "incoming");
        const char *resp = hu_json_get_string(item, "response");
        /* Accept input/output as alternate field names */
        if (!inc)
            inc = hu_json_get_string(item, "input");
        if (!resp)
            resp = hu_json_get_string(item, "output");
        if (!ctx)
            ctx = "";
        if (!inc || !resp)
            continue;
        examples[count].context = hu_strdup(alloc, ctx);
        examples[count].incoming = hu_strdup(alloc, inc);
        examples[count].response = hu_strdup(alloc, resp);
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
            alloc->free(alloc->ctx, examples, n * sizeof(hu_persona_example_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        count++;
    }

    if (count == 0) {
        alloc->free(alloc->ctx, examples, n * sizeof(hu_persona_example_t));
        return HU_OK;
    }
    *out_examples = examples;
    *out_count = count;
    return HU_OK;
}

/* Parse example bank from JSON. Format: {"examples":[{context,incoming,response},...]} */
hu_error_t hu_persona_examples_load_json(hu_allocator_t *alloc, const char *channel,
                                         size_t channel_len, const char *json, size_t json_len,
                                         hu_persona_example_bank_t *out) {
    if (!alloc || !channel || !json || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    out->channel = hu_strndup(alloc, channel, channel_len);
    if (!out->channel)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        alloc->free(alloc->ctx, out->channel, channel_len + 1);
        out->channel = NULL;
        return err != HU_OK ? err : HU_ERR_JSON_PARSE;
    }

    hu_json_value_t *arr = hu_json_object_get(root, "examples");
    if (!arr || arr->type != HU_JSON_ARRAY || !arr->data.array.items) {
        alloc->free(alloc->ctx, out->channel, channel_len + 1);
        out->channel = NULL;
        hu_json_free(alloc, root);
        return HU_OK;
    }

    err = persona_examples_fill_from_array(alloc, arr, &out->examples, &out->examples_count);
    hu_json_free(alloc, root);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, out->channel, channel_len + 1);
        out->channel = NULL;
        return err;
    }
    return HU_OK;
}

hu_error_t hu_persona_examples_bank_from_array(hu_allocator_t *alloc, const char *channel,
                                               size_t channel_len,
                                               const hu_json_value_t *examples_arr,
                                               hu_persona_example_bank_t *out) {
    if (!alloc || !channel || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->channel = hu_strndup(alloc, channel, channel_len);
    if (!out->channel)
        return HU_ERR_OUT_OF_MEMORY;
    hu_error_t err =
        persona_examples_fill_from_array(alloc, examples_arr, &out->examples, &out->examples_count);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, out->channel, strlen(out->channel) + 1);
        out->channel = NULL;
        return err;
    }
    if (out->examples_count == 0) {
        alloc->free(alloc->ctx, out->channel, strlen(out->channel) + 1);
        out->channel = NULL;
    }
    return HU_OK;
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

hu_error_t hu_persona_select_examples(const hu_persona_t *persona, const char *channel,
                                      size_t channel_len, const char *topic, size_t topic_len,
                                      const hu_persona_example_t **out, size_t *out_count,
                                      size_t max_examples) {
    if (!persona || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    if (!persona->example_banks || persona->example_banks_count == 0)
        return HU_OK;
    if (!channel || channel_len == 0)
        return HU_OK;

    /* Find matching bank */
    hu_persona_example_bank_t *bank = NULL;
    for (size_t i = 0; i < persona->example_banks_count; i++) {
        if (persona->example_banks[i].channel &&
            strlen(persona->example_banks[i].channel) == channel_len &&
            memcmp(persona->example_banks[i].channel, channel, channel_len) == 0) {
            bank = &persona->example_banks[i];
            break;
        }
    }
    if (!bank || !bank->examples || bank->examples_count == 0)
        return HU_OK;

    /* Score each example by keyword overlap */
    size_t n = bank->examples_count;
    if (n > HU_PERSONA_EXAMPLES_MAX)
        n = HU_PERSONA_EXAMPLES_MAX;
    struct {
        size_t idx;
        size_t score;
    } scores[HU_PERSONA_EXAMPLES_MAX];

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
    return HU_OK;
}

/* M3 Bridge A.0 — persona example bank → Alpaca JSONL exporter.
 *
 * Writes one JSON object per line in the de-facto-standard "Alpaca"
 * shape that llama.cpp/finetune, axolotl, unsloth, mlx-lm.lora, and
 * most open-weight fine-tuning toolchains consume directly:
 *
 *   {"instruction":"<channel + context>","input":"<incoming>",
 *    "output":"<response>"}
 *
 * The instruction string is composed as "On <channel>: <context>" when
 * a context is present, or just "On <channel>:" otherwise. This carries
 * the channel-specific frame that the persona example was authored for
 * (Telegram-casual vs. email-formal vs. CLI-terse, etc.) without
 * requiring a separate `system` field that not every toolchain accepts.
 *
 * Examples missing either `incoming` or `response` are skipped (they
 * can't form a valid training pair). On success, `*exported_count` is
 * the number of rows actually written; the file is truncated and the
 * caller-supplied path's parent must exist (we don't mkdir).
 *
 * This function is the "export side" of M3 Bridge A: a user runs
 *   human ml lora-persona --persona seth --export-jsonl seth.jsonl
 * to materialise the bank, then feeds seth.jsonl to llama.cpp/finetune
 * (or any compatible toolchain) to produce a real GGUF LoRA adapter
 * that the daemon's personalization block can then load via
 * hu_provider_load_adapter. Closes the loop end-to-end without
 * requiring llama.cpp to be vendored in-tree. */
hu_error_t hu_persona_bank_export_jsonl(const hu_persona_t *persona,
                                         const char *path, size_t path_len,
                                         size_t *exported_count) {
    if (!persona || !path || path_len == 0 || !exported_count)
        return HU_ERR_INVALID_ARGUMENT;
    *exported_count = 0;

    /* Bound the path on the stack; 1023 chars is well above any
     * reasonable filesystem limit and saves a heap allocation. */
    char filepath[1024];
    size_t flen = path_len < sizeof(filepath) - 1 ? path_len : sizeof(filepath) - 1;
    memcpy(filepath, path, flen);
    filepath[flen] = '\0';

    FILE *f = fopen(filepath, "w");
    if (!f)
        return HU_ERR_IO;

    /* Inline JSON-string emitter — same escape set as the DPO exporter
     * (matches RFC 8259 for the characters tooling will actually choke
     * on). Walks the input byte-by-byte; no allocation. */
    #define WRITE_JSON_STRING(s) do {                                   \
        const char *_p = (s) ? (s) : "";                                \
        for (; *_p; _p++) {                                             \
            switch (*_p) {                                              \
            case '"':  fputs("\\\"", f); break;                         \
            case '\\': fputs("\\\\", f); break;                         \
            case '\n': fputs("\\n", f);  break;                         \
            case '\r': fputs("\\r", f);  break;                         \
            case '\t': fputs("\\t", f);  break;                         \
            default:   fputc(*_p, f);    break;                         \
            }                                                           \
        }                                                               \
    } while (0)

    size_t total = 0;
    for (size_t bi = 0; bi < persona->example_banks_count; bi++) {
        const hu_persona_example_bank_t *bank = &persona->example_banks[bi];
        const char *channel = bank->channel ? bank->channel : "default";
        for (size_t ei = 0; ei < bank->examples_count; ei++) {
            const hu_persona_example_t *ex = &bank->examples[ei];
            if (!ex->incoming || !ex->incoming[0] || !ex->response || !ex->response[0])
                continue;

            fputs("{\"instruction\":\"On ", f);
            WRITE_JSON_STRING(channel);
            if (ex->context && ex->context[0]) {
                fputs(": ", f);
                WRITE_JSON_STRING(ex->context);
            } else {
                fputc(':', f);
            }
            fputs("\",\"input\":\"", f);
            WRITE_JSON_STRING(ex->incoming);
            fputs("\",\"output\":\"", f);
            WRITE_JSON_STRING(ex->response);
            fputs("\"}\n", f);
            total++;
        }
    }
    #undef WRITE_JSON_STRING

    int close_err = fclose(f);
    if (close_err != 0)
        return HU_ERR_IO;

    *exported_count = total;
    return HU_OK;
}
