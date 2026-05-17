# Design for US-3.3: Daemon wires embedder and vector store into agent at init

## TL;DR — Scope Pivot

**The original framing of US-3.3 ("wire the daemon to create embedder + vector_store when `hybrid_recall=true`") is largely already implemented in `src/bootstrap.c`.** The bootstrap unconditionally creates a Gemini-or-local embedder and an in-memory vector store, attaches them to `hu_app_ctx_t` slots, and tears them down on shutdown. The slots on `hu_app_ctx_t` already exist (`include/human/bootstrap.h:36-37`).

This pivots US-3.3 from **"build wiring"** to **"audit + flag-gate the existing wiring, populate it from `memory.db`, and prove cold-path safety when disabled"**. The risk profile shifts: the existing code does unconditional work that costs allocations, embedder construction, and (after backfill is added) potentially hundreds of TF-IDF embed calls — on every daemon start, for every user, regardless of whether hybrid recall is enabled. **The flag-gate is now the headline value, not a checkbox.**

---

## Pre-existing wiring (the discovery)

Citations are from this worktree at the time of design.

| What | Where | Status |
|---|---|---|
| `hu_app_ctx_t.embedder` slot (void* to `hu_embedder_t`) | `include/human/bootstrap.h:36` | Exists |
| `hu_app_ctx_t.vector_store` slot (void* to `hu_vector_store_t`) | `include/human/bootstrap.h:37` | Exists |
| Internal storage struct fields (`hu_embedder_t embedder; hu_vector_store_t vector_store;`) | `src/bootstrap.c:383-384` (inside `hu_bootstrap_internal_t`) | Exists |
| Gemini-or-local embedder creation | `src/bootstrap.c:871-879` | **Unconditional** (gated only on `GEMINI_API_KEY` env, falls back to local) |
| In-memory vector store creation | `src/bootstrap.c:880` | **Unconditional** |
| Retrieval engine wired w/ embedder + store | `src/bootstrap.c:881-882` | Unconditional |
| Pointers published to `app_ctx` for callers | `src/bootstrap.c:883-885` | Unconditional |
| Skill-route embedder hooked to agent | `src/bootstrap.c:1014` (`hu_agent_set_skill_route_embedder`) | Unconditional |
| Teardown of retrieval / vector_store / embedder | `src/bootstrap.c:1864-1869` | Exists (NULL-guarded by vtable check) |

Additionally, the `hu_agent_t` struct has **no permanent embedder/vector_store slots beyond `skill_route_embedder`** (`include/human/agent.h:362`, which is documented as *not owned*). That field is set to NULL on init at `src/agent/agent.c:1239`. There is no `agent->embedder` or `agent->vector_store` field today.

**Implication for US-3.1 wiring (the recall path):** the daemon must pass the embedder/vector_store from `app_ctx` through to `hu_memory_recall_for_contact`. There are two natural carriers:

- **Option A (preferred): add `hu_embedder_t *embedder` and `hu_vector_store_t *vector_store` pointer fields to `hu_agent_t`, set by a new `hu_agent_set_hybrid_retrieval(...)` setter mirroring the existing `hu_agent_set_skill_route_embedder` pattern (`src/agent/agent.c:844`).** These are NOT owned by the agent — same ownership model as `skill_route_embedder` — so `hu_agent_deinit` does not free them. The agent_turn.c call site at `src/agent/agent_turn.c:1490` reads them off `agent->` and threads them into the new US-3.1 signature.
- Option B: pass embedder/vector_store on every turn from gateway/cli call sites. Rejected — fans out the change across many call sites and breaks the abstraction the agent is supposed to provide.

We use Option A.

---

## Approach

Three concrete deltas vs. the existing code:

1. **Add `bool hybrid_recall` to `hu_memory_config_t` (`include/human/config.h:478`, the struct ending at line 499).** Default `false`. Parse from JSON in `src/config_parse.c::parse_memory` (or wherever sibling fields like `auto_save` and `encrypt_at_rest` are parsed). Default-set in `src/config_merge.c` near the existing `cfg->memory.*` defaults block.

