---
title: "Init 05 — Verifier-Driven Test-Time Training (TTT)"
created: 2026-05-11
status: deferred
parent: 2026-05-11-sota-2026-massive-team-program.md
risk: high
scope: include/human/ml/, include/human/agent/, src/ml/, src/agent/agent_turn.c, src/agent/scheduler runner
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-10-w13-learning-loop.md
  - 2026-05-10-w14-sleep-compute.md
  - 2026-05-11-full-sota-rl-improvement-loop-design.md
  - 2026-05-11-rl-loop-phase-0-honesty.md
  - 2026-05-10-m3-frontier-model-bridge.md
  - ../../include/human/ml/learner.h
  - ../../include/human/ml/fidelity.h
  - ../../include/human/agent/scheduler.h
  - ../../include/human/agent/response_verifier.h
  - ../standards/security/threat-model.md
  - ../standards/engineering/principles.md
  - ../standards/engineering/naming.md
  - ../standards/engineering/performance.md
arxiv:
  - "2505.19475 — VDS-TTT: Continuous Self-Improvement of LLMs by Test-time Training with Verifier-Driven Sample Selection"
  - "2604.06169 — In-Place Test-Time Training (fast-weight MLP projection)"
  - "2510.10223 — SyTTA: Synergistic Test-Time Adaptation for LLMs (4 extra tokens, dual-signal)"
  - "2601.19659 — KeepLoRA: Continual Learning with Residual Gradient Adaptation (ICLR 2026)"
  - "2601.03093 — ATLAS: Adaptive Test-Time Latent Steering with External Verifiers"
last_audit: 2026-05-25
---

# Init 05 — Verifier-Driven Test-Time Training (TTT)

> **Status:** design (D0 complete on file land). Owner: ML subsystem. Sprint slot:
> SOTA-2026-01 candidate. Critical-path position: **04 → 05 → 07 → 14**. Cannot
> ship until Init 04 (MLX Qwen3 provider) closes Bridge B; the same `hu_provider_t.load_adapter`
> seam Init 05 calls into is gated on Init 04 wiring. Until then, TTT runs against
> the existing CPU-fallback `hu_learner_t` backend (§2.2) for plumbing-only proof.

## 1. One-line and product framing

> When the verifier panel flags a low-fidelity response **and** a user-provided
> correction turn arrives, perform a tiny (1–10 step) on-device gradient update
> on the active LoRA adapter scoped to that conversation, with explicit rollback
> on subsequent user dissent.

This initiative closes the last gap in the M3 mission table (CLAUDE.md): the
adapter no longer waits for nightly W13/W14 training to learn — it learns
**now**, before the next turn, and reverts cleanly if the user disagrees with
what it learned. It is the **only** initiative in the 14-track program that
gives the user a within-conversation feedback loop on personalization.

The narrative win it unlocks: **"You said 'no, more like this' — the next turn
is already different, on your hardware, and if you keep saying no, the change
unwinds."** Apple Foundation Models cannot do this (no per-conversation TTT
hook). Gemini Personal Intelligence cannot do this (cloud only). OpenClaw
SOUL.md cannot do this (markdown templates, no learning).

## 2. Architecture

### 2.1 The four trigger conditions

A TTT step fires when **all four** are true within a single turn N+1:

| # | Condition | Source |
|---|-----------|--------|
| 1 | Turn N's response had `verifier_score < HU_TTT_FIDELITY_THRESHOLD` (default 0.55, configurable) | `hu_ml_fidelity_score_baseline` extension (§2.5) |
| 2 | Turn N+1 from user is classified `correction` (heuristic: contains negation + reformulation) | `hu_ttt_correction_classify` (§2.5) |
| 3 | `agent->learner` is non-NULL **and** vt-name ∈ {"mlx","ggml"} (CPU fallback runs but writes a no-op delta — kept on for plumbing tests) | `hu_learner_t` |
| 4 | Per-conversation TTT budget (default 8 steps / 1 hr) not exceeded | `hu_ttt_journal_t` budget meter |

If any of the four is false, no TTT step runs — the turn proceeds normally.
This four-gate design is deliberately conservative: false positives on TTT are
**much** more expensive than false negatives because every TTT step is a
candidate for catastrophic forgetting (§2.7).

### 2.2 The synchronous-but-bounded step

```
turn N response
     │
     │ verifier writes { score, claims, flagged }
     ▼
turn N+1 user message arrives
     │
     │ correction classifier runs FIRST (cheap, ~50 µs)
     │ if not correction → fall through to normal turn
     ▼
build (preferred=user_correction, dispreferred=turn_N_response) DPO pair
     │
     ▼
hu_learner_t.step_bounded(max_steps=4, max_ms=200, &out_journal)
     │   ├─ MLX backend: subprocess call (Init 04 wiring), max 200 ms wall
     │   ├─ ggml backend: in-process, max 200 ms wall
     │   └─ CPU fallback: no-op delta, journal entry recorded for tests
     │
     ▼
hu_ttt_journal_append(&out_journal)
     │
     ▼
adapter file rewritten atomically (fsync + rename, same as W13 §contract)
     │
     ▼
hu_provider_load_adapter() (Init 04) — invalidates KV cache on model_version bump
     │
     ▼
turn N+1 proceeds with the freshly-updated adapter
```

The hard wall budget (`max_ms=200`) is enforced two ways:

1. The C-side caller passes a deadline timestamp; on return the trainer thread
   is signalled to stop after the next mini-batch.
2. `hu_ttt_step_bounded` returns `HU_ERR_TIMEOUT` if the deadline fires before
   step 1 completes — in that case **no journal entry is written and no
   adapter swap occurs**. The turn proceeds against the unchanged adapter.

### 2.3 Vtable extensions

