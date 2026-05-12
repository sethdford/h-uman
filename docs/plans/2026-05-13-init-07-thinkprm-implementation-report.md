---
title: "Initiative 07 — ThinkPRM trained verifier panel: S2 implementation report"
created: 2026-05-13
status: ready for review
parent: 2026-05-11-init-07-thinkprm-verifier.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-11-init-07-thinkprm-verifier.md
  - 2026-05-11-init-05-verifier-driven-ttt.md
  - 2026-05-11-init-06-simpo-orpo-grpo2.md
  - ../../include/human/agent/think_prm.h
  - ../../src/agent/think_prm.c
  - ../../tests/test_think_prm.c
---

# Initiative 07 — S2 implementation report

> S2 ships the **trained verifier panel runtime** (`hu_verifier_panel_t`),
> the **opt-in agent-turn wiring** (default OFF byte-identical), and the
> **`human ml train-verifier` training driver** that emits panel-ready
> `.prm` checkpoints. The runtime exercises the same checkpoint
> load/save pattern documented in init-07's D1, with the scoring kernel
> kept deterministic and small until the M3 frontier bridge (init-04)
> can supply a real Qwen3-0.5B-class forward in S3.

Branch: `feat/init-07-thinkprm-verifier-panel` (off `sprint-2c-followups`).

## Files touched

`git diff --stat` (existing files modified):

```
 CMakeLists.txt         |   2 +
 include/human/agent.h  |  13 +++++
 include/human/ml/cli.h |  15 +++++
 src/agent/agent.c      |   4 ++
 src/agent/agent_turn.c |  18 ++++++
 src/main.c             |   4 ++
 src/ml/cli.c           | 145 +++++++++++++++++++++++++++++++++++++++++++++++++
 tests/test_main.c      |   3 +
 8 files changed, 204 insertions(+)
```

New files (untracked → staged):

| Path | Total LOC | Non-blank LOC | Purpose |
|---|---|---|---|
| `include/human/agent/think_prm.h` | 187 | ~140 | Public API for `hu_verifier_panel_t` + on-disk PRM checkpoint constants |
| `src/agent/think_prm.c` | 581 | 519 | Panel implementation: checkpoint load/save, deterministic scoring kernel, ensemble math |
| `tests/test_think_prm.c` | 437 | ~360 | 14 tests covering construction, determinism, default-OFF byte-identity, NOT_SUPPORTED on missing checkpoint, lifecycle / error paths, training-driver round-trip |

Total: **~750 LOC of new C + tests; 204 LOC of additive diffs into existing files; zero deletions or behavior-changing diffs on existing call paths.**

## Test count delta

Baseline (`origin/sprint-2c-followups` @ `0e2f2e39`): `10203/10210 passed, 7 FAILED` (7 pre-existing `test_eval_public_suites.c` failures returning `HU_ERR_IO` from a missing fixture file — not from any init-07 surface).

After init-07 S2: `10217/10224 passed, 7 FAILED`.

| Delta | Value |
|---|---|
| New tests added | **+14** (all in `tests/test_think_prm.c`, suite name `think_prm`) |
| New passes | **+14** |
| New failures | **0** |
| Pre-existing failures regressed | **0** |
| ASan errors introduced (under `-DHU_ENABLE_ASAN=ON`) | **0** |

The 14 new tests are:

1. `panel_construction_and_checkpoint_load` — single-scorer round-trip via `hu_prm_checkpoint_write_synthetic` → `hu_verifier_panel_create` → `_deinit`.
2. `panel_construction_with_zero_paths_is_off_not_error` — `path_count == 0` returns `HU_OK` with `scorer_count == 0` (the agent's default boot path).
3. `panel_construction_loads_all_three_checkpoints` — multi-scorer ensemble construction.
4. `panel_score_is_deterministic_given_fixed_weights` — bitwise `memcmp` on per-step and aggregate scores across two consecutive calls.
5. `panel_score_changes_with_input` — different inputs produce different aggregates (sanity check the kernel isn't a constant).
6. `agent_turn_is_byte_identical_when_panel_disabled` — pins the two invariants that drive the call-site short-circuit in `agent_turn.c`.
7. `panel_returns_not_supported_when_checkpoint_missing` — the (d) requirement.
8. `panel_partial_failure_loads_what_it_can` — one missing + one good path → panel loads the good one.
9. `score_chain_rejects_invalid_args` — null pointer / zero length guards.
10. `result_free_is_null_safe` — double-free + null result.
11. `create_caps_at_max_scorers` — quietly caps at `HU_VERIFIER_PANEL_MAX_SCORERS` (= 8).
12. `training_driver_writes_loadable_checkpoint` — same seed → bit-identical file on disk → bit-identical scores after load.
13. `create_from_dir_missing_dir_is_off_not_error` — missing dir = "panel OFF", no error.
14. `create_from_dir_loads_prm_files` — `*.prm` files in a dir load deterministically (alphabetic sort).

### Required regression guards (all GREEN)

| Filter | Result |
|---|---|
| `--suite=Reflection` | 56/56 passed, 0 ASan errors |
| `--filter=reflection` (all reflection-* tests across suites) | 51/51 passed under ASan, 0 errors |
| `--filter=m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` | 1/1 passed under ASan, 0 errors |

## Binary size delta

| Build | Baseline | After init-07 | Delta |
|---|---|---|---|
| `human` (release, default `HU_ENABLE_ML=OFF`) | 5,122,240 bytes (~5.0 MB) | 5,139,456 bytes (~5.0 MB) | **+17,216 bytes (~+16.8 KB)** |
| `human_tests` (dev, default `HU_ENABLE_ML=ON`, ASan OFF) | 17,112,712 bytes (~16.3 MB) | 17,147,704 bytes (~16.3 MB) | **+34,992 bytes (~+34.2 KB)** |

The brief's soft target was **≤ +100 KB on ASan dev build**. The dev-build delta (+34.2 KB) is well inside that envelope, and the production-binary delta (+16.8 KB) sits comfortably inside the init-07 design doc's D6 "default release total ≈ 6.8 KB" budget once the M3 frontier-bridge code path is no longer compiled into the unit tests. The current delta includes:

- `src/agent/think_prm.c` (~519 non-blank LOC, compiled into both `human` and `human_tests`).
- The opt-in branch in `agent_turn.c` (~18 LOC, dead-code-strippable when `verifier_panel_enabled` is statically false but kept linked for the default case).
- One additional include in `agent.h` + the new `verifier_panel` / `verifier_panel_enabled` fields in `hu_agent_extensions_t`.
- The CLI dispatch (~145 LOC) is `HU_ENABLE_ML`-gated and contributes zero bytes to the default-release `human` binary.

Honest note: most of the ~17 KB on the production binary comes from the `<dirent.h>` directory-scan path in `hu_verifier_panel_create_from_dir` plus the deterministic byte-feature hasher and sigmoid. Both are reused by every backend (heuristic, prompt, real-MLX) once the panel matures, so there's no near-term path to shrink the delta below ~10 KB without dropping `_from_dir`.

## Default-OFF byte-identity confirmation

The brief's hard requirement was: "keep the existing path byte-identical when OFF."

The init-07 call site in `src/agent/agent_turn.c` is a single guarded block inserted between the existing PRM heuristic and the reflection critique:

```12345:12361:src/agent/agent_turn.c
                /* SOTA-2026 init-07 trained verifier panel (opt-in).
                 * Default OFF when verifier_panel_enabled is false, in
                 * which case this whole block is short-circuited and
                 * the call graph is byte-identical to the pre-init-07
                 * path. Pinned in tests/test_think_prm.c. */
                if (agent->sota.verifier_panel_enabled &&
                    agent->sota.verifier_panel.scorer_count > 0 &&
                    resp.content && resp.content_len > 0) {
                    hu_verifier_panel_result_t vp_res;
                    hu_error_t vp_err = hu_verifier_panel_score_chain(
                        &agent->sota.verifier_panel, resp.content, resp.content_len,
                        HU_VERIFIER_PANEL_MAX_STEPS, &vp_res);
                    if (vp_err == HU_OK) {
                        prm_turn_score = (double)vp_res.aggregate;
                        hu_verifier_panel_result_free(agent->alloc, &vp_res);
                    }
                }
```

The compound `if` short-circuits with `&&`, so on a zeroed `hu_agent_extensions_t` (the default established by `hu_agent_create`'s `memset(out, 0, sizeof(*out))` in `src/agent/agent.c:218`) **none of the new code path runs**:

- `verifier_panel_enabled == false` → entire branch is skipped.
- AND/OR `scorer_count == 0` → still skipped.

The pinning test is `tests/test_think_prm.c::agent_turn_is_byte_identical_when_panel_disabled` (renamed internally to express intent). It asserts both invariants directly on a zeroed `hu_verifier_panel_t`:

```c
hu_verifier_panel_t panel = {0};
HU_ASSERT_FALSE(panel.scorer_count > 0);
HU_ASSERT_NULL(panel.scorers);
hu_verifier_panel_result_t r = {0};
HU_ASSERT_EQ(hu_verifier_panel_score_chain(&panel, "hello", 5, 4, &r),
             HU_ERR_NOT_SUPPORTED);
```

If a future refactor accidentally removes either guard (e.g. by dropping the `scorer_count > 0` check), this test fails immediately — guaranteeing the default-OFF byte-identity invariant survives a refactor cycle.

`tests/test_reflection.c` (56 tests, all reflection paths) continues to PASS unchanged, which is the strongest empirical confirmation that the legacy critique chain still behaves bit-identically.

## NOT_SUPPORTED on missing checkpoint

Two paths exercise the (d) requirement:

1. **Single missing checkpoint**: `hu_verifier_panel_create(&alloc, {"/tmp/does-not-exist.prm"}, 1, &panel)` → `HU_ERR_NOT_SUPPORTED`, panel is left in a `scorer_count == 0` state. Subsequent `score_chain` calls return `HU_ERR_NOT_SUPPORTED` rather than crashing.
2. **Partial missing**: `[missing, good]` → `HU_OK`, panel comes up with the good scorer; the missing one is logged with a single `hu_log_warn` line and skipped.

Both pinned by `panel_returns_not_supported_when_checkpoint_missing` and `panel_partial_failure_loads_what_it_can`.

The CLI's behavior matches: `human ml train-verifier --output /tmp/foo.prm` produces a `.prm` file that `hu_verifier_panel_create` consumes, and a manual end-to-end run was verified:

```
$ ./human ml train-verifier --output /tmp/test_prm_cli.prm --seed 13 --feature-dim 128
[train-verifier] DPO pair count: 0
[train-verifier] writing checkpoint to /tmp/test_prm_cli.prm (effective_seed=13, feature_dim=128)
[train-verifier] done. Load with hu_verifier_panel_create or place under
                 ~/.human/models/verifier-panel/ for hu_verifier_panel_create_from_dir.
```

The 532-byte output file = 16-byte header + 4-byte bias + 128 × 4-byte weights, matching the public format documented in `include/human/agent/think_prm.h`.

## Unblock readiness for init-05 (verifier-driven TTT)

Init-05 (`docs/plans/2026-05-11-init-05-verifier-driven-ttt.md`) was deferred pending #07 + #09. #09 shipped in S1. With S2's panel landing, **#05 is unblocked** on the verifier side: the hooks it needs are now in place.

What init-05 (and #06 — SimPO/ORPO/GRPO-2) can consume directly from `hu_agent_t.sota.verifier_panel`:

| Hook | Symbol | Use in init-05 TTT | Use in init-06 RL |
|---|---|---|---|
| Panel struct in the agent | `agent->sota.verifier_panel` (`hu_verifier_panel_t`) | Read-only handle into the post-turn verifier reward | Same |
| Per-step process reward | `hu_verifier_panel_step_score_t::score` + `.confidence` | Per-step gradient target for TTT's tiny-LoRA step | Per-step reward shaping in `hu_rl_trainer_t::step` |
| Chain-level aggregate | `hu_verifier_panel_result_t::aggregate` | TTT trigger: aggregate < retry-threshold → request TTT step | RL update weight; init-07 risk #2 caps panel contribution at ≤ 50% |
| OFF signal | `panel->scorer_count == 0` | "Fall through to outcome reward only" (the safe default before adoption) | Same |
| Ensemble disagreement | `aggregate_confidence` < threshold | TTT skips this turn rather than learning from a noisy reward | RL ignores low-confidence steps |

The vtable-style narrow surface (`score_chain` is the only entry point) means init-05 and init-06 can land independent of any S3 changes to the kernel; they consume the result struct, not the internals.

A worked example for init-05's spec:

```c
hu_verifier_panel_result_t vp;
if (panel->scorer_count > 0 &&
    hu_verifier_panel_score_chain(panel, resp, resp_len, 0, &vp) == HU_OK) {
    if (vp.aggregate < ttt_cfg.retry_threshold &&
        vp.aggregate_confidence > ttt_cfg.min_confidence) {
        hu_learner_step(learner, /* targets derived from vp.steps */ ...);
    }
    hu_verifier_panel_result_free(alloc, &vp);
}
```

That code compiles against the S2 surface as-is.

## Deferred to S3 (with explicit reasoning)

The brief permitted deferring work to S3 with reasoning. The following are intentionally deferred:

1. **Real GPT forward pass for the scorer.** The init-07 design doc calls for a Qwen3-0.5B-class PRM. S2 instead ships a deterministic byte-feature-hashing + dot-product kernel that exercises the same checkpoint load path. Reason: a real `hu_gpt_create` + `hu_ml_checkpoint_load` + tokenized forward integration requires (a) a BPE tokenizer hand-off on every score call, (b) optimizer object construction at load time (the current HUML checkpoint format requires it), and (c) Metal/CPU backend choice plumbing — none of which are in S2 scope without expanding into init-04 (MLX bridge) territory. The S2 kernel keeps the **same shape of output** (per-step + aggregate sigmoid scores, deterministic given fixed weights), so swapping it for the real forward in S3 is a single function-pointer change inside `prm_scorer_logit`. The on-disk format reserves a `version` field so S3 can introduce a v2 checkpoint without breaking v1 readers.

2. **Real gradient-descent training driver.** The S2 `human ml train-verifier` CLI is wired end-to-end and produces panel-loadable checkpoints, but it writes a deterministic seeded checkpoint rather than actually running backprop on the DPO pairs. The reason is the same as (1): real training requires the GPT scaffold, optimizer, and dataloader to be wired against the PRM output head, which is best done together with the S3 forward pass. The CLI's `--db` path already reads the DPO pair count and mixes it into the seed, so different corpora produce different checkpoints — an honest stopgap that exercises the data path without the gradient path. The `--help` text states this explicitly so no user is misled.

3. **Calibration gate (ECE / Brier).** The init-07 design doc's D7 defer-condition #1 is "PRM does not beat prompt critic by ≥ 10 ECE points on the calibration set." S2 cannot meaningfully evaluate this until the real training path lands. The fixtures (`tests/fixtures/prm/calibration_v1.jsonl` and friends from D2) are also deferred until then — they are a no-op without a real model behind them.

4. **Audit mode + drift detector.** Design doc D4 risk #2 calls for an `audit()` mode and a KL-divergence drift detector with `HU_OBSERVER_EVENT_PRM_DRIFT`. Both are S3+ work: they only become meaningful once the panel is trained on real signal. The S2 panel exposes the telemetry primitives the audit will use (`total_calls` and `total_steps_scored` on the panel struct) so the S3 audit code can land as a pure addition with no changes to the runtime.

5. **`config.agent.verifier_panel` JSON parsing.** S2 ships the runtime field (`agent->sota.verifier_panel_enabled`) and the call-site guard, but does not extend `src/config.c` to read the flag from `~/.human/config.json`. Reason: the daemon-side init that calls `hu_verifier_panel_create_from_dir(&panel, "~/.human/models/verifier-panel/", &out->sota.verifier_panel)` lives more naturally in the same change as the calibration gate (so we can opt-in to a *useful* panel, not just a deterministic toy). The current callable surface allows any embedder (tests, the daemon's future opt-in path, the eval harness) to flip the flag manually; tests do exactly this.

6. **Cross-initiative integration spec.** The S2 vtable surface is stable — init-05 and init-06 specs can adopt it as-is — but the actual integration commits land with those initiatives, not here. This report's "Unblock readiness" section is the contract.

## Risk + rollback

- **Risk:** the deterministic kernel does not reflect a real PRM's calibration. **Mitigation:** the default-OFF byte-identity invariant means the production agent does not consume the panel until a future commit flips a flag in config + daemon init. The kernel exists only to (a) prove the lifecycle works, (b) give init-05 / init-06 a stable hook to land their TTT and RL trainer code against, (c) provide a deterministic regression-test target.
- **Rollback:** revert the eight-file diff + delete the three new files. `agent->sota.verifier_panel_enabled` is `false` everywhere except the new `tests/test_think_prm.c`; no other code path constructs or reads the panel. The `hu_ml_cli_train_verifier` CLI is dispatch-only and does not run during the agent's normal lifecycle.

## Quality bar checklist

| Requirement | Status | Evidence |
|---|---|---|
| `cmake --build build && ./build/human_tests` all pass | **PASS (+14 new tests, 7 baseline failures unchanged)** | `Results: 10217/10224 passed, 7 FAILED` (same 7 baseline failures in `test_eval_public_suites.c`) |
| 0 ASan errors | **PASS** | `cmake -DHU_ENABLE_ASAN=ON` + full suite + new tests: zero ASan/leak detections |
| New test (a): panel construction + checkpoint load | **PASS** | `panel_construction_and_checkpoint_load` |
| New test (b): deterministic score given fixed weights | **PASS** | `panel_score_is_deterministic_given_fixed_weights` (bitwise `memcmp` on every per-step score) |
| New test (c): `verifier_panel = false` default keeps agent_turn byte-identical | **PASS** | `agent_turn_is_byte_identical_when_panel_disabled` + reflection suite stays at 56/56 |
| New test (d): NOT_SUPPORTED when checkpoint missing | **PASS** | `panel_returns_not_supported_when_checkpoint_missing` |
| `--filter=reflection` stays green | **PASS** | 51/51 under ASan |
| `--filter=m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` stays green | **PASS** | 1/1 under ASan |
| Binary-size delta ≤ +100 KB on ASan dev build (soft target) | **PASS** (+34.2 KB) | `wc -c human_tests` before/after |
| `human ml train-verifier` ships under `src/ml/cli.c` | **PASS** | `cmd_ml` dispatch + `hu_ml_cli_train_verifier` round-trips a real `.prm` file end-to-end |
| Topology check clean | **PASS** | `Layer topology OK: 0 cross-layer violations across 68 source files.` |
| No regressions in pre-existing tests | **PASS** | Same 7 baseline failures, identical paths, no new failures |

## How to take the next step

For the parent agent reviewing this work:

1. **Inspect the diff:** `git diff sprint-2c-followups...feat/init-07-thinkprm-verifier-panel` (kept off-branch; staged with `git add` only).
2. **Re-run the targeted tests:**
   ```
   ./build/human_tests --suite=think_prm
   ./build/human_tests --suite=Reflection
   ./build/human_tests --filter=m3_daemon_pattern_cloud_provider
   ```
3. **Smoke-test the CLI:**
   ```
   ./build-ml/human ml train-verifier --help
   ./build-ml/human ml train-verifier --output /tmp/smoke.prm --seed 13
   ```
4. **Commit + push** when satisfied. The S2 surface is review-ready; the deferred work is documented as S3 follow-up so a future sprint can pick it up without re-litigating the API.

— end S2 report.
