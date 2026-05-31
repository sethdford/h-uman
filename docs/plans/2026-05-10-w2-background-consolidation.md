---
title: "W2 — Background Consolidation: AutoDream subagent + community summaries + life chapters"
created: 2026-05-10
status: closed
parent: 2026-05-10-memory-roadmap-overview.md
depends_on: 2026-05-10-w1-bitemporal-foundation.md
risk: medium
scope: src/agent/, src/memory/, include/human/agent/, include/human/memory/
last_audit: 2026-05-25
---

# W2 — Background Consolidation

## Goal

Give h-uman REM sleep. A scheduled idle subagent runs while the user is away and:

1. Reviews W1's quarantined writes (release / drop / flag).
2. Runs the LLM-driven conflict resolver on relations the deterministic write-time resolver flagged.
3. Generates per-community summaries over the existing Leiden communities.
4. Compresses old episodic events into life-chapter summaries.
5. Reweights edges by retrieval frequency and applies Ebbinghaus decay.
6. Derives second-order facts (e.g., "Casey lives near Pat" inferred from `LIVES_IN(same_city)` + `LIVES_IN(same_city)`).

## Motivation

`src/memory/consolidation_engine.c` runs from `src/daemon.c` and `src/app/main.c`. Today it is a one-shot dedupe + recovery pass. It is not the AutoDream / Cognee `memify` shape: there's no scheduled idle window, no second-order derivation, no summary generation. `hu_graph_leiden_communities` and `hu_graph_build_communities` already cluster; `hu_graph_query_temporal` already reads time ranges; `hu_episodic_*` (`include/human/memory/episodic.h`) already captures contact-scoped narratives. None of these get woven into a periodic refinement pass.

Effects:

- W1's quarantined writes never get reviewed — they sit in `quarantine_relations` forever.
- Communities never get summarized — Microsoft GraphRAG-style "global" questions ("what's my year been like") fan out across thousands of facts instead of reading one paragraph per community.
- Old episodes never compress — DB grows unbounded; lost-in-the-middle attention degradation hits long retrievals.

## Prior art

- Claude Code AutoDream (Feb 2026) — background sub-agent reviews + consolidates `CLAUDE.md` between sessions.
- Cognee `memify` — prunes stale nodes, reweights edges by usage frequency, adds derived facts.
- Microsoft GraphRAG — community detection + community summaries; outperforms vector RAG on global questions.
- Existing prior plan: `docs/plans/2026-03-21-elastic-memory-episodic.md` proposes episodic pattern compression. W2 supersedes the consolidation portion of that plan and references it for pattern-shape guidance.

## Design

### 1. AutoDream subagent

New module `src/agent/autodream.c` + `include/human/agent/autodream.h`.

```c
typedef struct hu_autodream_config {
    int64_t idle_threshold_secs;       /* default: 1800 (30 min) */
    int64_t min_interval_secs;         /* default: 14400 (4 hours) */
    int64_t max_runtime_secs;          /* default: 300 (5 min budget) */
    bool dry_run;                      /* test/debug — no DB writes */
    bool enable_llm_resolver;          /* gated by HU_ENABLE_LLM_CONFLICT */
    bool enable_summary_generation;    /* gated by HU_ENABLE_AUTODREAM */
    bool enable_derived_facts;         /* gated by HU_ENABLE_AUTODREAM */
} hu_autodream_config_t;

typedef struct hu_autodream_report {
    int64_t started_at;
    int64_t finished_at;
    size_t quarantine_reviewed;
    size_t quarantine_released;
    size_t quarantine_dropped;
    size_t conflicts_resolved_by_llm;
    size_t communities_summarized;
    size_t episodes_compressed;
    size_t edges_reweighted;
    size_t derived_facts_added;
    bool budget_exceeded;
} hu_autodream_report_t;

hu_error_t hu_autodream_run(
    hu_allocator_t *alloc,
    hu_graph_t *graph,
    hu_memory_t *memory,
    const hu_autodream_config_t *cfg,
    hu_autodream_report_t *out_report);
```

Driven by `src/daemon.c` — adds an idle detector that calls `hu_autodream_run` when:

```
now() - last_user_activity > cfg.idle_threshold_secs
&& now() - last_autodream_run > cfg.min_interval_secs
```

Phases (each is a separate function inside `autodream.c`, each respects the global `max_runtime_secs` budget):

1. `phase_quarantine_review` — read `quarantine_relations`; for each, run LLM resolver (if enabled) → release / drop / flag.
2. `phase_conflict_resolution` — find relations with `HU_CONFLICT_FLAG` from W1's deterministic resolver; ask LLM to choose; write decision.
3. `phase_community_summaries` — for each Leiden community, generate ≤ 200-token summary; store in new `community_summaries` table.
4. `phase_life_chapters` — group episodic events by month / contact; compress to ≤ 500-token narrative; update `life_chapters` table.
5. `phase_edge_reweight` — for each relation, apply Ebbinghaus decay (`hu_graph_retention_score` already exists); deboost edges with no recall in N days.
6. `phase_derive_facts` — run a small set of inference rules (transitivity for `LIVES_IN`, `WORKS_AT`); write derived edges with `provenance = "autodream-derived"`.

### 2. Community summaries

```sql
CREATE TABLE IF NOT EXISTS community_summaries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL DEFAULT '',
    community_id INTEGER NOT NULL,
    summary_text TEXT NOT NULL,
    entity_count INTEGER NOT NULL DEFAULT 0,
    edge_count INTEGER NOT NULL DEFAULT 0,
    generated_at INTEGER NOT NULL,
    schema_version INTEGER NOT NULL DEFAULT 1,
    UNIQUE(contact_id, community_id)
);
```