```c
/* include/human/ml/learner.h — additive, behind HU_ENABLE_TTT */

/* TTT journal entry: enough state to invert one training step.
 * LoRA updates are linear in the delta-weights → exact inverse exists by
 * subtraction (no quantization round-trip when adapter is fp16). */
typedef struct hu_ttt_journal_entry {
    int64_t turn_id;                 /* monotonic per-conversation; 0 = invalid */
    int64_t applied_at_ms;           /* unix ms */
    char    conversation_id[64];     /* contact_id / session id */
    char    adapter_path_before[256];/* snapshot path BEFORE the step */
    char    adapter_path_after[256]; /* snapshot path AFTER the step */
    char    model_version_before[64];
    char    model_version_after[64];
    int     steps_taken;             /* 1..HU_TTT_MAX_STEPS */
    float   loss_before;             /* DPO loss on the pair before the step */
    float   loss_after;              /* DPO loss on the pair after the step */
    float   delta_l2_norm;           /* L2 norm of cumulative LoRA delta this step */
    float   cumulative_l2_norm;      /* L2 norm of all unrolled deltas in window */
    char    last_error[128];
} hu_ttt_journal_entry_t;

#define HU_TTT_MAX_STEPS               10  /* hard ceiling per single TTT call */
#define HU_TTT_DEFAULT_MAX_STEPS        4  /* default budget per call */
#define HU_TTT_DEFAULT_MAX_MS         200  /* default wall budget (ms) */
#define HU_TTT_MAX_JOURNAL_PER_CONV   32   /* rolling window */
#define HU_TTT_DELTA_L2_HARD_CAP     8.0f  /* cumulative L2 → snapshot-and-reset */
#define HU_TTT_FIDELITY_THRESHOLD   0.55f  /* below = candidate for TTT */

/* Vtable additions. Each backend MUST implement step_bounded; rollback_journal
 * is mandatory only for backends with a real adapter (mlx, ggml). The CPU
 * fallback returns HU_OK + no-op so plumbing tests pass on any host. */
typedef struct hu_learner_ttt_vtable {
    hu_error_t (*step_bounded)(void *ctx,
                               const hu_dpo_pair_t  *pair,
                               int                   max_steps,
                               int                   max_ms,
                               hu_ttt_journal_entry_t *out_journal);
    hu_error_t (*rollback_journal)(void *ctx,
                                   const hu_ttt_journal_entry_t *journal,
                                   int                            n);
    hu_error_t (*snapshot_safe_adapter)(void *ctx, const char *out_path);
} hu_learner_ttt_vtable_t;

/* Embedded in hu_learner_t under #ifdef HU_ENABLE_TTT — additive only,
 * does not change struct layout for callers built without the flag. */

/* Public entrypoints — all return HU_OK with no journal write on a no-op
 * (e.g. CPU fallback, budget exceeded, deadline expired before step 1). */
hu_error_t hu_learner_step_bounded(hu_learner_t              *l,
                                   const hu_dpo_pair_t       *pair,
                                   int                        max_steps,
                                   int                        max_ms,
                                   hu_ttt_journal_entry_t    *out_journal);
hu_error_t hu_learner_rollback_journal(hu_learner_t                  *l,
                                       const hu_ttt_journal_entry_t  *journal,
                                       int                            n);
```

```c
/* include/human/agent/ttt.h — NEW header for TTT orchestration */

typedef struct hu_ttt_journal {
    hu_ttt_journal_entry_t entries[HU_TTT_MAX_JOURNAL_PER_CONV];
    size_t                 count;       /* circular buffer head */
    size_t                 head;        /* next write index */
    char                   conversation_id[64];
    int64_t                window_started_at_ms;
    int                    steps_in_window;
    float                  cumulative_l2_norm;
    char                   safe_adapter_path[256];  /* last forced snapshot */
} hu_ttt_journal_t;

/* Decide whether THIS turn should fire a TTT step, and build the DPO pair.
 * Returns true and fills `out_pair` if the four gates of §2.1 all pass.
 * `prev_response` and `prev_response_len` are the assistant turn N text;
 * `user_correction` is the inbound turn N+1 text. */
bool hu_ttt_should_fire(const hu_agent_t *agent,
                        const char *prev_response, size_t prev_response_len,
                        const char *user_correction, size_t user_correction_len,
                        float prev_fidelity_score,
                        hu_dpo_pair_t *out_pair);

/* Apply a TTT step and append to the journal.
 * Idempotent on retry: if the journal already contains an entry for
 * (conversation_id, turn_id), this is a no-op. */
hu_error_t hu_ttt_apply(hu_agent_t *agent,
                        hu_ttt_journal_t *journal,
                        const hu_dpo_pair_t *pair,
                        int max_steps, int max_ms);

/* Roll back the most-recent N entries for `conversation_id`.
 * Reverts adapter + model_version + KV cache. */
hu_error_t hu_ttt_rollback_recent(hu_agent_t *agent,
                                  hu_ttt_journal_t *journal,
                                  int n);

/* Heuristic correction classifier. Returns score 0..1; agent_turn treats
 * ≥ HU_TTT_CORRECTION_THRESHOLD (default 0.6) as a correction signal. Pure
 * function, no allocation, deterministic. */
float hu_ttt_correction_classify(const char *user_msg, size_t user_msg_len);

/* Drift-detection runner: registered on hu_scheduler_t under a new job kind
 * HU_JOB_TTT_DRIFT_EVAL. Replays a held-out fidelity eval set; if the active
 * adapter regresses > HU_TTT_DRIFT_REGRESS_PCT vs the last-good safe adapter,
 * triggers automatic full rollback to safe_adapter_path. */
hu_error_t hu_ttt_drift_runner(hu_memory_facade_t *m,
                               const hu_job_spec_t *spec,
                               int64_t budget_ms, void *user_data);
```

```c
/* include/human/agent/scheduler.h — additive enum value */
HU_JOB_TTT_DRIFT_EVAL = 11,   /* nightly held-out persona-fidelity replay */
```

### 2.4 Rollback semantics — exact inverse via LoRA linearity

A LoRA update is `W_new = W_old + α/r · (B · A)`, where `B`, `A` are the
low-rank factors actually trained. The **delta** stored in the journal is
exactly `δW_i = α/r · (B_i · A_i - B_{i-1} · A_{i-1})` — a single dense
fp16 tensor of size `out_dim × in_dim` per LoRA layer.

Rollback applies `W ← W - δW_i` for each entry in reverse order. This is
**exact** under three preconditions, all of which are properties of our pipeline:

1. The adapter is stored fp16 (no Q4/Q8 round-trip between steps). The journal
   entry pins `model_version_before/after` so a rollback against a
   re-quantised adapter fails fast with `HU_ERR_INVALID_STATE`.
2. No background W13 training writes between the TTT step and the rollback.
   The W14 scheduler holds an exclusive lock on the adapter file during any
   training operation; TTT acquires the same lock for the duration of
   `step_bounded` + journal write (~250 ms total). See §3.4.
3. The base model weights are byte-identical (same SHA-256 of the GGUF /
   safetensors blob). `model_version_before` = `model_version_after` enforces
   this.

Under failure of any precondition, rollback degrades to a hard restore: copy
`adapter_path_before` over the live adapter, then bump `model_version` to
invalidate the KV cache. This is slower (full file write) but always correct.

### 2.5 Fidelity threshold and correction classifier

