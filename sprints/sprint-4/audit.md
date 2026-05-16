# Sprint 4 Audit — Validator Chain Hardening Follow-up

Independent re-derivation against `sprints/sprint-4/stories.md`. Commits `3429d068..72b79214` (8 total). Base: `v-sprint-3-close`.

## Stories audited

| Story | AC count | Delivered | Partial | Missed | Drift |
|---|---|---|---|---|---|
| US-6 | 4 | 4 | 0 | 0 | 0 |
| US-4 | 5 | 5 | 0 | 0 | 0 |
| US-5 | 5 | 4 | 1 | 0 | 0 |
| US-9 | 3 | 3 | 0 | 0 | 0 |
| US-10 | 4 | 3 | 1 | 0 | 0 |

## AC-by-AC findings

### US-6 (E2E daemon validator test)
- AC-6.1: DELIVERED — `tests/test_daemon_e2e_validator.c` mock provider returns `JORDAN_LEAK_F1`; drives `hu_agent_turn` (line 260). No inline chain reconstruction.
- AC-6.2: DELIVERED — asserts response does NOT contain `JORDAN_LEAK_F1`.
- AC-6.3: DELIVERED — header comment (lines 13–16) documents the deletion experiment at `agent_turn.c:5605`. Verified that line IS the `hu_output_validator_chain_execute` call.
- AC-6.4: DELIVERED — suite registered in `tests/test_main.c`.

### US-4 (chain caching on persona)
- AC-4.1: DELIVERED — `include/human/persona.h:455` adds `outbound_chain`; populated at `persona.c:2328`.
- AC-4.2: DELIVERED — built once at end of `hu_persona_load_json`; pointer stability test in `tests/test_validator_chain_cache.c`.
- AC-4.3: DELIVERED with design carve-out — grep returns 0 hits in AC-named scope (agent_turn.c, agent_stream.c, the 5 daemon paths in the spirit of the AC). agent_turn.c:5591 and agent_stream.c:1428/2160 read cached chain with inline fallback only when persona unavailable. Remaining inline-build sites in daemon.c, daemon_cron.c, openai_compat.c, channels/format.c, channels/imessage.c all pass `NULL` persona — no persona context exists at those call sites. Design doc US-4.md §approach pre-declared this carve-out; not silent drift.
- AC-4.4: DELIVERED — `persona.c:193` destroys chain in `hu_persona_deinit`; ASan leak cycle covered.
- AC-4.5: DELIVERED per reported full-suite PASS.

### US-5 (validator decision telemetry)
- AC-5.1: DELIVERED — `HU_OBSERVER_EVENT_VALIDATOR_DECISION` declared in `observer.h:33` with all required payload fields.
- AC-5.2: **PARTIAL** — AC reads "every REJECT or REWRITE outcome." Post critic HIGH fix `088cd019`, emit coverage = 10 sites. But `daemon.c` still executes chain WITHOUT emitting at **lines 1077, 1738, 9301, 10659, 11699** (5 sites: scheduled-flush, proactive-checkin, three other daemon outbound paths). Critic-fix commit explicitly skipped daemon.c. Operators monitoring these flows will see zero REJECT/REWRITE telemetry. No carve-out comment in source.
- AC-5.3: DELIVERED — `tests/test_validator_telemetry.c` asserts REJECT decision, validator_name, response_len.
- AC-5.4: DELIVERED — REWRITE test asserts `bytes_stripped > 0`.
- AC-5.5: DELIVERED — emit helper accepts NULL observer; daemon.c:2105 passes NULL.

### US-9 (Pattern C annotation + Sprint 3 DoD update)
- AC-9.1: DELIVERED — option (b). Block comments at `daemon.c:2127` and `daemon.c:2142` cite Sprint 3 US-2 / Sprint 4 US-9 and forbid removal without safety-net restoration.
- AC-9.2: DELIVERED — `sprints/sprint-3/stories.md:51` rewritten to describe the post-implementation invariant; verified via `git diff`.
- AC-9.3: PASS reported (annotation-only change, trust signal high).

### US-10 (hookify rule + pre-commit script)
- AC-10.1: DELIVERED — `.claude/rules/test-references-production-symbol.md` (80 lines).
- AC-10.2: DELIVERED — `scripts/check-test-references.sh` executable, has `--help`, exits non-zero on missing reference.
- AC-10.3: DELIVERED — wired at `.githooks/pre-commit:90-96`.
- AC-10.4: **PARTIAL** — `tests/test_pattern_c_paths.c` exits 0 (correct). Negative case: bundled fixture `tests/fixtures/check-test-refs/bad.c` is rejected by the path filter (`tests/test_*.c`) at line 128 and never content-scanned — it exits 0, not 1. The MED-fix content-scan fallback at line 151 only fires for `tests/test_*.c` files. Rule itself is correct; the negative-case proof is not reproducible from committed fixtures.

## Scope creep
None. All 8 commits trace to a named story or critic finding.

## DoD violations
- US-5 AC-5.2: 5 daemon chain executions emit no telemetry. Either close gap or amend AC with explicit in-source exception list.
- US-10 AC-10.4: negative case unprovable with shipped fixtures.

## Adversarial findings
- `daemon.c:2105` emits with `obs=NULL, vctx=NULL` — operators get a decision count without channel/persona context. Not an AC violation (AC-5.1 permits NULL fields), but reduces telemetry utility on the bus-broadcast path.
- The critic HIGH finding cited "5 of 16 emit sites" pre-fix. Post-fix is 10 of 15 chain executions. Daemon.c gap is real and recurring.

## Verdict

Sprint is shippable with two follow-ups for Sprint 5:
1. Wire `hu_observer_emit_validator_decision` at daemon.c sites 1077/1738/9301/10659/11699 OR add per-site carve-out comments.
2. Rename `check-test-refs/bad.c` to a `tests/test_*.c` path so the negative case actually exits 1.

## AC-5.2 follow-up (Sprint 5 US-11)

The five `hu_output_validator_chain_execute` call sites in `src/daemon.c` that
lack observer telemetry have been annotated with:

```
// telemetry: observer not in scope (architectural limit)
```

Site details (line numbers as of Sprint 5 US-11 commit):

| Line | Function | Code path |
|------|----------|-----------|
| 1077 | `hu_service_run_proactive_checkins` | Scheduled-flush outbound validation |
| 1739 | `hu_service_run_proactive_checkins` | Proactive check-in response validation |
| 9304 | `hu_service_run` | Burst-message outbound validation loop |
| 10664 | `hu_service_run` | Daemon outbound path (secondary response) |
| 11706 | `hu_service_run` | Deferred-task response validation |

Plumbing a `hu_observer_t *` into these sites is out of scope for Sprint 5
because it would require threading the observer pointer through multiple layers
of the daemon service loop — a structural refactor that crosses several
subsystems. These paths still run the full validator chain; REJECT and REWRITE
outcomes take effect. Only the `HU_OBSERVER_EVENT_VALIDATOR_DECISION` telemetry
event is not emitted. A future architectural sprint should either add a global
observer bus that these paths can reach without explicit pointer threading, or
plumb the observer explicitly as part of a broader daemon refactor.

RESULT_sprint-auditor=PASS_WITH_NOTES
