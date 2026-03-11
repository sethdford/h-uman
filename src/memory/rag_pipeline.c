#include "human/memory/rag_pipeline.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- RAG Pipeline (F153-F154) --- */

hu_rag_config_t hu_rag_default_config(void) {
    hu_rag_config_t cfg = {0};
    for (size_t i = 0; i < HU_RAG_SOURCE_COUNT; i++)
        cfg.source_weights[i] = 1.0;
    cfg.max_results = 10;
    cfg.min_relevance = 0.3;
    cfg.max_token_budget = 2000;
    return cfg;
}

double hu_rag_compute_score(double relevance, double freshness, double source_weight) {
    return relevance * 0.5 + freshness * 0.3 + source_weight * 0.2;
}

static int compare_by_score_desc(const void *a, const void *b) {
    const hu_rag_result_t *ra = (const hu_rag_result_t *)a;
    const hu_rag_result_t *rb = (const hu_rag_result_t *)b;
    if (ra->combined_score > rb->combined_score)
        return -1;
    if (ra->combined_score < rb->combined_score)
        return 1;
    return 0;
}

void hu_rag_sort_results(hu_rag_result_t *results, size_t count) {
    if (!results || count == 0)
        return;
    qsort(results, count, sizeof(hu_rag_result_t), compare_by_score_desc);
}

size_t hu_rag_select_within_budget(hu_rag_result_t *results, size_t count, size_t max_tokens) {
    if (!results || max_tokens == 0)
        return 0;
    size_t used = 0;
    size_t n = 0;
    for (; n < count; n++) {
        size_t est = (results[n].content_len + 3) / 4;
        if (used + est > max_tokens)
            break;
        used += est;
    }
    return n;
}

const char *hu_rag_source_str(hu_rag_source_t source) {
    switch (source) {
    case HU_RAG_EPISODIC:
        return "episodic";
    case HU_RAG_SUPERHUMAN:
        return "superhuman";
    case HU_RAG_KNOWLEDGE:
        return "knowledge";
    case HU_RAG_COMPRESSION:
        return "compression";
    case HU_RAG_OPINIONS:
        return "opinions";
    case HU_RAG_CHAPTERS:
        return "chapters";
    case HU_RAG_SOCIAL_GRAPH:
        return "social_graph";
    case HU_RAG_FEEDS:
        return "feeds";
    default:
        return "unknown";
    }
}

hu_error_t hu_rag_build_prompt(hu_allocator_t *alloc, const hu_rag_result_t *results,
                               size_t count, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!results || count == 0)
        return HU_OK;

    size_t cap = 64;
    for (size_t i = 0; i < count; i++) {
        cap += 32 + (results[i].content ? results[i].content_len : 0);
    }
    if (cap < 256)
        cap = 256;

    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    int w = snprintf(buf, cap, "[RETRIEVED CONTEXT]: ");
    if (w > 0)
        pos = (size_t)w;

    for (size_t i = 0; i < count && pos + 16 < cap; i++) {
        const char *src_str = hu_rag_source_str(results[i].source);
        const char *content = results[i].content ? results[i].content : "";
        size_t clen = results[i].content_len;
        if (pos > 22)
            w = snprintf(buf + pos, cap - pos, " [%s] ", src_str);
        else
            w = snprintf(buf + pos, cap - pos, "[%s] ", src_str);
        if (w > 0 && pos + (size_t)w < cap)
            pos += (size_t)w;
        size_t show = clen;
        if (pos + show + 4 > cap)
            show = show > (cap - pos - 4) ? (size_t)(cap - pos - 4) : 0;
        if (show > 0) {
            memcpy(buf + pos, content, show);
            pos += show;
        }
        if (pos + 2 < cap)
            buf[pos++] = '.';
    }

    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

void hu_rag_result_deinit(hu_allocator_t *alloc, hu_rag_result_t *r) {
    if (!alloc || !r)
        return;
    hu_str_free(alloc, r->content);
    r->content = NULL;
    r->content_len = 0;
}

/* --- On-Device Classifier (F155-F156) --- */

static bool starts_with_ci(const char *msg, size_t len, const char *prefix) {
    size_t plen = strlen(prefix);
    if (len < plen)
        return false;
    for (size_t i = 0; i < plen; i++) {
        char c = (char)((unsigned char)msg[i] | 0x20);
        char p = (char)((unsigned char)prefix[i] | 0x20);
        if (c != p)
            return false;
    }
    return true;
}

