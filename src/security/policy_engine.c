#include "human/security/policy_engine.h"
#include "human/core/string.h"
#include <stdlib.h>
#include <string.h>

#define MAX_RULES 256

struct hu_policy_engine {
    hu_allocator_t *alloc;
    hu_policy_rule_t rules[MAX_RULES];
    size_t rule_count;
};

hu_policy_engine_t *hu_policy_engine_create(hu_allocator_t *alloc) {
    if (!alloc)
        return NULL;
    hu_policy_engine_t *e = (hu_policy_engine_t *)alloc->alloc(alloc->ctx, sizeof(*e));
    if (!e)
        return NULL;
    memset(e, 0, sizeof(*e));
    e->alloc = alloc;
    return e;
}

void hu_policy_engine_destroy(hu_policy_engine_t *engine) {
    if (!engine)
        return;
    for (size_t i = 0; i < engine->rule_count; i++) {
        hu_policy_rule_t *r = &engine->rules[i];
        if (r->name)
            engine->alloc->free(engine->alloc->ctx, r->name, strlen(r->name) + 1);
        if (r->message)
            engine->alloc->free(engine->alloc->ctx, r->message, strlen(r->message) + 1);
    }
    engine->alloc->free(engine->alloc->ctx, engine, sizeof(*engine));
}

hu_error_t hu_policy_engine_add_rule(hu_policy_engine_t *engine, const char *name,
                                     hu_policy_match_t match, hu_policy_action_t action,
                                     const char *message) {
    if (!engine || !name)
        return HU_ERR_INVALID_ARGUMENT;
    if (engine->rule_count >= MAX_RULES)
        return HU_ERR_OUT_OF_MEMORY;

    hu_policy_rule_t *r = &engine->rules[engine->rule_count];
    r->name = hu_strndup(engine->alloc, name, strlen(name));
    r->match = match;
    r->action = action;
    r->message = message ? hu_strndup(engine->alloc, message, strlen(message)) : NULL;
    engine->rule_count++;
    return HU_OK;
}

static bool match_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle)
        return false;
    return strstr(haystack, needle) != NULL;
}

hu_policy_result_t hu_policy_engine_evaluate(hu_policy_engine_t *engine,
                                             const hu_policy_eval_ctx_t *ctx) {
    hu_policy_result_t result = {.action = HU_POLICY_ALLOW, .rule_name = NULL, .message = NULL};
    if (!engine || !ctx)
        return result;

    for (size_t i = 0; i < engine->rule_count; i++) {
        hu_policy_rule_t *r = &engine->rules[i];
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

        if (matched && r->action != HU_POLICY_ALLOW) {
            result.action = r->action;
            result.rule_name = r->name;
            result.message = r->message;
            return result;
        }
    }
    return result;
}

size_t hu_policy_engine_rule_count(hu_policy_engine_t *engine) {
    return engine ? engine->rule_count : 0;
}
