#include "seaclaw/core/json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

#define JSON_MAX_DEPTH 64

static int hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef struct parser {
    const char *src;
    size_t len;
    size_t pos;
    sc_allocator_t *alloc;
    int depth;
} parser_t;

static void skip_ws(parser_t *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->pos++;
    }
}

static char peek(parser_t *p) {
    skip_ws(p);
    return p->pos < p->len ? p->src[p->pos] : '\0';
}

static char advance(parser_t *p) {
    skip_ws(p);
    return p->pos < p->len ? p->src[p->pos++] : '\0';
}

static sc_json_value_t *alloc_value(sc_allocator_t *a, sc_json_type_t type) {
    sc_json_value_t *v = (sc_json_value_t *)a->alloc(a->ctx, sizeof(sc_json_value_t));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    v->type = type;
    return v;
}

static sc_error_t parse_value(parser_t *p, sc_json_value_t **out);

static sc_error_t parse_string_raw(parser_t *p, char **out, size_t *out_len) {
    if (p->src[p->pos] != '"') return SC_ERR_JSON_PARSE;
    p->pos++;

    size_t cap = 64;
    char *buf = (char *)p->alloc->alloc(p->alloc->ctx, cap);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
    size_t len = 0;

    while (p->pos < p->len && p->src[p->pos] != '"') {
        char c = p->src[p->pos++];
        if (c == '\\') {
            if (p->pos >= p->len) {
                p->alloc->free(p->alloc->ctx, buf, cap);
                return SC_ERR_JSON_PARSE;
            }
            c = p->src[p->pos++];
            switch (c) {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (p->pos + 4 > p->len) {
                        p->alloc->free(p->alloc->ctx, buf, cap);
                        return SC_ERR_JSON_PARSE;
                    }
                    int h0 = hex_char_val(p->src[p->pos]);
                    int h1 = hex_char_val(p->src[p->pos + 1]);
                    int h2 = hex_char_val(p->src[p->pos + 2]);
                    int h3 = hex_char_val(p->src[p->pos + 3]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                        p->alloc->free(p->alloc->ctx, buf, cap);
                        return SC_ERR_JSON_PARSE;
                    }
                    p->pos += 4;
                    unsigned int cp = (unsigned int)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    if (cp < 0x80) {
                        c = (char)cp;
                    } else if (cp < 0x800) {
                        if (len + 2 > cap) {
                            size_t old_cap = cap;
                            cap *= 2;
                            buf = (char *)p->alloc->realloc(p->alloc->ctx, buf, old_cap, cap);
                            if (!buf) return SC_ERR_OUT_OF_MEMORY;
                        }
                        buf[len++] = (char)(0xC0 | (cp >> 6));
                        c = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        if (len + 3 > cap) {
                            size_t old_cap = cap;
                            if (cap > SIZE_MAX / 2) { p->alloc->free(p->alloc->ctx, buf, cap); return SC_ERR_OUT_OF_MEMORY; }
                            cap *= 2;
                            buf = (char *)p->alloc->realloc(p->alloc->ctx, buf, old_cap, cap);
                            if (!buf) return SC_ERR_OUT_OF_MEMORY;
                        }
                        buf[len++] = (char)(0xE0 | (cp >> 12));
                        buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        c = (char)(0x80 | (cp & 0x3F));
                    } else {
                        p->alloc->free(p->alloc->ctx, buf, cap);
                        return SC_ERR_JSON_PARSE;
                    }
                    break;
                }
                default:
                    p->alloc->free(p->alloc->ctx, buf, cap);
                    return SC_ERR_JSON_PARSE;
            }
        } else if ((unsigned char)c < 0x20) {
            p->alloc->free(p->alloc->ctx, buf, cap);
            return SC_ERR_JSON_PARSE;
        }
        if (len + 1 >= cap) {
            size_t old = cap;
            if (cap > SIZE_MAX / 2) { p->alloc->free(p->alloc->ctx, buf, cap); return SC_ERR_OUT_OF_MEMORY; }
            cap *= 2;
            buf = (char *)p->alloc->realloc(p->alloc->ctx, buf, old, cap);
            if (!buf) return SC_ERR_OUT_OF_MEMORY;
        }
        buf[len++] = c;
    }

    if (p->pos >= p->len || p->src[p->pos] != '"') {
        p->alloc->free(p->alloc->ctx, buf, cap);
        return SC_ERR_JSON_PARSE;
    }
    p->pos++;

    buf[len] = '\0';
    *out = buf;
    if (out_len) *out_len = len;
    return SC_OK;
}

