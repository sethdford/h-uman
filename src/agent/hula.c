#include "human/agent/hula.h"
#include "agent_internal.h"
#include "human/agent/idempotency.h"
#include "human/agent/planner.h"
#include "human/agent/dag.h"
#include "human/agent/registry.h"
#include "human/agent/spawn.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/observer.h"
#include "human/security.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define HU_HULA_REF_EXPAND_CAP (256u * 1024u)

static uint64_t hula_wall_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
#endif
}

static bool hula_id_char_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool hula_id_char_cont(char c) {
    return hula_id_char_start(c) || (c >= '0' && c <= '9');
}

/* Longest program node id matching s[0..) with boundary at end or non-continuing char. */
static size_t hula_ref_longest_match(const char *s, size_t s_rem, const hu_hula_program_t *prog) {
    size_t best = 0;
    if (!s || s_rem == 0 || !prog)
        return 0;
    for (size_t ni = 0; ni < prog->node_count; ni++) {
        const hu_hula_node_t *nd = &prog->nodes[ni];
        if (!nd->id)
            continue;
        size_t idl = strlen(nd->id);
        if (idl == 0 || idl > s_rem)
            continue;
        if (memcmp(s, nd->id, idl) != 0)
            continue;
        if (s_rem > idl && hula_id_char_cont(s[idl]))
            continue;
        if (idl > best)
            best = idl;
    }
    return best;
}

static size_t hula_slot_key_longest_match(const hu_hula_exec_t *exec, const char *s, size_t s_rem) {
    size_t best = 0;
    if (!exec || !s || s_rem == 0)
        return 0;
    for (size_t si = 0; si < exec->slot_count; si++) {
        const char *k = exec->slots[si].key;
        size_t kl = exec->slots[si].key_len;
        if (!k || kl == 0 || kl > s_rem)
            continue;
        if (memcmp(s, k, kl) != 0)
            continue;
        if (s_rem > kl && hula_id_char_cont(s[kl]))
            continue;
        if (kl > best)
            best = kl;
    }
    return best;
}

