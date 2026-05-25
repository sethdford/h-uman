# Design for US-C3.7: Doctor `--json` output

## Approach

US-C3.7 adds structured JSON output to the `human doctor` command, enabling observability tooling (Datadog, Slack webhooks, monitoring dashboards) to parse diagnostics programmatically. The implementation layers onto the existing doctor subsystem architecture without refactoring it.

The design reuses the existing check-result structure and the registry pattern. After all checks run via `hu_doctor_registry_run_all`, the output formatter consumes the `hu_doctor_check_result_t[]` array and either:
- Emits human-readable text (default, no `--json` flag)
- Emits JSON v1 schema to stdout (if `--json` flag is present)

All human-readable diagnostics are suppressed in `--json` mode. The JSON schema is locked at v1; future enhancements (v2+) only ship via explicit `--json=v2` opt-in to prevent silent breakage for consuming agents.

The JSON emitter uses manual `fprintf` with simple character-by-character escaping for `"` and `\` in string fields. This avoids introducing a JSON library dependency for a ~300-byte output blob.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `src/main.c::cmd_doctor` | Add `--json` flag parsing and v1 emitter function + ISO-8601 timestamp generation | +80 |
| `tests/test_doctor_json_output.c` | New test file: schema validation, field presence, aggregate calculation, timestamp format | +250 |
| `tests/fixtures/doctor_pass_all/` | Fixture HOME tree (minimal config.json where all checks pass) | ~20 |
| `tests/fixtures/doctor_fail_provider/` | Fixture HOME tree (config.json with missing provider, provider check fails) | ~20 |
| `CMakeLists.txt` | Add test source + fixture paths | +10 |

## Implementation steps (for the implementer agent)

1. **Write the ISO-8601 timestamp helper** in `src/main.c` near `cmd_doctor`:
   - Input: `time_t` (wall-clock epoch seconds)
   - Output: `char[]` buffer with format `YYYY-MM-DDTHH:MM:SSZ` (UTC, fixed-width)
   - Test manually: `2026-05-25T14:30:00Z`

2. **Implement the JSON v1 emitter** as a static function in `src/main.c`:
   - Signature: `static void emit_doctor_json_v1(const hu_doctor_check_result_t *checks, size_t count, time_t now_epoch)`
   - Schema: emit `{"version":1,"ts":"...","checks":[...],"aggregate":"..."}` to stdout
   - For each check: `{"name":"<name>","verdict":"pass|fail","reason":"<text>"}`
   - `aggregate` = "pass" iff every verdict is "pass", else "fail"
   - Escape `"` and `\` in `reason` field only (name is system-generated, not user-data)
   - Newline at end of JSON blob

3. **Add `--json` flag parsing** to the dispatch logic in `cmd_doctor`:
   - Extract the flag-parsing loop (currently lines 852–859)
   - Add `bool emit_json = false;` and `if (argv[i] && strcmp(argv[i], "--json") == 0) emit_json = true;`
   - This is already partially done for `--install` (lines 884); reuse the same pattern

4. **Gate the output** based on the flag:
   - If `emit_json` is true AND running the default check path (not a subcommand), call `emit_doctor_json_v1` instead of the human-readable loop
   - In `--json` mode, suppress all human-readable text; only JSON goes to stdout
   - stderr remains empty on PASS (no debug output)

5. **Create test fixtures**:
   - `tests/fixtures/doctor_pass_all/`: `.human/config.json` with minimal valid config (one provider, one channel)
   - `tests/fixtures/doctor_fail_provider/`: `.human/config.json` with no provider (provider check will FAIL)
   - Each fixture is a shallow HOME tree; tests will set `HOME=/path/to/fixture` before running `human doctor`

6. **Write `tests/test_doctor_json_output.c`**:
   - Test 1: `test_doctor_json_v1_schema_valid` — parse the JSON output via `hu_json_parse`, walk the structure, assert all required fields present
   - Test 2: `test_doctor_json_v1_all_pass_aggregate_pass` — fixture `doctor_pass_all`, verify `aggregate == "pass"`
   - Test 3: `test_doctor_json_v1_one_fail_aggregate_fail` — fixture `doctor_fail_provider`, verify `aggregate == "fail"`
   - Test 4: `test_doctor_json_v1_timestamp_format_iso8601` — verify `ts` field matches regex `^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$`
   - Test 5: `test_doctor_json_v1_version_locked` — verify `version == 1` (future-proofs against silent schema drift)
   - Test 6: `test_doctor_json_v1_stderr_empty_on_pass` — capture stderr, assert it's empty
   - Integration test: spawn `human doctor --json` with fixture, parse output, validate round-trip

7. **Update CMakeLists.txt**:
   - Add `tests/test_doctor_json_output.c` to `HU_TEST_SOURCES`
   - No build-time gates needed (JSON output is always compiled)

8. **Run full test suite** to ensure no regressions in existing doctor checks

## Risks

### JSON schema evolution (MEDIUM / MEDIUM)
**What could go wrong:** A future story adds a new field to the schema (e.g., `"metadata": {...}`) without bumping the version. Consuming agents relying on `version == 1` to parse the schema break silently.

**Probability:** Medium (very likely if not explicitly gated in code)

**Impact:** Medium (parsers fail, but the failure is visible in monitoring dashboards; not a data-loss risk)