static sc_error_t parse_string(parser_t *p, sc_json_value_t **out) {
    char *str = NULL;
    size_t slen = 0;
    sc_error_t err = parse_string_raw(p, &str, &slen);
    if (err != SC_OK) return err;

    sc_json_value_t *v = alloc_value(p->alloc, SC_JSON_STRING);
    if (!v) { p->alloc->free(p->alloc->ctx, str, slen + 1); return SC_ERR_OUT_OF_MEMORY; }
    v->data.string.ptr = str;
    v->data.string.len = slen;
    *out = v;
    return SC_OK;
}

static sc_error_t parse_number(parser_t *p, sc_json_value_t **out) {
    const char *start = p->src + p->pos;
    char *end = NULL;
    double num = strtod(start, &end);
    if (end == start) return SC_ERR_JSON_PARSE;
    p->pos += (size_t)(end - start);

    if (fpclassify(num) == FP_NAN || fpclassify(num) == FP_INFINITE) return SC_ERR_JSON_PARSE;

    sc_json_value_t *v = alloc_value(p->alloc, SC_JSON_NUMBER);
    if (!v) return SC_ERR_OUT_OF_MEMORY;
    v->data.number = num;
    *out = v;
    return SC_OK;
}

static sc_error_t parse_array(parser_t *p, sc_json_value_t **out) {
    p->pos++;
    if (++p->depth > JSON_MAX_DEPTH) return SC_ERR_JSON_DEPTH;

    sc_json_value_t *arr = alloc_value(p->alloc, SC_JSON_ARRAY);
    if (!arr) return SC_ERR_OUT_OF_MEMORY;
    arr->data.array.items = NULL;
    arr->data.array.len = 0;
    arr->data.array.cap = 0;

    if (peek(p) == ']') { p->pos++; p->depth--; *out = arr; return SC_OK; }

    while (1) {
        sc_json_value_t *val = NULL;
        sc_error_t err = parse_value(p, &val);
        if (err != SC_OK) { sc_json_free(p->alloc, arr); return err; }

        if (arr->data.array.len >= arr->data.array.cap) {
            size_t new_cap = arr->data.array.cap ? arr->data.array.cap * 2 : 4;
            if (new_cap > SIZE_MAX / sizeof(sc_json_value_t *)) { sc_json_free(p->alloc, arr); return SC_ERR_OUT_OF_MEMORY; }
            size_t old_sz = arr->data.array.cap * sizeof(sc_json_value_t *);
            size_t new_sz = new_cap * sizeof(sc_json_value_t *);
            sc_json_value_t **items = (sc_json_value_t **)p->alloc->realloc(
                p->alloc->ctx, arr->data.array.items, old_sz, new_sz);
            if (!items) { sc_json_free(p->alloc, val); sc_json_free(p->alloc, arr); return SC_ERR_OUT_OF_MEMORY; }
            arr->data.array.items = items;
            arr->data.array.cap = new_cap;
        }
        arr->data.array.items[arr->data.array.len++] = val;

        if (peek(p) == ',') {
            advance(p);
            if (peek(p) == ']') { p->pos++; break; }
            continue;
        }
        if (peek(p) == ']') { p->pos++; break; }
        sc_json_free(p->alloc, arr);
        return SC_ERR_JSON_PARSE;
    }

    p->depth--;
    *out = arr;
    return SC_OK;
}

