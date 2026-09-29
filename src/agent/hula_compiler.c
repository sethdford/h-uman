#include "human/agent/hula_compiler.h"
#include "human/agent/hula_emergence.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/util/llm_json.h"
#include <stdio.h>
#include <string.h>

static const char HULA_COMPILER_SCHEMA[] =
    "You are a planner. Output a single JSON object (no markdown) for a HuLa program.\n"
    "HuLa is a small tree of nodes. Top-level fields: \"name\" (string), \"version\" (number, "
    "use 1), \"root\" (node).\n"
    "Each node has: \"op\" (string), \"id\" (string), and op-specific fields.\n"
    "Ops: \"call\" — tool (string), args (object; strings may use $node_id or $slot_key refs). "
    "\"seq\" | \"par\" — children (array of nodes).\n"
    "\"branch\" — pred: \"success\"|\"failure\"|\"contains\"|\"not_contains\"|\"always\", "
    "optional match, then (node), else (node).\n"
    "\"loop\" — pred, max_iter (number), body (node).\n"
    "\"delegate\" — goal (string), optional model, context, result_key, agent_id.\n"
    "\"emit\" — emit_key, emit_value (string; may use $node_id for prior output).\n"
    "\"try\" — body (node), catch (node) for error handling.\n"
    "Optional per node: timeout_ms, retry_count, retry_backoff_ms, required_capability.\n"
    "Prefer \"seq\" for ordered steps, \"par\" for independent parallel work.\n\n";

static const char HULA_COMPILER_GOAL[] = "Goal:\n";
static const char HULA_COMPILER_TOOLS[] = "\n\nTools (use exact \"tool\" names in call nodes):\n";
static const char HULA_COMPILER_SUFFIX[] =
    "\n\nRespond with only valid JSON matching the schema above.";

/* Shared locator (string-aware, strips <think> blocks and fences). The private
 * brace-counter this replaced broke on a "}" inside a JSON string. */
static void extract_json_from_response(const char *s, size_t len, const char **out_ptr,
                                       size_t *out_len) {
    if (!hu_llm_json_locate(s, len, out_ptr, out_len)) {
        *out_ptr = s;
        *out_len = len;
    }
}

static const char *template_lookup(const hu_json_value_t *obj, const char *key, size_t key_len,
                                   size_t *val_len) {
    *val_len = 0;
    if (!obj || obj->type != HU_JSON_OBJECT || !key || key_len == 0)
        return NULL;
    for (size_t i = 0; i < obj->data.object.len; i++) {
        hu_json_pair_t *p = &obj->data.object.pairs[i];
        if (p->key_len == key_len && memcmp(p->key, key, key_len) == 0 && p->value &&
            p->value->type == HU_JSON_STRING) {
            *val_len = p->value->data.string.len;
            return p->value->data.string.ptr;
        }
    }
    return NULL;
}

