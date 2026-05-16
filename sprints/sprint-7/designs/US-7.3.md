# Design for US-7.3 (P0): Surface the local-inference honesty gate (INS-B)

## 1. Architecture / Approach

The W13 Phase 4.1 auto-load is **already in place** at `src/daemon.c:2456-2494`. Today, when `hu_provider_load_adapter` returns `HU_ERR_NOT_SUPPORTED` (the cloud-provider case), the daemon emits `hu_log_info("human", agent->observer, "personalization: provider does not support LoRA adapters; skipping '%s'", adapter_id)`. The story requires upgrading this to a **WARN-level** line containing the literal substring `"personalization adapter ignored"` plus the provider name (AC-7.3.1), surfacing the same condition as a `[WARN]` line through `hu_doctor_check_config_semantics` (AC-7.3.2), and leaving the `HU_OK` (local provider) and "no adapter configured" paths silent (AC-7.3.3, AC-7.3.4).

The design is **two surgical edits plus one new test file**:

1. **Daemon path** — change the `HU_ERR_NOT_SUPPORTED` branch in `src/daemon.c` from `hu_log_info` to `hu_log_warn` with the canonical phrase. Gate it behind a **one-shot static flag** (`static int s_warned_once = 0;` colocated with the call site, or — to keep it per-agent — a new `bool personalization_warn_emitted` field on `hu_agent_t`). The static-local form is simpler and survives across reconnects in the same process; reconnect-induced reload paths do not re-execute this bootstrap block, so a process-scoped flag is sufficient.

2. **Doctor path** — extend `hu_doctor_check_config_semantics` (`src/doctor.c:668`) with a small block that runs unconditionally on every `human doctor` invocation: if `cfg->personalization.lora_adapter_path` is set AND `hu_config_provider_requires_api_key(cfg->default_provider)` returns `true` (which is the canonical "cloud provider" classifier already used elsewhere in `doctor.c` at line 66), push a `HU_DIAG_WARN` line with the exact text `"[WARN] personalization.lora_adapter_path is set but the active provider does not support adapters"`. This intentionally does **not** call into the provider vtable — `human doctor` runs without instantiating providers, so we use the same `hu_config_provider_requires_api_key` heuristic that already drives `doctor_config_wants_http`.

The two surfaces share the literal-string contract but **do not share a code path** — the daemon line is emitted live at startup against the actual provider's `load_adapter` return value; the doctor line is emitted statically against the config's `default_provider` name. They cannot drift because each AC tests its own surface against its own literal.

Out of scope per the story: provider-behavior changes, MLX integration, Bridge B.1.

## 2. Concrete File Plan

