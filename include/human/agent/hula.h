#ifndef HU_AGENT_HULA_H
#define HU_AGENT_HULA_H

#include "human/agent/dag.h"
#include "human/agent/planner.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/observer.h"
#include "human/security.h"
#include "human/tool.h"

struct hu_agent_pool;
struct hu_spawn_config;
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * HuLa — Human Language: a minimal typed IR for agent programs.
 *
 * HuLa unifies the planner, DAG/LLM-compiler, and skill systems into a
 * single serializable intermediate representation that sits between LLM
 * output and vtable dispatch.
 *
 * Design principles:
 *   - 8 opcodes, no more. Small grammar, growing vocabulary.
 *   - Machine-checkable: parse → validate → policy → execute.
 *   - Compiles from natural language (or traces) into the IR.
 *   - Maps 1:1 to existing vtable dispatch (tools, channels, memory).
 *   - Emergence: hot paths from logs become named programs (skills).
 *
 * Opcode set:
 *   CALL      — invoke a registered tool with JSON args
 *   SEQ       — execute children in order (short-circuits on failure)
 *   PAR       — execute children concurrently (join-all)
 *   BRANCH    — conditional: if tool result matches predicate, then/else
 *   LOOP      — repeat body while predicate holds (bounded)
 *   DELEGATE  — spawn a sub-agent with a goal string
 *   EMIT      — produce a named output (return value, side-channel message)
 *   TRY       — run body child; on failure optionally run catch child
 *   VERIFY    — validate preceding node output against predicate; halts on mismatch
 *               (arXiv:2603.11445 VMAO-inspired verification-driven execution)
 *
 * Wire format: JSON (parse/serialize), with a compact text notation for
 * human-readable debugging.
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_HULA_MAX_CHILDREN 32
#define HU_HULA_MAX_DEPTH    16
#define HU_HULA_MAX_NODES    128
#define HU_HULA_VERSION      1

typedef enum hu_hula_op {
    HU_HULA_CALL = 0,
    HU_HULA_SEQ,
    HU_HULA_PAR,
    HU_HULA_BRANCH,
    HU_HULA_LOOP,
    HU_HULA_DELEGATE,
    HU_HULA_EMIT,
    HU_HULA_TRY,
    HU_HULA_VERIFY,
    HU_HULA_OP_COUNT,
} hu_hula_op_t;

/* Predicate for BRANCH/LOOP conditions */
typedef enum hu_hula_pred {
    HU_HULA_PRED_SUCCESS = 0, /* last result was success */
    HU_HULA_PRED_FAILURE,     /* last result was failure */
    HU_HULA_PRED_CONTAINS,    /* output contains match_str */
    HU_HULA_PRED_NOT_CONTAINS,
    HU_HULA_PRED_ALWAYS, /* unconditional (for loops with explicit max) */
} hu_hula_pred_t;

typedef struct hu_hula_node hu_hula_node_t;

struct hu_hula_node {
    hu_hula_op_t op;
    char *id; /* unique node identifier; owned */

    /* CALL: tool invocation */
    char *tool_name; /* owned */
    char *args_json; /* owned; JSON object string */

    /* BRANCH / LOOP: condition */
    hu_hula_pred_t pred;
    char *match_str; /* owned; for CONTAINS predicates */
    size_t match_str_len;
    uint32_t max_iter; /* LOOP bound; 0 = use default (10) */

    /* DELEGATE: sub-agent goal */
    char *goal; /* owned */
    size_t goal_len;
    char *delegate_model; /* owned; optional model override */
    size_t delegate_model_len;

    /* EMIT: output channel */
    char *emit_key; /* owned; output name/channel */
    size_t emit_key_len;
    char *emit_value; /* owned; template, may reference $node_id */
    size_t emit_value_len;

    /* Execution hints (any opcode) */
    uint32_t timeout_ms;       /* 0 = no per-node wall limit */
    uint32_t retry_count;      /* extra attempts after first failure */
    uint32_t retry_backoff_ms; /* delay between retries (skipped in HU_IS_TEST) */

    /* DELEGATE: structured handoff / fleet */
    char *delegate_context; /* owned; prepended to child system prompt */
    size_t delegate_context_len;
    char *delegate_result_key; /* owned; if set, DONE output is stored as a named slot */
    size_t delegate_result_key_len;
    char *delegate_agent_id; /* owned; optional registry agent name for spawn_named */
    size_t delegate_agent_id_len;

    /* VERIFY: validate a preceding node's output */
    char *verify_node_id; /* owned; the node whose output to check */
    size_t verify_node_id_len;
    /* Uses pred + match_str for the validation condition (shared with BRANCH/LOOP) */