`hu_ml_fidelity_score_baseline` already exists (`include/human/ml/fidelity.h`)
and computes the same `mean` that the gateway's `metrics.fidelity` returns.
We add a per-response fidelity scorer that reuses the same primitive but
operates on a single response string:

```c
/* include/human/ml/fidelity.h — additive */
hu_error_t hu_ml_fidelity_score_response(const hu_communication_style_t *target,
                                         const char *response, size_t response_len,
                                         float *out_score);
```

This is an order-of-microseconds string check (no LLM call, no embedding) —
identical math to the W13 lora-baseline path. Default trigger threshold
`0.55` was picked from the existing `check-lora-baseline.sh` floor (`0.50`)
plus a 5-point margin so we don't fire TTT on responses that are merely "OK
according to the gate" — only on responses that are at-or-below the gate's
acceptance line.

The correction classifier is intentionally heuristic v1 (KISS):

```c
float hu_ttt_correction_classify(const char *msg, size_t len);
```

Internal: count negation tokens (`no, not, don't, that's wrong, never say,
i prefer, more like, less like, instead`) within the first 64 characters,
weighted higher when followed by an imperative ("say", "use", "respond"),
and combined with a presence-of-question-mark signal (corrections rarely
end in `?`). Returns `0..1`. **NOT** an LLM call — must run inline on every
turn at <100 µs amortized. Fixture-driven calibration in `tests/fixtures/
ttt_correction_phrases.json` (~50 positive + 50 negative phrases drawn
from real h-uman conversation logs after PII redaction).

### 2.6 Privacy contract — TTT data NEVER leaves the device

This is a **structural** property, not a configuration option:

1. The DPO pair built for a TTT step lives **only** in stack memory of
   `hu_ttt_apply`, plus a single SQLite row in `~/.human/ttt_journal.db`
   (gated under `HU_ENABLE_TTT`, never replicated). The pair text is
   redacted via the same `src/security/` PII pass used by W13 before any
   on-disk write — `hu_pii_redact` (existing) is invoked on `pair->prompt`,
   `pair->preferred`, `pair->dispreferred` before the journal row lands.
2. The MLX subprocess (Init 04) is bound to a Unix domain socket under
   `~/.human/ipc/`; no TCP, no public sockets. A test
   (`test_ttt_no_network_egress`) wraps the subprocess spawn in a netlink /
   bpf filter on Linux and a `proc_listen()` audit on macOS verifying no
   AF_INET/AF_INET6 socket is opened during the entire TTT cycle.
3. The journal SQLite file has filesystem mode `0600` (matching the existing
   `~/.human/personal_model.bin` contract from Personal Model v4).
4. No cloud provider is invoked during a TTT step. The provider used for
   correction classification is never the cloud chat provider — it's a pure
   C function (§2.5). The verifier (`hu_response_verify`) already runs
   against the local W7 graph, no network.
5. The proof artifact in `~/.human/proofs/ttt-<conv-id>/` (§4.4) lists the
   network-egress test result as a release-gate item. CI fails if the
   egress test ever passes (a single AF_INET socket = abort).

This list is reproduced verbatim in the public-facing `docs/proof/
ttt-privacy.md` released alongside the first SOTA-2026-01 sprint that
ships TTT, so the privacy story is auditable by external reviewers.

### 2.7 Catastrophic-forgetting guard (KeepLoRA-inspired)

KeepLoRA (arXiv:2601.19659) projects gradient updates onto the residual
subspace orthogonal to the principal subspace of the pre-trained model.
Implementing the full residual-subspace projection requires a one-time SVD
of the base model's MLP weights — too expensive to do on every TTT step,
but cheap to do **once** per base model, cached in `~/.human/safe_subspace/
<model_sha>.bin`. We adopt a lighter v1 guard for SOTA-2026-01 and keep
the full KeepLoRA projection on the post-merge follow-up list:

| Layer | Guard | When |
|-------|-------|------|
| Per-step | Hard L2 norm cap on δW_i ≤ `HU_TTT_DELTA_L2_PER_STEP` (1.0) | Every step; clip in-place if exceeded |
| Per-window | Cumulative L2 across all unrolled deltas ≤ `HU_TTT_DELTA_L2_HARD_CAP` (8.0) | Every step; if exceeded, force `snapshot_safe_adapter` and reset `cumulative_l2_norm` |
| Per-night (W14 idle) | Drift gate (§2.8) replays held-out eval; > 5% regression → automatic rollback to last safe adapter | Once per quiet-hours window |
| Per-month | Optional KeepLoRA residual-subspace projection (post-SOTA-2026-01) | Background `HU_JOB_TTT_SUBSPACE_REFRESH` |

The L2 numbers are calibrated empirically in P2 with a synthetic-corpus
ablation (see §6 test plan). Initial values come from VDS-TTT's reported
LoRA-rank-8 update norms scaled to our default rank=8.

### 2.8 Drift-detection (nightly, W14 idle scheduler)

Lives as a `hu_job_runner_fn` registered against the new
`HU_JOB_TTT_DRIFT_EVAL`. Triggered by `hu_w14_scheduler_tick` once per
quiet-hours window (default: 03:00–05:00 local, configurable):

1. Replay a 100-prompt held-out persona fidelity eval set
   (`tests/fixtures/ttt_drift_eval_persona.json`, never used for any TTT
   training).
2. Compute `mean fidelity` against the active adapter.
3. Compare to `mean fidelity` of `safe_adapter_path` (the last KeepLoRA
   snapshot or the W13-trained baseline if no snapshot exists yet).
4. If `active < safe - HU_TTT_DRIFT_REGRESS_PCT` (default 5%):
   a. Replace active adapter with `safe_adapter_path`.
   b. Bump `model_version` (KV-cache invalidation).
   c. Truncate the TTT journal (the rollback obviates the unrolled deltas).
   d. Write a `~/.human/proofs/ttt-drift-rollback-<ts>/` evidence dir.
   e. Fire a `HU_OBSERVER_LEVEL_WARN` log line; the dashboard tile from
      Init 14 surfaces the rollback to the user with a one-line "TTT
      drift detected — reverted to last safe adapter" notification.

This runner is the **mechanical backbone** of the D7 defer condition
(§7) — repeated drift rollbacks are the empirical evidence that justifies
parking TTT and falling back to nightly W13 batched training only.

### 2.9 Module dependency direction

```
agent/agent_turn.c  ──►  agent/ttt.c  ──►  ml/learner.h (vtable)
                                       ──►  ml/fidelity.h (response scorer)
                                       ──►  agent/scheduler.h (drift job)
ml/learner_mlx.c  ──implements──►  hu_learner_ttt_vtable_t
ml/learner_ggml.c ──implements──►  hu_learner_ttt_vtable_t
ml/learner_cpu.c  ──implements──►  hu_learner_ttt_vtable_t (no-op)
```