| Action | File | Purpose |
|---|---|---|
| MODIFY | `src/daemon.c` (lines 2479–2493) | Replace the `HU_ERR_NOT_SUPPORTED` branch's `hu_log_info` with `hu_log_warn`, include literal `"personalization adapter ignored"` + provider name, gate behind a static `s_warned_once` flag. |
| MODIFY | `src/doctor.c` (in `hu_doctor_check_config_semantics`, ~line 668) | Add a new check that emits `HU_DIAG_WARN` when `personalization.lora_adapter_path` is non-empty AND `hu_config_provider_requires_api_key(cfg->default_provider)` returns `true`. |
| MODIFY | `tests/test_provider_all.c` | Add three new tests: `test_cloud_provider_emits_adapter_ignored_warning` (AC-7.3.1), `test_no_adapter_path_no_warning` (AC-7.3.3), `test_llamacpp_provider_no_spurious_warning` (AC-7.3.4). Register them in `run_provider_all_tests()`. Do **not** modify `test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` (AC-7.3.5). |
| ADD | `tests/test_doctor_personalization_warning.c` | New file. Verifies `hu_doctor_check_config_semantics` emits the literal `[WARN]` line for cloud+adapter, and does not emit it for local+adapter or cloud+no-adapter. Register the runner from `tests/test_main.c` (or wherever doctor suites are registered). |
| READ-ONLY-DEP | `include/human/core/log.h` | `hu_log_warn(component, obs, fmt, ...)` macro → routes to `hu_observer_event_t{.tag=HU_OBSERVER_EVENT_ERR, .data.err.message=...}` when observer set, else `fprintf(stderr, "[%s] %s\n", ...)`. |
| READ-ONLY-DEP | `src/providers/helpers.c` (`hu_provider_load_adapter`, line 148) | Returns `HU_ERR_NOT_SUPPORTED` when `vtable->load_adapter == NULL` (cloud providers). Contract unchanged. |
| READ-ONLY-DEP | `src/config_getters.c` (`hu_config_provider_requires_api_key`, line 9) | Canonical "cloud vs local" classifier. `ollama`, `lmstudio`, `llamacpp`, `mlx_local`, `vertex`, `claude_cli`, `codex_cli`, `vllm`, `sglang` return `false`; everything else (openai, anthropic, gemini, openrouter, etc.) returns `true`. Reused by both daemon path and doctor path so the two stay logically aligned. |
| READ-ONLY-DEP | `include/human/observer.h` + `tests/test_observer.c:140` (`hu_log_observer_create(&alloc, FILE *f)`) | The test seam. A `tmpfile()`-backed observer captures log lines as text; `rewind(f); fread(buf,...)` recovers them. Pattern is established in `test_multi_observer_forwards`. |
| READ-ONLY-DEP | `include/human/doctor.h` (`hu_doctor_check_config_semantics`, `hu_diag_item_t`, `HU_DIAG_WARN`) | Doctor public API. No changes. |

## 3. Existing-Code Interface Notes

### 3.1 Logging API (exact signature)

```c
/* include/human/core/log.h */
#define hu_log_warn(component, obs_ptr, ...) hu_log_impl_((component), (obs_ptr), __VA_ARGS__)
```

`hu_log_impl_` formats the message into a 512-byte stack buffer with `vsnprintf`, then either dispatches it as `HU_OBSERVER_EVENT_ERR` through `obs->vtable->record_event` (when `obs && obs->vtable && obs->vtable->record_event`) or falls back to `fprintf(stderr, "[%s] %s\n", component, buf)`. Note: `hu_log_warn` and `hu_log_info` are **the same macro** at runtime — severity is not encoded in the routed event. The story's "warning-level log line" requirement is therefore satisfied **textually** by emitting the literal string `"personalization adapter ignored"`, not by a severity bit on the wire.

### 3.2 Test capture seam (exact pattern)

From `tests/test_observer.c:140-160`:

```c
FILE *f = tmpfile();
hu_observer_t obs = hu_log_observer_create(&alloc, f);
/* ... exercise code that calls hu_log_warn(component, &obs, ...) ... */
hu_observer_flush(obs);
rewind(f);
char buf[2048]; size_t n = fread(buf, 1, sizeof(buf)-1, f); buf[n] = '\0';
HU_ASSERT_TRUE(strstr(buf, "personalization adapter ignored") != NULL);
HU_ASSERT_TRUE(strstr(buf, "openai") != NULL); /* provider name */
fclose(f);
```

The daemon code at the load_adapter site already calls `hu_log_info("human", agent->observer, ...)` — `agent->observer` is the observer the test will inject. The implementation reuses this call path with `hu_log_warn` and the new format string.

### 3.3 Daemon extension point (already exists)

`src/daemon.c:2485-2489` — the existing branch reads:

```c
else if (le == HU_ERR_NOT_SUPPORTED)
    hu_log_info("human", agent->observer,
                "personalization: provider does not support LoRA adapters; "
                "skipping '%s'",
                adapter_id);
```

The implementer replaces these four lines with the new warn (and adds the static one-shot gate around the entire `if (config && ... lora_adapter_path && ...)` block, or just the warn line — see Risk R1).

### 3.4 Doctor extension point (already exists)

`src/doctor.c:668` — `hu_doctor_check_config_semantics(alloc, cfg, items, count)` is the canonical place to add config-level warnings. It already uses `doctor_push_line(alloc, &buf, &n, &cap, HU_DIAG_WARN, line)` (see line 129 et al.). The implementer adds one new block near the existing provider/temperature/gateway-port checks. Doctor does **not** instantiate providers, so it uses `hu_config_provider_requires_api_key(cfg->default_provider)` as the cloud-vs-local proxy.

