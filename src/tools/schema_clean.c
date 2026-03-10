/*
 * SchemaCleanr: JSON Schema cleaning for LLM tool-calling compatibility.
 * JSON Schema cleaning for LLM tool-calling compatibility.
 * Removes unsupported keywords per provider, resolves $ref, const->enum, etc.
 */
#include "human/tools/schema_clean.h"
#include "human/core/json.h"
#include <stdlib.h>
#include <string.h>

#define HU_REF_STACK_MAX         32
#define HU_GEMINI_KW_COUNT       22
#define HU_ANTHROPIC_KW_COUNT    3
#define HU_OPENAI_KW_COUNT       0
#define HU_CONSERVATIVE_KW_COUNT 4

typedef enum {
    HU_STRAT_GEMINI,
    HU_STRAT_ANTHROPIC,
    HU_STRAT_OPENAI,
    HU_STRAT_CONSERVATIVE
} hu_schema_strategy_t;

static const char *const GEMINI_KW[HU_GEMINI_KW_COUNT] = {"$ref",
                                                          "$schema",
                                                          "$id",
                                                          "$defs",
                                                          "definitions",
                                                          "additionalProperties",
                                                          "patternProperties",
                                                          "minLength",
                                                          "maxLength",
                                                          "pattern",
                                                          "format",
                                                          "minimum",
                                                          "maximum",
                                                          "multipleOf",
                                                          "minItems",
                                                          "maxItems",
                                                          "uniqueItems",
                                                          "minProperties",
                                                          "maxProperties",
                                                          "examples"};

static const char *const ANTHROPIC_KW[HU_ANTHROPIC_KW_COUNT] = {"$ref", "$defs", "definitions"};

static const char *const CONSERVATIVE_KW[HU_CONSERVATIVE_KW_COUNT] = {
    "$ref", "$defs", "definitions", "additionalProperties"};

static bool kw_in_list(const char *key, const char *const *list, size_t count) {
    if (!key || !list)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (list[i] && strcmp(key, list[i]) == 0)
            return true;
    }
    return false;
}

static bool is_unsupported(const char *key, hu_schema_strategy_t strat) {
    switch (strat) {
    case HU_STRAT_GEMINI:
        return kw_in_list(key, GEMINI_KW, HU_GEMINI_KW_COUNT);
    case HU_STRAT_ANTHROPIC:
        return kw_in_list(key, ANTHROPIC_KW, HU_ANTHROPIC_KW_COUNT);
    case HU_STRAT_OPENAI:
        return false;
    case HU_STRAT_CONSERVATIVE:
        return kw_in_list(key, CONSERVATIVE_KW, HU_CONSERVATIVE_KW_COUNT);
    }
    return false;
}

static hu_json_value_t *get_defs(hu_allocator_t *alloc, hu_json_value_t *root) {
    (void)alloc;
    if (!root || root->type != HU_JSON_OBJECT)
        return NULL;
    hu_json_value_t *v = hu_json_object_get(root, "$defs");
    if (v && v->type == HU_JSON_OBJECT)
        return v;
    v = hu_json_object_get(root, "definitions");
    if (v && v->type == HU_JSON_OBJECT)
        return v;
    return NULL;
}

static bool obj_has_key(const hu_json_value_t *obj, const char *key) {
    return hu_json_object_get(obj, key) != NULL;
}

static const char *get_string_val(const hu_json_value_t *v) {
    if (!v || v->type != HU_JSON_STRING)
        return NULL;
    return v->data.string.ptr;
}

/* Parse #/$defs/Name or #/definitions/Name, return Name or NULL */
static const char *parse_local_ref(const char *ref) {
    const char *p1 = "#/$defs/";
    const char *p2 = "#/definitions/";
    if (strncmp(ref, p1, 8) == 0)
        return ref + 8;
    if (strncmp(ref, p2, 14) == 0)
        return ref + 14;
    return NULL;
}

static hu_json_value_t *get_def_by_name(hu_json_value_t *defs, const char *name) {
    if (!defs || defs->type != HU_JSON_OBJECT)
        return NULL;
    return hu_json_object_get(defs, name);
}

typedef struct {
    const char *refs[HU_REF_STACK_MAX];
    size_t count;
} ref_stack_t;

static bool ref_stack_contains(ref_stack_t *s, const char *ref) {
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->refs[i], ref) == 0)
            return true;
    }
    return false;
}

static bool ref_stack_push(ref_stack_t *s, const char *ref) {
    if (s->count >= HU_REF_STACK_MAX)
        return false;
    s->refs[s->count++] = ref;
    return true;
}

static void ref_stack_pop(ref_stack_t *s) {
    if (s->count > 0)
        s->count--;
}