static sc_error_t parse_object(parser_t *p, sc_json_value_t **out) {
    p->pos++;
    if (++p->depth > JSON_MAX_DEPTH) return SC_ERR_JSON_DEPTH;

    sc_json_value_t *obj = alloc_value(p->alloc, SC_JSON_OBJECT);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    obj->data.object.pairs = NULL;
    obj->data.object.len = 0;
    obj->data.object.cap = 0;

    if (peek(p) == '}') { p->pos++; p->depth--; *out = obj; return SC_OK; }

    while (1) {
        skip_ws(p);
        if (peek(p) == '}') {
            p->pos++;
            break;
        }
        char *key = NULL;
        size_t key_len = 0;
        sc_error_t err = parse_string_raw(p, &key, &key_len);
        if (err != SC_OK) { sc_json_free(p->alloc, obj); return err; }

        if (advance(p) != ':') {
            p->alloc->free(p->alloc->ctx, key, key_len + 1);
            sc_json_free(p->alloc, obj);
            return SC_ERR_JSON_PARSE;
        }

        sc_json_value_t *val = NULL;
        err = parse_value(p, &val);
        if (err != SC_OK) {
            p->alloc->free(p->alloc->ctx, key, key_len + 1);
            sc_json_free(p->alloc, obj);
            return err;
        }

        for (size_t i = 0; i < obj->data.object.len; i++) {
            sc_json_pair_t *ex = &obj->data.object.pairs[i];
            if (ex->key_len == key_len && memcmp(ex->key, key, key_len) == 0) {
                p->alloc->free(p->alloc->ctx, ex->key, ex->key_len + 1);
                sc_json_free(p->alloc, ex->value);
                ex->key = key;
                ex->key_len = key_len;
                ex->value = val;
                key = NULL;
                break;
            }
        }
        if (key) {
            if (obj->data.object.len >= obj->data.object.cap) {
                size_t new_cap = obj->data.object.cap ? obj->data.object.cap * 2 : 4;
                if (new_cap > SIZE_MAX / sizeof(sc_json_pair_t)) {
                    p->alloc->free(p->alloc->ctx, key, key_len + 1);
                    sc_json_free(p->alloc, val);
                    sc_json_free(p->alloc, obj);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                size_t old_sz = obj->data.object.cap * sizeof(sc_json_pair_t);
                size_t new_sz = new_cap * sizeof(sc_json_pair_t);
                sc_json_pair_t *pairs = (sc_json_pair_t *)p->alloc->realloc(
                    p->alloc->ctx, obj->data.object.pairs, old_sz, new_sz);
                if (!pairs) {
                    p->alloc->free(p->alloc->ctx, key, key_len + 1);
                    sc_json_free(p->alloc, val);
                    sc_json_free(p->alloc, obj);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                obj->data.object.pairs = pairs;
                obj->data.object.cap = new_cap;
            }
            sc_json_pair_t *pair = &obj->data.object.pairs[obj->data.object.len++];
            pair->key = key;
            pair->key_len = key_len;
            pair->value = val;
        }

        if (peek(p) == ',') { advance(p); continue; }
        if (peek(p) == '}') { p->pos++; break; }
        sc_json_free(p->alloc, obj);
        return SC_ERR_JSON_PARSE;
    }

    p->depth--;
    *out = obj;
    return SC_OK;
}

static sc_error_t parse_value(parser_t *p, sc_json_value_t **out) {
    char c = peek(p);
    if (c == '"') return parse_string(p, out);
    if (c == '{') return parse_object(p, out);
    if (c == '[') return parse_array(p, out);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p, out);

    if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "true", 4) == 0) {
        p->pos += 4;
        sc_json_value_t *v = alloc_value(p->alloc, SC_JSON_BOOL);
        if (!v) return SC_ERR_OUT_OF_MEMORY;
        v->data.boolean = true;
        *out = v;
        return SC_OK;
    }
    if (p->pos + 5 <= p->len && memcmp(p->src + p->pos, "false", 5) == 0) {
        p->pos += 5;
        sc_json_value_t *v = alloc_value(p->alloc, SC_JSON_BOOL);
        if (!v) return SC_ERR_OUT_OF_MEMORY;
        v->data.boolean = false;
        *out = v;
        return SC_OK;
    }
    if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "null", 4) == 0) {
        p->pos += 4;
        *out = alloc_value(p->alloc, SC_JSON_NULL);
        return *out ? SC_OK : SC_ERR_OUT_OF_MEMORY;
    }

    return SC_ERR_JSON_PARSE;
}

sc_error_t sc_json_parse(sc_allocator_t *alloc, const char *input, size_t input_len,
                         sc_json_value_t **out) {
    if (!input || !out) return SC_ERR_INVALID_ARGUMENT;
    parser_t p = { .src = input, .len = input_len, .pos = 0, .alloc = alloc, .depth = 0 };
    return parse_value(&p, out);
}

