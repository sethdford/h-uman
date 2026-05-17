# Sprint 7 — Follow-up Tasks

Findings the gates surfaced that are not in-scope for Sprint 7 but should
land in Sprint 8 (or earlier if a P0 is present).

## P0 — must address before Sprint 7 closes

(none outstanding — FU-7.7.a was fixed inline at `4a460b1d`)

## P1 — Sprint 8

### FU-7.8.a — Adapter-id basename collision in MoLoRA router
- **Source:** US-7.8 critic HIGH-2.
- `src/agent/agent_turn.c:439-445` derives adapter-id via `strrchr(path,'/')` basename. Two channel adapter paths sharing the same filename (`telegram/lora.bin` + `slack/lora.bin`) both produce `base="lora.bin"`; the idempotency check `strcmp(cur_id, base) != 0` permanently suppresses subsequent channel swaps after the first load.
- **Fix sketch:** derive adapter-id from full path (or hash of it), or store full path as the active adapter id in `llamacpp_load_adapter`.
- **Risk if deferred:** silent breakage when users have same-named adapter files in different channel directories. The conventional layout `~/.human/adapters/<persona>-<channel>.lora` avoids this; the bug only fires on non-conventional layouts. Low frequency, real impact.

### FU-7.10.a — `human ml rl-train --algorithm simpo` returns NOT_SUPPORTED in production
- **Source:** US-7.10 critic HIGH-1.
- `compute_loss` is wired and tested (golden test passes); `train_step` is the stub. Production users get exit-2 when invoking SimPO training even though the help text advertises it.
- **Fix sketch:** either (a) gate behind `[experimental]` warning that still exits 0, (b) document `--logprobs-only` mode via `compute_loss`, or (c) wire the forward-pass.
- **Risk if deferred:** confusing operator UX. Workaround documented in CLI help meantime.

### FU-7.7.b — Agent-level telemetry counters absent
- **Source:** US-7.7 critic HIGH-2.
- 4 `uint64_t` counters on `hu_agent_t` (`best_of_n_invocations`, `best_of_n_cost_cap_hits`, `best_of_n_picks_above_first`, `best_of_n_total_candidates`) not added. `stats_out` is computed and lost.
- **Fix sketch:** add the 4 counters; fold from `bcfg.stats_out` after best-of-N call returns; add `hu_agent_best_of_n_telemetry()` accessor.

### FU-7.7.c — Score-bounds telemetry edge cases
- **Source:** US-7.7 critic HIGH-3.
- `min_score=1.0f / max_score=0.0f` initializers misleading; `picked_score = scores[0]` may be `-1.0f` sentinel.
- **Fix sketch:** add `HU_ASSERT(s >= 0.0f && s <= 1.0f)` or clamp in loop body. Move min/max init into first-scored-entry branch.

### FU-7.9.a — `hu_style_critique_run` silent skip on regen failure
- **Source:** US-7.9 critic HIGH-1.
- Returns HU_OK silently when provider regen fails — caller can't distinguish "no violation" from "violation+regen errored."
- **Fix sketch:** emit `style_rule_violation_unresolved` log before early-return on regen failure; OR return `HU_ERR_RETRY_FAILED`.

### FU-7.9.b — Emoji alias coverage gap (BMP block)
- **Source:** US-7.9 critic HIGH-2.
- Alias matches only `\xF0\x9F` (plane-1 4-byte UTF-8). Misses U+2600-U+27BF (☀️ ✅ ✈️).
- **Fix sketch:** widen alias table to cover BMP block; or add Unicode-aware downstream pass per header note.

### FU-7.9.c — `test_critique_disabled_short_circuits` is vacuous
- **Source:** US-7.9 critic HIGH-3.
- Test only resets counters and asserts zero — trivially true.
- **Fix sketch:** replace with actual `hu_agent_turn` invocation, `style_rules_enabled=false`, assert `check_invocations==0`.

### FU-7.9.d — Prompt-injection vector via user-controlled rule text
- **Source:** US-7.9 critic MED-2 (but real security finding).
- `violated_rule` concatenated verbatim into regen system prompt.
- **Fix sketch:** strip/escape `\n`, `\r`, chars > 0x7E; truncate at 128 bytes.

### FU-7.6.a — `check-lora-ab.sh --judgment` silent-pass when STATUS empty
- **Source:** US-7.6 critic HIGH.
- When CLI produces no JSON, `STATUS` is empty, script falls through to `exit 0`.
- **Fix sketch:** when STATUS is empty, `exit 1` or emit SKIP on stdout.

### FU-7.5.a — World_model_bridge.c reformat churn
- **Source:** US-7.5 panel FLAG (Regression).
- ~250 lines reformat mixed with feature. Violates "one concern per change."
- **Fix sketch:** post-sprint hygiene; document the reformat as a separate commit if a future split is wanted.