Written via a new `hu_summary_writer_t` vtable so the in-tree default uses the configured provider but tests can stub it:

```c
typedef struct hu_summary_writer_vtable {
    hu_error_t (*generate)(void *ctx, hu_allocator_t *alloc,
                           const char *input, size_t input_len,
                           size_t max_tokens,
                           char **out, size_t *out_len);
} hu_summary_writer_vtable_t;

typedef struct hu_summary_writer {
    void *ctx;
    const hu_summary_writer_vtable_t *vtable;
} hu_summary_writer_t;
```

This is the only new vtable W2 introduces. Reuses the existing `hu_provider_t` under the hood for default impl.

Retrieval changes (in `src/memory/retrieval/qmd.c` / `src/memory/retrieval/engine.c`): when QMD classifies a query as "global / what's my year been like," prefer community summaries over raw fact fan-out.

### 3. Life chapters

```sql
CREATE TABLE IF NOT EXISTS life_chapters (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL DEFAULT '',
    chapter_label TEXT NOT NULL,    /* e.g. "2025-Q3", "the move-to-Austin" */
    summary_text TEXT NOT NULL,
    event_start INTEGER NOT NULL,
    event_end INTEGER NOT NULL,
    episode_count INTEGER NOT NULL,
    generated_at INTEGER NOT NULL
);
```

`life_chapters.c` already exists in the tree (per `src/memory/CLAUDE.md`); W2 wires it to the AutoDream cycle and gives it a backing table.

### 4. Configuration

Adds to `~/.human/config.json`:

```json
{
  "memory": {
    "autodream": {
      "enabled": true,
      "idle_threshold_secs": 1800,
      "min_interval_secs": 14400,
      "max_runtime_secs": 300,
      "enable_llm_resolver": true,
      "enable_summary_generation": true,
      "enable_derived_facts": false
    }
  }
}
```

`enable_derived_facts` defaults off until LoCoMo measures whether second-order inference helps or hurts.

CLI: `human autodream --dry-run` triggers a one-shot run for inspection; `human autodream --report` shows the last `hu_autodream_report_t`.

## File map

| File | Role |
|------|------|
| `include/human/agent/autodream.h` | New — public API, config, report struct |
| `src/agent/autodream.c` | New — phases, idle scheduler integration |
| `include/human/memory/summary_writer.h` | New — `hu_summary_writer_t` vtable |
| `src/memory/summary_writer.c` | New — default provider-backed impl |
| `src/memory/community_summary.c` | New — summary table CRUD + retrieval integration |
| `src/memory/life_chapters.c` | Existing — extended to write `life_chapters` table on autodream cycle |
| `src/memory/graph.c` | Add `quarantine_relations`, `community_summaries` schema migrations |
| `src/memory/retrieval/qmd.c` | Route global queries to community summaries |
| `src/memory/retrieval/engine.c` | Read summaries when QMD signals |
| `src/daemon.c` | Idle detector + invocation; persist `last_autodream_run` |
| `src/app/main.c` | `human autodream` subcommand |
| `src/config/config.c` | Parse new config block |
| `include/human/config.h` | Config struct fields |
| `tests/test_autodream.c` | New — phases under `HU_IS_TEST` with stub summary writer |
| `tests/test_community_summary.c` | New — CRUD + retrieval-routing |
| `tests/test_summary_writer.c` | New — vtable wiring + stub fallback |
| `eval_suites/global_questions.json` | New — 10-task suite for "what's my year been like" shape |

## Test strategy

- AutoDream phases run independently and stop on budget exceeded.
- Stub `hu_summary_writer_t` returns deterministic text; assert summaries written and retrievable.
- Idle detector: simulate 31 min of no activity, assert run; simulate 2nd run within `min_interval_secs`, assert skipped.
- Quarantine review: insert 3 quarantined writes, run autodream with stub LLM, assert one of each (released / dropped / flagged) outcome.
- Community summary retrieval: insert a "global" query, assert routed via QMD to summaries not raw facts.
- ASan clean.

## Success criteria

- All existing tests still pass.
- `tests/test_autodream.c`, `tests/test_community_summary.c`, `tests/test_summary_writer.c` all green.
- 30-day-old fact recall accuracy maintained at ≥ baseline (no regression while DB pruning runs).
- Community summary suite: ≥ 70% pass rate on stub-judge eval.
- Binary size delta: < 60 KB. AutoDream phases gated behind `HU_ENABLE_AUTODREAM` so minimal builds skip the cost.

## Risks

| Risk | Mitigation |
|------|------------|
| Subagent eats CPU on user devices | `max_runtime_secs` budget; idle-only schedule; CI perf test |
| LLM summary calls drain provider budget | `enable_summary_generation` toggle; only on idle; record token cost in report |
| Derived facts hallucinate | Default off; only deterministic transitivity rules in W2; LLM derivation deferred |
| Quarantine never gets reviewed (autodream never fires) | `human autodream --dry-run` for manual trigger; W4's UI surfaces quarantine count |

## Open questions

1. Should AutoDream phases be parallel or strictly sequential? Recommendation: sequential for W2 (simpler, deterministic, debuggable); parallelize in a later workstream if profiling shows budget pressure.
2. What's the canonical chapter granularity — month, season, year, semantic-event-cluster? Recommendation: month for v1, semantic clustering in W3 once multi-graph topology lands.

## References

- AutoDream (Claude Code release notes, Feb 2026)
- Cognee `cognify` / `memify`: cognee.ai docs
- Microsoft GraphRAG: research.microsoft.com/en-us/projects/graphrag/
- Project prior work: `docs/plans/2026-03-21-elastic-memory-episodic.md`