2. **Flag-gate the existing bootstrap block at `src/bootstrap.c:871-885`** so it runs only when `bi->cfg.memory.hybrid_recall == true`. When false, `bi->embedder`, `bi->vector_store`, and `bi->retrieval_engine` remain zero-initialized; `ctx->embedder`, `ctx->vector_store`, `ctx->retrieval` remain NULL. The existing teardown at `src/bootstrap.c:1864-1869` is already NULL-vtable-guarded so it is safe in the disabled case.

3. **Populate the vector store from `memory.db` at init** (when `hybrid_recall == true`). After `hu_vector_store_mem_create` returns, iterate scoped memory rows from the SQLite backend, batch-embed via `embedder.vtable->embed_batch`, and `vector_store.vtable->insert` each entry keyed by `session_id + ":" + row_key`. Wrap in a helper `static hu_error_t bootstrap_seed_vector_store(hu_bootstrap_internal_t *bi)` local to `bootstrap.c`. Use the `embed_batch` vtable method (see `include/human/memory/vector.h:35`) — a single call across all rows is dramatically cheaper than N single-text calls because TF-IDF vocabulary is built once.

The Gemini-vs-local choice at `bootstrap.c:871-879` is **preserved as-is** inside the flag-gated block. The story focuses on local; the existing Gemini upgrade path is orthogonal.

Two anti-options we are *not* taking:
- We are NOT persisting the vector store to disk. Open Q3 is pre-resolved: re-embed from `memory.db` at every daemon start. Acceptable given ~80 rows × 128-dim × 4 bytes < 50 KB and TF-IDF embed of ~100 short strings completes in single-digit ms locally.
- We are NOT moving ownership to `hu_agent_t`. The agent borrows pointers; bootstrap owns and deinits. This mirrors today's `bi->` storage and the comment at `include/human/agent.h:529-530`.

---

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `include/human/config.h` (line 478 struct) | Add `bool hybrid_recall;` field to `hu_memory_config_t` | +2 |
| `src/config_merge.c` (defaults block ~line 282) | Default `cfg->memory.hybrid_recall = false;` | +1 |
| `src/config_parse.c` (`parse_memory` or nearest sibling parser) | Read `memory.hybrid_recall` from JSON | +5 |
| `src/bootstrap.c:871-885` | Wrap embedder/vector_store/retrieval creation in `if (bi->cfg.memory.hybrid_recall)` block; insert seed call | +10 / -0 |
| `src/bootstrap.c:1014` | Gate `hu_agent_set_skill_route_embedder` call on `bi->cfg.memory.hybrid_recall` (or on `bi->embedder.ctx != NULL`) | +2 |
| `src/bootstrap.c` (new static fn) | `bootstrap_seed_vector_store(bi)` — iterate `memory.db`, batch-embed, insert | +60 |
| `include/human/agent.h` (struct around line 362) | Add `hu_embedder_t *retrieval_embedder; hu_vector_store_t *retrieval_vector_store;` (NOT owned) | +3 |
| `include/human/agent.h` (near line 531) | Declare `hu_agent_set_hybrid_retrieval(hu_agent_t*, hu_embedder_t*, hu_vector_store_t*)` | +3 |
| `src/agent/agent.c` (near line 844, near line 1239) | Implement setter; NULL-init the two fields in `hu_agent_init` | +10 |
| `src/bootstrap.c` (after agent created) | Call `hu_agent_set_hybrid_retrieval(&bi->agent, &bi->embedder, &bi->vector_store)` inside the flag-gated block | +3 |
| `src/agent/agent_turn.c:1490` | Pass `agent->retrieval_embedder, agent->retrieval_vector_store` into the US-3.1 expanded signature | +2 |
| `tests/test_config_parse.c` | Two test cases: explicit-true, missing-key-default-false | +30 |
| `tests/test_bootstrap.c` (or nearest) | Cold-path test: `hybrid_recall=false` → `ctx.embedder == NULL`, `ctx.vector_store == NULL`, no embedder invocation | +40 |
| `tests/test_bootstrap.c` | Allocator-failure test: failing-allocator on embedder create → daemon does not abort, hybrid effectively disabled | +30 |
| `sprints/sprint-3/evidence/3.3/` | Evidence dir with full-suite log + cold-path proof | n/a |

