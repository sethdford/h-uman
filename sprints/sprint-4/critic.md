# Critic findings — Sprint 4 Validator Chain Hardening Follow-up

Reviewed commits: 3429d068 (US-9), 3eab2730 (US-10), 52e3c611 (housekeeping),
bc32a082 (US-5), 8a9f1e66 (US-4), ee448c84 (US-6).

---

## CRITICAL (0)

None. The `chain_owned` borrow flag is correctly set and read at all three
agent call sites. `hu_output_validator_chain_destroy` is never called on a
borrowed persona pointer. No Jordan-leak memory-safety hole introduced.

---

## HIGH (1)

- `src/channels/format.c:595,647,710`, `src/channels/imessage.c:914`,
  `src/gateway/openai_compat.c:627`, `src/daemon_cron.c:294` — **11 of 16
  `hu_output_validator_chain_execute` call sites emit zero telemetry.**
  US-5 wired 3 agent sites + 1 daemon.c site (which passes `NULL` observer,
  making it a no-op). The channel-layer and gateway sites, which are the
  paths most visible to end users and most relevant for operator monitoring,
  are completely dark. AC-5.2 says "emits … for every REJECT or REWRITE
  outcome" with no scope carve-out for channel sites. The implementer's
  commit message acknowledges only 3 primary sites. **Fix:** wire
  `hu_observer_emit_validator_decision` at the remaining 11 sites, or add
  an explicit scope carve-out to AC-5.2 and close it as a known gap.

---

## MED (2)

- `scripts/check-test-references.sh` (US-10) — **no-match silently passes.**
  When `find_production_module` returns empty (no matching `src/**/<module>.c`
  found), the script prints a warning to stderr and continues — exit 0. A test
  file with a deliberate or typo'd name (e.g. `tests/test_validator_util.c`
  where no `src/validator_util.c` exists) is silently let through. The AC
  requires exit 1 on missing reference; the escape hatch is `@covers-none`,
  not "module not found". **Fix:** treat unresolvable module as a failure
  unless `@covers-none` is present, or promote the warning to an error.

- `src/observability/validator_telemetry.c:47` — `bytes_stripped` is
  computed as `input_len - cr->final_text_len` only when `final_text_len <
  input_len`. If a REWRITE validator expands the content (unlikely but
  possible via padding/escaping), `bytes_stripped` silently reports 0 rather
  than a negative delta. AC-5.4 only asserts `bytes_stripped > 0`; a
  content-expanding REWRITE would make that assertion pass for the wrong
  reason. Low blast radius but the comment says "bytes stripped" while the
  semantics are actually "bytes reduced". **Fix:** document the floor-at-zero
  behavior in the field comment in `observer.h`.

---

## LOW (1)

- `include/human/persona.h` — the new `outbound_chain` field comment says
  "NULL only when persona has zero validator rules or _load_json() failed
  before completion." The first clause is wrong: `hu_validators_build_default_outbound_chain`
  always builds a chain (the default ruleset is unconditional); the chain
  will only be NULL on OOM or if `_load_json` returned early. The misleading
  comment could cause a future reader to add a spurious NULL guard and
  suppress the fallback inline-build path. **Fix:** correct the comment to
  "NULL only on allocation failure during persona load."

---

## STRENGTHS

- US-4 borrow/own flag pattern is clean and consistent across all three call
  sites; deinit destroy path is correct; ASan coverage is real.
- US-6 AC-6.3 deletion proof is explicitly documented in the test file with
  mechanism (call_count==2 → 1, F1 fragment appears) — genuinely deletion-sensitive.
- US-9 annotation text is accurate and cross-references the audit note;
  `strip_channel_tags` scope in production is now confirmed to only 2 sites
  in `daemon.c` (both annotated); no unannotated survivors found elsewhere.
- US-10 longest-prefix-match module resolution is a thoughtful design that
  handles `test_daemon_e2e_validator.c → daemon.c` correctly.

---

## Per-story verdict

| Story | Verdict |
|-------|---------|
| US-4 | APPROVED |
| US-5 | NEEDS_FIXES — 11 call sites dark; AC-5.2 not satisfied |
| US-6 | APPROVED |
| US-9 | APPROVED |
| US-10 | NEEDS_FIXES — unresolvable module silently exits 0 |

---

RESULT_critic=HAS_FINDINGS_0_1