### FU-7.5.b — D3 logging conflation distinct from exit-code path
- **Source:** US-7.5 critic CRITICAL (downgraded — verdict logic is safe).
- The fix re-dispatch DID partially address this (gate non-zero exit → FAILED; zero+non-PASS → SKIPPED_GATE_FAIL). Remaining: verify `lora_retrain_failed` event includes `step` discriminator (gate vs probe vs finetune) for operator clarity.

### FU-7.5.c — `static hu_lora_retrain_ctx_t` is function-local
- **Source:** US-7.5 critic HIGH-1.
- Should be file-scope so re-entry doesn't zero state.

### FU-7.5.d — Stale-PID TOCTOU race
- **Source:** US-7.5 critic HIGH-3. Narrow window.

### FU-7.5.e — `budget_ms` parameter accepted but ignored
- **Source:** US-7.5 critic MED-1.

### FU-7.5.f — JSON parser brittleness on schema growth
- **Source:** US-7.5 critic MED-2.

### FU-7.5.g — Enqueue inside `HU_ENABLE_LEARNING` block
- **Source:** US-7.5 critic HIGH-2.
- On minimal builds (without HU_ENABLE_ML), the cron never fires. Matches existing pattern but worth documenting as intentional.

### FU-7.1.a — `mlx-lm-lora` dep pin documented only in docstring
- **Source:** US-7.1 critic MED.
- Add to project's Python deps file.

### FU-7.1.b — `chosen == rejected` rows not filtered before DPO
- **Source:** US-7.1 panel FLAG.
- Add filter in `_prepare_dpo_from_jsonl` and `prepare_dpo_from_db`.

### FU-7.6.b — `hu_ml_fidelity_score_judgment` lacks `isfinite(nll)` guard
- **Source:** US-7.6 panel.

### FU-7.6.c — `g_nll_fn` mutable static lacks thread-safety
- **Source:** US-7.6 critic + panel.

### FU-7.4.a — `rank=0` falsy substitution trap
- **Source:** US-7.4 critic MED.

### FU-7.4.b — `no_version` path missing mkdir before write
- **Source:** US-7.4 critic MED.

### FU-7.10.b — Init #06 vtable divergence documentation
- **Source:** US-7.10 design Open Q2.
- Update `docs/plans/2026-05-11-init-06-simpo-orpo-grpo2.md` to document the v1 3-member vtable surface and the planned widening path.

### FU-7.8.b — MoLoRA config arena `a->free` pattern
- **Source:** US-7.8 critic HIGH-1.
- Pre-existing pattern in `src/config_parse.c`; affects molora entries on config reload. Same defect exists for `tools` strings.
- **Fix sketch:** rely on arena lifetime; null pointers without freeing OR switch to non-arena backing.

## P2 — informational

### FU-7.3.a — Vacuous absence-tests in `test_provider_all.c`
- **Source:** US-7.3 critic MED.

### FU-7.3.b — Whitespace-only changes to protected test (AC-7.3.5)
- **Source:** US-7.3 critic MED.

### FU-7.6.d — clang-format churn in `src/ml/cli.c`
- **Source:** US-7.6 panel.

### FU-7.7.d — Default `best_of_n=0` vs `1` round-trip asymmetry
- **Source:** US-7.7 critic MED.

### FU-7.7.e — N=2 cost-cap edge case untested
- **Source:** US-7.7 critic MED.

### FU-7.7.f — Fidelity-scorer-coupled test fragility
- **Source:** US-7.7 critic MED.

### FU-7.9.e — `find_last_quoted` early-return logic flaw
- **Source:** US-7.9 critic MED-1.

### FU-7.9.f — Dead `warned_drop` variable
- **Source:** US-7.9 critic LOW-1.

### FU-7.8.c — Inline normalizer in parser duplicates the runtime normalizer
- **Source:** US-7.8 critic MED-1.

### FU-7.8.d — Interior-whitespace channel keys silently truncated
- **Source:** US-7.8 critic MED-1.

### FU-7.8.e — OFF-build symbol absence not asserted in CI
- **Source:** US-7.8 critic MED-2.

### FU-7.8.f — `check-molora-binary-budget.sh` uses different flags than production
- **Source:** US-7.8 critic MED-3.

### FU-7.8.g — ABI-split risk documentation in `hu_agent_t`
- **Source:** US-7.8 critic LOW-1.

### FU-7.10.c — `cli.c` reformat churn (also see US-7.6 / US-7.2 / US-7.7)
- **Source:** US-7.10 critic LOW.

### FU-7.10.d — DPO delegation argv convention untested
- **Source:** US-7.10 critic MED.

### FU-7.10.e — `hu_rl_trainer_type_name` switch lacks `default:` arm
- **Source:** US-7.10 critic MED.

### FU-7.10.f — Floor-test fixture uses positive logprob (intentional but misleading)
- **Source:** US-7.10 critic MED.

---

**Status:** Sprint 7 closes with 0 outstanding P0. P1 follow-ups are
backlog for Sprint 8. P2 are nice-to-haves. No findings were so severe
that they blocked a story's close — every story passed verifier + (panel
PASS or PASS_WITH_NOTES), with critic findings documented for follow-up.