Estimated total: ~200 LOC across ~10 files. Risk-tier: **Medium** (touches bootstrap; not in `src/security/`).

---

## Implementation steps (for the implementer agent)

Sequence is reversible and incremental — each step compiles and the suite passes after each.

1. **Add config field, default, parser, two tests.** Land + verify. No behavior change yet because nothing reads it.
2. **Read the field in `bootstrap.c:871-885` — flag-gate the existing block.** Add the cold-path test (`hybrid_recall=false` → `ctx.embedder == NULL`). Full suite must remain green; the default is `false` so existing tests that depended on these pointers being non-NULL will surface here. Expect to fix 0–3 tests; if more, halt and surface — that's signal the bootstrap-already-creates assumption is leakier than the audit suggests.
3. **Add `retrieval_embedder` + `retrieval_vector_store` pointer fields to `hu_agent_t`. Add setter. NULL-init in `hu_agent_init`.** Compile only; no behavior change.
4. **Call setter from bootstrap (inside the flag-gated block).** Compile + cold-path test still green.
5. **Wire `agent->retrieval_embedder` / `retrieval_vector_store` into the US-3.1 call site at `src/agent/agent_turn.c:1490`.** This depends on US-3.1's signature change landing first.
6. **Implement `bootstrap_seed_vector_store`** — iterate `memory.db`, batch-embed, insert. Add the failing-allocator test (AC-3.3.5).
7. **Add evidence dump**: run full suite with default config (cold path), run integration test with `hybrid_recall=true` (hot path). Capture both logs to `sprints/sprint-3/evidence/3.3/`.
8. **Run /verify.**

Steps 1, 3 are independent and can be parallelized. 2 and 4 are sequential. Step 5 has a cross-story dependency on US-3.1.

---

## AC mapping — what is already met vs. needs new work

| AC | Status | Why |
|---|---|---|
| **AC-3.3.1** (config field added, defaults false) | **Needs new work** | `hu_memory_config_t` (`include/human/config.h:478`) does not contain `hybrid_recall`. Step 1. |
| **AC-3.3.2** (config parsed from JSON) | **Needs new work** | Step 1. |
| **AC-3.3.3** (daemon initialises embedder + vector_store when enabled) | **Partially met** — bootstrap already creates them at `src/bootstrap.c:871-880` and publishes to `app_ctx`. **Missing:** flag-gating to ONLY do this when `hybrid_recall == true`, and the seed-from-memory.db step that gives the vector store useful contents. Steps 2 + 6. |
| **AC-3.3.4** (daemon does NOT initialise when disabled) | **NOT met today** — bootstrap creates them unconditionally. This is the highest-impact part of the story. Step 2. |
| **AC-3.3.5** (init failure is non-fatal, falls back to BM25) | **Partially met** — `bi->embedder.ctx` is checked at `bootstrap.c:877` (the Gemini→local fallback) but the local create returning a zero-ctx value would currently leak into a downstream `hu_vector_store_mem_create` + retrieval engine with a broken embedder. Need an explicit guard: if either `embedder.ctx == NULL` or `vector_store.ctx == NULL`, log warn, deinit whichever did succeed, and leave both `ctx->` slots NULL. Step 6 (the test forces this path). |
| **AC-3.3.6** (memory ownership: daemon owns, agent borrows; no double-free) | **Partially met** — bootstrap already owns and tears down (`bootstrap.c:1864-1869`). The agent's existing `skill_route_embedder` slot is documented NOT-owned and is correctly skipped in `hu_agent_deinit`. The new fields (`retrieval_embedder`, `retrieval_vector_store`) follow the same NOT-owned semantics. Grep-verified: `grep -n "embedder.*deinit\|vector_store.*deinit" src/agent/agent.c` must return zero matches after step 3. |
| **AC-3.3.7** (full suite passes with default false, 0 ASan errors) | **Will be met** if step 2 surfaces zero broken tests. If existing tests assume `ctx.embedder != NULL`, those tests must either (a) set `hybrid_recall=true` in their test fixture config, or (b) be updated to tolerate NULL — preferring (a) because it preserves the cold-path safety contract. |

