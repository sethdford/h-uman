/* include/human/agent/turn.h — per-turn context for the carved hu_agent_turn stages.
 *
 * hu_agent_turn (src/agent/agent_turn.c) is being carved into stages under
 * src/agent/turn/ (docs/superpowers/specs/2026-09-30-agent-turn-carve-design.md).
 * Each stage is a verbatim move of one block of the old function; this struct
 * is that block's MEASURED surface — the locals it read and the locals it
 * produced — and nothing else. It grows only as stages move.
 *
 * Ownership: a field marked "owned" holds an agent->alloc allocation. A stage
 * writes it; hu_agent_turn unpacks it into its historical local and clears the
 * field in the same step, so exactly one owner exists at any time.
 * hu_turn_ctx_free releases whatever is still owned, then the struct.
 *
 * Lifetime: heap-allocated once per hu_agent_turn call, never on the stack —
 * the turn runs on worker threads, and ASan on Darwin arm64 false-positives
 * cross-thread stack structs (.claude/rules/asan-pthread-stack-aliasing-darwin.md).
 */
#ifndef HU_AGENT_TURN_H
#define HU_AGENT_TURN_H

#include "human/agent.h"
#include "human/cognition/dual_process.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/adaptive_rag.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_tool_cache;

typedef struct hu_turn_ctx {
    hu_allocator_t *alloc; /* agent->alloc at creation; owns this struct */
    struct {
        hu_agent_t *agent;
        const char *msg;
        size_t msg_len;
        char **response_out;
        size_t *response_len_out;
    } in;
    struct {
        hu_cognition_budget_t cognition_budget; /* retrieval budget: S2 output, S3 input */
        char *acp_context;                      /* owned: pending inter-agent messages */
        size_t acp_context_len;
        const char *tone_hint; /* static storage (rhythm literals / tone table) */
        size_t tone_hint_len;
        char *pref_ctx; /* owned: stored user preferences */
        size_t pref_ctx_len;
    } perception;
    struct {
        char *memory_ctx; /* owned */
        size_t memory_ctx_len;
        char *graph_ctx; /* owned */
        size_t graph_ctx_len;
        bool memory_ctx_nonempty; /* memory_ctx was non-empty BEFORE the W12 merge */
        char *instruction_ctx;    /* borrowed from agent->instruction_discovery */
        size_t instruction_ctx_len;
        hu_rag_strategy_t rag_strategy_used;
    } retrieval;
    struct {
        const char *plan_ctx; /* borrowed from the turn body (input to S4 and S17) */
        size_t plan_ctx_len;
        char *stm_ctx; /* owned */
        size_t stm_ctx_len;
        char *commitment_ctx; /* owned */
        size_t commitment_ctx_len;
        char *pattern_ctx; /* owned */
        size_t pattern_ctx_len;
        char *proactive_ctx; /* owned */
        size_t proactive_ctx_len;
        char *superhuman_ctx; /* owned */
        size_t superhuman_ctx_len;
        char *adaptive_ctx; /* owned */
        size_t adaptive_ctx_len;
        char *awareness_ctx; /* owned */
        size_t awareness_ctx_len;
        char *outcome_ctx; /* owned */
        size_t outcome_ctx_len;
        char *intelligence_ctx; /* owned */
        size_t intelligence_ctx_len;
    } context;
    struct {
        struct hu_tool_cache *turn_cache; /* borrowed; owned by the turn body */
        size_t turn_tool_results_count;   /* in/out: accumulates across iterations */
        uint32_t iter;                    /* S17 input: the current tool iteration (1-based) */
        uint64_t turn_tokens;             /* S17 input: tokens this turn has used so far */
        size_t replan_floor;              /* S17 scan floor: agent->history_count at hu_turn_ctx_new
                                           * (earlier turns sit below it), then after each replan
                                           * (failures below it have had theirs) */
    } loop;
} hu_turn_ctx_t;

