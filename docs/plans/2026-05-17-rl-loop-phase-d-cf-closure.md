---
title: RL Loop Phase D — Carry-Forward Closure (CF-1 through CF-7)
status: closed
owner: rl-sota
parent_plan: docs/plans/2026-05-11-full-sota-rl-improvement-loop.md
related_proofs:
  - docs/proof/rl-loop-shipcontract.md (Open carry-forwards table)
  - docs/proof/adversarial-audit-report.md
  - docs/proof/rl-loop-proof.md
risk_tier: high
created: 2026-05-17
revision_log:
  - 2026-05-17 v1: initial draft → NEEDS-REWORK (4 blockers + 6 highs from critic + spec-verifier)
  - 2026-05-17 v2: this revision. Blockers fixed B1-B4 (phantom citations), B5 (CF-7 regex), B6 (CF-1 missing ACs); highs fixed H1 (D-2 system probe), H2 (D-4 db_path), H3 (D-5 mandatory eval_provider), H4 (CF-6 before CF-1 sequencing), H5 (D-4 deterministic test), H6 (D-7 pin n=10 + new residual CF-2-R).
last_audit: 2026-05-25
---

# RL Loop Phase D — Carry-Forward Closure

Closes the seven open carry-forwards (CF-1 … CF-7) tracked in `docs/proof/rl-loop-shipcontract.md::Open carry-forwards (honest)` that survived the `rl-sota-phase-6-complete` close-out and the `sprint-auditor` re-audit (`PASS_WITH_NOTES`).

**Why this is its own phase, not a series of patches**: CF-1 and CF-4 must close together (co-closure dependency — see `rl-loop-shipcontract.md` line 271), and CF-2 depends on the shared metric helper that CF-4 introduces. The seven items form three independent slices plus one inter-dependent cluster.

**Honest contract**: this is "finish the wiring at user-facing surfaces", not "fix broken math". The `rl-sota-phase-6-complete` tag still stands. This phase produces `rl-sota-phase-d-cf-closure-complete` on top.

---

## §1 Sequencing (mandatory order)

```
   ┌── CF-7 (security HIGH, foundation) ──┐
   │                                      │
   │   ┌── CF-3 (daemon poll) ────┐       │
   │   │                          │       │
   │   └── CF-5 (KTO grad sweep) ─┘       │
   │                                      │
   │   CF-6 → CF-4 → CF-1 → CF-2          │
   │   (CF-6 plumbs reasons used by CF-1) │
   └──────────────────────────────────────┘
```

**Sequencing rationale (revised per critic H-4, spec-verifier CF-6 finding):**

1. **CF-7 first**: security HIGH, isolated, and CF-1/CF-4 expand the eval pipeline that already calls into `popen` paths. Land the shared script-path resolver before adding more callers.
2. **CF-3, CF-5 in parallel after CF-7**: fully independent surfaces (daemon, test).
3. **CF-6 BEFORE CF-1** (revised): CF-1's CLI scorecard renders the per-judge `unavailable_reason` string per slot. If CF-1 lands first, it constructs harness slots with `unavailable_reason = NULL` (or stale hardcoded strings), which would emit a scorecard that says nothing useful when judges are unavailable. CF-6 ships the factory `out_reason` plumbing so CF-1's scorecard can render real reasons.
4. **CF-4 → CF-1 → CF-2 sequential** (per the co-closure note in `rl-loop-shipcontract.md` line 271):
   - CF-4 first introduces the shared `hu_persona_rollout_run` helper that turns `(provider, adapter_path, prompts) → (persona_scores[], p95_ms)`. Without it, CF-1's CLI scorecard would be backed by synthetic gate inputs (a *new* inflation).
   - CF-1 then wires the three CLI surfaces to consume the helper plus existing `competitive_harness`, `eval_gate`, `leaderboard`, plus CF-6's factory `out_reason`.
   - CF-2 finally consumes the same helper output to populate the demo evidence files end-to-end.

---

## §2 CF-7 — `popen` relative-CWD hardening (Task D-1)

**Owner**: security-reviewer-blessed change. Risk tier: **High** (`src/ml/*_mlx.c`, security-critical).

### Findings (from recon, file:line evidence)

| File | Line | Command template (script path is relative-CWD today) |
|------|------|------------------------------------------------------|
| `src/ml/dpo_real_mlx.c` | 165 | `python3 scripts/dpo_mlx_train.py --model '%s' --data '%s' ...` |
| `src/ml/kto_mlx.c` | 169 | `python3 scripts/kto_mlx_train.py --model '%s' --data '%s' ...` |
| `src/ml/grpo_mlx.c` | 257 | `python3 scripts/grpo_mlx_train.py --input '%s' --adapter-out '%s' ...` |
| `src/ml/reward_model_mlx.c` | 88 | `python3 scripts/rm_mlx_train.py --infer ...` (same class, in-scope) |

No shared resolver exists. `src/feeds/apple.c::resolve_script_path` uses `HU_PROJECT_ROOT` env + executable-relative + `getcwd()` fallback — close-but-not-quite (still CWD-sensitive on dev).

### D-1 work

1. **Add compile-time macro** `HU_ML_SCRIPTS_DIR` in `CMakeLists.txt`:
   ```cmake
   target_compile_definitions(human_core PRIVATE
       HU_ML_SCRIPTS_DIR="${HU_ROOT}/scripts")
   target_compile_definitions(human_tests PRIVATE
       HU_ML_SCRIPTS_DIR="${HU_ROOT}/scripts")
   ```
   Same idiom as `HU_EVAL_SUITES_DIR` (CMakeLists.txt ~1464-1465).

2. **Add shared helper** `src/ml/ml_scripts_dir.{h,c}` (new TU, ~40 lines):
   ```c
   /* Returns absolute path to a helper script under <scripts dir>/<name>.
    * Resolution order:
    *   1. getenv("HU_ML_SCRIPTS_DIR") if set and non-empty
    *   2. HU_ML_SCRIPTS_DIR compile-time macro
    *   3. HU_PROJECT_ROOT env + "/scripts" (compat with apple.c)
    * Caller-allocated out buffer; returns HU_ERR_BUFFER_TOO_SMALL if cap insufficient.
    * Never falls back to getcwd() — explicit and predictable. */
   hu_error_t hu_ml_resolve_script_path(const char *script_name,
                                        char *out, size_t cap);
   ```

3. **Update 4 popen sites** (dpo_real_mlx, kto_mlx, grpo_mlx, reward_model_mlx) to call the resolver and splice the absolute path into the `popen` command. Single-quoting of the path is mandatory (paths can contain spaces but cannot contain single quotes — we already reject single-quote in user inputs; apply the same to resolved paths).

4. **Update availability probes** (`mlx_lm_lora_kto_available`, `mlx_lm_lora_grpo_available`, similar) — they currently `popen` a `python3 -c '...'` string with no script-path dependency, so no change to the probe; just confirm no relative-CWD lurks.

