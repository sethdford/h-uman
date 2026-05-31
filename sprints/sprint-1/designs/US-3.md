# Design for US-3: Promote Salience/Arbitration from SHADOW toward Live

**Status:** Ready for implementation  
**Author:** Tech Lead  
**Date:** 2026-05-30  
**Sprint:** Sprint 1 — Activate and MEASURE h-uman's humanness moat

---

## Approach

The salience/arbitration layer currently runs in **SHADOW mode only** (`HU_SALIENCE_SHADOW` env flag): it ranks humanness directives but logs the ranking result without modifying the assembled prompt. US-3 introduces a **three-state trichotomy** (OFF / SHADOW / LIVE) so the ranking can transition to **live mode** where it actually suppresses lower-ranked directives before they reach the model.

The design is minimal and reversible:

1. **Trichotomy state machine**: replace the boolean `sal_shadow` check with a three-way branch (OFF, SHADOW, LIVE), each with distinct behavior:
   - **OFF**: skip all salience work entirely (default, no perf cost)
   - **SHADOW**: rank directives, log kept-vs-suppressed (current behavior, preserved)
   - **LIVE**: rank directives, **filter the assembled buffer** to keep only selected directives, emit the filtered prompt

2. **Config-driven flag**: add `HU_SALIENCE_LIVE` env flag (parsed via existing `getenv` pattern, no config-file changes needed for MVP) so operator can test the transition without rebuilding. When LIVE is set, SHADOW is implicitly off.

3. **Calibration harness**: a shell script (`scripts/salience-calibration.sh`) that orchestrates a live-mode test run, extracts suppressed-vs-kept counts from logs, and outputs JSON evidence for AC-3.3. This proves the mechanism works before blind A/B gates the final decision.

4. **Never-suppress floor**: preserve the `hu_salience_source_is_required()` predicate — safety, crisis, grief, direct-question directives always pass through regardless of score. This is the hard invariant across all three states.

5. **Gate comment**: add a future-facing comment at the decision point that live activation requires Story D (blind A/B) measurement to pass before the flag is flipped to default-ON.

**Why this design and not alternatives:**

- ❌ **Alternative**: flip SHADOW directly to LIVE (remove the trichotomy). Too dangerous — we'd lose the logging-only mode that let us observe behavior cost-free. We need SHADOW as a permanent debug/calibration lever.
- ❌ **Alternative**: add the flag to `config.json` schema. Simpler for operators, but adds config parsing changes to the risk surface. ENV var is already wired and proven in the codebase (same `getenv` pattern as SHADOW).
- ❌ **Alternative**: filter directives in the arbitrator itself. The arbitrator is a reusable engine; salience's LIVE filtering is domain-specific. Belongs in agent_turn.c where we know the assembled-buffer shape.

---

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `src/agent/agent_turn.c` | Replace `sal_shadow` bool with trichotomy; add LIVE filtering logic at line ~2838 | +35 |
| `src/agent/salience.c` | (no changes needed — salience.h interface unchanged) | 0 |
| `tests/test_salience.c` | Add 3 tests: OFF state no-op, SHADOW preserved, LIVE filters | +45 |
| `scripts/salience-calibration.sh` | New shell script: runs daemon in LIVE mode, extracts metrics, outputs JSON | +100 |
| `.gitignore` | (optional) add `sprints/sprint-1/evidence/US-3/` to prevent accidental log commits | +2 |

---

## Implementation steps (for the implementer)

### Phase 1: Trichotomy state machine (reversible skeleton)