static bool contains_ci(const char *msg, size_t len, const char *needle) {
    size_t nlen = strlen(needle);
    if (len < nlen)
        return false;
    for (size_t i = 0; i <= len - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char c = (char)((unsigned char)msg[i + j] | 0x20);
            char n = (char)((unsigned char)needle[j] | 0x20);
            if (c != n) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

hu_classify_result_t hu_classify_message(const char *message, size_t msg_len,
                                         double *confidence_out) {
    if (!message || msg_len == 0) {
        if (confidence_out)
            *confidence_out = 0.0;
        return HU_CLASS_UNKNOWN;
    }

    double conf = 0.8;

    if (starts_with_ci(message, msg_len, "hey") || starts_with_ci(message, msg_len, "hi") ||
        starts_with_ci(message, msg_len, "hello") || starts_with_ci(message, msg_len, "yo") ||
        starts_with_ci(message, msg_len, "sup")) {
        if (confidence_out)
            *confidence_out = conf;
        return HU_CLASS_GREETING;
    }

    if (contains_ci(message, msg_len, "did you know") ||
        contains_ci(message, msg_len, "apparently")) {
        if (confidence_out)
            *confidence_out = conf;
        return HU_CLASS_INFORMATIONAL;
    }

    if (contains_ci(message, msg_len, "?")) {
        if (confidence_out)
            *confidence_out = conf;
        return HU_CLASS_QUESTION;
    }

    if (contains_ci(message, msg_len, "asap") || contains_ci(message, msg_len, "emergency") ||
        contains_ci(message, msg_len, "need help") || contains_ci(message, msg_len, "urgent")) {
        if (confidence_out)
            *confidence_out = conf;
        return HU_CLASS_URGENT;
    }

    if (contains_ci(message, msg_len, "sad") || contains_ci(message, msg_len, "angry") ||
        contains_ci(message, msg_len, "happy") || contains_ci(message, msg_len, "excited") ||
        contains_ci(message, msg_len, "frustrated") || contains_ci(message, msg_len, "worried") ||
        contains_ci(message, msg_len, "anxious") || contains_ci(message, msg_len, "love") ||
        contains_ci(message, msg_len, "hate") || contains_ci(message, msg_len, "miss")) {
        if (confidence_out)
            *confidence_out = 0.75;
        return HU_CLASS_EMOTIONAL;
    }

    if (contains_ci(message, msg_len, "when") || contains_ci(message, msg_len, "where") ||
        contains_ci(message, msg_len, "let's") || contains_ci(message, msg_len, "should we")) {
        if (confidence_out)
            *confidence_out = conf;
        return HU_CLASS_PLANNING;
    }

    if (contains_ci(message, msg_len, "lol") || contains_ci(message, msg_len, "haha") ||
        contains_ci(message, msg_len, "lmao") || contains_ci(message, msg_len, "hahaha")) {
        if (confidence_out)
            *confidence_out = conf;
        return HU_CLASS_HUMOR;
    }

    if (confidence_out)
        *confidence_out = 0.6;
    return HU_CLASS_CASUAL;
}

const char *hu_classify_result_str(hu_classify_result_t r) {
    switch (r) {
    case HU_CLASS_GREETING:
        return "greeting";
    case HU_CLASS_QUESTION:
        return "question";
    case HU_CLASS_EMOTIONAL:
        return "emotional";
    case HU_CLASS_INFORMATIONAL:
        return "informational";
    case HU_CLASS_PLANNING:
        return "planning";
    case HU_CLASS_HUMOR:
        return "humor";
    case HU_CLASS_URGENT:
        return "urgent";
    case HU_CLASS_CASUAL:
        return "casual";
    default:
        return "unknown";
    }
}

hu_error_t hu_classifier_build_prompt(hu_allocator_t *alloc, hu_classify_result_t cls,
                                      double confidence, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *cls_str = hu_classify_result_str(cls);
    char *buf = (char *)alloc->alloc(alloc->ctx, 256);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int w = snprintf(buf, 256, "[CLASSIFICATION]: %s (confidence: %.2f)", cls_str, confidence);
    if (w < 0) {
        alloc->free(alloc->ctx, buf, 256);
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out = buf;
    *out_len = (size_t)w;
    return HU_OK;
}
