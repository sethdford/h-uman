# Design for US-9.6: `human doctor imessage` chat.db locked diagnostic

## Approach

The story asks for *user-actionable language* that distinguishes the
three reasons `chat.db` is inaccessible (Full Disk Access denied,
Messages.app holding a lock, or chat.db missing entirely). The
infrastructure already exists — `hu_imessage_classify_sqlite_error`
maps SQLite return codes to `hu_imessage_error_class_t`, the daemon
serializes that class into `~/.human/imessage.poll_status`, and
`hu_doctor_check_imessage` (`src/doctor.c:450`) already reads both the
live sqlite open and the poll-status JSON. What's missing is
*translation*: the doctor currently emits free-form `hu_sprintf` lines
that mix error codes ("rc=23") with FDA boilerplate even when the
underlying class is `BUSY`, and the JSON path has no stable
`error_class` field.

The cheapest design is to extract a **pure classifier-to-presentation
predicate** that lives in `src/doctor.c` next to the existing helpers
(`doctor_imsg_status_extract_*`) and is callable from both the live
sqlite branch (US-9.6 line ~493) and the poll-status branch
(line ~601). The predicate takes one input (the error-class enum or a
string for the poll-status path) and returns a `{ state, severity,
message, json_category }` quad. Tests construct fixtures (poll-status
JSON files in `tmp/` or direct enum inputs) and assert presentation
strings without ever opening a real `chat.db`. This matches
`.claude/rules/security-predicate-extraction.md`: the predicate is
pure, lives in the same translation unit, and is called by the
production code path — no duplication.

We avoid two near-misses:

1. **A new public type `hu_doctor_imessage_state_t`** in
   `include/human/doctor.h` was tempting (and the story prompt
   suggested it), but the existing `hu_imessage_error_class_t` is
   already the enum we need — adding a parallel enum would force a
   mapping table and risk drift. We re-use the existing enum and add
   only the presentation predicate.
2. **A full subcommand split (e.g. `human doctor imessage chat-db`).**
   The current `human doctor imessage` already surfaces the three
   diagnostic surfaces (live sqlite probe, imsg CLI, poll-status). We
   only refine the messaging, not the CLI shape.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `src/doctor.c` | Add `doctor_imessage_diagnose_class()` pure predicate that maps `hu_imessage_error_class_t` → `{ severity, message, category }`. Add `doctor_imessage_diagnose_poll_status()` that classifies a poll-status JSON blob (re-uses the same predicate). Wire both branches (live sqlite probe at line 493/518, poll-status reader at line 601) through these predicates. JSON branch in `src/main.c` already emits `category` — we set it to `"imessage_fda"` / `"imessage_busy"` / `"imessage_not_found"` / `"imessage_breaker"` so AC-9.6.5 holds via the existing serializer. | +80 |
| `src/doctor.c` | Add a `hu_imessage_diag_from_poll_status()` non-static wrapper exported via `include/human/doctor.h` so tests can call it without going through `hu_doctor_check_imessage` (which still touches `$HOME/Library/Messages/chat.db`). | +25 |
| `include/human/doctor.h` | Declare the wrapper. Document the predicate contract. | +12 |
| `tests/test_doctor_imessage_diagnose.c` | NEW. 7 fixture tests (see test strategy below). References `hu_imessage_diag_from_poll_status` / `hu_doctor_check_imessage` to satisfy `.claude/rules/test-references-production-symbol.md`. | +220 |
| `tests/CMakeLists.txt` (or wherever test suites are registered) | Register the new test file. | +1 |

Net: ~+340 LOC, no public API breaks; existing tests in
`tests/test_ported_modules.c` (lines 555-809) remain valid because
`hu_doctor_check_imessage`'s signature is unchanged.

## Implementation steps (for the implementer agent)

1. **Skeleton predicate.** Add static functions to `src/doctor.c`:
   ```c
   typedef struct doctor_imsg_presentation {
       hu_diag_severity_t severity;
       const char *category;   /* stable enum-as-string for JSON */
       char *message;          /* heap, hu_sprintf'd; caller frees */
   } doctor_imsg_presentation_t;

   static hu_error_t doctor_imessage_diagnose_class(
       hu_allocator_t *alloc,
       hu_imessage_error_class_t cls,
       int sqlite_rc,                /* for "(rc=23)" tail; -1 = unknown */
       const char *db_path,          /* for the FDA navigation hint */
       doctor_imsg_presentation_t *out);
   ```
   Returns `HU_OK` and populates `out` for every input; never fails
   except `HU_ERR_OUT_OF_MEMORY`. Initially returns an empty message
   and `HU_DIAG_OK` for all branches.

