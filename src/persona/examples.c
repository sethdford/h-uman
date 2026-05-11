#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/ml/training_data_quality.h"
#include "human/persona.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#define HU_PERSONA_EXAMPLES_MAX 256

/* Phase A1.3 — caps for hu_persona_banks_extract_from_history. */
#define HU_PERSONA_BANKS_FROM_HIST_DEFAULT_MAX_PER_CHANNEL 32u
#define HU_PERSONA_BANKS_FROM_HIST_MAX_CHANNELS            32u

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

/* ── Phase A1.3 — example bank generator from history ──────────────────── */

void hu_persona_example_banks_free(hu_allocator_t *alloc,
                                   hu_persona_example_bank_t *banks,
                                   size_t banks_count) {
    if (!alloc || !banks || banks_count == 0)
        return;
    for (size_t bi = 0; bi < banks_count; bi++) {
        hu_persona_example_bank_t *bank = &banks[bi];
        if (bank->channel) {
            alloc->free(alloc->ctx, bank->channel, strlen(bank->channel) + 1);
            bank->channel = NULL;
        }
        if (bank->examples) {
            for (size_t ei = 0; ei < bank->examples_count; ei++) {
                hu_persona_example_t *ex = &bank->examples[ei];
                if (ex->context)
                    alloc->free(alloc->ctx, ex->context, strlen(ex->context) + 1);
                if (ex->incoming)
                    alloc->free(alloc->ctx, ex->incoming, strlen(ex->incoming) + 1);
                if (ex->response)
                    alloc->free(alloc->ctx, ex->response, strlen(ex->response) + 1);
            }
            alloc->free(alloc->ctx, bank->examples,
                        bank->examples_count * sizeof(hu_persona_example_t));
            bank->examples = NULL;
        }
        bank->examples_count = 0;
    }
    alloc->free(alloc->ctx, banks, banks_count * sizeof(hu_persona_example_bank_t));
}

#ifdef HU_ENABLE_SQLITE

/* Internal scratch — a growing bank with capacity tracked separately
 * so we can realloc-style grow during scan without touching the
 * public examples_count field. */
typedef struct hu_pbh_bank {
    char *channel;
    hu_persona_example_t *examples;
    size_t count;
    size_t cap;
} hu_pbh_bank_t;

/* Find an existing bank by channel name, or initialize a new slot in
 * place. Returns the bank pointer on success; NULL on alloc failure
 * or when the per-extraction channel cap is exhausted (caller treats
 * NULL as "skip this pair"). `*banks_count_inout` advances when a
 * new slot is initialized. */
static hu_pbh_bank_t *pbh_find_or_init_bank(hu_allocator_t *alloc,
                                            hu_pbh_bank_t *banks,
                                            size_t *banks_count_inout,
                                            size_t banks_cap,
                                            const char *channel,
                                            size_t channel_len) {
    for (size_t i = 0; i < *banks_count_inout; i++) {
        if (banks[i].channel &&
            strlen(banks[i].channel) == channel_len &&
            memcmp(banks[i].channel, channel, channel_len) == 0) {
            return &banks[i];
        }
    }
    if (*banks_count_inout >= banks_cap)
        return NULL;
    hu_pbh_bank_t *slot = &banks[*banks_count_inout];
    memset(slot, 0, sizeof(*slot));
    slot->channel = hu_strndup(alloc, channel, channel_len);
    if (!slot->channel)
        return NULL;
    (*banks_count_inout)++;
    return slot;
}

/* Append one example to a scratch bank, growing the examples vector
 * by doubling. Takes ownership of ctx_dup/inc_dup/resp_dup on
 * success; on failure the caller still owns them (and must free).
 * Returns HU_OK / HU_ERR_OUT_OF_MEMORY. */