`agent/ttt.c` is the **only** new file that calls into `ml/`. The rest of
the agent loop is unchanged. This satisfies the inward-dependency rule
from `docs/standards/engineering/principles.md` and the boundary
contract in §3.3 of the parent RL design doc.

## 3. Component design

### 3.1 New files

| Action | Path | LOC est. | Responsibility |
|--------|------|----------|----------------|
| NEW | `include/human/agent/ttt.h` | ~120 | Public API (`hu_ttt_journal_t`, `hu_ttt_should_fire`, `hu_ttt_apply`, `hu_ttt_rollback_recent`, `hu_ttt_correction_classify`, `hu_ttt_drift_runner`) |
| NEW | `src/agent/ttt.c` | ~600 | Orchestrator: gates, journal management, scheduler runner registration, PII redaction, atomic adapter swap, KV-cache bump |
| NEW | `src/agent/ttt_classifier.c` | ~250 | Pure C correction-phrase classifier with fixture-loadable phrase tables |
| NEW | `src/agent/ttt_journal.c` | ~350 | SQLite-backed journal: schema, atomic append, rollback rewind, window L2 accountancy, idempotency by `(conversation_id, turn_id)` |
| NEW | `include/human/ml/learner_ttt.h` | ~80 | TTT vtable additions (`step_bounded`, `rollback_journal`, `snapshot_safe_adapter`) |
| NEW | `src/ml/learner_ttt_cpu.c` | ~150 | CPU fallback: no-op step (returns HU_OK + zero-delta journal entry); used by every host that lacks MLX/ggml so plumbing tests pass everywhere |
| MODIFY | `include/human/ml/learner.h` | +30 | Embed optional `const hu_learner_ttt_vtable_t *ttt` field under `#ifdef HU_ENABLE_TTT` |
| MODIFY | `src/ml/learner_mlx.c` | +250 | Implement TTT vtable (Init 04 wiring; subprocess deadline support, fp16 delta extraction) |
| MODIFY | `src/ml/learner_ggml.c` | +250 | Same, for the ggml backend |
| MODIFY | `src/ml/learner_cpu.c` | +60 | Wire CPU TTT vtable from `learner_ttt_cpu.c` |
| MODIFY | `src/ml/learner.c` | +80 | `hu_learner_step_bounded` / `hu_learner_rollback_journal` dispatchers |
| MODIFY | `src/agent/agent_turn.c` | +120 | Single new block at the post-verifier hook site (line ~5750); see §3.2 |
| MODIFY | `src/agent/agent_stream.c` | +40 | Same call site for streaming responses (mirror of `agent_turn.c` block) |
| MODIFY | `include/human/agent.h` | +12 | Add `hu_ttt_journal_t *ttt_journal` (optional, NULL when `HU_ENABLE_TTT` is off) |
| MODIFY | `src/agent/agent.c` | +30 | Init/deinit of `agent->ttt_journal` |
| MODIFY | `include/human/agent/scheduler.h` | +4 | New `HU_JOB_TTT_DRIFT_EVAL` enum value |
| MODIFY | `src/agent/scheduler.c` | +10 | Register `hu_ttt_drift_runner` in default runner table |
| MODIFY | `include/human/ml/fidelity.h` | +12 | Public decl for `hu_ml_fidelity_score_response` |
| MODIFY | `src/ml/fidelity.c` | +50 | Per-response scorer (delegates to existing `hu_communication_style_fidelity_score` over the response string) |
| NEW | `tests/test_ttt_correction_classifier.c` | ~250 | Fixture-driven golden tests for classifier |
| NEW | `tests/test_ttt_journal.c` | ~350 | Append + rewind + L2 accountancy + idempotency |
| NEW | `tests/test_ttt_should_fire.c` | ~200 | Four-gate trigger logic with deterministic fixtures |
| NEW | `tests/test_ttt_apply_e2e.c` | ~400 | End-to-end: turn N → low fidelity → user correction → step → adapter swap → next turn loads new adapter |
| NEW | `tests/test_ttt_rollback.c` | ~300 | Apply N=4 steps → rollback all 4 → adapter byte-identical to start |
| NEW | `tests/test_ttt_l2_norm_cap.c` | ~200 | Cumulative L2 over hard cap → forced snapshot + reset |
| NEW | `tests/test_ttt_drift_runner.c` | ~250 | Synthetic adapter degradation → drift runner detects + rolls back |
| NEW | `tests/test_ttt_no_network_egress.c` | ~200 | Spawn subprocess, audit sockets, assert zero AF_INET |
| NEW | `tests/test_ttt_pii_redaction.c` | ~200 | DPO pair text contains synthetic email/phone → journal row PII-redacted |
| NEW | `tests/fixtures/ttt_correction_phrases.json` | ~80 | 50 pos / 50 neg classifier calibration phrases |
| NEW | `tests/fixtures/ttt_drift_eval_persona.json` | ~150 | 100 prompt-tagged held-out responses for drift-gate scoring |
| NEW | `fuzz/fuzz_ttt_classifier.c` | ~80 | libFuzzer harness on the classifier (zero crashes on arbitrary UTF-8) |
| NEW | `fuzz/fuzz_ttt_journal_replay.c` | ~80 | libFuzzer harness on rollback (any journal sequence → no UB) |
| MODIFY | `CMakeLists.txt` | +20 | New `HU_ENABLE_TTT` option (default OFF), wires `src/agent/ttt*.c` + `src/ml/learner_ttt_*.c` + tests |

**Total new C LOC**: ~3,400 across 9 new headers/sources + 9 new test files.
**Total modified C LOC**: ~900 across 11 existing files.
**Net CMake / fixtures**: ~530 LOC.

### 3.2 The post-verifier call site (`src/agent/agent_turn.c`)

The hook lands **after** the `hu_response_verify` block at line ~5717
and **before** the semantic response-cache `hu_semantic_cache_put` at line
~5788. This places it after fidelity is computable but before the response
is broadcast to the channel — the right point because:

- The verifier has already populated `vreport.claims_extracted/flagged`,
  giving us a free fidelity signal in addition to the `hu_ml_fidelity_score_
  response` call.
- We're inside the response-text owned region (`*response_out` has not yet
  been transferred to the channel), so the previous turn's text is still
  retrievable from `agent->history` for building the DPO pair.
- The KV cache invalidation that follows the adapter swap is well-defined:
  the next turn re-tokenises with the new adapter from a clean cache
  state, matching the invariant the W13 plan already enforces.

