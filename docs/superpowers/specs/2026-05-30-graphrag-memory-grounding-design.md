---
title: GraphRAG Memory-Grounding for the Local-Model Prompt — Design
date: 2026-05-30
status: approved-design
roadmap: "SOTA-4 / #19"
branch: feat/graphrag-grounding
---

# GraphRAG Memory-Grounding for the Local-Model Prompt — Design

- **Date:** 2026-05-30
- **Branch:** `feat/graphrag-grounding` (worktree off `origin/main` @ 43ebd971)
- **Roadmap item:** #19 (SOTA-4) — the "superhuman recall" lever
- **Status:** Approved design, pre-implementation

## 1. Background & the corrected premise

The roadmap framed #19 as *"h-uman has the richest memory layer but it's NOT feeding the
local-model generation prompt — wire it in."* Verification against the code shows that
framing is **imprecise**, and the corrected picture narrows the work substantially:

- **Flat RAG is already wired.** `hu_memory_loader_load` (keyword/semantic/hybrid over
  SQLite-FTS, with an adaptive strategy learner) populates `hu_prompt_config_t.memory_context`
  (`include/human/agent/prompt.h:26`), which `prompt.c` appends into the system prompt POSTed
  to the local MLX server. Streaming path: `src/agent/agent_stream.c:490` (retrieval) →
  `:1187` (config) → `:1256` (`hu_prompt_build_system`) → `:1575` (`stream_chat`). Non-stream
  mirror: `src/agent/agent_turn.c:1461`.
- **The actual graph layer is computed and then discarded.** `src/agent/autodream.c` builds
  per-contact `community_summaries` (entities + relationships + community detection — the
  PersonaAgent / Microsoft-GraphRAG pattern) on a schedule, but
  `hu_autodream_read_community_summary` is called **only inside autodream.c**. Nothing in the
  prompt path, loader, or retrieval engine reads those summaries back.