void sc_json_free(sc_allocator_t *alloc, sc_json_value_t *val) {
    if (!val) return;
    switch (val->type) {
        case SC_JSON_STRING:
            if (val->data.string.ptr)
                alloc->free(alloc->ctx, val->data.string.ptr, val->data.string.len + 1);
            break;
        case SC_JSON_ARRAY:
            for (size_t i = 0; i < val->data.array.len; i++)
                sc_json_free(alloc, val->data.array.items[i]);
            if (val->data.array.items)
                alloc->free(alloc->ctx, val->data.array.items,
                           val->data.array.cap * sizeof(sc_json_value_t *));
            break;
        case SC_JSON_OBJECT:
            for (size_t i = 0; i < val->data.object.len; i++) {
                sc_json_pair_t *pair = &val->data.object.pairs[i];
                if (pair->key)
                    alloc->free(alloc->ctx, pair->key, pair->key_len + 1);
                sc_json_free(alloc, pair->value);
            }
            if (val->data.object.pairs)
                alloc->free(alloc->ctx, val->data.object.pairs,
                           val->data.object.cap * sizeof(sc_json_pair_t));
            break;
        default:
            break;
    }
    alloc->free(alloc->ctx, val, sizeof(sc_json_value_t));
}

sc_json_value_t *sc_json_null_new(sc_allocator_t *alloc) {
    return alloc_value(alloc, SC_JSON_NULL);
}

sc_json_value_t *sc_json_bool_new(sc_allocator_t *alloc, bool val) {
    sc_json_value_t *v = alloc_value(alloc, SC_JSON_BOOL);
    if (v) v->data.boolean = val;
    return v;
}

sc_json_value_t *sc_json_number_new(sc_allocator_t *alloc, double val) {
    sc_json_value_t *v = alloc_value(alloc, SC_JSON_NUMBER);
    if (v) v->data.number = val;
    return v;
}

sc_json_value_t *sc_json_string_new(sc_allocator_t *alloc, const char *s, size_t len) {
    if (!alloc || (len > 0 && !s)) return NULL;
    sc_json_value_t *v = alloc_value(alloc, SC_JSON_STRING);
    if (!v) return NULL;
    char *dup = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!dup) { alloc->free(alloc->ctx, v, sizeof(*v)); return NULL; }
    memcpy(dup, s, len);
    dup[len] = '\0';
    v->data.string.ptr = dup;
    v->data.string.len = len;
    return v;
}

sc_json_value_t *sc_json_array_new(sc_allocator_t *alloc) {
    return alloc_value(alloc, SC_JSON_ARRAY);
}

sc_json_value_t *sc_json_object_new(sc_allocator_t *alloc) {
    return alloc_value(alloc, SC_JSON_OBJECT);
}

sc_error_t sc_json_array_push(sc_allocator_t *alloc, sc_json_value_t *arr, sc_json_value_t *val) {
    if (!arr || arr->type != SC_JSON_ARRAY || !val) return SC_ERR_INVALID_ARGUMENT;
    if (arr->data.array.len >= arr->data.array.cap) {
        size_t new_cap = arr->data.array.cap ? arr->data.array.cap * 2 : 4;
        if (new_cap > SIZE_MAX / sizeof(sc_json_value_t *)) return SC_ERR_OUT_OF_MEMORY;
        size_t old_sz = arr->data.array.cap * sizeof(sc_json_value_t *);
        size_t new_sz = new_cap * sizeof(sc_json_value_t *);
        sc_json_value_t **items = (sc_json_value_t **)alloc->realloc(
            alloc->ctx, arr->data.array.items, old_sz, new_sz);
        if (!items) return SC_ERR_OUT_OF_MEMORY;
        arr->data.array.items = items;
        arr->data.array.cap = new_cap;
    }
    arr->data.array.items[arr->data.array.len++] = val;
    return SC_OK;
}