hu_error_t hu_hula_expand_template(hu_allocator_t *alloc, const char *tmpl, size_t tmpl_len,
                                   const char *vars_json, size_t vars_json_len, char **out,
                                   size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!tmpl)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *vars = NULL;
    if (!vars_json || vars_json_len == 0 ||
        hu_json_parse(alloc, vars_json, vars_json_len, &vars) != HU_OK || !vars ||
        vars->type != HU_JSON_OBJECT) {
        if (vars)
            hu_json_free(alloc, vars);
        return HU_ERR_PARSE;
    }

    size_t cap = tmpl_len + 64;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        hu_json_free(alloc, vars);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t pos = 0;

    for (size_t i = 0; i < tmpl_len;) {
        if (i + 4 <= tmpl_len && tmpl[i] == '{' && tmpl[i + 1] == '{') {
            size_t j = i + 2;
            while (j + 1 < tmpl_len && !(tmpl[j] == '}' && tmpl[j + 1] == '}'))
                j++;
            if (j + 1 >= tmpl_len) {
                hu_json_free(alloc, vars);
                alloc->free(alloc->ctx, buf, cap);
                return HU_ERR_PARSE;
            }
            size_t key_len = j - (i + 2);
            const char *key = tmpl + i + 2;
            while (key_len > 0 && (key[0] == ' ' || key[0] == '\t')) {
                key++;
                key_len--;
            }
            while (key_len > 0 && (key[key_len - 1] == ' ' || key[key_len - 1] == '\t'))
                key_len--;

            size_t vlen = 0;
            const char *val = template_lookup(vars, key, key_len, &vlen);
            if (!val)
                val = "";
            while (pos + vlen + 1 > cap) {
                size_t nc = cap * 2u;
                char *nb = (char *)alloc->alloc(alloc->ctx, nc);
                if (!nb) {
                    hu_json_free(alloc, vars);
                    alloc->free(alloc->ctx, buf, cap);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                memcpy(nb, buf, pos);
                alloc->free(alloc->ctx, buf, cap);
                buf = nb;
                cap = nc;
            }
            memcpy(buf + pos, val, vlen);
            pos += vlen;
            i = j + 2;
            continue;
        }
        if (pos + 2 > cap) {
            size_t nc = cap * 2u;
            char *nb = (char *)alloc->alloc(alloc->ctx, nc);
            if (!nb) {
                hu_json_free(alloc, vars);
                alloc->free(alloc->ctx, buf, cap);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(nb, buf, pos);
            alloc->free(alloc->ctx, buf, cap);
            buf = nb;
            cap = nc;
        }
        buf[pos++] = tmpl[i++];
    }
    buf[pos] = '\0';
    hu_json_free(alloc, vars);
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

hu_error_t hu_hula_compiler_build_prompt(hu_allocator_t *alloc, const char *goal, size_t goal_len,
                                         hu_tool_t *tools, size_t tools_count, const char *domain,
                                         size_t domain_len, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    size_t ex_len = 0;
    const char *ex = (domain && domain_len > 0)
                         ? hu_hula_compiler_examples_for_domain(domain, domain_len, &ex_len)
                         : NULL;

    size_t cap = sizeof(HULA_COMPILER_SCHEMA) + sizeof(HULA_COMPILER_GOAL) + goal_len +
                 sizeof(HULA_COMPILER_TOOLS) + sizeof(HULA_COMPILER_SUFFIX) + 256 + ex_len + 32;
    for (size_t i = 0; i < tools_count && tools; i++) {
        if (!tools[i].vtable || !tools[i].vtable->name)
            continue;
        const char *nm = tools[i].vtable->name(tools[i].ctx);
        const char *ds =
            tools[i].vtable->description ? tools[i].vtable->description(tools[i].ctx) : "";
        cap += (nm ? strlen(nm) : 0) + (ds ? strlen(ds) : 0) + 8;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
#define APPEND(s, n)                    \
    do {                                \
        size_t nn = (n);                \
        if (pos + nn >= cap)            \
            nn = cap - pos - 1;         \
        if (nn > 0) {                   \
            memcpy(buf + pos, (s), nn); \
            pos += nn;                  \
        }                               \
    } while (0)

    APPEND(HULA_COMPILER_SCHEMA, sizeof(HULA_COMPILER_SCHEMA) - 1);
    APPEND(HULA_COMPILER_GOAL, sizeof(HULA_COMPILER_GOAL) - 1);
    if (goal && goal_len > 0) {
        size_t n = goal_len < cap - pos ? goal_len : cap - pos - 1;
        memcpy(buf + pos, goal, n);
        pos += n;
    }
    if (ex && ex_len > 0) {
        APPEND("\nDomain example:\n", sizeof("\nDomain example:\n") - 1);
        APPEND(ex, ex_len);
    }
    APPEND(HULA_COMPILER_TOOLS, sizeof(HULA_COMPILER_TOOLS) - 1);
    for (size_t i = 0; i < tools_count && tools; i++) {
        if (!tools[i].vtable || !tools[i].vtable->name)
            continue;
        const char *nm = tools[i].vtable->name(tools[i].ctx);
        const char *ds =
            tools[i].vtable->description ? tools[i].vtable->description(tools[i].ctx) : "";
        if (!nm)
            continue;
        int ln = snprintf(buf + pos, cap > pos ? cap - pos : 0, "- %s: %s\n", nm, ds ? ds : "");
        if (ln > 0 && (size_t)ln < cap - pos)
            pos += (size_t)ln;
    }
    APPEND(HULA_COMPILER_SUFFIX, sizeof(HULA_COMPILER_SUFFIX) - 1);
    buf[pos] = '\0';
#undef APPEND

    *out = buf;
    *out_len = pos;
    return HU_OK;
}

hu_error_t hu_hula_compiler_parse_response(hu_allocator_t *alloc, const char *response,
                                           size_t response_len, hu_hula_program_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!response || response_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *json = NULL;
    size_t json_len = 0;
    extract_json_from_response(response, response_len, &json, &json_len);

    return hu_hula_parse_json(alloc, json, json_len, out);
}

static const char HULA_TAG_OPEN[] = "<hula_program>";
static const size_t HULA_TAG_OPEN_LEN = 14;
static const char HULA_TAG_CLOSE[] = "</hula_program>";
static const size_t HULA_TAG_CLOSE_LEN = 15;

static const char *find_hula_tag(const char *haystack, size_t haystack_len, const char *needle,
                                 size_t needle_len) {
    if (needle_len > haystack_len)
        return NULL;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return haystack + i;
    }
    return NULL;
}

hu_error_t hu_hula_program_source_slice_from_text(const char *text, size_t text_len,
                                                  const char **out_start, size_t *out_len) {
    if (!text || !out_start || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_start = NULL;
    *out_len = 0;
    if (text_len == 0)
        return HU_ERR_NOT_FOUND;

    const char *open = find_hula_tag(text, text_len, HULA_TAG_OPEN, HULA_TAG_OPEN_LEN);
    if (!open)
        return HU_ERR_NOT_FOUND;

    const char *json_start = open + HULA_TAG_OPEN_LEN;
    size_t after_open = text_len - (size_t)(json_start - text);
    const char *close = find_hula_tag(json_start, after_open, HULA_TAG_CLOSE, HULA_TAG_CLOSE_LEN);
    if (!close)
        return HU_ERR_NOT_FOUND;

    size_t json_len = (size_t)(close - json_start);
    while (json_len > 0 && (json_start[0] == ' ' || json_start[0] == '\n' ||
                            json_start[0] == '\r' || json_start[0] == '\t')) {
        json_start++;
        json_len--;
    }
    while (json_len > 0 && (json_start[json_len - 1] == ' ' || json_start[json_len - 1] == '\n' ||
                            json_start[json_len - 1] == '\r' || json_start[json_len - 1] == '\t')) {
        json_len--;
    }
    if (json_len == 0)
        return HU_ERR_NOT_FOUND;

    *out_start = json_start;
    *out_len = json_len;
    return HU_OK;
}

hu_error_t hu_hula_extract_program_from_text(hu_allocator_t *alloc, const char *text,
                                             size_t text_len, hu_hula_program_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!text || text_len == 0)
        return HU_ERR_NOT_FOUND;

    const char *json_start = NULL;
    size_t json_len = 0;
    hu_error_t sl = hu_hula_program_source_slice_from_text(text, text_len, &json_start, &json_len);
    if (sl != HU_OK)
        return sl;

    return hu_hula_parse_json(alloc, json_start, json_len, out);
}

hu_error_t hu_hula_strip_program_tags(hu_allocator_t *alloc, const char *text, size_t text_len,
                                      char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!text || text_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *open = find_hula_tag(text, text_len, HULA_TAG_OPEN, HULA_TAG_OPEN_LEN);
    if (!open)
        return HU_ERR_NOT_FOUND;

    const char *json_start = open + HULA_TAG_OPEN_LEN;
    size_t after_open = text_len - (size_t)(json_start - text);
    const char *close = find_hula_tag(json_start, after_open, HULA_TAG_CLOSE, HULA_TAG_CLOSE_LEN);
    if (!close)
        return HU_ERR_NOT_FOUND;

    const char *after_close = close + HULA_TAG_CLOSE_LEN;
    size_t before_len = (size_t)(open - text);
    size_t tail_len = text_len - (size_t)(after_close - text);
    size_t total = before_len + tail_len;
    char *buf = alloc->alloc(alloc->ctx, total + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t pos = 0;
    if (before_len > 0) {
        memcpy(buf + pos, text, before_len);
        pos += before_len;
    }
    if (tail_len > 0) {
        memcpy(buf + pos, after_close, tail_len);
        pos += tail_len;
    }
    while (pos > 0 && (buf[pos - 1] == ' ' || buf[pos - 1] == '\n' || buf[pos - 1] == '\r'))
        pos--;
    size_t start = 0;
    while (start < pos && (buf[start] == ' ' || buf[start] == '\n' || buf[start] == '\r'))
        start++;
    if (start > 0 && start < pos)
        memmove(buf, buf + start, pos - start);
    pos -= start;
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

static char *append_repair_suffix(hu_allocator_t *alloc, const char *base, size_t base_len,
                                  const char *suffix, size_t suffix_len) {
    if (!alloc || !base)
        return NULL;
    size_t total = base_len + suffix_len + 1;
    char *n = (char *)alloc->alloc(alloc->ctx, total);
    if (!n)
        return NULL;
    memcpy(n, base, base_len);
    if (suffix_len > 0 && suffix)
        memcpy(n + base_len, suffix, suffix_len);
    n[base_len + suffix_len] = '\0';
    return n;
}

hu_error_t hu_hula_compiler_chat_compile_execute(
    hu_allocator_t *alloc, const char *goal, size_t goal_len, hu_tool_t *tools, size_t tools_count,
    hu_security_policy_t *policy, hu_observer_t *observer, hu_agent_pool_t *pool,
    hu_spawn_config_t *spawn_tpl, hu_hula_compiler_chat_fn chat_fn, void *chat_ctx,
    const char *model_name, size_t model_name_len, double temperature, const char *domain,
    size_t domain_len, hu_hula_compiler_done_fn done_fn, void *done_ctx, bool *out_ok) {
    if (!alloc || !out_ok || !chat_fn)
        return HU_ERR_INVALID_ARGUMENT;
    *out_ok = false;

    char *hprompt = NULL;
    size_t hprompt_len = 0;
    hu_error_t err = hu_hula_compiler_build_prompt(alloc, goal, goal_len, tools, tools_count,
                                                   domain, domain_len, &hprompt, &hprompt_len);
    if (err != HU_OK)
        return err;
    if (!hprompt)
        return HU_ERR_OUT_OF_MEMORY;

    hu_hula_program_t hcp;
    memset(&hcp, 0, sizeof(hcp));
    const char **tnames = NULL;
    size_t tnc = 0;
    if (tools_count > 0) {
        tnames = (const char **)alloc->alloc(alloc->ctx, tools_count * sizeof(const char *));
        if (!tnames) {
            hu_str_free(alloc, hprompt);
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t ui = 0; ui < tools_count; ui++) {
            tnames[ui] = (tools[ui].vtable && tools[ui].vtable->name)
                             ? tools[ui].vtable->name(tools[ui].ctx)
                             : "";
        }
        tnc = tools_count;
    }

    static const int max_attempts = 3;
    char *saved_compile_json = NULL;
    size_t saved_compile_json_len = 0;
    hu_error_t last_err = HU_ERR_INVALID_ARGUMENT;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        hu_chat_message_t hm[1] = {
            {.role = HU_ROLE_USER, .content = hprompt, .content_len = hprompt_len}};
        hu_chat_request_t hreq = {.messages = hm,
                                  .messages_count = 1,
                                  .tools = NULL,
                                  .tools_count = 0,
                                  .response_format = "json_object",
                                  .response_format_len = 11};
        hu_chat_response_t hresp;
        memset(&hresp, 0, sizeof(hresp));
        err = chat_fn(chat_ctx, alloc, &hreq, model_name, model_name_len, temperature, &hresp);
        if (err != HU_OK) {
            hu_chat_response_free(alloc, &hresp);
            last_err = err;
            break;
        }
        if (!hresp.content || hresp.content_len == 0) {
            hu_chat_response_free(alloc, &hresp);
            last_err = HU_ERR_NOT_FOUND;
            if (attempt + 1 >= max_attempts)
                break;
            const char *note =
                "\n\nPrevious response was empty. Emit one valid HuLa JSON object.\n";
            char *np = append_repair_suffix(alloc, hprompt, hprompt_len, note, strlen(note));
            if (!np) {
                last_err = HU_ERR_OUT_OF_MEMORY;
                break;
            }
            hu_str_free(alloc, hprompt);
            hprompt = np;
            hprompt_len = strlen(np);
            continue;
        }

        hu_hula_program_deinit(&hcp);
        memset(&hcp, 0, sizeof(hcp));
        const char *const raw_body = hresp.content;
        const size_t raw_len = hresp.content_len;
        err = hu_hula_compiler_parse_response(alloc, raw_body, raw_len, &hcp);
        char *dup_ok = NULL;
        if (err == HU_OK && hcp.root && raw_body && raw_len > 0) {
            dup_ok = hu_strndup(alloc, raw_body, raw_len);
            if (!dup_ok)
                err = HU_ERR_OUT_OF_MEMORY;
        }
        hu_chat_response_free(alloc, &hresp);
        if (err != HU_OK || !hcp.root) {
            if (dup_ok)
                hu_str_free(alloc, dup_ok);
            hu_hula_program_deinit(&hcp);
            memset(&hcp, 0, sizeof(hcp));
            last_err = err != HU_OK ? err : HU_ERR_PARSE;
            if (attempt + 1 >= max_attempts)
                break;
            const char *note =
                "\n\nPrevious output was not valid HuLa JSON. Fix syntax and required fields.\n";
            char *np = append_repair_suffix(alloc, hprompt, hprompt_len, note, strlen(note));
            if (!np) {
                last_err = HU_ERR_OUT_OF_MEMORY;
                break;
            }
            hu_str_free(alloc, hprompt);
            hprompt = np;
            hprompt_len = strlen(np);
            continue;
        }

        if (saved_compile_json)
            hu_str_free(alloc, saved_compile_json);
        saved_compile_json = dup_ok;
        saved_compile_json_len = dup_ok ? raw_len : 0;

        hu_hula_validation_t hv;
        memset(&hv, 0, sizeof(hv));
        bool vok = (hu_hula_validate(&hcp, alloc, tnames, tnc, &hv) == HU_OK && hv.valid);
        if (vok) {
            hu_hula_validation_deinit(alloc, &hv);
            last_err = HU_OK;
            break;
        }

        char diag[768];
        size_t dpos = 0;
        dpos =
            hu_buf_appendf(diag, sizeof(diag), dpos,
                           "\n\nThe program failed validation. Fix and respond with JSON only.\n");
        for (size_t di = 0; di < hv.diag_count && di < HU_HULA_MAX_DIAGS && dpos + 2 < sizeof(diag);
             di++) {
            const char *m = hv.diags[di].message ? hv.diags[di].message : "?";
            int w = snprintf(diag + dpos, sizeof(diag) - dpos, "- %s\n", m);
            if (w > 0 && (size_t)w < sizeof(diag) - dpos)
                dpos += (size_t)w;
        }
        hu_hula_validation_deinit(alloc, &hv);
        last_err = HU_ERR_INVALID_ARGUMENT;
        if (attempt + 1 >= max_attempts)
            break;
        char *np = append_repair_suffix(alloc, hprompt, hprompt_len, diag, dpos);
        if (!np) {
            last_err = HU_ERR_OUT_OF_MEMORY;
            break;
        }
        hu_str_free(alloc, hprompt);
        hprompt = np;
        hprompt_len = strlen(np);
    }

    hu_str_free(alloc, hprompt);
    hprompt = NULL;

    if (tnames)
        alloc->free(alloc->ctx, (void *)tnames, tools_count * sizeof(const char *));

    if (last_err != HU_OK || !hcp.root) {
        if (saved_compile_json)
            hu_str_free(alloc, saved_compile_json);
        hu_hula_program_deinit(&hcp);
        return last_err;
    }

    /* Auto-insert VERIFY nodes after high-risk tool calls */
    {
        static const char *const default_high_risk[] = {
            "shell", "write_file", "delete_file", "http_request", "run_sql",
        };
        (void)hu_hula_auto_verify(&hcp, default_high_risk,
                                  sizeof(default_high_risk) / sizeof(default_high_risk[0]));
    }

    hu_hula_exec_t hcx;
    memset(&hcx, 0, sizeof(hcx));
    err = hu_hula_exec_init_full(&hcx, *alloc, &hcp, tools, tools_count, policy, observer);
    if (err != HU_OK) {
        if (saved_compile_json)
            hu_str_free(alloc, saved_compile_json);
        hu_hula_program_deinit(&hcp);
        return err;
    }
    if (pool && spawn_tpl)
        hu_hula_exec_set_spawn(&hcx, pool, spawn_tpl);
    err = hu_hula_exec_run(&hcx);
    bool hok = false;
    if (err == HU_OK) {
        if (hcp.root && hcp.root->id)
            hok = hu_hula_exec_result(&hcx, hcp.root->id)->status == HU_HULA_DONE;
        else
            hok = true;
        size_t trl = 0;
        const char *tr = hu_hula_exec_trace(&hcx, &trl);
        {
            char *pj = NULL;
            size_t pjl = 0;
            if (hu_hula_to_json(alloc, &hcp, &pj, &pjl) == HU_OK && pj) {
                (void)hu_hula_trace_persist(alloc, NULL, tr, trl, hcp.name, hcp.name_len, hok, pj,
                                            pjl, saved_compile_json, saved_compile_json_len);
                hu_str_free(alloc, pj);
            } else {
                (void)hu_hula_trace_persist(alloc, NULL, tr, trl, hcp.name, hcp.name_len, hok, NULL,
                                            0, saved_compile_json, saved_compile_json_len);
            }
        }
        if (hok && done_fn)
            done_fn(done_ctx, &hcp, &hcx);
        if (hok)
            *out_ok = true;
    }
    if (saved_compile_json)
        hu_str_free(alloc, saved_compile_json);
    hu_hula_exec_deinit(&hcx);
    hu_hula_program_deinit(&hcp);
    return err;
}

/* ── Auto-VERIFY insertion ────────────────────────────────────────────── */

static bool is_high_risk_tool(const char *name, const char *const *list, size_t list_count) {
    if (!name || !list)
        return false;
    size_t nlen = strlen(name);
    for (size_t i = 0; i < list_count; i++) {
        if (list[i] && strlen(list[i]) == nlen && memcmp(list[i], name, nlen) == 0)
            return true;
    }
    return false;
}

static size_t auto_verify_walk(hu_hula_program_t *prog, hu_hula_node_t *node,
                               const char *const *high_risk_tools, size_t hrt_count) {
    if (!node)
        return 0;
    size_t inserted = 0;

    if (node->op == HU_HULA_SEQ) {
        /*
         * For SEQ nodes, scan children for high-risk CALLs and insert
         * a VERIFY sibling after each one. We iterate backwards so
         * insertions don't shift unprocessed indices.
         */
        for (size_t i = node->children_count; i > 0; i--) {
            hu_hula_node_t *child = node->children[i - 1];
            if (!child)
                continue;
            inserted += auto_verify_walk(prog, child, high_risk_tools, hrt_count);

            if (child->op == HU_HULA_CALL && child->tool_name && child->id &&
                is_high_risk_tool(child->tool_name, high_risk_tools, hrt_count)) {
                if (node->children_count < HU_HULA_MAX_CHILDREN) {
                    char vid[64];
                    snprintf(vid, sizeof(vid), "auto_verify_%s", child->id);
                    hu_hula_node_t *vn = hu_hula_program_alloc_node(prog, HU_HULA_VERIFY, vid);
                    if (vn) {
                        vn->verify_node_id = hu_strndup(&prog->alloc, child->id, strlen(child->id));
                        vn->verify_node_id_len = strlen(child->id);
                        vn->pred = HU_HULA_PRED_SUCCESS;

                        /* Shift children to make room after current index */
                        for (size_t j = node->children_count; j > i; j--)
                            node->children[j] = node->children[j - 1];
                        node->children[i] = vn;
                        node->children_count++;
                        inserted++;
                    }
                }
            }
        }
    } else {
        for (size_t i = 0; i < node->children_count; i++)
            inserted += auto_verify_walk(prog, node->children[i], high_risk_tools, hrt_count);
    }
    return inserted;
}

size_t hu_hula_auto_verify(hu_hula_program_t *prog, const char *const *high_risk_tools,
                           size_t high_risk_tool_count) {
    if (!prog || !prog->root || !high_risk_tools || high_risk_tool_count == 0)
        return 0;
    return auto_verify_walk(prog, prog->root, high_risk_tools, high_risk_tool_count);
}