5. **Tests** (`tests/test_ml_scripts_dir.c`, new):
   - `test_resolver_uses_env_when_set` — set `HU_ML_SCRIPTS_DIR=/tmp/fake`, assert resolver returns `/tmp/fake/foo.py`
   - `test_resolver_falls_back_to_compile_time` — unset env, assert returns `${HU_ROOT}/scripts/foo.py`
   - `test_resolver_rejects_buffer_too_small` — pass cap=4, assert `HU_ERR_BUFFER_TOO_SMALL`
   - `test_resolver_rejects_quote_in_path` — set env to `/tmp/o'malley`, assert `HU_ERR_INVALID_ARGUMENT`
   - `test_resolver_rejects_null_name` — assert `HU_ERR_INVALID_ARGUMENT`

6. **Regression guard** (`tests/test_grpo_mlx.c`, `test_kto_mlx.c` once created, `test_dpo_real_mlx.c`):
   - For each popen wrapper, drive `step()` with `HU_ML_SCRIPTS_DIR=/nonexistent` and a controlled environment. Assert the wrapper returns a non-HU_OK error (`HU_ERR_IO` or `HU_ERR_NOT_SUPPORTED`) rather than silently invoking a different script. This is the security regression test — proves the wrapper *uses* the resolved path, not CWD.

### D-1 Acceptance Criteria

- **AC1 (revised per spec-verifier B-5)**: No `popen` command-string literal in `src/ml/` contains a relative `scripts/` path-prefix. Audit method:
  - `rg -n '"python3 scripts/' src/ml/` returns **zero matches**.
  - `rg -n '"python3 .*scripts/' src/ml/` returns **zero matches**.
  - Every `popen` call in `src/ml/` is preceded (in the same function, within 30 lines) by a `hu_ml_resolve_script_path` call whose output is spliced into the command string. Verified by manual review of the diff + a per-file regression test (AC4).
- **AC2**: All 4 popen sites invoke `hu_ml_resolve_script_path` and splice its output, single-quoted, into the command string. `hu_ml_resolve_script_path` returns an absolute path (starts with `/`) verified by `test_resolver_returns_absolute_path`.
- **AC3**: New `tests/test_ml_scripts_dir.c` has ≥5 tests, all pass with 0 ASan errors.
- **AC4**: Per-wrapper regression test asserts CWD-shadow attack fails: set `HU_ML_SCRIPTS_DIR=/nonexistent`, change CWD to a temp dir containing a malicious `scripts/grpo_mlx_train.py`, invoke wrapper, assert it returns `HU_ERR_IO` or `HU_ERR_NOT_SUPPORTED` (NOT `HU_OK` — that would prove the attack succeeded).
- **AC5**: `security-reviewer` subagent reviews the diff, returns PASS (no remaining HIGH/CRITICAL findings on the popen surface).
- **AC6**: Full `rl_sota` suite green (10330+ pass, 0 ASan).

---

## §3 CF-6 — Apple FM / Gemini Nano `unavailable(reason)` strings (Task D-2)

**Owner**: small, isolated FFI plumbing. Risk tier: **Low** (`src/eval/eval_judge_external.c`, only error-path).

### Findings

- `src/eval/eval_judge_external.c:148-164`: `hu_eval_judge_create_apple_fm` and `_gemini_nano` return `HU_ERR_NOT_SUPPORTED` with no detail.
- `include/human/eval/eval_judge_external.h:86-96`: factories take `(alloc, out)` only — no `out_reason`.
- `include/human/core/error.h`: no sidecar mechanism.
- `tests/test_competitive_harness.c:24-25`: hardcodes `"unavailable (no chrome)"` on the slot struct — bypasses the factory.

### D-2 work

1. **Extend factory signature** with optional out-param:
   ```c
   /* If out_reason is non-NULL and the factory returns HU_ERR_NOT_SUPPORTED,
    * writes a static string into *out_reason describing why. The string is
    * statically allocated (no caller-free). NULL out_reason is fine; the
    * factory still returns HU_ERR_NOT_SUPPORTED. */
   hu_error_t hu_eval_judge_create_apple_fm(hu_allocator_t *alloc,
                                            hu_eval_judge_external_t *out,
                                            const char **out_reason);
   hu_error_t hu_eval_judge_create_gemini_nano(hu_allocator_t *alloc,
                                               hu_eval_judge_external_t *out,
                                               const char **out_reason);
   ```

2. **Implement detection** (pure C, no `system()` or `popen()` from inside factory constructors — see H-1 fix):

   **Phase D scope (today, no bridge built):** the C factory checks ONLY the compile-time impl flag and returns a static reason. The runtime probe reasons listed below become reachable LATER when the Swift bridge / Chrome subprocess paths land — they are documented here for forward-compat but are NOT implemented in this phase.

   **Apple FM**: today returns:
   - `"unavailable (not built: HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL=0)"` — always, until Swift bridge lands

   Future (when bridge lands, separate task): `"unavailable (requires macOS 26.0+)"`, `"unavailable (Foundation Models not on this device)"`, `"unavailable (Foundation Models guardrail)"` — all sourced from the Swift bridge's typed error, never from C `system()`.

   **Gemini Nano**: today returns:
   - `"unavailable (not built: HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL=0)"` — always, until Chrome subprocess bridge lands

   Future (when bridge lands, separate task): the bridge process discovers Chrome via `posix_spawn` + `argv` exit codes (NOT `system()`); `unavailable_reason` is populated from the bridge's structured exit code. The C factory NEVER calls `system()` or `popen()` in its constructor path (cold-path safety, no shell injection surface).

   **Rationale**: per critic H-1, calling `system("which chrome")` from a factory constructor is wrong — it inherits the parent's `PATH`, can be CWD-shadowed (same CF-7 class of bug), and runs an interactive shell at module-init time. Detection runtime probes belong in the bridge subprocess, not the factory.

3. **Update all call sites** to pass `NULL` or capture the reason:
   - `src/eval/competitive_harness.c` (if it constructs judges via these factories — recon to verify)
   - Any `cli_demo.c` / `cli_eval.c` site post-CF-1

4. **Tests** (extend `tests/test_eval_judge_external.c`):
   - `test_apple_fm_reports_not_built_reason_when_impl_flag_off`
   - `test_gemini_nano_reports_not_built_reason_when_impl_flag_off`
   - `test_apple_fm_accepts_null_out_reason` — backward-compatible NULL handling
   - `test_competitive_harness_renders_reason_from_factory` — slot's `unavailable_reason` comes from the factory, not a hardcoded string

### D-2 Acceptance Criteria

- **AC1**: Both factories accept `const char **out_reason`; non-NULL out_reason gets a static string on `HU_ERR_NOT_SUPPORTED`; NULL out_reason is gracefully ignored.
- **AC2**: `tests/test_eval_judge_external.c` gains ≥4 new tests, all pass.
- **AC3**: `tests/test_competitive_harness.c` updates to source the reason from the factory (not hardcoded), and the rendered Markdown contains the reason verbatim.
- **AC4**: Full suite green.