sc_error_t sc_json_object_set(sc_allocator_t *alloc, sc_json_value_t *obj,
                              const char *key, sc_json_value_t *val) {
    if (!obj || obj->type != SC_JSON_OBJECT || !key || !val)
        return SC_ERR_INVALID_ARGUMENT;

    size_t klen = strlen(key);
    for (size_t i = 0; i < obj->data.object.len; i++) {
        sc_json_pair_t *pair = &obj->data.object.pairs[i];
        if (pair->key_len == klen && memcmp(pair->key, key, klen) == 0) {
            sc_json_free(alloc, pair->value);
            pair->value = val;
            return SC_OK;
        }
    }

    if (obj->data.object.len >= obj->data.object.cap) {
        size_t new_cap = obj->data.object.cap ? obj->data.object.cap * 2 : 4;
        if (new_cap > SIZE_MAX / sizeof(sc_json_pair_t)) return SC_ERR_OUT_OF_MEMORY;
        size_t old_sz = obj->data.object.cap * sizeof(sc_json_pair_t);
        size_t new_sz = new_cap * sizeof(sc_json_pair_t);
        sc_json_pair_t *pairs = (sc_json_pair_t *)alloc->realloc(
            alloc->ctx, obj->data.object.pairs, old_sz, new_sz);
        if (!pairs) return SC_ERR_OUT_OF_MEMORY;
        obj->data.object.pairs = pairs;
        obj->data.object.cap = new_cap;
    }

    char *kdup = (char *)alloc->alloc(alloc->ctx, klen + 1);
    if (!kdup) return SC_ERR_OUT_OF_MEMORY;
    memcpy(kdup, key, klen + 1);

    sc_json_pair_t *pair = &obj->data.object.pairs[obj->data.object.len++];
    pair->key = kdup;
    pair->key_len = klen;
    pair->value = val;
    return SC_OK;
}

sc_json_value_t *sc_json_object_get(const sc_json_value_t *obj, const char *key) {
    if (!obj || obj->type != SC_JSON_OBJECT || !key) return NULL;
    size_t klen = strlen(key);
    for (size_t i = 0; i < obj->data.object.len; i++) {
        sc_json_pair_t *pair = &obj->data.object.pairs[i];
        if (pair->key_len == klen && memcmp(pair->key, key, klen) == 0)
            return pair->value;
    }
    return NULL;
}

bool sc_json_object_remove(sc_allocator_t *alloc, sc_json_value_t *obj, const char *key) {
    if (!alloc || !obj || obj->type != SC_JSON_OBJECT || !key) return false;
    size_t klen = strlen(key);
    for (size_t i = 0; i < obj->data.object.len; i++) {
        sc_json_pair_t *pair = &obj->data.object.pairs[i];
        if (pair->key_len == klen && memcmp(pair->key, key, klen) == 0) {
            if (pair->key)
                alloc->free(alloc->ctx, pair->key, pair->key_len + 1);
            if (pair->value)
                sc_json_free(alloc, pair->value);
            pair->key = NULL;
            pair->key_len = 0;
            pair->value = NULL;
            /* Swap with last */
            if (i != obj->data.object.len - 1) {
                obj->data.object.pairs[i] = obj->data.object.pairs[obj->data.object.len - 1];
            }
            obj->data.object.len--;
            return true;
        }
    }
    return false;
}

const char *sc_json_get_string(const sc_json_value_t *val, const char *key) {
    sc_json_value_t *v = sc_json_object_get(val, key);
    if (v && v->type == SC_JSON_STRING) return v->data.string.ptr;
    return NULL;
}

double sc_json_get_number(const sc_json_value_t *val, const char *key, double default_val) {
    sc_json_value_t *v = sc_json_object_get(val, key);
    if (v && v->type == SC_JSON_NUMBER) return v->data.number;
    return default_val;
}

bool sc_json_get_bool(const sc_json_value_t *val, const char *key, bool default_val) {
    sc_json_value_t *v = sc_json_object_get(val, key);
    if (v && v->type == SC_JSON_BOOL) return v->data.boolean;
    return default_val;
}

static sc_error_t stringify_value(sc_allocator_t *alloc, const sc_json_value_t *val,
                                  char **buf, size_t *len, size_t *cap);

static sc_error_t buf_append(sc_allocator_t *alloc, char **buf, size_t *len, size_t *cap,
                             const char *s, size_t slen) {
    if (slen > SIZE_MAX - *len - 1) return SC_ERR_OUT_OF_MEMORY;
    while (*len + slen + 1 > *cap) {
        if (*cap > SIZE_MAX / 2) return SC_ERR_OUT_OF_MEMORY;
        size_t new_cap = *cap ? *cap * 2 : 256;
        char *nb = (char *)alloc->realloc(alloc->ctx, *buf, *cap, new_cap);
        if (!nb) return SC_ERR_OUT_OF_MEMORY;
        *buf = nb;
        *cap = new_cap;
    }
    memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
    return SC_OK;
}