/* Expand $node_id in JSON string values to prior node outputs (HU_HULA_DONE). */
static hu_error_t hula_expand_dollar_refs(hu_hula_exec_t *exec, const char *in, size_t in_len,
                                          char **out, size_t *out_len) {
    if (!exec || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!in && in_len > 0)
        return HU_ERR_INVALID_ARGUMENT;
    hu_allocator_t *a = &exec->alloc;
    size_t cap = in_len + 128;
    if (cap < 64)
        cap = 64;
    char *buf = (char *)a->alloc(a->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t pos = 0;
    for (size_t i = 0; i < in_len;) {
        /* Literal dollar: `$$` → `$` (so e.g. `$$5` → `$5`, not a failed ref). */
        if (i + 1 < in_len && in[i] == '$' && in[i + 1] == '$') {
            size_t need = pos + 2;
            while (cap < need) {
                size_t oldc = cap;
                if (cap > SIZE_MAX / 2) {
                    a->free(a->ctx, buf, oldc);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                cap *= 2;
                void *nb = a->realloc(a->ctx, buf, oldc, cap);
                if (!nb) {
                    a->free(a->ctx, buf, oldc);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                buf = (char *)nb;
            }
            buf[pos++] = '$';
            i += 2;
            continue;
        }
        if (in[i] == '$' && i + 1 < in_len && hula_id_char_start(in[i + 1])) {
            size_t rem = in_len - (i + 1);
            size_t skl = hula_slot_key_longest_match(exec, in + i + 1, rem);
            size_t nid = hula_ref_longest_match(in + i + 1, rem, exec->program);
            bool use_slot = (skl >= nid && skl > 0);
            size_t ref_len = use_slot ? skl : nid;
            const char *rep = NULL;
            size_t rep_len = 0;
            if (use_slot) {
                for (size_t si = 0; si < exec->slot_count; si++) {
                    if (exec->slots[si].key_len == skl &&
                        memcmp(exec->slots[si].key, in + i + 1, skl) == 0) {
                        rep = exec->slots[si].value;
                        rep_len = exec->slots[si].value_len;
                        break;
                    }
                }
            } else if (nid > 0) {
                char idbuf[160];
                if (nid >= sizeof(idbuf)) {
                    a->free(a->ctx, buf, cap);
                    return HU_ERR_INVALID_ARGUMENT;
                }
                memcpy(idbuf, in + i + 1, nid);
                idbuf[nid] = '\0';
                const hu_hula_result_t *hr = hu_hula_exec_result(exec, idbuf);
                if (!hr || hr->status != HU_HULA_DONE || !hr->output) {
                    a->free(a->ctx, buf, cap);
                    return HU_ERR_INVALID_ARGUMENT;
                }
                rep = hr->output;
                rep_len = hr->output_len;
            }
            if (!rep || ref_len == 0 || rep_len > HU_HULA_REF_EXPAND_CAP) {
                a->free(a->ctx, buf, cap);
                return HU_ERR_INVALID_ARGUMENT;
            }
            size_t need = pos + rep_len + 1;
            while (cap < need) {
                size_t oldc = cap;
                if (cap > SIZE_MAX / 2) {
                    a->free(a->ctx, buf, oldc);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                cap *= 2;
                if (cap < need)
                    cap = need;
                void *nb = a->realloc(a->ctx, buf, oldc, cap);
                if (!nb) {
                    a->free(a->ctx, buf, oldc);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                buf = (char *)nb;
            }
            memcpy(buf + pos, rep, rep_len);
            pos += rep_len;
            if (pos > HU_HULA_REF_EXPAND_CAP) {
                a->free(a->ctx, buf, cap);
                return HU_ERR_INVALID_ARGUMENT;
            }
            i += 1 + ref_len;
            continue;
        }
        size_t need = pos + 2;
        while (cap < need) {
            size_t oldc = cap;
            if (cap > SIZE_MAX / 2) {
                a->free(a->ctx, buf, oldc);
                return HU_ERR_OUT_OF_MEMORY;
            }
            cap *= 2;
            void *nb = a->realloc(a->ctx, buf, oldc, cap);
            if (!nb) {
                a->free(a->ctx, buf, oldc);
                return HU_ERR_OUT_OF_MEMORY;
            }
            buf = (char *)nb;
        }
        buf[pos++] = in[i++];
    }
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

static hu_error_t hula_substitute_json_strings(hu_hula_exec_t *exec, hu_json_value_t *v) {
    if (!exec || !v)
        return HU_ERR_INVALID_ARGUMENT;
    switch (v->type) {
    case HU_JSON_STRING: {
        char *s = v->data.string.ptr;
        size_t L = v->data.string.len;
        if (!s || !memchr(s, '$', L))
            return HU_OK;
        char *new_s = NULL;
        size_t new_len = 0;
        hu_error_t e = hula_expand_dollar_refs(exec, s, L, &new_s, &new_len);
        if (e != HU_OK)
            return e;
        exec->alloc.free(exec->alloc.ctx, s, L + 1);
        v->data.string.ptr = new_s;
        v->data.string.len = new_len;
        return HU_OK;
    }
    case HU_JSON_ARRAY:
        for (size_t j = 0; j < v->data.array.len; j++) {
            hu_error_t e = hula_substitute_json_strings(exec, v->data.array.items[j]);
            if (e != HU_OK)
                return e;
        }
        return HU_OK;
    case HU_JSON_OBJECT:
        for (size_t j = 0; j < v->data.object.len; j++) {
            hu_error_t e = hula_substitute_json_strings(exec, v->data.object.pairs[j].value);
            if (e != HU_OK)
                return e;
        }
        return HU_OK;
    default:
        return HU_OK;
    }
}

/* ── Name tables ────────────────────────────────────────────────────────── */

static const char *const op_names[] = {
    [HU_HULA_CALL]     = "call",
    [HU_HULA_SEQ]      = "seq",
    [HU_HULA_PAR]      = "par",
    [HU_HULA_BRANCH]   = "branch",
    [HU_HULA_LOOP]     = "loop",
    [HU_HULA_DELEGATE]  = "delegate",
    [HU_HULA_EMIT]     = "emit",
    [HU_HULA_TRY]      = "try",
    [HU_HULA_VERIFY]   = "verify",
};

static const char *const pred_names[] = {
    [HU_HULA_PRED_SUCCESS]      = "success",
    [HU_HULA_PRED_FAILURE]      = "failure",
    [HU_HULA_PRED_CONTAINS]     = "contains",
    [HU_HULA_PRED_NOT_CONTAINS] = "not_contains",
    [HU_HULA_PRED_ALWAYS]       = "always",
};

static const char *const status_names[] = {
    [HU_HULA_PENDING] = "pending",
    [HU_HULA_RUNNING] = "running",
    [HU_HULA_DONE]    = "done",
    [HU_HULA_FAILED]  = "failed",
    [HU_HULA_SKIPPED] = "skipped",
    [HU_HULA_CANCELLED] = "cancelled",
};

const char *hu_hula_op_name(hu_hula_op_t op) {
    if ((unsigned)op < HU_HULA_OP_COUNT) return op_names[op];
    return "unknown";
}

const char *hu_hula_pred_name(hu_hula_pred_t pred) {
    if ((unsigned)pred <= HU_HULA_PRED_ALWAYS) return pred_names[pred];
    return "unknown";
}

const char *hu_hula_status_name(hu_hula_status_t status) {
    if ((unsigned)status <= HU_HULA_CANCELLED) return status_names[status];
    return "unknown";
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

hu_error_t hu_hula_program_init(hu_hula_program_t *prog, hu_allocator_t alloc,
                                const char *name, size_t name_len) {
    if (!prog) return HU_ERR_INVALID_ARGUMENT;
    memset(prog, 0, sizeof(*prog));
    prog->alloc = alloc;
    prog->version = HU_HULA_VERSION;
    if (name && name_len > 0) {
        prog->name = hu_strndup(&alloc, name, name_len);
        if (!prog->name) return HU_ERR_OUT_OF_MEMORY;
        prog->name_len = name_len;
    }
    return HU_OK;
}

static void hula_node_clear(hu_allocator_t *alloc, hu_hula_node_t *n) {
    if (n->id)              hu_str_free(alloc, n->id);
    if (n->tool_name)       hu_str_free(alloc, n->tool_name);
    if (n->args_json)       hu_str_free(alloc, n->args_json);
    if (n->match_str)       hu_str_free(alloc, n->match_str);
    if (n->goal)            hu_str_free(alloc, n->goal);
    if (n->delegate_model)  hu_str_free(alloc, n->delegate_model);
    if (n->emit_key)        hu_str_free(alloc, n->emit_key);
    if (n->emit_value)           hu_str_free(alloc, n->emit_value);
    if (n->delegate_context)     hu_str_free(alloc, n->delegate_context);
    if (n->delegate_result_key)  hu_str_free(alloc, n->delegate_result_key);
    if (n->delegate_agent_id)    hu_str_free(alloc, n->delegate_agent_id);
    if (n->verify_node_id)       hu_str_free(alloc, n->verify_node_id);
    if (n->required_capability)   hu_str_free(alloc, n->required_capability);
    if (n->description)        hu_str_free(alloc, n->description);
    memset(n, 0, sizeof(*n));
}

void hu_hula_program_deinit(hu_hula_program_t *prog) {
    if (!prog) return;
    for (size_t i = 0; i < prog->node_count; i++)
        hula_node_clear(&prog->alloc, &prog->nodes[i]);
    if (prog->nodes)
        prog->alloc.free(prog->alloc.ctx, prog->nodes,
                         prog->node_cap * sizeof(hu_hula_node_t));
    if (prog->name) hu_str_free(&prog->alloc, prog->name);
    memset(prog, 0, sizeof(*prog));
}

hu_hula_node_t *hu_hula_program_alloc_node(hu_hula_program_t *prog, hu_hula_op_t op,
                                            const char *id) {
    if (!prog || prog->node_count >= HU_HULA_MAX_NODES) return NULL;
    if (prog->node_count >= prog->node_cap) {
        size_t new_cap = prog->node_cap ? prog->node_cap * 2 : 16;
        if (new_cap > HU_HULA_MAX_NODES) new_cap = HU_HULA_MAX_NODES;
        hu_hula_node_t *buf = prog->alloc.alloc(prog->alloc.ctx,
                                                  new_cap * sizeof(hu_hula_node_t));
        if (!buf) return NULL;
        memset(buf, 0, new_cap * sizeof(hu_hula_node_t));
        if (prog->nodes) {
            memcpy(buf, prog->nodes, prog->node_count * sizeof(hu_hula_node_t));
            prog->alloc.free(prog->alloc.ctx, prog->nodes,
                             prog->node_cap * sizeof(hu_hula_node_t));
        }
        /* Fix child pointers after realloc */
        ptrdiff_t delta = (char *)buf - (char *)prog->nodes;
        if (prog->nodes) {
            for (size_t i = 0; i < prog->node_count; i++) {
                for (size_t c = 0; c < buf[i].children_count; c++) {
                    if (buf[i].children[c])
                        buf[i].children[c] = (hu_hula_node_t *)((char *)buf[i].children[c] + delta);
                }
            }
        }
        if (prog->root)
            prog->root = (hu_hula_node_t *)((char *)prog->root + delta);
        prog->nodes = buf;
        prog->node_cap = new_cap;
    }
    hu_hula_node_t *n = &prog->nodes[prog->node_count++];
    memset(n, 0, sizeof(*n));
    n->op = op;
    if (id) {
        n->id = hu_strdup(&prog->alloc, id);
        if (!n->id) { prog->node_count--; return NULL; }
    }
    return n;
}

/* ── Parse JSON ─────────────────────────────────────────────────────────── */

static hu_hula_op_t parse_op(const char *s) {
    if (!s) return HU_HULA_CALL;
    for (unsigned i = 0; i < HU_HULA_OP_COUNT; i++) {
        if (strcmp(s, op_names[i]) == 0) return (hu_hula_op_t)i;
    }
    return HU_HULA_CALL;
}

static hu_hula_pred_t parse_pred(const char *s) {
    if (!s) return HU_HULA_PRED_SUCCESS;
    for (unsigned i = 0; i <= HU_HULA_PRED_ALWAYS; i++) {
        if (strcmp(s, pred_names[i]) == 0) return (hu_hula_pred_t)i;
    }
    return HU_HULA_PRED_SUCCESS;
}

static hu_hula_node_t *parse_node(hu_hula_program_t *prog, const hu_json_value_t *obj,
                                   int depth);

static void parse_children(hu_hula_program_t *prog, hu_hula_node_t *parent,
                            const hu_json_value_t *arr, int depth) {
    if (!arr || arr->type != HU_JSON_ARRAY) return;
    for (size_t i = 0; i < arr->data.array.len && parent->children_count < HU_HULA_MAX_CHILDREN; i++) {
        hu_hula_node_t *child = parse_node(prog, arr->data.array.items[i], depth + 1);
        if (child) parent->children[parent->children_count++] = child;
    }
}

static hu_hula_node_t *parse_node(hu_hula_program_t *prog, const hu_json_value_t *obj,
                                   int depth) {
    if (!obj || obj->type != HU_JSON_OBJECT || depth > HU_HULA_MAX_DEPTH) return NULL;

    const char *op_str = hu_json_get_string(obj, "op");
    const char *id_str = hu_json_get_string(obj, "id");
    hu_hula_op_t op = parse_op(op_str);

    hu_hula_node_t *n = hu_hula_program_alloc_node(prog, op, id_str);
    if (!n) return NULL;

    const char *s;
    if ((s = hu_json_get_string(obj, "tool")))
        n->tool_name = hu_strdup(&prog->alloc, s);
    if ((s = hu_json_get_string(obj, "description")))
        n->description = hu_strdup(&prog->alloc, s);
    if ((s = hu_json_get_string(obj, "goal"))) {
        n->goal = hu_strdup(&prog->alloc, s);
        n->goal_len = strlen(s);
    }
    if ((s = hu_json_get_string(obj, "model"))) {
        n->delegate_model = hu_strdup(&prog->alloc, s);
        n->delegate_model_len = strlen(s);
    }
    if ((s = hu_json_get_string(obj, "emit_key"))) {
        n->emit_key = hu_strdup(&prog->alloc, s);
        n->emit_key_len = strlen(s);
    }
    if ((s = hu_json_get_string(obj, "emit_value"))) {
        n->emit_value = hu_strdup(&prog->alloc, s);
        n->emit_value_len = strlen(s);
    }
    if ((s = hu_json_get_string(obj, "context"))) {
        n->delegate_context = hu_strdup(&prog->alloc, s);
        n->delegate_context_len = strlen(s);
    }
    if ((s = hu_json_get_string(obj, "result_key"))) {
        n->delegate_result_key = hu_strdup(&prog->alloc, s);
        n->delegate_result_key_len = strlen(s);
    }
    if ((s = hu_json_get_string(obj, "agent_id"))) {
        n->delegate_agent_id = hu_strdup(&prog->alloc, s);
        n->delegate_agent_id_len = strlen(s);
    }
    if ((s = hu_json_get_string(obj, "verify_node_id"))) {
        n->verify_node_id = hu_strdup(&prog->alloc, s);
        n->verify_node_id_len = strlen(s);
    }
    if ((s = hu_json_get_string(obj, "required_capability"))) {
        n->required_capability = hu_strdup(&prog->alloc, s);
        n->required_capability_len = strlen(s);
    }
    if ((s = hu_json_get_string(obj, "pred")))
        n->pred = parse_pred(s);
    if ((s = hu_json_get_string(obj, "match"))) {
        n->match_str = hu_strdup(&prog->alloc, s);
        n->match_str_len = strlen(s);
    }

    n->max_iter = (uint32_t)hu_json_get_number(obj, "max_iter", 0);
    n->timeout_ms = (uint32_t)hu_json_get_number(obj, "timeout_ms", 0);
    n->retry_count = (uint32_t)hu_json_get_number(obj, "retry_count", 0);
    n->retry_backoff_ms = (uint32_t)hu_json_get_number(obj, "retry_backoff_ms", 0);

    /* args: stringify sub-object to JSON string */
    hu_json_value_t *args = hu_json_object_get(obj, "args");
    if (args) {
        char *args_str = NULL;
        size_t args_str_len = 0;
        if (hu_json_stringify(&prog->alloc, args, &args_str, &args_str_len) == HU_OK)
            n->args_json = args_str;
    }

    /* Recurse into children */
    hu_json_value_t *children = hu_json_object_get(obj, "children");
    parse_children(prog, n, children, depth);

    /* BRANCH shorthand: "then" / "else" as children[0] / children[1] */
    if (op == HU_HULA_BRANCH) {
        hu_json_value_t *then_val = hu_json_object_get(obj, "then");
        hu_json_value_t *else_val = hu_json_object_get(obj, "else");
        if (then_val && n->children_count < HU_HULA_MAX_CHILDREN) {
            hu_hula_node_t *t = parse_node(prog, then_val, depth + 1);
            if (t) n->children[n->children_count++] = t;
        }
        if (else_val && n->children_count < HU_HULA_MAX_CHILDREN) {
            hu_hula_node_t *e = parse_node(prog, else_val, depth + 1);
            if (e) n->children[n->children_count++] = e;
        }
    }

    /* LOOP shorthand: "body" as single child */
    if (op == HU_HULA_LOOP) {
        hu_json_value_t *body = hu_json_object_get(obj, "body");
        if (body && n->children_count == 0 && n->children_count < HU_HULA_MAX_CHILDREN) {
            hu_hula_node_t *b = parse_node(prog, body, depth + 1);
            if (b) n->children[n->children_count++] = b;
        }
    }

    if (op == HU_HULA_TRY) {
        hu_json_value_t *body = hu_json_object_get(obj, "body");
        hu_json_value_t *catchv = hu_json_object_get(obj, "catch");
        if (body && n->children_count < HU_HULA_MAX_CHILDREN) {
            hu_hula_node_t *b = parse_node(prog, body, depth + 1);
            if (b) n->children[n->children_count++] = b;
        }
        if (catchv && n->children_count < HU_HULA_MAX_CHILDREN) {
            hu_hula_node_t *c = parse_node(prog, catchv, depth + 1);
            if (c) n->children[n->children_count++] = c;
        }
    }

    return n;
}

hu_error_t hu_hula_parse_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                              hu_hula_program_t *out) {
    if (!alloc || !json || !out) return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK) return err;
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    const char *name = hu_json_get_string(root, "name");
    err = hu_hula_program_init(out, *alloc, name, name ? strlen(name) : 0);
    if (err != HU_OK) { hu_json_free(alloc, root); return err; }

    out->version = (uint32_t)hu_json_get_number(root, "version", HU_HULA_VERSION);

    hu_json_value_t *root_node = hu_json_object_get(root, "root");
    if (root_node && root_node->type == HU_JSON_OBJECT) {
        out->root = parse_node(out, root_node, 0);
    }

    hu_json_free(alloc, root);
    if (!out->root) {
        /* hu_hula_program_init above allocated `prog->name`; the other
         * parse_* entry points in this file (lines 2064/2070/2081/...) all
         * deinit on the failure path. PR55's ASan run flagged a 6-byte leak
         * here from hula_parse_missing_root_returns_error feeding `{"name":
         * "empty"}` — name copy survives, error returns, caller never
         * inits→deinits. Match the existing pattern. */
        hu_hula_program_deinit(out);
        return HU_ERR_PARSE;
    }
    return HU_OK;
}

/* ── Serialize to JSON ──────────────────────────────────────────────────── */

static hu_error_t serialize_node(hu_json_buf_t *buf, const hu_hula_node_t *n, int depth);

static hu_error_t serialize_children(hu_json_buf_t *buf, const hu_hula_node_t *n, int depth) {
    hu_error_t err;
    if ((err = hu_json_buf_append_raw(buf, "\"children\":[", 12)) != HU_OK) return err;
    for (size_t i = 0; i < n->children_count; i++) {
        if (i > 0) { if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err; }
        if ((err = serialize_node(buf, n->children[i], depth + 1)) != HU_OK) return err;
    }
    return hu_json_buf_append_raw(buf, "]", 1);
}

static hu_error_t serialize_node(hu_json_buf_t *buf, const hu_hula_node_t *n, int depth) {
    if (!n || depth > HU_HULA_MAX_DEPTH) return HU_ERR_INVALID_ARGUMENT;
    hu_error_t err;
    if ((err = hu_json_buf_append_raw(buf, "{", 1)) != HU_OK) return err;

    if ((err = hu_json_append_key_value(buf, "op", 2, hu_hula_op_name(n->op),
                                         strlen(hu_hula_op_name(n->op)))) != HU_OK) return err;
    if (n->id) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "id", 2, n->id, strlen(n->id))) != HU_OK) return err;
    }
    if (n->tool_name) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "tool", 4, n->tool_name,
                                             strlen(n->tool_name))) != HU_OK) return err;
    }
    if (n->args_json) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key(buf, "args", 4)) != HU_OK) return err;
        if ((err = hu_json_buf_append_raw(buf, n->args_json, strlen(n->args_json))) != HU_OK) return err;
    }
    if (n->description) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "description", 11, n->description,
                                             strlen(n->description))) != HU_OK) return err;
    }
    if (n->goal) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "goal", 4, n->goal, n->goal_len)) != HU_OK)
            return err;
    }
    if (n->delegate_model) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "model", 5, n->delegate_model,
                                             n->delegate_model_len)) != HU_OK) return err;
    }
    if (n->emit_key) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "emit_key", 8, n->emit_key,
                                             n->emit_key_len)) != HU_OK) return err;
    }
    if (n->emit_value) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "emit_value", 10, n->emit_value,
                                             n->emit_value_len)) != HU_OK) return err;
    }
    if (n->op == HU_HULA_BRANCH || n->op == HU_HULA_LOOP) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "pred", 4, hu_hula_pred_name(n->pred),
                                             strlen(hu_hula_pred_name(n->pred)))) != HU_OK)
            return err;
        if (n->match_str) {
            if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
            if ((err = hu_json_append_key_value(buf, "match", 5, n->match_str,
                                                 n->match_str_len)) != HU_OK) return err;
        }
    }
    if (n->op == HU_HULA_LOOP && n->max_iter > 0) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_int(buf, "max_iter", 8, (long long)n->max_iter)) != HU_OK)
            return err;
    }
    if (n->timeout_ms > 0) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_int(buf, "timeout_ms", 10, (long long)n->timeout_ms)) != HU_OK)
            return err;
    }
    if (n->retry_count > 0) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_int(buf, "retry_count", 11, (long long)n->retry_count)) != HU_OK)
            return err;
    }
    if (n->retry_backoff_ms > 0) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_int(buf, "retry_backoff_ms", 16,
                                          (long long)n->retry_backoff_ms)) != HU_OK)
            return err;
    }
    if (n->delegate_context) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "context", 7, n->delegate_context,
                                             n->delegate_context_len)) != HU_OK)
            return err;
    }
    if (n->delegate_result_key) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "result_key", 10, n->delegate_result_key,
                                             n->delegate_result_key_len)) != HU_OK)
            return err;
    }
    if (n->delegate_agent_id) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "agent_id", 8, n->delegate_agent_id,
                                             n->delegate_agent_id_len)) != HU_OK)
            return err;
    }
    if (n->required_capability) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = hu_json_append_key_value(buf, "required_capability", 19, n->required_capability,
                                             n->required_capability_len)) != HU_OK)
            return err;
    }
    if (n->op == HU_HULA_TRY) {
        if (n->children_count > 0) {
            if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
            if ((err = hu_json_append_key(buf, "body", 4)) != HU_OK) return err;
            if ((err = serialize_node(buf, n->children[0], depth + 1)) != HU_OK) return err;
        }
        if (n->children_count > 1) {
            if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
            if ((err = hu_json_append_key(buf, "catch", 5)) != HU_OK) return err;
            if ((err = serialize_node(buf, n->children[1], depth + 1)) != HU_OK) return err;
        }
        return hu_json_buf_append_raw(buf, "}", 1);
    }
    if (n->children_count > 0) {
        if ((err = hu_json_buf_append_raw(buf, ",", 1)) != HU_OK) return err;
        if ((err = serialize_children(buf, n, depth)) != HU_OK) return err;
    }

    return hu_json_buf_append_raw(buf, "}", 1);
}