    /* Policy: node requires this capability token in policy->hula_capability_allowlist */
    char *required_capability; /* owned */
    size_t required_capability_len;

    /* Tree structure */
    hu_hula_node_t *children[HU_HULA_MAX_CHILDREN];
    size_t children_count;

    /* For BRANCH: then_branch = children[0], else_branch = children[1],
       condition_node = the CALL preceding the branch (resolved at validate) */
    char *description; /* owned; optional human-readable annotation */
};

/* A complete HuLa program */
typedef struct hu_hula_program {
    hu_allocator_t alloc;
    char *name; /* owned; program/skill name */
    size_t name_len;
    uint32_t version;
    hu_hula_node_t *nodes; /* flat owned array of all nodes */
    size_t node_count;
    size_t node_cap;
    hu_hula_node_t *root; /* entry point (points into nodes array) */
} hu_hula_program_t;

/* Execution result per node */
typedef enum hu_hula_status {
    HU_HULA_PENDING = 0,
    HU_HULA_RUNNING,
    HU_HULA_DONE,
    HU_HULA_FAILED,
    HU_HULA_SKIPPED,
    HU_HULA_CANCELLED,
} hu_hula_status_t;

typedef struct hu_hula_result {
    hu_hula_status_t status;
    char *output; /* owned */
    size_t output_len;
    char *error; /* owned */
    size_t error_len;
} hu_hula_result_t;

#define HU_HULA_MAX_SLOTS 64

typedef struct hu_hula_slot {
    char *key; /* owned */
    size_t key_len;
    char *value; /* owned */
    size_t value_len;
} hu_hula_slot_t;

struct hu_agent_registry;
struct hu_agent;

/* Execution state for a running program */
typedef struct hu_hula_exec {
    hu_allocator_t alloc;
    hu_hula_program_t *program; /* borrowed */
    hu_hula_result_t *results;  /* owned; indexed by node position */
    size_t results_count;
    hu_tool_t *tools; /* borrowed */
    size_t tools_count;
    hu_security_policy_t *policy;      /* borrowed; optional — NULL skips policy */
    hu_observer_t *observer;           /* borrowed; optional — NULL skips tracing */
    struct hu_agent_pool *pool;        /* borrowed; optional — NULL stubs delegate */
    struct hu_spawn_config *spawn_cfg; /* borrowed; optional — template for delegate spawns */
    /* Trace log for emergence analysis */
    char *trace_log; /* owned; accumulated JSON array of executed nodes */
    size_t trace_log_len;
    size_t trace_log_cap;
    bool halted;
    char *halt_reason; /* owned */
    size_t halt_reason_len;
    hu_hula_slot_t slots[HU_HULA_MAX_SLOTS];
    size_t slot_count;
    /* Optional budget (set via hu_hula_exec_set_budget); 0 = unlimited per field */
    bool budget_enabled;
    uint32_t budget_max_depth;      /* execution depth from root; 0 = unlimited */
    uint32_t budget_max_wall_ms;    /* wall clock since hu_hula_exec_run; 0 = unlimited */
    uint32_t budget_max_tool_calls; /* HU_HULA_CALL invocations; 0 = unlimited */
    uint64_t budget_run_start_ms;
    uint32_t budget_tool_calls_used;
    struct hu_agent_registry *delegate_registry; /* optional; for delegate_agent_id */
    struct hu_idempotency_registry *idempotency_registry; /* optional; for crash-proof replay */
    /* Optional agent for Wave A security envelope on CALL (borrowed). */
    struct hu_agent *security_agent;
} hu_hula_exec_t;

/* Validation diagnostics */
typedef struct hu_hula_diag {
    char *message; /* owned */
    size_t message_len;
    const hu_hula_node_t *node; /* borrowed; may be NULL */
} hu_hula_diag_t;

#define HU_HULA_MAX_DIAGS 32

typedef struct hu_hula_validation {
    hu_hula_diag_t diags[HU_HULA_MAX_DIAGS];
    size_t diag_count;
    bool valid;
} hu_hula_validation_t;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

hu_error_t hu_hula_program_init(hu_hula_program_t *prog, hu_allocator_t alloc, const char *name,
                                size_t name_len);
void hu_hula_program_deinit(hu_hula_program_t *prog);

/* Allocate a new node within the program. Returns pointer into the arena. */
hu_hula_node_t *hu_hula_program_alloc_node(hu_hula_program_t *prog, hu_hula_op_t op,
                                           const char *id);

/* ── Parse ──────────────────────────────────────────────────────────────── */

