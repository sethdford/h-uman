# Design for US-7.7 (P1): Test-time persona scoring — best-of-N at inference

## 1. Architecture / Approach

The story's framing ("provider calls completion exactly 4 times") suggests a provider-internal loop, but the codebase pushes us toward a **higher-level decorator** for three load-bearing reasons:

1. **A daemon-level best-of-N already exists** at `src/daemon.c:9438–9504` keyed off `config->agent.best_of_n`, scoring with `hu_turing_score_heuristic` + persona-alignment. It generates extras via `hu_agent_turn(...)` and keeps the best. The story's new `inference.best_of_n` is **a different selector** (fidelity scorer, llamacpp-only) but **the same architectural shape** (decorator above provider, not inside it). Implementing inside `src/providers/llamacpp.c` would create two parallel best-of-N machines on different scoring metrics — one in the daemon, one in the provider — that fight each other when both are configured.
2. **llamacpp.c is no longer the stub the PO note claims.** The chat path is real (lines 185–353): tokenize → KV cache lookup → `llama_decode` of full prompt → sampler decode loop → detokenize. Sampling temperature/top_k/top_p/min_p/seed all live inside `llamacpp_chat_with_system`. To re-sample N times from inside that function, we would have to either re-decode the full prompt (wasting the KV cache work that's the whole point of Phase 1 RL SOTA) or factor the sampler loop out and re-enter it N times with reseeded sampler state. Both are invasive surgery on a hot path that just landed.
3. **Provider vtable stability.** Sprint risk policy says "no vtable changes outside US-7.8/US-7.10." Threading `best_of_n` into `hu_llamacpp_config_t` is technically additive (the existing `factory.c` already destructures the struct), but a decorator pattern keeps the vtable contract untouched and the provider config struct minimal.

**Decision: option (b) — higher-level decorator.** A new `hu_best_of_n_chat` helper wraps the provider's `chat_with_system` call. Gated on `config->inference.best_of_n >= 2` AND the active provider's `get_name()` returns `"llamacpp"` (any other name → straight passthrough, single call, no scoring). The wrapper lives in a new file `src/agent/best_of_n.c` with header `include/human/agent/best_of_n.h`, and is invoked from the existing chat invocation point inside `src/agent/agent_turn.c`. The fidelity score for selection is `hu_communication_style_fidelity_score(&personal_model->style, response, response_len)` — signature unchanged (AC-7.7.6).

**Telemetry.** There is no central `hu_telemetry_record(event, fields...)` API in the codebase — telemetry is per-feature counters on `hu_agent_t` (e.g. `verifier_*`, `world_model_loads`, `self_rag_*`) plus `hu_log_info`/`hu_log_warn` lines routed through the observer. We follow that pattern: add four `uint64_t` counters to `hu_agent_t` (`best_of_n_invocations`, `best_of_n_cost_cap_hits`, `best_of_n_picks_above_first`, `best_of_n_total_candidates`) and emit one `hu_log_info` line per pick (and one per cost-cap hit) containing the literal strings `"best_of_n_pick"` / `"best_of_n_cost_cap_hit"` with the AC-7.7.4 field tags inline. The captured-observer pattern from US-7.3 (`hu_log_observer_create(&alloc, tmpfile())`) is the test seam — no new module needed.

**One-shot warning for cloud-provider misconfiguration (AC-7.7.3).** Follows D4's pattern exactly: `static int s_best_of_n_warn_emitted = 0;` in `src/doctor.c`, fired from `hu_doctor_check_config_semantics` when `cfg->inference.best_of_n >= 2 && hu_config_provider_requires_api_key(cfg->default_provider) == true`. Same `HU_IS_TEST` reset shim convention. Doctor surface only — does **not** fire from the daemon path, because the daemon path is the silent-passthrough case (decorator detects non-llamacpp provider and skips). Doctor is the operator-visible signal.

## 2. Concrete File Plan

| Action | File | Purpose | LOC |
|---|---|---|---|
| ADD | `include/human/agent/best_of_n.h` | Public declaration of `hu_best_of_n_chat(...)` + `hu_best_of_n_config_t`. | +35 |
| ADD | `src/agent/best_of_n.c` | The decorator: loop N times calling `provider->chat_with_system`, score each via `hu_communication_style_fidelity_score`, track best, honor cost cap via `clock_gettime(CLOCK_MONOTONIC)` deltas. Emits `hu_log_info` telemetry lines. Increments agent counters via a small `hu_best_of_n_stats_t` out-param so the helper stays decoupled from `hu_agent_t`. | +180 |
| MODIFY | `include/human/config.h` | Add `typedef struct hu_inference_config { uint32_t best_of_n; uint32_t cost_cap_ms; } hu_inference_config_t;` and embed as `hu_inference_config_t inference;` in `hu_config_t` (after `hu_personalization_config_t personalization;` at line 651). | +12 |
| MODIFY | `src/config_parse.c` (or `config_parse_agent.c` style — implementer greps for top-level section parsers) | Parse `"inference"` JSON object: keys `best_of_n` (uint, clamped 0..8), `cost_cap_ms` (uint, 0 == disabled). | +35 |
| MODIFY | `src/config_merge.c` | Merge `inference` block (default-on-zero pattern; whichever side is non-zero wins, matching the merge style of `agent.best_of_n`). | +15 |
| MODIFY | `src/config_serialize.c` | Round-trip the new keys to JSON. | +20 |
| MODIFY | `src/config_validate.c` | Clamp `best_of_n > 8 → 8`; `cost_cap_ms` accepts any uint32 (0 means no cap). | +10 |
| MODIFY | `include/human/agent.h` | Add four `uint64_t` best-of-N counters to `hu_agent_t` near the existing `verifier_*` block (~line 274). | +6 |
| MODIFY | `src/agent/agent_turn.c` | At the provider `chat`/`chat_with_system` invocation site, branch: if `agent->config->inference.best_of_n >= 2` AND provider name == `"llamacpp"` AND `agent->personal_model && agent->personal_model->style.sample_count > 0` → route through `hu_best_of_n_chat`; else passthrough (no behavior change). | +30 |
| MODIFY | `src/doctor.c` | Add the cloud-provider warning block in `hu_doctor_check_config_semantics` mirroring D4's one-shot pattern. | +25 |
| ADD | `tests/test_llamacpp_best_of_n.c` | Five tests covering AC-7.7.1, 7.7.2, 7.7.4, 7.7.5 + a fidelity-tie tiebreak test. Uses a mock `hu_provider_t` whose `chat_with_system` returns a sequence of fixed strings from a fixture vector; injects a `hu_communication_style_t` fingerprint that produces known fidelity scores per string. No llamacpp linkage required. | +320 |
| ADD | `tests/test_doctor_best_of_n_warning.c` | Three tests for AC-7.7.3 (cloud + best_of_n >= 2 → warn; local + best_of_n >= 2 → silent; cloud + best_of_n in {0,1} → silent). | +130 |
| MODIFY | `tests/test_main.c` (or wherever doctor + llamacpp suites register) | Register the two new runners. | +4 |
| READ-ONLY-DEP | `include/human/memory/personal_model.h:535` | `hu_communication_style_fidelity_score(target, response, response_len) → float [0,1] or -1 sentinel`. Unmodified (AC-7.7.6). |  |
| READ-ONLY-DEP | `src/config_getters.c::hu_config_provider_requires_api_key` | Reused classifier from US-7.3 design (D4). |  |
| READ-ONLY-DEP | `include/human/core/log.h::hu_log_info`/`hu_log_warn` | Observer-routable log macros. |  |

## 3. Existing-Code Interface Notes

### 3.1 The fidelity scorer (frozen)

```c
/* include/human/memory/personal_model.h:535 */
float hu_communication_style_fidelity_score(const hu_communication_style_t *target,
                                            const char *response, size_t response_len);
```

Returns `[0.0, 1.0]` on success, `-1.0f` on NULL inputs or `target->sample_count == 0`. The wrapper treats `-1.0f` as "skip scoring; this candidate is unscored, fall back to first-completion behavior." If **every** candidate scores `-1.0f`, return the first one and emit `best_of_n_unscored_fallback`. This matters because cold-start agents (no `personal_model` populated) would otherwise behave non-deterministically.

### 3.2 Provider vtable invocation in `agent_turn.c`

The implementer greps for `vtable->chat\b` or `chat_with_system(` to locate the call site. The decorator wraps the existing call shape:

```c
/* before */
err = agent->provider.vtable->chat(agent->provider.ctx, alloc, &req, model, model_len, temp, &resp);

/* after */
if (best_of_n_eligible(agent)) {
    err = hu_best_of_n_chat(agent, alloc, &req, model, model_len, temp, &resp);
} else {
    err = agent->provider.vtable->chat(agent->provider.ctx, alloc, &req, model, model_len, temp, &resp);
}
```

`hu_best_of_n_chat` internally calls `chat_with_system` (not `chat`) N times — `chat_with_system` is the lower-level path that takes flat `(system, user)` strings, which is what the fidelity scorer needs to consume. The decorator translates `hu_chat_request_t` → `(sys, user)` via the same role-walking loop already present at `src/providers/llamacpp.c:370–378`. This duplication is acceptable — extracting it to a shared helper is a follow-on refactor, not in this story.

### 3.3 Cost-cap clock

`clock_gettime(CLOCK_MONOTONIC, &ts)` matches the existing pattern elsewhere in the daemon. Computed once at decorator entry (`t_start`), checked after each completion (`t_now - t_start >= cost_cap_ms * 1e6` in ns). The check fires **after** a completion returns, not as a preemption — preempting an in-flight `llama_decode` is not feasible without thread cancellation, which we will not introduce. This means the cap is a soft cap (worst case: N-th completion that pushes us over the cap still runs to completion before we return the best-so-far). Documented in the cost-cap event log line.

### 3.4 Telemetry counters

Following `agent.h:274` (`verifier_*`), `agent.h:288` (`producer_*`), `agent.h:311` (self-RAG):

```c
/* additions to hu_agent_t */
uint64_t best_of_n_invocations;        /* turns where N >= 2 fired */
uint64_t best_of_n_cost_cap_hits;      /* turns truncated by cost cap */
uint64_t best_of_n_picks_above_first;  /* picked candidate index > 0 */
uint64_t best_of_n_total_candidates;   /* sum of N actually executed */
```

A read-only snapshot accessor `hu_agent_best_of_n_telemetry(const hu_agent_t *, ...)` mirrors `hu_agent_self_rag_telemetry` at `agent.h:666` so doctor/dashboard can render without reaching into the struct.

### 3.5 The one-shot warning seam (mirrors D4)

```c
/* src/doctor.c — near hu_doctor_check_config_semantics */
static int s_best_of_n_warn_emitted = 0;
#ifdef HU_IS_TEST
void hu_doctor_best_of_n_warn_reset_for_test(void) { s_best_of_n_warn_emitted = 0; }
#endif
```

The warning literal (AC-7.7.3): `"[WARN] inference.best_of_n has no effect with cloud providers"`. Pushed via `doctor_push_line(alloc, &buf, &n, &cap, HU_DIAG_WARN, ...)` exactly as US-7.3's design specifies.

## 4. Test Plan (AC → test function)

| AC | Test file::function | Mechanism |
|---|---|---|
| AC-7.7.1 | `tests/test_llamacpp_best_of_n.c::test_best_of_4_returns_highest_score` | Build a fake `hu_provider_t` with `get_name()` returning `"llamacpp"` and a `chat_with_system` that walks a 4-element fixed-string fixture (`call_count` static, returns `fixture[call_count++]`). Construct a `hu_communication_style_t` fingerprint that yields known scores `{0.4, 0.7, 0.5, 0.6}` for the four strings. Set `inference.best_of_n=4`, `cost_cap_ms=0`. Call `hu_best_of_n_chat`. Assert `call_count == 4`, returned `resp.content == fixture[1]` (highest score). |
| AC-7.7.2 | `tests/test_llamacpp_best_of_n.c::test_best_of_1_is_single_call` | Set `inference.best_of_n=1`. Call the wrapper. Assert `call_count == 1`, returned content == `fixture[0]`. Repeat with `best_of_n=0`. The eligibility check in `agent_turn.c` is also exercised: with `best_of_n < 2`, `hu_best_of_n_chat` is not entered at all (verified via a separate assertion on the decorator branch). |
| AC-7.7.3 | `tests/test_doctor_best_of_n_warning.c::test_doctor_warns_when_cloud_provider_has_best_of_n` | Build `hu_config_t` with `default_provider="openai"`, `inference.best_of_n=4`. Call `hu_doctor_best_of_n_warn_reset_for_test()`; call `hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count)`. Walk `items[]` asserting at least one with `HU_DIAG_WARN` and the literal `"inference.best_of_n has no effect with cloud providers"`. Two negative tests: `test_doctor_silent_when_local_provider` (`default_provider="llamacpp"`) and `test_doctor_silent_when_best_of_n_disabled` (`best_of_n=0`). |
| AC-7.7.4 | `tests/test_llamacpp_best_of_n.c::test_best_of_n_telemetry_emitted` | Set up `hu_log_observer_create(&alloc, tmpfile())`. Run a 4-candidate scenario. After completion, `rewind(f); fread(...)`. Assert the captured buffer contains the literal `"best_of_n_pick"` AND `"n=4"` AND `"picked_score=0.7"` (formatted to 2 dp) AND `"min_score=0.4"` AND `"max_score=0.7"`. Also assert agent counters: `best_of_n_invocations==1`, `best_of_n_total_candidates==4`, `best_of_n_picks_above_first==1`. |
| AC-7.7.5 | `tests/test_llamacpp_best_of_n.c::test_cost_cap_returns_best_seen` | Inject a `HU_IS_TEST`-only mock clock seam (`hu_best_of_n_clock_fn_t` function pointer; production calls `clock_gettime`, test injects a stepper that returns `t=0, 50ms, 60ms, 130ms, ...`). Set `cost_cap_ms=100`. Run a 4-candidate scenario. Assert `call_count == 3` (third call pushes us past 100ms; fourth never fires), returned content matches the highest-scored of the first 3, captured log contains `"best_of_n_cost_cap_hit"` with `n_completed=3`. Counter `best_of_n_cost_cap_hits == 1`. |
| AC-7.7.6 | `tests/test_llamacpp_best_of_n.c::test_fidelity_signature_unchanged` | Compile-only test that calls `hu_communication_style_fidelity_score(&style, "abc", 3)` with the exact signature from `include/human/memory/personal_model.h:535`. Plus a `git diff sprint-7-digital-twin-dpo -- include/human/memory/personal_model.h` check in the implementer pre-flight that returns empty for that line range. |

Additional non-AC coverage (defensive):

- `test_best_of_n_all_candidates_unscored_returns_first`: every candidate scores `-1.0f` → return `fixture[0]`, log `"best_of_n_unscored_fallback"`.
- `test_best_of_n_non_llamacpp_provider_passthrough`: provider name `"openai"` → wrapper exits early, single call, no scoring.
- `test_best_of_n_provider_error_on_first_propagates`: first completion returns `HU_ERR_PROVIDER_RESPONSE` → wrapper returns that error (don't retry).
- `test_best_of_n_provider_error_on_third_keeps_best_so_far`: third completion errors → return best of first two, log `"best_of_n_partial_failure"`.

## 5. Risks

### R1 — "Called exactly 4 times" semantics when the provider is the linked-but-failing llamacpp (MEDIUM probability, MEDIUM impact)

**Scenario:** `HU_LLAMACPP_LINKED=1` but the GGUF model isn't loaded (`c->model == NULL`). `chat_with_system` returns `HU_ERR_NOT_SUPPORTED` on the first call (see `llamacpp.c:200`). What does "exactly 4 times" mean here? The test only sees one call before bailing.

**Mitigation:** the wrapper's contract is "call up to N times until either (a) the cost cap fires, (b) N successes, or (c) a hard error on the first call short-circuits." On non-first errors, return the best so far (test `test_best_of_n_provider_error_on_third_keeps_best_so_far`). On first-call hard error, propagate (test `test_best_of_n_provider_error_on_first_propagates`). AC-7.7.1's "exactly 4 times" is interpreted in the test as "the mock completion function's call counter equals 4 when all 4 calls succeed" — which is what the AC literally asserts in its `verified by` clause. This interpretation is documented at the top of `tests/test_llamacpp_best_of_n.c`.

### R2 — Cost cap clock determinism in tests (MEDIUM probability, SMALL impact)

**Scenario:** Tests that use real `clock_gettime` are flaky on slow CI runners — a 100ms cost cap might fire on call 2 in one run and call 3 in another, breaking the assertion.

**Mitigation:** introduce a function-pointer seam `hu_best_of_n_clock_fn_t` in the wrapper (production = `clock_gettime`-backed, test = injected stepper that returns deterministic timestamps from a fixture array). Production code does not pay any cost for the seam — a static function pointer initialized to the real implementation, no virtual dispatch. The seam is `HU_IS_TEST`-resettable via `hu_best_of_n_set_clock_fn_for_test(fn)`. Mirrors the existing `hu_ml_nll_compute_fn_t` pattern documented in US-7.6's design (D3).

### R3 — Telemetry noise: 4 candidates × every chat turn could flood logs (MEDIUM probability, SMALL impact)

**Scenario:** When best_of_N is on, every chat turn produces 4 completions. If we emitted one log line per candidate, a 60-message-per-hour chat would emit 240 log lines/hour — operator log noise.

**Mitigation:** the wrapper emits **exactly one** `best_of_n_pick` log line per turn (containing the aggregate `n=`, `picked_score=`, `min_score=`, `max_score=` fields), plus **at most one** `best_of_n_cost_cap_hit` line when the cap fires. Per-candidate scores are aggregated into the single pick line, not emitted individually. The four-counter snapshot on `hu_agent_t` gives doctor/dashboard the per-turn aggregate without parsing log lines.

## 6. Sequencing (for the implementer agent)

1. **Config plumbing.** Add `hu_inference_config_t inference` to `hu_config_t`; wire it through parse/merge/serialize/validate. Defaults are zeros (`best_of_n=0` ≡ disabled).
   - **Verify:** `./build/human_tests --suite=Config` → still green; new round-trip test `config_inference_roundtrip` PASS.

2. **Wrapper + header skeleton.** Create `include/human/agent/best_of_n.h` and `src/agent/best_of_n.c` with the function declared and a stub body that just calls `chat_with_system` once and returns. Add it to `CMakeLists.txt` (or whatever globs `src/agent/*.c`).
   - **Verify:** `cmake --build --preset dev` clean.

3. **Agent counter fields.** Add the four `uint64_t` counters to `hu_agent_t` + the read-only snapshot accessor. Initialize to 0 in `hu_agent_create`.
   - **Verify:** `cmake --build --preset dev` clean; `./build/human_tests --suite=Agent` still green.

4. **Decorator wiring in agent_turn.c.** Add the eligibility branch around the provider chat invocation. With the wrapper still a passthrough stub, behavior must be byte-for-byte identical to pre-story.
   - **Verify:** `./build/human_tests` full suite → 9,800+ pass, zero ASan.

5. **Implement the loop body.** Replace the stub: N iterations, fidelity scoring, cost-cap clock seam, telemetry counters + the single `best_of_n_pick` log line + cost-cap log line. Honor all the corner cases from R1 (first-call error propagation, mid-loop error retains best-so-far, all-unscored fallback).
   - **Verify:** `./build/human_tests --filter=test_best_of_4_returns_highest_score --filter=test_best_of_1_is_single_call --filter=test_best_of_n_telemetry_emitted --filter=test_cost_cap_returns_best_seen` → all PASS.

6. **Doctor warning.** Add the cloud-provider warning block to `hu_doctor_check_config_semantics` with the one-shot static flag + `HU_IS_TEST` reset shim (mirrors D4). Add `tests/test_doctor_best_of_n_warning.c` with three cases.
   - **Verify:** `./build/human_tests --filter=test_doctor_warns_when_cloud_provider_has_best_of_n --filter=test_doctor_silent_when_local_provider --filter=test_doctor_silent_when_best_of_n_disabled` → all PASS.

7. **AC-7.7.6 signature pin.** Run `git diff sprint-7-digital-twin-dpo -- include/human/memory/personal_model.h | grep "fidelity_score"` → must be empty. Add this as a comment at the top of `tests/test_llamacpp_best_of_n.c` so a future critic notices if it's ever modified.
   - **Verify:** diff is empty.

8. **Full preflight + verify.** `scripts/agent-preflight.sh`; full `./build/human_tests`; spawn `/verify`.
   - **Verify:** `RESULT_verifier=PASS`.

## 7. Acceptance Criteria → Behavior/Test Mapping (compact)

- AC-7.7.1 → `tests/test_llamacpp_best_of_n.c::test_best_of_4_returns_highest_score`
- AC-7.7.2 → `tests/test_llamacpp_best_of_n.c::test_best_of_1_is_single_call`
- AC-7.7.3 → `tests/test_doctor_best_of_n_warning.c::test_doctor_warns_when_cloud_provider_has_best_of_n` (+ two negative tests)
- AC-7.7.4 → `tests/test_llamacpp_best_of_n.c::test_best_of_n_telemetry_emitted` (log capture + counter snapshot)
- AC-7.7.5 → `tests/test_llamacpp_best_of_n.c::test_cost_cap_returns_best_seen` (mock-clock seam)
- AC-7.7.6 → compile-only `test_fidelity_signature_unchanged` + empty-diff check on `personal_model.h`

## 8. Open Questions for Seth

1. **Coexistence with `agent.best_of_n`.** The daemon already has a best-of-N driven by `hu_turing_score_heuristic` at `daemon.c:9438`. If both `agent.best_of_n >= 2` AND `inference.best_of_n >= 2` AND provider is llamacpp, **two layers of best-of-N fire** (the inner one selects 4-from-4 by fidelity inside each provider call; the outer one then asks for additional candidates and re-scores by turing+persona). Design assumes that's acceptable — they compose, multiplicatively. Alternative: have the doctor emit an `[INFO]` line when both are set, noting the multiplicative cost. Recommend: ship the multiplicative behavior, add the info line in a follow-on if operators complain. **Confirm: ship as multiplicative.**
2. **Cost-cap unit.** Story says `inference.best_of_n_cost_cap_ms`. Design uses wall-clock monotonic. Is wall-clock the right axis, or should it be sum-of-completion-latencies (which equals wall-clock here since we're sequential)? They're identical for sequential calls; flagging only because a future parallel-completion implementation would diverge. **No action needed in this story.**
3. **Provider name match.** Design hardcodes `strcmp(provider_name, "llamacpp") == 0`. Should `huml` (the reference GPT) also qualify? Story's title says "llamacpp provider" explicitly, so we restrict to that name. If `mlx_local` ships later (Bridge B.1), it will need to be added explicitly — documented as a follow-on. **Confirm: llamacpp-only.**

RESULT_tech-lead=READY