hu_error_t hu_hula_to_json(hu_allocator_t *alloc, const hu_hula_program_t *prog,
                           char **out, size_t *out_len) {
    if (!alloc || !prog || !out || !out_len) return HU_ERR_INVALID_ARGUMENT;

    hu_json_buf_t buf;
    hu_error_t err = hu_json_buf_init(&buf, alloc);
    if (err != HU_OK) return err;

    if ((err = hu_json_buf_append_raw(&buf, "{", 1)) != HU_OK) goto fail;
    if (prog->name) {
        if ((err = hu_json_append_key_value(&buf, "name", 4, prog->name,
                                             prog->name_len)) != HU_OK) goto fail;
        if ((err = hu_json_buf_append_raw(&buf, ",", 1)) != HU_OK) goto fail;
    }
    if ((err = hu_json_append_key_int(&buf, "version", 7, prog->version)) != HU_OK) goto fail;
    if (prog->root) {
        if ((err = hu_json_buf_append_raw(&buf, ",", 1)) != HU_OK) goto fail;
        if ((err = hu_json_append_key(&buf, "root", 4)) != HU_OK) goto fail;
        if ((err = serialize_node(&buf, prog->root, 0)) != HU_OK) goto fail;
    }
    if ((err = hu_json_buf_append_raw(&buf, "}", 1)) != HU_OK) goto fail;

    *out = buf.ptr;
    *out_len = buf.len;
    return HU_OK;

fail:
    hu_json_buf_free(&buf);
    return err;
}

/* ── Validate ───────────────────────────────────────────────────────────── */

static void add_diag(hu_hula_validation_t *v, hu_allocator_t *alloc,
                      const hu_hula_node_t *node, const char *msg) {
    if (v->diag_count >= HU_HULA_MAX_DIAGS) return;
    hu_hula_diag_t *d = &v->diags[v->diag_count++];
    d->message = hu_strdup(alloc, msg);
    d->message_len = msg ? strlen(msg) : 0;
    d->node = node;
    v->valid = false;
}

static void validate_node(const hu_hula_node_t *n, hu_allocator_t *alloc,
                           const char *const *tool_names, size_t tool_count,
                           hu_hula_validation_t *v, int depth) {
    if (depth > HU_HULA_MAX_DEPTH) {
        add_diag(v, alloc, n, "exceeds max depth");
        return;
    }
    if (!n->id) add_diag(v, alloc, n, "node missing id");

    switch (n->op) {
    case HU_HULA_CALL:
        if (!n->tool_name)
            add_diag(v, alloc, n, "call node missing tool name");
        else if (tool_names) {
            bool found = false;
            for (size_t i = 0; i < tool_count; i++) {
                if (strcmp(tool_names[i], n->tool_name) == 0) { found = true; break; }
            }
            if (!found) {
                char msg[128];
                (void)snprintf(msg, sizeof(msg), "unknown tool: %.64s", n->tool_name);
                add_diag(v, alloc, n, msg);
            }
        }
        break;
    case HU_HULA_SEQ:
    case HU_HULA_PAR:
        if (n->children_count == 0)
            add_diag(v, alloc, n, "seq/par node has no children");
        break;
    case HU_HULA_BRANCH:
        if (n->children_count < 1)
            add_diag(v, alloc, n, "branch needs at least a then child");
        break;
    case HU_HULA_LOOP:
        if (n->children_count == 0)
            add_diag(v, alloc, n, "loop has no body");
        break;
    case HU_HULA_DELEGATE:
        if (!n->goal || n->goal_len == 0)
            add_diag(v, alloc, n, "delegate node missing goal");
        break;
    case HU_HULA_EMIT:
        if (!n->emit_key || n->emit_key_len == 0)
            add_diag(v, alloc, n, "emit node missing key");
        break;
    case HU_HULA_TRY:
        if (n->children_count < 1)
            add_diag(v, alloc, n, "try needs a body child");
        break;
    case HU_HULA_VERIFY:
        if (!n->verify_node_id || n->verify_node_id_len == 0)
            add_diag(v, alloc, n, "verify needs verify_node_id");
        break;
    default:
        add_diag(v, alloc, n, "unknown opcode");
        break;
    }

    for (size_t i = 0; i < n->children_count; i++) {
        if (n->children[i])
            validate_node(n->children[i], alloc, tool_names, tool_count, v, depth + 1);
    }
}

hu_error_t hu_hula_validate(const hu_hula_program_t *prog, hu_allocator_t *alloc,
                            const char *const *tool_names, size_t tool_count,
                            hu_hula_validation_t *out) {
    if (!prog || !alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->valid = true;

    if (!prog->root) {
        add_diag(out, alloc, NULL, "program has no root node");
        return HU_OK;
    }

    validate_node(prog->root, alloc, tool_names, tool_count, out, 0);
    return HU_OK;
}

void hu_hula_validation_deinit(hu_allocator_t *alloc, hu_hula_validation_t *v) {
    if (!v) return;
    for (size_t i = 0; i < v->diag_count; i++) {
        if (v->diags[i].message)
            hu_str_free(alloc, v->diags[i].message);
    }
    memset(v, 0, sizeof(*v));
}

/* ── Execute ────────────────────────────────────────────────────────────── */

hu_error_t hu_hula_exec_init(hu_hula_exec_t *exec, hu_allocator_t alloc,
                             hu_hula_program_t *prog, hu_tool_t *tools, size_t tools_count) {
    if (!exec || !prog) return HU_ERR_INVALID_ARGUMENT;
    memset(exec, 0, sizeof(*exec));
    exec->alloc = alloc;
    exec->program = prog;
    exec->tools = tools;
    exec->tools_count = tools_count;
    exec->results_count = prog->node_count;
    if (prog->node_count > 0) {
        exec->results = alloc.alloc(alloc.ctx, prog->node_count * sizeof(hu_hula_result_t));
        if (!exec->results) return HU_ERR_OUT_OF_MEMORY;
        memset(exec->results, 0, prog->node_count * sizeof(hu_hula_result_t));
    }
    return HU_OK;
}

hu_error_t hu_hula_exec_init_full(hu_hula_exec_t *exec, hu_allocator_t alloc,
                                  hu_hula_program_t *prog, hu_tool_t *tools, size_t tools_count,
                                  hu_security_policy_t *policy, hu_observer_t *observer) {
    hu_error_t err = hu_hula_exec_init(exec, alloc, prog, tools, tools_count);
    if (err != HU_OK) return err;
    exec->policy = policy;
    exec->observer = observer;
    exec->trace_log_cap = 64;
    exec->trace_log = alloc.alloc(alloc.ctx, exec->trace_log_cap);
    if (!exec->trace_log) {
        hu_hula_exec_deinit(exec);
        memset(exec, 0, sizeof(*exec));
        return HU_ERR_OUT_OF_MEMORY;
    }
    exec->trace_log[0] = '[';
    exec->trace_log[1] = '\0';
    exec->trace_log_len = 1;
    return HU_OK;
}

void hu_hula_exec_set_spawn(hu_hula_exec_t *exec, struct hu_agent_pool *pool,
                            struct hu_spawn_config *spawn_cfg) {
    if (!exec)
        return;
    exec->pool = pool;
    exec->spawn_cfg = spawn_cfg;
}

void hu_hula_exec_set_delegate_registry(hu_hula_exec_t *exec, struct hu_agent_registry *registry) {
    if (!exec)
        return;
    exec->delegate_registry = registry;
}

void hu_hula_exec_set_security_agent(hu_hula_exec_t *exec, struct hu_agent *agent) {
    if (!exec)
        return;
    exec->security_agent = agent;
}

void hu_hula_exec_set_idempotency_registry(hu_hula_exec_t *exec,
                                           struct hu_idempotency_registry *registry) {
    if (!exec)
        return;
    exec->idempotency_registry = registry;
}

void hu_hula_exec_cancel(hu_hula_exec_t *exec, const char *reason, size_t reason_len) {
    if (!exec)
        return;
    exec->halted = true;
    if (exec->halt_reason)
        exec->alloc.free(exec->alloc.ctx, exec->halt_reason, exec->halt_reason_len + 1);
    exec->halt_reason = NULL;
    exec->halt_reason_len = 0;
    if (reason && reason_len > 0) {
        exec->halt_reason = hu_strndup(&exec->alloc, reason, reason_len);
        if (exec->halt_reason)
            exec->halt_reason_len = reason_len;
    }
}

void hu_hula_exec_set_budget(hu_hula_exec_t *exec, uint32_t max_depth, uint32_t max_wall_ms,
                             uint32_t max_tool_calls) {
    if (!exec)
        return;
    exec->budget_max_depth = max_depth;
    exec->budget_max_wall_ms = max_wall_ms;
    exec->budget_max_tool_calls = max_tool_calls;
    exec->budget_enabled =
        (max_depth > 0) || (max_wall_ms > 0) || (max_tool_calls > 0);
}

static size_t node_index(const hu_hula_program_t *prog, const hu_hula_node_t *n) {
    return (size_t)(n - prog->nodes);
}

static hu_hula_result_t *result_for(hu_hula_exec_t *exec, const hu_hula_node_t *n) {
    size_t idx = node_index(exec->program, n);
    if (idx < exec->results_count) return &exec->results[idx];
    return NULL;
}

static hu_tool_t *find_tool(hu_tool_t *tools, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        const char *tn = tools[i].vtable->name(tools[i].ctx);
        if (tn && strcmp(tn, name) == 0) return &tools[i];
    }
    return NULL;
}