---

## §4 CF-5 — KTO HUML finite-diff grad sweep (Task D-3)

**Owner**: small test-hardening, isolated. Risk tier: **Low** (`tests/test_kto_loss.c`).

### Findings

- `tests/test_kto_loss.c::test_kto_loss_finite_diff_matches_analytical` (~lines 98-174, probe at 129-132) is a **branch-consistency** check (positive vs negative branch FD sum), not analytical-vs-numerical.
- No analytical-gradient C helper exists for KTO lm_head (`hu_kto_compute_lm_head_grad_for_test` is referenced in the Phase 3 plan but never landed).
- lm_head shape: 32 × 16 = 512 floats.

### D-3 work

1. **Add analytical-grad helper** `src/ml/kto.c` (gated behind `HU_IS_TEST`):
   ```c
   /* Compute analytical ∂L/∂lm_head for the KTO loss on a fixed pair set.
    * Output is row-major flat array, caller-allocated, vocab*embd floats. */
   #ifdef HU_IS_TEST
   hu_error_t hu_kto_compute_lm_head_grad_for_test(
       const hu_kto_pair_t *pairs, size_t n_pairs,
       const float *lm_head, size_t vocab, size_t embd,
       const hu_kto_params_t *params,
       float *out_grad);
   #endif
   ```
   Implementation reuses the same backward path used by the existing `step()` — we extract it into a public-to-test entry point.

2. **Add per-parameter sweep test** (`tests/test_kto_loss.c::test_kto_loss_analytical_grad_matches_finite_diff_per_param`). Reuses the existing test seam **`kto_compute_loss_only_for_test`** (declared `extern` in `tests/test_kto_loss.c:99`, defined at `src/ml/kto.c:281`) for loss evaluations:
   ```c
   /* Existing forward declaration in tests/test_kto_loss.c — reused: */
   extern hu_error_t kto_compute_loss_only_for_test(void *ctx, ...);
   /* New per-parameter sweep test: */
   for (size_t row = 0; row < vocab; row++) {
       for (size_t col = 0; col < embd; col++) {
           float orig = lm_head[row * embd + col];
           lm_head[row * embd + col] = orig + eps;
           double L_plus = kto_compute_loss_only_for_test(...);
           lm_head[row * embd + col] = orig - eps;
           double L_minus = kto_compute_loss_only_for_test(...);
           lm_head[row * embd + col] = orig;
           double numerical = (L_plus - L_minus) / (2 * eps);
           double analytical = analytical_grad[row * embd + col];
           double rel_err = fabs(numerical - analytical) /
                            fmax(fabs(numerical), fabs(analytical) + 1e-12);
           if (rel_err > 0.05 && fmax(fabs(numerical), fabs(analytical)) > 1e-4)
               HU_FAIL("...");
       }
   }
   ```

3. **Tolerance + skip semantics**:
   - Tolerance: 5% relative error (matches existing GRPO loss test).
   - Skip cells where both analytical and numerical are < 1e-4 (numerical noise floor).
   - Expected runtime: 512 cells × 2 loss evals = 1024 loss evals. Each loss eval on a 32-vocab tiny GPT is ~1ms → total <2s.

### D-3 Acceptance Criteria

- **AC1**: `hu_kto_compute_lm_head_grad_for_test` exposed and used.
- **AC2**: New per-parameter sweep test covers all 512 cells.
- **AC3**: ≥99% of cells (≥507/512) pass the 5% relative-error tolerance. (Allow up to 5 cells below 1e-4 noise floor.)
- **AC4**: New test runtime <5s in `--suite=kto`.
- **AC5**: Existing branch-consistency test stays green (regression guard).

---

## §5 CF-3 — Daemon iMessage reaction poll production wiring (Task D-4)

**Owner**: daemon + reaction-handler integration. Risk tier: **Medium** (`src/daemon.c`, `src/channels/imessage_reactions.c`).

### Findings

- `src/daemon.c::hu_service_run` non-test branch starts at line 2187+. Main channel poll loop at 3905-3916. iMessage watchdog at 12217-12243 (30s cadence, only ticks when `poll_fn == hu_imessage_poll`).
- `hu_imessage_poll_reactions` is **not declared in any public header** under `include/human/`; tests forward-declare it. Implementation: `src/channels/imessage_reactions.c:36-98`. Returns `HU_ERR_NOT_SUPPORTED` when `HU_IS_TEST` or non-Apple or `HU_ENABLE_SQLITE` off.
- `hu_reaction_handler_set_collector` is `src/agent/reaction_handler.c:38`. `hu_reaction_handler_register_assistant_message_for_production` exists; **never called from `src/daemon.c` today.**
- Config: `hu_reaction_collection_config_t` already parsed at `src/config_parse.c:210-234` with `enabled`, `poll_interval_seconds` (default 30), `channels[]`.
- `src/daemon_reaction_poll.c` (entire file `#if HU_IS_TEST`-gated, only built when `HU_ENABLE_RL_FULL`) contains test-only `hu_daemon_tick_for_test`.

### D-4 work

1. **Promote `hu_imessage_poll_reactions` to a public header** — `include/human/channels/imessage_reactions.h` (new):
   ```c
   #ifndef HU_IMESSAGE_REACTIONS_H
   #define HU_IMESSAGE_REACTIONS_H
   #include "human/core/error.h"
   #include "human/channels/reaction_event.h"
   #include <stdint.h>
   #include <stddef.h>

   /* Reads tapback rows from chat.db inserted since since_unix.
    * Fills out[0..min(cap, returned)] with events; sets *out_n.
    * Returns HU_ERR_NOT_SUPPORTED on non-Apple/test/missing SQLite.
    * Returns HU_ERR_IO on chat.db open/prepare failure. */
   hu_error_t hu_imessage_poll_reactions(const char *db_path, int64_t since_unix,
                                          hu_reaction_event_t *out, size_t cap,
                                          size_t *out_n);
   #endif
   ```