### 3.5 The provider-classifier seam

`hu_config_provider_requires_api_key` is the closest semantic match to "this provider does not support local adapters today." It is not perfect (e.g. `vertex` returns `false` but doesn't currently implement `load_adapter` either), so the doctor surface intentionally uses this as a heuristic for the operator-visible warning; the daemon surface uses the **actual** `HU_ERR_NOT_SUPPORTED` return value, which is the ground truth. Drift between the two is documented in Risk R3.

## 4. Test Plan (AC → test function)

| AC | Test file::function | Mechanism |
|---|---|---|
| AC-7.3.1 | `tests/test_provider_all.c::test_cloud_provider_emits_adapter_ignored_warning` | Create openai provider; create `hu_log_observer_create(&alloc, tmpfile())`; call the same daemon-shaped sequence (`hu_provider_load_adapter` returns `HU_ERR_NOT_SUPPORTED`); the test exercises a small helper extracted from daemon.c (or copies the four-line branch directly into the test) that calls `hu_log_warn` with `&obs`. Assert `strstr(captured, "personalization adapter ignored") != NULL` AND `strstr(captured, "openai") != NULL`. |
| AC-7.3.2 | `tests/test_doctor_personalization_warning.c::test_doctor_warns_when_cloud_provider_has_lora_path` | Build a `hu_config_t` with `default_provider = "openai"` and `personalization.lora_adapter_path = "/tmp/x.lora"`. Call `hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count)`. Walk `items[]` asserting at least one with `severity == HU_DIAG_WARN` and `message` containing the literal `"personalization.lora_adapter_path is set but the active provider does not support adapters"`. |
| AC-7.3.3 | `tests/test_provider_all.c::test_no_adapter_path_no_warning` | Same observer-capture harness, but with `lora_adapter_path = NULL` (or `""`). Assert `strstr(captured, "personalization adapter ignored") == NULL`. Also: `tests/test_doctor_personalization_warning.c::test_doctor_silent_when_no_adapter_path` — assert no item has the canonical WARN substring. |
| AC-7.3.4 | `tests/test_provider_all.c::test_llamacpp_provider_no_spurious_warning` | Use the llamacpp provider (which implements `load_adapter`). The runtime call returns `HU_OK` (or `HU_ERR_INVALID_ARGUMENT` if the path doesn't exist — adjust fixture or stub). Assert `strstr(captured, "personalization adapter ignored") == NULL`. Doctor side: `test_doctor_silent_when_local_provider` — `default_provider = "llamacpp"`, `lora_adapter_path = "/tmp/x.lora"` → no WARN. |
| AC-7.3.5 | `tests/test_provider_all.c::test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` | **Unchanged.** Do not touch lines 3071–3104 or the runner registration at line 3369. The implementer's diff against this function must be empty; verified by `git diff sprint-7-digital-twin-dpo -- tests/test_provider_all.c | grep -A0 "test_m3_daemon_pattern"`. |

### Test-side helper

The daemon block (`config + provider + lora_adapter_path → call hu_provider_load_adapter → branch on return code → log`) should be extracted to a small static helper `daemon_load_personalization_adapter(agent, config, alloc)` in `daemon.c` so the tests can exercise the exact code path rather than re-implementing the branch. This is a refactor-extract, not a behavior change; the original M3 test continues to call `hu_provider_load_adapter` directly and is unaffected. **If extraction is non-trivial in the available LOC budget**, the tests instead pin the contract by replicating the four-line branch and asserting the log shape — slightly weaker, but still satisfies the AC literal-string requirement.

## 5. Risk Analysis

### R1 — Double-fire on reconnect / daemon restart loop (MEDIUM probability, SMALL impact)
**Scenario:** if the daemon is restarted by a supervisor (systemd, launchd, `human service-loop` re-entry) the bootstrap block at `src/daemon.c:2456` runs each time, emitting the warning on every startup. If the operator has acknowledged the warning, repeated emission becomes noise in the log file.

**Mitigation:** scope a `static int s_personalization_warn_emitted = 0;` at file scope in `daemon.c` near the call site, and guard the new `hu_log_warn` call with `if (!s_personalization_warn_emitted) { hu_log_warn(...); s_personalization_warn_emitted = 1; }`. This is per-process — across actual restarts the message fires once per process lifetime, which is the desired honesty signal. Test note: tests that exercise the warn path twice in the same process must reset this flag via a `HU_IS_TEST`-only setter (e.g. `daemon_personalization_warn_reset_for_test()`), or the second assertion will silently fail. Document this in the test header comment.

**Alternative considered:** stash the flag on `hu_agent_t`. Adds a struct field for a single transient signal — rejected as over-engineering for LOW-risk tier work.

### R2 — Regressing AC-7.3.5 (existing M3 daemon-pattern test) (LOW probability, LARGE impact)
**Scenario:** the implementer refactors the daemon bootstrap block (e.g. extracts a helper) and accidentally changes the call shape — `hu_provider_load_adapter` still returns `HU_ERR_NOT_SUPPORTED` but the test's `prov.vtable->chat_with_system` now sees a freed provider, or `active_adapter` is no longer NULL.

**Mitigation:** the existing test calls `hu_provider_load_adapter` directly on a provider it owns — it does **not** route through daemon.c. So as long as `hu_provider_load_adapter`'s public contract (signature, return values) is unchanged in `src/providers/helpers.c`, this test cannot regress. The implementer's diff to `src/providers/helpers.c` must be empty; verified in the implementer pre-flight: `git diff sprint-7-digital-twin-dpo -- src/providers/helpers.c` returns no output. Belt-and-suspenders: the implementer runs `./build/human_tests --filter=test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` after every increment.

### R3 — Drift between daemon classifier (runtime return code) and doctor classifier (config name heuristic) (MEDIUM probability, SMALL impact)
**Scenario:** a provider name that `hu_config_provider_requires_api_key` classifies as "cloud" (returns `true`) actually implements `load_adapter` and returns `HU_OK` at runtime — or vice versa. Doctor warns but the daemon doesn't (or doctor stays silent while the daemon warns), confusing the operator.

**Mitigation:** today this is theoretical — every provider in the list that returns `true` (openai, anthropic, gemini, openrouter, vertex-as-cloud) has no `load_adapter` vtable hook; only `llamacpp` implements it, and that's already on the `false` side. The design accepts the heuristic as the doctor's source of truth and documents it in a comment at the new doctor block. **If a future provider breaks this assumption** (e.g. anthropic ships LoRA), the fix is a one-line update to the heuristic — not a structural change. No downstream tool currently parses `human doctor` output as a contract (greppable by humans only); adding one new `[WARN]` line is additive and breaks no parsers we control.

## 6. Sequencing (for the implementer agent)

1. **Daemon warn upgrade.** In `src/daemon.c:2485-2489`, replace the `HU_ERR_NOT_SUPPORTED` branch with a `hu_log_warn` call that includes the literal `"personalization adapter ignored"` plus the provider name (extract via `agent->provider.vtable->get_name(agent->provider.ctx)` if available, else use `config->default_provider`). Add the static one-shot flag and the `HU_IS_TEST` reset shim.
   - **Verify:** `cmake --build --preset dev && ./build/human_tests --filter=test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` → PASS.

2. **Extract test-callable helper (optional but recommended).** Lift the existing `if (config && ... lora_adapter_path && ...)` block (currently ~30 LOC) into `static void daemon_load_personalization_adapter(hu_agent_t *agent, const hu_config_t *config, hu_allocator_t *alloc)` inside `daemon.c`. The original call site becomes a single call to that helper. No behavior change.
   - **Verify:** `cmake --build --preset dev` clean; full daemon-related suites unchanged: `./build/human_tests --suite=Provider --suite=Daemon`.

3. **Add the three provider-side tests.** In `tests/test_provider_all.c`, add `test_cloud_provider_emits_adapter_ignored_warning`, `test_no_adapter_path_no_warning`, `test_llamacpp_provider_no_spurious_warning` using the `hu_log_observer_create(&alloc, tmpfile())` pattern from `tests/test_observer.c:140`. Register them in `run_provider_all_tests()` after the existing M3 test.
   - **Verify:** `./build/human_tests --filter=test_cloud_provider_emits_adapter_ignored_warning --filter=test_no_adapter_path_no_warning --filter=test_llamacpp_provider_no_spurious_warning` → all PASS; AC-7.3.5 still PASS.

4. **Doctor warning.** In `src/doctor.c::hu_doctor_check_config_semantics` (~line 668), after the existing temperature/gateway-port checks, add the `lora_adapter_path` + cloud-provider check using `doctor_push_line(alloc, &buf, &n, &cap, HU_DIAG_WARN, "[WARN] personalization.lora_adapter_path is set but the active provider does not support adapters")`. Reuse `hu_config_provider_requires_api_key(cfg->default_provider)` as the cloud classifier (already imported into `doctor.c` at line 66).
   - **Verify:** `./build/human_tests --suite=Doctor` → still green.

5. **Add the doctor test file.** Create `tests/test_doctor_personalization_warning.c` with three cases: `test_doctor_warns_when_cloud_provider_has_lora_path`, `test_doctor_silent_when_no_adapter_path`, `test_doctor_silent_when_local_provider`. Wire its `run_doctor_personalization_warning_tests()` into the suite registration in `tests/test_main.c` (or wherever doctor suites register today — implementer greps `run_doctor` to find).
   - **Verify:** `./build/human_tests --filter=test_doctor_warns_when_cloud_provider_has_lora_path --filter=test_doctor_silent_when_no_adapter_path --filter=test_doctor_silent_when_local_provider` → all PASS.

6. **Full preflight.** `scripts/agent-preflight.sh` (auto-detects change scope) → must report 0 failures, 0 ASan errors. Then `./build/human_tests` full suite → must report 9,800+ tests passing, 0 failures.
   - **Verify:** spawn `/verify` agent. Expect `RESULT_verifier=PASS`.

## 7. Acceptance Criteria → Behavior/Test Mapping (compact)

- AC-7.3.1 → `tests/test_provider_all.c::test_cloud_provider_emits_adapter_ignored_warning` (literal `"personalization adapter ignored"` + provider name in captured log).
- AC-7.3.2 → `tests/test_doctor_personalization_warning.c::test_doctor_warns_when_cloud_provider_has_lora_path` (literal `"[WARN] personalization.lora_adapter_path is set but the active provider does not support adapters"` in `hu_diag_item_t` with `HU_DIAG_WARN`).
- AC-7.3.3 → `tests/test_provider_all.c::test_no_adapter_path_no_warning` and `tests/test_doctor_personalization_warning.c::test_doctor_silent_when_no_adapter_path`.
- AC-7.3.4 → `tests/test_provider_all.c::test_llamacpp_provider_no_spurious_warning` and `tests/test_doctor_personalization_warning.c::test_doctor_silent_when_local_provider`.
- AC-7.3.5 → `tests/test_provider_all.c::test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` — diff to this function must be empty.

## 8. Open Questions for Seth

1. **One-shot gate scope.** Per-process (`static int` in `daemon.c`) vs per-agent (field on `hu_agent_t`). Design defaults to per-process for simplicity. If the daemon is expected to re-bootstrap mid-process (e.g. config hot-reload re-enters the `personalization.enabled` block), we'd want per-agent. Today no such reload path exists, so per-process is fine.
2. **Warning suppression.** Should we honor a config flag like `personalization.suppress_ignored_warning = true` for operators who deliberately configure an adapter path "just in case the provider gains support"? Story does not require it; design omits it (YAGNI).
3. **Provider name source.** Prefer `agent->provider.vtable->get_name(agent->provider.ctx)` (the ground-truth runtime name) or `config->default_provider` (the configured name)? Design recommends the vtable getter when available, falling back to config — both should match in practice.
