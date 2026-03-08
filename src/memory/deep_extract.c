#include "seaclaw/memory/deep_extract.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdlib.h>
#include <string.h>

static const char DEEP_EXTRACT_PROMPT[] =
    "Extract structured information from this conversation.\n"
    "Return JSON: {\"facts\":[{\"subject\":\"...\",\"predicate\":\"...\",\"object\":\"...\","
    "\"confidence\":0.9}],\"relations\":[{\"entity_a\":\"...\",\"relation\":\"...\","
    "\"entity_b\":\"...\",\"confidence\":0.8}],\"summary\":\"1-2 sentence summary\"}\n\n"
    "Conversation:\n";

sc_error_t sc_deep_extract_build_prompt(sc_allocator_t *alloc, const char *conversation,
                                        size_t conversation_len, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;

    size_t prompt_len = sizeof(DEEP_EXTRACT_PROMPT) - 1;
    size_t total = prompt_len + conversation_len + 1;
    char *buf = (char *)alloc->alloc(alloc->ctx, total);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;

    memcpy(buf, DEEP_EXTRACT_PROMPT, prompt_len);
    if (conversation && conversation_len > 0)
        memcpy(buf + prompt_len, conversation, conversation_len);
    buf[prompt_len + conversation_len] = '\0';

    *out = buf;
    *out_len = prompt_len + conversation_len;
    return SC_OK;
}

static const char *get_str(sc_json_value_t *v) {
    if (!v || v->type != SC_JSON_STRING)
        return NULL;
    return v->data.string.ptr;
}

static size_t get_str_len(sc_json_value_t *v) {
    if (!v || v->type != SC_JSON_STRING)
        return 0;
    return v->data.string.len;
}

static double get_num(sc_json_value_t *v, double def) {
    if (!v || v->type != SC_JSON_NUMBER)
        return def;
    return v->data.number;
}

sc_error_t sc_deep_extract_parse(sc_allocator_t *alloc, const char *response, size_t response_len,
                                 sc_deep_extract_result_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!response || response_len == 0)
        return SC_OK;

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, response, response_len, &root);
    if (err != SC_OK || !root || root->type != SC_JSON_OBJECT)
        return err != SC_OK ? err : SC_ERR_JSON_PARSE;

    sc_json_value_t *facts_arr = sc_json_object_get(root, "facts");
    if (facts_arr && facts_arr->type == SC_JSON_ARRAY) {
        for (size_t i = 0; i < facts_arr->data.array.len && out->fact_count < SC_DE_MAX_FACTS;
             i++) {
            sc_json_value_t *item = facts_arr->data.array.items[i];
            if (!item || item->type != SC_JSON_OBJECT)
                continue;
            sc_json_value_t *subj = sc_json_object_get(item, "subject");
            sc_json_value_t *pred = sc_json_object_get(item, "predicate");
            sc_json_value_t *obj = sc_json_object_get(item, "object");
            const char *s = get_str(subj);
            const char *p = get_str(pred);
            const char *o = get_str(obj);
            if (s && p && o) {
                sc_extracted_fact_t *f = &out->facts[out->fact_count];
                f->subject = sc_strndup(alloc, s, get_str_len(subj));
                f->predicate = sc_strndup(alloc, p, get_str_len(pred));
                f->object = sc_strndup(alloc, o, get_str_len(obj));
                f->confidence = get_num(sc_json_object_get(item, "confidence"), 0.9);
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

    sc_json_value_t *rels_arr = sc_json_object_get(root, "relations");
    if (rels_arr && rels_arr->type == SC_JSON_ARRAY) {
        for (size_t i = 0;
             i < rels_arr->data.array.len && out->relation_count < SC_DE_MAX_RELATIONS; i++) {
            sc_json_value_t *item = rels_arr->data.array.items[i];
            if (!item || item->type != SC_JSON_OBJECT)
                continue;
            sc_json_value_t *ea = sc_json_object_get(item, "entity_a");
            sc_json_value_t *rel = sc_json_object_get(item, "relation");
            sc_json_value_t *eb = sc_json_object_get(item, "entity_b");
            const char *a = get_str(ea);
            const char *r = get_str(rel);
            const char *b = get_str(eb);
            if (a && r && b) {
                sc_extracted_relation_t *rel_out = &out->relations[out->relation_count];
                rel_out->entity_a = sc_strndup(alloc, a, get_str_len(ea));
                rel_out->relation = sc_strndup(alloc, r, get_str_len(rel));
                rel_out->entity_b = sc_strndup(alloc, b, get_str_len(eb));
                rel_out->confidence = get_num(sc_json_object_get(item, "confidence"), 0.8);
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

    const char *summary = sc_json_get_string(root, "summary");
    if (summary) {
        size_t slen = strlen(summary);
        out->summary = sc_strndup(alloc, summary, slen);
        if (out->summary)
            out->summary_len = slen;
    }

    sc_json_free(alloc, root);
    return SC_OK;
}

void sc_deep_extract_result_deinit(sc_deep_extract_result_t *result, sc_allocator_t *alloc) {
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