Pseudocode block to be inserted (the actual implementation follows the
existing `if (vrf_err == HU_OK) { ... }` style):

```c
#ifdef HU_ENABLE_TTT
    /* TTT trigger evaluation. Cheap fast-path: skip entirely when the
     * agent has no learner, no journal, or the per-conversation budget
     * is already exhausted (single counter check). */
    if (agent->learner && agent->ttt_journal && response_effective_len > 0) {
        float prev_fidelity = 0.f;
        hu_communication_style_t target;
        bool synth = true;
        if (hu_ml_fidelity_resolve_target(agent->alloc, &target, &synth) == HU_OK) {
            (void)hu_ml_fidelity_score_response(&target, *response_out,
                                                response_effective_len,
                                                &prev_fidelity);
        }
        /* Stash for the *next* turn's hu_ttt_should_fire(). Per-conv
         * one-message lookahead state lives on the journal. */
        hu_ttt_journal_record_pending(agent->ttt_journal, *response_out,
                                      response_effective_len, prev_fidelity);
    }
#endif
```

The actual `hu_ttt_apply` call fires at the **start** of the *next* turn,
inside `hu_agent_turn` after the inbound user message is received but
before the system prompt is rebuilt — a separate ~30-line block at line
~830 (right after the `verifier_graph` check, where `agent->learner` is
already a known-live pointer).

This two-step structure (record pending at end of turn N, decide+apply at
start of turn N+1) keeps the trigger gate logic synchronous with the user
message that confirms the correction, and avoids speculatively training on
a turn the user might never reply to.

### 3.3 Adapter file lifecycle

Three on-disk artifacts per conversation:

```
~/.human/adapters/<contact-id>/
  active.bin             ← live adapter; loaded by provider
  active.model_version   ← 64-byte tag, byte-identical to active.bin's header
  safe.bin               ← last KeepLoRA snapshot or W13 baseline
  safe.model_version
  history/
    <ts>-<turn-id>.bin   ← per-step snapshots, kept until journal rolls them off
    <ts>-<turn-id>.model_version
~/.human/ttt_journal.db  ← SQLite, schema in §3.4
```

Atomic swap follows the W13 contract verbatim: write `<adapter>.tmp`,
`fwrite + fflush + fsync + rename`. The adversary test pinned in
`tests/test_personal_model_atomic_save.c::test_personal_model_save_preserves_
prior_state_when_tmp_blocked` is the model — we add the equivalent
`tests/test_ttt_apply_atomic.c::test_ttt_apply_preserves_prior_adapter_when_
tmp_blocked`.

### 3.4 SQLite schema (`ttt_journal.db`)

```sql
CREATE TABLE IF NOT EXISTS ttt_journal (
    id                    INTEGER PRIMARY KEY AUTOINCREMENT,
    conversation_id       TEXT    NOT NULL,
    turn_id               INTEGER NOT NULL,            -- monotonic per conversation
    applied_at_ms         INTEGER NOT NULL,
    adapter_path_before   TEXT    NOT NULL,
    adapter_path_after    TEXT    NOT NULL,
    model_version_before  TEXT    NOT NULL,
    model_version_after   TEXT    NOT NULL,
    steps_taken           INTEGER NOT NULL,
    loss_before           REAL    NOT NULL,
    loss_after            REAL    NOT NULL,
    delta_l2_norm         REAL    NOT NULL,
    cumulative_l2_norm    REAL    NOT NULL,
    pair_prompt_redacted  TEXT    NOT NULL,            -- after PII pass
    pair_pref_redacted    TEXT    NOT NULL,
    pair_dispref_redacted TEXT    NOT NULL,
    rolled_back_at_ms     INTEGER NOT NULL DEFAULT 0,  -- 0 = still applied
    last_error            TEXT
);
CREATE UNIQUE INDEX IF NOT EXISTS ttt_journal_conv_turn
    ON ttt_journal (conversation_id, turn_id);

CREATE TABLE IF NOT EXISTS ttt_safe_snapshots (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    snapshotted_at  INTEGER NOT NULL,
    adapter_path    TEXT    NOT NULL,
    model_version   TEXT    NOT NULL,
    cumulative_l2   REAL    NOT NULL,
    reason          TEXT    NOT NULL                   -- 'l2_cap' | 'drift' | 'manual'
);
```

The exclusive write lock from §2.4 is enforced by SQLite's WAL mode +
`BEGIN IMMEDIATE TRANSACTION` for the (append-journal, copy-adapter,
update-active) sequence. W13 nightly training acquires the same lock at
the same scope, serialising the two writers without a separate lockfile.

## 4. Data flow

### 4.1 Happy path — single TTT step accepted

```
User: "give me a quick rundown of the meeting"
Agent: "Here's a comprehensive 5-paragraph summary..."   ← turn N
                                                            verifier: score 0.42
                                                            ttt_journal_record_pending (fidelity=0.42)
User: "no, just bullets — i don't have time"             ← turn N+1
                                                            classifier: 0.78 (correction)
                                                            should_fire: true
                                                            build pair {
                                                              prompt = "give me a quick rundown of the meeting",
                                                              preferred = (synthesized hint from correction),
                                                              dispreferred = "Here's a comprehensive..."
                                                            }
                                                            step_bounded(max_steps=4, max_ms=200)
                                                              → loss_before=2.31, loss_after=1.87
                                                              → delta_l2=0.42, cumulative_l2=0.42
                                                            journal append
                                                            adapter swap, model_version bump
                                                            KV cache invalidate
Agent: "• Q3 numbers ↑ 12% • next sprint ships Friday"  ← turn N+2 with new adapter
```

### 4.2 Dissent path — rollback fires

```
…continuing from §4.1 turn N+2:
User: "actually no, i preferred the longer one — undo"  ← turn N+3
                                                            classifier: 0.91 (correction + explicit "undo")
                                                            ttt_rollback_recent(n=1)
                                                              → restore adapter_path_before
                                                              → bump model_version (re-invalidate KV cache)
                                                              → mark journal row rolled_back_at_ms=now
Agent: (next response uses pre-TTT adapter; reverts to longer style)
```

The classifier returns a higher score for explicit dissent tokens
(`undo`, `revert`, `take it back`, `i preferred the old`), which triggers
the rollback branch instead of building a new DPO pair. This is the only
asymmetry in the classifier — same primitive, different output dispatch.

### 4.3 L2-cap path — forced safe snapshot

```
…sequence of 6 corrections lands across one conversation in ~10 min:
journal cumulative_l2_norm grows: 0.42 → 0.81 → 1.4 → 3.2 → 5.7 → 8.1
                                                            → exceeds HU_TTT_DELTA_L2_HARD_CAP (8.0)
                                                            → snapshot_safe_adapter(safe.bin)
                                                            → reset cumulative_l2_norm to 0
                                                            → log WARN "TTT L2 cap hit, forced snapshot"
```