**Mitigation:** 
- The v1 schema is explicitly locked in the stories.md AC-1.2. 
- Document in `docs/guides/doctor.md` (US-C3.9 story) that v2+ changes ONLY ship via `--json=v2` flag (explicit opt-in).
- Add a pre-commit script (`scripts/check-doctor-json-schema-version.sh`) to verify any change to the JSON emitter increments the version number.

### Merge conflict with US-C3.9 (LOW / SMALL)
**What could go wrong:** Both US-C3.9 and US-C3.7 edit `src/doctor.c` and `src/main.c::cmd_doctor`. If merged out of order, the second story's diff will be invalid.

**Probability:** Low (wave plan explicitly serializes the two stories)

**Impact:** Small (rebase is straightforward; the stories don't overlap on the same lines)

**Mitigation:**
- Wave plan mandates US-C3.9 lands first (exit-code contract simpler, JSON builds on it).
- Implementer for US-C3.7 rebases after US-C3.9 merges.
- No destructive operations in either story.

### stderr discipline in `--json` mode (MEDIUM / MEDIUM)
**What could go wrong:** One of the doctor checks emits to stderr (e.g., a debug log or error log inside `hu_doctor_check_*`). In `--json` mode, the operator expects stderr to be empty on PASS.

**Probability:** Medium (existing checks may have debug output; need to audit)

**Impact:** Medium (monitoring agent parsing stderr may fail; not a correctness issue, but violates the contract)

**Mitigation:**
- Audit existing check functions for stderr writes.
- Wrap any debug output with `#ifdef HU_IS_TEST` or similar.
- Test explicitly captures stderr and asserts it's empty on PASS.

## Test strategy

- **Unit tests** in `tests/test_doctor_json_output.c`:
  - Schema structure validation (JSON parse, field presence)
  - Timestamp format validation
  - Aggregate logic (all pass → "pass", any fail → "fail")
  - Version field locked at 1
  - stderr empty on PASS
  - Round-trip: spawn `human doctor --json`, parse, validate

- **Integration via fixtures**:
  - `doctor_pass_all/` exercises the happy path (all checks pass)
  - `doctor_fail_provider/` exercises failure case (aggregate = "fail")

- **No changes needed to existing doctor tests** (this story is output-only; the check logic is unchanged)

## Acceptance criteria mapping

- **AC-1.1** (--json flag exists): Implemented by flag parsing in cmd_doctor (step 3)
- **AC-1.2** (JSON schema v1 locked): Implemented by emit_doctor_json_v1 function with hardcoded "version":1 (step 2)
- **AC-1.3** (aggregate iff all pass): Implemented in JSON emitter loop (step 2)
- **AC-1.4** (stdout/stderr discipline): Tested by `test_doctor_json_v1_stderr_empty_on_pass` (step 6)
- **AC-1.5** (fixtures): Created in steps 5
- **AC-1.6** (test schema structure): Tested by `test_doctor_json_v1_schema_valid` (step 6)

## Out of scope

- New doctor checks or check enhancements (those belong in US-C3.3, US-C3.9, etc.)
- `--json=v2` flag implementation (future story only)
- `--json --fix` output format (US-C3.8 out of scope per sprint backlog)
- docs/guides/doctor.md documentation (US-C3.9 owns that)
- Exit-code contract changes (US-C3.9 owns that)

## Schema v1 (LOCKED)

```json
{
  "version": 1,
  "ts": "2026-05-25T14:30:00Z",
  "checks": [
    {
      "name": "install",
      "verdict": "pass",
      "reason": ""
    },
    {
      "name": "provider",
      "verdict": "fail",
      "reason": "provider not configured"
    }
  ],
  "aggregate": "fail"
}
```

**Field semantics:**
- `version`: Integer; always 1 for this story. Future major changes increment this.
- `ts`: ISO-8601 UTC timestamp in `YYYY-MM-DDTHH:MM:SSZ` format.
- `checks[]`: Array of check results. Order matches registry order (stable).
- `name`: String; system-generated check name (no escaping needed).
- `verdict`: Enum: "pass" (PASS), "fail" (FAIL or N/A per AC-1.3).
- `reason`: String; diagnostic message. Empty string on PASS. Escaped for `"` and `\`.
- `aggregate`: Enum: "pass" iff ALL verdicts are "pass", else "fail".

## Notes on implementation

### Why manual escaping, not a JSON library?

The output blob is ~300 bytes (ten checks × 30 bytes each). A JSON library introduces ~20 KB of code and dependencies. Manual `fprintf` with character-level escaping is ~20 lines and zero dependencies. The project already uses this pattern in `src/main.c` line 793–814 for `--install --json` output.

### Timestamp generation

Use `time(NULL)` (wall-clock) and `gmtime_r()` or equivalent to convert to UTC. Format to buffer via `snprintf` with format string `"%04d-%02d-%02dT%02d:%02d:%02dZ"`. Pinned by test `test_doctor_json_v1_timestamp_format_iso8601`.

### Integration with US-C3.9

US-C3.9 adds exit-code logic to `cmd_doctor`. The JSON output is orthogonal to exit codes. US-C3.7 only affects stdout/stderr formatting; the exit code is determined by US-C3.9 (0 = all pass, 1 = user-action-required fail, 2 = bug-grade fail, etc.).

Merge order: US-C3.9 first, then US-C3.7 rebases on top.