/* Parse a HuLa program from JSON. */
hu_error_t hu_hula_parse_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                              hu_hula_program_t *out);

/* Serialize a HuLa program to JSON. Caller frees output. */
hu_error_t hu_hula_to_json(hu_allocator_t *alloc, const hu_hula_program_t *prog, char **out,
                           size_t *out_len);

/* ── Validate ───────────────────────────────────────────────────────────── */

/* Validate program structure, tool references, depth, cycles, etc.
 * tool_names: available tool names for reference checking (may be NULL to skip). */
hu_error_t hu_hula_validate(const hu_hula_program_t *prog, hu_allocator_t *alloc,
                            const char *const *tool_names, size_t tool_count,
                            hu_hula_validation_t *out);

void hu_hula_validation_deinit(hu_allocator_t *alloc, hu_hula_validation_t *v);

/* ── Execute ────────────────────────────────────────────────────────────── */

hu_error_t hu_hula_exec_init(hu_hula_exec_t *exec, hu_allocator_t alloc, hu_hula_program_t *prog,
                             hu_tool_t *tools, size_t tools_count);

/* Create exec with policy and observer (full integration). */
hu_error_t hu_hula_exec_init_full(hu_hula_exec_t *exec, hu_allocator_t alloc,
                                  hu_hula_program_t *prog, hu_tool_t *tools, size_t tools_count,
                                  hu_security_policy_t *policy, hu_observer_t *observer);

/* Optional: enable delegate opcode to use hu_agent_pool_spawn (non-test builds). */
void hu_hula_exec_set_spawn(hu_hula_exec_t *exec, struct hu_agent_pool *pool,
                            struct hu_spawn_config *spawn_cfg);

/* Optional: registry for delegate nodes with delegate_agent_id (spawn_named). */
void hu_hula_exec_set_delegate_registry(hu_hula_exec_t *exec, struct hu_agent_registry *registry);

/* Optional: idempotency registry for crash-proof tool execution (workflow replay). */
void hu_hula_exec_set_idempotency_registry(hu_hula_exec_t *exec,
                                           struct hu_idempotency_registry *registry);

/* Optional: borrow an agent so CALL nodes run the full pre-execute security
 * envelope (permission → hook → escalate → policy) before tool execute. */
void hu_hula_exec_set_security_agent(hu_hula_exec_t *exec, struct hu_agent *agent);

/* Optional: stop execution; in-flight nodes complete as cancelled. */
void hu_hula_exec_cancel(hu_hula_exec_t *exec, const char *reason, size_t reason_len);

/* Optional: enforce max depth, wall time, and tool-call count (0 = unlimited each). */
void hu_hula_exec_set_budget(hu_hula_exec_t *exec, uint32_t max_depth, uint32_t max_wall_ms,
                             uint32_t max_tool_calls);

/* Run the program to completion. */
hu_error_t hu_hula_exec_run(hu_hula_exec_t *exec);

/* Get result for a specific node by id. Returns NULL if not found. */
const hu_hula_result_t *hu_hula_exec_result(const hu_hula_exec_t *exec, const char *node_id);

/* Get the accumulated trace log (JSON array). Borrowed pointer, valid until exec_deinit. */
const char *hu_hula_exec_trace(const hu_hula_exec_t *exec, size_t *out_len);

void hu_hula_exec_deinit(hu_hula_exec_t *exec);

/* ── Utilities ──────────────────────────────────────────────────────────── */

const char *hu_hula_op_name(hu_hula_op_t op);
const char *hu_hula_pred_name(hu_hula_pred_t pred);
const char *hu_hula_status_name(hu_hula_status_t status);

/* Pre-execution bounds (conservative; loops use max_iter, branch counts both arms). */
typedef struct hu_hula_cost_estimate {
    size_t estimated_tool_calls;
    size_t max_parallel_width;
    uint32_t max_loop_iterations_bound;
    hu_command_risk_level_t max_tool_risk;
} hu_hula_cost_estimate_t;

void hu_hula_estimate_cost(const hu_hula_program_t *prog, hu_hula_cost_estimate_t *out);

/* ── Bridges (convert existing structures to HuLa) ──────────────────── */

hu_error_t hu_hula_from_plan(hu_allocator_t *alloc, const hu_plan_t *plan, const char *name,
                             size_t name_len, hu_hula_program_t *out);

hu_error_t hu_hula_from_dag(hu_allocator_t *alloc, const hu_dag_t *dag, const char *name,
                            size_t name_len, hu_hula_program_t *out);

#endif /* HU_AGENT_HULA_H */