static hu_error_t exec_node_depth(hu_hula_exec_t *exec, hu_hula_node_t *n, int depth);
static hu_error_t exec_dispatch(hu_hula_exec_t *exec, hu_hula_node_t *n, int depth);

static void hula_result_reset(hu_hula_exec_t *exec, hu_hula_node_t *n) {
    hu_hula_result_t *r = result_for(exec, n);
    if (!r)
        return;
    if (r->output) {
        exec->alloc.free(exec->alloc.ctx, r->output, r->output_len + 1);
        r->output = NULL;
    }
    if (r->error) {
        exec->alloc.free(exec->alloc.ctx, r->error, r->error_len + 1);
        r->error = NULL;
    }
    memset(r, 0, sizeof(*r));
}

static hu_error_t hula_slot_set(hu_hula_exec_t *exec, const char *key, size_t key_len,
                                const char *val, size_t val_len) {
    if (!exec || !key || key_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < exec->slot_count; i++) {
        if (exec->slots[i].key_len == key_len && memcmp(exec->slots[i].key, key, key_len) == 0) {
            if (exec->slots[i].value)
                exec->alloc.free(exec->alloc.ctx, exec->slots[i].value,
                                 exec->slots[i].value_len + 1);
            exec->slots[i].value =
                val_len > 0 ? hu_strndup(&exec->alloc, val, val_len) : hu_strdup(&exec->alloc, "");
            exec->slots[i].value_len = val_len;
            return (exec->slots[i].value) ? HU_OK : HU_ERR_OUT_OF_MEMORY;
        }
    }
    if (exec->slot_count >= HU_HULA_MAX_SLOTS)
        return HU_ERR_OUT_OF_MEMORY;
    hu_hula_slot_t *sl = &exec->slots[exec->slot_count];
    sl->key = hu_strndup(&exec->alloc, key, key_len);
    sl->key_len = key_len;
    sl->value = val_len > 0 ? hu_strndup(&exec->alloc, val, val_len) : hu_strdup(&exec->alloc, "");
    sl->value_len = val_len;
    if (!sl->key || !sl->value)
        return HU_ERR_OUT_OF_MEMORY;
    exec->slot_count++;
    return HU_OK;
}

static void set_result(hu_hula_exec_t *exec, hu_hula_node_t *n, hu_hula_status_t status,
                       const char *output, size_t output_len, const char *error, size_t error_len) {
    hu_hula_result_t *r = result_for(exec, n);
    if (!r)
        return;
    if (r->output) {
        exec->alloc.free(exec->alloc.ctx, r->output, r->output_len + 1);
        r->output = NULL;
    }
    if (r->error) {
        exec->alloc.free(exec->alloc.ctx, r->error, r->error_len + 1);
        r->error = NULL;
    }
    r->status = status;
    if (output && output_len > 0)
        r->output = hu_strndup(&exec->alloc, output, output_len);
    r->output_len = output_len;
    if (error && error_len > 0)
        r->error = hu_strndup(&exec->alloc, error, error_len);
    r->error_len = error_len;
}

static bool eval_pred(hu_hula_exec_t *exec, hu_hula_node_t *n, hu_hula_result_t *last) {
    (void)exec;
    switch (n->pred) {
    case HU_HULA_PRED_SUCCESS:
        return last && last->status == HU_HULA_DONE;
    case HU_HULA_PRED_FAILURE:
        return last && last->status == HU_HULA_FAILED;
    case HU_HULA_PRED_CONTAINS:
        return last && last->output && n->match_str &&
               strstr(last->output, n->match_str) != NULL;
    case HU_HULA_PRED_NOT_CONTAINS:
        return !last || !last->output || !n->match_str ||
               strstr(last->output, n->match_str) == NULL;
    case HU_HULA_PRED_ALWAYS:
        return true;
    }
    return false;
}

/* Get the last completed result for predecessor context */
static hu_hula_result_t *last_result(hu_hula_exec_t *exec) {
    for (size_t i = exec->program->node_count; i > 0; i--) {
        hu_hula_result_t *r = &exec->results[i - 1];
        if (r->status == HU_HULA_DONE || r->status == HU_HULA_FAILED) return r;
    }
    return NULL;
}

static bool trace_ensure(hu_hula_exec_t *exec, size_t min_extra) {
    size_t need = exec->trace_log_len + min_extra + 1;
    if (need <= exec->trace_log_cap) return true;
    size_t new_cap = exec->trace_log_cap ? exec->trace_log_cap * 2 : 64;
    while (new_cap < need) new_cap *= 2;
    hu_allocator_t *a = &exec->alloc;
    char *nb;
    if (a->realloc) {
        nb = (char *)a->realloc(a->ctx, exec->trace_log, exec->trace_log_cap, new_cap);
    } else {
        nb = (char *)a->alloc(a->ctx, new_cap);
        if (nb && exec->trace_log) {
            memcpy(nb, exec->trace_log, exec->trace_log_len + 1);
            a->free(a->ctx, exec->trace_log, exec->trace_log_cap);
        }
    }
    if (!nb) return false;
    exec->trace_log = nb;
    exec->trace_log_cap = new_cap;
    return true;
}

static void trace_append(hu_hula_exec_t *exec, const hu_hula_node_t *n) {
    if (!exec->trace_log || !n) return;
    const char *id = n->id ? n->id : "";
    const char *opn = hu_hula_op_name(n->op);
    hu_hula_result_t *r = result_for(exec, (hu_hula_node_t *)n);
    const char *st = r ? hu_hula_status_name(r->status) : "pending";
    size_t outlen = r ? r->output_len : 0;

    for (;;) {
        size_t room = exec->trace_log_cap > exec->trace_log_len
                          ? exec->trace_log_cap - exec->trace_log_len - 1
                          : 0;
        const char *safe_tool = NULL;
        if (n->op == HU_HULA_CALL && n->tool_name && n->tool_name[0]) {
            bool ok = true;
            for (const char *p = n->tool_name; *p && ok; p++) {
                if (*p == '"' || *p == '\\' || (unsigned char)*p < 32)
                    ok = false;
            }
            if (ok)
                safe_tool = n->tool_name;
        }
        int npr;
        if (safe_tool)
            npr = snprintf(exec->trace_log + exec->trace_log_len, room > 0 ? room : 0,
                           "{\"id\":\"%s\",\"op\":\"%s\",\"tool\":\"%s\",\"status\":\"%s\","
                           "\"output_len\":%zu},",
                           id, opn, safe_tool, st, outlen);
        else
            npr = snprintf(exec->trace_log + exec->trace_log_len, room > 0 ? room : 0,
                           "{\"id\":\"%s\",\"op\":\"%s\",\"status\":\"%s\",\"output_len\":%zu},", id,
                           opn, st, outlen);
        if (npr < 0) return;
        if (room > 0 && (size_t)npr < room) {
            exec->trace_log_len += (size_t)npr;
            return;
        }
        if (!trace_ensure(exec, (size_t)npr + 1)) return;
    }
}

static hu_error_t exec_call(hu_hula_exec_t *exec, hu_hula_node_t *n) {
    /* Policy check runs first — before tool lookup or arg parsing */
    if (exec->policy) {
        hu_command_risk_level_t risk = hu_tool_risk_level(n->tool_name);
        if (exec->policy->autonomy == HU_AUTONOMY_LOCKED) {
            set_result(exec, n, HU_HULA_FAILED, NULL, 0, "blocked by policy: locked",
                       strlen("blocked by policy: locked"));
            return HU_OK;
        }
        if (risk == HU_RISK_HIGH && exec->policy->block_high_risk_commands) {
            set_result(exec, n, HU_HULA_FAILED, NULL, 0, "blocked by policy: high risk",
                       strlen("blocked by policy: high risk"));
            return HU_OK;
        }
        if (exec->policy->tracker && hu_policy_is_rate_limited(exec->policy)) {
            set_result(exec, n, HU_HULA_FAILED, NULL, 0, "rate limited", 12);
            return HU_OK;
        }
    }

    if (exec->budget_enabled && exec->budget_max_tool_calls > 0 &&
        exec->budget_tool_calls_used >= exec->budget_max_tool_calls) {
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "hula budget: tool call limit exceeded",
                   strlen("hula budget: tool call limit exceeded"));
        return HU_OK;
    }

    hu_tool_t *tool = find_tool(exec->tools, exec->tools_count, n->tool_name);
    if (!tool) {
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "tool not found", 14);
        return HU_OK;
    }

    hu_json_value_t *args = NULL;
    const char *args_src = n->args_json ? n->args_json : "{}";
    hu_error_t err = hu_json_parse(&exec->alloc, args_src, strlen(args_src), &args);
    if (err != HU_OK) {
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "args parse error", 16);
        return HU_OK;
    }

    if (strchr(args_src, '$') != NULL) {
        err = hula_substitute_json_strings(exec, args);
        if (err != HU_OK) {
            hu_json_free(&exec->alloc, args);
            set_result(exec, n, HU_HULA_FAILED, NULL, 0, "hula $ref in args failed",
                       strlen("hula $ref in args failed"));
            return HU_OK;
        }
    }

    hu_tool_result_t tr;
    memset(&tr, 0, sizeof(tr));

    /* Check idempotency registry before executing tool */
    bool use_cached_result = false;
    hu_idempotency_entry_t cached_entry;
    if (exec->idempotency_registry) {
        if (hu_idempotency_check(exec->idempotency_registry, n->tool_name, n->args_json ? n->args_json : "{}", &cached_entry)) {
            use_cached_result = true;
            /* Reconstruct tool result from cached entry */
            if (cached_entry.is_error) {
                tr.success = false;
                tr.error_msg = cached_entry.result_json;
                tr.error_msg_len = cached_entry.result_json_len;
                tr.output = "";
                tr.output_len = 0;
            } else {
                tr.success = true;
                tr.output = cached_entry.result_json;
                tr.output_len = cached_entry.result_json_len;
                tr.error_msg = NULL;
                tr.error_msg_len = 0;
            }
            tr.output_owned = false;
            tr.error_msg_owned = false;

            if (exec->observer) {
                hu_observer_event_t ev = {0};
                ev.tag = HU_OBSERVER_EVENT_TOOL_CALL;
                ev.trace_id = NULL;
                ev.data.tool_call.tool = n->tool_name;
                ev.data.tool_call.duration_ms = 0;
                ev.data.tool_call.success = tr.success;
                ev.data.tool_call.detail = NULL;
                hu_observer_record_event(*exec->observer, &ev);
            }
        }
    }

    if (!use_cached_result) {
        /* Wave A: optional agent security envelope before execute. */
        if (exec->security_agent && n->tool_name) {
            const char *gate_args = n->args_json ? n->args_json : "{}";
            hu_tool_result_t gate_out = {0};
            hu_tool_gate_t gate = hu_agent_internal_pre_execute_checks(
                (hu_agent_t *)exec->security_agent, n->tool_name, strlen(n->tool_name), gate_args,
                strlen(gate_args), &gate_out);
            if (gate != HU_TOOL_GATE_ALLOW) {
                const char *em = gate_out.error_msg ? gate_out.error_msg : "denied by security";
                size_t el = gate_out.error_msg ? gate_out.error_msg_len : 18;
                set_result(exec, n, HU_HULA_FAILED, NULL, 0, em, el);
                hu_agent_internal_post_hook_fire((hu_agent_t *)exec->security_agent, n->tool_name,
                                                 strlen(n->tool_name), gate_args, strlen(gate_args),
                                                 &gate_out);
                hu_tool_result_free(&exec->alloc, &gate_out);
                hu_json_free(&exec->alloc, args);
                return HU_OK;
            }
            hu_tool_result_free(&exec->alloc, &gate_out);
        }

        if (exec->observer) {
            hu_observer_event_t ev = {0};
            ev.tag = HU_OBSERVER_EVENT_TOOL_CALL_START;
            ev.trace_id = NULL;
            ev.data.tool_call_start.tool = n->tool_name;
            hu_observer_record_event(*exec->observer, &ev);
        }

        if (exec->budget_enabled && exec->budget_max_tool_calls > 0)
            exec->budget_tool_calls_used++;

        err = tool->vtable->execute(tool->ctx, &exec->alloc, args, &tr);

        if (exec->security_agent && n->tool_name) {
            const char *gate_args = n->args_json ? n->args_json : "{}";
            hu_agent_internal_post_hook_fire((hu_agent_t *)exec->security_agent, n->tool_name,
                                             strlen(n->tool_name), gate_args, strlen(gate_args),
                                             &tr);
        }

        /* Record result in idempotency registry for future replay */
        if (exec->idempotency_registry && err == HU_OK) {
            const char *result_to_cache = tr.success ? tr.output : tr.error_msg;
            hu_error_t record_err = hu_idempotency_record(exec->idempotency_registry, &exec->alloc,
                                                           n->tool_name,
                                                           n->args_json ? n->args_json : "{}",
                                                           result_to_cache ? result_to_cache : "{}",
                                                           !tr.success);
            /* Log but don't fail the tool call if recording fails */
            (void)record_err;
        }

        if (exec->observer) {
            hu_observer_event_t ev = {0};
            ev.tag = HU_OBSERVER_EVENT_TOOL_CALL;
            ev.trace_id = NULL;
            ev.data.tool_call.tool = n->tool_name;
            ev.data.tool_call.duration_ms = 0;
            ev.data.tool_call.success = (err == HU_OK && tr.success);
            ev.data.tool_call.detail = NULL;
            hu_observer_record_event(*exec->observer, &ev);
        }
    }

    hu_json_free(&exec->alloc, args);

    if (!use_cached_result && err != HU_OK) {
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "tool execution error", 20);
        return HU_OK;
    }

    if (tr.success)
        set_result(exec, n, HU_HULA_DONE, tr.output, tr.output_len, NULL, 0);
    else
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, tr.error_msg, tr.error_msg_len);

    hu_tool_result_free(&exec->alloc, &tr);
    return HU_OK;
}