/* How a stage that can end the turn reports back (S0, S8). */
typedef enum hu_turn_step_kind {
    HU_TURN_STEP_CONTINUE = 0, /* fall through to the rest of the turn */
    HU_TURN_STEP_RETURN = 1,   /* hu_agent_turn returns `err` now */
} hu_turn_step_kind_t;

typedef struct hu_turn_step {
    hu_turn_step_kind_t kind;
    hu_error_t err;
} hu_turn_step_t;

static inline hu_turn_step_t hu_turn_step_continue(void) {
    hu_turn_step_t s = {HU_TURN_STEP_CONTINUE, HU_OK};
    return s;
}

static inline hu_turn_step_t hu_turn_step_return(hu_error_t err) {
    hu_turn_step_t s = {HU_TURN_STEP_RETURN, err};
    return s;
}

/* NULL when agent, agent->alloc or the allocation is missing. Stores the
 * pointers and records agent->history_count as loop.replan_floor; touches
 * neither *response_out nor *response_len_out. */
hu_turn_ctx_t *hu_turn_ctx_new(hu_agent_t *agent, const char *msg, size_t msg_len,
                               char **response_out, size_t *response_len_out);

/* Releases every still-owned field, then the struct. NULL-safe. */
void hu_turn_ctx_free(hu_turn_ctx_t *turn_ctx);

/* S3 retrieval (src/agent/turn/turn_retrieve.c): Self-RAG gate, memory loader,
 * graph grounding, instruction discovery, data-quality check, adaptive-RAG
 * pick, W12 contact-recall merge. Reads in.*, perception.cognition_budget;
 * writes retrieval.*. HU_ERR_INVALID_ARGUMENT on a NULL ctx or agent. */
hu_error_t hu_turn_retrieve(hu_turn_ctx_t *turn_ctx);

/* S1 plan resume (src/agent/turn/turn_plan.c): a copy of the newest
 * "[ACTIVE_PLAN]" system message among the last 10 history entries, allocated
 * with agent->alloc (caller frees plan_len + 1 bytes), or NULL when there is
 * none, on NULL input or on allocation failure. *plan_len_out is always set. */
char *hu_turn_active_plan(hu_agent_t *agent, size_t *plan_len_out);

/* S2 perception (src/agent/turn/turn_perceive.c): ACP inbox, cognition budget +
 * dual-process dispatch, fast capture / STM / pattern radar, commitments,
 * preference and outcome learning, tone and rhythm hints. Reads in.*; writes
 * perception.*. HU_ERR_INVALID_ARGUMENT on a NULL ctx, agent or msg. */
hu_error_t hu_turn_perceive(hu_turn_ctx_t *turn_ctx);

/* S0 entry (src/agent/turn/turn_entry.c): clears the out-params, per-turn
 * resets, speculative + semantic response caches, mailbox, slash commands,
 * input guard, persona-correction observation. RETURN when the inline block
 * returned (a cache hit, a slash command, a guard verdict); CONTINUE
 * otherwise. RETURN(HU_ERR_INVALID_ARGUMENT) on a NULL ctx, agent or
 * response_out. */
hu_turn_step_t hu_turn_entry(hu_turn_ctx_t *turn_ctx);

/* S8 silence gate (src/agent/turn/turn_silence.c): decides whether to skip the
 * LLM call; when it answers (silence or a brief acknowledgment) it writes
 * *response_out, records the experience (SQLite builds) and returns RETURN(HU_OK)
 * — the caller then frees its turn-body buffers. CONTINUE when the full
 * response path should run. RETURN(HU_ERR_INVALID_ARGUMENT) on NULL input. */
hu_turn_step_t hu_turn_silence(hu_turn_ctx_t *turn_ctx);

