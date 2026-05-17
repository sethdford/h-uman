# Design for US-7.10: ORPO/SimPO pilot — Init #06 phase 1 (vtable + one loss head)

## 1. Approach

This story lands the `hu_rl_trainer_t` vtable surface plus exactly one loss
head (SimPO) and a thin CLI router. The vtable shape is **dictated by the
acceptance criteria**, not by Init #06's prose: AC-7.10.1 names three
vtable members (`train_step`, `compute_loss`, `deinit`), a public type enum
`hu_rl_trainer_type_t {DPO, SIMPO, ORPO, GRPO2}`, and a factory named
`hu_rl_trainer_simpo_create`. Init #06 proposed a five-member vtable
(`name/prepare/step/save_adapter/deinit`) and a different factory naming
(`hu_rl_trainer_create_simpo`). The story is the contract; we align to the
story and flag the divergence in §7 so the init plan can be updated
post-merge.

The cheapest design that satisfies all six AC: a minimal vtable
(three function pointers), a single registry/dispatcher file
(`src/ml/rl_trainer.c`), one loss-head file (`src/ml/rl_trainer_simpo.c`),
and a new top-level subcommand `human ml rl-train` that delegates to either
the new SimPO path (via vtable) or the **existing** `hu_dpo_collector_t`
path (no rewrite of DPO). ORPO/GRPO-2 are stubbed to return exit code 2
with a "not yet implemented" message. The golden-loss test (AC-7.10.2) is
the load-bearing test; we make it tractable by giving the SimPO loss head a
**logprob-injection seam** so the test never needs to instantiate a GPT or
LoRA adapter — the analytical loss is computed from injected scalar
logprobs against the canonical SimPO formula. The full forward-pass path
through `hu_gpt_compute_loss`/`gpt_forward` stays available behind the same
seam for the `train_step` E2E test (AC-7.10.3) but is not exercised at
1e-4 tolerance.

The SimPO loss formula (arXiv:2405.14734):
`L = -log(σ(β · ((1/|y_w|)·logπ(y_w|x) - (1/|y_l|)·logπ(y_l|x)) - γ))`.
With `β=0.1`, `γ=0.5`, length-normalized logprobs for chosen/rejected,
both the implementer and the test can compute this in <20 LOC of pure C.

## 2. Existing-code interface notes

- **`docs/plans/2026-05-11-init-06-simpo-orpo-grpo2.md`** (lines 158–300):
  proposes a richer five-member vtable + `hu_pref_batch_t` + `hu_train_telemetry_t`.
  AC-7.10.1 explicitly narrows this to `{train_step, compute_loss, deinit}`.
  We honor the AC. Telemetry is collapsed into `train_step`'s output (a
  single `double *out_loss`); `prepare`/`save_adapter` are absorbed into
  the loss head's internal state (no public surface). Init #06 plan
  diverges from the story — open question in §7.