2. **Add production tick function** in `src/channels/imessage_reactions.c` (or new TU `src/daemon_reaction_poll_prod.c` if we want symmetry with the test TU). **Per critic NH-1**: the interval-check lives INSIDE this function (not in `hu_service_run`), with an injectable `now_unix` clock parameter. This makes the schedule logic deterministically testable without touching `hu_service_run`.
   ```c
   /* Production version (not HU_IS_TEST-gated). Called from daemon main loop
    * once per outer tick; internally checks whether poll_interval_seconds has
    * elapsed since last_poll_unix and polls iff so. Returns:
    *   HU_OK              — polled (or no-op when not enabled / interval not yet elapsed)
    *   HU_ERR_NOT_SUPPORTED — compile guards off (Linux, non-Apple)
    *   HU_ERR_IO          — chat.db open/prepare failed (caller logs + continues)
    *
    * On a successful poll: increments an internal poll counter (exposed
    * for tests via hu_daemon_reaction_poll_get_count_for_test) and writes
    * now_unix into *last_poll_unix_inout. On a no-op (interval not elapsed),
    * leaves *last_poll_unix_inout unchanged.
    *
    * db_path resolution (per critic H-2):
    *   1. cfg->chatdb_path if non-empty (new config field, see step 7)
    *   2. getenv("HU_CHATDB") for dev override
    *   3. fallback "$HOME/Library/Messages/chat.db" — production default
    *
    * now_unix MUST be a real (or fake-test-clock) unix timestamp in seconds;
    * the function does NOT call time(NULL) internally — that's the caller's
    * job, which lets tests pass a controlled fake clock. */
   hu_error_t hu_daemon_tick_reaction_poll(const hu_reaction_collection_config_t *cfg,
                                            int64_t now_unix,
                                            int64_t *last_poll_unix_inout,
                                            int64_t *watermark_inout);
   ```

3. **Wire into `hu_service_run`** at `src/daemon.c` (non-test branch). After the existing iMessage watchdog block (~12243). **Per critic NM-3**: move state into the daemon context (or `hu_agent_t::sota`) rather than static locals — but for v1 of this plan, static locals are acceptable to keep the diff focused; a follow-up can hoist them:
   ```c
   #if defined(HU_HAS_IMESSAGE) && defined(__APPLE__) && defined(HU_ENABLE_SQLITE)
       /* TODO(post-D-4): hoist out of static locals into agent ctx. */
       static int64_t reaction_watermark = 0;
       static int64_t reaction_last_poll_unix = 0;
       int64_t now_unix = (int64_t)time(NULL);
       if (reaction_watermark == 0) reaction_watermark = now_unix;
       if (config && config->reaction_collection.enabled) {
           (void)hu_daemon_tick_reaction_poll(&config->reaction_collection,
                                               now_unix,
                                               &reaction_last_poll_unix,
                                               &reaction_watermark);
       }
   #endif
   ```
   The interval-check now lives inside `hu_daemon_tick_reaction_poll`, so the daemon loop body is trivial and the schedule logic is unit-testable.

4. **Register collector at daemon init** (in `hu_service_run` early, when `config->reaction_collection.enabled` and the SOTA subsystem is initialized — per critic B-2 fix, the field is `agent->sota.sota_initialized`, NOT `dpo_collector_initialized` which does not exist; `dpo_collector` is an embedded value struct, always valid when `sota_initialized` is true):
   ```c
   if (config && config->reaction_collection.enabled && agent->sota.sota_initialized) {
       hu_reaction_handler_set_collector(&agent->sota.dpo_collector);
   }
   ```
   And clear on shutdown: `hu_reaction_handler_set_collector(NULL)`.

5. **Register outbound assistant messages** — locate where the daemon dispatches outbound chats (channel `send_fn` invocations) and add:
   ```c
   if (config->reaction_collection.enabled && response_text && response_text[0]) {
       hu_reaction_handler_register_assistant_message_for_production(
           channel_id, thread_id, message_ref, prompt_text, response_text);
   }
   ```
   This is the **hardest** part — requires understanding the daemon's outbound message ID flow.

6. **Tests** (`tests/test_daemon_reaction_poll_production.c`, new, gated `HU_ENABLE_RL_FULL`):
   - `test_daemon_tick_no_op_when_reaction_collection_disabled` — verify `*last_poll_unix_inout` unchanged.
   - `test_daemon_tick_returns_not_supported_when_compile_guards_off` — Linux build assertion.
   - `test_daemon_tick_advances_watermark_after_poll` — even when chat.db is bogus (HU_ERR_IO is fine), watermark should still advance to avoid stuck loops.
   - **`test_daemon_tick_polls_only_after_interval_with_fake_clock`** (per critic NH-1 fix): now testable because the interval-check lives in the tick function. Pass `cfg->poll_interval_seconds = 30`, call `hu_daemon_tick_reaction_poll(cfg, now=1000, &last=1000, &watermark)` — assert no poll (last unchanged, counter stays 0). Then call with `now=1030` — assert poll happened (last==1030, counter==1). Then `now=1045` (< 30s elapsed) — assert no poll. Then `now=1060` — assert poll happened (counter==2). Pure deterministic clock advancement; no `clock_gettime` involved.
   - **Skip on non-Apple**: `HU_SKIP_IF(!hu_build_has_imessage(), ...)`

7. **Config knob for db_path** (per critic H-2): add field to `include/human/config.h::hu_reaction_collection_config_t`:
   ```c
   /* Optional override for iMessage chat.db path. If empty, daemon uses
    * getenv("HU_CHATDB"), then falls back to "$HOME/Library/Messages/chat.db".
    * Must be an absolute path; relative paths rejected at config-parse time. */
   char chatdb_path[256];
   ```
   Update `src/config_parse.c::parse_reaction_collection` to parse `reaction_collection.chatdb_path` and reject non-absolute values with `HU_ERR_INVALID_ARGUMENT`. (Mitigates path-traversal risk.)

8. **Linux build safety**: ensure all new daemon hooks compile-out cleanly when `HU_HAS_IMESSAGE` is undefined. CI matrix includes Linux x86_64 — must stay green.

### D-4 Acceptance Criteria

- **AC1**: `hu_imessage_poll_reactions` declared in `include/human/channels/imessage_reactions.h`.
- **AC2**: `src/daemon.c` non-test branch invokes `hu_daemon_tick_reaction_poll` on `poll_interval_seconds` cadence when enabled.
- **AC3**: `hu_reaction_handler_set_collector` is called from `src/daemon.c` at startup (production path, not `#if HU_IS_TEST`). The init guard uses `agent->sota.sota_initialized` (not the phantom `dpo_collector_initialized`).
- **AC4**: `hu_reaction_handler_register_assistant_message_for_production` called from the daemon's outbound dispatch path. (At minimum, in the iMessage channel `send_fn` post-handler.) An audit `rg "register_assistant_message_for_production" src/daemon.c src/channels/imessage*.c` shows at least 1 production-path match.
- **AC5**: New production test file with ≥4 tests (now includes the fake-clock deterministic test), all pass on macOS; cleanly skip on Linux.
- **AC6**: `scripts/agent-preflight.sh src/daemon.c` runs and passes (daemon-touching change).
- **AC7 (revised per spec-verifier H-5)**: `test_daemon_loop_invokes_poll_after_interval_via_fake_clock` passes deterministically — proves the daemon loop actually calls poll on schedule, replacing the subjective `verifier` subagent check.
- **AC8** (new per critic H-2): `tests/test_config_parse.c` gains a regression test asserting `reaction_collection.chatdb_path = "../etc/passwd"` is rejected at parse time with `HU_ERR_INVALID_ARGUMENT`.

---

## §6 CF-4 — Real measured candidate metrics into eval gate (Task D-5)

