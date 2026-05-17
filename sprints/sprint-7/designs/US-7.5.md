# Design for US-7.5: Wire W14 nightly re-train cron

Sprint: 7 (Digital Twin via Gemma DPO) | Sprint base: `13b89763`
Risk tier: **MEDIUM** | Estimate: M
Depends on: US-7.1 (real DPO pass), US-7.2 (`human ml mine-corrections`)
Must merge after: US-7.6 (judgment-PPL in `check-lora-ab.sh`)

## 1. Architecture / approach

### 1.1 Sibling runner, not an extension of `hu_lora_training_runner`

`src/agent/lora_training_runner.c` already exists, but it serves a **different** pipeline:
that runner drains `hu_learner_t` pending signals and calls `hu_learner_train` —
the in-process C path against the reference HUML GPT (M3 Bridge A).
US-7.5 is the **MLX-Gemma frontier path** invoked via subprocess (`finetune-gemma.py`).
The two share neither training entry point, data shape (`hu_training_signal_t` vs DPO
JSONL/SQLite from `chat.db`), nor adapter format (HUML safetensors vs MLX adapters).

Coupling them under one runner would force a branchy ctx struct that's half-empty
on either path and require the C runner to know about subprocess plumbing.
We add a **new sibling runner** `hu_lora_retrain_runner` registered for the same
`HU_JOB_LORA_TRAINING` kind, gated by a context flag, OR — cleaner — we introduce a
new job kind `HU_JOB_LORA_RETRAIN_NIGHTLY` so the daemon decides which path fires
based on the device's model configuration. **We pick the new-job-kind path.**

### 1.2 Chain of execution

```
daemon.c 1Hz tick
  └─ once per night, if scheduler probes report idle + on_ac_power
     └─ hu_w14_scheduler_enqueue_lora_retrain_nightly(s, now_ms, ...)
        └─ scheduler tick dispatches HU_JOB_LORA_RETRAIN_NIGHTLY
           └─ hu_lora_retrain_runner(m, spec, budget_ms, ctx)
              ├─ STEP 1: pair-count probe
              │   └─ subprocess: human ml mine-corrections --count-only
              │      → 0 pairs ⇒ emit lora_retrain_skipped_no_new_data, return HU_OK
              ├─ STEP 2: train
              │   └─ subprocess: finetune-gemma.py --dpo --from-corrections
              │                  --no-restart-server --no-version
              │      → exit != 0 ⇒ emit lora_retrain_failed, return HU_OK
              ├─ STEP 3: gate
              │   └─ subprocess: check-lora-ab.sh --judgment <candidate>
              │      → exit 0 (PASS) ⇒ STEP 4
              │      → exit !=0 or "SKIP" stdout ⇒ discard candidate, emit
              │                                      lora_retrain_skipped (no promotion)
              ├─ STEP 4: promote (atomic symlink swap)
              │   └─ subprocess: human ml promote-adapter <candidate-dir>
              │      OR direct C symlink swap (see §1.3)
              └─ STEP 5: status write
                  └─ hu_w14_scheduler_status_save (extended)
```

`lora_retrain_scheduled` event fires at enqueue time. The runner only emits the
**outcome** events (`failed`, `skipped*`, `promoted`).

### 1.3 Where the symlink swap happens

The existing `version_adapter()` in `finetune-gemma.py:139-162` already does
versioning + symlink. With `--no-version`, the script writes only to the candidate
adapter dir (no symlink touch). The promote step is therefore the C runner's job.

**Decision: a new `human ml promote-adapter <candidate-path>` CLI subcommand**
performs the atomic swap. Rationale:
- Reusable from manual ops (`human ml promote-adapter` from a shell after a manual run)
- The atomic-rename pattern is already in `hu_personal_model_save` (see CLAUDE.md M2)
- Keeps the runner's C code free of `symlink(2)` plumbing
- Mirrors the M2 pattern: `tmp + fsync + rename`, with a directory variant
  (`symlink(target, tmp_link); rename(tmp_link, current_link)`)

