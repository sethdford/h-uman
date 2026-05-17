# Design for US-7.6 (P1): Judgment-fidelity eval (INS-A) — held-out perplexity on real continuations

**Sprint:** sprint-7-digital-twin-dpo
**Story:** US-7.6
**Risk tier:** MEDIUM (new fn in `src/ml/fidelity.c`; extends `check-lora-ab.sh`)
**Author:** tech-lead
**Status:** READY FOR IMPLEMENTATION

---

## 1. Architecture / approach

Add a held-out **mean negative log-likelihood (NLL)** scorer alongside the existing lexical-surface metrics. The scorer is purely additive: a new function `hu_ml_fidelity_score_judgment` next to the existing `hu_ml_fidelity_score_baseline`, plus a new struct `hu_ml_judgment_summary_t`, plus a new helper `hu_ml_fidelity_load_holdout` that reads the JSONL fixture. **No existing signature changes.**

The load-bearing design choice is the **mock seam**: `hu_ml_nll_compute_fn_t` is a function-pointer typedef (`hu_error_t (*)(const char *prompt, size_t prompt_len, const char *continuation, size_t cont_len, void *ctx, double *out_nll)`) injected via a setter `hu_ml_fidelity_set_nll_compute_fn(fn, ctx)` that is `HU_IS_TEST`-gated for the override path but always present as a public symbol. Production callers do not call the setter; a process-wide default fn pointer (a `static` in `fidelity.c`) provides the real implementation. This pattern mirrors how other modules in the tree do dependency injection without `#ifdef`-poisoning the production call site.

**The real-implementation question (the one Seth needs to confirm — see Open Questions §7):** for this story, the production default `hu_ml_nll_compute_fn_t` returns `HU_ERR_NOT_SUPPORTED`. The CLI emits `"judgment_ppl": null` and a `"judgment_ppl_status": "not_supported_no_local_inference"` field when the production seam returns that error. This is consistent with US-7.3 (honesty gate) and the M3 "Bridge A daemon-pattern proven, frontier model bridge planned" status quo from the project CLAUDE.md. Wiring NLL into `src/ml/gpt.c` (reference HUML GPT, would actually work in CI) **or** into the active provider via a new `chat_with_logprobs` vtable method **is a follow-on story**. This story ships the seam, the holdout fixture, the gate plumbing, and the deterministic mock test that proves the gate catches a degenerate adapter. That is what the AC actually require — AC-7.6.3 forbids loading real weights in CI; AC-7.6.1 says "or base model if no adapter is active," which the `not_supported` status satisfies because no adapter is active in CI.

---

## 2. Concrete file plan

| Path | Action | Purpose | Est. LOC |
|---|---|---|---|
| `include/human/ml/fidelity.h` | MODIFY | Add `hu_ml_judgment_summary_t`, `hu_ml_judgment_holdout_row_t`, `hu_ml_judgment_holdout_t`, `hu_ml_nll_compute_fn_t`, `hu_ml_fidelity_set_nll_compute_fn`, `hu_ml_fidelity_load_holdout`, `hu_ml_fidelity_free_holdout`, `hu_ml_fidelity_score_judgment`, `hu_ml_fidelity_default_holdout_path` | +90 |
| `src/ml/fidelity.c` | MODIFY | Implement the above; add `static hu_ml_nll_compute_fn_t g_nll_fn` with a default that returns `HU_ERR_NOT_SUPPORTED` and `void *g_nll_ctx`; reset hook for tests | +160 |
| `src/ml/cli.c` | MODIFY | Add `--judgment` flag to `hu_ml_cli_fidelity_status`; emit `judgment_ppl`, `judgment_ppl_status`, `judgment_scored`, `judgment_skipped` keys; emit `null` + status string when seam returns `HU_ERR_NOT_SUPPORTED` | +60 |
| `tests/test_ml_fidelity_judgment.c` | ADD | `test_judgment_ppl_computed_on_holdout` (AC-7.6.1), `test_judgment_ppl_catches_degenerate_adapter` (AC-7.6.2), `test_nll_seam_never_loads_real_weights` (AC-7.6.3), `test_baseline_signature_unchanged_compile_check` (AC-7.6.4), `test_holdout_loads_at_least_ten_rows`, `test_holdout_skips_malformed_rows`, `test_production_default_returns_not_supported` | +280 |
| `tests/fixtures/judgment_fidelity_holdout.jsonl` | ADD | ≥ 10 synthetic `{prompt, continuation}` JSONL rows, **zero PII**, deterministic UTF-8 ASCII, channel-diverse | +12 lines |
| `tests/fixtures/judgment_fidelity_holdout_malformed.jsonl` | ADD | Tiny fixture with one good row + one malformed + one empty (for skip-counting test) | +3 lines |
| `scripts/check-lora-ab.sh` | MODIFY | Accept `--judgment` flag; when set, run `human ml fidelity-status --judgment --persona ... --output ...` and extract `judgment_ppl_delta` (after − before); fall through gracefully when status is `not_supported_no_local_inference` (emit `[lora-ab-gate] SKIP judgment_ppl (no local inference)` and continue) | +35 |
| `tests/test_check_lora_ab_judgment.sh` | ADD | Drives `check-lora-ab.sh --judgment` with `LORA_AB_BIN` pointing at a stub binary (or the real one in `not_supported` mode); asserts JSON contains `judgment_ppl_delta` key OR `judgment_ppl_status: not_supported_no_local_inference` and exit code 0 | +60 |
| `CMakeLists.txt` (or `tests/CMakeLists.txt`) | MODIFY | Register `test_ml_fidelity_judgment.c` in the test target; ensure JSONL fixtures are copied to build dir if test resolution depends on it | +6 |
| `tests/CMakeLists.txt` or `scripts/run-shell-tests.sh` | MODIFY (if pattern exists) | Wire `test_check_lora_ab_judgment.sh` into the shell-test runner. If no harness, add a `ctest` `add_test(NAME ... COMMAND bash ...)` entry | +5 |