/* S4 context builders (src/agent/turn/turn_context.c): STM, commitments,
 * pattern radar, proactive, superhuman + cross-channel identity,
 * adaptive/circadian, awareness + PWA, outcomes, AGI-frontier intelligence.
 * Reads in.*, context.plan_ctx; writes the nine owned context.* strings.
 * HU_ERR_INVALID_ARGUMENT on a NULL ctx or agent. */
hu_error_t hu_turn_context(hu_turn_ctx_t *turn_ctx);

/* S16 tool dispatch (src/agent/turn/turn_tools.c): runs the tool calls of the
 * newest assistant message in agent->history — LOCKED short-circuit, HuLa
 * compiler / LLMCompiler DAG and native HuLa IR (daemon builds), multi-agent
 * orchestrator, dispatcher with world-model ordering and TTL cache, per-tool
 * guards and learning, sequential fallback — appending one tool result each.
 * Reads in.*, loop.turn_cache; advances loop.turn_tool_results_count.
 * HU_ERR_INVALID_ARGUMENT on a NULL ctx or agent, else HU_OK. */
hu_error_t hu_turn_tools(hu_turn_ctx_t *turn_ctx);

/* S17 iteration tail (src/agent/turn/turn_tail.c): after one iteration's tool
 * results — replan when a plan is in progress and >= 2 of THIS turn's tool
 * results failed since its last replan (within the 8 newest history entries),
 * mid-turn memory retrieval against the last tool result (plus the user's goal
 * from iteration 2), scratchpad turn metadata, periodic checkpoint. Reads in.*,
 * context.plan_ctx (presence only), loop.iter, loop.turn_tokens,
 * loop.turn_tool_results_count, loop.replan_floor; writes
 * loop.replan_floor and otherwise only through in.agent.
 * HU_ERR_INVALID_ARGUMENT on a NULL ctx, agent or msg, else HU_OK. */
hu_error_t hu_turn_tail(hu_turn_ctx_t *turn_ctx);

/* Keeps loop.replan_floor pointing at the same entries after mid-turn history
 * compaction (src/agent/turn/turn_tail.c): compaction drops entries from the
 * front, so a floor recorded before it would sit past the end of the shrunk
 * history and S17 would never scan again. Shifts the floor down by
 * before - after, clamped at 0; a non-shrinking change leaves it alone.
 * NULL-safe. */
void hu_turn_note_history_shift(hu_turn_ctx_t *turn_ctx, size_t before, size_t after);

/* S18 tool-iterations-exhausted exit (src/agent/turn/turn_tail.c): records the
 * TOOL_ITERATIONS_EXHAUSTED and ERR observer events. The exit's frees and its
 * HU_ERR_TIMEOUT return stay in the turn body (plan gap G7).
 * HU_ERR_INVALID_ARGUMENT on a NULL ctx or agent, else HU_OK. */
hu_error_t hu_turn_exhausted(hu_turn_ctx_t *turn_ctx);

/* Renders the personal-model prompt block for this turn into buf
 * (src/agent/turn/personal_model_prompt.c): the HU_CONFIDENCE_BOUNDARY view
 * of agent->personal_model, with the reflection-loop slice when that loop is
 * on and the memory is sqlite. Returns the bytes written (0 when there is
 * nothing to render, or on NULL/0-cap input). */
size_t hu_turn_personal_model_prompt(hu_agent_t *agent, char *buf, size_t cap);

/* Per-turn thread scope around agent_turn_run (src/agent/turn/turn_scope.c):
 * tags the local-only guard's audit caller as "agent_turn" and, when the
 * thread is untagged, sets the X-HU-Purpose to REPLY (a caller's tag wins).
 * hu_turn_scope_exit restores both, in reverse order. */
typedef struct hu_turn_scope {
    const char *local_only_prev;
    int llm_purpose_prev; /* hu_llm_purpose_t */
} hu_turn_scope_t;
hu_turn_scope_t hu_turn_scope_enter(void);
void hu_turn_scope_exit(hu_turn_scope_t scope);

#endif /* HU_AGENT_TURN_H */
