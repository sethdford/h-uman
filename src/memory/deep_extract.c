#include "human/memory/deep_extract.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char DEEP_EXTRACT_PROMPT[] =
    "Extract structured information from this conversation.\n"
    "Return JSON: {\"facts\":[{\"subject\":\"...\",\"predicate\":\"...\",\"object\":\"...\","
    "\"confidence\":0.9}],\"relations\":[{\"entity_a\":\"...\",\"relation\":\"...\","
    "\"entity_b\":\"...\",\"confidence\":0.8}],\"summary\":\"1-2 sentence summary\"}\n\n"
    "Conversation:\n";

hu_error_t hu_deep_extract_build_prompt(hu_allocator_t *alloc, const char *conversation,
                                        size_t conversation_len, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t prompt_len = sizeof(DEEP_EXTRACT_PROMPT) - 1;
    size_t total = prompt_len + conversation_len + 1;
    char *buf = (char *)alloc->alloc(alloc->ctx, total);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    memcpy(buf, DEEP_EXTRACT_PROMPT, prompt_len);
    if (conversation && conversation_len > 0)
        memcpy(buf + prompt_len, conversation, conversation_len);
    buf[prompt_len + conversation_len] = '\0';

    *out = buf;
    *out_len = prompt_len + conversation_len;
    return HU_OK;
}

static const char *get_str(hu_json_value_t *v) {
    if (!v || v->type != HU_JSON_STRING)
        return NULL;
    return v->data.string.ptr;
}

static size_t get_str_len(hu_json_value_t *v) {
    if (!v || v->type != HU_JSON_STRING)
        return 0;
    return v->data.string.len;
}

static double get_num(hu_json_value_t *v, double def) {
    if (!v || v->type != HU_JSON_NUMBER)
        return def;
    return v->data.number;
}

hu_error_t hu_deep_extract_parse(hu_allocator_t *alloc, const char *response, size_t response_len,
                                 hu_deep_extract_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!response || response_len == 0)
        return HU_OK;

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, response, response_len, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(alloc, root);
        return err != HU_OK ? err : HU_ERR_JSON_PARSE;
    }

    hu_json_value_t *facts_arr = hu_json_object_get(root, "facts");
    if (facts_arr && facts_arr->type == HU_JSON_ARRAY) {
        for (size_t i = 0; i < facts_arr->data.array.len && out->fact_count < HU_DE_MAX_FACTS;
             i++) {
            hu_json_value_t *item = facts_arr->data.array.items[i];
            if (!item || item->type != HU_JSON_OBJECT)
                continue;
            hu_json_value_t *subj = hu_json_object_get(item, "subject");
            hu_json_value_t *pred = hu_json_object_get(item, "predicate");
            hu_json_value_t *obj = hu_json_object_get(item, "object");
            const char *s = get_str(subj);
            const char *p = get_str(pred);
            const char *o = get_str(obj);
            if (s && p && o) {
                hu_extracted_fact_t *f = &out->facts[out->fact_count];
                f->subject = hu_strndup(alloc, s, get_str_len(subj));
                f->predicate = hu_strndup(alloc, p, get_str_len(pred));
                f->object = hu_strndup(alloc, o, get_str_len(obj));
                f->confidence = get_num(hu_json_object_get(item, "confidence"), 0.9);
                if (f->subject && f->predicate && f->object)
                    out->fact_count++;
                else {
                    if (f->subject)
                        alloc->free(alloc->ctx, f->subject, strlen(f->subject) + 1);
                    if (f->predicate)
                        alloc->free(alloc->ctx, f->predicate, strlen(f->predicate) + 1);
                    if (f->object)
                        alloc->free(alloc->ctx, f->object, strlen(f->object) + 1);
                }
            }
        }
    }

    hu_json_value_t *rels_arr = hu_json_object_get(root, "relations");
    if (rels_arr && rels_arr->type == HU_JSON_ARRAY) {
        for (size_t i = 0;
             i < rels_arr->data.array.len && out->relation_count < HU_DE_MAX_RELATIONS; i++) {
            hu_json_value_t *item = rels_arr->data.array.items[i];
            if (!item || item->type != HU_JSON_OBJECT)
                continue;
            hu_json_value_t *ea = hu_json_object_get(item, "entity_a");
            hu_json_value_t *rel = hu_json_object_get(item, "relation");
            hu_json_value_t *eb = hu_json_object_get(item, "entity_b");
            const char *a = get_str(ea);
            const char *r = get_str(rel);
            const char *b = get_str(eb);
            if (a && r && b) {
                hu_extracted_relation_t *rel_out = &out->relations[out->relation_count];
                rel_out->entity_a = hu_strndup(alloc, a, get_str_len(ea));
                rel_out->relation = hu_strndup(alloc, r, get_str_len(rel));
                rel_out->entity_b = hu_strndup(alloc, b, get_str_len(eb));
                rel_out->confidence = get_num(hu_json_object_get(item, "confidence"), 0.8);
                if (rel_out->entity_a && rel_out->relation && rel_out->entity_b)
                    out->relation_count++;
                else {
                    if (rel_out->entity_a)
                        alloc->free(alloc->ctx, rel_out->entity_a, strlen(rel_out->entity_a) + 1);
                    if (rel_out->relation)
                        alloc->free(alloc->ctx, rel_out->relation, strlen(rel_out->relation) + 1);
                    if (rel_out->entity_b)
                        alloc->free(alloc->ctx, rel_out->entity_b, strlen(rel_out->entity_b) + 1);
                }
            }
        }
    }

    const char *summary = hu_json_get_string(root, "summary");
    if (summary) {
        size_t slen = strlen(summary);
        out->summary = hu_strndup(alloc, summary, slen);
        if (out->summary)
            out->summary_len = slen;
    }

    hu_json_free(alloc, root);
    return HU_OK;
}

