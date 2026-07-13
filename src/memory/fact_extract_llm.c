/*
 * fact_extract_llm — LLM-based personal-fact extractor.
 *
 * See include/human/memory/fact_extract_llm.h for the contract and the
 * scope notes (this is a skeleton; integration into the agent turn loop
 * is the caller's call). The implementation pattern is intentionally
 * symmetric with `hu_persona_fidelity_judge`: build a prompt → call
 * provider->chat_with_system → parse JSON → populate result.
 *
 * Why JSON: the response shape needs to be machine-extractable. Asking
 * for free-form text and regex-parsing it would re-invent the problem
 * the regex extractor is failing at. JSON is the contract.
 *
 * Why a soft-fail on malformed JSON: provider responses vary — Claude
 * sometimes wraps JSON in ```json fences, Gemini sometimes adds prose
 * preamble. Rather than rejecting those calls hard, we strip common
 * wrappers and try to find the JSON inside. If we can't, return
 * HU_OK with zero facts — the regex fast-path already ran and produced
 * its own batch, so the caller isn't starved.
 */

#include "human/memory/fact_extract_llm.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/memory/fact_extract.h"
#include "human/memory/trust.h"
#include "human/provider.h"

#include <stdio.h>
#include <string.h>

/* Prompt template — focused, asks for JSON output. Length is bounded
 * by the snprintf below; if you grow it past ~1.5 KB, bump the stack
 * buffer or promote to alloc. */
#define HU_FACT_LLM_SYS                                                            \
    "You are a personal-fact extractor for text-message conversations. Read the "  \
    "message and output ONLY a JSON object listing personal facts you can "        \
    "extract. Output the JSON immediately — no preamble, no reasoning, no "        \
    "explanation, no markdown fences."

/* Predicate vocabulary: the original preference/state set alone measured
 * ~0 facts on the real iMessage corpus (2026-07-11, :8744 base-model
 * measurement) — casual texts carry EVENTS and plans ("I finally graduate
 * sunday", "are you still moving", "found a place to rent in Tampa?"), and
 * a schema-faithful model correctly returns {"facts":[]} when no predicate
 * fits. The event/plan predicates below are what make the extractor recover
 * signal on real traffic. */
#define HU_FACT_LLM_USER_TEMPLATE                                                                 \
    "Extract personal facts from this message. Output JSON of the shape:\n"                       \
    "{\"facts\": [\n"                                                                             \
    "  "                                                                                          \
    "{\"subject\":\"user\",\"predicate\":\"<verb>\",\"object\":\"<value>\",\"confidence\":<0-1>}" \
    "\n]}\n"                                                                                      \
    "Predicates use short forms — states: likes, hates, lives_in, works_at, owns, uses, "         \
    "prefers, avoids, knows, learning; events and plans: graduating, moving_to, visiting, "       \
    "attending, planning, expecting, celebrating, asking_about. Subject \"user\" means the "      \
    "message author. Questions the author asks count as asking_about facts. Output "              \
    "{\"facts\":[]} only when the message truly carries no personal content (bare "               \
    "acknowledgments like \"ok\"). Confidence 0.0-1.0 reflects how unambiguous the "              \
    "statement is.\n\n"                                                                           \
    "Message:\n%.*s\n\nOutput:\n"

/* Find the start of the answer JSON. Thinking-style local models (gemma-4
 * thought channel) ECHO the requested schema inside their reasoning before
 * emitting the real object, so "first '{' in the response" can land inside
 * the thought text and the first-{..last-} span fails to parse (measured
 * 2026-07-11 on the real corpus). Prefer the LAST occurrence of the
 * `{"facts"` envelope — the actual answer — and only fall back to the first
 * '{'/'[' when no envelope is present (bare-array responses). */
static const char *find_json_start(const char *text, size_t len) {
    static const char envelope[] = "{\"facts\"";
    const size_t elen = sizeof(envelope) - 1;
    const char *last = NULL;
    if (len >= elen) {
        for (size_t i = 0; i + elen <= len; i++) {
            if (memcmp(text + i, envelope, elen) == 0)
                last = text + i;
        }
    }
    if (last)
        return last;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '{' || text[i] == '[')
            return text + i;
    }
    return NULL;
}

/* Find the last '}' or ']' so we can trim trailing prose / fence. */
static size_t find_json_end(const char *text, size_t len) {
    for (size_t i = len; i > 0; i--) {
        char c = text[i - 1];
        if (c == '}' || c == ']')
            return i;
    }
    return 0;
}

/* Copy at most `cap-1` bytes from `src` into `dst`, NUL-terminate.
 * Returns the number of bytes copied (excluding the NUL). */