**Summary**: 2 of 7 ACs are partially met by existing code. 5 require new work. The most valuable new work is **AC-3.3.4** (cold-path safety), which is currently outright violated by the unconditional creation at `bootstrap.c:871-880`.

---

## Risks

### HIGHEST RISK: Cold-path regression — existing tests/callers assume unconditional embedder

**What could go wrong:** When step 2 flips the default to `hybrid_recall=false`, code paths that today silently rely on `ctx.embedder` or `ctx.vector_store` being non-NULL (skill routing at `bootstrap.c:1014`, retrieval engine consumers, any test fixture that doesn't set the new flag) start receiving NULL. Symptoms: NULL-deref crashes in the test suite, or — worse — silent feature loss in production where skill routing degrades without anyone noticing.

**Probability:** Medium-High. The bootstrap line at `bootstrap.c:1014` (`hu_agent_set_skill_route_embedder(&bi->agent, &bi->embedder)`) is the smoking gun: skill routing is a separate feature from hybrid recall but is wired off the same embedder. Flag-gating the embedder also flag-gates skill routing, which is a scope creep we did not intend.

**Impact:** Medium. Skill routing has fallback behavior (cosine route is "optional"), but losing it silently is a bad UX.

**Mitigation:**
- Preferred mitigation: flag-gate everything (`871-885`), AND introduce the gate at `hu_agent_set_skill_route_embedder` at line 1014. If `hybrid_recall=false`, skip the skill-route hookup. Document in the commit message that this couples skill routing to hybrid recall **temporarily**; if a future user wants skill routing without hybrid, we add a separate `cfg.agent.skill_routing` flag then.
- Pre-flight: run `grep -rn "ctx->embedder\|ctx->vector_store\|app_ctx\.embedder\|app_ctx\.vector_store" src/ tests/` to enumerate all readers BEFORE flipping the gate. This is the audit step.

### Performance — startup cost of seeding from memory.db

**What could go wrong:** A user with thousands of rows pays multi-second daemon startup as TF-IDF re-embeds every row at every daemon boot.

**Probability:** Low at current scale (~80 rows). Medium at 1-year-from-now scale.

**Impact:** Small-Medium. Daemon startup is not on the hot path of any user-visible latency budget, but multi-second startup hurts.

**Mitigation:**
- Use `embed_batch` not per-row `embed`. The TF-IDF vocab build amortizes across the batch (`include/human/memory/vector.h:35`).
- Cap initial seed at a configurable `cfg.memory.hybrid_recall_seed_max` (default 5000). Beyond that, defer to lazy / future US-3.4 backfill. **YAGNI exception:** add the cap field only if we observe slowness in evidence; do not add it speculatively.
- Measure: include daemon-startup time in `sprints/sprint-3/evidence/3.3/` evidence so we have a baseline.

### Memory growth — vector store unbounded

**What could go wrong:** Every stored memory adds another 128-dim × 4-byte embedding plus id/content overhead to the in-memory vector store. No eviction policy.

**Probability:** Low at current scale. Linear growth with `memory.db` size.

**Impact:** Small. <50 KB for 80 rows, ~600 KB for 10k rows. Negligible vs. the daemon's existing RSS.

**Mitigation:** None required this sprint. Note in `sprint-3/followups.md` if memory profiling at end-of-sprint shows otherwise.

### Allocator-failure path — partial init leak

**What could go wrong:** Embedder creates successfully, vector store fails to allocate (or vice versa). The partial-init state is hard to teardown correctly.

**Probability:** Low (allocator failures are rare in practice).

**Impact:** Medium (ASan failure = sprint DoD failure).

**Mitigation:** Step 6's test exercises this. The new failure handler must: deinit whichever component DID succeed, zero both fields, return success (so daemon continues), and leave `ctx->embedder = ctx->vector_store = NULL` so downstream readers see the disabled state.

### Backward compat for `hu_memory_recall_for_contact` signature

**What could go wrong:** US-3.1 changes the signature. US-3.3 inherits that risk because `agent_turn.c:1490` must pass the new pointers. If US-3.1 doesn't land first, US-3.3 step 5 is blocked.

**Probability:** Medium (cross-story dependency).

**Impact:** Small. Step 5 is the last behavior step; everything before is independent.

**Mitigation:** US-3.3 explicitly depends on US-3.1 per the story metadata. The implementer should verify the new signature is in `main` (or in their branch base) before starting step 5.

---

## Test strategy

### Cold-path test (highest priority — AC-3.3.4)

`tests/test_bootstrap.c::bootstrap_disabled_hybrid_recall_leaves_embedder_null`:
- Construct a `hu_config_t` with `cfg.memory.hybrid_recall = false`.
- Call `hu_app_bootstrap(...)`.
- Assert `ctx.embedder == NULL`.
- Assert `ctx.vector_store == NULL`.
- Assert daemon teardown succeeds with no ASan errors.

### Hot-path test (AC-3.3.3)

`tests/test_bootstrap.c::bootstrap_enabled_hybrid_recall_populates_vector_store`:
- Pre-seed a tempdir `memory.db` with 5 scoped rows (use the AC-3.2 fixture pattern).
- Construct config with `hybrid_recall = true` and `sqlite_path` pointing at tempdir.
- Bootstrap.
- Assert `ctx.embedder != NULL`, `ctx.vector_store != NULL`.
- Assert `vector_store.vtable->count(vector_store.ctx) == 5`.

### Allocator-failure test (AC-3.3.5)

`tests/test_bootstrap.c::bootstrap_embedder_alloc_failure_falls_back_safe`:
- Install a failing allocator that returns NULL on the Nth call sized for embedder ctx.
- Bootstrap with `hybrid_recall = true`.
- Assert bootstrap returns `HU_OK` (non-fatal).
- Assert `ctx.embedder == NULL` AND `ctx.vector_store == NULL` (both off).
- Assert no leaks (ASan).

### Config parse tests (AC-3.3.1, AC-3.3.2)

`tests/test_config_parse.c`:
- `config_parse_hybrid_recall_explicit_true` — input `{"memory":{"hybrid_recall":true}}` → `cfg.memory.hybrid_recall == true`.
- `config_parse_hybrid_recall_default_false` — input `{"memory":{}}` → `cfg.memory.hybrid_recall == false`.

### No-regression (AC-3.3.7)

- `./build/human_tests` full suite, 0 failures, 0 ASan errors.
- Captured to `sprints/sprint-3/evidence/3.3/full-suite.log`.

---

## Observability

- On bootstrap with `hybrid_recall=true`: log `hu_log_info("human", "bootstrap", "hybrid recall enabled: seeded vector store with %zu entries", count)`.
- On allocator-failure path: log `hu_log_warn("human", "bootstrap", "hybrid recall disabled at runtime: embedder/vector_store init failed")`.
- On bootstrap with `hybrid_recall=false`: no log (cold path stays cold; logging would be noise).

These logs are the field-deployment signal that lets us tell, from a user's daemon log, which retrieval mode is active.

---

## Decision points the implementer must NOT make alone

- Coupling skill routing (`bootstrap.c:1014`) to `hybrid_recall` flag: **flagged as a risk above; the implementer should follow the recommended mitigation (couple temporarily, document, file a follow-up if needed). Do not invent a new `skill_routing` config field this sprint** — that's YAGNI scope creep.
- Persistent vector store: out of scope per Open Q3. If the implementer is tempted to write one, file a follow-up instead.
- Changing the embedder's vocabulary lifecycle (e.g., incremental TF-IDF updates as new memories arrive): out of scope. Sprint 3 only seeds at boot.

---

## Last line

`RESULT_tech-lead=READY`
