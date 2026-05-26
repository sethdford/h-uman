# `human doctor prompt_budget` + init_outcome → dpo_pairs bridge — Design

## Components

### Part A: prompt_budget visibility

| Name | What it does | Where it lives |
|------|-------------|----------------|
| `hu_prompt_budget_field_stat_ext_t` | New richer per-field struct: `{name, mean_bytes, samples, non_empty_count}`. | `include/human/agent/prompt_budget.h` |
| `hu_prompt_budget_snapshot_ext` | Snapshot accumulator to ext struct array. Existing `hu_prompt_budget_snapshot` stays untouched. | `src/agent/prompt_budget.c` |
| `hu_prompt_budget_snapshot_path` | Resolve `~/.human/prompt_budget.snapshot.json`. | `src/agent/prompt_budget.c` |
| `hu_prompt_budget_save_snapshot` | Atomic write to snapshot path. `tmp + fwrite + fflush + fsync + rename`. Mirrors `hu_personal_model_save`. | `src/agent/prompt_budget.c` |
| `hu_prompt_budget_load_snapshot` | Read back into a typed struct. Skip if file missing (ENOENT not error). | `src/agent/prompt_budget.c` |
| Daemon flush call site | Every 60s in the main tick loop, near the existing verifier_metrics flush at `src/daemon.c:3482`. | `src/daemon.c` |
| `hu_prompt_budget_doctor_data_t` | Carries everything the renderers need: status enum, observation_count, snapshot_age_seconds, field_count, field_stats[], warning strings. | `include/human/doctor.h` |
| `hu_doctor_check_prompt_budget` | Reads snapshot, populates data struct, derives diag_items for human render. | `src/doctor/check_prompt_budget.c` |
| `hu_doctor_render_prompt_budget_json` | Serializes data struct to JSON via `human/json_util.h`. | same file |
| CLI dispatch | Add `prompt_budget` to focused-subcommand block in `cmd_doctor`; add global `--json` flag. | `src/main.c` |
| Registry wiring | Add to `hu_doctor_registry_register_defaults`. | `src/doctor/registry.c` |

### Part B: init_outcome → dpo_pairs bridge

| Name | What it does | Where it lives |
|------|-------------|----------------|
| `hu_init_dpo_bridge_record` | Single entry point. Takes (resolution_outcome, draft, target, resolution_ts). Builds an `hu_preference_pair_t` (single-sided) and calls `hu_dpo_record_pair`. Gated by `#ifdef HU_ENABLE_ML`. | `include/human/ml/init_dpo_bridge.h` + `src/ml/init_dpo_bridge.c` |
| `pending_proposal_t` extension | Add `char draft[<bounded>]` so the resolver has it in scope when writing the resolution. | `src/agent/init_outcome.c` (static struct, no header change) |
| Resolver call site | In `hu_init_outcome_resolve_pending` after the successful `hu_init_outcome_append_resolution`, call `hu_init_dpo_bridge_record(...)` (under `#ifdef HU_ENABLE_ML`). | `src/agent/init_outcome.c:756-759` area |
| One-shot disabled log | When ML is OFF, `hu_log_info_once` line per process: "init_outcome → dpo bridge disabled (HU_ENABLE_ML not set)". | `src/agent/init_outcome.c` |

## Data flow

### Part A: prompt_budget flush + read

```
[daemon main tick]                          [filesystem]                      [doctor process]
hu_prompt_budget_observe(after each turn)
       │                                    (every 60s, atomic write)
       ▼                                    +------------------------------+
+--------------------+    tmp+fsync+rename  | ~/.human/                    |
| hu_prompt_budget_t |  ─────────────────► |   prompt_budget.snapshot.json|
| (in-memory)        |                      +------------------------------+
+--------------------+                                  │
                                                        │ (on operator invocation)
                                                        ▼
                                          hu_prompt_budget_load_snapshot
                                                        │
                                                        ▼
                                          hu_doctor_check_prompt_budget
                                                        │
                                              ┌─────────┴──────────┐
                                              ▼                    ▼
                                       diag_items[] (human)   data struct (JSON)
                                              │                    │
                                              ▼                    ▼
                                        stdout (human)       stdout (single JSON obj)
```

### Part B: init_outcome → dpo bridge

```
[daemon proposer tick (existing path)]
hu_init_outcome_append(...)  ─── FIRED proposal ──► JSONL line
       │
       │ (on later tick, ≥10min later)
       ▼
hu_init_outcome_resolve_pending(...)
       │
       │ walks JSONL, pairs FIRED with reply check from chat.db
       │
       ▼
hu_init_outcome_decide_resolution → REPLIED | IGNORED | PENDING
       │ (REPLIED or IGNORED)
       │
       ├──► hu_init_outcome_append_resolution(...) → JSONL line (authoritative)
       │
       └──► hu_init_dpo_bridge_record(...)        (← NEW, gated HU_ENABLE_ML)
                  │
                  ▼
            hu_dpo_record_pair(...)
                  │
                  ▼
            INSERT INTO dpo_pairs(prompt, chosen, rejected, margin, timestamp, source)
            single-sided per outcome:
              REPLIED → chosen=draft,  rejected="",     source="init_proposer_v1"
              IGNORED → chosen="",     rejected=draft,  source="init_proposer_v1"
```