This is intentionally a **non-rollback** event — the adaptation is
preserved, but a checkpoint is created so a later drift gate (§4.4) has
something to fall back to that's not the original W13 baseline.

### 4.4 Drift-gate path — nightly auto-rollback

```
W14 scheduler tick at 03:30 local
  → hu_ttt_drift_runner picks up HU_JOB_TTT_DRIFT_EVAL
  → replay 100 prompts against active.bin    → mean fidelity 0.61
  → replay 100 prompts against safe.bin       → mean fidelity 0.68
  → 0.61 < 0.68 - 0.05 (regression threshold) → ROLLBACK
  → copy safe.bin → active.bin
  → bump model_version
  → write ~/.human/proofs/ttt-drift-rollback-2026-05-12T03:31Z/
  → log WARN
```

If this fires twice in any 7-day window, the alert escalates to ERROR and
sets `agent.config.ttt.disabled_until = now + 24h`. If it fires twice in
two consecutive 7-day windows, the D7 defer condition (§7) is met and TTT
is parked until a new design lands.

## 5. Build sequence (phased)

| Phase | Goal | Exit criterion |
|-------|------|----------------|
| **P0** | Skeleton + CPU fallback | `cmake --preset dev -DHU_ENABLE_TTT=ON` builds clean. `tests/test_ttt_journal.c`, `test_ttt_correction_classifier.c`, `test_ttt_should_fire.c` pass against CPU no-op backend. 0 ASan errors. |
| **P1** | Hook + journal at agent_turn site | `tests/test_ttt_apply_e2e.c` passes against CPU backend (DPO pair built, journal row appended, adapter file rewritten with no-op delta, KV cache marker bumped). `agent_stream.c` mirror block added. |
| **P2** | Rollback + L2 caps | `test_ttt_rollback.c` passes (4 steps applied, 4 reverted, adapter byte-identical to baseline). `test_ttt_l2_norm_cap.c` passes (cumulative L2 cap fires forced snapshot). Atomic-save adversary test pinned. |
| **P3** | MLX backend (Init 04 dependency) | `learner_mlx.c::step_bounded` produces real fp16 delta + non-zero loss reduction on synthetic pair. `test_ttt_apply_e2e.c` passes against MLX backend. |
| **P4** | Drift runner + scheduler integration | `test_ttt_drift_runner.c` passes with synthetic adapter degradation. `HU_JOB_TTT_DRIFT_EVAL` registered + driven by `hu_w14_scheduler_tick` once per quiet window. |
| **P5** | Privacy + PII gates | `test_ttt_no_network_egress.c` and `test_ttt_pii_redaction.c` pass. `docs/proof/ttt-privacy.md` published with the §2.6 list. |
| **P6** | Fuzz + adversarial review | Both libFuzzer harnesses run 1M iterations zero crashes. `critic` + `aspect-panel` verdicts collected. Defer-condition status documented. |
| **P7** | Drift-eval bake-in | TTT runs on the developer's own conversation log for ≥7 days; drift gate result written to `docs/proof/ttt-bakein-2026-XX.md`. PASS = ship to SOTA-2026-01 sprint completion. |

P0–P5 are estimated **5 working days** by a single senior engineer, parallelisable
with Init 04 (MLX provider) on the critical path. P6–P7 add **3 more days** of
adversarial-review time and **7 more days** of bakein. Total wall: ~3 weeks
including bakein.

The sprint can ship P0–P5 as the SOTA-2026-01 deliverable for Init 05 with
the explicit caveat in the proof artifact: "TTT enabled in shadow mode (writes
journal, applies CPU no-op delta, no real adaptation) until Init 04 P1 lands."
The first real TTT commit lands in SOTA-2026-02, gated on Init 04.

## 6. Test plan

| Tier | Test | Coverage |
|------|------|----------|
| T1 unit | `test_ttt_correction_classifier.c` | 50/50 fixture phrases ≥ threshold; 50/50 negatives below threshold; UTF-8 fuzz survival |
| T1 unit | `test_ttt_journal.c` | Append, query-by-conversation, rollback rewind, idempotency, cumulative L2 accountancy |
| T1 unit | `test_ttt_should_fire.c` | Each of the 4 gates rejects independently; all-4-pass triggers; dissent token short-circuits to rollback |
| T2 property | `test_ttt_l2_norm_cap.c` | Random sequences of synthetic deltas always either stay under cap or trigger snapshot; never silently exceed |
| T2 property | `test_ttt_apply_atomic.c` | Pre-block `<adapter>.tmp` → apply fails → on-disk adapter byte-identical to pre-state |
| T3 integration | `test_ttt_apply_e2e.c` (CPU backend) | Full turn-N → turn-N+1 cycle; adapter swap observable; journal row written; CPU no-op delta correctness |
| T3 integration | `test_ttt_apply_e2e.c` (MLX backend, gated `HU_HAVE_MLX=1`) | Same, real fp16 delta, loss reduces |
| T3 integration | `test_ttt_drift_runner.c` | Synthetic 10% degradation in `active.bin` → drift runner rolls back to `safe.bin` |
| T4 e2e | `test_ttt_rollback.c` | 4-step apply → 4-step rollback → byte-equal to start (with quantization-stable fixture) |
| T5 adversarial | `test_ttt_no_network_egress.c` | Linux: bpf socket filter; macOS: `proc_listen` audit; Linux+macOS: zero AF_INET sockets opened across full TTT cycle |
| T5 adversarial | `test_ttt_pii_redaction.c` | Synthetic emails/phones in DPO pair → journal row contains only redaction tokens |
| T5 adversarial | `fuzz/fuzz_ttt_classifier.c` | 1M iterations, 0 crashes on arbitrary UTF-8 input |
| T5 adversarial | `fuzz/fuzz_ttt_journal_replay.c` | 1M iterations, arbitrary `(seq of journal entries) → rollback any prefix` no UB |

Total new tests: **12 deterministic + 2 fuzzer harnesses**.
Suite tag: `--suite=TTT`. Estimated runtime: <45 sec at full coverage.

Pre-commit gate (added to `scripts/agent-preflight.sh`):
- Any change touching `src/agent/ttt*.c`, `src/ml/learner_ttt_*.c`, or
  `src/agent/agent_turn.c` runs `--suite=TTT` automatically.

## 7. Risk register

