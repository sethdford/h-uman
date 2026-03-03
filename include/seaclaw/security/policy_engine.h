#ifndef SC_POLICY_ENGINE_H
#define SC_POLICY_ENGINE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum sc_policy_action {
    SC_POLICY_ALLOW,
    SC_POLICY_DENY,
    SC_POLICY_REQUIRE_APPROVAL,
} sc_policy_action_t;

typedef struct sc_policy_match {
    const char *tool;
    const char *args_contains;
    double session_cost_gt;
    bool has_cost_check;
} sc_policy_match_t;

typedef struct sc_policy_rule {
    char *name;
    sc_policy_match_t match;
    sc_policy_action_t action;
    char *message;
} sc_policy_rule_t;

typedef struct sc_policy_engine sc_policy_engine_t;

sc_policy_engine_t *sc_policy_engine_create(sc_allocator_t *alloc);
void sc_policy_engine_destroy(sc_policy_engine_t *engine);

sc_error_t sc_policy_engine_add_rule(sc_policy_engine_t *engine, const char *name,
                                     sc_policy_match_t match, sc_policy_action_t action,
                                     const char *message);

typedef struct sc_policy_eval_ctx {
    const char *tool_name;
    const char *args_json;
    double session_cost_usd;
} sc_policy_eval_ctx_t;

typedef struct sc_policy_result {
    sc_policy_action_t action;
    const char *rule_name;
    const char *message;
} sc_policy_result_t;

sc_policy_result_t sc_policy_engine_evaluate(sc_policy_engine_t *engine,
                                             const sc_policy_eval_ctx_t *ctx);

size_t sc_policy_engine_rule_count(sc_policy_engine_t *engine);

#endif /* SC_POLICY_ENGINE_H */