2. **Truth-table tests (must fail).** Write
   `tests/test_doctor_imessage_diagnose.c` with seven cases:

   | # | Input class | Expected category | Expected severity | Adversarial assertion |
   |---|---|---|---|---|
   | 1 | `HU_IMESSAGE_ERR_AUTH` | `imessage_fda` | `HU_DIAG_ERR` | message CONTAINS "Full Disk Access" AND CONTAINS "System Settings" AND does NOT contain "Messages.app may be syncing" |
   | 2 | `HU_IMESSAGE_ERR_BUSY` | `imessage_busy` | `HU_DIAG_WARN` | message CONTAINS "Messages.app may be syncing" AND does NOT contain "Full Disk Access" |
   | 3 | `HU_IMESSAGE_ERR_CANTOPEN` | `imessage_not_found` | `HU_DIAG_ERR` | message CONTAINS "chat.db" AND CONTAINS "not found" AND does NOT contain "permission" / "Full Disk Access" |
   | 4 | `HU_IMESSAGE_ERR_NONE` | `imessage_chat_db` | `HU_DIAG_OK` | severity is OK; message does NOT contain "error" or "denied" |
   | 5 | `HU_IMESSAGE_ERR_OTHER` | `imessage_other` | `HU_DIAG_ERR` | message references sqlite rc; does NOT pretend to know the cause |
   | 6 | Poll-status JSON with `"last_error_class": "AUTH"` and `"circuit_breaker_tripped": true` | `imessage_breaker` | `HU_DIAG_ERR` | message contains "Full Disk Access" AND mentions `consecutive_open_failures` count AND suggests `human doctor --fix` (AC-9.6.3) |
   | 7 | Poll-status JSON with `"last_error_class": "BUSY"`, breaker not tripped | `imessage_busy` | `HU_DIAG_WARN` | message says "transient" / "syncing"; does NOT contain "Full Disk Access" |

   All seven must fail against the skeleton.

3. **Implement predicate.** Fill in each enum branch with
   `hu_sprintf`'d strings that satisfy the assertions. Re-use the exact
   FDA navigation phrasing already in `src/doctor.c:506-508` for the
   AUTH branch (consistency with the existing live-sqlite path).

4. **Wire production callers.** Replace the inline `hu_sprintf` calls
   in the two live-sqlite branches (`src/doctor.c:495-514` and
   `:518-530`) and the poll-status branch (`:603-625`) with calls to
   the predicate. Each call site now reads:
   ```c
   doctor_imsg_presentation_t p = {0};
   doctor_imessage_diagnose_class(alloc, cls, rc, db_path, &p);
   doctor_push_item(alloc, items, count, cap, p.severity, p.category, p.message);
   if (p.message) alloc->free(alloc->ctx, p.message, strlen(p.message) + 1);
   ```
   (`doctor_push_item` is a small wrapper around the existing
   `doctor_push_line` that also sets `category`.)

5. **Export the wrapper for AC-9.6.4.** Add
   `hu_imessage_diag_from_poll_status(const char *json_blob,
   hu_diag_item_t *out)` to `include/human/doctor.h`. The wrapper
   parses the blob with the existing `doctor_imsg_status_extract_*`
   helpers and delegates to the predicate. Test 6 and 7 use this entry
   point — they never call `hu_doctor_check_imessage` (so they don't
   touch `$HOME`).

6. **Run targeted suite.**
   ```bash
   ./build/human_tests --filter=doctor_imessage_diagnose
   ```
   All seven must pass.