| Risk | Severity | Mitigation |
|------|----------|------------|
| **Catastrophic forgetting** — adapter degrades over many TTT steps even within L2 cap | High | (a) per-step + per-window L2 caps (§2.7); (b) nightly drift gate (§2.8) auto-rolls back on >5% regression; (c) defer condition triggers if drift gate fires repeatedly |
| **Latency budget blown** — TTT step exceeds 200 ms wall, slowing turn N+1 | High | Hard deadline enforced in `step_bounded`; on `HU_ERR_TIMEOUT` the journal is **not** written and adapter is **not** swapped — turn proceeds against the unchanged adapter (degradation = no learning, never a stalled turn) |
| **RAM blow-out during MLX subprocess training** — exceeds 300 MB transient | High | MLX backend pre-flight checks `vm_stat` / `getrusage` and aborts with `HU_ERR_RESOURCE_EXHAUSTED` if free RAM <1 GB. Subprocess RSS budget enforced via `RLIMIT_AS`. |
| **Privacy leak via subprocess** — MLX child opens unintended network socket | High | Test `test_ttt_no_network_egress.c` audits sockets across the whole cycle; CI fail on any AF_INET open. Privacy contract (§2.6) reproduced verbatim in `docs/proof/ttt-privacy.md`. |
| **Adapter file corruption mid-step** — crash between `fsync` and `rename` leaves invalid state | High | Atomic-save contract from W13; tests/test_ttt_apply_atomic.c pre-blocks `.tmp` to force partial-write recovery. SQLite WAL provides crash-consistent journal. |
| **PII leak into journal** | High | `hu_pii_redact` runs on `prompt`/`preferred`/`dispreferred` before the SQLite write; `test_ttt_pii_redaction.c` uses synthetic emails / phones / SSN-shape strings to verify redaction. Journal file mode `0600`. |
| **Correction classifier false positives** — TTT fires on an "ok" message the user didn't intend as a correction | Medium | Conservative default threshold 0.6; per-conversation budget caps 8 steps/hour; user dissent rolls back the spurious adaptation cleanly. Bake-in P7 calibrates against real conversation logs. |
| **Correction classifier false negatives** — user complaint is missed, TTT never fires | Medium | Acceptable degradation: behaves like W13 nightly batched training. The product loss is "no within-conversation learning" not "wrong learning." |
| **Binary-size budget overrun** — TTT pulls in heavy code paths | Medium | Net +16 KB MinSizeRel measured target; CI gate; all TTT code under `#ifdef HU_ENABLE_TTT`. CPU fallback file `learner_ttt_cpu.c` is the only TTT code in default release builds. |
| **L2 cap calibration is wrong** — caps too tight (no learning) or too loose (forgetting) | Medium | P2 ablation builds the cap empirically; default values are "best estimate from VDS-TTT scaled to rank=8" and tuned during P7 bakein |
| **Init 04 (MLX provider) slips** — TTT has no real backend | Medium | P0–P5 ship in shadow mode against CPU fallback. SOTA-2026-02 enables MLX backend after Init 04 lands. Honestly documented in proof artifact. |
| **Journal SQLite contention with W13 nightly trainer** | Medium | WAL mode + `BEGIN IMMEDIATE TRANSACTION` serialises writers cleanly; tests cover concurrent W13 trainer + TTT step without deadlock |
| **Classifier embeds latent bias from English-only fixtures** | Low (initially) | English-only v1 explicitly documented; non-English correction phrases extend the fixture in a follow-up. False negatives on non-English are equivalent to disabling TTT — graceful. |

## 8. Defer / descope condition (D7)

**Park TTT and rely on nightly W13 batched LoRA training only if any of:**

1. The drift gate (§2.8) fires automatic rollbacks **twice within any
   single 7-day bake-in window** (§5 P7) on the developer's own conversation
   log. This is the empirical signal that the per-conversation TTT budget
   is causing more drift than the per-night W13 training is offsetting,
   and the design needs to be reconsidered (e.g. shrink steps to 1, drop
   step budget by 4×, or move to KeepLoRA full residual-subspace projection).
2. The MLX subprocess RSS exceeds 500 MB (1.66× the 300 MB budget) on
   the M3 Max benchmark profile and cannot be brought back under budget
   in a one-week optimization sprint. RAM is the **real** scaling cost of
   TTT — if we can't keep it under 300 MB transient, the laptop user-
   experience falls off a cliff.
3. The wall-clock latency of `step_bounded(max_steps=4)` exceeds 350 ms
   p95 on M3 Max in the bake-in profile (1.75× the 200 ms target). At
   that point TTT is no longer "tiny," it's "noticeable to the user," and
   the right answer is W13 nightly training only.

When parked, the SOTA-2026-01 sprint completion adds a one-paragraph
note to `docs/plans/2026-05-11-sota-2026-massive-team-program.md` synthesis
section: "Init 05 parked at <date>; rationale: <gate>; W13/W14 path
continues unchanged; revisit when KeepLoRA full subspace projection lands."

The flag `HU_ENABLE_TTT` stays in CMake as `OFF` permanently in the parked
case — every code path is preserved (so unparking is a feature-flag flip),
but the binary contains zero TTT code by default.

## 9. Binary size & RSS budget

| Component | MinSizeRel cost (KB) | Notes |
|-----------|---------------------|-------|
| `src/agent/ttt.c` | ~4.5 | Main orchestrator |
| `src/agent/ttt_classifier.c` | ~3.0 | Phrase tables + scorer |
| `src/agent/ttt_journal.c` | ~3.5 | SQLite glue (reuses existing sqlite3 link) |
| `src/ml/learner_ttt_cpu.c` | ~1.5 | No-op fallback |
| `agent_turn.c` + `agent_stream.c` deltas | ~1.5 | Hook block |
| `learner.c` dispatchers | ~1.0 | `step_bounded` / `rollback_journal` |
| Header / typedef overhead | ~1.0 | Fields on `hu_learner_t`, `hu_agent_t` |
| **Total C-side static cost** | **~16.0 KB** | matches the brief's ≤16 KB ceiling |

Real cost is **transient RAM** during a TTT step:

| Backend | Transient RSS (MB on M3 Max) | Notes |
|---------|------------------------------|-------|
| CPU fallback | ~5 | Just journal + classifier; no model load |
| ggml | ~180 | Loads frontier-model layers being adapted; releases on step return |
| MLX subprocess | ≤300 | Subprocess RSS budget; main daemon adds <10 MB for IPC buffers |

CI gate: `scripts/check-ttt-budget.sh` (new, parallel to `check-lora-baseline.sh`)
runs `size build-release/human` and asserts the delta vs the most recent
tagged release is ≤16 KB. Wired into `scripts/verify-all.sh`.