/* Check if schema represents null */
static bool is_null_schema(hu_json_value_t *v) {
    if (!v || v->type != HU_JSON_OBJECT)
        return false;
    hu_json_value_t *c = hu_json_object_get(v, "const");
    if (c && c->type == HU_JSON_NULL)
        return true;
    hu_json_value_t *e = hu_json_object_get(v, "enum");
    if (e && e->type == HU_JSON_ARRAY && e->data.array.len == 1) {
        hu_json_value_t *ei = e->data.array.items[0];
        if (ei && ei->type == HU_JSON_NULL)
            return true;
    }
    hu_json_value_t *t = hu_json_object_get(v, "type");
    if (t && t->type == HU_JSON_STRING && t->data.string.len == 4 &&
        memcmp(t->data.string.ptr, "null", 4) == 0)
        return true;
    return false;
}

/* Clean type array: remove null, return single type if one left */
static hu_json_value_t *clean_type_array(hu_allocator_t *alloc, hu_json_value_t *v) {
    if (!v || v->type != HU_JSON_ARRAY)
        return v;
    size_t n = v->data.array.len;
    if (n == 0)
        return v;
    hu_json_value_t **items = v->data.array.items;
    hu_json_value_t *last_non_null = NULL;
    size_t non_null_count = 0;
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *t = items[i];
        if (t->type == HU_JSON_STRING && t->data.string.len == 4 &&
            memcmp(t->data.string.ptr, "null", 4) == 0)
            continue;
        non_null_count++;
        last_non_null = t;
    }
    if (non_null_count == 0)
        return hu_json_string_new(alloc, "null", 4);
    if (non_null_count == 1)
        return hu_json_string_new(alloc, last_non_null->data.string.ptr,
                                  last_non_null->data.string.len);
    return v; /* multiple non-null, keep array */
}

/* Preserve metadata (description, title, default) from source onto target */
static void preserve_meta(hu_allocator_t *alloc, hu_json_value_t *source, hu_json_value_t *target) {
    if (!target || target->type != HU_JSON_OBJECT)
        return;
    const char *meta[] = {"description", "title", "default"};
    for (size_t i = 0; i < 3; i++) {
        hu_json_value_t *val = hu_json_object_get(source, meta[i]);
        if (val) {
            /* Deep copy val - for simplicity we only handle string/number/bool for metadata */
            hu_json_value_t *dup = NULL;
            if (val->type == HU_JSON_STRING)
                dup = hu_json_string_new(alloc, val->data.string.ptr, val->data.string.len);
            else if (val->type == HU_JSON_NUMBER)
                dup = hu_json_number_new(alloc, val->data.number);
            else if (val->type == HU_JSON_BOOL)
                dup = hu_json_bool_new(alloc, val->data.boolean);
            else if (val->type == HU_JSON_NULL)
                dup = hu_json_null_new(alloc);
            if (dup)
                hu_json_object_set(alloc, target, meta[i], dup);
        }
    }
}

/* Recursive clean - returns new value, caller owns. */
static hu_json_value_t *clean_value(hu_allocator_t *alloc, hu_json_value_t *val,
                                    hu_schema_strategy_t strat, hu_json_value_t *defs,
                                    ref_stack_t *ref_stack);

static hu_json_value_t *resolve_ref(hu_allocator_t *alloc, const char *ref_value,
                                    hu_json_value_t *source_obj, hu_json_value_t *defs,
                                    hu_schema_strategy_t strat, ref_stack_t *ref_stack) {
    if (ref_stack_contains(ref_stack, ref_value)) {
        hu_json_value_t *empty = hu_json_object_new(alloc);
        if (empty)
            preserve_meta(alloc, source_obj, empty);
        return empty;
    }
    const char *def_name = parse_local_ref(ref_value);
    if (def_name) {
        hu_json_value_t *def = get_def_by_name(defs, def_name);
        if (def) {
            ref_stack_push(ref_stack, ref_value);
            hu_json_value_t *cleaned = clean_value(alloc, def, strat, defs, ref_stack);
            ref_stack_pop(ref_stack);
            if (cleaned && cleaned->type == HU_JSON_OBJECT)
                preserve_meta(alloc, source_obj, cleaned);
            return cleaned;
        }
    }
    hu_json_value_t *empty = hu_json_object_new(alloc);
    if (empty)
        preserve_meta(alloc, source_obj, empty);
    return empty;
}

/* Try simplify anyOf/oneOf - strip null, single variant, literal enum flatten. Returns new value or
 * NULL to keep as-is. */