**READ-ONLY dependencies (referenced, not modified):**

- `include/human/memory/personal_model.h` — `hu_communication_style_t`, `hu_communication_style_set_summary_t`, `hu_communication_style_fidelity_score` — **must not change** (AC-7.6.4 + sprint non-goal §272).
- `include/human/persona.h` — `hu_persona_t`, `hu_persona_example_bank_t`.
- `src/ml/cli.c::hu_ml_cli_fidelity_status` — existing fidelity-status command we are extending. Lines 2247–2397 (read).
- `scripts/check-lora-ab.sh` — existing gate script we are extending. Pattern: env-var floor + awk float compare.
- `tests/fixtures/lora_baseline_persona.json` — fixture-shape reference (zero PII, deterministic strings).

---

## 3. Existing-code interface notes

**Confirmed PO note is correct:** `hu_communication_style_fidelity_score` is declared in `include/human/memory/personal_model.h` (line 535), not in `fidelity.h`. We do not call it from the judgment path — judgment uses NLL, not surface metrics — so no signature contact.

**Existing signature (must remain byte-identical per AC-7.6.4):**
```c
hu_error_t hu_ml_fidelity_score_baseline(const hu_persona_t *persona,
                                         const hu_communication_style_t *target,
                                         hu_communication_style_set_summary_t *out_summary);
```
Callers we will NOT touch:
- `src/ml/cli.c:2306` (`hu_ml_cli_fidelity_status`)
- `src/gateway/cp_admin.c:1118` area (gateway `metrics.fidelity`)
- `src/ml/lora-baseline` CLI path

