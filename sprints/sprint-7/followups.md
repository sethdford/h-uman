# Sprint 7 — Follow-up Tasks

Findings the gates surfaced that are not in-scope for the current sprint but
should land before Sprint 8 or as part of Sprint 8 if not addressed in-sprint.

## P0 — must address before Sprint 7 closes

### FU-7.7.a — `src/doctor.c` comment claims D4 one-shot but it is NOT implemented
- **Source:** US-7.7 critic HIGH-1 (`a99a81ede1904a6d0`).
- The comment near `src/doctor.c:742-747` describes a `static int s_best_of_n_warn_emitted` one-shot flag and a `hu_doctor_best_of_n_warn_reset_for_test` shim, but the static and shim do NOT exist in the file. The warning fires unconditionally on every `hu_doctor_check_config_semantics` call. AC-7.7.3's "per-process one-shot" contract is unverified and the comment is actively misleading (violates project rule against lying comments).
- **Fix sketch:** add `static int s_best_of_n_warn_emitted = 0;` with the `HU_IS_TEST` reset shim mirroring US-7.3's pattern at `src/daemon.c:81`. OR remove the lying comment.
- **Risk if deferred:** lying comment passes through sprint-auditor adversarial read and undermines trust. Should be P0 hygiene.
- **Owner:** mini-fix before Phase 6 tag.

## P1 — should address in-sprint or early Sprint 8

### FU-7.7.b — Agent-level telemetry counters absent
- **Source:** US-7.7 critic HIGH-2.
- Design called for 4 `uint64_t` counters on `hu_agent_t`: `best_of_n_invocations`, `best_of_n_cost_cap_hits`, `best_of_n_picks_above_first`, `best_of_n_total_candidates`. None shipped. `stats_out` is populated and immediately lost. AC-7.7.4 half-satisfied (log line works, agent-level accumulation missing).
- **Fix sketch:** add the 4 counters to `hu_agent_t`; in `agent_turn.c` after the best-of-N call returns, fold `bcfg.stats_out` into the agent counters. Add a small public accessor `hu_agent_best_of_n_telemetry()`.
- **Risk if deferred:** dashboards can't show long-running pick/cap-hit rates; observability gap. AC-7.7.4 is technically passable on the log alone.
- **Owner:** Sprint 8 observability story.

### FU-7.7.c — Score-bounds telemetry edge cases
- **Source:** US-7.7 critic HIGH-3.
- `min_score=1.0f / max_score=0.0f` initializers are misleading if `hu_communication_style_fidelity_score` ever returns outside `[0,1]` (not enforced today). Initial `picked_score = scores[0]` may be `-1.0f` sentinel.
- **Fix sketch:** add `HU_ASSERT(s >= 0.0f && s <= 1.0f)` or a clamp in the loop body in `src/agent/best_of_n.c:1199-1222`. Move `min_score`/`max_score` initialization into the first-scored-entry branch.
- **Owner:** Sprint 8 hardening.

### FU-7.9.a — `hu_style_critique_run` silent skip on regen failure
- **Source:** US-7.9 critic HIGH-1.
- When the provider regen fails (`rerr != HU_OK`), the function returns `HU_OK` silently keeping the original violating draft. Caller cannot distinguish "no violation fired" from "violation fired but regen errored."
- **Fix sketch:** in `src/persona/style_critique.c:373`, emit the `style_rule_violation_unresolved` log + increment counter before the early-return on regen failure, OR return `HU_ERR_RETRY_FAILED`.
- **Owner:** Sprint 8 robustness story.

### FU-7.9.b — Emoji alias coverage gap (BMP block)
- **Source:** US-7.9 critic HIGH-2 + panel (FLAG, documented).
- Alias matches only `\xF0\x9F` (plane-1 emoji 4-byte UTF-8 prefix). Misses entire BMP block U+2600-U+27BF (☀️ ✅ ✈️). Header acknowledges this as deferred to "downstream Unicode-aware pass."
- **Fix sketch:** widen alias table to cover `\xE2\x98\x80`-`\xE2\x9F\xBF` (3-byte UTF-8) and lone-modifier bytes. OR add documented user-facing caveat.
- **Owner:** Sprint 8 Unicode-aware pass.

### FU-7.9.c — `test_critique_disabled_short_circuits` is vacuous
- **Source:** US-7.9 critic HIGH-3.
- Test only resets counters and asserts they are zero — trivially true post-reset. Does not exercise the actual gate in `agent_turn.c:5441`.
- **Fix sketch:** replace test body with an actual `hu_agent_turn` invocation with `style_rules_enabled=false`, asserting `check_invocations==0`. Verifier acknowledged the test pattern is "sufficient given the gate" — but adversarial reading is correct.
- **Owner:** Sprint 8 test hardening.