static hu_error_t exec_seq(hu_hula_exec_t *exec, hu_hula_node_t *n, int depth) {
    for (size_t i = 0; i < n->children_count; i++) {
        hu_error_t err = exec_node_depth(exec, n->children[i], depth + 1);
        if (err != HU_OK)
            return err;
        hu_hula_result_t *cr = result_for(exec, n->children[i]);
        if (cr && cr->status == HU_HULA_FAILED) {
            set_result(exec, n, HU_HULA_FAILED, NULL, 0, cr->error, cr->error_len);
            return HU_OK;
        }
    }
    if (n->children_count > 0) {
        hu_hula_result_t *lr = result_for(exec, n->children[n->children_count - 1]);
        if (lr)
            set_result(exec, n, HU_HULA_DONE, lr->output, lr->output_len, NULL, 0);
    } else {
        set_result(exec, n, HU_HULA_DONE, NULL, 0, NULL, 0);
    }
    return HU_OK;
}

static hu_error_t exec_par(hu_hula_exec_t *exec, hu_hula_node_t *n, int depth) {
    bool any_failed = false;
    for (size_t i = 0; i < n->children_count; i++) {
        hu_error_t err = exec_node_depth(exec, n->children[i], depth + 1);
        if (err != HU_OK)
            return err;
        hu_hula_result_t *cr = result_for(exec, n->children[i]);
        if (cr && cr->status == HU_HULA_FAILED)
            any_failed = true;
    }
    set_result(exec, n, any_failed ? HU_HULA_FAILED : HU_HULA_DONE, NULL, 0,
               any_failed ? "child failed" : NULL, any_failed ? 12 : 0);
    return HU_OK;
}

static hu_error_t exec_branch(hu_hula_exec_t *exec, hu_hula_node_t *n, int depth) {
    hu_hula_result_t *lr = last_result(exec);
    bool cond = eval_pred(exec, n, lr);
    if (cond && n->children_count >= 1) {
        hu_error_t err = exec_node_depth(exec, n->children[0], depth + 1);
        if (err != HU_OK)
            return err;
        hu_hula_result_t *cr = result_for(exec, n->children[0]);
        if (cr)
            set_result(exec, n, cr->status, cr->output, cr->output_len, cr->error, cr->error_len);
    } else if (!cond && n->children_count >= 2) {
        hu_error_t err = exec_node_depth(exec, n->children[1], depth + 1);
        if (err != HU_OK)
            return err;
        hu_hula_result_t *cr = result_for(exec, n->children[1]);
        if (cr)
            set_result(exec, n, cr->status, cr->output, cr->output_len, cr->error, cr->error_len);
    } else {
        set_result(exec, n, HU_HULA_SKIPPED, NULL, 0, NULL, 0);
    }
    return HU_OK;
}

static hu_error_t exec_loop(hu_hula_exec_t *exec, hu_hula_node_t *n, int depth) {
    uint32_t max = n->max_iter > 0 ? n->max_iter : 10;
    for (uint32_t i = 0; i < max; i++) {
        hu_hula_result_t *lr = last_result(exec);
        if (n->pred != HU_HULA_PRED_ALWAYS && !eval_pred(exec, n, lr))
            break;

        for (size_t c = 0; c < n->children_count; c++) {
            hu_hula_result_t *cr = result_for(exec, n->children[c]);
            if (cr) {
                if (cr->output)
                    hu_str_free(&exec->alloc, cr->output);
                if (cr->error)
                    hu_str_free(&exec->alloc, cr->error);
                memset(cr, 0, sizeof(*cr));
            }
            hu_error_t err = exec_node_depth(exec, n->children[c], depth + 1);
            if (err != HU_OK)
                return err;
            cr = result_for(exec, n->children[c]);
            if (cr && cr->status == HU_HULA_FAILED) {
                set_result(exec, n, HU_HULA_FAILED, NULL, 0, cr->error, cr->error_len);
                return HU_OK;
            }
        }
    }
    set_result(exec, n, HU_HULA_DONE, NULL, 0, NULL, 0);
    return HU_OK;
}

static hu_error_t exec_try(hu_hula_exec_t *exec, hu_hula_node_t *n, int depth) {
    if (n->children_count < 1) {
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "try needs body", 14);
        return HU_OK;
    }
    hu_error_t err = exec_node_depth(exec, n->children[0], depth + 1);
    if (err != HU_OK)
        return err;
    hu_hula_result_t *br = result_for(exec, n->children[0]);
    if (br && br->status == HU_HULA_FAILED && n->children_count >= 2) {
        err = exec_node_depth(exec, n->children[1], depth + 1);
        if (err != HU_OK)
            return err;
        hu_hula_result_t *cr = result_for(exec, n->children[1]);
        if (cr)
            set_result(exec, n, cr->status, cr->output, cr->output_len, cr->error, cr->error_len);
        return HU_OK;
    }
    if (br)
        set_result(exec, n, br->status, br->output, br->output_len, br->error, br->error_len);
    else
        set_result(exec, n, HU_HULA_DONE, NULL, 0, NULL, 0);
    return HU_OK;
}

/* VERIFY: validate a preceding node's output against a predicate (VMAO-inspired). */
static hu_error_t exec_verify(hu_hula_exec_t *exec, hu_hula_node_t *n) {
    if (!n->verify_node_id || n->verify_node_id_len == 0) {
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "verify: missing node_id", 22);
        return HU_OK;
    }
    const hu_hula_result_t *target = hu_hula_exec_result(exec, n->verify_node_id);
    if (!target) {
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "verify: target not found", 24);
        return HU_OK;
    }
    if (target->status != HU_HULA_DONE) {
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "verify: target not done", 23);
        return HU_OK;
    }

    bool pass = false;
    switch (n->pred) {
    case HU_HULA_PRED_SUCCESS:
        pass = (target->status == HU_HULA_DONE);
        break;
    case HU_HULA_PRED_FAILURE:
        pass = (target->status == HU_HULA_FAILED);
        break;
    case HU_HULA_PRED_CONTAINS:
        pass = target->output && n->match_str &&
               strstr(target->output, n->match_str) != NULL;
        break;
    case HU_HULA_PRED_NOT_CONTAINS:
        pass = !target->output || !n->match_str ||
               strstr(target->output, n->match_str) == NULL;
        break;
    case HU_HULA_PRED_ALWAYS:
        pass = true;
        break;
    }

    if (pass) {
        set_result(exec, n, HU_HULA_DONE, "verified", 8, NULL, 0);
    } else {
        static const char msg[] = "verify: assertion failed";
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, msg, sizeof(msg) - 1);
        hu_hula_exec_cancel(exec, msg, sizeof(msg) - 1);
    }
    return HU_OK;
}

