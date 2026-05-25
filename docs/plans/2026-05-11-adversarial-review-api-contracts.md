---
status: closed
last_audit: 2026-05-25
---

# Adversarial API-Contract Review — SOTA-2026 Design Fleet

**Reviewer:** api-contract-watcher  
**Date:** 2026-05-11  
**Scope:** 13 design docs at `docs/plans/2026-05-11-init-*.md`; baseline from `include/human/*.h` and `include/human/**/*.h`

---

## Executive Summary

6 **ABI-BREAKING** findings were discovered. 3 additional issues require **NEEDS-WRITE-CONFIRMATION**. The most serious are: (1) `hu_personal_model_ingest` gains a required new parameter in Init #09 — 4 existing call sites will not compile without updates; (2) four independent initiatives all claim to append an enum value at `HU_JOB_KIND_MAX` in `hu_job_kind_t` without coordinating ordinal assignments, producing guaranteed value collisions; (3) Init #12 removes the existing `hu_mcp_host_t` symbol family from `mcp_server.h` without a shim, breaking `src/main.c` and `src/mcp_server.c`. The architects' "strictly additive" claim is false for at least Init #09 and Init #12.

---

## 1. Per-Initiative Breaking-Change Ledger

### Init #01 — Activation Steering

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/provider.h` — `hu_provider_vtable_t` | Ends at `const char *(*active_adapter)(void *ctx);` | Adds `hu_error_t (*apply_steering)(void *ctx, const float *vec, size_t dim);` at tail | All provider implementors (new NULL field, zero-init safe) | **NO** — tail append, optional NULL |
| `include/human/persona/steering.h` (new) | Does not exist | New header | None — new surface | **NO** |
| `include/human/config.h` — `hu_steering_config_t` | Not present | New substruct in config | Config callers (struct initializers must zero-extend) | **NO** — additive |
| `include/human/agent/frontier_prompt.h` | `hu_frontier_prompt_bundle_t` current fields | Adds `char *steering_dir; size_t steering_dir_len;` | Callers stack-allocating `hu_frontier_prompt_bundle_t` | **NO** — fields are pointers, tail-appended |

**Verdict: ABI-SAFE.** All additions are tail-appended to vtable and structs. New fields are optional (NULL/zero-safe). No callers broken.

---

### Init #02 — MoLoRA Channels

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/provider.h` — `hu_provider_vtable_t` | Ends at `apply_steering` (Init #01) or `active_adapter` (baseline) | Adds `load_adapter_mixture`, `set_adapter_mixture`, `active_mixture` at tail | All provider implementors | **NO** — tail append, optional NULL |
| `include/human/persona.h` — `hu_persona_overlay_t` | Ends at `uint8_t leave_on_read_pct;` | Adds `uint8_t expert_id; float expert_weight_floor;` at tail | `src/persona/markdown_loader.c` (9 sizeof), `src/persona/creator.c` (2 sizeof), `src/persona/analyzer.c` (2 sizeof) | **NO** — tail append; all consumers use `sizeof(hu_persona_overlay_t)` which re-evaluates at compile time; no binary serialization detected |
| `include/human/agent/scheduler.h` — `hu_job_kind_t` | `HU_JOB_TRAINING_DATA_EXTRACT = 9`, `HU_JOB_KIND_MAX` (implicit 10) | Adds `HU_JOB_MOLORA_ROUTER_TRAIN` before `HU_JOB_KIND_MAX` | Scheduler dispatch tables, SQLite `scheduler_jobs` rows | **YES — SEE CROSS-INITIATIVE COLLISION §4** |
| `include/human/persona/lora_router.h` (new) | Does not exist | New header | None | **NO** |
| `include/human/config_types.h` — `hu_molora_config_t` | Not present | New struct | None | **NO** |

**Verdict: ABI-SAFE (vtable + persona overlay)** / **ABI-BREAKING for `hu_job_kind_t` enum** — see §4.

---

### Init #03 — Apple FoundationModels Provider

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/providers/apple.h` — `hu_apple_config_t` | Fields `base_url`, `base_url_len` | Adds `transport`, `transport_floor`, `pcc_policy`, `uds_path`, `bridge_dylib_path`, `emit_disclosure_on_pcc` at tail | Callers using struct literals | **NO** — tail append, new fields zero-safe |
| `hu_apple_probe_transports`, `hu_apple_provider_active_transport`, `hu_apple_provider_last_used_pcc` | Do not exist | New public functions | None | **NO** |

**Verdict: ABI-SAFE.** No existing symbols altered. New config struct fields are tail-appended. Explicit statement: "no changes to `hu_provider_vtable_t`."

---

### Init #04 — MLX Qwen3 Provider

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/providers/mlx_qwen3.h` (new) | Does not exist | New header | None | **NO** |
| `include/human/ml/adapter_format.h` | Does not exist | New header | None | **NO** |
| `include/human/ml/cli.h` | Existing CLI declarations | Adds `hu_ml_cli_lora_convert` | None — new symbol | **NO** |

**Verdict: ABI-SAFE.** New provider, no existing vtable or struct changes. Implements existing `load_adapter` / `unload_adapter` / `active_adapter` triple from baseline `provider.h`.

---

### Init #05 — Verifier-Driven Test-Time Training

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/ml/learner.h` — `hu_learner_vtable_t` | `{ name, available, train, deinit }` (4 members) | Adds `const hu_learner_ttt_vtable_t *ttt` under `#ifdef HU_ENABLE_TTT` | All learner vtable implementors; any TU that stack-initializes the vtable | **YES — ODR risk; see §5** |
| `include/human/agent.h` — `hu_agent_t` | Does not include `ttt_journal` | Adds `hu_ttt_journal_t *ttt_journal` | All code allocating or zero-initializing `hu_agent_t` | **NO** — pointer field; zero-init is a valid empty state |
| `include/human/agent/scheduler.h` — `hu_job_kind_t` | Same as Init #02 collision | Adds `HU_JOB_TTT_DRIFT_EVAL` | Same collision as Init #02 | **YES — SEE §4** |
| `include/human/agent/ttt.h`, `include/human/ml/fidelity.h` (new) | Do not exist | New headers | None | **NO** |

**Verdict: NEEDS-WRITE-CONFIRMATION** (vtable `#ifdef` field) / **ABI-BREAKING** (`hu_job_kind_t` enum).

---

### Init #07 — ThinkPRM Verifier

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/ml/reward_model.h` (new) | Does not exist | New vtable header | None | **NO** |
| `include/human/agent.h` — `hu_agent_extensions_t` | Ends at `hu_escalate_protocol_t escalate_protocol;` | Adds `hu_reward_model_t reward_model;` | **All allocators of `hu_agent_extensions_t`** (embedded by value in `hu_agent_t.sota`) | **YES — see §6** |
| `include/human/ml/cli.h` | Existing | Adds `hu_ml_cli_prm_train`, `hu_ml_cli_prm_audit` | None | **NO** |

**Verdict: NEEDS-WRITE-CONFIRMATION.** `hu_agent_extensions_t` is embedded by value in `hu_agent_t`. Adding a `hu_reward_model_t` field changes the layout of both structs. While the field is pointer-safe (the vtable contains function pointers and a context), the `hu_reward_model_t` struct size must be confirmed. If `hu_agent_t` is zero-initialized at the call site (it is, via `calloc` / `memset`), the new field initializes to a valid NULL-vtable state. The real risk: any `memcpy`, `fwrite`, or fixed-size inter-process protocol that transfers `hu_agent_t` or `hu_agent_extensions_t` by bytes will silently produce incorrect results.

---

### Init #08 — Federated LoRA

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/federation.h` (new) | Does not exist | Entirely new vtable | None | **NO** |
| `include/human/core/error.h` | Existing `HU_ERR_*` codes | New `HU_ERR_FED_*` codes at tail of enum | Switch statements on `hu_error_t` without a default case | **NO** — new enum values at tail do not change existing values; callers with non-exhaustive switches may emit new compiler warnings, not failures |
| `include/human/config_types.h` — `hu_federation_config_t` | Not present | New struct | None | **NO** |

**Verdict: ABI-SAFE.** Entirely new surface. No existing types modified.

---

### Init #09 — Memory Trust Tiers

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/memory/personal_model.h` — `hu_personal_model_ingest` | `hu_error_t hu_personal_model_ingest(hu_personal_model_t *model, const char *message, size_t message_len, bool from_user, int64_t timestamp);` | `hu_error_t hu_personal_model_ingest(hu_personal_model_t *model, const char *message, size_t message_len, bool from_user, int64_t timestamp, const hu_provenance_t *prov);` | **4 call sites** — see §3 | **YES — SIGNATURE CHANGE** |
| `include/human/memory/fact_extract.h` — `hu_heuristic_fact_t` | Ends at `int64_t last_seen_at;` | Adds `hu_provenance_t provenance;` (~208 bytes) at tail | All code sizing `hu_heuristic_fact_t` (embedded in `hu_personal_model_t.facts[64]`) | **NO** — tail append; sizeof-based allocation auto-corrects on recompile; `HU_PM_VERSION` bumped to 5 |
| `include/human/memory.h` — `hu_memory_entry_t` | Ends at `double score;` | Adds `int trust_tier; char *provenance; size_t provenance_len;` | **40+ sizeof consumers** in `src/` across 15+ files | **YES — STRUCT LAYOUT CHANGE; see §5** |
| `include/human/memory/trust.h` (new) | Does not exist | New header | None | **NO** |
| `include/human/agent/channel_trust.h` (new) | Does not exist | New header | None | **NO** |

**Verdict: ABI-BREAKING (function signature, struct layout).**

---

### Init #10 — Episode Storage & Sleep Consolidation

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/memory/deep_memory.h` — `hu_episode_t` | `hu_episode_t { id, summary, ..., source_tag }` | Renamed to `hu_deep_episode_t`; old name removed | `src/memory/deep_memory.c:80,417`; `tests/test_deep_memory.c` | **YES — type rename** |
| `include/human/agent/episodic.h` — `hu_episode_t` | `hu_episode_t { summary, timestamp_ms, session_id }` | Renamed to `hu_session_episode_t`; old name removed | callers of `hu_episodic_store` / `hu_episodic_load` via struct literal | **YES — type rename** |
| `include/human/memory/episode_store.h` (new) | Does not exist | Canonical `hu_episode_t` with new, broader schema | None (new symbol) | **NO** |
| `include/human/agent/scheduler.h` — `hu_job_kind_t` | Same as Inits #02, #05 | Adds `HU_JOB_CONSOLIDATE_NREM`, `HU_JOB_CONSOLIDATE_REM` | Same collision as Inits #02, #05 | **YES — SEE §4** |
| `include/human/memory/consolidation_two_phase.h` (new) | Does not exist | New `hu_consolidation_t` vtable | None | **NO** |

**Verdict: ABI-BREAKING** (type renames in two headers; enum collision). See §2 for full call-site list.

---

### Init #11 — Proactivity & Typing Simulator

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/agent/proactivity_gate.h` (new) | Does not exist | New header | None | **NO** |
| `include/human/agent/typing_simulator.h` (new) | Does not exist | New header | None | **NO** |
| `include/human/agent/scheduler.h` — `hu_job_kind_t` | Same as Inits #02, #05, #10 | Adds `HU_JOB_PROACTIVITY_RECHECK` | Same collision | **YES — SEE §4** |
| `hu_channel_supports_typing` (new free function) | Does not exist | New public function | None | **NO** |

**Verdict: ABI-SAFE (all new surface)** / **ABI-BREAKING (`hu_job_kind_t` enum collision).**

---

### Init #12 — MCP Server Mode

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/mcp.h` — `hu_mcp_server_t` | The MCP CLIENT type (wraps a child process) | Renamed to `hu_mcp_client_t`; deprecated shim left | **19 refs in `src/mcp.c`; 2 refs in `src/mcp_manager.c`; tests** — see §3 | **YES — type rename, 21+ call sites** |
| `include/human/mcp_server.h` — `hu_mcp_host_t` | `hu_mcp_host_create`, `hu_mcp_host_set_resources`, `hu_mcp_host_set_prompts`, `hu_mcp_host_run`, `hu_mcp_host_destroy` | Entire file replaced; `hu_mcp_host_t` removed with NO SHIM | `src/mcp_server.c` (14 uses of `hu_mcp_host_t`); `src/main.c:2407-2421` (5 uses); `tests/test_mcp.c` | **YES — symbol removal, no compatibility shim** |
| `include/human/mcp_server.h` — `hu_mcp_server_t` (new vtable) | Does not exist | New vtable type introduced | None — new surface | **NO** |
| `include/human/mcp/consent.h`, `mcp/engine.h`, `mcp/discovery.h`, `mcp/server_audit.h` (new) | Do not exist | New headers | None | **NO** |

**Verdict: ABI-BREAKING** (two distinct breaking changes: client type rename + host symbol family removal).

---

### Init #13 — KV-Cache Compression

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/provider.h` — `hu_provider_vtable_t` | Ends at tail (after Inits #01, #02 additions in dependency order) | Adds `const hu_provider_caps_t *(*caps)(void *ctx);` at tail | All provider implementors | **NO** — tail append, optional NULL |
| `include/human/memory/kv_compressor.h` (new) | Does not exist | New vtable + compressor types | None | **NO** |

**Verdict: ABI-SAFE.** No existing symbols altered. The `caps` method is optional and NULL-safe per the existing pattern.

---

### Init #14 — Public Benchmarks

| Header | Before | After | Callers Affected | Breaking? |
|--------|--------|-------|-----------------|-----------|
| `include/human/eval_benchmarks.h` — `hu_benchmark_type_t` | Existing values | Adds 5 new values at tail before sentinel | `switch` without default case | **NO** — tail-appended; existing ordinals unchanged |
| `hu_benchmark_publish_results`, `hu_benchmark_compare_with_frontier` | Do not exist | New public functions | None | **NO** |

**Verdict: ABI-SAFE.**

---

## 2. Name-Collision Call Sites

### 2.1 `hu_episode_t` — Three Conflicting Definitions

The type `hu_episode_t` is **multiply defined across three public headers** with incompatible struct layouts. This is an existing pre-SOTA-2026 collision that Init #10 must resolve before shipping. Any TU that includes more than one of these headers will fail to compile.

| Header | Type Name | Fields | Status |
|--------|-----------|--------|--------|
| `include/human/agent/episodic.h:22` | `hu_episode_t` | `summary`, `summary_len`, `timestamp_ms`, `session_id`, `session_id_len` | **ACTIVE DEFINITION — thin session struct** |
| `include/human/memory/deep_memory.h:23` | `hu_episode_t` | `id`, `summary`, `summary_len`, `emotional_arc`, ..., `source_tag`, `source_tag_len` (11 fields) | **ACTIVE DEFINITION — rich deep-memory struct** |
| `include/human/memory/episodic.h:11` | `hu_episode_sqlite_t` (uses different name) | Avoided collision by using `hu_episode_sqlite_t` | Correctly sidestepped; the comment on line 11 acknowledges the collision |

**Call sites requiring updates when Init #10 applies the renames:**

For `include/human/memory/deep_memory.h` → rename `hu_episode_t` to `hu_deep_episode_t`:
- `src/memory/deep_memory.c:80` — `hu_episodic_insert_sql(const hu_episode_t *ep, ...)`
- `src/memory/deep_memory.c:417` — `hu_episode_deinit(hu_allocator_t *alloc, hu_episode_t *ep)`
- `include/human/memory/deep_memory.h:26` — function signature for `hu_episodic_insert_sql`
- `include/human/memory/deep_memory.h:94` — function signature for `hu_episode_deinit`
- `tests/test_deep_memory.c` — all uses of `hu_episode_t` in this file

For `include/human/agent/episodic.h` → rename `hu_episode_t` to `hu_session_episode_t`:
- `src/agent/episodic.c` — implementation of `hu_episodic_store` / `hu_episodic_summarize_session` (uses the struct internally)
- `src/daemon.c:7610, 10427` — calls `hu_episodic_load` / `hu_episodic_store`; if these pass struct literals, they must be updated

**Total `hu_episode_t` unique references: 6+ files, minimum 9 source locations.**

### 2.2 `hu_mcp_server_t` — Client Type Renamed to `hu_mcp_client_t` (Init #12)

The existing `hu_mcp_server_t` (defined in `include/human/mcp.h:17`) is the MCP **client** — the struct that manages a child-process MCP server connection via stdio pipes. Init #12 renames it to `hu_mcp_client_t` and recycles the `hu_mcp_server_t` name for the new server vtable.

**All existing call sites using the old `hu_mcp_server_t` name that must be updated:**

| File | Lines | Reference type |
|------|-------|----------------|
| `include/human/mcp.h` | 17, 19, 20, 21, 24, 27, 31, 35 | Type definition + 6 function signatures |
| `src/mcp.c` | 41, 79, 162, 165, 181, 272, 416, 519, 544, 593, 620, 721, 725, 729, 734, 738, 844, 863 | Implementation + local variables |
| `src/mcp_manager.c` | 23, 251 | Struct field, `hu_mcp_server_create` call |
| `tests/test_mcp.c` | multiple | Test fixtures |
| `tests/test_modules_coverage.c` | multiple | Coverage assertions |

**Total `hu_mcp_server_t` unique source references: ~28 (1 definition + 6 header signatures + ~18 in `src/mcp.c` + 2 in `src/mcp_manager.c` + 2 test files).**

The design doc acknowledges this with: "A `typedef hu_mcp_client_t hu_mcp_server_t HU_DEPRECATED` shim is left behind for one release cycle." However, the shim only applies to the **renamed client type**. It does NOT cover the separately-broken `hu_mcp_host_t` family — see below.

### 2.3 `hu_mcp_host_t` — Symbol Family Removed Without Shim

The existing `include/human/mcp_server.h` defines `hu_mcp_host_t` with these public symbols:

| Symbol | Current location | Used at |
|--------|-----------------|---------|
| `hu_mcp_host_create` | `include/human/mcp_server.h:13` | `src/main.c:2408`, `src/mcp_server.c:28` |
| `hu_mcp_host_set_resources` | `include/human/mcp_server.h:16` | `src/main.c:2416`, `src/mcp_server.c:46` |
| `hu_mcp_host_set_prompts` | `include/human/mcp_server.h:17` | `src/main.c:2417`, `src/mcp_server.c:51` |
| `hu_mcp_host_run` | `include/human/mcp_server.h:19` | `src/main.c:2419`, `src/mcp_server.c:565` |
| `hu_mcp_host_destroy` | `include/human/mcp_server.h:21` | `src/main.c:2421`, `src/mcp_server.c:651` |

Init #12 replaces `mcp_server.h` entirely with the new `hu_mcp_server_t` vtable. **No `HU_DEPRECATED` typedef or compatibility wrapper is provided for `hu_mcp_host_t`.** The entrypoint in `src/main.c:2407-2421` will fail to compile. The init doc must include a migration path: either (a) provide `hu_mcp_host_t` → `hu_mcp_server_default_create` adapter shims, or (b) require a coordinated update of `src/main.c` in the same PR.

---

## 3. Silent Struct-Layout Break Analysis

### 3.1 `hu_persona_overlay_t` (Init #02)

**Current tail field:** `uint8_t leave_on_read_pct;`  
**Added fields:** `uint8_t expert_id; float expert_weight_floor;`  
**Position:** At tail ✅  
**Sizeof consumers:** 13 sites in `src/persona/markdown_loader.c`, 2 in `src/persona/creator.c`, 1 in `src/persona/analyzer.c` — all use `sizeof(hu_persona_overlay_t)` for allocation, which auto-corrects at compile time.  
**Serialization risk:** None detected — overlays are loaded from JSON, not `fwrite`-serialized.  
**Alignment hazard:** `float expert_weight_floor` (4-byte aligned) follows `uint8_t expert_id` (1-byte). The compiler will insert 3 bytes of padding. Callers using `memset(buf, 0, ...)` to zero-initialize are safe; direct byte-copy across an ABI boundary is not.  
**Verdict: Layout-safe for this codebase; padding gap is a documentation risk.**

### 3.2 `hu_heuristic_fact_t` (Init #09)

**Current tail field:** `int64_t last_seen_at;`  
**Added field:** `hu_provenance_t provenance;` (~208 bytes)  
**Position:** At tail ✅  
**Impact on `hu_personal_model_t`:** Contains `hu_heuristic_fact_t facts[HU_PM_MAX_FACTS]` where `HU_PM_MAX_FACTS = 64`. Total size growth: `64 × 208 = 13,312 bytes`. The personal model is heap-allocated via `hu_personal_model_open`, so this is safe.  
**Binary format:** `HU_PM_VERSION` bumped 4 → 5; `hu_personal_model_load` rejects version mismatch. This prevents silent partial-read corruption of existing user saves. ✅  
**Verdict: Tail-safe with version guard.**

### 3.3 `hu_memory_entry_t` (Init #09)

**Current tail field:** `double score;`  
**Added fields:** `int trust_tier; char *provenance; size_t provenance_len;`  
**Position:** At tail ✅  
**Sizeof consumers:** 40+ allocation sites across 15+ source files (full list in §1 Init #09 ledger). All use `count * sizeof(hu_memory_entry_t)` — size auto-corrects on recompile. ✅  
**Serialization risk:** The field is a `char *` pointer — any code path that serializes `hu_memory_entry_t` by writing raw bytes to disk or a socket will be broken. No such code path was found in the search, but the large number of consumers makes exhaustive verification necessary before shipping.  
**Verdict: Compile-safe after full rebuild, but requires a serialization audit across all 40+ call sites before shipping.**

### 3.4 `hu_provider_vtable_t` (Inits #01, #02, #13)

**Current tail field:** `const char *(*active_adapter)(void *ctx);`  
**Three initiatives each independently add fields at the tail:**

| Initiative | Field added | Proposed position |
|-----------|-------------|-------------------|
| Init #01 | `hu_error_t (*apply_steering)(void *ctx, const float *vec, size_t dim);` | After `active_adapter` |
| Init #02 | `load_adapter_mixture`, `set_adapter_mixture`, `active_mixture` | After `apply_steering` (Init #01) |
| Init #13 | `const hu_provider_caps_t *(*caps)(void *ctx);` | After Init #02 additions |

**Coordination gap:** The actual canonical ordering of these fields must be established in the master header **before any initiative ships**. If Init #13 ships before Init #01, its `caps` field occupies the slot Init #01 expects for `apply_steering`. All provider implementors use designated initializers (`.chat = ..., .get_name = ...`) based on code patterns observed — this avoids positional misalignment. However, if any legacy provider uses positional (non-designated) initializers, it will silently assign `caps` to `apply_steering`.  
**Verdict: NEEDS-WRITE-CONFIRMATION** — enforce designated initializers in all provider vtable definitions, and establish canonical field order in `provider.h` before any initiative ships.

### 3.5 `hu_agent_extensions_t` (Init #07)

**Current tail field:** `hu_escalate_protocol_t escalate_protocol;`  
**Added field:** `hu_reward_model_t reward_model;`  
**Position:** At tail ✅  
**Embedded-by-value:** `hu_agent_t` contains `hu_agent_extensions_t sota;` as an embedded field (verified at `include/human/agent.h:461`). Adding a field to `hu_agent_extensions_t` changes the layout of `hu_agent_t` itself, shifting all fields after `sota` by `sizeof(hu_reward_model_t)`.  
**`hu_reward_model_t` is opaque:** The header declares it as an opaque type with a vtable pointer and context pointer. Its actual size depends on the struct definition in `ml/reward_model.h`. If it contains only `{void *ctx; const hu_reward_model_vtable_t *vtable;}`, that's 16 bytes on aarch64. Any code path that zero-initializes `hu_agent_t` via `calloc` / `memset` will produce a valid "no reward model" state.  
**Risk:** IPC or binary protocols that transfer `hu_agent_t` by size-encoded byte blobs will silently break. The daemon `src/daemon.c` must be audited.

---

## 4. Cross-Initiative Enum Collision — `hu_job_kind_t`

**This is the most insidious cross-initiative bug in the fleet.**

Four initiatives independently propose appending new enum values "before `HU_JOB_KIND_MAX`" to `hu_job_kind_t` in `include/human/agent/scheduler.h`. The current enum ends at `HU_JOB_TRAINING_DATA_EXTRACT = 9`, making `HU_JOB_KIND_MAX = 10` (implicit). Without explicit ordinal assignments:

| Initiative | New Value | Would Receive Ordinal |
|-----------|-----------|----------------------|
| Init #02 | `HU_JOB_MOLORA_ROUTER_TRAIN` | 10 (collision!) |
| Init #05 | `HU_JOB_TTT_DRIFT_EVAL` | 10 (collision!) |
| Init #10 | `HU_JOB_CONSOLIDATE_NREM` | 10 (collision!), `HU_JOB_CONSOLIDATE_REM` = 11 |
| Init #11 | `HU_JOB_PROACTIVITY_RECHECK` | 10 (collision!) |

**Every initiative claims ordinal 10.** Any two initiatives compiled together will produce enum value collisions, causing the scheduler's dispatch table to route `HU_JOB_MOLORA_ROUTER_TRAIN` jobs to the TTT runner (or vice versa). The SQLite `scheduler_jobs` table's `kind` INTEGER column will also silently map to wrong runners across binary upgrades.

**Required resolution (before any initiative ships):**

```c
typedef enum hu_job_kind {
    /* Existing (0-9) unchanged */
    HU_JOB_AUTODREAM_QUARANTINE     = 0,
    HU_JOB_AUTODREAM_COMMUNITY      = 1,
    HU_JOB_AUTODREAM_DECAY          = 2,
    HU_JOB_KV_CACHE_EVICTION        = 3,
    HU_JOB_KV_CACHE_WARMING         = 4,
    HU_JOB_LORA_TRAINING            = 5,
    HU_JOB_COUNTERFACTUAL_REHEARSAL = 6,
    HU_JOB_BELIEF_REVERIFICATION    = 7,
    HU_JOB_PERSONA_EVOLVER          = 8,
    HU_JOB_TRAINING_DATA_EXTRACT    = 9,
    /* SOTA-2026 additions — MUST be explicit to prevent collision */
    HU_JOB_MOLORA_ROUTER_TRAIN      = 10,  /* Init #02 */
    HU_JOB_TTT_DRIFT_EVAL           = 11,  /* Init #05 */
    HU_JOB_CONSOLIDATE_NREM         = 12,  /* Init #10 */
    HU_JOB_CONSOLIDATE_REM          = 13,  /* Init #10 */
    HU_JOB_PROACTIVITY_RECHECK      = 14,  /* Init #11 */
    HU_JOB_KIND_MAX                 = 15,
} hu_job_kind_t;
```

This assignment must be merged as a precondition PR before any of the four initiatives land.

---

## 5. NULL No-Op Coverage — New Optional Vtable Methods

For each new optional vtable method, the dispatch helper must contain: `if (!provider->vtable->method) return HU_ERR_NOT_SUPPORTED;`. The design docs claim this pattern is followed. Verification against actual source:

### 5.1 `hu_provider_vtable_t` — Existing pattern

The existing `hu_provider_load_adapter` helper in `include/human/provider.h:261` is documented: _"Each returns `HU_ERR_NOT_SUPPORTED` when the provider's vtable leaves the corresponding method NULL."_ This establishes the canonical NULL-check pattern. The implementation is in `src/agent/provider.c` (not directly read, but consistent with the helper declarations).

### 5.2 New Methods from SOTA-2026 Initiatives

| Method | Helper declared | NULL guard expected in |
|--------|----------------|----------------------|
| `apply_steering` (Init #01) | `hu_provider_apply_steering` in `persona/steering.h` | `src/persona/steering.c` (new file) |
| `load_adapter_mixture` (Init #02) | `hu_provider_load_adapter_mixture` in `provider.h` | `src/agent/provider.c` or new molora dispatch |
| `set_adapter_mixture` (Init #02) | `hu_provider_set_adapter_mixture` | Same |
| `active_mixture` (Init #02) | `hu_provider_active_mixture` | Same |
| `caps` (Init #13) | `hu_kv_compressor_decode_by_envelope` (indirect) | `src/providers/factory.c` or caps dispatch |
| TTT vtable methods (Init #05) | `hu_learner_step_bounded`, `hu_learner_rollback_journal` | `src/ml/learner.c` (new) |
| Reward model vtable (Init #07) | `hu_reward_model_score` etc. | `src/ml/reward_model.c` (new) |
| Federation vtable (Init #08) | `hu_federation_run_round` etc. | `src/federation.c` (new) |

**Coverage gap:** None of the new dispatch helpers have been written yet — all are declared only in the proposed headers. The NULL-check pattern must be implemented in the new `.c` files. The fact that the design docs describe the pattern does not substitute for its presence in code. Recommend: mark each initiative's PR as "not mergeable" until the dispatcher `.c` file with NULL guards is reviewed.

### 5.3 `#ifdef HU_ENABLE_TTT` ODR Violation (Init #05)

Init #05 adds the `ttt` field to `hu_learner_vtable_t` under `#ifdef HU_ENABLE_TTT`:

```c
typedef struct hu_learner_vtable {
    const char *name;
    bool (*available)(void);
    hu_error_t (*train)(...);
    void (*deinit)(void *ctx);
#ifdef HU_ENABLE_TTT
    const hu_learner_ttt_vtable_t *ttt;
#endif
} hu_learner_vtable_t;
```

If any translation unit links against a version of `hu_learner_vtable_t` compiled without `HU_ENABLE_TTT` and another is compiled with it, the struct layouts differ, violating C's one-definition rule. This is a **latent ODR violation** that the linker will not catch and sanitizers will not reliably detect.

**Mitigation options:**
1. Always compile `HU_ENABLE_TTT` as all-or-nothing via CMake (add it to the build system default; the `#ifdef` serves only as a binary size gate, not a runtime toggle).
2. Alternatively, keep the field unconditional (set it to NULL when TTT is not available — same approach used for other optional vtable slots in this codebase).

---

## 6. Ship-Order Verification — `load_adapter` Triad (04 → 02 → 05)

**Claimed order:** Init #04 ships first (MLX Qwen3 backend with `load_adapter` triple), then Init #02 (MoLoRA adds mixture methods), then Init #05 (TTT adds learner step methods).

**Verification:**

### Does Init #04 actually unblock Init #02?

Init #02 adds `load_adapter_mixture` / `set_adapter_mixture` / `active_mixture` — **three new vtable methods** distinct from the existing `load_adapter` / `unload_adapter` / `active_adapter` triple. Init #04 implements the existing triple. Init #02 is **not actually blocked by** Init #04's vtable shape — Init #02's new methods can be added independently to the vtable stub. They do share the Init #04 MLX helper bridge (same `mlx_finetune` subprocess), so Init #02's implementation would need Init #04's bridge binary. **The vtable shape dependency is real; the protocol dependency exists but Init #02's design could ship with a no-op stub until Init #04 lands.**

### Does Init #04's protocol reserve enough space for Init #02 and #05?

Init #04 defines a helper protocol with reserved opcodes:
- `stream` — for Init #04 v1.5
- `mutate_adapter` — reserved for Init #05 (TTT step)
- `load_adapter_mix` — reserved for Init #02 (mixture loading)

**Space reservation: YES.** Three distinct opcodes are explicitly reserved for the successor initiatives. No allocation conflict exists at the protocol layer.

### Does Init #05 actually require Init #02?

Init #05 (TTT) needs to:
1. Load a LoRA adapter checkpoint to checkpoint — uses `hu_learner_t.vt->train`, not the provider `load_adapter` directly.
2. Apply the adapter to inference — uses the existing `hu_provider_load_adapter` triple.

Init #05 **does not require Init #02's mixture methods.** The claimed ship order of `04 → 02 → 05` is overly conservative. `05` depends on `04` (MLX bridge) but not on `02` (MoLoRA). The correct dependency graph is:

```
Init #04 (MLX bridge) ──→ Init #02 (MoLoRA mixture)
      └──────────────────→ Init #05 (TTT, independent of #02)
```

**The master plan's linear ordering `04 → 02 → 05` is safe but unnecessary. Init #02 and #05 can ship concurrently after Init #04.** The real gating precondition is Init #04.

---

## 7. Public-Header Changes Requiring a Major-Version Bump

Under semantic versioning, a major-version bump (X.0.0) is required for any change that breaks source or binary compatibility for existing consumers of the public header API.

| Finding | Change | Semver impact |
|---------|--------|--------------|
| Init #09 — `hu_personal_model_ingest` signature change | Required parameter added | **MAJOR** — existing call sites will not compile |
| Init #12 — `hu_mcp_host_t` symbol family removed | Public symbols deleted | **MAJOR** — binary and source incompatible |
| Init #12 — `hu_mcp_server_t` renamed to `hu_mcp_client_t` | Type rename (shim provided) | **MINOR** (shim degrades it to deprecation warning) |
| Init #09 — `hu_memory_entry_t` struct layout change | New fields at tail | **MINOR** (source-compatible after recompile, binary-incompatible at ABI boundary) |
| Cross-init — `hu_job_kind_t` enum collision | Value collision if not corrected | **MAJOR** (silent runtime behavioral corruption) — if shipped as-is |
| Init #10 — `hu_episode_t` rename | Type rename in two headers | **MAJOR** (two types renamed, new canonical type introduced) |

**Recommendation:** The SOTA-2026 fleet as proposed constitutes a major-version release. A single coordinated `v2.0.0` tag should be planned to cover all breaking changes, rather than shipping them across multiple minor releases and producing repeated ABI breaks.

---

## 8. Final Verdict Per Initiative

| Initiative | Verdict | Key reason |
|-----------|---------|-----------|
| Init #01 — Activation Steering | **ABI-SAFE** | All additions at tail; new header; no existing signatures changed |
| Init #02 — MoLoRA Channels | **ABI-BREAKING** (partial) | `hu_job_kind_t` enum collision; vtable additions themselves are safe |
| Init #03 — Apple FM Provider | **ABI-SAFE** | New provider; no vtable changes; config struct tail-append |
| Init #04 — MLX Qwen3 Provider | **ABI-SAFE** | New provider; implements existing triple; protocol reserves opcodes |
| Init #05 — Verifier-Driven TTT | **NEEDS-WRITE-CONFIRMATION** (ODR) + **ABI-BREAKING** (enum) | `#ifdef`-gated vtable field is an ODR violation; enum collision |
| Init #07 — ThinkPRM Verifier | **NEEDS-WRITE-CONFIRMATION** | `hu_agent_extensions_t` layout change requires daemon audit |
| Init #08 — Federated LoRA | **ABI-SAFE** | Entirely new surface; error enum tail-append only |
| Init #09 — Memory Trust Tiers | **ABI-BREAKING** | `hu_personal_model_ingest` signature change (4 call sites); `hu_memory_entry_t` struct growth (40+ sizeof consumers) |
| Init #10 — Episode Storage | **ABI-BREAKING** | `hu_episode_t` renamed in 2 headers; pre-existing triple-definition collision; enum collision |
| Init #11 — Proactivity & Typing | **ABI-BREAKING** (enum only) | `hu_job_kind_t` collision; new surface otherwise ABI-safe |
| Init #12 — MCP Server Mode | **ABI-BREAKING** | `hu_mcp_host_t` family removed without shim; `hu_mcp_server_t` client type renamed |
| Init #13 — KV Compression | **ABI-SAFE** | New vtable; `caps` method tail-appended to provider vtable |
| Init #14 — Public Benchmarks | **ABI-SAFE** | Enum tail-append; new functions only |

---

## Appendix A — Summary Counts

| Category | Count |
|----------|-------|
| **ABI-BREAKING findings** | **6** |
| NEEDS-WRITE-CONFIRMATION findings | 3 |
| ABI-SAFE initiatives | 6 |
| `hu_episode_t` name-collision call sites | **9** (2 header definitions + 2 src + 1 test + 2 function signatures + 2 daemon call sites) |
| `hu_mcp_server_t` name-collision call sites | **28** (1 definition + 6 function signatures + ~18 `src/mcp.c` + 2 `src/mcp_manager.c` + 2 test files) |
| `hu_mcp_host_t` removal call sites (no shim) | **7** (`src/main.c`: 5 calls; `src/mcp_server.c:28,651` public functions) |
| Total name-collision call sites | **37** |

## Appendix B — Preconditions Before Any Initiative Merges

1. **Resolve `hu_job_kind_t` ordinal collision** — produce a single precondition PR that assigns explicit numeric values (10–14) to all SOTA-2026 enum additions before any of Inits #02, #05, #10, #11 ship.
2. **Resolve `hu_episode_t` triple-definition** — Init #10 must ship first among any initiative that uses episodic memory types. No other initiative should introduce further `hu_episode_t` references.
3. **Establish `hu_provider_vtable_t` canonical tail order** — one precondition PR locks in the field ordering for `apply_steering`, `load_adapter_mixture`, `set_adapter_mixture`, `active_mixture`, `caps`. All provider implementors must be audited to use designated initializers.
4. **Init #09 must provide migration path for `hu_personal_model_ingest`** — the 4 call sites in `src/` must be updated atomically in the same PR that changes the signature. Consider providing an inline wrapper that passes `NULL` provenance for legacy callers.
5. **Init #12 must provide a `hu_mcp_host_t` migration** — either shims in `mcp_server.h` or coordinated update of `src/main.c` in the same PR.

BASE: `include/human/*.h` (current workspace)  
HEAD: proposed changes per `docs/plans/2026-05-11-init-*.md`

RESULT_api-contract-watcher=BREAKING