static hu_error_t pbh_bank_append(hu_allocator_t *alloc,
                                  hu_pbh_bank_t *bank,
                                  char *ctx_dup, char *inc_dup, char *resp_dup) {
    if (bank->count >= bank->cap) {
        size_t new_cap = bank->cap == 0 ? 8 : bank->cap * 2;
        size_t old_size = bank->cap * sizeof(hu_persona_example_t);
        size_t new_size = new_cap * sizeof(hu_persona_example_t);
        hu_persona_example_t *tmp = (hu_persona_example_t *)alloc->alloc(alloc->ctx, new_size);
        if (!tmp)
            return HU_ERR_OUT_OF_MEMORY;
        memset(tmp, 0, new_size);
        if (bank->examples) {
            memcpy(tmp, bank->examples, bank->count * sizeof(hu_persona_example_t));
            alloc->free(alloc->ctx, bank->examples, old_size);
        }
        bank->examples = tmp;
        bank->cap = new_cap;
    }
    hu_persona_example_t *ex = &bank->examples[bank->count];
    ex->context = ctx_dup;
    ex->incoming = inc_dup;
    ex->response = resp_dup;
    bank->count++;
    return HU_OK;
}

/* Free everything inside a scratch banks array (channel + examples +
 * vector). Safe on partially-initialized arrays. Does NOT free the
 * outer `banks` block — that's stack-allocated. */
static void pbh_banks_free_contents(hu_allocator_t *alloc,
                                    hu_pbh_bank_t *banks, size_t count) {
    if (!alloc || !banks || count == 0)
        return;
    for (size_t i = 0; i < count; i++) {
        if (banks[i].channel) {
            alloc->free(alloc->ctx, banks[i].channel, strlen(banks[i].channel) + 1);
            banks[i].channel = NULL;
        }
        if (banks[i].examples) {
            for (size_t j = 0; j < banks[i].count; j++) {
                hu_persona_example_t *ex = &banks[i].examples[j];
                if (ex->context)
                    alloc->free(alloc->ctx, ex->context, strlen(ex->context) + 1);
                if (ex->incoming)
                    alloc->free(alloc->ctx, ex->incoming, strlen(ex->incoming) + 1);
                if (ex->response)
                    alloc->free(alloc->ctx, ex->response, strlen(ex->response) + 1);
            }
            alloc->free(alloc->ctx, banks[i].examples,
                        banks[i].cap * sizeof(hu_persona_example_t));
            banks[i].examples = NULL;
        }
        banks[i].count = 0;
        banks[i].cap = 0;
    }
}

/* Extract the channel substring from a session_id of the form
 * "<channel>:<id>". On hit, sets *out / *out_len to the prefix
 * (no trailing ':'). On miss (no ':'), falls back to "default".
 * Both pointers are guaranteed non-NULL with non-zero length. */
static void pbh_channel_from_session(const char *session_id,
                                     const char **out, size_t *out_len) {
    static const char DEFAULT_CHANNEL[] = "default";
    if (!session_id || !session_id[0]) {
        *out = DEFAULT_CHANNEL;
        *out_len = sizeof(DEFAULT_CHANNEL) - 1;
        return;
    }
    const char *colon = strchr(session_id, ':');
    if (!colon || colon == session_id) {
        *out = DEFAULT_CHANNEL;
        *out_len = sizeof(DEFAULT_CHANNEL) - 1;
        return;
    }
    *out = session_id;
    *out_len = (size_t)(colon - session_id);
}

#endif /* HU_ENABLE_SQLITE */

