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
} hu_turn_ctx_t;

/* NULL when agent, agent->alloc or the allocation is missing. Stores the
 * pointers only; touches neither *response_out nor *response_len_out. */
hu_turn_ctx_t *hu_turn_ctx_new(hu_agent_t *agent, const char *msg, size_t msg_len,
                               char **response_out, size_t *response_len_out);

/* Releases every still-owned field, then the struct. NULL-safe. */
void hu_turn_ctx_free(hu_turn_ctx_t *turn_ctx);

/* S3 retrieval (src/agent/turn/turn_retrieve.c): Self-RAG gate, memory loader,
 * graph grounding, instruction discovery, data-quality check, adaptive-RAG
 * pick, W12 contact-recall merge. Reads in.*, perception.cognition_budget;
 * writes retrieval.*. HU_ERR_INVALID_ARGUMENT on a NULL ctx or agent. */
hu_error_t hu_turn_retrieve(hu_turn_ctx_t *turn_ctx);

/* S2 perception (src/agent/turn/turn_perceive.c): ACP inbox, cognition budget +
 * dual-process dispatch, fast capture / STM / pattern radar, commitments,
 * preference and outcome learning, tone and rhythm hints. Reads in.*; writes
 * perception.*. HU_ERR_INVALID_ARGUMENT on a NULL ctx, agent or msg. */
hu_error_t hu_turn_perceive(hu_turn_ctx_t *turn_ctx);

#endif /* HU_AGENT_TURN_H */