**Owner**: extracts the production rollout-and-score path from `human ml lora-ab --require-positive` into a shared helper consumed by `lora_training_runner`. Risk tier: **Medium**.

### Findings (revised per critic B-1)

- `src/agent/lora_training_runner.c:107-114` `run_promotion_gate` hardcodes `persona[20] = 0.75`, NULL/NULL/NULL, `p95 = 100.0`.
- **Correction (critic B-1)**: the recon agent's claim that `src/ml/cli.c:1791-1818` (`lora-ab --require-positive`) does live adapter inference is **wrong**. `hu_ml_cli_lora_ab` at line 1734 reads pre-scored JSON files written by an earlier `lora-baseline` invocation; it does NOT load an adapter or run chats. The path with `hu_provider_load_adapter` + `chat_with_system` loop is in **`hu_ml_cli_lora_runner` at line 2070+**. That is the function to refactor.
- The real composition target: `hu_ml_cli_lora_runner` already does (a) load persona examples, (b) optionally `hu_provider_load_adapter`, (c) loop `chat_with_system` per input, (d) emit JSON array of responses. Extract steps (b)+(c) into a shared helper, then add (d') per-response v2 fidelity scoring + p95 latency capture.
- `hu_e2e_closed_loop_run` (test seam) already computes `hu_rl_trainer_metrics_t metrics` but doesn't propagate it.
- **Per critic B-3**: the public header is `include/human/agent/lora_runner.h` (not the phantom `lora_training_runner.h`). The struct is `hu_lora_runner_ctx_t` at `lora_runner.h:67`.

### D-5 work

1. **Add shared helper** `src/eval/persona_rollout.{h,c}` (new):
   ```c
   /* Run a candidate adapter against a fixed prompt fixture, score each
    * response with v2 fidelity, return persona_scores[] + p95 latency.
    *
    * Caller owns alloc; helper allocates persona_scores via alloc and
    * stores the pointer in *out_scores. Caller must free.
    *
    * If adapter_path is NULL or empty, uses the provider's base model
    * (baseline measurement path). */
   typedef struct hu_persona_rollout_config {
       hu_provider_t *provider;            /* loaded base model */
       const char *adapter_path;           /* optional candidate adapter */
       const hu_communication_style_t *target;  /* persona fingerprint */
       const char **prompts;
       const char **system_prompts;        /* optional; NULL means use defaults */
       size_t n_prompts;
       int64_t timeout_ms_per_prompt;      /* per-chat budget */
   } hu_persona_rollout_config_t;

   typedef struct hu_persona_rollout_result {
       double *persona_scores;             /* alloc-owned, size n_prompts */
       size_t n_scored;                    /* may be < n_prompts on errors */
       double p95_ms;                      /* 95th percentile chat duration */
       double mean_ms;
       size_t n_errors;
   } hu_persona_rollout_result_t;

   hu_error_t hu_persona_rollout_run(hu_allocator_t *alloc,
                                      const hu_persona_rollout_config_t *cfg,
                                      hu_persona_rollout_result_t *out);
   void hu_persona_rollout_result_free(hu_allocator_t *alloc,
                                        hu_persona_rollout_result_t *r);
   ```

2. **Refactor `hu_ml_cli_lora_runner` at `src/ml/cli.c:2070+`** (per critic B-1 — NOT `lora-ab` at line 1734): extract the `hu_provider_load_adapter` + `chat_with_system` loop into the new `hu_persona_rollout_run` helper. The handler keeps its argument parsing + JSON emission; loop body delegates.

3. **Rewrite `src/agent/lora_training_runner.c::run_promotion_gate`** to:
   - Acquire prompt fixture from `ctx->eval_prompt_fixture_path` (new field; default `~/.human/eval/persona_prompts.txt`)
   - Load adapter at `report.adapter_path`
   - Acquire persona target via `ctx->eval_target` (new field) or load from `personal_model.bin`
   - Call `hu_persona_rollout_run`
   - Feed `result.persona_scores`, `n_scored`, `result.p95_ms` into `hu_eval_gate_decide_from_arrays_for_test` (gate currently exports only the `_for_test` symbol — fine, it's the canonical gate entry-point)

4. **Add new fields to `hu_lora_runner_ctx_t`** (in **`include/human/agent/lora_runner.h`** — per critic B-3, NOT `lora_training_runner.h` which does not exist; the struct is at `lora_runner.h:67`):
   ```c
   /* Required for real (non-synthetic) gate measurement.
    * Per spec-verifier H-3: when eval_gate is non-NULL AND HU_ENABLE_RL_FULL,
    * eval_provider + eval_prompt_fixture_path + eval_target are MANDATORY.
    * Missing them is HU_ERR_INVALID_ARGUMENT, not a synthetic fallback —
    * the synthetic fallback is exactly the inflation CF-4 closes. */
   struct hu_provider *eval_provider;
   const char *eval_prompt_fixture_path;
   const struct hu_communication_style *eval_target;
   size_t eval_n_prompts;        /* default 20; min 10 (gate floor) */
   int64_t eval_timeout_ms;       /* per-chat budget; default 5000 */
   ```

5. **Synthetic-array path retirement** (per spec-verifier H-3): the old synthetic `persona[20] = {0.75}` path is removed from the production code path. It is preserved ONLY behind `#ifdef HU_IS_TEST` AND `ctx->eval_use_synthetic_for_test == true` (new bool, default false, never set by production). Tests that don't care about the gate inputs (e.g., `test_runner_skips_gate_when_eval_gate_is_null`) continue working unchanged. Tests that need the synthetic path (e.g., the existing `test_runner_blocks_promotion_when_gate_rejects` if not migrated) opt in explicitly.

6. **Tests**:
   - `tests/test_persona_rollout.c` (new): rollout against canned provider, assert scores in [-1, 1], p95 reasonable, NULL inputs rejected.
   - **Update** `tests/test_lora_training_runner_eval_gate.c::test_runner_blocks_promotion_when_gate_rejects` to pass real eval provider+target+prompts; assert the gate sees real scores (mean < baseline) rather than the hardcoded 0.75 array.
   - Add `test_runner_returns_invalid_argument_when_eval_provider_missing_with_gate` — production-path regression guard (proves we don't silently fall back).
   - Keep `test_runner_uses_synthetic_when_test_flag_set` as the explicit-opt-in test for the legacy path.

### D-5 Acceptance Criteria

- **AC1**: `hu_persona_rollout_run` exists, tested, used from at least 2 call sites: `src/ml/cli.c::hu_ml_cli_lora_runner` (line 2070+) and `src/agent/lora_training_runner.c::run_promotion_gate`.
- **AC2 (revised per spec-verifier H-3)**: `run_promotion_gate` in production code path NEVER constructs the literal `persona[20] = {0.75, ...}` array. Audit method: `rg "persona\[20\]" src/agent/lora_training_runner.c` returns matches ONLY inside `#ifdef HU_IS_TEST` blocks.
- **AC3**: `tests/test_lora_training_runner_eval_gate.c` updated `test_runner_blocks_promotion_when_gate_rejects` passes real scores through the gate (assert the gate input array is NOT all-0.75 — e.g., assert at least 2 scores differ).
- **AC4**: `hu_communication_style_fidelity_score_v2` is the scoring function (NOT v1).
- **AC5**: New test `test_runner_returns_invalid_argument_when_eval_provider_missing_with_gate` proves production gate path rejects synthetic fallback.
- **AC6**: `eval_n_prompts >= 10` (gate's hard floor in `eval_gate.c:45-46`); rollout errors out if config requests fewer.
- **AC7**: Full suite green.
- **AC8**: **MUST land before CF-1 (D-6)** — verified by commit SHA in D-6's commit message.

---

## §7 CF-1 — Wire `human eval competitive/_gate/_leaderboard` to real backends (Task D-6)

**Owner**: depends on D-5 (Task D-5 ships the rollout helper). Risk tier: **Medium**.

### Findings

- `src/eval/cli_eval.c:13-40` is three printf stubs.
- `src/eval/competitive_harness.c` emits Markdown + minimal JSON (no bootstrap CIs).
- `src/eval/leaderboard.c` has `lb_run` returning canned scores keyed by prompt+kind.
- `tests/test_cli_eval_phase5.c` exists but **only tests `--help`** today (1 test). All headroom to add real wiring tests.

### D-6 work

1. **Wire `hu_eval_cli_competitive`** at `src/eval/cli_eval.c`:
   - Parse `--persona <name>`, `--adapter <path>`, `--out-md <path>`, `--out-json <path>`, `--prompts <fixture>`, `--min-available N`
   - Load persona profile + communication-style target (same path as `lora-ab`)
   - Call `hu_persona_rollout_run` (from D-5) against the base provider and the candidate adapter
   - Construct `hu_competitive_harness_judge_slot_t[]` — one slot per backend (stock/apple-fm/gemini-nano), each populated via the CF-6 factories
   - Call `hu_competitive_harness_run_with_test_judges`
   - **Extend the harness output JSON** to include per-column persona-fidelity score, mean, and bootstrap CI lower/upper (via `hu_bootstrap_ci`)

2. **Wire `hu_eval_cli_gate`** at `src/eval/cli_eval.c`:
   - Parse `--persona-scores <csv>`, `--persona-baseline <f>`, `--persona-delta-min <f>`, `--bootstrap-samples <n>`, `--bootstrap-seed <n>`, `--candidate-p95-ms <f>`, `--latency-baseline-ms <f>`, `--out <path>`
   - Construct `hu_eval_gate_t` from baselines, call `hu_eval_gate_decide_from_arrays_for_test`
   - Emit `hu_eval_gate_verdict_t` as JSON to `--out` (or stdout)

3. **Wire `hu_eval_cli_leaderboard`** at `src/eval/cli_eval.c`:
   - Parse `--kind {mt-bench, ifeval, alpaca}`, `--canned`, `--prompts <fixture>`, `--out <path>`
   - When `--canned`, instantiate the canned-runner factories (`hu_leaderboard_create_mt_bench` etc.) — they exist in `leaderboard.h`
   - Run against prompts fixture, emit scores

4. **Extend competitive harness JSON schema** (`src/eval/competitive_harness.c`) — per spec-verifier B-6, add `win_condition_met` field at the top level:
   ```json
   {
     "summary": "...",
     "available": 2,
     "n_columns": 3,
     "win_condition_met": true,
     "win_condition_rationale": "persona_fidelity beats stock by 0.10 [0.04, 0.16 CI]",
     "columns": [
       {"name": "stock", "available": true, "persona_fidelity": 0.62,
        "ci_lower": 0.55, "ci_upper": 0.69, "n_samples": 30, "p95_ms": 234.5,
        "is_baseline": true},
       {"name": "candidate", "available": true, "persona_fidelity": 0.72,
        "ci_lower": 0.65, "ci_upper": 0.79, "n_samples": 30, "p95_ms": 251.0,
        "delta_vs_baseline": 0.10, "delta_ci_lower": 0.04, "delta_ci_upper": 0.16},
       {"name": "apple_fm", "available": false,
        "unavailable_reason": "unavailable (not built: HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL=0)"}
     ]
   }
   ```

   Where `win_condition_met` is true when the candidate's CI lower bound strictly exceeds the baseline's CI upper bound on `persona_fidelity` (matches spec §9 "win-condition scorecard"). Markdown output also surfaces this prominently.

5. **Tests** (extend `tests/test_cli_eval_phase5.c` from 1 → ≥6 tests; gate on `HU_ENABLE_RL_FULL`):
   - `test_competitive_emits_scorecard_with_bootstrap_cis` — assert JSON contains `ci_lower`/`ci_upper`
   - `test_competitive_renders_unavailable_columns_with_reason` — reason came from CF-6 factory
   - `test_gate_emits_verdict_json_from_csv_inputs` — pipe `--persona-scores=0.7,0.75,0.8,...` through `--out`, assert verdict
   - `test_leaderboard_canned_run_emits_scores` — `--kind mt-bench --canned --out /tmp/lb.json`
   - `test_unknown_flag_returns_invalid_argument`
   - `test_missing_required_flag_returns_invalid_argument`

### D-6 Acceptance Criteria

- **AC1**: `hu_eval_cli_competitive` produces a JSON file at `--out-json` with per-column persona fidelity + bootstrap CIs. JSON schema matches the literal schema in §7 step 4 (verified by a test that parses every named field).
- **AC2**: `hu_eval_cli_gate` produces a verdict JSON file with `promote`, `persona_pass`, all CI bounds (matches `hu_eval_gate_verdict_t` field-for-field).
- **AC3 (revised per spec-verifier B-6)**: `hu_eval_cli_leaderboard --canned --kind mt-bench --out <path>` produces a JSON file whose schema includes `kind`, `prompts` array, `scores` array (one float per prompt), and `mean`. Test parses every field.
- **AC4**: 5+ new tests in `tests/test_cli_eval_phase5.c`, all pass.
- **AC5 (new per spec-verifier B-6)**: New test `test_competitive_emits_win_condition_met_field` asserts the scorecard JSON contains `win_condition_met` (bool) AND `win_condition_rationale` (string non-empty).
- **AC6 (new per spec-verifier B-6)**: New test `test_competitive_literal_spec9_form_works` invokes the exact spec §9 form `human eval competitive --persona seth --adapter /tmp/test.adapter --out-json /tmp/scorecard.json` (test fixture seth persona + canned adapter) and asserts exit code 0 + scorecard file exists. This proves DoD-9's literal command form works end-to-end.
- **AC7**: `ship-contract DoD-9` re-verifies to PASS (no longer PARTIAL).
- **AC8**: **MUST land after CF-4 (D-5) and after CF-6 (D-2)** — verified by commit ordering. CF-1 closed without CF-4 = inflation (CLI scorecard backed by synthetic gate inputs). CF-1 closed without CF-6 = scorecard renders NULL reasons.

---

## §8 CF-2 — Populate 5 demo evidence files + drop hard-coded literals (Task D-7)

**Owner**: depends on D-5 (persona rollout) + D-6 (eval gate verdict source). Risk tier: **Medium**.

### Findings

- `src/ml/cli_demo.c:73-121` writes 9 files, of which 5 are `write_stub(path, "{}\n")`:
  - `training_curves.json`, `eval_before.json`, `eval_after.json`, `eval_delta.json`, `adversarial_review.md`
- `reproduce.sh` written as 25-byte `"#!/bin/sh\necho reproduce\n"` placeholder.
- Hard-coded: `run->persona_delta = 0.06` (line 222), `gate_decision.reason = "demo"` (line 112).
- `hu_e2e_closed_loop_output_t` doesn't carry training curves, eval, or gate — must be extended or a new evidence-bundle struct introduced.

### D-7 work

1. **Extend `hu_e2e_closed_loop_output_t`** (or introduce `hu_demo_evidence_bundle_t` — TBD per critic review) to carry:
   - `hu_rl_trainer_metrics_t metrics` (already computed at line 217-218; just propagate)
   - `hu_persona_rollout_result_t baseline_result` (eval_before — base model on prompts)
   - `hu_persona_rollout_result_t candidate_result` (eval_after — candidate adapter)
   - `hu_eval_gate_verdict_t gate_verdict` (real verdict from real gate call)
   - Delta computed: per-prompt `candidate - baseline`, plus bootstrap CI on the difference

2. **Update `cli_demo_run_closed_loop`** to:
   - After training, run `hu_persona_rollout_run` twice (baseline + candidate) on a fixture of **exactly 10 prompts** (per critic H-6: matches the gate's hard `n >= 10` floor in `eval_gate.c:45-46`; spec §8 calls for 20 prompts in `delta_responses.md` which is acknowledged as a residual gap — see §10 and new carry-forward CF-2-R)
   - Construct `hu_eval_gate_t` with reasonable defaults
   - Call `hu_eval_gate_decide_from_arrays_for_test` with real persona arrays
   - Store everything in the bundle struct

3. **Update `write_evidence_dir`** to emit real content for the 5 stub files:
   - **`eval_before.json`**: `{"persona_fidelity": {"mean": 0.62, "ci_lower": 0.55, "ci_upper": 0.69, "n": 10, "scores": [...]}, "p95_ms": 234.5}`
   - **`eval_after.json`**: same schema, candidate results
   - **`eval_delta.json`**: `{"persona_delta": {"mean": 0.10, "ci_lower": 0.05, "ci_upper": 0.15}, "p95_delta_ms": -12.3}`
   - **`training_curves.json`**: `{"final_loss": <metrics.final_loss>, "iters_completed": <metrics.iters_completed>, "logprob_delta_chosen_minus_rejected": <metrics....>}` — limited (single-step demo), but real values
   - **`adversarial_review.md`**: Markdown rendering of competitive harness output (canned judge fallback when external judges unavailable; lists each column + reason if any)

4. **Replace literals**:
   - `manifest.json::persona_delta` ← `bundle.gate_verdict.persona_pass ? bundle.delta_mean : 0.0`
   - `gate_decision.json` ← full verdict serialization:
     ```json
     {"promote": <verdict.promote>, "persona_pass": <...>,
      "persona_ci_lower": <...>, "persona_ci_upper": <...>,
      "reason": "<verdict.reason>"}
     ```

5. **Real `reproduce.sh`** (~10 lines):
   ```sh
   #!/usr/bin/env bash
   set -euo pipefail
   # Re-runs the closed loop with the same seeds and inputs as the
   # artifacts in this directory. Requires HU_E2E_FIXED_TIMESTAMP for
   # byte-stable output.
   cd "${REPO_ROOT:-$PWD}"
   cmake --build build-rl-sota -j
   export HU_E2E_FIXED_TIMESTAMP=1
   exec ./build-rl-sota/human demo rl-closed-loop \
       --backend huml --reaction-count 50 \
       --prompt "what should i do first?" \
       --out "$(pwd)/rerun-$(date +%s)"
   ```

6. **Tests** (extend `tests/test_e2e_rl_loop.c` and/or add `tests/test_cli_demo_evidence.c`):
   - `test_demo_writes_real_eval_before_after_with_persona_scores` — parse `eval_before.json`, assert `persona_fidelity.scores` array length >0 and values in [-1, 1]
   - `test_demo_eval_delta_includes_bootstrap_ci` — assert `ci_lower < mean < ci_upper`
   - `test_demo_gate_decision_matches_real_verdict` — assert `gate_decision.json::promote` matches what `hu_eval_gate_decide_*` returns for the same inputs
   - `test_demo_training_curves_has_real_metrics` — assert `final_loss` is a real float, not literal 0
   - `test_demo_reproduce_sh_is_executable_and_nonplaceholder` — file size > 100 bytes, contains `cmake`

7. **Update `scripts/validate-rl-sota.sh`** to assert all 9 files are non-empty (currently only checks manifest + gate_decision exist).

### D-7 Acceptance Criteria

- **AC1**: None of the 9 files are 3-byte `{}` stubs (size > 50 bytes for each). `reproduce.sh` is > 100 bytes and parses as POSIX shell.
- **AC2**: `manifest.json::persona_delta` is sourced from the real gate verdict, not literal `0.06`. Audit method: `rg "persona_delta = 0\." src/ml/cli_demo.c` returns zero matches.
- **AC3**: `gate_decision.json` is the full `hu_eval_gate_verdict_t` serialization including all CI bounds. Test parses every named field.
- **AC4**: New test asserting non-stub content for each of the 5 previously-stub files (parse JSON; assert mean ∈ [-1, 1] for fidelity scores).
- **AC4a (new per critic NM-2 — cargo-cult guard)**: New test `test_demo_evidence_values_are_not_plan_example_literals` asserts the actual emitted JSON values are NOT identical to the literal example values shown in this plan's §8 step 3 (e.g., `eval_before.json::persona_fidelity.mean != 0.62`, `eval_after.json::persona_fidelity.mean != 0.72`, `eval_delta.json::persona_delta.mean != 0.10`). This prevents an implementer from copy-pasting the plan's example JSON as the actual implementation. The plan's examples are illustrative; the real values must come from running the rollout helper.
- **AC5**: `scripts/validate-rl-sota.sh` updated to validate all 9 files (size + JSON parse where applicable).
- **AC6 (revised per critic B-4 + spec-verifier B-6)**: `ship-contract DoD-10` re-verifies to **PASS_WITH_NOTES** (NOT PASS) — the literal honest verdict, given that:
  - Five previously-stub files now have real content (the main half of DoD-10 closes).
  - The `reproduce.sh` placeholder is replaced with a real 10-line script.
  - The `manifest.json::persona_delta=0.06` and `gate_decision.json` literals are gone.
  - **But**: `delta_responses.md` uses 10 prompts (gate floor); spec §8 explicitly calls for 20. This residual is tracked as **CF-2-R** in `docs/proof/rl-loop-shipcontract.md` Open carry-forwards, scheduled for a future "demo polish" sprint. Closing CF-2-R requires either raising the gate-fixture prompt count to 20 (modest runtime cost) OR documenting the 10-vs-20 deviation as intentional.
- **AC7**: New residual carry-forward CF-2-R added to `docs/proof/rl-loop-shipcontract.md` Open carry-forwards section with the gap description above.
- **AC8**: **MUST land after CF-1 (D-6)** which lands after CF-4 (D-5).

---

## §9 Validation matrix

After each task lands:

```bash
cd /Users/sethford/Documents/h-uman/.worktrees/pr-109-merge
cmake --build build-rl-sota -j$(sysctl -n hw.ncpu)
./build-rl-sota/human_tests
# expect: 10330+ PASS, 0 ASan, 0 UBSan, 0 leaks
# the new tests add ~25 more pass entries → target 10355+ PASS
```

Task-specific suites for fast iteration:

| Task | Suite filter | Expected new tests |
|------|--------------|--------------------|
| D-1 (CF-7) | `--suite=ml_scripts_dir` + `--suite=grpo_mlx` + `--suite=kto_loss` + `--suite=dpo_real_mlx` | +5 (resolver) + 3 (regression) |
| D-2 (CF-6) | `--suite=eval_judge_external` + `--suite=competitive_harness` | +4 |
| D-3 (CF-5) | `--suite=kto_loss` | +1 (per-param sweep) |
| D-4 (CF-3) | `--suite=daemon_reaction_poll_production` + `--suite=config_parse` | +4 (incl. fake-clock deterministic test) + 1 (chatdb_path traversal guard) |
| D-5 (CF-4) | `--suite=persona_rollout` + `--suite=lora_training_runner_eval_gate` | +3 + 2 modified |
| D-6 (CF-1) | `--suite=cli_eval_phase5` | +5 |
| D-7 (CF-2) | `--suite=e2e_rl_loop` + `--suite=cli_demo_evidence` | +5 |

**Total new test count: ~30+ tests on top of 10330.**

End-gate checks (before tagging `rl-sota-phase-d-cf-closure-complete`):

1. Full `human_tests` suite green (≥10355 PASS, 2 SKIP, 0 ASan, 0 UBSan, 0 leaks)
2. `scripts/agent-preflight.sh` clean (auto-detects what changed)
3. `dead-code-finder` subagent reports no new dead code
4. `security-reviewer` subagent on the CF-7 + CF-3 changes
5. 5-aspect-panel review on D-5 + D-6 + D-7 (correctness, edge-case, security, regression, style)
6. `spec-verifier` on each CF closure against this plan's AC sections
7. `sprint-auditor` independent close-out audit on the entire D phase

---

## §10 Honest scope boundaries (what this phase does NOT do)

To avoid the same inflation pattern that the program-level close-out hit:

- **D-4 does NOT** add a persistent watermark to `~/.human/state/`. Watermark is in-process only — daemon restart re-processes last interval of reactions. Persistent watermark is a future task.
- **D-7 does NOT** populate `delta_responses.md` for 20 fixed prompts (design spec §8 calls for 20). Demo uses **exactly 10 prompts** (matches gate's `n >= 10` floor; H-6 fix). The 20-prompt expansion is tracked as the new residual **CF-2-R** in `rl-loop-shipcontract.md` after this phase, NOT pre-closed.
- **D-7 does NOT** close DoD-10 to PASS. It closes to **PASS_WITH_NOTES** with the 10-vs-20 gap as residual CF-2-R (per critic B-4 + spec-verifier).
- **D-6 does NOT** add a `--reference` flag for the competitive subcommand. The recon found tests don't expect it; spec §9 doesn't require it.
- **D-2 does NOT** ship the real Apple FM Swift bridge or Gemini Nano Chrome subprocess. It only ships the `out_reason` plumbing. The bridges remain `HU_ERR_NOT_SUPPORTED`, just with explanatory strings sourced from the compile-time impl flag. Runtime probes (chrome detection, macOS version detection) are deferred to the future bridge sprint — they MUST NOT be implemented via `system()` or `popen()` calls inside the factory constructor (H-1 fix).
- **D-3 does NOT** add a sampled-cell or randomized-cell variant. Full 512-cell sweep only (deterministic).
- **D-1 does NOT** convert `popen` to `posix_spawn`. That's a larger refactor; today's fix is "make the script path absolute and not CWD-shadowable", which is the actual CF-7 ask.
- **No phase-level new feature**. Only closes the seven open carry-forwards, with one residual (CF-2-R) honestly documented.

---

## §11 Sequencing summary (commit order — revised per H-4 + spec-verifier CF-6 finding)

```
1. D-1 (CF-7) — security HIGH foundation         → commit, push
2. D-3 (CF-5) — KTO grad sweep                   → commit, push (parallel-safe with D-4)
3. D-4 (CF-3) — daemon production wiring         → commit, push (parallel-safe with D-3)
4. D-2 (CF-6) — judge unavailable reasons        → commit, push (MUST precede D-6)
5. D-5 (CF-4) — persona-rollout helper           → commit, push (MUST precede D-6)
6. D-6 (CF-1) — CLI wiring                       → commit, push (MUST precede D-7; depends on D-2 + D-5)
7. D-7 (CF-2) — demo evidence content            → commit, push
8. End-gate: full suite + 5-panel + sprint-auditor
9. Update ship-contract / audit-report / proof index — mark CF-1..CF-7 closed, add CF-2-R residual
10. Tag rl-sota-phase-d-cf-closure-complete
11. PR #109 picks up new commits automatically
```

Each numbered step is one commit. No mixed-concern commits. Steps 2 + 3 can be done in parallel (separate worktrees) for wall-clock speed; the others must be serial.

**Dependency chain (mandatory)**:
- D-6 depends on D-2 (CF-6 reasons surface in CF-1 scorecard).
- D-6 depends on D-5 (CF-4 helper feeds CF-1 scorecard with real scores).
- D-7 depends on D-6 (CF-2 demo uses the same eval helpers + gate plumbing CF-1 ships).
- D-1 should land first because CF-3 (D-4) and the eval pipeline (D-5+) all ultimately touch popen surfaces; better to land the resolver early.

---

## §12 Cross-references

- Open carry-forwards table: `docs/proof/rl-loop-shipcontract.md` §Open carry-forwards (honest), lines 255-271
- Co-closure note: `docs/proof/rl-loop-shipcontract.md` line 271
- Program-level audit log: `docs/proof/adversarial-audit-report.md` §Program-level close-out audit
- Umbrella plan: `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md` §Status table (will gain a Phase D row on close)
- Master track: `docs/plans/2026-05-10-master-follow-through-program.md` Phase D3