1. **Replace the boolean flag** at `agent_turn.c:2659`:
   ```c
   // Current:
   bool sal_shadow = getenv("HU_SALIENCE_SHADOW") != NULL;
   
   // New:
   const char *sal_mode_str = getenv("HU_SALIENCE_LIVE") != NULL ? "live" : 
                               (getenv("HU_SALIENCE_SHADOW") != NULL ? "shadow" : "off");
   enum hu_salience_mode {
       HU_SALIENCE_OFF = 0,
       HU_SALIENCE_SHADOW = 1,
       HU_SALIENCE_LIVE = 2
   } sal_mode;
   sal_mode = (strcmp(sal_mode_str, "live") == 0) ? HU_SALIENCE_LIVE :
              (strcmp(sal_mode_str, "shadow") == 0) ? HU_SALIENCE_SHADOW : HU_SALIENCE_OFF;
   ```

2. **Update all `sal_shadow` checks** to branch on the enum:
   - `if (sal_shadow && ...)` → `if ((sal_mode == HU_SALIENCE_SHADOW || sal_mode == HU_SALIENCE_LIVE) && ...)`
   - Anywhere we build a candidate for ranking, guard with the inclusive check above.

3. **Add LIVE filtering** at `agent_turn.c:2838` (post-ranking, pre-prompt-assembly):
   ```c
   // Current code (after ranking):
   if (sal_shadow && sal_count > 0) {
       hu_salience_profile_t sal_prof;
       // ... ranking happens, fills sal_res with kept/suppressed ...
   }
   
   // New code: after ranking, if LIVE mode, FILTER the humanness_ctx buffer
   if (sal_mode == HU_SALIENCE_LIVE && sal_count > 0 && sal_res is populated) {
       // sal_res.kept[] holds the indices of directives that passed
       // Rebuild humanness_ctx to include ONLY those directives
       // (Reconstruct from the kept directive set, respecting buffer layout)
   }
   
   // Then log if in shadow OR live (for observability)
   if ((sal_mode == HU_SALIENCE_SHADOW || sal_mode == HU_SALIENCE_LIVE) && sal_count > 0) {
       hu_log_info("agent_turn", NULL, "salience(%s): ...", sal_mode == HU_SALIENCE_LIVE ? "live" : "shadow");
   }
   ```

4. **Add gate comment** at the decision point (line ~2655):
   ```c
   /* Salience P4: when HU_SALIENCE_LIVE is set, rank directives and filter 
    * the assembled prompt to keep only the most salient few. This ensures
    * the persona reads as one coherent voice, not a committee. Default OFF.
    *
    * GATE: Live activation requires Story D (blind A/B measurement) to show
    * that the filtered persona is judged superior by real humans before
    * flipping HU_SALIENCE_LIVE to default-ON in production. */
   ```

### Phase 2: Tests for the trichotomy

5. **Add to `tests/test_salience.c`**:
   - `test_salience_live_filters_candidates`: mock env HU_SALIENCE_LIVE=1, verify that after ranking, the humanness buffer contains only the selected directives (not all built ones).
   - `test_salience_shadow_preserves_all`: mock env HU_SALIENCE_SHADOW=1 (no LIVE), verify all built directives appear in prompt (current behavior).
   - `test_salience_off_skips_ranking`: no env set, verify no salience work happens, buffer unchanged.

6. **Run test suite**: `./build/human_tests --suite=salience` — all new tests should pass, existing tests unchanged.

### Phase 3: Calibration harness

7. **Create `scripts/salience-calibration.sh`**:
   - Accept `--contact-id` and `--count=N` (or use fixtures)
   - Set `HU_SALIENCE_LIVE=true` in daemon config
   - Run daemon for ≥10 turns, capturing logs
   - Extract per-turn lines like `salience(live): kept 2/9 [shared_reference,emotional_checkin] suppressed 7 ...`
   - Aggregate per-contact: count total suppressed directives, total kept, ratio
   - Output to `sprints/sprint-1/evidence/US-3/calibration-metrics.json`:
     ```json
     {
       "contact_id": "Mom",
       "total_turns": 10,
       "suppressed_count": 45,
       "kept_count": 18,
       "suppression_rate": 0.714,
       "timestamp": "2026-05-30T..."
     }
     ```

8. **Validate script output**: run `jq .` on the JSON to confirm structure.