#if (defined(__unix__) || defined(__APPLE__)) && !defined(HU_IS_TEST)
static hu_error_t delegate_embed_children_json(hu_allocator_t *alloc, hu_hula_node_t *n,
                                                char **out_json, size_t *out_len) {
    if (!alloc || !n || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_json = NULL;
    *out_len = 0;
    if (n->children_count == 0)
        return HU_ERR_NOT_FOUND;

    hu_json_value_t *prog = hu_json_object_new(alloc);
    if (!prog)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, prog, "name",
                       hu_json_string_new(alloc, "delegate_embed", sizeof("delegate_embed") - 1));
    hu_json_object_set(alloc, prog, "version", hu_json_number_new(alloc, 1));

    hu_json_value_t *seq = hu_json_object_new(alloc);
    if (!seq) {
        hu_json_free(alloc, prog);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_object_set(alloc, seq, "op", hu_json_string_new(alloc, "seq", 3));
    hu_json_object_set(alloc, seq, "id", hu_json_string_new(alloc, "inner", 5));
    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (!arr) {
        hu_json_free(alloc, seq);
        hu_json_free(alloc, prog);
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < n->children_count; i++) {
        hu_hula_node_t *c = n->children[i];
        if (!c || c->op != HU_HULA_CALL)
            continue;
        hu_json_value_t *cn = hu_json_object_new(alloc);
        if (!cn)
            continue;
        hu_json_object_set(alloc, cn, "op", hu_json_string_new(alloc, "call", 4));
        if (c->id)
            hu_json_object_set(alloc, cn, "id",
                               hu_json_string_new(alloc, c->id, strlen(c->id)));
        if (c->tool_name)
            hu_json_object_set(
                alloc, cn, "tool", hu_json_string_new(alloc, c->tool_name, strlen(c->tool_name)));
        hu_json_value_t *args = NULL;
        const char *as = c->args_json ? c->args_json : "{}";
        if (hu_json_parse(alloc, as, strlen(as), &args) == HU_OK && args)
            hu_json_object_set(alloc, cn, "args", args);
        else
            hu_json_object_set(alloc, cn, "args", hu_json_object_new(alloc));
        hu_json_array_push(alloc, arr, cn);
    }
    hu_json_object_set(alloc, seq, "children", arr);
    hu_json_object_set(alloc, prog, "root", seq);

    hu_error_t err = hu_json_stringify(alloc, prog, out_json, out_len);
    hu_json_free(alloc, prog);
    return err;
}
#endif

static hu_error_t exec_delegate(hu_hula_exec_t *exec, hu_hula_node_t *n) {
#if (defined(__unix__) || defined(__APPLE__)) && !defined(HU_IS_TEST)
    if (exec->pool && exec->spawn_cfg && n->goal && n->goal_len > 0) {
        hu_spawn_config_t cfg = *exec->spawn_cfg;
        char *ctx_sp = NULL;
        if (n->delegate_context && n->delegate_context_len > 0) {
            static const char dcx[] = "Delegate context:\n";
            size_t old_len = cfg.system_prompt_len;
            const char *old = cfg.system_prompt;
            size_t need = sizeof(dcx) - 1;
            if (need > SIZE_MAX - n->delegate_context_len - 3 - old_len)
                return HU_ERR_OUT_OF_MEMORY;
            need += n->delegate_context_len + 2 + old_len + 1;
            ctx_sp = exec->alloc.alloc(exec->alloc.ctx, need);
            if (ctx_sp) {
                size_t pos = 0;
                memcpy(ctx_sp + pos, dcx, sizeof(dcx) - 1);
                pos += sizeof(dcx) - 1;
                memcpy(ctx_sp + pos, n->delegate_context, n->delegate_context_len);
                pos += n->delegate_context_len;
                ctx_sp[pos++] = '\n';
                ctx_sp[pos++] = '\n';
                if (old && old_len > 0) {
                    memcpy(ctx_sp + pos, old, old_len);
                    pos += old_len;
                }
                ctx_sp[pos] = '\0';
                cfg.system_prompt = ctx_sp;
                cfg.system_prompt_len = pos;
            }
        }
        char *embed = NULL;
        size_t embed_len = 0;
        char *combined_sp = NULL;
        if (n->children_count > 0 &&
            delegate_embed_children_json(&exec->alloc, n, &embed, &embed_len) == HU_OK && embed) {
            static const char pre[] =
                "Execute this HuLa JSON program (nested plan):\n";
            size_t old_len = cfg.system_prompt_len;
            const char *old = cfg.system_prompt;
            size_t tail = (old ? old_len : 0) + 3;
            if (sizeof(pre) - 1 > SIZE_MAX - embed_len - tail) {
                exec->alloc.free(exec->alloc.ctx, embed, embed_len + 1);
                if (ctx_sp)
                    exec->alloc.free(exec->alloc.ctx, ctx_sp, strlen(ctx_sp) + 1);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t need = sizeof(pre) - 1 + embed_len + tail;
            combined_sp = exec->alloc.alloc(exec->alloc.ctx, need);
            if (combined_sp) {
                size_t pos = 0;
                memcpy(combined_sp + pos, pre, sizeof(pre) - 1);
                pos += sizeof(pre) - 1;
                memcpy(combined_sp + pos, embed, embed_len);
                pos += embed_len;
                combined_sp[pos++] = '\n';
                combined_sp[pos++] = '\n';
                if (old && old_len > 0) {
                    memcpy(combined_sp + pos, old, old_len);
                    pos += old_len;
                }
                combined_sp[pos] = '\0';
                cfg.system_prompt = combined_sp;
                cfg.system_prompt_len = pos;
            }
            exec->alloc.free(exec->alloc.ctx, embed, embed_len + 1);
            if (ctx_sp && combined_sp && cfg.system_prompt == combined_sp) {
                exec->alloc.free(exec->alloc.ctx, ctx_sp, strlen(ctx_sp) + 1);
                ctx_sp = NULL;
            }
        }
        if (n->delegate_model && n->delegate_model_len > 0) {
            cfg.model = n->delegate_model;
            cfg.model_len = n->delegate_model_len;
        }
        uint64_t agent_id = 0;
        hu_error_t err;
        if (n->delegate_agent_id && n->delegate_agent_id_len > 0 && exec->delegate_registry)
            err = hu_agent_pool_spawn_named(exec->pool, exec->delegate_registry,
                                            n->delegate_agent_id, n->goal, n->goal_len, &agent_id);
        else
            err = hu_agent_pool_spawn(exec->pool, &cfg, n->goal, n->goal_len,
                                      n->id ? n->id : "hula_delegate", &agent_id);
        if (combined_sp)
            exec->alloc.free(exec->alloc.ctx, combined_sp, strlen(combined_sp) + 1);
        combined_sp = NULL;
        if (ctx_sp) {
            exec->alloc.free(exec->alloc.ctx, ctx_sp, strlen(ctx_sp) + 1);
            ctx_sp = NULL;
        }
        if (err != HU_OK) {
            set_result(exec, n, HU_HULA_FAILED, NULL, 0, "delegate spawn failed", 21);
            return HU_OK;
        }
        for (int poll = 0; poll < 300; poll++) {
            hu_agent_status_t st = hu_agent_pool_status(exec->pool, agent_id);
            if (st == HU_AGENT_COMPLETED || st == HU_AGENT_FAILED) {
                const char *result = hu_agent_pool_result(exec->pool, agent_id);
                if (st == HU_AGENT_COMPLETED && result) {
                    set_result(exec, n, HU_HULA_DONE, result, strlen(result), NULL, 0);
                    if (n->delegate_result_key && n->delegate_result_key_len > 0)
                        (void)hula_slot_set(exec, n->delegate_result_key, n->delegate_result_key_len,
                                            result, strlen(result));
                } else {
                    set_result(exec, n, HU_HULA_FAILED, NULL, 0,
                               result ? result : "delegate failed", result ? strlen(result) : 15);
                }
                return HU_OK;
            }
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
            nanosleep(&ts, NULL);
        }
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "delegate timeout", 16);
        hu_agent_pool_cancel(exec->pool, agent_id);
        return HU_OK;
    }
#endif
#if defined(_WIN32) && !defined(HU_IS_TEST)
    if (exec->pool && exec->spawn_cfg && n->goal && n->goal_len > 0) {
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "delegate spawn not supported on this platform",
                   strlen("delegate spawn not supported on this platform"));
        return HU_OK;
    }
#endif
    set_result(exec, n, HU_HULA_DONE, n->goal, n->goal_len, NULL, 0);
    return HU_OK;
}

static hu_error_t exec_emit(hu_hula_exec_t *exec, hu_hula_node_t *n) {
    /* Resolve $node_id references in emit_value */
    const char *val = n->emit_value ? n->emit_value : "";
    size_t val_len = n->emit_value_len;

    if (n->emit_value && strstr(n->emit_value, "$") != NULL) {
        /* Simple substitution: find $id references */
        for (size_t i = 0; i < exec->program->node_count; i++) {
            hu_hula_node_t *ref = &exec->program->nodes[i];
            if (!ref->id) continue;
            char var[128];
            int vlen = snprintf(var, sizeof(var), "$%s", ref->id);
            if (vlen <= 0) continue;
            if (strstr(n->emit_value, var)) {
                hu_hula_result_t *rr = result_for(exec, ref);
                if (rr && rr->output) { val = rr->output; val_len = rr->output_len; }
                break;
            }
        }
    }

    set_result(exec, n, HU_HULA_DONE, val, val_len, NULL, 0);
    if (n->emit_key && n->emit_key_len > 0)
        (void)hula_slot_set(exec, n->emit_key, n->emit_key_len, val, val_len);
    return HU_OK;
}

static void hula_emit_stream_output(hu_hula_exec_t *exec, const hu_hula_node_t *n) {
    if (!exec->observer || !n)
        return;
    const hu_hula_result_t *rr = result_for(exec, (hu_hula_node_t *)n);
    if (!rr || rr->status != HU_HULA_DONE || !rr->output || rr->output_len == 0)
        return;
    hu_observer_event_t ev = {0};
    ev.tag = HU_OBSERVER_EVENT_HULA_NODE_OUTPUT;
    ev.data.hula_node_output.node_id = n->id ? n->id : "";
    ev.data.hula_node_output.output = rr->output;
    ev.data.hula_node_output.output_len = rr->output_len;
    hu_observer_record_event(*exec->observer, &ev);
}

static hu_error_t exec_dispatch(hu_hula_exec_t *exec, hu_hula_node_t *n, int depth) {
    hu_error_t err;
    switch (n->op) {
    case HU_HULA_CALL:
        err = exec_call(exec, n);
        break;
    case HU_HULA_SEQ:
        err = exec_seq(exec, n, depth);
        break;
    case HU_HULA_PAR:
        err = exec_par(exec, n, depth);
        break;
    case HU_HULA_BRANCH:
        err = exec_branch(exec, n, depth);
        break;
    case HU_HULA_LOOP:
        err = exec_loop(exec, n, depth);
        break;
    case HU_HULA_DELEGATE:
        err = exec_delegate(exec, n);
        break;
    case HU_HULA_EMIT:
        err = exec_emit(exec, n);
        break;
    case HU_HULA_TRY:
        err = exec_try(exec, n, depth);
        break;
    case HU_HULA_VERIFY:
        err = exec_verify(exec, n);
        break;
    default:
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "unknown opcode", 14);
        err = HU_OK;
        break;
    }
    return err;
}

