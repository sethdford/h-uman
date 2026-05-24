# Design for US-C1.3a: Notarization Failure Translator

## Summary
Translate Apple's notarization rejection JSON (from `xcrun notarytool log`) into human-readable, actionable diagnostics. Map cryptic issue codes to concrete fixes: "binary uses old SDK" → "rebuild with newer macOS SDK", "missing timestamp" → "add --timestamp to codesign", etc. Output is a per-issue summary with severity, location, problem, and fix command.

## Approach

**Tight scope:** One pure function (`notary_translate_issues`) accepts parsed JSON (struct with "issues" array) and returns rendered text. I/O (file read, network fetch) is a thin wrapper in the shell script. This shape is testable without invoking notarytool or touching the network.

**Translation table:** Hardcoded map of ~8 common Apple rejection patterns → human text + fix. Unknown issues fall back to a URL link to Apple's docs. Table lives in the C function as a static array of `{ code_pattern, message_template, fix_command }` structs.

**Resilience:** JSON schema validation happens before translation. If the log structure doesn't match expectations (missing "issues" key, issue missing "message" field), we emit a clear error ("Unrecognized notarization log format; check Apple's latest schema") instead of crashing.

**Exit code:** Number of issues found (0 = clean). Caller can branch on `[ $? -eq 0 ]` for success.

## Files to modify

| File | Change | Est LOC |
|---|---|---|
| `scripts/release/diagnose-notary.sh` | New shell wrapper accepting `--log <path>` or `--submission-id <id>` flags; error handling for missing file; calls the C translator and formats output | +100 |
| `src/tools/notary_translate.c` | Pure function `hu_notary_translate_issues(const cJSON *log_root, hu_string_buf_t *out)` with translation table for 8 common patterns; JSON schema validation; returns `hu_error_t` | +180 |
| `include/human/tools/notary_translate.h` | Public header with function prototype and schema documentation (sample JSON structure) | +50 |
| `tests/test_notary_translate.c` | 5 test cases: one per common rejection type (sdk-old, timestamp-missing, runtime-disabled, unsupported-entitlement, cfrundle-version), plus one unknown-issue fallback; 80+ LoC | +90 |
| `tests/fixtures/notary/` | 5 minimal JSON fixtures (one per test case) | +200 (5 files × 40 lines) |

## Implementation steps (for the implementer)

1. **Define the JSON schema** — document expected "issues" array structure in `include/human/tools/notary_translate.h` with a comment containing a representative sample JSON object
2. **Create `hu_notary_translate_issues` skeleton** with signature, type stubs, and schema validation (return `HU_ERR_JSON_INVALID` if log_root is NULL or missing "issues")
3. **Write 5 test fixtures** (minimal JSON objects representing real Apple rejection scenarios)
4. **Write test cases** (one per fixture; assert output contains expected fix text)
5. **Implement translation table** as `static const` array of pattern-match structs; add `_find_matching_pattern` helper that checks issue message against each pattern
6. **Implement pattern matching** — string substring search for each known pattern; collect all matching issues and render them to the output buffer
7. **Handle unknowns** — if no pattern matches, render the issue as-is with a fallback "See Apple's docs: <url>"
8. **Wire into sign-and-notarize.sh** — in US-C1.3, capture notarytool's JSON output, pipe through this translator, print the result to stderr before exiting 1
9. **Shell script wrapper** — `diagnose-notary.sh --log <path>` reads file, calls translator, exits with issue count

## Risks

| Risk | Prob | Impact | Mitigation |
|---|---|---|---|
| **Apple changes JSON schema** | MED | MED | JSON validation step emits "Unrecognized format" on schema mismatch; script exits 1 with a helpful message, not a crash |
| **Pattern matching too brittle** | MED | SMALL | Patterns are substring matches (case-insensitive), not exact equality; covers Apple's habit of appending version/details. Fallback URL provided for unknowns |
| **Network fetch in --submission-id path** | LOW | MED | Fetching via `xcrun notarytool info` happens only if user explicitly asks for it; documented as optional. Error handling for network failure. |
| **Encoding/BOM in JSON** | LOW | SMALL | Use cJSON (already a dependency in the codebase) to parse; it handles BOM and encoding gracefully |

## Test strategy

**Unit tests only** — all 5 test cases use fixture JSON files (no network, no file I/O calls to notarytool). Each test:
1. Load fixture JSON (via `hu_load_json_from_file("tests/fixtures/notary/sdk-old.json")` or inline JSON string)
2. Call `hu_notary_translate_issues(parsed_json, &output_buf)`
3. Assert `output_buf` contains expected fix text (e.g., "Add --timestamp", "Rebuild with", "Check Keychain", etc.)

**Integration seam (skip at first):** In US-C1.3, a script-level test can mock `xcrun notarytool log` to return our test fixture JSON and verify the shell wrapper calls the translator and exits with the right code.

## Acceptance criteria mapping

- AC-C1.3a.1 (parse common failures) → test cases for sdk-old, timestamp-missing, runtime-disabled, unsupported-entitlement, cfbundle-version
- AC-C1.3a.2 (output explanation + fix command) → each test assertion checks for fix text in output
- AC-C1.3a.3 (no network in tests) → all fixtures are JSON files; no shell invocation

## JSON Schema

Expected structure (sample):
```json
{
  "issues": [
    {
      "severity": "error",
      "architecture": "arm64",
      "message": "The binary uses an SDK older than the recommended macOS 10.15; rebuild with macOS 10.15 SDK",
      "docUrl": "https://developer.apple.com/...",
      "path": "Payload/Applications/Human.app/Contents/MacOS/human"
    }
  ]
}
```

Validation: must have "issues" key (array); each issue must have "message" string.

## Out of scope

- Fixing the issues automatically (e.g., invoking codesign ourselves)
- Fetching notarization logs from Apple (--submission-id path is nice-to-have; main flow reads local JSON file)
- Providing machine-readable output (this is human-first; JSON output can come in a future PR)
