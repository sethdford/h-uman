#ifndef HU_AGENT_AUTODREAM_H
#define HU_AGENT_AUTODREAM_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stdint.h>

struct hu_graph;
struct hu_memory_facade;

/* W2 — AutoDream: scheduled background consolidation.
 *
 * Drives idle-time refinement of the memory graph. The intent matches Claude
 * Code AutoDream and Cognee `memify`: while the user is away, review what we
 * wrote, summarize what we know, drop what was wrong.
 *
 * Each phase function is independent and respects the global runtime budget.
 * If a phase trips the budget it stops cleanly and the next run picks up where
 * it left off. AutoDream NEVER blocks the user-facing path. */

typedef struct hu_autodream_config {
    int64_t now_ms;                  /* "current time" for tests; 0 = use real clock */
    int64_t quarantine_max_age_ms;   /* drop quarantine entries older than this; default 14d */
    int64_t max_runtime_ms;          /* total wall-clock budget; default 300_000 (5 min) */
    bool enable_quarantine_review;   /* default true */
    bool enable_community_summaries; /* default true */
    bool enable_edge_reweight;       /* default true */
    bool enable_derived_facts;       /* default true */
    bool dry_run;                    /* if true, classify but don't write */
} hu_autodream_config_t;

typedef struct hu_autodream_report {
    int64_t started_at_ms;
    int64_t finished_at_ms;
    size_t quarantine_reviewed;
    size_t quarantine_released;
    size_t quarantine_dropped;
    size_t communities_summarized;
    size_t edges_reweighted;
    size_t derived_facts_added;
    bool budget_exceeded;
    /* Last error message (best-effort, truncated). Empty on full success. */
    char last_error[128];
} hu_autodream_report_t;

/* Default config: all phases enabled, 14-day quarantine TTL, 5-minute budget,
 * `now_ms` = 0 means "ask the OS clock." */
hu_autodream_config_t hu_autodream_default_config(void);

/* Run one AutoDream cycle synchronously. Caller is responsible for scheduling
 * (e.g. daemon idle detector). Each phase respects the runtime budget. */
hu_error_t hu_autodream_run(hu_allocator_t *alloc, struct hu_graph *graph,
                            const hu_autodream_config_t *cfg,
                            hu_autodream_report_t *out_report);

/* Same as `hu_autodream_run` but when `m` is non-NULL, quarantine **release**
 * promotes the live relation through `hu_memory_facade_write` (HU_MEM_RELATION)
 * so ingestion stays on the W7 surface. DDL and batch SQL use the same
 * SQLite handle as the facade when available. If `m` is NULL or has no graph
 * handle, returns
 * HU_ERR_INVALID_ARGUMENT. */
hu_error_t hu_autodream_run_on_facade(hu_allocator_t *alloc, struct hu_memory_facade *m,
                                      const hu_autodream_config_t *cfg,
                                      hu_autodream_report_t *out_report);

/* Generate-or-refresh a community summary for one (contact, community) pair.
 * Heuristic backend: produces a structured summary from entity names + edge
 * counts. Replace with an LLM-driven backend by injecting a vtable later. */
hu_error_t hu_autodream_summarize_community(hu_allocator_t *alloc, struct hu_graph *graph,
                                            const char *contact_id, size_t contact_id_len,
                                            int64_t community_id, int64_t now_ms);

/* Read the current summary for a (contact, community). Returns HU_ERR_NOT_FOUND
 * if none exists. Caller must free *out_summary via alloc. */
hu_error_t hu_autodream_read_community_summary(hu_allocator_t *alloc, struct hu_graph *graph,
                                               const char *contact_id, size_t contact_id_len,
                                               int64_t community_id, char **out_summary,
                                               size_t *out_summary_len);

#endif /* HU_AGENT_AUTODREAM_H */