### Phase 4: Verify no behavior change when OFF

9. **Run full test suite three times** with environment variations:
   ```bash
   HU_SALIENCE_LIVE=off ./build/human_tests > /tmp/off.txt 2>&1
   unset HU_SALIENCE_LIVE && unset HU_SALIENCE_SHADOW && ./build/human_tests > /tmp/default.txt 2>&1
   HU_SALIENCE_SHADOW=1 ./build/human_tests > /tmp/shadow.txt 2>&1
   ```

10. **Compare results**: `diff /tmp/off.txt /tmp/default.txt` should be empty (no behavior change in OFF mode).

---

## Risks

### Risk 1: Buffer reconstruction complexity in LIVE mode (MEDIUM / MEDIUM)

**What could go wrong:** The humanness buffer (agent_turn.c ~line 2650) is assembled by concatenating directive strings from multiple sources (shared_reference, curiosity, absence, etc.). When we filter in LIVE mode, we must rebuild it containing only the selected directives. If the reconstruction logic is wrong, we could:
- Duplicate directives (same content appears twice)
- Lose newlines/spacing, creating malformed prompt
- Include an unselected directive by mistake

**Probability:** MEDIUM — the buffer assembly is intricate (8-10 sources, dynamic offsets), and off-by-one errors in reconstruction are common.

**Impact:** MEDIUM — wrong buffer crashes the turn or sends a malformed prompt to the model. Either way, the feature is unusable and requires fixing before test-suite passes.

**Mitigation:** 
- Before LIVE filtering, **snapshot the original humanness_ctx and all built directives to test fixtures** so the reconstruction can be unit-tested in isolation.
- **Add an invariant check** after filtering: verify the reconstructed buffer contains only directive sources that are in the kept set (`sal_res.kept`), and no unknown bytes.
- **Add a sanity test** that builds a buffer, filters it, and asserts buffer length <= original (never grows).

### Risk 2: Never-suppress floor not enforced in LIVE (MEDIUM / LARGE)

**What could go wrong:** The `hu_salience_source_is_required()` predicate identifies directives that should never be suppressed (safety, crisis, grief, direct-question). If the LIVE filtering code doesn't respect this floor, we could suppress a safety directive and send a dangerously incoherent reply.

**Probability:** MEDIUM — the required check is easy to forget; it's orthogonal to the ranking logic.

**Impact:** LARGE — silent suppression of safety directives is a hard failure. A persona that ignores "don't say X to them" is unsafe.

**Mitigation:**
- **Hard contract at the call site**: before filtering, assert that every kept directive in `sal_res.kept` either (a) has `required=true` in the original candidate, OR (b) was selected by the arbitrator's scoring. Any required directive must appear in kept; if not, REJECT the entire filtering and log loudly (`HU_ERR_VIOLATION`).
- **Test pinning**: `test_salience_live_never_suppresses_safety` — build candidates including a safety directive, run arbitrator, assert it appears in kept REGARDLESS of its score.

### Risk 3: Collision on agent_turn.c edit (HIGH / MEDIUM)

**What could go wrong:** Per the story preamble, US-1 and US-2 also edit `agent_turn.c`. If all three work in parallel, merge conflicts are likely. The salience changes are around line 2655–2853; US-1 adds a comment at line 1471 (far from here); US-2 may wire contextual bandit somewhere unknown.

**Probability:** HIGH — the story explicitly flags this as a collision risk.

**Impact:** MEDIUM — merge conflicts are resolvable, but they block testing. A parallel agent working on US-2 could land code that interferes with the salience trichotomy logic.

**Mitigation:**
- **Sequence: US-1 → US-3 → US-2** (or equivalent, but serial). US-1 is a documentation-only gate comment (line 1471); US-3 is the core salience work (lines 2655–2853); US-2 is a contextual-bandit audit that will add calls somewhere in the turn loop if it wires anything.
- **Use atomic git commits** per story so merges can be isolated: US-1 lands, merged to main; US-3 lands on main, merged; US-2 lands on top.
- If US-2 discovers contextual_bandit needs wiring, coordinate the call sites — the lead should review the US-2 branch and flag any new agent_turn.c changes before merging.