static hu_error_t exec_node_depth(hu_hula_exec_t *exec, hu_hula_node_t *n, int depth) {
    if (!n)
        return HU_OK;

    hu_hula_result_t *r = result_for(exec, n);
    if (exec->halted) {
        if (r) {
            const char *rs = exec->halt_reason ? exec->halt_reason : "cancelled";
            size_t rl = exec->halt_reason_len ? exec->halt_reason_len : strlen(rs);
            set_result(exec, n, HU_HULA_CANCELLED, NULL, 0, rs, rl);
            trace_append(exec, n);
        }
        return HU_OK;
    }

    if (n->required_capability && n->required_capability_len > 0) {
        if (!hu_policy_hula_capability_allowed(exec->policy, n->required_capability,
                                               n->required_capability_len)) {
            set_result(exec, n, HU_HULA_FAILED, NULL, 0, "hula capability denied",
                       strlen("hula capability denied"));
            trace_append(exec, n);
            return HU_OK;
        }
    }

    if (exec->budget_enabled && exec->budget_max_depth > 0 &&
        (uint32_t)depth > exec->budget_max_depth) {
        set_result(exec, n, HU_HULA_FAILED, NULL, 0, "hula budget: max depth exceeded",
                   strlen("hula budget: max depth exceeded"));
        trace_append(exec, n);
        return HU_OK;
    }
    if (exec->budget_enabled && exec->budget_max_wall_ms > 0 && exec->budget_run_start_ms > 0) {
        uint64_t now = hula_wall_ms();
        if (now >= exec->budget_run_start_ms &&
            now - exec->budget_run_start_ms > (uint64_t)exec->budget_max_wall_ms) {
            set_result(exec, n, HU_HULA_FAILED, NULL, 0, "hula budget: wall time exceeded",
                       strlen("hula budget: wall time exceeded"));
            trace_append(exec, n);
            return HU_OK;
        }
    }

    if (r)
        r->status = HU_HULA_RUNNING;

    uint64_t t0 = hula_wall_ms();

    if (exec->observer) {
        hu_observer_event_t ev = {0};
        ev.tag = HU_OBSERVER_EVENT_HULA_NODE_START;
        ev.data.hula_node_start.node_id = n->id ? n->id : "";
        ev.data.hula_node_start.op_name = hu_hula_op_name(n->op);
        hu_observer_record_event(*exec->observer, &ev);
    }

    if (exec->halted) {
        if (r) {
            const char *rs = exec->halt_reason ? exec->halt_reason : "cancelled";
            size_t rl = exec->halt_reason_len ? exec->halt_reason_len : strlen(rs);
            set_result(exec, n, HU_HULA_CANCELLED, NULL, 0, rs, rl);
        }
        uint64_t elapsed_ms = 0;
        {
            uint64_t t1 = hula_wall_ms();
            if (t1 >= t0)
                elapsed_ms = t1 - t0;
        }
        if (exec->observer) {
            hu_observer_event_t ev2 = {0};
            ev2.tag = HU_OBSERVER_EVENT_HULA_NODE_END;
            ev2.data.hula_node_end.node_id = n->id ? n->id : "";
            ev2.data.hula_node_end.op_name = hu_hula_op_name(n->op);
            r = result_for(exec, n);
            ev2.data.hula_node_end.status =
                r ? hu_hula_status_name(r->status) : hu_hula_status_name(HU_HULA_PENDING);
            ev2.data.hula_node_end.elapsed_ms = elapsed_ms;
            hu_observer_record_event(*exec->observer, &ev2);
        }
        trace_append(exec, n);
        return HU_OK;
    }
    uint32_t max_attempts = 1u + n->retry_count;
    if (max_attempts > 64u)
        max_attempts = 64u;
    hu_error_t err = HU_OK;

    for (uint32_t att = 0; att < max_attempts; att++) {
        if (att > 0) {
            hula_result_reset(exec, n);
#if (defined(__unix__) || defined(__APPLE__)) && !defined(HU_IS_TEST)
            if (n->retry_backoff_ms > 0) {
                struct timespec ts = {.tv_sec = n->retry_backoff_ms / 1000u,
                                      .tv_nsec = (long)(n->retry_backoff_ms % 1000u) * 1000000L};
                (void)nanosleep(&ts, NULL);
            }
#endif
        }

        err = exec_dispatch(exec, n, depth);
        if (err != HU_OK)
            return err;

        if (n->timeout_ms > 0) {
            uint64_t elapsed = hula_wall_ms() - t0;
            if (elapsed > (uint64_t)n->timeout_ms) {
                set_result(exec, n, HU_HULA_FAILED, NULL, 0, "timeout exceeded", 16);
                break;
            }
        }

        r = result_for(exec, n);
        if (!r || r->status != HU_HULA_FAILED || att + 1 >= max_attempts)
            break;
    }

    uint64_t elapsed_ms = 0;
    {
        uint64_t t1 = hula_wall_ms();
        if (t1 >= t0)
            elapsed_ms = t1 - t0;
    }

    if (exec->observer) {
        hu_observer_event_t ev2 = {0};
        ev2.tag = HU_OBSERVER_EVENT_HULA_NODE_END;
        ev2.data.hula_node_end.node_id = n->id ? n->id : "";
        ev2.data.hula_node_end.op_name = hu_hula_op_name(n->op);
        r = result_for(exec, n);
        ev2.data.hula_node_end.status =
            r ? hu_hula_status_name(r->status) : hu_hula_status_name(HU_HULA_PENDING);
        ev2.data.hula_node_end.elapsed_ms = elapsed_ms;
        hu_observer_record_event(*exec->observer, &ev2);
    }

    if (err == HU_OK &&
        (n->op == HU_HULA_CALL || n->op == HU_HULA_EMIT || n->op == HU_HULA_DELEGATE))
        hula_emit_stream_output(exec, n);

    if (err == HU_OK)
        trace_append(exec, n);
    return err;
}

hu_error_t hu_hula_exec_run(hu_hula_exec_t *exec) {
    if (!exec || !exec->program || !exec->program->root)
        return HU_ERR_INVALID_ARGUMENT;
    if (exec->budget_enabled)
        exec->budget_run_start_ms = hula_wall_ms();
    exec->budget_tool_calls_used = 0;
    uint64_t t_run0 = hula_wall_ms();
    hu_error_t e = exec_node_depth(exec, exec->program->root, 0);
    if (exec->observer) {
        hu_observer_event_t ev = {0};
        ev.tag = HU_OBSERVER_EVENT_HULA_PROGRAM_END;
        const hu_hula_result_t *rr =
            exec->program->root && exec->program->root->id
                ? hu_hula_exec_result(exec, exec->program->root->id)
                : NULL;
        bool ok = rr && rr->status == HU_HULA_DONE;
        ev.data.hula_program_end.program_name =
            exec->program->name ? exec->program->name : "";
        ev.data.hula_program_end.success = ok;
        uint64_t t1 = hula_wall_ms();
        ev.data.hula_program_end.total_ms = (t1 >= t_run0) ? (t1 - t_run0) : 0;
        ev.data.hula_program_end.node_count = exec->program->node_count;
        hu_observer_record_event(*exec->observer, &ev);
    }
    return e;
}

const hu_hula_result_t *hu_hula_exec_result(const hu_hula_exec_t *exec, const char *node_id) {
    if (!exec || !node_id) return NULL;
    for (size_t i = 0; i < exec->program->node_count; i++) {
        if (exec->program->nodes[i].id && strcmp(exec->program->nodes[i].id, node_id) == 0)
            return &exec->results[i];
    }
    return NULL;
}

const char *hu_hula_exec_trace(const hu_hula_exec_t *exec, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!exec || !exec->trace_log) return NULL;

    hu_hula_exec_t *e = (hu_hula_exec_t *)exec;
    char *log = e->trace_log;
    size_t len = e->trace_log_len;

    if (len == 1 && log[0] == '[') {
        if (e->trace_log_cap < 3) return NULL;
        log[1] = ']';
        log[2] = '\0';
        e->trace_log_len = 2;
        len = 2;
    } else if (len > 1 && log[len - 1] == ',') {
        log[len - 1] = ']';
    }

    if (out_len) *out_len = e->trace_log_len;
    return e->trace_log;
}

static size_t subtree_estimated_calls(const hu_hula_node_t *n) {
    if (!n)
        return 0;
    switch (n->op) {
    case HU_HULA_CALL:
        return 1;
    case HU_HULA_SEQ:
    case HU_HULA_PAR: {
        size_t s = 0;
        for (size_t i = 0; i < n->children_count; i++)
            s += subtree_estimated_calls(n->children[i]);
        return s;
    }
    case HU_HULA_BRANCH: {
        size_t a = n->children_count > 0 ? subtree_estimated_calls(n->children[0]) : 0;
        size_t b = n->children_count > 1 ? subtree_estimated_calls(n->children[1]) : 0;
        return a + b;
    }
    case HU_HULA_LOOP: {
        uint32_t m = n->max_iter > 0 ? n->max_iter : 10;
        size_t body = 0;
        for (size_t i = 0; i < n->children_count; i++)
            body += subtree_estimated_calls(n->children[i]);
        return body * (size_t)m;
    }
    case HU_HULA_DELEGATE: {
        size_t s = 1;
        for (size_t i = 0; i < n->children_count; i++)
            s += subtree_estimated_calls(n->children[i]);
        return s;
    }
    case HU_HULA_EMIT:
        return 0;
    case HU_HULA_TRY: {
        size_t s = 0;
        for (size_t i = 0; i < n->children_count; i++)
            s += subtree_estimated_calls(n->children[i]);
        return s;
    }
    default:
        return 0;
    }
}

static void cost_visit(const hu_hula_node_t *n, size_t *calls, size_t *par_max, uint32_t *loop_sum,
                       hu_command_risk_level_t *max_risk) {
    if (!n)
        return;
    switch (n->op) {
    case HU_HULA_CALL:
        (*calls)++;
        if (n->tool_name) {
            hu_command_risk_level_t r = hu_tool_risk_level(n->tool_name);
            if (r > *max_risk)
                *max_risk = r;
        }
        break;
    case HU_HULA_SEQ:
        for (size_t i = 0; i < n->children_count; i++)
            cost_visit(n->children[i], calls, par_max, loop_sum, max_risk);
        break;
    case HU_HULA_PAR:
        if (n->children_count > *par_max)
            *par_max = n->children_count;
        for (size_t i = 0; i < n->children_count; i++)
            cost_visit(n->children[i], calls, par_max, loop_sum, max_risk);
        break;
    case HU_HULA_BRANCH:
        if (n->children_count > 0)
            cost_visit(n->children[0], calls, par_max, loop_sum, max_risk);
        if (n->children_count > 1)
            cost_visit(n->children[1], calls, par_max, loop_sum, max_risk);
        break;
    case HU_HULA_LOOP: {
        uint32_t m = n->max_iter > 0 ? n->max_iter : 10;
        *loop_sum += m;
        size_t body = 0;
        for (size_t i = 0; i < n->children_count; i++)
            body += subtree_estimated_calls(n->children[i]);
        *calls += body * (size_t)m;
        break;
    }
    case HU_HULA_DELEGATE:
        (*calls)++;
        /* Children describe the child agent's program — not executed in this process. */
        break;
    case HU_HULA_EMIT:
        break;
    case HU_HULA_TRY:
        for (size_t i = 0; i < n->children_count; i++)
            cost_visit(n->children[i], calls, par_max, loop_sum, max_risk);
        break;
    case HU_HULA_VERIFY:
        break;
    default:
        break;
    }
}

void hu_hula_estimate_cost(const hu_hula_program_t *prog, hu_hula_cost_estimate_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->max_tool_risk = HU_RISK_LOW;
    if (!prog || !prog->root)
        return;
    cost_visit(prog->root, &out->estimated_tool_calls, &out->max_parallel_width,
               &out->max_loop_iterations_bound, &out->max_tool_risk);
}

void hu_hula_exec_deinit(hu_hula_exec_t *exec) {
    if (!exec) return;
    for (size_t i = 0; i < exec->results_count; i++) {
        hu_hula_result_t *r = &exec->results[i];
        if (r->output) exec->alloc.free(exec->alloc.ctx, r->output, r->output_len + 1);
        if (r->error) exec->alloc.free(exec->alloc.ctx, r->error, r->error_len + 1);
    }
    if (exec->results)
        exec->alloc.free(exec->alloc.ctx, exec->results,
                         exec->results_count * sizeof(hu_hula_result_t));
    if (exec->trace_log)
        exec->alloc.free(exec->alloc.ctx, exec->trace_log, exec->trace_log_cap);
    if (exec->halt_reason)
        exec->alloc.free(exec->alloc.ctx, exec->halt_reason, exec->halt_reason_len + 1);
    for (size_t si = 0; si < exec->slot_count; si++) {
        if (exec->slots[si].key)
            exec->alloc.free(exec->alloc.ctx, exec->slots[si].key, exec->slots[si].key_len + 1);
        if (exec->slots[si].value)
            exec->alloc.free(exec->alloc.ctx, exec->slots[si].value, exec->slots[si].value_len + 1);
    }
    memset(exec, 0, sizeof(*exec));
}

/* ── Bridges (plan / DAG → HuLa) ───────────────────────────────────────── */