**Seam signature (precise — implementer must match exactly):**
```c
/* Compute mean negative log-likelihood of `continuation` conditioned on `prompt`.
 *
 * Returns:
 *   HU_OK            — `*out_nll` populated with a finite double >= 0.0
 *   HU_ERR_NOT_SUPPORTED — no inference backend wired (production default in
 *                        builds without a local model); caller treats this as
 *                        "judgment scoring unavailable", NOT a failure.
 *   any other error  — hard failure; caller propagates.
 *
 * MUST NOT allocate memory the caller has to free. MUST be reentrant.
 * In `HU_IS_TEST` builds with a registered mock, MUST NOT touch the filesystem,
 * network, or model weights — the verifier asserts on this. */
typedef hu_error_t (*hu_ml_nll_compute_fn_t)(const char *prompt,
                                             size_t prompt_len,
                                             const char *continuation,
                                             size_t continuation_len,
                                             void *ctx,
                                             double *out_nll);

/* Replace the process-wide NLL computer. Pass NULL fn to restore the default
 * (which returns HU_ERR_NOT_SUPPORTED). Thread-safety: not thread-safe; call
 * from main thread only, during setup. */
void hu_ml_fidelity_set_nll_compute_fn(hu_ml_nll_compute_fn_t fn, void *ctx);

/* Default path: $PWD/tests/fixtures/judgment_fidelity_holdout.jsonl with fallback
 * to $HU_JUDGMENT_HOLDOUT env var. Returns a borrowed pointer; do not free. */
const char *hu_ml_fidelity_default_holdout_path(void);

typedef struct {
    char *prompt;       /* alloc-owned, NUL-terminated */
    size_t prompt_len;
    char *continuation; /* alloc-owned, NUL-terminated */
    size_t continuation_len;
} hu_ml_judgment_holdout_row_t;

typedef struct {
    hu_ml_judgment_holdout_row_t *rows;
    size_t rows_count;
    size_t rows_capacity;
} hu_ml_judgment_holdout_t;

hu_error_t hu_ml_fidelity_load_holdout(hu_allocator_t *alloc,
                                       const char *path,
                                       hu_ml_judgment_holdout_t *out);
void hu_ml_fidelity_free_holdout(hu_allocator_t *alloc,
                                 hu_ml_judgment_holdout_t *holdout);

typedef struct {
    size_t scored;           /* rows where NLL fn returned HU_OK */
    size_t skipped;          /* rows where NLL fn returned HU_ERR_NOT_SUPPORTED
                                or row was malformed */
    double mean_nll;         /* arithmetic mean over scored rows; 0.0 if scored==0 */
    double min_nll;          /* over scored rows; 0.0 if scored==0 */
    double max_nll;          /* over scored rows; 0.0 if scored==0 */
    bool available;          /* false iff scored == 0 (treated as
                                judgment_ppl_status="not_supported_no_local_inference"
                                in CLI/JSON output) */
} hu_ml_judgment_summary_t;

hu_error_t hu_ml_fidelity_score_judgment(const hu_ml_judgment_holdout_t *holdout,
                                         hu_ml_judgment_summary_t *out_summary);
```

`hu_ml_fidelity_score_judgment` is the only function that consumes the seam. It iterates rows, calls `g_nll_fn(...)` for each, and accumulates per the contract. PPL exposed at the CLI is `exp(mean_nll)`; we store mean_nll (lossless) in the summary and let the CLI/script compute PPL if needed. (Storing mean_nll means the summary is numerically invariant under rescaling; PPL is just a display convention.)

**Fixture format** (`tests/fixtures/judgment_fidelity_holdout.jsonl`, one JSON object per line):
```jsonl
{"prompt":"ready when you are","continuation":"ok cool, give me a sec","channel":"telegram"}
{"prompt":"can you send the doc?","continuation":"sure, just a sec — sending now","channel":"slack"}
{"prompt":"thanks!","continuation":"np","channel":"sms"}
... (≥ 10 rows total)
```
- `channel` is optional metadata (not consumed in this story; reserved for the per-channel breakdown that is explicitly out of scope).
- All strings ASCII-printable, no real names, no real phone numbers, no real URLs, no real emails. The fixtures look like a `lora_baseline_persona.json`-style synthetic exchange.
- Rationale for ≥ 10: not statistical significance — this is a **regression sentinel**, not a research-grade eval. 10 rows is the minimum at which a mock returning `nll = 0.5` for chosen continuations and `nll = 0.9` for a "degenerate adapter" (which the test installs) produces a clearly non-zero, deterministic delta. With < 10 rows the test becomes brittle to small fixture edits. The real eval (when the bridge lands) will use thousands of rows; this fixture exists to keep the *plumbing* honest.