### Risk 4: Calibration harness captures wrong signal (LOW / MEDIUM)

**What could go wrong:** The calibration script extracts metrics from daemon logs. If the log lines have a different format than expected, the script's regex may fail and produce empty JSON or wrong counts.

**Probability:** LOW — the log format is fixed (defined in agent_turn.c:2846), and we control both the emitter and the parser.

**Impact:** MEDIUM — bad metrics mean we can't evidence AC-3.3, and the harness must be debugged before closing the story.

**Mitigation:**
- **Pin the log format** in a comment at agent_turn.c:2846 so the script can find it:
  ```c
  hu_log_info("agent_turn", NULL, "salience(%s): kept %zu/%zu [...] suppressed %zu [...]",
              sal_mode == HU_SALIENCE_LIVE ? "live" : "shadow", ...);
  ```
- **Test the script against a sample log** before declaring it done. The test run (AC-3.3) will expose mismatches immediately.

### Risk 5: Performance regression if LIVE filtering is slow (LOW / SMALL)

**What could go wrong:** Reconstructing the humanness buffer per turn has token cost. If it's slow (e.g., repeated string allocations, inefficient copying), it could add latency.

**Probability:** LOW — the humanness buffer is ~4KB (agent_turn.c:2652 `hum_buf[4096]`), and filtering happens once per turn before the LLM call (which is orders of magnitude slower).

**Impact:** SMALL — even 10ms of overhead is noise compared to a 500ms+ LLM round-trip.

**Mitigation:**
- **No special action needed** unless profiling shows otherwise. The filtering happens in-memory on a small buffer; allocation cost is negligible.

### Risk 6: Observability: LIVE mode filtering is silent in production logs (MEDIUM / SMALL)

**What could go wrong:** If an operator runs a daemon in LIVE mode and the feature silently suppresses directives, the operator has no signal that the behavior is different from SHADOW or OFF. If something seems wrong, they won't know to check the log for salience info.

**Probability:** MEDIUM — operators may not read the detailed log lines; they only see high-level summaries.

**Impact:** SMALL — we add a gate comment and the log line is explicit. An operator who debugs will find it.

**Mitigation:**
- **Log once at daemon startup** when LIVE is detected:
  ```c
  hu_log_warn("config", NULL, "SALIENCE LIVE MODE ACTIVE: "
              "directives will be filtered by rank; only the top N will be included. "
              "Set HU_SALIENCE_LIVE=off to disable. See logs for per-turn details.");
  ```
  This gives operators a banner they'll notice.
- The per-turn log line (`salience(live): kept 2/9 ...`) provides fine detail for diagnosis.

---

## Test strategy

### Unit tests (new, in `tests/test_salience.c`)

- **`test_salience_off_skips_ranking`**: no env flag, verify no salience candidates are built. Assertions: `sal_count == 0` or ranking is skipped.
- **`test_salience_shadow_logs_only`**: set `HU_SALIENCE_SHADOW=1`, verify candidates are built and ranked, but humanness buffer is unchanged. Assertion: buffer before ranking == buffer after ranking.
- **`test_salience_live_filters_buffer`**: set `HU_SALIENCE_LIVE=1`, build 9 candidates (3 required, 6 ranked), run ranking, verify humanness buffer contains only the 2 highest-scoring + 3 required (5 total). Assertion: buffer length < pre-filter length, directives in buffer are subset of original.
- **`test_salience_live_never_suppresses_safety`**: set LIVE, build 5 candidates including one with `required=true`, mock arbitrator to rank it lowest, verify it appears in kept anyway. Assertion: required directive cannot be filtered.
- **`test_salience_trichotomy_mutual_exclusion`**: set both `HU_SALIENCE_SHADOW=1` and `HU_SALIENCE_LIVE=1`, verify LIVE takes precedence (mode is LIVE, not SHADOW). Assertion: log shows "salience(live)", not "salience(shadow)".