static hu_hula_node_t *hula_bridge_alloc_call(hu_hula_program_t *prog, const char *id,
                                              const char *tool_name, const char *args_json,
                                              const char *description) {
    hu_hula_node_t *n = hu_hula_program_alloc_node(prog, HU_HULA_CALL, id);
    if (!n) return NULL;
    if (tool_name) {
        n->tool_name = hu_strdup(&prog->alloc, tool_name);
        if (!n->tool_name) return NULL;
    }
    if (args_json) {
        n->args_json = hu_strdup(&prog->alloc, args_json);
        if (!n->args_json) return NULL;
    }
    if (description) {
        n->description = hu_strdup(&prog->alloc, description);
        if (!n->description) return NULL;
    }
    return n;
}

static bool plan_step_ready(const hu_plan_t *plan, size_t idx, const unsigned char *placed) {
    const hu_plan_step_t *st = &plan->steps[idx];
    for (size_t d = 0; d < st->depends_count; d++) {
        int di = st->depends_on[d];
        if (di < 0 || (size_t)di >= plan->steps_count) return false;
        if (!placed[(size_t)di]) return false;
    }
    return true;
}

static int dag_index_by_id(const hu_dag_t *dag, const char *id) {
    if (!id) return -1;
    for (size_t i = 0; i < dag->node_count; i++) {
        if (dag->nodes[i].id && strcmp(dag->nodes[i].id, id) == 0) return (int)i;
    }
    return -1;
}

static bool dag_node_ready(const hu_dag_t *dag, size_t idx, const unsigned char *placed) {
    const hu_dag_node_t *n = &dag->nodes[idx];
    for (size_t d = 0; d < n->dep_count; d++) {
        int di = dag_index_by_id(dag, n->deps[d]);
        if (di < 0) return false;
        if (!placed[(size_t)di]) return false;
    }
    return true;
}

hu_error_t hu_hula_from_plan(hu_allocator_t *alloc, const hu_plan_t *plan, const char *name,
                             size_t name_len, hu_hula_program_t *out) {
    if (!alloc || !plan || !out) return HU_ERR_INVALID_ARGUMENT;
    if (plan->steps_count > 0 && !plan->steps) return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_hula_program_init(out, *alloc, name, name_len);
    if (err != HU_OK) return err;

    if (plan->steps_count == 0) {
        out->root = NULL;
        return HU_OK;
    }

    bool any_dep = false;
    for (size_t i = 0; i < plan->steps_count; i++) {
        if (plan->steps[i].depends_count > 0) {
            any_dep = true;
            break;
        }
    }

    /* No dependencies: flat seq of calls in step order (not batched as par). */
    if (!any_dep) {
        if (plan->steps_count > HU_HULA_MAX_CHILDREN) {
            hu_hula_program_deinit(out);
            memset(out, 0, sizeof(*out));
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_hula_node_t *flat = hu_hula_program_alloc_node(out, HU_HULA_SEQ, "plan");
        if (!flat) {
            hu_hula_program_deinit(out);
            memset(out, 0, sizeof(*out));
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < plan->steps_count; i++) {
            char sid[32];
            (void)snprintf(sid, sizeof(sid), "s%zu", i);
            const hu_plan_step_t *st = &plan->steps[i];
            hu_hula_node_t *call = hula_bridge_alloc_call(out, sid, st->tool_name, st->args_json,
                                                           st->description);
            if (!call) {
                hu_hula_program_deinit(out);
                memset(out, 0, sizeof(*out));
                return HU_ERR_OUT_OF_MEMORY;
            }
            flat->children[flat->children_count++] = call;
        }
        out->root = flat;
        return HU_OK;
    }

    unsigned char *placed = alloc->alloc(alloc->ctx, plan->steps_count);
    if (!placed) {
        hu_hula_program_deinit(out);
        memset(out, 0, sizeof(*out));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(placed, 0, plan->steps_count);

    size_t *wave_idx = alloc->alloc(alloc->ctx, plan->steps_count * sizeof(size_t));
    if (!wave_idx) {
        alloc->free(alloc->ctx, placed, plan->steps_count);
        hu_hula_program_deinit(out);
        memset(out, 0, sizeof(*out));
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_hula_node_t *outer = hu_hula_program_alloc_node(out, HU_HULA_SEQ, "plan");
    if (!outer) {
        err = HU_ERR_OUT_OF_MEMORY;
        goto fail;
    }

    size_t placed_count = 0;
    size_t wave_num = 0;
    while (placed_count < plan->steps_count) {
        size_t wn = 0;
        for (size_t i = 0; i < plan->steps_count; i++) {
            if (placed[i]) continue;
            if (!plan_step_ready(plan, i, placed)) continue;
            wave_idx[wn++] = i;
        }
        if (wn == 0) {
            err = HU_ERR_INVALID_ARGUMENT;
            goto fail;
        }
        if (wn > HU_HULA_MAX_CHILDREN) {
            err = HU_ERR_INVALID_ARGUMENT;
            goto fail;
        }

        if (wn == 1) {
            size_t si = wave_idx[0];
            char sid[32];
            (void)snprintf(sid, sizeof(sid), "s%zu", si);
            const hu_plan_step_t *st = &plan->steps[si];
            hu_hula_node_t *call = hula_bridge_alloc_call(out, sid, st->tool_name, st->args_json,
                                                           st->description);
            if (!call) {
                err = HU_ERR_OUT_OF_MEMORY;
                goto fail;
            }
            if (outer->children_count >= HU_HULA_MAX_CHILDREN) {
                err = HU_ERR_INVALID_ARGUMENT;
                goto fail;
            }
            outer->children[outer->children_count++] = call;
            placed[si] = 1;
        } else {
            char wid[40];
            (void)snprintf(wid, sizeof(wid), "w%zu", wave_num);
            hu_hula_node_t *par = hu_hula_program_alloc_node(out, HU_HULA_PAR, wid);
            if (!par) {
                err = HU_ERR_OUT_OF_MEMORY;
                goto fail;
            }
            for (size_t k = 0; k < wn; k++) {
                size_t si = wave_idx[k];
                char sid[32];
                (void)snprintf(sid, sizeof(sid), "s%zu", si);
                const hu_plan_step_t *st = &plan->steps[si];
                hu_hula_node_t *call = hula_bridge_alloc_call(out, sid, st->tool_name, st->args_json,
                                                               st->description);
                if (!call) {
                    err = HU_ERR_OUT_OF_MEMORY;
                    goto fail;
                }
                if (par->children_count >= HU_HULA_MAX_CHILDREN) {
                    err = HU_ERR_INVALID_ARGUMENT;
                    goto fail;
                }
                par->children[par->children_count++] = call;
            }
            if (outer->children_count >= HU_HULA_MAX_CHILDREN) {
                err = HU_ERR_INVALID_ARGUMENT;
                goto fail;
            }
            outer->children[outer->children_count++] = par;
            for (size_t k = 0; k < wn; k++) placed[wave_idx[k]] = 1;
        }

        placed_count += wn;
        wave_num++;
    }

    out->root = outer;
    alloc->free(alloc->ctx, wave_idx, plan->steps_count * sizeof(size_t));
    alloc->free(alloc->ctx, placed, plan->steps_count);
    return HU_OK;

fail:
    alloc->free(alloc->ctx, wave_idx, plan->steps_count * sizeof(size_t));
    alloc->free(alloc->ctx, placed, plan->steps_count);
    hu_hula_program_deinit(out);
    memset(out, 0, sizeof(*out));
    return err;
}

hu_error_t hu_hula_from_dag(hu_allocator_t *alloc, const hu_dag_t *dag, const char *name,
                            size_t name_len, hu_hula_program_t *out) {
    if (!alloc || !dag || !out) return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_hula_program_init(out, *alloc, name, name_len);
    if (err != HU_OK) return err;

    if (dag->node_count == 0) {
        out->root = NULL;
        return HU_OK;
    }

    unsigned char *placed = alloc->alloc(alloc->ctx, dag->node_count);
    if (!placed) {
        hu_hula_program_deinit(out);
        memset(out, 0, sizeof(*out));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(placed, 0, dag->node_count);

    size_t *wave_idx = alloc->alloc(alloc->ctx, dag->node_count * sizeof(size_t));
    if (!wave_idx) {
        alloc->free(alloc->ctx, placed, dag->node_count);
        hu_hula_program_deinit(out);
        memset(out, 0, sizeof(*out));
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_hula_node_t *outer = hu_hula_program_alloc_node(out, HU_HULA_SEQ, "dag");
    if (!outer) {
        err = HU_ERR_OUT_OF_MEMORY;
        goto fail_dag;
    }

    size_t placed_count = 0;
    size_t wave_num = 0;
    while (placed_count < dag->node_count) {
        size_t wn = 0;
        for (size_t i = 0; i < dag->node_count; i++) {
            if (placed[i]) continue;
            if (!dag_node_ready(dag, i, placed)) continue;
            wave_idx[wn++] = i;
        }
        if (wn == 0) {
            err = HU_ERR_INVALID_ARGUMENT;
            goto fail_dag;
        }
        if (wn > HU_HULA_MAX_CHILDREN) {
            err = HU_ERR_INVALID_ARGUMENT;
            goto fail_dag;
        }

        if (wn == 1) {
            size_t ni = wave_idx[0];
            const hu_dag_node_t *dn = &dag->nodes[ni];
            char nid[40];
            if (dn->id)
                (void)snprintf(nid, sizeof(nid), "%s", dn->id);
            else
                (void)snprintf(nid, sizeof(nid), "n%zu", ni);
            hu_hula_node_t *call = hula_bridge_alloc_call(out, nid, dn->tool_name, dn->args_json, NULL);
            if (!call) {
                err = HU_ERR_OUT_OF_MEMORY;
                goto fail_dag;
            }
            if (outer->children_count >= HU_HULA_MAX_CHILDREN) {
                err = HU_ERR_INVALID_ARGUMENT;
                goto fail_dag;
            }
            outer->children[outer->children_count++] = call;
            placed[ni] = 1;
        } else {
            char wid[40];
            (void)snprintf(wid, sizeof(wid), "w%zu", wave_num);
            hu_hula_node_t *par = hu_hula_program_alloc_node(out, HU_HULA_PAR, wid);
            if (!par) {
                err = HU_ERR_OUT_OF_MEMORY;
                goto fail_dag;
            }
            for (size_t k = 0; k < wn; k++) {
                size_t ni = wave_idx[k];
                const hu_dag_node_t *dn = &dag->nodes[ni];
                char nid[40];
                if (dn->id)
                    (void)snprintf(nid, sizeof(nid), "%s", dn->id);
                else
                    (void)snprintf(nid, sizeof(nid), "n%zu", ni);
                hu_hula_node_t *call = hula_bridge_alloc_call(out, nid, dn->tool_name, dn->args_json, NULL);
                if (!call) {
                    err = HU_ERR_OUT_OF_MEMORY;
                    goto fail_dag;
                }
                if (par->children_count >= HU_HULA_MAX_CHILDREN) {
                    err = HU_ERR_INVALID_ARGUMENT;
                    goto fail_dag;
                }
                par->children[par->children_count++] = call;
            }
            if (outer->children_count >= HU_HULA_MAX_CHILDREN) {
                err = HU_ERR_INVALID_ARGUMENT;
                goto fail_dag;
            }
            outer->children[outer->children_count++] = par;
            for (size_t k = 0; k < wn; k++) placed[wave_idx[k]] = 1;
        }

        placed_count += wn;
        wave_num++;
    }

    out->root = outer;
    alloc->free(alloc->ctx, wave_idx, dag->node_count * sizeof(size_t));
    alloc->free(alloc->ctx, placed, dag->node_count);
    return HU_OK;

fail_dag:
    alloc->free(alloc->ctx, wave_idx, dag->node_count * sizeof(size_t));
    alloc->free(alloc->ctx, placed, dag->node_count);
    hu_hula_program_deinit(out);
    memset(out, 0, sizeof(*out));
    return err;
}