static sc_error_t stringify_string(sc_allocator_t *alloc, const char *s, size_t slen,
                                   char **buf, size_t *len, size_t *cap) {
    sc_error_t err = buf_append(alloc, buf, len, cap, "\"", 1);
    if (err) return err;

    for (size_t i = 0; i < slen; i++) {
        char c = s[i];
        const char *esc = NULL;
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char ubuf[8];
                    snprintf(ubuf, sizeof(ubuf), "\\u%04x", (unsigned char)c);
                    err = buf_append(alloc, buf, len, cap, ubuf, 6);
                    if (err) return err;
                    continue;
                }
                break;
        }
        if (esc) {
            err = buf_append(alloc, buf, len, cap, esc, strlen(esc));
        } else {
            err = buf_append(alloc, buf, len, cap, &c, 1);
        }
        if (err) return err;
    }

    return buf_append(alloc, buf, len, cap, "\"", 1);
}

static sc_error_t stringify_value(sc_allocator_t *alloc, const sc_json_value_t *val,
                                  char **buf, size_t *len, size_t *cap) {
    sc_error_t err;

    switch (val->type) {
        case SC_JSON_NULL:
            return buf_append(alloc, buf, len, cap, "null", 4);
        case SC_JSON_BOOL:
            return val->data.boolean
                ? buf_append(alloc, buf, len, cap, "true", 4)
                : buf_append(alloc, buf, len, cap, "false", 5);
        case SC_JSON_NUMBER: {
            char nbuf[64];
            int n;
            double d = val->data.number;
            if (d == (double)(long long)d && fabs(d) < 1e15)
                n = snprintf(nbuf, sizeof(nbuf), "%lld", (long long)d);
            else
                n = snprintf(nbuf, sizeof(nbuf), "%.17g", d);
            return buf_append(alloc, buf, len, cap, nbuf, (size_t)n);
        }
        case SC_JSON_STRING:
            return stringify_string(alloc, val->data.string.ptr, val->data.string.len,
                                    buf, len, cap);
        case SC_JSON_ARRAY:
            err = buf_append(alloc, buf, len, cap, "[", 1);
            if (err) return err;
            for (size_t i = 0; i < val->data.array.len; i++) {
                if (i > 0) { err = buf_append(alloc, buf, len, cap, ",", 1); if (err) return err; }
                err = stringify_value(alloc, val->data.array.items[i], buf, len, cap);
                if (err) return err;
            }
            return buf_append(alloc, buf, len, cap, "]", 1);
        case SC_JSON_OBJECT:
            err = buf_append(alloc, buf, len, cap, "{", 1);
            if (err) return err;
            for (size_t i = 0; i < val->data.object.len; i++) {
                if (i > 0) { err = buf_append(alloc, buf, len, cap, ",", 1); if (err) return err; }
                err = stringify_string(alloc, val->data.object.pairs[i].key,
                                       val->data.object.pairs[i].key_len, buf, len, cap);
                if (err) return err;
                err = buf_append(alloc, buf, len, cap, ":", 1);
                if (err) return err;
                err = stringify_value(alloc, val->data.object.pairs[i].value, buf, len, cap);
                if (err) return err;
            }
            return buf_append(alloc, buf, len, cap, "}", 1);
    }
    return SC_ERR_INTERNAL;
}

sc_error_t sc_json_stringify(sc_allocator_t *alloc, const sc_json_value_t *val,
                             char **out, size_t *out_len) {
    if (!val || !out) return SC_ERR_INVALID_ARGUMENT;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    sc_error_t err = stringify_value(alloc, val, &buf, &len, &cap);
    if (err != SC_OK) {
        if (buf) alloc->free(alloc->ctx, buf, cap);
        return err;
    }
    *out = buf;
    if (out_len) *out_len = len;
    return SC_OK;
}

static sc_error_t json_buf_append(sc_json_buf_t *buf, const char *s, size_t slen) {
    if (slen > SIZE_MAX - buf->len - 1) return SC_ERR_OUT_OF_MEMORY;
    while (buf->len + slen + 1 > buf->cap) {
        if (buf->cap > SIZE_MAX / 2) return SC_ERR_OUT_OF_MEMORY;
        size_t new_cap = buf->cap ? buf->cap * 2 : 256;
        char *nb = (char *)buf->alloc->realloc(buf->alloc->ctx, buf->ptr, buf->cap, new_cap);
        if (!nb) return SC_ERR_OUT_OF_MEMORY;
        buf->ptr = nb;
        buf->cap = new_cap;
    }
    memcpy(buf->ptr + buf->len, s, slen);
    buf->len += slen;
    buf->ptr[buf->len] = '\0';
    return SC_OK;
}