**Existing `check-lora-ab.sh` extension pattern:**
- Already uses `set -euo pipefail`, env-var-overridable thresholds, `awk` float compare.
- New: parse the optional `--judgment` flag (positional arg, simplest); when set, read `judgment_ppl` from before & after JSON via `grep`/`awk` (or `jq` if it's already an accepted runtime dep — confirm; if not, stick to `awk`).
- New env var: `LORA_AB_JUDGMENT_PPL_DELTA_FLOOR` (default e.g. `0.05`; "after_ppl - before_ppl" must be ≤ negative floor → lower PPL is better, so we expect a **negative** delta for a good adapter).
- Graceful skip when `judgment_ppl_status == "not_supported_no_local_inference"`: emit `[lora-ab-gate] SKIP judgment_ppl (no local inference backend)` and continue with the existing lexical-surface gate. This is what lets US-7.5's nightly cron run honestly: the gate is in place, it just declares itself inactive until the bridge lands.

---

## 4. Test plan (AC → exact test function → mock setup)

| AC | Test function | Mock / fixture setup |
|---|---|---|
| AC-7.6.1 | `tests/test_ml_fidelity_judgment.c::test_judgment_ppl_computed_on_holdout` | Register a mock NLL fn that returns `0.5 + 0.01 * row_index` deterministically. Load `tests/fixtures/judgment_fidelity_holdout.jsonl`. Call `hu_ml_fidelity_score_judgment`. Assert `summary.scored == N_rows`, `summary.skipped == 0`, `summary.mean_nll` within `1e-9` of expected mean, `summary.available == true`. Also drives the CLI end-to-end: invoke `hu_ml_cli_fidelity_status` with `--judgment`, capture stdout JSON, assert `judgment_ppl` key present and equals `exp(expected_mean)` within `1e-6`. |
| AC-7.6.2 | `test_judgment_ppl_catches_degenerate_adapter` | Two registered mocks in sequence: (a) "baseline" returns `nll = 0.5` flat for all rows; (b) "degenerate adapter" returns `nll = 1.2` flat for all rows (mimicking an adapter that has degraded the model's probability over real continuations even though its surface metrics would pass). Compute summaries for both, compute `delta = mean_nll_after − mean_nll_before = 0.7` (positive = adapter made it worse). Assert `delta > 0`, i.e., the gate (whose floor is "delta must be ≤ −LORA_AB_JUDGMENT_PPL_DELTA_FLOOR") would FAIL the degenerate adapter. The companion lexical-surface metrics in the same persona fixture must PASS (precondition the test asserts to prove it's the judgment metric that caught the regression). |
| AC-7.6.3 | `test_nll_seam_never_loads_real_weights` | In `HU_IS_TEST` mode, register a mock that asserts via a counter-variable in `ctx`. Run the full judgment pipeline. Assert `counter == N_rows` AND the production default (`g_nll_fn_default`) was never invoked. Additionally: a second sub-test runs WITHOUT registering a mock (i.e., production default active) and asserts `hu_ml_fidelity_score_judgment` returns `HU_OK` with `summary.available == false` and `summary.scored == 0` — proving the default returns `HU_ERR_NOT_SUPPORTED` and does NOT attempt to mmap any model file. Use a `getenv("HU_FORBIDDEN_WEIGHT_LOAD")=1` sentinel + a check that `open(2)` on any path under `~/.human/models/` is never made (use a temp `HOME` to make this checkable). |
| AC-7.6.4 | `test_baseline_signature_unchanged_compile_check` | Pure compile-only test: take `&hu_ml_fidelity_score_baseline`, assign it to a local `hu_error_t (*)(const hu_persona_t *, const hu_communication_style_t *, hu_communication_style_set_summary_t *)` typed pointer. If the signature changed, the build breaks. Plus a runtime call with the existing `lora_baseline_persona.json` fixture asserting unchanged mean (golden value to 1e-6). |
| AC-7.6.5 | `tests/test_check_lora_ab_judgment.sh` | Build human, point `LORA_AB_BIN` at it. Run `bash scripts/check-lora-ab.sh --judgment`. Assert: (a) exit code 0, (b) stdout contains either `judgment_ppl_delta` numeric line OR `SKIP judgment_ppl (no local inference backend)` line, (c) the existing `[lora-ab-gate] PASS: fixture delta=...` line is still present (we did not break the lexical-surface gate). |

Bonus targeted tests:
- `test_holdout_loads_at_least_ten_rows`: count rows in shipped fixture, assert ≥ 10.
- `test_holdout_skips_malformed_rows`: load `judgment_fidelity_holdout_malformed.jsonl`, assert `scored + skipped` matches row count and `skipped >= 1`.
- `test_production_default_returns_not_supported`: pure unit on the default fn.

All tests must be ASan-clean and run in <100 ms.

---

## 5. Risk analysis

**Risk 1 — "Seam ships but real implementation never lands" (PROBABILITY HIGH, IMPACT MEDIUM).**
The shape of M3 in CLAUDE.md says the frontier-model bridge is the unsolved problem of the project. There is real institutional incentive to land the seam and call this done. If that happens, US-7.5's nightly cron will gate on a metric that always reports `not_supported_no_local_inference` and never actually catches anything — exactly the failure mode this story exists to prevent.
*Mitigation:* (a) The seam's contract is published in the header with explicit semantics for `HU_ERR_NOT_SUPPORTED` so the follow-on story has an unambiguous interface to implement against. (b) Open a follow-on story in this sprint's backlog **as part of this story's PR** — titled "US-7.6.1: wire hu_ml_nll_compute_fn_t to src/ml/gpt.c reference HUML GPT" — referencing this design doc by SHA. The reference GPT *can* run in CI (it's already used by `lora-persona --checkpoint` per CLAUDE.md M3), so it's a legitimate, CI-runnable real implementation that doesn't require the frontier bridge. (c) `scripts/check-lora-ab.sh --judgment` emits a visible `SKIP` line — not a silent pass — so anyone reading CI output sees the metric is dormant. (d) Eval evidence in `sprints/sprint-7/evidence/US-7.6/` must include a screenshot/log showing the `SKIP` line, so reviewers see the dormancy before merge.