static bool prefix_match_ci(const char *text, size_t text_len, const char *prefix,
                            size_t prefix_len) {
    if (text_len < prefix_len)
        return false;
    for (size_t i = 0; i < prefix_len; i++) {
        char a = (unsigned char)text[i];
        char b = (unsigned char)prefix[i];
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (b >= 'A' && b <= 'Z')
            b += 32;
        if (a != b)
            return false;
    }
    return true;
}

static size_t find_clause_end(const char *text, size_t text_len, size_t start) {
    for (size_t i = start; i < text_len; i++) {
        char c = text[i];
        if (c == '.' || c == ',' || c == '\n' || c == '!' || c == '?')
            return i;
    }
    return text_len;
}

static bool is_word_boundary(char c) {
    return c == '\0' || (unsigned char)c <= 32 || c == '.' || c == ',' || c == '!' || c == '?';
}

typedef struct {
    const char *pattern;
    size_t pattern_len;
    const char *predicate;
    size_t predicate_len;
} hu_de_lightweight_pattern_t;

static const hu_de_lightweight_pattern_t LIGHTWEIGHT_PATTERNS[] = {
    {"I work at ", 10, "works_at", 8},
    {"I'm a ", 6, "is_a", 4},
    {"I am a ", 7, "is_a", 4},
    {"I'm an ", 7, "is_a", 4},
    {"I am an ", 8, "is_a", 4},
    {"I live in ", 10, "lives_in", 8},
    {"I like ", 7, "likes", 5},
    {"I love ", 7, "loves", 5},
    {"I hate ", 7, "hates", 5},
    {"my name is ", 11, "name", 4},
    {"my job is ", 10, "job", 3},
};

#define LIGHTWEIGHT_PATTERN_COUNT \
    (sizeof(LIGHTWEIGHT_PATTERNS) / sizeof(LIGHTWEIGHT_PATTERNS[0]))

hu_error_t hu_deep_extract_lightweight(hu_allocator_t *alloc, const char *text, size_t text_len,
                                       hu_deep_extract_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!text) {
        if (text_len > 0)
            return HU_ERR_INVALID_ARGUMENT;
        return HU_OK;
    }

    for (size_t i = 0; i < text_len && out->fact_count < HU_DE_MAX_FACTS; i++) {
        while (i < text_len && (unsigned char)text[i] <= 32)
            i++;
        if (i >= text_len)
            break;

        for (size_t p = 0; p < LIGHTWEIGHT_PATTERN_COUNT; p++) {
            const hu_de_lightweight_pattern_t *pat = &LIGHTWEIGHT_PATTERNS[p];
            if (!prefix_match_ci(text + i, text_len - i, pat->pattern, pat->pattern_len))
                continue;
            if (i > 0 && !is_word_boundary(text[i - 1]))
                continue;

            size_t obj_start = i + pat->pattern_len;
            while (obj_start < text_len && (unsigned char)text[obj_start] <= 32)
                obj_start++;
            if (obj_start >= text_len)
                break;

            size_t obj_end = find_clause_end(text, text_len, obj_start);
            if (obj_end <= obj_start)
                continue;

            size_t obj_len = obj_end - obj_start;
            while (obj_len > 0 && (unsigned char)text[obj_start + obj_len - 1] <= 32)
                obj_len--;
            if (obj_len == 0)
                continue;

            hu_extracted_fact_t *f = &out->facts[out->fact_count];
            f->subject = hu_strndup(alloc, "user", 4);
            f->predicate = hu_strndup(alloc, pat->predicate, pat->predicate_len);
            f->object = hu_strndup(alloc, text + obj_start, obj_len);
            f->confidence = 0.85;
            if (f->subject && f->predicate && f->object) {
                out->fact_count++;
                i = obj_end - 1;
                break;
            }
            if (f->subject)
                alloc->free(alloc->ctx, f->subject, strlen(f->subject) + 1);
            if (f->predicate)
                alloc->free(alloc->ctx, f->predicate, strlen(f->predicate) + 1);
            if (f->object)
                alloc->free(alloc->ctx, f->object, strlen(f->object) + 1);
        }
    }
    return HU_OK;
}

void hu_deep_extract_result_deinit(hu_deep_extract_result_t *result, hu_allocator_t *alloc) {
    if (!result || !alloc)
        return;
    for (size_t i = 0; i < result->fact_count; i++) {
        if (result->facts[i].subject)
            alloc->free(alloc->ctx, result->facts[i].subject, strlen(result->facts[i].subject) + 1);
        if (result->facts[i].predicate)
            alloc->free(alloc->ctx, result->facts[i].predicate,
                        strlen(result->facts[i].predicate) + 1);
        if (result->facts[i].object)
            alloc->free(alloc->ctx, result->facts[i].object, strlen(result->facts[i].object) + 1);
    }
    result->fact_count = 0;
    for (size_t i = 0; i < result->relation_count; i++) {
        if (result->relations[i].entity_a)
            alloc->free(alloc->ctx, result->relations[i].entity_a,
                        strlen(result->relations[i].entity_a) + 1);
        if (result->relations[i].relation)
            alloc->free(alloc->ctx, result->relations[i].relation,
                        strlen(result->relations[i].relation) + 1);
        if (result->relations[i].entity_b)
            alloc->free(alloc->ctx, result->relations[i].entity_b,
                        strlen(result->relations[i].entity_b) + 1);
    }
    result->relation_count = 0;
    if (result->summary) {
        alloc->free(alloc->ctx, result->summary, result->summary_len + 1);
        result->summary = NULL;
        result->summary_len = 0;
    }
}