The runner invokes `human ml promote-adapter <path>` via subprocess. On exit 0,
emit `lora_retrain_promoted`. On non-zero, emit `lora_retrain_promotion_failed`
(candidate stays on disk, current symlink untouched). The S1/S2 gates from the
ADR are honored by `check-lora-ab.sh --judgment` (US-7.6 wires the judgment
side); S3 is explicitly out of scope per stories.md.

### 1.4 Idle threshold detection

Already present. `hu_scheduler_t` honors `spec.requires_idle` (skips when
`probe_load_pct > HU_SCHED_IDLE_LOAD_MAX = 60`) and `spec.requires_ac_power`.
The nightly enqueue sets both true; **this story adds no new probe**.
"Nightly" cadence comes from `interval_sec = 86400` plus an `earliest_at`
pinned to a quiet-hours window. The story title says "idle scheduler"; W14
already is the idle scheduler — the wiring is the new bit.

## 2. Existing-code interface notes (pinned)

| Symbol | Location | Notes |
|---|---|---|
| `hu_job_kind_t` | `include/human/agent/scheduler.h:45` | Add `HU_JOB_LORA_RETRAIN_NIGHTLY` before `HU_JOB_KIND_MAX` |
| `hu_scheduler_register_runner` | `include/human/agent/scheduler.h:122` | Used to bind the new runner |
| `hu_w14_scheduler_enqueue_lora` | `src/agent/world_model_bridge.c:1030` | Pattern to copy for new enqueue helper |
| `hu_w14_scheduler_status_save` | `src/agent/world_model_bridge.c:1136` | Extend JSON output to add `lora_retrain` block |
| `hu_scheduler_status_parse_json` | `src/agent/scheduler_status_json.c:76` | Extend to populate new outputs (back-compat: missing block → defaults) |
| `version_adapter()` | `scripts/finetune-gemma.py:139` | Reference logic for the C-side promote step |
| `--no-version`, `--no-restart-server` | `scripts/finetune-gemma.py:797,799` | **Already exist.** AC-7.5.1 names them; no script change needed for the flags themselves. |
| `--from-corrections` | not yet present | **Must be added.** Pipes the US-7.2 miner output into the existing `find_dpo_data` path. |

`scheduler.status` current shape (`world_model_bridge.c:1162-1170`):
```json
{ "jobs_pending": N, "jobs_completed_today": N, "battery_pct": N,
  "on_ac_power": bool, "updated_epoch": N }
```
Adding a `lora_retrain` block at the same top level is non-breaking because the
parser uses `strstr` for each key and ignores unknown keys.

## 3. Concrete file plan

| Action | File | LOC | Why |
|---|---|---|---|
| ADD | `include/human/agent/lora_retrain_runner.h` | +40 | New runner public header |
| ADD | `src/agent/lora_retrain_runner.c` | +280 | Subprocess orchestration runner |
| ADD | `src/agent/lora_retrain_status.c` | +80 | Status JSON helper for the new block |
| MODIFY | `include/human/agent/scheduler.h` | +3 | Add `HU_JOB_LORA_RETRAIN_NIGHTLY` enum |
| MODIFY | `include/human/agent/scheduler_status_json.h` | +18 | Add parser outputs for `lora_retrain` block |
| MODIFY | `src/agent/scheduler_status_json.c` | +50 | Optional-block parsing |
| MODIFY | `src/agent/world_model_bridge.c` | +90 | New enqueue helper; extend status write |
| MODIFY | `src/ml/cli.c` | +140 | `human ml promote-adapter` subcommand |
| MODIFY | `src/doctor.c` | +30 | Display `lora_retrain` block in `human doctor scheduler` |
| MODIFY | `src/daemon.c` | +25 | Register new runner; nightly enqueue site |
| MODIFY | `scripts/finetune-gemma.py` | +35 | Add `--from-corrections` flag; verify `--no-version`/`--no-restart-server` cover the AC contract |
| ADD | `tests/test_w14_lora_retrain.c` | +420 | All AC-7.5.1..7.5.4 tests |
| MODIFY | `tests/test_scheduler_status.c` | +90 | AC-7.5.5: parse new block (forward + backward compat) |
| ADD | `tests/fixtures/candidate_adapter_metadata.json` | +20 | Promotion gate simulation |
| ADD | `tests/fixtures/check_lora_ab_pass.json` | +10 | Mock gate PASS output |
| ADD | `tests/fixtures/check_lora_ab_skip.json` | +10 | Mock gate SKIP output (US-7.6 dormant) |
| ADD | `tests/fixtures/check_lora_ab_fail.json` | +10 | Mock gate FAIL output |