**Risk 2 — "Fixture too small to be statistically meaningful, gate becomes flaky" (PROBABILITY LOW, IMPACT MEDIUM).**
With only 10 rows, a single fixture edit can move the mean meaningfully.
*Mitigation:* This is a **regression sentinel, not a research eval.** The test never asserts a precise numeric mean against the real NLL fn (which doesn't exist yet); it asserts against the **mock** NLL fn's known outputs, which are deterministic regardless of fixture size. The fixture's only contract is "≥ 10 well-formed rows." When the real fn lands, the follow-on story will (a) increase the fixture to ~100 rows and (b) set the floor based on measured baseline variance. The 10-row floor in *this* story exists to keep the loader's loop honest (loops over zero items are trivially passing) and to provide test signal — nothing else. Document this explicitly in the JSONL file's leading `#`-comment or in a sibling README so future Seth doesn't ratchet the threshold prematurely.

**Risk 3 — "Breaks existing callers of `hu_ml_fidelity_score_baseline`" (PROBABILITY LOW, IMPACT HIGH).**
AC-7.6.4 forbids signature change; sprint non-goal §272 forbids changing `hu_communication_style_fidelity_score`. A careless `include/human/ml/fidelity.h` edit (e.g., adding a typedef *before* the existing prototype that shadows a typename) could break ABI/API silently.
*Mitigation:* (a) Strictly additive header changes — new declarations APPEND to the file; do not reorder, retype, or rename anything existing. (b) The compile-only test in §4 ensures the function-pointer type of `&hu_ml_fidelity_score_baseline` is unchanged. (c) Run `grep -rn hu_ml_fidelity_score_baseline src/ tests/` before commit; every existing caller must still compile and pass without edits. (d) `clang-tidy` on the worktree must be clean.

---

## 6. Sequencing (5–7 implementer steps)

1. **Skeleton + fixture + loader.** Add new types and prototypes to `include/human/ml/fidelity.h`. Implement `hu_ml_fidelity_load_holdout` and `hu_ml_fidelity_free_holdout` in `src/ml/fidelity.c`. Create both `tests/fixtures/judgment_fidelity_holdout.jsonl` (≥ 10 rows, zero PII) and the malformed sibling. Verify: `cmake --build --preset dev && ./build/human_tests --filter=test_holdout_loads_at_least_ten_rows` passes; `test_holdout_skips_malformed_rows` passes; no ASan errors.

2. **Seam + default + setter.** Add the `static hu_ml_nll_compute_fn_t g_nll_fn` (initialized to a `default_nll_fn_not_supported`), `static void *g_nll_ctx`, and the public `hu_ml_fidelity_set_nll_compute_fn`. Add the production-default unit test. Verify: `./build/human_tests --filter=test_production_default_returns_not_supported` passes; `grep -rn hu_ml_fidelity_score_baseline src/ tests/` shows zero callers required edits.

3. **Score function + AC-7.6.1 happy-path test.** Implement `hu_ml_fidelity_score_judgment`. Write `test_judgment_ppl_computed_on_holdout` with the deterministic mock. Verify: targeted test green; ASan-clean. Also run `./build/human_tests --suite=fidelity` to confirm no prior tests regressed.

4. **AC-7.6.2 + AC-7.6.3 adversary tests.** Write `test_judgment_ppl_catches_degenerate_adapter` and `test_nll_seam_never_loads_real_weights`. The second test should temporarily set `HOME` to a tmpdir and assert no `open(2)` on `~/.human/models/` (use an `LD_PRELOAD`-style hook OR just verify the default fn was never invoked — the latter is simpler and equally correct because the default is the *only* code path that would touch weights). Verify: `./build/human_tests --filter=test_judgment_ppl_catches_degenerate_adapter` AND `--filter=test_nll_seam_never_loads_real_weights` both green.

5. **CLI wiring.** Extend `hu_ml_cli_fidelity_status` in `src/ml/cli.c`: parse `--judgment`, call the new score function, emit JSON with `judgment_ppl`, `judgment_ppl_status`, `judgment_scored`, `judgment_skipped` keys (with `null` PPL when status is `not_supported_no_local_inference`). Write the CLI integration assertion in `test_judgment_ppl_computed_on_holdout` (it already drives the CLI per §4). Verify: `./build/human ml fidelity-status --persona lora_baseline_fixture --judgment` prints valid JSON containing the new keys (manual smoke); `./build/human_tests --filter=fidelity` all green.

6. **Shell-gate extension + AC-7.6.5 test.** Add `--judgment` to `scripts/check-lora-ab.sh` with the graceful-skip path. Add `tests/test_check_lora_ab_judgment.sh`. Wire it into the shell-test runner / `ctest`. Verify: `bash scripts/check-lora-ab.sh --judgment` exits 0 and prints either delta line or SKIP line; `bash tests/test_check_lora_ab_judgment.sh` exits 0; the existing `bash scripts/check-lora-ab.sh` (no flag) still passes unchanged.

7. **Full preflight + evidence capture.** Run `scripts/agent-preflight.sh` (auto-detects changed files), then full suite `./build/human_tests`. Capture stdout of `human ml fidelity-status --judgment` and the shell-gate output to `sprints/sprint-7/evidence/US-7.6/` (showing the `SKIP` line so reviewers see the seam is intentionally dormant). Then `/verify` to capture `RESULT_verifier=PASS`.

---

## 7. Open questions for Seth

1. **Production default: `HU_ERR_NOT_SUPPORTED` or wire to `src/ml/gpt.c` now?** This design assumes deferred (the seam ships dormant, with a planned follow-on story to wire the reference HUML GPT). The reference GPT is already used elsewhere in the tree (per CLAUDE.md M3 status) and can run in CI without frontier-model weights, so wiring it now is **possible** — it would just expand this story from M to L. Confirm: ship dormant (recommended; matches AC-7.6.3's "no real weights in CI" and keeps the story M-sized) OR expand scope to include the gpt.c wiring (would still satisfy AC because the reference GPT is small enough to run in CI deterministically)?

2. **`jq` runtime dependency in `check-lora-ab.sh`?** The existing script uses `awk`/`grep` to avoid a `jq` dep. The new `--judgment` flag will parse two JSON keys (`judgment_ppl`, `judgment_ppl_status`). `awk` works but is uglier. Confirm `jq` is acceptable in CI (a quick `command -v jq` check + skip with a warning when missing is a safe middle ground), or stick to awk?

3. **Default delta floor.** Recommended `LORA_AB_JUDGMENT_PPL_DELTA_FLOOR=0.05` (meaning: after-PPL must be at least 5% lower than before-PPL in nat units = mean_nll improves by 0.05). This is a guess; once the bridge lands and real numbers exist, the follow-on story will measure variance and pick a real floor. OK to ship the placeholder, with the JSONL fixture's leading comment documenting "this is a placeholder; measure before tightening"?

---

## 8. AC traceability matrix

| AC | Where covered |
|---|---|
| AC-7.6.1 | `test_judgment_ppl_computed_on_holdout` (C + CLI assertion) |
| AC-7.6.2 | `test_judgment_ppl_catches_degenerate_adapter` |
| AC-7.6.3 | `test_nll_seam_never_loads_real_weights` + `test_production_default_returns_not_supported` |
| AC-7.6.4 | `test_baseline_signature_unchanged_compile_check` + grep audit step in sequencing §6.2 |
| AC-7.6.5 | `tests/test_check_lora_ab_judgment.sh` |
| Sprint non-goal §272 (no `hu_communication_style_fidelity_score` change) | Not touched; verified by header-diff grep |

---

## 9. Out of scope (explicit)

- Real NLL via loaded frontier model weights (Bridge B — separate sprint).
- Wiring NLL to `src/ml/gpt.c` reference HUML GPT (follow-on story US-7.6.1 — pending answer to Open Question §7.1).
- Per-channel PPL breakdown (Init #02 / MoLoRA territory — separate story).
- A `metrics.fidelity` gateway extension for judgment PPL — the gateway path is `cp_admin.c`'s domain and can adopt the new helper in a follow-on once the production default actually works.
- Statistically rigorous holdout sizing — current 10-row floor is a regression sentinel only.