hu_error_t hu_persona_banks_extract_from_history(hu_allocator_t *alloc,
                                                 const char *db_path,
                                                 size_t max_per_channel,
                                                 hu_persona_example_bank_t **out_banks,
                                                 size_t *out_count) {
    if (!alloc || !db_path || !out_banks || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_banks = NULL;
    *out_count = 0;

#ifndef HU_ENABLE_SQLITE
    (void)max_per_channel;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (max_per_channel == 0)
        max_per_channel = HU_PERSONA_BANKS_FROM_HIST_DEFAULT_MAX_PER_CHANNEL;

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    /* All messages, in temporal order, with their session_id so we
     * can derive channel + reset adjacency tracking when sessions
     * change. Single statement keeps the SQL trivial and the I/O
     * pattern sequential. */
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT session_id, role, content FROM messages "
        "ORDER BY session_id, id ASC";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    /* Stack-allocated scratch banks. Bounded at compile time so we
     * never grow the outer array; per-bank examples vectors do grow. */
    hu_pbh_bank_t scratch[HU_PERSONA_BANKS_FROM_HIST_MAX_CHANNELS];
    memset(scratch, 0, sizeof(scratch));
    size_t scratch_count = 0;

    /* Quality + dedup gates — same defaults as the JSONL extractor. */
    hu_quality_thresholds_t qthresh;
    hu_quality_thresholds_default(&qthresh);
    hu_dedup_set_t dedup;
    (void)hu_dedup_set_init(&dedup, 256);

    /* Adjacency tracking — when a `user` row is followed by an
     * `assistant` row in the same session, that's a candidate pair. */
    char prev_session[512] = {0};
    char pending_user[4096] = {0};
    bool have_pending_user = false;

    /* Scratch buffers for PII-redacted variants. Same 4 KB caps as
     * the messages content column in the existing extractor. */
    char redacted_user[4096];
    char redacted_assistant[4096];
    char fingerprint[8200]; /* user + '\n' + assistant + slack */

    hu_error_t ret_err = HU_OK;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *session_id = (const char *)sqlite3_column_text(stmt, 0);
        const char *role = (const char *)sqlite3_column_text(stmt, 1);
        const char *content = (const char *)sqlite3_column_text(stmt, 2);
        if (!session_id || !role || !content)
            continue;

        /* Session change resets the adjacency state — a "user" turn
         * never pairs across session boundaries. */
        if (strcmp(session_id, prev_session) != 0) {
            have_pending_user = false;
            pending_user[0] = '\0';
            size_t sl = strlen(session_id);
            if (sl >= sizeof(prev_session))
                sl = sizeof(prev_session) - 1;
            memcpy(prev_session, session_id, sl);
            prev_session[sl] = '\0';
        }

        if (strcmp(role, "user") == 0) {
            size_t cl = strlen(content);
            if (cl >= sizeof(pending_user))
                cl = sizeof(pending_user) - 1;
            memcpy(pending_user, content, cl);
            pending_user[cl] = '\0';
            have_pending_user = true;
            continue;
        }

        if (strcmp(role, "assistant") != 0 || !have_pending_user) {
            /* assistant-without-user, or non-user/non-assistant role
             * (e.g. "system" / "tool") — both reset adjacency. */
            have_pending_user = false;
            continue;
        }

        /* Found a user→assistant pair. Run gates in cheap-to-
         * expensive order so the common reject paths are fast. */

        /* 1. PII redaction. We never feed un-redacted user text into
         *    a persona example bank — the bank is an inference-time
         *    artifact and a future operator may share or export it. */
        size_t ured_len = 0, ared_len = 0;
        hu_pii_stats_t s_user = {0}, s_asst = {0};
        if (hu_pii_redact(pending_user, strlen(pending_user),
                          redacted_user, sizeof(redacted_user),
                          &ured_len, &s_user) != HU_OK ||
            hu_pii_redact(content, strlen(content),
                          redacted_assistant, sizeof(redacted_assistant),
                          &ared_len, &s_asst) != HU_OK) {
            have_pending_user = false;
            continue;
        }
        (void)s_user;
        (void)s_asst;
        if (ured_len == 0 || ared_len == 0) {
            have_pending_user = false;
            continue;
        }

        /* 2. Quality gate on the concatenated fingerprint. */
        size_t fp_len = 0;
        if (ured_len + ared_len + 2 > sizeof(fingerprint)) {
            /* One side overflows the buffer — too long for a chat
             * example, skip rather than truncate. */
            have_pending_user = false;
            continue;
        }
        memcpy(fingerprint, redacted_user, ured_len);
        fp_len = ured_len;
        fingerprint[fp_len++] = '\n';
        memcpy(&fingerprint[fp_len], redacted_assistant, ared_len);
        fp_len += ared_len;
        fingerprint[fp_len] = '\0';

        if (hu_quality_check(fingerprint, fp_len, &qthresh) != HU_QUALITY_OK) {
            have_pending_user = false;
            continue;
        }

        /* 3. Within-extraction dedup. */
        if (hu_dedup_set_check_and_add(&dedup, fingerprint, fp_len)) {
            have_pending_user = false;
            continue;
        }

        /* 4. Channel routing + per-bank cap. */
        const char *channel = NULL;
        size_t channel_len = 0;
        pbh_channel_from_session(session_id, &channel, &channel_len);

        hu_pbh_bank_t *bank = pbh_find_or_init_bank(
            alloc, scratch, &scratch_count,
            HU_PERSONA_BANKS_FROM_HIST_MAX_CHANNELS,
            channel, channel_len);
        if (!bank) {
            /* Channel cap exhausted, or alloc failed for the channel
             * string. Skip this pair — the next one may land in an
             * existing bank. We don't escalate to OOM here because
             * the cap-exhausted case is by design. */
            have_pending_user = false;
            continue;
        }

        if (bank->count >= max_per_channel) {
            have_pending_user = false;
            continue;
        }

        /* Pair survived every gate. Allocate the canonical strings. */
        char *ctx_dup = hu_strdup(alloc, ""); /* context blank — channel carries the frame */
        char *inc_dup = hu_strndup(alloc, redacted_user, ured_len);
        char *resp_dup = hu_strndup(alloc, redacted_assistant, ared_len);
        if (!ctx_dup || !inc_dup || !resp_dup) {
            if (ctx_dup)
                alloc->free(alloc->ctx, ctx_dup, strlen(ctx_dup) + 1);
            if (inc_dup)
                alloc->free(alloc->ctx, inc_dup, strlen(inc_dup) + 1);
            if (resp_dup)
                alloc->free(alloc->ctx, resp_dup, strlen(resp_dup) + 1);
            ret_err = HU_ERR_OUT_OF_MEMORY;
            break;
        }

        if (pbh_bank_append(alloc, bank, ctx_dup, inc_dup, resp_dup) != HU_OK) {
            alloc->free(alloc->ctx, ctx_dup, strlen(ctx_dup) + 1);
            alloc->free(alloc->ctx, inc_dup, strlen(inc_dup) + 1);
            alloc->free(alloc->ctx, resp_dup, strlen(resp_dup) + 1);
            ret_err = HU_ERR_OUT_OF_MEMORY;
            break;
        }

        have_pending_user = false;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    hu_dedup_set_free(&dedup);

    if (ret_err != HU_OK) {
        pbh_banks_free_contents(alloc, scratch, scratch_count);
        return ret_err;
    }

    /* Trim away banks that ended up empty (channel cap raced ahead
     * of the pair survival rate). Compaction is in-place. */
    size_t kept = 0;
    for (size_t i = 0; i < scratch_count; i++) {
        if (scratch[i].count == 0) {
            if (scratch[i].channel) {
                alloc->free(alloc->ctx, scratch[i].channel, strlen(scratch[i].channel) + 1);
                scratch[i].channel = NULL;
            }
            if (scratch[i].examples) {
                alloc->free(alloc->ctx, scratch[i].examples,
                            scratch[i].cap * sizeof(hu_persona_example_t));
                scratch[i].examples = NULL;
            }
            continue;
        }
        if (kept != i)
            scratch[kept] = scratch[i];
        kept++;
    }

    if (kept == 0)
        return HU_OK; /* zero banks is a valid success state */

    /* Materialize the public-shape array. We can't just copy the
     * scratch struct because the public type has no `cap` field —
     * and we want examples_count == cap (no slack) so future
     * `_free` calls don't over- or under-free. */
    hu_persona_example_bank_t *out =
        (hu_persona_example_bank_t *)alloc->alloc(alloc->ctx,
                                                  kept * sizeof(hu_persona_example_bank_t));
    if (!out) {
        pbh_banks_free_contents(alloc, scratch, scratch_count);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(out, 0, kept * sizeof(hu_persona_example_bank_t));

    for (size_t i = 0; i < kept; i++) {
        out[i].channel = scratch[i].channel;
        out[i].examples_count = scratch[i].count;

        /* Shrink the per-bank examples vector to fit when there's
         * meaningful slack, so the freed size matches what the
         * public free function will compute (count * sizeof). */
        if (scratch[i].cap > scratch[i].count) {
            hu_persona_example_t *tight =
                (hu_persona_example_t *)alloc->alloc(
                    alloc->ctx, scratch[i].count * sizeof(hu_persona_example_t));
            if (!tight) {
                /* Roll back: free what we've materialized so far + the
                 * remaining scratch entries we haven't taken yet. */
                for (size_t j = 0; j < i; j++) {
                    if (out[j].channel)
                        alloc->free(alloc->ctx, out[j].channel,
                                    strlen(out[j].channel) + 1);
                    if (out[j].examples) {
                        for (size_t k = 0; k < out[j].examples_count; k++) {
                            hu_persona_example_t *ex = &out[j].examples[k];
                            if (ex->context)
                                alloc->free(alloc->ctx, ex->context, strlen(ex->context) + 1);
                            if (ex->incoming)
                                alloc->free(alloc->ctx, ex->incoming, strlen(ex->incoming) + 1);
                            if (ex->response)
                                alloc->free(alloc->ctx, ex->response, strlen(ex->response) + 1);
                        }
                        alloc->free(alloc->ctx, out[j].examples,
                                    out[j].examples_count * sizeof(hu_persona_example_t));
                    }
                }
                alloc->free(alloc->ctx, out, kept * sizeof(hu_persona_example_bank_t));
                /* `i..kept-1` still own their scratch storage. */
                for (size_t j = i; j < kept; j++) {
                    if (scratch[j].channel)
                        alloc->free(alloc->ctx, scratch[j].channel,
                                    strlen(scratch[j].channel) + 1);
                    if (scratch[j].examples) {
                        for (size_t k = 0; k < scratch[j].count; k++) {
                            hu_persona_example_t *ex = &scratch[j].examples[k];
                            if (ex->context)
                                alloc->free(alloc->ctx, ex->context, strlen(ex->context) + 1);
                            if (ex->incoming)
                                alloc->free(alloc->ctx, ex->incoming, strlen(ex->incoming) + 1);
                            if (ex->response)
                                alloc->free(alloc->ctx, ex->response, strlen(ex->response) + 1);
                        }
                        alloc->free(alloc->ctx, scratch[j].examples,
                                    scratch[j].cap * sizeof(hu_persona_example_t));
                    }
                }
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(tight, scratch[i].examples,
                   scratch[i].count * sizeof(hu_persona_example_t));
            alloc->free(alloc->ctx, scratch[i].examples,
                        scratch[i].cap * sizeof(hu_persona_example_t));
            out[i].examples = tight;
        } else {
            out[i].examples = scratch[i].examples;
        }
        /* Scratch no longer owns these — handed over to `out`. */
        scratch[i].channel = NULL;
        scratch[i].examples = NULL;
        scratch[i].count = 0;
        scratch[i].cap = 0;
    }

    *out_banks = out;
    *out_count = kept;
    return HU_OK;
#endif /* HU_ENABLE_SQLITE */
}