### 3.1 New runner signatures

```c
/* include/human/agent/lora_retrain_runner.h */
typedef struct hu_lora_retrain_ctx {
    hu_allocator_t *alloc;          /* optional */
    const char *script_path;        /* default: scripts/finetune-gemma.py */
    const char *miner_cmd;          /* default: "human ml mine-corrections" */
    const char *gate_script;        /* default: scripts/check-lora-ab.sh */
    const char *promote_cmd;        /* default: "human ml promote-adapter" */
    const char *candidate_dir;      /* required: where the new adapter lands */
    /* Event sink (NULL is fine — falls back to hu_log_info). */
    void (*emit_event)(const char *event_name, const char *json_payload, void *ud);
    void *emit_user_data;
    /* HU_IS_TEST seam — when set, subprocess exec is replaced by these. */
    int (*test_run_subprocess)(const char *const argv[], int *exit_code, char *stdout_buf,
                               size_t stdout_cap, void *ud);
    void *test_subprocess_ud;
} hu_lora_retrain_ctx_t;

hu_error_t hu_lora_retrain_runner(struct hu_memory_facade *m,
                                  const struct hu_job_spec *spec,
                                  int64_t budget_ms, void *user_data);
```

The `test_run_subprocess` hook is the deterministic mock seam used by every
AC test (see §4). Under `HU_IS_TEST`, the production exec path returns
`HU_ERR_NOT_SUPPORTED` if no hook is registered — this prevents accidental
real Python invocation in CI.

## 4. Test plan (AC → test → fixture)

All in `tests/test_w14_lora_retrain.c` unless noted; all use the
`test_run_subprocess` hook (no real exec).

| AC | Test function | Fixture | What it asserts |
|---|---|---|---|
| 7.5.1 | `test_retrain_enqueues_and_invokes_finetune` | — | After `hu_w14_scheduler_enqueue_lora_retrain_nightly` and a `hu_scheduler_tick`, the captured argv contains `finetune-gemma.py`, `--dpo`, `--from-corrections`, `--no-restart-server`, `--no-version`. A `lora_retrain_scheduled` event was emitted at enqueue time. |
| 7.5.2 (PASS) | `test_retrain_promotes_on_pass_skips_on_fail` (PASS half) | `check_lora_ab_pass.json` + `candidate_adapter_metadata.json` | Mock gate returns exit 0; runner invokes `promote-adapter` with `candidate_dir`; `lora_retrain_promoted` event emitted. |
| 7.5.2 (FAIL) | `test_retrain_promotes_on_pass_skips_on_fail` (FAIL half) | `check_lora_ab_fail.json` | Mock gate returns exit 1; runner does NOT invoke `promote-adapter`; `lora_retrain_skipped` event emitted with `reason=gate_fail`; current symlink path string never appears in any argv after the gate call. |
| 7.5.2 (SKIP) | `test_retrain_treats_judgment_skip_as_not_pass` | `check_lora_ab_skip.json` | Mock gate stdout contains `"verdict":"SKIP"` (US-7.6 dormant); runner treats as non-promote (NOT PASS). **D3 contract.** |
| 7.5.3 | `test_retrain_failure_preserves_adapter` | — | `test_run_subprocess` returns exit code 137 on the finetune step; runner emits `lora_retrain_failed` with `exit_code=137`; gate/promote subprocess hooks are NEVER called. |
| 7.5.4 | `test_retrain_skipped_on_empty_delta` | — | First subprocess (`mine-corrections --count-only`) stdout is `{"pairs":0}`; runner emits `lora_retrain_skipped_no_new_data`; finetune/gate/promote hooks NEVER called. |
| 7.5.5 (write) | `test_status_write_includes_lora_retrain_block` (in `test_scheduler_status.c`) | — | After a tick that ran the runner, `hu_w14_scheduler_status_save` produces JSON with `lora_retrain: { last_run_ts, last_outcome, pairs_consumed }`. |
| 7.5.5 (read) | `test_status_parse_lora_retrain_optional` (in `test_scheduler_status.c`) | — | Parser accepts JSON with and without the block (backward compat); when present, populates output struct; when missing, leaves defaults. |
| 7.5.5 (display) | `test_doctor_renders_lora_retrain` (in `test_scheduler_status.c`) | — | `human doctor scheduler` rendering helper formats the new block (snapshot-style string compare). |

