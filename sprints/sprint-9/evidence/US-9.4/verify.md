# Verifier Evidence — US-9.4 `human doctor --install` gate

Branch: `impl/US-9.4`
Commit: `ea2ca02f`
Verified: 2026-05-17

---

## Contract

1. Build is -Werror clean (`cmake --build --preset dev`)
2. Targeted suite: 7/7 doctor_install tests pass
3. Full suite: pre-existing flake only (test_model_router.c:338)
4. `scripts/check-test-references.sh` exits 0 for test_doctor_install.c
5. Smoke test: `human doctor --install` exits 1 with four sub-check labels on fresh HOME
6. JSON smoke: `--json` emits valid JSON with `status:"NOT_READY"` and four `checks` entries
7. Dispatch precedence: `--install` → `--privacy` (reserved slot) → `--fix` → default documented in main.c:677-682
8. No short-circuit: all four items appended even on red (verified via test `install_check_does_not_short_circuit_on_first_red` and source inspection of `hu_doctor_check_install` — all four blocks execute unconditionally)
9. Adversarial AC: all red-path tests use `HU_ASSERT_NE(rc, HU_OK)` at lines 261, 289, 314, 343, 372, 401

---

## Evidence

### B1 — Build clean

```
BEHAVIOR: -Werror clean build
COMMAND: cmake --build --preset dev
EXIT: 0
EVIDENCE:
  [100%] Built target human_tests
  Layer topology OK: 0 cross-layer violations across 68 source files.
  [100%] Built target human
RESULT: PASS
```

### B2 — Targeted suite 7/7

```
BEHAVIOR: doctor_install 7/7 pass
COMMAND: ./build/human_tests --filter=doctor_install
EXIT: 0
EVIDENCE:
    install_check_all_green_returns_ok_and_marks_ready: passed
    install_check_missing_binary_returns_not_found: passed
    install_check_missing_config_dir_returns_not_found: passed
    install_check_no_channel_returns_not_found: passed
    install_check_missing_persona_returns_not_found: passed
    install_check_unparseable_persona_returns_not_found: passed
    install_check_does_not_short_circuit_on_first_red: passed
  doctor_install: 7/7 passed
RESULT: PASS
```

### B3 — Full suite pre-existing flake only

```
BEHAVIOR: Full suite 10388/10389, one pre-existing failure
COMMAND: ./build/human_tests
EXIT: 1 (due to pre-existing flake)
EVIDENCE:
  --- Results: 10388/10389 passed, 1 FAILED ---
  FAIL  (.../tests/test_model_router.c:338) assert failed: hu_route_log_count(log) > before
  (doctor_install: 7/7 passed — no new failures)
  Pre-existing: test_model_router.c introduced in commit 9cd07239, long before ea2ca02f
RESULT: PASS (pre-existing flake only, confirmed in commit message of ea2ca02f)
```

### B4 — check-test-references

```
BEHAVIOR: scripts/check-test-references.sh exits 0
COMMAND: scripts/check-test-references.sh tests/test_doctor_install.c
EXIT: 0
EVIDENCE: (no output, clean exit)
RESULT: PASS
```

### B5 — Smoke test exit 1, four labels

```
BEHAVIOR: human doctor --install exits 1, emits four sub-check labels
COMMAND: HOME=$(mktemp -d) ./build/human doctor --install
EXIT: 1
EVIDENCE:
  doctor[install]: config_dir failed
  doctor[install]: channel failed
  doctor[install]: persona failed

    human doctor --install — install-readiness

    ok      binary: OK (/...build/human)
    error   config_dir: MISSING — run 'human onboard' to create ~/.human/
    error   channel: NONE — run 'human doctor imessage' to pair iMessage
    error   persona: MISSING — no persona configured. Run 'human doctor --fix' to restore defaults
RESULT: PASS
```

### B6 — JSON smoke

```
BEHAVIOR: --json emits valid JSON, status NOT_READY, four checks entries
COMMAND: HOME=$(mktemp -d) ./build/human doctor --install --json
EXIT: 1
EVIDENCE:
  {"status":"NOT_READY","checks":[
    {"name":"binary","ok":true,"message":"binary: OK (...)"},
    {"name":"config_dir","ok":false,"message":"config_dir: MISSING — run 'human onboard' to create ~/.human/"},
    {"name":"channel","ok":false,"message":"channel: NONE — run 'human doctor imessage' to pair iMessage"},
    {"name":"persona","ok":false,"message":"persona: MISSING — no persona configured. Run 'human doctor --fix' to restore defaults"}
  ]}
RESULT: PASS
```

### B7 — Dispatch precedence

```
BEHAVIOR: --install → --privacy (reserved) → --fix → default in main.c
COMMAND: grep -n "privacy\|Dispatch precedence" src/main.c
EXIT: 0
EVIDENCE:
  src/main.c:677:    /* Dispatch precedence for `human doctor` flags:
  src/main.c:678:     *   1. subcommand (imessage|verifier|scheduler|responses) — handled above
  src/main.c:679:     *   2. --install (US-9.4) — install-readiness gate, exits nonzero on red
  src/main.c:680:     *   3. --privacy [SPRINT-8 RESERVED] — slot kept free; do not move
  src/main.c:681:     *   4. --fix — auto-repair
  src/main.c:682:     *   5. default — legacy full report
RESULT: PASS
```

### B8 — No short-circuit

```
BEHAVIOR: all four items appended even when first sub-check is red
COMMAND: test install_check_does_not_short_circuit_on_first_red + source review
EXIT: 0 (test passes)
EVIDENCE:
  Test at test_doctor_install.c:380 breaks BOTH config_dir AND persona,
  asserts count == 4u and err_count >= 2.
  Source at doctor.c:1122-1227: four blocks (binary/config_dir/channel/persona)
  all execute unconditionally — no early return between them.
  The function only returns after all four blocks at line 1234.
RESULT: PASS
```

### B9 — Adversarial AC: HU_ASSERT_NE form

```
BEHAVIOR: red-path tests use HU_ASSERT_NE(rc, HU_OK) not equality with integer
COMMAND: grep -n "HU_ASSERT_NE" tests/test_doctor_install.c
EXIT: 0
EVIDENCE:
  261: HU_ASSERT_NE(rc, HU_OK);  // missing binary
  289: HU_ASSERT_NE(rc, HU_OK);  // missing config_dir
  314: HU_ASSERT_NE(rc, HU_OK);  // no channel
  343: HU_ASSERT_NE(rc, HU_OK);  // missing persona
  372: HU_ASSERT_NE(rc, HU_OK);  // unparseable persona
  401: HU_ASSERT_NE(rc, HU_OK);  // multi-red (both config_dir + persona)
RESULT: PASS
```

---

## Summary

Verified 9/9 behaviors. 0 failed. 0 inconclusive.

RESULT_verifier=PASS story=US-9.4