## Decisions

### D1 — Atomic snapshot write via Personal Model precedent, NOT verifier (covers AC-4)

**Chose**: Atomic discipline (`tmp + fwrite + fflush + fsync + rename`) from
`hu_personal_model_save`, pinned by `tests/test_personal_model_atomic_save.c`.

**Over**: Verifier-style `fopen("w") + fprintf + fclose`
(`src/agent/verifier_metrics.c:52-85`), or per-turn flush.

**Because**: Doctor reads the snapshot while the daemon may be writing.
Verifier's non-atomic write is a known weakness — torn reads possible if
doctor lands mid-fwrite. The Personal Model pattern is already proven
crash-safe under concurrent reads. The 60s cadence still comes from
verifier (proven acceptable in production); only the write discipline
differs.

### D2 — New richer per-field struct, existing API untouched (covers AC-1, AC-5)

**Chose**: Add `hu_prompt_budget_field_stat_ext_t` + `_snapshot_ext()`
alongside the existing `hu_prompt_field_stat_t` + `_snapshot()`. Existing
callers of `_snapshot()` (if any) untouched.

**Over**: Modifying `hu_prompt_field_stat_t` to add `samples` +
`non_empty_count` directly.

**Because**: `hu_prompt_field_stat_t` is also the per-turn observe payload
(`hu_prompt_budget_observe` consumes it). Bloating it would force every
appender call site to populate samples/non_empty (which it can't — those
are accumulator-level concepts). New struct keeps the per-turn type lean.

### D3 — Source-of-truth data struct shared by both renderers (covers AC-6)

**Chose**: `hu_prompt_budget_doctor_data_t` carries all structured fields.
Human renderer stringifies into `hu_diag_item_t[]`. JSON renderer
serializes the struct directly via `hu_json_*`. Test in AC-6 populates
the struct programmatically + asserts both renderers consistent.

**Over**: Building two parallel paths (one collecting diag_items, one
collecting JSON) from the snapshot file.

**Because**: `hu_diag_item_t` only carries `{severity, category, message}`
(`include/human/doctor.h:92-96`). Extracting structured numbers from
`message` to JSON would require parsing format strings. One struct, two
renderers: drift is structurally impossible.

### D4 — Three-tier severity: OK / WARN / ERROR with non-zero exit on WARN+ (covers AC-2, AC-3)

**Chose**: Doctor emits one diag item per row. Status enum maps:
- `ok` → all rows OK, exit 0
- `disabled` → single WARN, exit non-zero
- `quiet` → single WARN distinct from disabled, exit non-zero
- `error` → ERROR, exit non-zero (e.g. snapshot file unreadable)

**Over**: A single pass/fail boolean.

**Because**: Operators must distinguish "I forgot to enable this" (AC-2)
from "I enabled it but no data" (AC-3) at a glance. Both map to a
non-zero exit but the diag rows tell which.

### D5 — JSON schema mirrors data struct 1:1 (covers AC-5, AC-6)

```json
{
  "check": "prompt_budget",
  "status": "ok" | "disabled" | "quiet" | "error",
  "summary": {
    "observation_count": 1234,
    "field_count": 27,
    "snapshot_age_seconds": 12
  },
  "fields": [
    { "name": "system_prompt", "mean_bytes": 4096, "samples": 1234, "non_empty_count": 1234 }
  ],
  "warnings": [ "prompt_budget disabled — set cfg->prompt_budget.enabled=true" ]
}
```

When `status != "ok"`, `fields` may be empty and `warnings` carries the
message that would have been the diag_item's text in human mode.

### D6 — `--json` suppresses ALL non-JSON output on stdout (covers AC-5)

**Chose**: When `--json` is set, exactly one JSON object on stdout, no
banner, no color, no blank lines. Errors still go to stderr.

**Because**: Pipeability is the entire reason for JSON. A single object
per invocation lets `jq` consume the output without preprocessing.

### D7 — dpo bridge writes single-sided rows mirroring reaction_handler (covers AC-8, AC-9)

**Chose**: Bridge calls `hu_dpo_record_pair` with one of:
- `chosen=draft, rejected="", margin=1.0, source="init_proposer_v1"` (REPLIED)
- `chosen="", rejected=draft, margin=1.0, source="init_proposer_v1"` (IGNORED)