7. **Run full suite (`.claude/rules/tests-that-pin-bugs.md` —
   don't trust targeted-only).**
   ```bash
   ./build/human_tests
   ```
   Confirm `tests/test_ported_modules.c` doctor-imessage cases
   (lines 555-809) still pass — they assert the legacy free-form
   strings, so they may need light updates to match the new
   category-aware output. **Update those tests' assertions to match
   the new wording AND add a comment** referencing this story so a
   future reader knows why the strings changed.

8. **Run `/verify`** to confirm `RESULT_verifier=PASS`.

## Risks

- **Backward compat — output string regression (MEDIUM/SMALL).**
  Existing tests in `tests/test_ported_modules.c` (the
  `hu_doctor_check_imessage red-team` block, line 555+) assert that
  the doctor output contains specific substrings ("Full Disk Access",
  "circuit breaker", etc). The new predicate-driven output must
  preserve those substrings. **Mitigation:** before changing wording,
  grep the test tree for the existing substrings and treat any test
  hit as a contract the new wording must also satisfy.
- **JSON consumer breakage (LOW/SMALL).** AC-9.6.5 asks for a
  `category` field that monitoring tools may key off. The main.c JSON
  serializer (line 615+) already emits `category` if non-NULL — the
  new code only needs to populate it consistently. No JSON schema
  change is needed; we're filling an existing optional field.
  **Mitigation:** AC-9.6.5 maps to test 1 + a JSON-format integration
  test (`tests/test_doctor_imessage_json.c` — optional, see note).
- **`HU_ENABLE_SQLITE=OFF` and `HU_HAS_IMESSAGE=OFF` builds
  (LOW/SMALL).** The predicate must compile and link in those builds.
  **Mitigation:** the predicate takes raw enum / int / string inputs
  (no sqlite headers); it's `#ifdef`-free.
- **Data integrity — none.** Doctor is read-only.
- **Concurrency — none.** Doctor reads a snapshot file already
  written atomically by `imessage_save_poll_status`.
- **Performance — none.** Adds one `hu_sprintf` per call; doctor runs
  at human interaction frequency.
- **Adversarial test inversion
  (`.claude/rules/tests-that-pin-bugs.md` — MEDIUM/SMALL).** Test 1's
  most important assertion is the *negative* one ("does NOT contain
  'Messages.app may be syncing'") — if the implementer accidentally
  writes a generic "could be FDA or syncing" message, test 1 must
  fail. **Mitigation:** every truth-table row above includes at least
  one negative assertion; reviewer checks that all negatives are
  exercised.
- **Observability — LOW.** The doctor command IS the observability.
  Adding `category` makes monitoring keying easier (Datadog/Slack can
  alert on `category=imessage_fda` regardless of wording drift).

## Test strategy

- **Unit / pure-predicate tests** in
  `tests/test_doctor_imessage_diagnose.c`: 7 cases (truth table
  above). No filesystem, no sqlite, no `$HOME` access.
- **Adversarial cross-check** (AC-9.6 adversarial line): explicitly
  assert that AUTH-class output does NOT contain BUSY phrasing and
  vice versa. These assertions must be `HU_ASSERT_FALSE(contains)` —
  per `.claude/rules/tests-that-pin-bugs.md`, the test name
  ("test_busy_state_does_not_mention_fda") is a claim, not a label.
- **Existing integration tests** in `tests/test_ported_modules.c`
  remain — they exercise `hu_doctor_check_imessage` end-to-end with a
  fixture `imessage.poll_status` file under a temp `$HOME`. Update
  their string-contains assertions to match the new predicate output;
  add comments tying each updated assertion to US-9.6.
- **No new fuzz harness** — input space is a 5-variant enum + a
  bounded JSON blob already covered by existing `doctor_imsg_status_*`
  parsers (which DO have fuzz coverage in `fuzz/`).
- **Manual smoke test** (DoD line in stories.md): documented in PR
  description, not enforced in CI (FDA toggling is non-deterministic
  on hosted runners).

## Acceptance criteria mapping

- **AC-9.6.1** (`last_error_class=AUTH` → output contains "Full Disk
  Access" + System Settings path, red indicator) → truth-table test
  #1 (AUTH branch) plus test #6 (poll-status AUTH + breaker tripped).
- **AC-9.6.2** (`last_error_class=BUSY` → "Messages.app may be
  syncing", yellow/WARN distinct from red) → truth-table test #2
  (BUSY branch, includes the negative "does NOT contain Full Disk
  Access" assertion) and test #7 (poll-status BUSY, severity is
  `HU_DIAG_WARN`).
- **AC-9.6.3** (breaker tripped → output includes
  `consecutive_open_failures` and suggests `human doctor --fix`) →
  truth-table test #6.
- **AC-9.6.4** (tests write mock poll-status JSON to a temp path and
  call the doctor check; assert message contains "Full Disk Access";
  reference production symbol) → tests #6, #7 directly call
  `hu_imessage_diag_from_poll_status` (the exported wrapper). The
  test file also `#include`s `human/doctor.h` and at least one
  assertion references `hu_doctor_check_imessage` to satisfy
  `.claude/rules/test-references-production-symbol.md`.
- **AC-9.6.5** (`--json` output contains stable
  `{"check": "imessage_fda", "ok": false, "error_class": "AUTH",
  "message": "...Full Disk Access..."}`) → category field is set by
  the predicate (test #1 asserts `category == "imessage_fda"`);
  serialization to JSON is already handled in `src/main.c:615+`. We
  add an additional `error_class` field to the JSON output —
  trivial 5-line change in `cmd_doctor` after the `category` print.
  Verified by manual `human doctor imessage --json` snapshot in PR
  description; or a small JSON parse test if time permits (optional,
  flagged as a stretch).

## Out of scope (story explicit)

- Auto-granting FDA (impossible on macOS 13+).
- Circuit-breaker threshold changes (`HU_IMESSAGE_BREAKER_THRESHOLD`
  stays at its current value).
- `chat.db` schema introspection.
- The `human doctor --fix` reset path itself (AC-9.6.3 only needs the
  *suggestion*, not the implementation; the fix lives in a separate
  story).
