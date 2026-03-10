#ifndef HU_POLICY_ENGINE_H
#define HU_POLICY_ENGINE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_policy_action {
    HU_POLICY_ALLOW,
    HU_POLICY_DENY,
    HU_POLICY_REQUIRE_APPROVAL,
} hu_policy_action_t;

typedef struct hu_policy_match {
    const char *tool;
    const char *args_contains;
    double session_cost_gt;
    bool has_cost_check;
} hu_policy_match_t;

typedef struct hu_policy_rule {
    char *name;
    hu_policy_match_t match;
    hu_policy_action_t action;
    char *message;
} hu_policy_rule_t;

typedef struct hu_policy_engine hu_policy_engine_t;

hu_policy_engine_t *hu_policy_engine_create(hu_allocator_t *alloc);
void hu_policy_engine_destroy(hu_policy_engine_t *engine);

hu_error_t hu_policy_engine_add_rule(hu_policy_engine_t *engine, const char *name,
                                     hu_policy_match_t match, hu_policy_action_t action,
                                     const char *message);

typedef struct hu_policy_eval_ctx {
    const char *tool_name;
    const char *args_json;
    double session_cost_usd;
} hu_policy_eval_ctx_t;

typedef struct hu_policy_result {
    hu_policy_action_t action;
    const char *rule_name;
    const char *message;
} hu_policy_result_t;

hu_policy_result_t hu_policy_engine_evaluate(hu_policy_engine_t *engine,
                                             const hu_policy_eval_ctx_t *ctx);

size_t hu_policy_engine_rule_count(hu_policy_engine_t *engine);

#endif /* HU_POLICY_ENGINE_H */