### 4.1 Test seam details

`tests/test_w14_lora_retrain.c` registers `test_run_subprocess` and records the
argv of every subprocess invocation into a fixed-size capture array. Each test
arranges (1) a queued response per subprocess step and (2) asserts the argv
sequence post-tick. No SQLite writes, no filesystem writes outside `/tmp/test-*`.

`HU_IS_TEST` guard ensures `hu_lora_retrain_runner` short-circuits to the test
hook; without `HU_IS_TEST`, the runner refuses to run if no hook is set AND the
real Python interpreter is absent — preventing accidental partial exec.

## 5. Risks

### R1 (HIGH / MEDIUM): Lock contention — overlapping retrains
A 30-60 minute MLX training run can outlive multiple scheduler ticks (the
1 Hz tick window means many opportunities to re-fire). If the runner returns
HU_OK while a background subprocess is still alive, a second tick could
enqueue another retrain whose finetune subprocess fights the first for GPU memory.

**Mitigation:** the runner is **synchronous** — it blocks the tick until the
subprocess chain completes. The W14 per-tick budget enforces an upper bound
(set `budget_ms = 0` to use scheduler default 60s, then override per-job
with a generous nightly budget like 90 minutes). Additionally: a PID-file at
`~/.human/lora_retrain.pid` acquired with `O_EXCL` at runner entry — if the
file exists and points at a live PID, the runner emits
`lora_retrain_skipped_already_running` and returns. The PID file is removed
on runner exit (including failure paths) via a cleanup helper.

**Verification:** AC-7.5.1 + AC-7.5.3 tests both exercise the PID-file
lifecycle. New test `test_retrain_skipped_if_pidfile_held` covers the
contention case directly.

### R2 (MED / MED): S3 user-feedback signal is out of scope
The rollback ADR (`docs/plans/adr/2026-05-11-adapter-rollback-signal.md`)
specifies three rollback signals: S1 (persona-eval regression), S2 (PPL
drift on holdout), S3 (user-feedback rate). Stories.md line 271 explicitly
defers S3 to a future sprint. This story's promotion gate honors S1+S2 via
`check-lora-ab.sh --judgment` (US-7.6) — but the **rollback** half of the
ADR is not wired here.

**Mitigation:** document in the runner comment and in the design doc that
this story implements the *promotion* gate (forward path) only.
A separate `lora_rollback_runner` is a future story (likely Sprint 8).
The `lora_retrain` status block uses `last_outcome` ∈ `{skipped, pass, fail}`,
which is forward-compatible with adding a `rolled_back` value later.

### R3 (MED / MED): `SKIP` verdict ambiguity — D3 contract
US-7.6's `check-lora-ab.sh --judgment` emits `SKIP` when the judgment-PPL
backend is dormant (no held-out fixture, no model loaded). A naive parser
that does `exit == 0 → PASS` would treat SKIP as PASS and promote untested
adapters.

**Mitigation:** the runner parses the gate's JSON stdout, not just exit
code. The promotion path requires `verdict == "PASS"` literally; any other
value (`"SKIP"`, `"FAIL"`, missing key, malformed JSON) routes to the
non-promote branch. Test `test_retrain_treats_judgment_skip_as_not_pass`
pins this contract with a deterministic fixture.

### R4 (LOW / SMALL — flagged for honesty): event sink wiring
The runner emits events via an optional callback. If the daemon forgets to
wire one, events go only to `hu_log_info`. This is acceptable for V1 —
status JSON carries the persistent state — but operators using log-tailing
will see events without structured shape.

**Mitigation:** documented in the runner header; not a blocker.

## 6. Sequencing (numbered, verifiable steps)

