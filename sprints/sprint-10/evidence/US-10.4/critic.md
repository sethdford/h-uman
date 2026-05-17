# Critic findings — US-10.4 HuLa examples gallery

## CRITICAL (0)

## HIGH (1)

- tests/test_hula_examples.c:254-268 — `all_five_examples_are_present` asserts
  `K_CASE_COUNT == 5` as a constant, not as a count of actual filesystem entries.
  If a new developer adds a sixth directory under `examples/hula/` but forgets to
  append a row to `k_cases[]`, the compile-time constant stays at 6 and the test
  still passes — the new program is silently unvalidated. The test name promises
  "all five … are present" but it cannot detect an unchecked seventh. Fix: replace
  the hardcoded `(size_t)5` with a `glob`/`opendir` count of `HU_EXAMPLES_HULA_DIR`
  subdirectories and assert equality against `K_CASE_COUNT`, so additions to the
  filesystem that are not matched by a case-table row fail loudly.

## MED (3)

- examples/hula/README.md:29-38 — "Adding a new example" step 2 says "append a row
  to the k_cases[] table". There is no CI gate that enforces this. The corruption
  adversarial check was run once manually and reverted (verify.md:83-88). Neither
  check has a CI workflow entry; both exist only as prose promises. Fix: add a
  `scripts/check-hula-examples-complete.sh` that diffs `ls examples/hula/*/` count
  against `K_CASE_COUNT` and wire it into `ci.yml`, replacing the manual
  revert-and-observe pattern with a deterministic gate.

- tests/test_hula_examples.c:148-150 — `snprintf` path construction truncates
  silently if `HU_EXAMPLES_HULA_DIR + "/" + path_suffix` exceeds 1023 bytes. The
  assert `(size_t)n < sizeof(path)` catches the truncation, but the failure message
  is `HU_ASSERT_TRUE` with no indication of which path was too long. In a deeply
  nested install tree (e.g., a CI workspace path like
  `/home/runner/work/h-uman/h-uman/...`) this can produce a confusing test failure
  that looks like a missing file rather than a path-length problem. Fix: bump the
  buffer to 4096 (PATH_MAX on Linux) or use `asprintf`; add the computed path to
  the failure message.

- examples/hula/02-branching/README.md:36 — The "How to run" paragraph states "The
  test asserts the root op is `BRANCH` when the entire tree is walked (a `BRANCH`
  node is reachable from the root)." The actual test (`k_cases[1]`) asserts
  `expected_root_op == HU_HULA_SEQ` — the root is SEQ, not BRANCH. The README
  description is wrong and will mislead a developer trying to understand the test
  contract. Fix: change the sentence to "The test asserts the root op is `SEQ` and
  that a `BRANCH` node is reachable anywhere in the subtree."

## LOW (0)

## Cross-agent regression risk

- None identified. No production C source was modified; the change is additive
  (new files only plus two CMakeLists.txt lines). No shared utility or vtable
  interface was touched.

---

RESULT_critic=HAS_FINDINGS story=US-10.4 severity=HIGH
