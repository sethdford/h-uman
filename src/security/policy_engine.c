#include "seaclaw/security/policy_engine.h"
#include "seaclaw/core/string.h"
#include <stdlib.h>
#include <string.h>

#define MAX_RULES 256

struct sc_policy_engine {
    sc_allocator_t *alloc;
    sc_policy_rule_t rules[MAX_RULES];
    size_t rule_count;
};

sc_policy_engine_t *sc_policy_engine_create(sc_allocator_t *alloc) {
    if (!alloc)
        return NULL;
    sc_policy_engine_t *e = (sc_policy_engine_t *)alloc->alloc(alloc->ctx, sizeof(*e));
    if (!e)
        return NULL;
    memset(e, 0, sizeof(*e));
    e->alloc = alloc;
    return e;
}

void sc_policy_engine_destroy(sc_policy_engine_t *engine) {
    if (!engine)
        return;
    for (size_t i = 0; i < engine->rule_count; i++) {
        sc_policy_rule_t *r = &engine->rules[i];
        if (r->name)
            engine->alloc->free(engine->alloc->ctx, r->name, strlen(r->name) + 1);
        if (r->message)
            engine->alloc->free(engine->alloc->ctx, r->message, strlen(r->message) + 1);
    }
    engine->alloc->free(engine->alloc->ctx, engine, sizeof(*engine));
}

sc_error_t sc_policy_engine_add_rule(sc_policy_engine_t *engine, const char *name,
                                     sc_policy_match_t match, sc_policy_action_t action,
                                     const char *message) {
    if (!engine || !name)
        return SC_ERR_INVALID_ARGUMENT;
    if (engine->rule_count >= MAX_RULES)
        return SC_ERR_OUT_OF_MEMORY;

    sc_policy_rule_t *r = &engine->rules[engine->rule_count];
    r->name = sc_strndup(engine->alloc, name, strlen(name));
    r->match = match;
    r->action = action;
    r->message = message ? sc_strndup(engine->alloc, message, strlen(message)) : NULL;
    engine->rule_count++;
    return SC_OK;
}

static bool match_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle)
        return false;
    return strstr(haystack, needle) != NULL;
}

sc_policy_result_t sc_policy_engine_evaluate(sc_policy_engine_t *engine,
                                             const sc_policy_eval_ctx_t *ctx) {
    sc_policy_result_t result = {.action = SC_POLICY_ALLOW, .rule_name = NULL, .message = NULL};
    if (!engine || !ctx)
        return result;

    for (size_t i = 0; i < engine->rule_count; i++) {
        sc_policy_rule_t *r = &engine->rules[i];
        bool matched = true;

        if (r->match.tool && ctx->tool_name) {
            if (strcmp(r->match.tool, ctx->tool_name) != 0)
                matched = false;
        } else if (r->match.tool && !ctx->tool_name) {
            matched = false;
        }

        if (matched && r->match.args_contains && ctx->args_json) {
            if (!match_contains(ctx->args_json, r->match.args_contains))
                matched = false;
        } else if (matched && r->match.args_contains && !ctx->args_json) {
            matched = false;
        }

        if (matched && r->match.has_cost_check) {
            if (ctx->session_cost_usd <= r->match.session_cost_gt)
                matched = false;
        }

        if (matched && r->action != SC_POLICY_ALLOW) {
            result.action = r->action;
            result.rule_name = r->name;
            result.message = r->message;
            return result;
        }
    }
    return result;
}

size_t sc_policy_engine_rule_count(sc_policy_engine_t *engine) {
    return engine ? engine->rule_count : 0;
}