static hu_json_value_t *try_simplify_union(hu_allocator_t *alloc, hu_json_value_t *obj,
                                           hu_schema_strategy_t strat, hu_json_value_t *defs,
                                           ref_stack_t *ref_stack) {
    const char *union_key =
        obj_has_key(obj, "anyOf") ? "anyOf" : (obj_has_key(obj, "oneOf") ? "oneOf" : NULL);
    if (!union_key)
        return NULL;
    hu_json_value_t *variants_val = hu_json_object_get(obj, union_key);
    if (!variants_val || variants_val->type != HU_JSON_ARRAY)
        return NULL;

    size_t n = variants_val->data.array.len;
    hu_json_value_t **items = variants_val->data.array.items;

    /* Clean variants, strip null */
    hu_json_value_t *non_null[32];
    size_t nn = 0;
    for (size_t i = 0; i < n && nn < 32; i++) {
        hu_json_value_t *cleaned = clean_value(alloc, items[i], strat, defs, ref_stack);
        if (!cleaned)
            continue;
        if (is_null_schema(cleaned)) {
            hu_json_free(alloc, cleaned);
            continue;
        }
        non_null[nn++] = cleaned;
    }
    if (nn == 0)
        return NULL;
    if (nn == 1) {
        hu_json_value_t *one = non_null[0];
        preserve_meta(alloc, obj, one);
        return one;
    }
    /* Could try literal enum flatten here - skip for brevity, keep union */
    for (size_t i = 0; i < nn; i++)
        hu_json_free(alloc, non_null[i]);
    return NULL;
}

static hu_json_value_t *clean_value(hu_allocator_t *alloc, hu_json_value_t *val,
                                    hu_schema_strategy_t strat, hu_json_value_t *defs,
                                    ref_stack_t *ref_stack) {
    if (!val)
        return NULL;

    if (val->type == HU_JSON_OBJECT) {
        /* Handle $ref first */
        hu_json_value_t *ref_val = hu_json_object_get(val, "$ref");
        if (ref_val) {
            const char *ref_str = get_string_val(ref_val);
            if (ref_str) {
                hu_json_value_t *resolved =
                    resolve_ref(alloc, ref_str, val, defs, strat, ref_stack);
                if (resolved)
                    return resolved;
            }
        }

        /* Try union simplification */
        hu_json_value_t *simplified = try_simplify_union(alloc, val, strat, defs, ref_stack);
        if (simplified)
            return simplified;

        bool has_union = obj_has_key(val, "anyOf") || obj_has_key(val, "oneOf");

        hu_json_value_t *cleaned = hu_json_object_new(alloc);
        if (!cleaned)
            return NULL;

        for (size_t i = 0; i < val->data.object.len; i++) {
            hu_json_pair_t *p = &val->data.object.pairs[i];
            const char *key = p->key;
            hu_json_value_t *v = p->value;
            if (!key || !v)
                continue;

            if (is_unsupported(key, strat))
                continue;

            if (strcmp(key, "const") == 0) {
                /* Convert const to enum */
                hu_json_value_t *dup = NULL;
                if (v->type == HU_JSON_STRING)
                    dup = hu_json_string_new(alloc, v->data.string.ptr, v->data.string.len);
                else if (v->type == HU_JSON_NUMBER)
                    dup = hu_json_number_new(alloc, v->data.number);
                else if (v->type == HU_JSON_BOOL)
                    dup = hu_json_bool_new(alloc, v->data.boolean);
                else if (v->type == HU_JSON_NULL)
                    dup = hu_json_null_new(alloc);
                if (dup) {
                    hu_json_value_t *enum_arr = hu_json_array_new(alloc);
                    if (enum_arr && hu_json_array_push(alloc, enum_arr, dup) == HU_OK)
                        hu_json_object_set(alloc, cleaned, "enum", enum_arr);
                    else if (dup)
                        hu_json_free(alloc, dup);
                }
            } else if (strcmp(key, "type") == 0 && has_union) {
                continue; /* skip type when we have anyOf/oneOf */
            } else if (strcmp(key, "type") == 0 && v->type == HU_JSON_ARRAY) {
                hu_json_value_t *ct = clean_type_array(alloc, v);
                if (ct)
                    hu_json_object_set(alloc, cleaned, key, ct);
            } else if (strcmp(key, "properties") == 0 && v->type == HU_JSON_OBJECT) {
                hu_json_value_t *cp = clean_value(alloc, v, strat, defs, ref_stack);
                if (cp)
                    hu_json_object_set(alloc, cleaned, key, cp);
            } else if (strcmp(key, "items") == 0) {
                hu_json_value_t *ci = clean_value(alloc, v, strat, defs, ref_stack);
                if (ci)
                    hu_json_object_set(alloc, cleaned, key, ci);
            } else if (strcmp(key, "anyOf") == 0 || strcmp(key, "oneOf") == 0 ||
                       strcmp(key, "allOf") == 0) {
                if (v->type == HU_JSON_ARRAY) {
                    hu_json_value_t *arr = hu_json_array_new(alloc);
                    if (arr) {
                        for (size_t j = 0; j < v->data.array.len; j++) {
                            hu_json_value_t *cj =
                                clean_value(alloc, v->data.array.items[j], strat, defs, ref_stack);
                            if (cj)
                                hu_json_array_push(alloc, arr, cj);
                        }
                        hu_json_object_set(alloc, cleaned, key, arr);
                    }
                }
            } else {
                hu_json_value_t *cv;
                if (v->type == HU_JSON_OBJECT || v->type == HU_JSON_ARRAY)
                    cv = clean_value(alloc, v, strat, defs, ref_stack);
                else if (v->type == HU_JSON_STRING)
                    cv = hu_json_string_new(alloc, v->data.string.ptr, v->data.string.len);
                else if (v->type == HU_JSON_NUMBER)
                    cv = hu_json_number_new(alloc, v->data.number);
                else if (v->type == HU_JSON_BOOL)
                    cv = hu_json_bool_new(alloc, v->data.boolean);
                else if (v->type == HU_JSON_NULL)
                    cv = hu_json_null_new(alloc);
                else
                    cv = NULL;
                if (cv)
                    hu_json_object_set(alloc, cleaned, key, cv);
            }
        }
        return cleaned;
    }

    if (val->type == HU_JSON_ARRAY) {
        hu_json_value_t *arr = hu_json_array_new(alloc);
        if (!arr)
            return NULL;
        for (size_t i = 0; i < val->data.array.len; i++) {
            hu_json_value_t *ci =
                clean_value(alloc, val->data.array.items[i], strat, defs, ref_stack);
            if (ci)
                hu_json_array_push(alloc, arr, ci);
        }
        return arr;
    }

    /* Primitive: copy */
    if (val->type == HU_JSON_NULL)
        return hu_json_null_new(alloc);
    if (val->type == HU_JSON_BOOL)
        return hu_json_bool_new(alloc, val->data.boolean);
    if (val->type == HU_JSON_NUMBER)
        return hu_json_number_new(alloc, val->data.number);
    if (val->type == HU_JSON_STRING)
        return hu_json_string_new(alloc, val->data.string.ptr, val->data.string.len);
    return NULL;
}