### Integration test (in `tests/test_agent_turn.c` or new `tests/test_agent_turn_salience_e2e.c`)

- **`test_agent_turn_salience_live_end_to_end`**: construct a minimal agent, set LIVE mode, run a turn with 8+ directives, verify the assembled prompt (sent to LLM) contains fewer directives than logged candidates. This proves the filtering actually reaches the model.

### Shell script validation (AC-3.3)

- Run `scripts/salience-calibration.sh --count=5 --contact-id=test_fixture`
- Verify output is valid JSON: `jq . sprints/sprint-1/evidence/US-3/calibration-metrics.json`
- Assertions: `suppressed_count > 0`, `kept_count > 0`, `suppression_rate` in [0, 1], timestamp is recent.

### Regression suite

- Run full test suite with each mode (`OFF`, `SHADOW`, `LIVE`)
- Verify no existing test failures introduced (AC-3.4: identical results for OFF and default).

---

## Acceptance criteria mapping

| AC | Test/Evidence | Owner |
|---|---|---|
| AC-3.1 | `grep -n "HU_SALIENCE_SHADOW" src/agent/agent_turn.c` shows at least 1 match. Test suite has salience tests. | Implementer runs and captures in commit msg |
| AC-3.2 | Code recognizes `HU_SALIENCE_LIVE` flag; three states (OFF/SHADOW/LIVE) are mutually exclusive; default OFF. Test: `test_salience_trichotomy_mutual_exclusion` + `test_salience_off_skips_ranking`. | `test_salience.c` |
| AC-3.3 | Script `scripts/salience-calibration.sh` runs; output at `sprints/sprint-1/evidence/US-3/calibration-metrics.json` is valid JSON with keys `contact_id`, `suppressed_count`, `kept_count`. `jq .` succeeds. | Harness validation step |
| AC-3.4 | Three runs of full suite (LIVE=off, default, LIVE=shadow) produce identical test counts + pass/fail. Captured in evidence/US-3/regression-results.txt | Implementer runs, diff tool output |
| AC-3.5 | Comment added at agent_turn.c ~line 2655 stating "Salience LIVE activation gated on Story D". | Code review |
| AC-3.6 | `./build/human_tests \| grep Results:` shows 0 failures; ASan reports zero errors. | /verify agent |

---

## Shared-file collision mitigation

Per the story preamble, this is the top risk. Sequence:

1. **US-1 (landing first)**: adds gate comment at agent_turn.c:1471 (far from salience code).
2. **US-3 (this design)**: edits agent_turn.c:2655–2853 (salience block).
3. **US-2 (landing last)**: audits contextual_bandit; if it wires, agent_turn.c changes are isolated to specific call-sites (unlikely to overlap with salience).

If US-2 discovers contextual_bandit is dead and needs wiring, it will call new functions elsewhere in the turn loop — no collision with the salience trichotomy. If US-2 discovers contextual_bandit IS wired, no code changes needed.

**Action**: lead coordinates merge order in GitHub, merges US-1 then US-3 before US-2.

---

## Why this design is cheap and low-risk

1. **Minimal code change**: ~35 lines in agent_turn.c (replace bool check with enum branch + add LIVE filtering).
2. **No config schema change**: uses existing `getenv` pattern, same as HU_SALIENCE_SHADOW.
3. **Backward compatible**: OFF mode preserves all existing behavior (tests pass unchanged).
4. **Reversible**: can flip LIVE off at runtime without rebuild.
5. **Already-written subsystem**: reuses hu_salience_rank + hu_arbitrator_select (no new ranking logic needed).
6. **Simple contract**: never-suppress floor is already enforced by `hu_salience_source_is_required()`.

---

RESULT_tech-lead=DESIGN_READY