static size_t copy_capped(char *dst, size_t cap, const char *src, size_t src_len) {
    if (!dst || cap == 0)
        return 0;
    size_t n = src_len < cap - 1 ? src_len : cap - 1;
    if (src && n > 0)
        memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

/* Populate one hu_heuristic_fact_t from a JSON object value. Returns
 * true on success, false when required fields are missing. */
static bool fact_from_json_obj(const hu_json_value_t *obj, int64_t now_ts,
                               hu_heuristic_fact_t *out) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return false;

    const char *subject = hu_json_get_string(obj, "subject");
    const char *predicate = hu_json_get_string(obj, "predicate");
    const char *object = hu_json_get_string(obj, "object");
    if (!predicate || !object) /* subject defaults to "user" */
        return false;

    memset(out, 0, sizeof(*out));
    out->type = HU_KNOWLEDGE_PROPOSITIONAL;
    copy_capped(out->subject, sizeof(out->subject), subject ? subject : "user",
                subject ? strlen(subject) : 4);
    copy_capped(out->predicate, sizeof(out->predicate), predicate, strlen(predicate));
    copy_capped(out->object, sizeof(out->object), object, strlen(object));

    double conf = hu_json_get_number(obj, "confidence", 0.7);
    if (conf < 0.0)
        conf = 0.0;
    if (conf > 1.0)
        conf = 1.0;
    out->confidence = (float)conf;

    out->last_seen_at = now_ts;
    out->provenance = hu_provenance_user_direct(now_ts);
    copy_capped(out->source_hint, sizeof(out->source_hint), "llm_extract", 11);
    return true;
}

const char *hu_fact_extract_llm_status_str(hu_fact_extract_llm_status_t status) {
    switch (status) {
    case HU_FACT_LLM_OK:
        return "ok";
    case HU_FACT_LLM_EMPTY_RESPONSE:
        return "empty_response";
    case HU_FACT_LLM_NO_JSON:
        return "no_json";
    case HU_FACT_LLM_BAD_JSON:
        return "bad_json";
    case HU_FACT_LLM_BAD_SHAPE:
        return "bad_shape";
    }
    return "unknown";
}

hu_error_t hu_fact_extract_llm(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                               size_t model_len, const char *text, size_t text_len, int64_t now_ts,
                               hu_fact_extract_result_t *result,
                               hu_fact_extract_llm_status_t *status_out) {
    if (status_out)
        *status_out = HU_FACT_LLM_OK;
    if (!alloc || !provider || !provider->vtable || !provider->vtable->chat_with_system || !text ||
        text_len == 0 || !result)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));

    /* Build the user prompt. Cap text at 4096 bytes — longer messages
     * are unusual in this codebase's chat surfaces and the LLM doesn't
     * need the whole War and Peace to extract a handful of facts. */
    size_t text_cap = text_len < 4096 ? text_len : 4096;
    char user_msg[6144];
    int n = snprintf(user_msg, sizeof(user_msg), HU_FACT_LLM_USER_TEMPLATE, (int)text_cap, text);
    if (n < 0 || (size_t)n >= sizeof(user_msg))
        return HU_ERR_INTERNAL;

    /* Multi-allocation function — use the goto-cleanup pattern per the
     * project convention (multiple-allocation rule, see src dir
     * CLAUDE.md). Frees in reverse allocation order at the single
     * `cleanup:` label. CodeRabbit 2026-05-17 refactor request. */
    hu_error_t ret = HU_OK;
    char *response = NULL;
    size_t response_len = 0;
    hu_json_value_t *root = NULL;

    hu_error_t err = provider->vtable->chat_with_system(
        provider->ctx, alloc, HU_FACT_LLM_SYS, strlen(HU_FACT_LLM_SYS), user_msg, (size_t)n, model,
        model_len, 0.0, &response, &response_len);
    if (err != HU_OK) {
        ret = err;
        goto cleanup;
    }
    if (!response || response_len == 0) {
        /* soft fail: provider returned empty */
        if (status_out)
            *status_out = HU_FACT_LLM_EMPTY_RESPONSE;
        goto cleanup;
    }

    /* Locate JSON body inside response (strip ```json fences / prose). */
    {
        const char *json_start = find_json_start(response, response_len);
        size_t json_end_off = find_json_end(response, response_len);
        if (!json_start || json_end_off == 0 || (size_t)(json_start - response) >= json_end_off) {
            /* soft fail: no JSON body */
            if (status_out)
                *status_out = HU_FACT_LLM_NO_JSON;
            goto cleanup;
        }
        size_t json_len = json_end_off - (size_t)(json_start - response);

        hu_error_t perr = hu_json_parse(alloc, json_start, json_len, &root);
        if (perr != HU_OK || !root) {
            /* soft fail: malformed JSON */
            if (status_out)
                *status_out = HU_FACT_LLM_BAD_JSON;
            goto cleanup;
        }
    }

    /* Accept either {"facts":[...]} or a bare array [...]. */
    {
        const hu_json_value_t *arr = root;
        if (root->type == HU_JSON_OBJECT) {
            arr = hu_json_object_get(root, "facts");
        }
        if (!arr || arr->type != HU_JSON_ARRAY) {
            /* soft fail: structure not matched */
            if (status_out)
                *status_out = HU_FACT_LLM_BAD_SHAPE;
            goto cleanup;
        }

        for (size_t i = 0; i < arr->data.array.len && result->fact_count < HU_FACT_EXTRACT_MAX;
             i++) {
            if (fact_from_json_obj(arr->data.array.items[i], now_ts,
                                   &result->facts[result->fact_count])) {
                result->propositional_count++;
                result->fact_count++;
            }
        }
    }

cleanup:
    if (root)
        hu_json_free(alloc, root);
    if (response)
        alloc->free(alloc->ctx, response, response_len + 1);
    return ret;
}