static hu_schema_strategy_t strategy_from_name(const char *name) {
    if (!name)
        return HU_STRAT_CONSERVATIVE;
    if (strcmp(name, "gemini") == 0)
        return HU_STRAT_GEMINI;
    if (strcmp(name, "anthropic") == 0)
        return HU_STRAT_ANTHROPIC;
    if (strcmp(name, "openai") == 0)
        return HU_STRAT_OPENAI;
    if (strcmp(name, "conservative") == 0)
        return HU_STRAT_CONSERVATIVE;
    return HU_STRAT_CONSERVATIVE;
}

hu_error_t hu_schema_clean(hu_allocator_t *alloc, const char *schema_json, size_t schema_len,
                           const char *provider_name, char **out_cleaned, size_t *out_len) {
    if (!alloc || !schema_json || !out_cleaned)
        return HU_ERR_INVALID_ARGUMENT;
    *out_cleaned = NULL;
    if (out_len)
        *out_len = 0;

    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, schema_json, schema_len, &parsed);
    if (err != HU_OK || !parsed)
        return err;

    hu_json_value_t *defs_obj = get_defs(alloc, parsed);
    ref_stack_t ref_stack = {.count = 0};
    hu_schema_strategy_t strat = strategy_from_name(provider_name);

    hu_json_value_t *cleaned = clean_value(alloc, parsed, strat, defs_obj, &ref_stack);
    hu_json_free(alloc, parsed);
    if (!cleaned)
        return HU_ERR_OUT_OF_MEMORY;

    err = hu_json_stringify(alloc, cleaned, out_cleaned, out_len);
    hu_json_free(alloc, cleaned);
    return err;
}

bool hu_schema_validate(hu_allocator_t *alloc, const char *schema_json, size_t schema_len) {
    if (!alloc || !schema_json)
        return false;
    hu_json_value_t *parsed = NULL;
    if (hu_json_parse(alloc, schema_json, schema_len, &parsed) != HU_OK || !parsed)
        return false;
    bool ok = (parsed->type == HU_JSON_OBJECT && hu_json_object_get(parsed, "type") != NULL);
    hu_json_free(alloc, parsed);
    return ok;
}