- **A graph traversal exists but is demoted to a fallback.** `hu_w12_planner_recall`
  (multi-hop PageRank traversal, bound via the loader's `facade`) only fires when flat
  retrieval returns **zero** results (`src/agent/memory_loader.c`, the `if (count == 0 &&
  loader->facade)` branch). Query-relevant graph recall is built and wired, but structurally a
  last resort rather than a first-class signal.

So the real #19 gap is: **surface the two existing-but-unsurfaced graph signals into the
generation prompt** — not "build GraphRAG." The substrate exists; the read-into-prompt hop is
missing.

### Verified plumbing facts

- **`session_id == contact_id`.** `src/daemon.c:1759` sets `agent->memory_session_id =
  cp->contact_id`; `include/human/daemon_proactive.h:158` documents the equality. The loader
  already holds the key needed to fetch a contact's summaries.
- **Summary read needs no new handle.** The loader reaches the SQLite db via
  `hu_sqlite_memory_get_db` (already used in its strategy-learner block). `community_summaries`
  is keyed by `contact_id` with columns `community_id, summary_text, entity_count, edge_count,
  generated_at, schema_version` (`src/agent/autodream.c:55`). A single
  `SELECT … WHERE contact_id=? ORDER BY (entity_count+edge_count) DESC LIMIT N` fetches them.
- **A real prompt-budget system exists.** `hu_prompt_budget` with per-field DEAD-field
  trimming and a `field_allowlist` that keeps `memory_context`
  (`include/human/agent/prompt.h:154-198`, `include/human/config.h:352`). A new field must
  register in that field enum to be budgeted/trimmed as a first-class citizen.

## 2. Goals / non-goals

**Goals**
- Inject **community summaries** ("who this person is / our interaction patterns" — the global
  half of PersonaAgent) into the system prompt.
- Promote **query-relevant graph traversal** (`hu_w12_planner_recall`) to a first-class signal
  that runs alongside flat RAG and merges (the local-search half).
- Keep both signals independently budgeted, measurable, and flag-gated for safe rollout and a
  future blind-A/B.

**Non-goals**
- No new graph-construction or community-detection work (autodream already does this).
- No change to the flat-RAG retrieval algorithm itself.
- No change to cloud/other providers' behavior beyond what falls out of the shared prompt path.
- Increment 2's ranking-fusion tuning is *enabled* here but its calibration is its own follow-up.

## 3. Architecture (Approach A)

A new **grounding orchestrator** in the memory-loader path produces a distinct `graph_context`
string that becomes a **new sibling field** of `memory_context` in `hu_prompt_config_t`. Flat
RAG is untouched. Behavior is gated by one env flag, `HU_GRAPH_GROUNDING`, with three states:

| State    | Behavior |
|----------|----------|
| `off` (default) | New call skipped entirely; prompt **byte-identical** to today. |
| `shadow` | Compute `graph_context` + log its size/latency; inject **nothing**. |
| `on`     | Inject `graph_context` into the prompt. |

This mirrors the project's existing `HU_SALIENCE_SHADOW` / `HU_SALVAGE_RUNAWAY` rollout
discipline.

## 4. Components & interfaces

### 4.1 `hu_graph_ground_load` (new — `src/agent/graph_grounding.c` + `include/human/agent/graph_grounding.h`)
```c
/* Best-effort: on any error, *out_graph_context = NULL, len = 0, returns HU_OK. */
hu_error_t hu_graph_ground_load(
    hu_allocator_t *alloc,
    hu_memory_loader_t *loader,        /* for db handle + W7 facade */
    const char *contact_id, size_t contact_id_len,
    const char *query, size_t query_len,
    const hu_graph_ground_budget_t *budget,
    char **out_graph_context, size_t *out_graph_context_len);
```
Internally:
- **`read_contact_community_summaries`** — `SELECT summary_text, entity_count, edge_count FROM
  community_summaries WHERE contact_id=? ORDER BY (entity_count+edge_count) DESC LIMIT N`.
  (Increment 1.)
- **`query_relevant_graph_recall`** — calls `hu_w12_planner_recall` as a first-class call (not
  the zero-result fallback) when `loader->facade` is present. (Increment 2.)
- **merge + dedup + budget-trim** → one `graph_context` markdown blob.

### 4.2 `hu_prompt_config_t` (modify — `include/human/agent/prompt.h`)
Add `const char *graph_context; size_t graph_context_len;`. Register a corresponding field in
the `prompt_budget` field enum (`include/human/agent/prompt_budget.h`) and add it to the
`field_allowlist` so it is never trimmed below the floor.

### 4.3 `prompt.c` (modify — `src/agent/prompt.c`)
Append `graph_context` in two logical spots: **community summaries near the identity block**
(stable context), **query-relevant recall adjacent to the existing `memory_context` recall
section**. Gated by the same field-wrapping the budget system already applies to
`memory_context` (`prompt.c:256`).

### 4.4 Call sites (modify — `src/agent/agent_stream.c`, `src/agent/agent_turn.c`)
After the existing `hu_memory_loader_load` call, when `HU_GRAPH_GROUNDING != off`, call
`hu_graph_ground_load` and set the new config fields (next to `:1187` / `:1461`).

## 5. Data flow

```
agent_stream/agent_turn
  → hu_memory_loader_load        → memory_context      (flat RAG, unchanged)
  → hu_graph_ground_load (NEW)   → graph_context       (community summaries + W12 recall)
  → hu_prompt_config_t { memory_context, graph_context }
  → hu_prompt_build_system       → system prompt
  → provider.stream_chat         → POST mlx_local
```
Flag `off` ⇒ the NEW line is skipped ⇒ identical to today.

## 6. Budget split (defaults)

- Community summaries: cap ~600 chars (top 2–3 communities by entity+edge count).
- Query-relevant graph recall: shares the recall budget, merged-by-score with flat RAG so the
  **combined** recall section respects the existing `prompt_budget`.
- **Flat RAG keeps priority**; graph signals are additive within remaining budget and never
  push flat RAG below its current floor.

## 7. Error handling — fail-open everywhere

Every new read is best-effort: missing `community_summaries` table, empty graph, `hu_w12`
error, or zero budget ⇒ `graph_context` empty ⇒ prompt is exactly today's. No new failure can
break a reply. Matches the loader's existing "retrieval error → fall back to v1 recall" posture
(`memory_loader.c`).

## 8. Test plan (TDD; suite is `./build/human_tests`, 13k+ tests)

- **Off = identical (golden):** flag `off` yields a byte-identical prompt to baseline for a
  contact with a populated graph.
- **Summaries read (unit):** ordering by entity+edge count, `LIMIT`, empty-table fail-open.
- **On = present:** flag `on` ⇒ community-summary text + graph recall appear in the assembled
  prompt for a seeded graph fixture.
- **Budget:** graph signals never push flat RAG below its floor; combined recall respects
  `prompt_budget`.
- **Fail-open:** W12 error / empty graph ⇒ empty `graph_context`, reply unaffected.
- **Shadow:** shadow mode logs sizes but injects nothing (prompt identical to off).

Build: `cmake --preset dev && cmake --build --preset dev`. Run a focused test via
`./build/human_tests --filter=<name>`.

## 9. Staging

- **Increment 1 — community summaries:** the new field + budget registration, the 3-state flag,
  `read_contact_community_summaries`, prompt placement near identity, tests. Low risk; immediate
  "who they are" grounding. Ships behind the flag (default `off`).
- **Increment 2 — promote + fuse graph traversal:** first-class `hu_w12_planner_recall`,
  merge/dedup/rank with flat RAG, latency verification (no tok/s regression). Touches the hot
  retrieval path; verified separately.

## 10. Success criteria

- Flag `off` is a verified no-op (golden test).
- Flag `on` measurably injects both signals for a populated contact.
- `/verify` PASS on the new tests **and** the full suite (per quality-gates: full suite, not
  changed-files only).
- No latency regression beyond the budget on increment 2 (verify against the live server, not
  just the eval scoreboard — per the project's "curl the server before trusting an eval number"
  rule).
- Then eligible for the blind-A/B harness (`scripts/blind_ab/`).

## 11. Risks / open questions

- **R1 — empty graphs are the common case.** Many contacts may have no `community_summaries`
  yet (autodream runs on a schedule). Fail-open covers correctness, but the *value* of
  increment 1 depends on populated graphs. Mitigation: pick a known-populated contact for the
  on-test; measure coverage (how many contacts have summaries) before claiming impact.
- **R2 — increment 2 latency.** Promoting W12 from fallback to per-turn adds graph traversal to
  the hot path at ~20 tok/s. Must verify against the live server; keep behind the flag until
  measured.
- **R3 — prompt-cache invalidation.** The server reports `lm_prompt_cache_active` with
  `cached_system_tokens`. Injecting per-contact graph context changes the system prefix and may
  reduce cache hits. Measure `avg_tok_per_sec` and cache state on/off before promoting `on`.
- **OQ1 — contact_id edge cases.** Confirm group-chat / multi-identity sessions still map
  cleanly to a `contact_id` for the summary read (the `hu_contact_identity` resolver in
  `contact_graph.h` may matter for groups).