sc_error_t sc_json_buf_append_raw(sc_json_buf_t *buf, const char *str, size_t str_len) {
    if (!buf || !buf->alloc) return SC_ERR_INVALID_ARGUMENT;
    if (!str && str_len > 0) return SC_ERR_INVALID_ARGUMENT;
    return json_buf_append(buf, str ? str : "", str_len ? str_len : 0);
}

static sc_error_t json_buf_append_escaped(sc_json_buf_t *buf, const char *s, size_t slen) {
    sc_error_t err = json_buf_append(buf, "\"", 1);
    if (err) return err;
    for (size_t i = 0; i < slen; i++) {
        char c = s[i];
        const char *esc = NULL;
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '/':  esc = "\\/";  break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char ubuf[8];
                    snprintf(ubuf, sizeof(ubuf), "\\u%04x", (unsigned char)c);
                    err = json_buf_append(buf, ubuf, 6);
                    if (err) return err;
                    continue;
                }
                break;
        }
        if (esc) {
            err = json_buf_append(buf, esc, strlen(esc));
        } else {
            err = json_buf_append(buf, &c, 1);
        }
        if (err) return err;
    }
    return json_buf_append(buf, "\"", 1);
}

sc_error_t sc_json_buf_init(sc_json_buf_t *buf, sc_allocator_t *alloc) {
    if (!buf || !alloc) return SC_ERR_INVALID_ARGUMENT;
    buf->ptr = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->alloc = alloc;
    return SC_OK;
}

void sc_json_buf_free(sc_json_buf_t *buf) {
    if (!buf) return;
    if (buf->ptr && buf->alloc)
        buf->alloc->free(buf->alloc->ctx, buf->ptr, buf->cap);
    buf->ptr = NULL;
    buf->len = 0;
    buf->cap = 0;
}

sc_error_t sc_json_append_string(sc_json_buf_t *buf, const char *str, size_t str_len) {
    if (!buf || !buf->alloc) return SC_ERR_INVALID_ARGUMENT;
    if (!str && str_len > 0) return SC_ERR_INVALID_ARGUMENT;
    return json_buf_append_escaped(buf, str ? str : "", str_len);
}

sc_error_t sc_json_append_key(sc_json_buf_t *buf, const char *key, size_t key_len) {
    if (!buf || !buf->alloc) return SC_ERR_INVALID_ARGUMENT;
    if (!key && key_len > 0) return SC_ERR_INVALID_ARGUMENT;
    sc_error_t err = json_buf_append_escaped(buf, key ? key : "", key_len);
    if (err) return err;
    return json_buf_append(buf, ":", 1);
}

sc_error_t sc_json_append_key_value(sc_json_buf_t *buf, const char *key, size_t key_len,
                                    const char *value, size_t value_len) {
    if (!buf || !buf->alloc) return SC_ERR_INVALID_ARGUMENT;
    sc_error_t err = sc_json_append_key(buf, key, key_len);
    if (err) return err;
    return sc_json_append_string(buf, value, value_len);
}

sc_error_t sc_json_append_key_int(sc_json_buf_t *buf, const char *key, size_t key_len,
                                  long long value) {
    if (!buf || !buf->alloc) return SC_ERR_INVALID_ARGUMENT;
    sc_error_t err = sc_json_append_key(buf, key, key_len);
    if (err) return err;
    char nbuf[24];
    int n = snprintf(nbuf, sizeof(nbuf), "%lld", (long long)value);
    if (n < 0 || (size_t)n >= sizeof(nbuf)) return SC_ERR_INTERNAL;
    return json_buf_append(buf, nbuf, (size_t)n);
}

sc_error_t sc_json_append_key_bool(sc_json_buf_t *buf, const char *key, size_t key_len,
                                   bool value) {
    if (!buf || !buf->alloc) return SC_ERR_INVALID_ARGUMENT;
    sc_error_t err = sc_json_append_key(buf, key, key_len);
    if (err) return err;
    if (value)
        return json_buf_append(buf, "true", 4);
    return json_buf_append(buf, "false", 5);
}