1. **Add enum + headers, no behavior.** Add `HU_JOB_LORA_RETRAIN_NIGHTLY` to
   `hu_job_kind_t`. Add `include/human/agent/lora_retrain_runner.h`. Build.
   - Verify: `cmake --build --preset dev` succeeds; `./build/human_tests` still 0 failures.

2. **Stub runner with PID-file + test-hook scaffolding.** Implement
   `hu_lora_retrain_runner` that only acquires/releases the PID file and
   returns HU_OK. Wire the test_run_subprocess hook. No subprocess steps yet.
   - Verify: `./build/human_tests --filter=test_retrain_skipped_if_pidfile_held` passes.

3. **Wire mine-corrections probe (AC-7.5.4).**
   - Verify: `./build/human_tests --filter=test_retrain_skipped_on_empty_delta` passes.

4. **Wire finetune subprocess invocation (AC-7.5.1).** Capture argv shape.
   Add `--from-corrections` flag to `scripts/finetune-gemma.py`.
   - Verify: `./build/human_tests --filter=test_retrain_enqueues_and_invokes_finetune` passes.

5. **Wire failure path (AC-7.5.3).** Non-zero exit ⇒ `lora_retrain_failed`,
   no gate call, no promote.
   - Verify: `./build/human_tests --filter=test_retrain_failure_preserves_adapter` passes.

6. **Wire gate + promote (AC-7.5.2).** Parse JSON verdict, route on `PASS`
   vs everything else. Implement `human ml promote-adapter` CLI as the atomic
   symlink swap (tmp + rename pattern from M2).
   - Verify: `./build/human_tests --filter=test_retrain_promotes_on_pass_skips_on_fail` passes.

7. **Wire SKIP handling (D3, AC-7.5.2 SKIP variant).** Add the
   verdict-parsing test fixture and the "not PASS ≠ SKIP" branch.
   - Verify: `./build/human_tests --filter=test_retrain_treats_judgment_skip_as_not_pass` passes.

8. **Extend status JSON (AC-7.5.5).** Write the `lora_retrain` block in
   `hu_w14_scheduler_status_save`; extend parser; extend doctor renderer.
   - Verify: `./build/human_tests --suite=scheduler_status` passes (existing + new).

9. **Daemon wiring.** Register the runner in `src/daemon.c`; add the nightly
   enqueue site (interval_sec=86400, requires_idle=1, requires_ac_power=1,
   priority=0). Confirm coexistence with the existing `hu_lora_training_runner`
   (different job kind; no conflict).
   - Verify: full `./build/human_tests` 0 failures, 0 ASan errors;
     `scripts/agent-preflight.sh` clean.

## 7. Open questions

1. **Subprocess synchronous vs detached.** The design above runs the
   finetune subprocess synchronously (blocks the scheduler tick for up to
   90 minutes). This is the cheapest correct option but holds the W14 tick
   loop. The scheduler has a per-tick budget cap to prevent global stalls,
   but a 90-minute LoRA train is well above that. **Recommend** raising
   `HU_SCHED_TOTAL_BUDGET_MS` for the LoRA-retrain tick only, OR fork-and-poll
   via a `waitpid(WNOHANG)` state machine across multiple ticks. The latter
   is correct but adds substantial complexity; I lean synchronous-with-PID-file
   for V1 and defer the state-machine variant to a follow-up if real wall-clock
   data shows tick starvation. **User decision needed.**

2. **`human ml promote-adapter` vs inline C swap.** The design uses a
   subcommand for reusability. The runner could equally do the swap inline
   via the same atomic-rename code reused from `hu_personal_model_save`.
   **Recommend** the subcommand for the reuse story; flag if you'd prefer
   inline to reduce subprocess count.

3. **Where does the nightly enqueue site live exactly?** `src/daemon.c:2901`
   is the existing `hu_w14_scheduler_enqueue_lora` site (W13 path). The
   nightly retrain enqueue is a different cadence (once per 24h, not per
   N signals). Placing it adjacent in `daemon.c`'s housekeeping path is the
   obvious home; flagging in case the user has a preferred site (e.g.
   inside `world_model_bridge.c`).

---

RESULT_tech-lead=READY