The MLX RSS budget is enforced at runtime via `RLIMIT_AS` on the spawned
subprocess; on overflow the trainer aborts with `HU_ERR_RESOURCE_EXHAUSTED`
and the agent_turn block treats it identically to `HU_ERR_TIMEOUT` (no
journal write, no adapter swap, turn proceeds normally).

## 10. arXiv references

- **VDS-TTT** — Liang et al. 2025, *Continuous Self-Improvement of Large
  Language Models by Test-time Training with Verifier-Driven Sample
  Selection*, **arXiv:2505.19475**. The first formulation that pairs
  test-time training with verifier-driven sample selection; reports
  +6.66% over verifier-only without TTT and steady improvement across
  iterations. Our four-gate trigger (§2.1) generalises VDS-TTT's
  confidence-threshold gate by adding the user-correction signal as the
  ground-truth label.
- **In-Place Test-Time Training** — Anonymous 2026, **arXiv:2604.06169**.
  Treats the final MLP projection as fast weights with chunk-wise updates;
  framing as continual learning. We borrow the chunk-wise update bound
  (`max_steps`) and the explicit framing as a continual-learning system.
- **SyTTA** — Lyu et al. 2025, *You only need 4 extra tokens: Synergistic
  Test-time Adaptation for LLMs*, **arXiv:2510.10223**. Dual-signal
  (input-side perplexity + output-side entropy) gating; we adopt the dual-
  signal idea via "verifier score AND user correction" as our gate (§2.1).
- **KeepLoRA** — Luo et al. 2026, *KeepLoRA: Continual Learning with
  Residual Gradient Adaptation*, **arXiv:2601.19659** (ICLR 2026). The
  principled approach to catastrophic-forgetting in LoRA: project gradient
  updates onto the residual subspace orthogonal to the principal subspace
  of the pre-trained model. Our v1 uses a lighter L2-norm cap (§2.7); the
  full KeepLoRA projection is the post-SOTA-2026-01 follow-up.
- **ATLAS** — Singh et al. 2025, *Adaptive Test-Time Latent Steering with
  External Verifiers for Enhancing LLMs Reasoning*, **arXiv:2601.03093**.
  Bonus reference for Init 01 (activation steering); informs the design
  decision to **not** combine TTT with activation steering in v1 (one
  control loop at a time on the inference path).

(All five satisfy D5: ≥2 arXiv references with arXiv ID.)

## 11. Proof bar checklist (D0–D7)

| Gate | Requirement | Pass evidence in this doc |
|------|-------------|---------------------------|
| **D0** | Document at `docs/plans/2026-05-11-init-05-verifier-driven-ttt.md` with YAML frontmatter | This file ✓ |
| **D1** | Maps to `include/human/*.h` vtable additions; new public functions per `naming.md` | `hu_learner_step_bounded`, `hu_learner_rollback_journal`, `hu_ttt_*`, `hu_ml_fidelity_score_response`, all `snake_case`, `hu_<module>_<action>`, types `hu_<name>_t` ✓ (§2.3) |
| **D2** | Every file to create/modify with LOC estimate | §3.1 — 9 NEW + 11 MODIFY + 9 test files ✓ |
| **D3** | ≥1 unit, ≥1 integration test, optional fuzzer | 12 deterministic tests + 2 fuzz harnesses ✓ (§6) |
| **D4** | Top 3 risks with mitigations | 13 risks tabulated ✓ (§7) |
| **D5** | ≥2 arXiv refs with arXiv ID | 5 refs ✓ (§10) |
| **D6** | Binary KB delta + RSS ceiling | ~16 KB MinSizeRel + ≤300 MB transient RSS ✓ (§9) |
| **D7** | Defer/descope condition | §8 — 3 explicit empirical gates with parking procedure ✓ |

## 12. Cross-initiative dependencies

| Init | Surface | TTT depends on | Notes |
|------|---------|----------------|-------|
| **04 — MLX Qwen3** | `hu_provider_t.load_adapter`, MLX subprocess | TTT P3 onwards needs real adapter swap | Without Init 04, TTT runs in shadow mode (CPU no-op) |
| **02 — MoLoRA channels** | `hu_provider_load_adapter` per-channel | TTT scopes to active channel's adapter | If both ship, MoLoRA decides *which* adapter, TTT decides *how* it updates |
| **06 — SimPO/ORPO/GRPO-2** | `hu_rl_trainer_t` vtable | TTT may swap DPO loss for SimPO under flag | SimPO drops the reference policy → cheaper for TTT; flagged follow-up |
| **07 — ThinkPRM verifier** | `hu_reward_model_t` | Replaces fidelity-score-as-trigger with trained PRM score | Improves trigger precision; not blocking |
| **09 — memory trust tiers** | `hu_personal_model_t.fact.provenance` | TTT inputs filtered by trust tier (no TTT on third-party-sourced corrections) | Hard security gate when Init 09 ships |
| **14 — public benchmarks** | `eval.c` + `tests/eval/` | TTT drift-gate eval set must be derivable from the public benchmark fixtures | Ensures the drift gate is replicable by external reviewers |

The `hu_provider_t.load_adapter` surface is the **single API contract**
shared between Init 02, 04, 05, and 06. The synthesis section of the parent
program doc (`2026-05-11-sota-2026-massive-team-program.md` §"Synthesis
target") is the right place to coordinate that surface; this doc documents
TTT's needs from it (atomic swap, version bump, KV-cache invalidation
hook) so the synthesis owner has the full constraint set.

## 13. Honest unknowns

These are open after this design pass and require P2/P3 evidence to close:

1. **Is the 0.55 fidelity threshold the right v1 trigger?** We're using the
   `lora-baseline` floor + 5 points; the right number is whatever maximises
   true-positive corrections / minimises false-positive TTT fires on a real
   conversation log. Calibrated in P7 bakein.
2. **Does the heuristic correction classifier survive contact with reality?**
   The classifier is intentionally pure C with no LLM call (latency budget),
   but real correction phrasing is messy. P7 bakein on a 2-week conversation
   log is the answer. If it falls below 70% precision/recall, we fall back
   to a tiny on-device classifier (small linear model over BPE token freq
   features) — same vtable, different impl, no architectural change.
3. **Will the `step_bounded` 200 ms wall hold on rank=16?** Default rank is
   8 (matches W13). If we move to rank 16 for richer adaptation, MLX
   backend latency may exceed 200 ms. P3 measures; if it does, the budget
   becomes a config knob (`ttt.max_ms`), default 200 for rank=8 and 400 for
   rank=16, with the documentation gain as "TTT budgets scale with adapter
   rank."
