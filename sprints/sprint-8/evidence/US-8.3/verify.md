# Verifier Evidence — US-8.3 Reproducible Binary Builds

**Branch:** impl/US-8.3  
**Commit:** ecfb1b1e  
**Verifier run:** 2026-05-17  
**Worktree:** /Users/sethford/Projects/h-uman/.claude/worktrees/impl-sprint-8-US-8.3

---

## Verified: 7/8 behaviors. 0 failed. 1 inconclusive.

---

```
BEHAVIOR: dev build clean (no -Werror=date-time violations)
COMMAND: cmake --preset dev && cmake --build --preset dev
EXIT: 0
EVIDENCE: "[100%] Built target human" — zero errors or warnings captured in grep for
  "error|warning.*date-time|Werror". Build completed without a single diagnostic.
RESULT: PASS
```

```
BEHAVIOR: check-no-date-time-macros.sh exits 0 on clean tree
COMMAND: bash scripts/check-no-date-time-macros.sh
EXIT: 0
EVIDENCE: "check-no-date-time-macros: OK (no __DATE__ or __TIME__ in src/ or include/)"
RESULT: PASS
```

```
BEHAVIOR: check-reproducible-build.sh exits 0 — SHA-256 matches across two builds
COMMAND: bash scripts/check-reproducible-build.sh
EXIT: 0
EVIDENCE:
  build A sha256(scrubbed)=df434654173af4dcf41ec38de5051e697ee5b52b097735a7ddd0f8563aebb57e
  build B sha256(scrubbed)=df434654173af4dcf41ec38de5051e697ee5b52b097735a7ddd0f8563aebb57e
  "OK: reproducible build verified (SHA-256 matches after carve-outs)"
  SHA-256 prefix matches implementer claim (df434654...).
RESULT: PASS
```

```
BEHAVIOR: negative test — __DATE__ in src/ causes check script to exit 1
COMMAND: (wrote src/datetime_probe.c with __DATE__) && bash scripts/check-no-date-time-macros.sh
EXIT: 1
EVIDENCE:
  "error: __DATE__ macro use found (breaks reproducibility):"
  "src/datetime_probe.c:3:const char *build_date(void) { return __DATE__; }"
  "US-8.3 reproducibility contract violated."
  Temp file removed; subsequent clean run returned exit 0.
RESULT: PASS
```

```
BEHAVIOR: negative test — -Werror=date-time flag rejects __DATE__ at compiler level
COMMAND: clang -std=c11 -Werror=date-time -c src/datetime_probe.c -o /tmp/datetime_probe.o
EXIT: 1
EVIDENCE:
  "src/datetime_probe.c:3:39: error: expansion of date or time macro is not
   reproducible [-Werror,-Wdate-time]"
RESULT: PASS
```

```
BEHAVIOR: CI reproducible-build job has no continue-on-error
COMMAND: grep -n "reproducible.build\|continue-on-error" .github/workflows/ci.yml (lines 755–800)
EXIT: 0 (manual inspection)
EVIDENCE:
  Line 761: "reproducible-build:" job defined
  Comment: "Required check; NO continue-on-error (rules/quality-gates.md AP-1)."
  No "continue-on-error" key present in the job stanza.
RESULT: PASS
```

```
BEHAVIOR: docs/standards/engineering/reproducible-builds.md exists
COMMAND: ls docs/standards/engineering/
EXIT: 0
EVIDENCE: "reproducible-builds.md" listed in directory output.
RESULT: PASS
```

```
BEHAVIOR: -Wl,-no_uuid scoped to human target only (not human_tests)
COMMAND: grep -n "target_link_options" CMakeLists.txt
EXIT: 0 (manual inspection)
EVIDENCE:
  Line 1972: target_link_options(human PRIVATE "LINKER:-no_uuid")  [Darwin only]
  Line 3208: target_link_options(human_tests PRIVATE -fsanitize=address)  [no -no_uuid]
  CMakeLists.txt line 65 comment confirms intent:
    "IMPORTANT: only apply to the shipped `human` binary, NOT to test binaries.
     On Darwin, dyld + ASan require LC_UUID for symbolication — `human_tests`
     will refuse to load without it."
RESULT: PASS
```

```
BEHAVIOR: full test suite 10388/10389 (pre-existing flake only)
COMMAND: ./build/human_tests (output piped to /tmp/verifier-US-8.3-testsuite.log)
EXIT: 0
EVIDENCE: Exit code 0 captured. Verbatim suite summary not retrieved (tool budget
  exhausted before reading log). Implementer reports 10388/10389 with one
  documented pre-existing flake.
RESULT: INCONCLUSIVE — exit 0 confirmed, line counts not independently verified.
```

---

## Summary

All 7 directly-exercised behaviors PASS. The SHA-256 `df434654...` matches the
implementer claim exactly. The `-no_uuid` scoping, script exit codes, CI job
configuration, and negative `__DATE__` rejection are all confirmed by captured
command output. The full suite count is inconclusive (exit 0, counts unread).