- **`src/ml/dpo.c`** (line 504): existing DPO loss is
  `-log(sigmoid(β · log_ratio))` with `1e-10` floor on sigmoid to avoid
  `log(0)`. The SimPO head must mirror this floor (same numerical hazard).
  `hu_dpo_collector_t` is **kept as-is** — `--algorithm dpo` routes
  straight to the existing `hu_ml_cli_dpo_train` path; we do NOT wrap it
  behind the new vtable in this story (AC-7.10.4 says "no behavior
  change").
- **`src/ml/gpt.c`** (`gpt_forward` at line 349, vtable wired at 913):
  the model is the `hu_ml_model_t` vtable's `forward` op. For `train_step`,
  the SimPO head calls `model->vtable->forward(...)` on prompt+chosen and
  prompt+rejected, sums token logprobs, length-normalizes, and applies
  the SimPO formula. For the golden test, we bypass this by passing
  logprobs directly via a test-only struct field (`compute_loss` accepts
  `chosen_logprob_sum`, `chosen_token_count`, etc. — see §3).
- **`src/main.c`** (lines 238–248): `human ml <sub>` dispatch is a flat
  `strcmp` chain. Adding `rl-train` is a one-line addition + one new
  `hu_ml_cli_rl_train` entry in `include/human/ml/cli.h`.
- **`src/ml/cli.c`** (line 484, `hu_ml_cli_dpo_train`): the existing DPO
  CLI signature is the template — same `(alloc, argc, argv)` shape,
  same `HU_IS_TEST` skip pattern (line 529 prints "test mode: skipped"
  and returns OK). The new `hu_ml_cli_rl_train` follows this template.

## 3. Concrete file plan

| File | Change | Est LOC |
|---|---|---|
| `include/human/ml/rl_trainer.h` | **ADD**: `hu_rl_trainer_type_t` enum, `hu_rl_trainer_vtable_t`, `hu_rl_trainer_t`, `hu_simpo_config_t`, `hu_rl_trainer_simpo_create`, `hu_rl_trainer_deinit` helper | +90 |
| `src/ml/rl_trainer.c` | **ADD**: thin dispatcher / type→string helpers / `hu_rl_trainer_deinit` that calls vtable->deinit | +60 |
| `src/ml/rl_trainer_simpo.c` | **ADD**: SimPO loss head — context struct, static vtable, `compute_loss` (analytical + injection seam), `train_step` (calls model forward, computes loss, no backward in v1 stub→follow-up), `deinit`, factory | +180 |
| `include/human/ml/cli.h` | **MODIFY**: add `hu_ml_cli_rl_train` prototype | +1 |
| `src/ml/cli.c` | **MODIFY**: add `hu_ml_cli_rl_train` — parses `--algorithm`, dispatches: `dpo`→`hu_ml_cli_dpo_train`, `simpo`→new SimPO path, `orpo`/`grpo2`→exit 2 with message | +90 |
| `src/main.c` | **MODIFY**: add `if (strcmp(sub, "rl-train") == 0)` branch + help line | +3 |
| `tests/test_rl_trainer_simpo.c` | **ADD**: `test_simpo_loss_golden` (1e-4 tolerance against analytical), `test_simpo_loss_floor` (numerical floor at sigmoid≈0), `test_simpo_deinit_idempotent`, `test_simpo_factory_invalid_config_rejects_null` | +160 |
| `tests/test_ml_cli_rl_train.c` | **ADD**: `test_rl_train_simpo_e2e_fixture`, `test_rl_train_dpo_backward_compat`, `test_rl_train_unimplemented_algorithms`, `test_rl_train_missing_algorithm_flag` | +140 |
| `tests/fixtures/simpo_golden.json` | **ADD**: prompt/chosen/rejected as strings + `chosen_logprob_sum`, `chosen_token_count`, `rejected_logprob_sum`, `rejected_token_count`, `beta`, `gamma`, `expected_loss` (computed offline in a Python comment in the JSON for review) | +20 |
| `CMakeLists.txt` | **MODIFY**: register `src/ml/rl_trainer.c`, `src/ml/rl_trainer_simpo.c`, and the two new test files | +6 |
| `src/ml/CLAUDE.md` | **MODIFY**: add `rl-train` row to the subcommand table | +2 |

Total estimate: ~750 LOC, of which ~300 is test/fixture code.

### 3.1 Public header (AC-7.10.1)

```c
/* include/human/ml/rl_trainer.h — sketch, implementer owns final form */
typedef enum {
    HU_RL_TRAINER_DPO    = 0,
    HU_RL_TRAINER_SIMPO  = 1,
    HU_RL_TRAINER_ORPO   = 2,
    HU_RL_TRAINER_GRPO2  = 3,
} hu_rl_trainer_type_t;

typedef struct hu_rl_trainer_vtable {
    hu_error_t (*compute_loss)(void *ctx,
                               const hu_pref_pair_logprobs_t *lp,
                               double *out_loss);
    hu_error_t (*train_step)(void *ctx,
                             const hu_preference_pair_t *pair,
                             double *out_loss);
    void       (*deinit)(void *ctx);
} hu_rl_trainer_vtable_t;

typedef struct hu_rl_trainer {
    void *ctx;
    const hu_rl_trainer_vtable_t *vtable;
    hu_rl_trainer_type_t type;
} hu_rl_trainer_t;

/* Logprob-injection seam for the golden test and for any future caller
 * that already holds tokenized logprobs (e.g. an MLX bridge). */
typedef struct hu_pref_pair_logprobs {
    double chosen_logprob_sum;
    size_t chosen_token_count;
    double rejected_logprob_sum;
    size_t rejected_token_count;
} hu_pref_pair_logprobs_t;

typedef struct hu_simpo_config {
    float beta;     /* default 0.1 per AC fixture */
    float gamma;    /* default 0.5 per AC fixture */
    hu_ml_model_t *model;   /* nullable; required only for train_step */
} hu_simpo_config_t;

hu_error_t hu_rl_trainer_simpo_create(hu_allocator_t *alloc,
                                      const hu_simpo_config_t *cfg,
                                      hu_rl_trainer_t *out);
void hu_rl_trainer_deinit(hu_rl_trainer_t *trainer);
```

`train_step` taking a single `hu_preference_pair_t *` (not a batch) is the
minimum surface that satisfies AC-7.10.3's "runs without crashing." A
later story can widen to batches when GRPO-2 needs paired rollouts.

## 4. Test plan

| AC | Test | Strategy |
|---|---|---|
| 7.10.1 | `tests/test_rl_trainer_simpo.c::test_simpo_factory_creates_valid_trainer` | Header parse-and-compile is implicit (test file `#include`s it). Test asserts `trainer.type == HU_RL_TRAINER_SIMPO` and all three vtable pointers non-NULL after create. |
| 7.10.2 | `tests/test_rl_trainer_simpo.c::test_simpo_loss_golden` | Load `tests/fixtures/simpo_golden.json`, call `vtable->compute_loss` with the injected logprobs, assert `fabs(out_loss - fixture.expected_loss) < 1e-4`. **Load-bearing test.** |
| 7.10.3 | `tests/test_ml_cli_rl_train.c::test_rl_train_simpo_e2e_fixture` | Invoke `hu_ml_cli_rl_train` with `--algorithm simpo` and a tiny fixture pair list; in `HU_IS_TEST` builds the model forward is mocked to return a fixed logprob (matches existing DPO test pattern at `cli.c:529`); assert `HU_OK` returned. |
| 7.10.4 | `tests/test_ml_cli_rl_train.c::test_rl_train_dpo_backward_compat` | Invoke `--algorithm dpo`; assert the call path lands in `hu_ml_cli_dpo_train` (verified by either the existing "test mode: skipped" stdout or a counter incremented by a test-only shim). |
| 7.10.5 | `tests/test_ml_cli_rl_train.c::test_rl_train_unimplemented_algorithms` | Invoke `--algorithm orpo` then `--algorithm grpo2`; both return exit code 2 (mapped to `HU_ERR_NOT_SUPPORTED`) and write a "not yet implemented" message to stderr. |
| 7.10.6 | CI build job | `-Wall -Wextra -Wpedantic -Werror` + ASan are already the `dev` preset defaults; no story-specific work needed. |

Extra defensive tests (not AC-required but cheap):
- `test_simpo_loss_floor` — feed logprobs that produce `sigmoid(...) ≈ 0`,
  assert no NaN/Inf and loss is clamped via the same `1e-10` floor DPO uses.
- `test_simpo_deinit_idempotent` — call `hu_rl_trainer_deinit` twice; second
  call must be a no-op (set `vtable = NULL` after first deinit, guard in
  helper).
- `test_rl_train_missing_algorithm_flag` — invoke without `--algorithm`;
  assert clear error message and exit code != 0.

## 5. Risks

### Risk 1: Golden fixture analytical loss must be reproducible (HIGH / MEDIUM)
**Failure mode:** The 1e-4 tolerance is tight. If the fixture's
`expected_loss` is computed in Python with float64 but the C code uses
float32 anywhere in the loss chain, the test will flake.
**Mitigation:** The `compute_loss` path is **pure double-precision in C**.
The injection seam (`hu_pref_pair_logprobs_t`) is `double`, not `float`.
The `expected_loss` in the fixture JSON is computed with the exact same
formula and `double` precision in a comment block at the top of the JSON
file (with Python reference snippet) so a reviewer can re-derive it.
No model forward pass touches the golden test.

### Risk 2: DPO backward-compat regression (MEDIUM / LARGE)
**Failure mode:** `--algorithm dpo` re-routes through new vtable code
and subtly changes DPO behavior (e.g. different default β, different
DB path argument parsing, different exit code semantics).
**Mitigation:** `--algorithm dpo` in `hu_ml_cli_rl_train` does
`return hu_ml_cli_dpo_train(alloc, argc_minus_algo_flag, argv_minus_algo_flag)`.
**No new code in the DPO path.** `hu_dpo_collector_t` is NOT wrapped
behind the vtable in this story (Init #06 plan §3.2 already flagged this:
"existing DPO surface stays. The new vtable is additive."). Test
`test_rl_train_dpo_backward_compat` is the gate.

### Risk 3: Init #06 plan and the AC disagree on the vtable shape (MEDIUM / SMALL)
**Failure mode:** A future reviewer reads Init #06, expects
`prepare`/`save_adapter`, sees a three-member vtable, and files a bug.
The ORPO/GRPO-2 implementers may also be blocked needing `prepare` for
tokenization or `save_adapter` for the LoRA write-out.
**Mitigation:** §7 (Open Questions) flags this explicitly. The
three-member vtable can be extended in a follow-on story without
breaking AC-7.10.1 (it asserts only that those three exist, not that
they're the only ones). For now: tokenization happens inside `train_step`
on first call; adapter save happens in a future story when a loss head
actually trains weights.

## 6. Sequencing

1. **Skeleton header.** Create `include/human/ml/rl_trainer.h` with the
   enum, vtable struct, opaque trainer struct, config struct, factory
   prototype. Compile check: `cmake --build --preset dev --target human` —
   header included by nothing yet but must parse standalone.
2. **Dispatcher stub.** Create `src/ml/rl_trainer.c` with
   `hu_rl_trainer_deinit` (vtable->deinit + NULL the vtable pointer for
   idempotency). Build still green.
3. **SimPO loss head (compute_loss only).** Create
   `src/ml/rl_trainer_simpo.c` with the static vtable, the context
   struct, `compute_loss` implementing the SimPO formula from injected
   logprobs, `deinit`, and `hu_rl_trainer_simpo_create`. `train_step`
   and any model-forward integration return `HU_ERR_NOT_SUPPORTED` for now.
4. **Golden test (AC-7.10.2).** Write `tests/fixtures/simpo_golden.json`
   with hand-computed `expected_loss`. Write `test_simpo_loss_golden`.
   Verify: `./build/human_tests --filter=test_simpo_loss_golden` PASSES
   at 1e-4. This is the load-bearing gate; do not move past until green.
5. **Floor + factory tests.** Add `test_simpo_loss_floor`,
   `test_simpo_deinit_idempotent`, `test_simpo_factory_creates_valid_trainer`.
6. **`train_step` minimal impl.** Wire `train_step` to call
   `cfg->model->vtable->forward` on prompt+chosen and prompt+rejected,
   sum logprobs, length-normalize, hand off to `compute_loss`. No backward
   pass in v1 (a TODO referencing the follow-up story). In `HU_IS_TEST`
   builds, accept a NULL model and return a fixed mock loss so the CLI
   test in step 8 doesn't need to boot a real GPT.
7. **CLI router (AC-7.10.3/4/5).** Add `hu_ml_cli_rl_train` to
   `src/ml/cli.c`; parse `--algorithm`; dispatch. Wire into
   `src/main.c`. Add header prototype. Build: `cmake --build --preset dev`.
8. **CLI tests.** Write `tests/test_ml_cli_rl_train.c` with the four
   test cases. Verify: `./build/human_tests --suite=ml_cli_rl_train` all
   green.
9. **Full preflight.** Run `scripts/agent-preflight.sh`. Run
   `./build/human_tests` (full suite, expect 0 failures, 0 ASan errors).
   Update `src/ml/CLAUDE.md` subcommand table. Spawn `/verify`.

## 7. Open questions

1. **`human ml rl-train --algorithm dpo` vs existing `human ml dpo-train`:
   alias or distinct entry?** Recommended: **alias** — `rl-train --algorithm dpo`
   internally calls `hu_ml_cli_dpo_train` with rewritten argv (strip the
   `--algorithm dpo` flag pair). This satisfies AC-7.10.4 ("no behavior
   change") cheaply and preserves muscle memory for existing users of
   `dpo-train`. The old `dpo-train` subcommand stays registered in
   `main.c` and the help text. Confirm with product-owner before the
   implementer touches `main.c`.

2. **Init #06 plan vs AC vtable shape — which is canonical going forward?**
   The story's three-member vtable (`train_step`, `compute_loss`,
   `deinit`) wins for this sprint. After merge, Init #06 plan should be
   updated to match — or, alternately, a follow-on story should widen
   the vtable to the five-member form (`name`, `prepare`,
   `save_adapter` added) before ORPO/GRPO-2 land. Recommended: **update
   Init #06** to document the three-member v1 surface and the planned
   widening, so the next implementer doesn't re-introduce the divergence.
   This is a docs-only follow-up.

3. **Factory naming: `hu_rl_trainer_simpo_create` (AC) vs
   `hu_rl_trainer_create_simpo` (Init #06 plan + naming standard
   `hu_<module>_<action>`).** AC wins. The `<module>_<algo>_<action>`
   form is a documented variant for vtable-family factories (cf.
   `hu_provider_<name>_create`). Note in `src/ml/CLAUDE.md` so the next
   author doesn't refactor it back.

4. **`train_step` signature: single pair vs batch?** Story says
   "runs `train_step` without crashing" — single-pair is sufficient and
   cheapest. ORPO/GRPO-2 will likely want batches; defer until then.

## 8. Acceptance criteria mapping

| AC | Verified by | Step |
|---|---|---|
| AC-7.10.1 | `tests/test_rl_trainer_simpo.c::test_simpo_factory_creates_valid_trainer` + compile of `include/human/ml/rl_trainer.h` | 1, 5 |
| AC-7.10.2 | `tests/test_rl_trainer_simpo.c::test_simpo_loss_golden` (1e-4) | 4 |
| AC-7.10.3 | `tests/test_ml_cli_rl_train.c::test_rl_train_simpo_e2e_fixture` | 8 |
| AC-7.10.4 | `tests/test_ml_cli_rl_train.c::test_rl_train_dpo_backward_compat` | 8 |
| AC-7.10.5 | `tests/test_ml_cli_rl_train.c::test_rl_train_unimplemented_algorithms` | 8 |
| AC-7.10.6 | CI `dev` preset build with `-Werror` + ASan; `/verify` | 9 |