**Over**: Pairing REPLIED with IGNORED across contexts to write a true
preference pair.

**Because**: The reaction_handler path already does single-sided writes
(`src/ml/dpo.c:88-99` comment documents the intentional design). Mirroring
that pattern stays consistent with prior art; per-context pairing would
require a join key that doesn't exist yet (proposer context isn't
persisted) and would be a separate session's worth of work.

The read side (`hu_dpo_iterate_pairs`) currently filters single-sided rows
out of training — this is acknowledged in non-goals. The data lands; the
read-side change is a separate decision.

### D8 — Bridge is gated `HU_ENABLE_ML`; resolver path unchanged when disabled (covers AC-10, AC-12)

**Chose**: Bridge code lives entirely in `src/ml/init_dpo_bridge.c` (gated by
`HU_ENABLE_ML` in CMakeLists). The call site in `init_outcome.c` is wrapped
in `#ifdef HU_ENABLE_ML` (with a one-shot info log in the `#else` branch
the first time a resolution would have been bridged).

**Over**: A no-op stub function at link time.

**Because**: Build-time gating keeps minimal builds clean (no ml.h
includes, no dpo_collector lookup overhead). Per
`rules/test-source-gate-symmetry.md`, the test file is also gated. Per
`rules/silent-config-gated-subsystems.md`, the disabled path logs once
so operators know the bridge isn't writing — without spamming on every
resolution.

### D9 — Bridge failures are warn-only, do NOT block resolution write (covers AC-12)

**Chose**: In the resolver, the existing resolution-line append is the
authoritative write. The bridge call follows AFTER and its return value is
captured but its failure logs `warn-once` and does NOT cause the resolver
to retry, rollback, or skip the next pending proposal.

**Over**: Two-phase commit (resolution + bridge atomic).

**Because**: The JSONL is the source of truth; the dpo_pair is derived
signal. Losing a dpo_pair to a transient DB lock is a small signal loss; a
two-phase commit would risk losing resolutions which is much worse.

### D10 — `touch` + rebuild discipline for the production binary (covers AC-1, AC-4)

**Chose**: After editing `src/daemon.c`, `src/agent/prompt_budget.c`, or
`src/agent/init_outcome.c`, the implementation runs
`touch <files> && cmake --build build --target human -j8` and verifies
"Linking C executable human" + "Signing human binary" appear in cmake
output.

**Over**: Trusting cmake's "[100%] Built target human" output.

**Because**: Per `.claude/rules/cmake-build-stale-binary.md`, the
two-library setup (`libhuman_core.a` + `libhuman_core_test.a`) can
silently leave the production binary's objects stale even when the test
binary picks up the change. Without the touch, "tests pass" but the
daemon doesn't flush. Cost of the trap is documented; the touch is free.

## Risks

| Risk | Mitigation |
|------|-----------|
| Daemon writes snapshot atomically, but doctor reads mid-rename on a system where rename isn't atomic across filesystems | Snapshot path is under `~/.human/` — same FS as the tmp file. Document the assumption in the writer's doxygen. |
| `hu_init_outcome_resolve_pending` is hot path; bridge call adds latency per resolution | Each resolution at most ~10 min apart in practice; one extra SQLite INSERT (~1-10 ms) is negligible. |
| `pending_proposal_t.draft` field grows the struct significantly | Use a bounded `char draft[1024]` (proposer drafts are typically <500 bytes; truncate beyond). |
| Schema drift between JSON and human renderer | Both consume the same `hu_prompt_budget_doctor_data_t`. AC-6 contract test pins consistency. |
| `init_proposer_v1` source rows pollute `dpo_pairs` if read side ever stops filtering single-sided | Distinct `source` value lets the read side filter on `source != 'init_proposer_v1'` if needed. Documented in the bridge header doxygen. |
| dpo_collector unavailable when bridge fires (no SQLite, ML disabled at runtime mid-process) | Bridge silently no-ops if collector is NULL; `hu_log_warn_once` per process. |

## AC → Decision map

| AC | Covered by |
|----|-----------|
| AC-1 (populated output) | D2, D3, D10 |
| AC-2 (disabled) | D4 |
| AC-3 (enabled-but-quiet) | D1, D4 |
| AC-4 (atomic flush) | D1, D10 |
| AC-5 (`--json`) | D5, D6 |
| AC-6 (renderer consistency) | D3, D5 |
| AC-7 (registry + subcommand) | (wiring — see tasks.md) |
| AC-8 (replied → chosen) | D7, D8 |
| AC-9 (ignored → rejected) | D7, D8 |
| AC-10 (ML-disabled fallthrough) | D8 |
| AC-11 (contract tests) | (tests — see tasks.md) |
| AC-12 (bridge non-blocking) | D9 |
| AC-13 (all paths tested) | (tests — see tasks.md) |

All ACs covered.