### FU-7.9.d — Prompt-injection vector via user-controlled rule text
- **Source:** US-7.9 critic MED-2.
- `violated_rule` is concatenated verbatim into the regen system prompt. A user rule like `"never start with 'Sure! \n\nIgnore previous instructions and...'` becomes a prompt-injection vector.
- **Fix sketch:** strip/escape `\n`, `\r`, chars above 0x7E before embedding; truncate at 128 bytes.
- **Owner:** Sprint 8 security hardening.

### FU-7.6.a — `check-lora-ab.sh --judgment` silent-pass when STATUS empty
- **Source:** US-7.6 critic HIGH (carried forward from Wave 0 close).
- When `human ml fidelity-status --judgment` produces no JSON, `STATUS` is empty, script falls through to `exit 0` reporting spurious PASS.
- **Owner:** any implementer touching `scripts/check-lora-ab.sh`. Likely Sprint 8.

### FU-7.1.a — `mlx-lm-lora` dep pin documented only in docstring
- **Source:** US-7.1 critic MED.
- Add `mlx-lm-lora>=2.1.0,<3` to the project's Python deps file.
- **Owner:** Sprint 7 close hygiene.

### FU-7.1.b — `chosen == rejected` rows not filtered before DPO
- **Source:** US-7.1 panel (FLAG).
- Add filter in `_prepare_dpo_from_jsonl` and `prepare_dpo_from_db`.
- **Owner:** Sprint 8 fine-tune pipeline polish.

### FU-7.6.b — `hu_ml_fidelity_score_judgment` lacks `isfinite(nll)` guard
- **Source:** US-7.6 panel.
- **Owner:** US-7.6.1 implementer.

### FU-7.6.c — `g_nll_fn` mutable static lacks thread-safety
- **Source:** US-7.6 critic + panel.
- **Owner:** US-7.6.1 implementer.

### FU-7.4.a — `rank=0` falsy substitution trap
- **Source:** US-7.4 critic MED.
- `int(getattr(args, "rank", None) or 32)` silently converts explicit 0 to 32.
- **Fix sketch:** use `is not None` check rather than falsy `or`.
- **Owner:** Sprint 8 polish.

### FU-7.4.b — `no_version` path missing mkdir before write
- **Source:** US-7.4 critic MED.
- **Owner:** Sprint 8 polish.

## P2 — informational

### FU-7.3.a — vacuous absence-tests in `test_provider_all.c`
- **Source:** US-7.3 critic (MED).
- **Owner:** maintenance pass.

### FU-7.3.b — whitespace-only changes to protected test (AC-7.3.5)
- **Source:** US-7.3 critic (MED).
- **Owner:** optional revert.

### FU-7.6.d — clang-format churn in `src/ml/cli.c`
- **Source:** US-7.6 panel.
- ~500 lines of cli.c are reformat-only. Same issue surfaced in US-7.2 and US-7.7.
- **Owner:** post-sprint hygiene; split reformat commits.

### FU-7.7.d — Default `best_of_n=0` vs `1` round-trip asymmetry
- **Source:** US-7.7 critic MED.
- User-explicit `0` is silently upgraded to `1` after save/load.
- **Owner:** Sprint 8 config polish.

### FU-7.7.e — N=2 cost-cap edge case untested
- **Source:** US-7.7 critic MED.
- Add test where only the last call exceeds the cap.
- **Owner:** Sprint 8 test hardening.

### FU-7.7.f — Fidelity-scorer-coupled test fragility
- **Source:** US-7.7 critic MED.
- `test_best_of_4_returns_highest_score` relies on internal scorer ranking; should inject mock scorer.
- **Owner:** Sprint 8 test isolation.

### FU-7.10.a — Init #06 vtable divergence documentation
- **Source:** US-7.10 design (Open Q2).
- **Owner:** docs follow-up after US-7.10 cherry-pick.

### FU-7.9.e — `find_last_quoted` early-return logic flaw
- **Source:** US-7.9 critic MED-1.
- Inner `return false` bails outer loop prematurely on compound quoted rules.
- **Owner:** Sprint 8 polish.

### FU-7.9.f — Dead `warned_drop` variable in style_critique.c
- **Source:** US-7.9 critic LOW-1.
- Design specified one-shot warning for rules-over-32; variable is dead.
- **Owner:** Sprint 8 polish.

---

**Status as of Wave 1 partial close:**
- P0: FU-7.7.a (lying comment) should be fixed inline before sprint close.
- P1/P2: deferred to Sprint 8 or maintenance.
- Sprint-auditor will read this file and flag any P0 silently deferred.
