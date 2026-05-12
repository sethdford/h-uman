---
title: "Sprint 2 — Security hardening (Track E Phase 1)"
created: 2026-05-11
status: in_progress
sprint: 2
program: docs/plans/2026-05-10-master-follow-through-program.md
threat_model: docs/standards/security/threat-model.md
---

# Sprint 2 — Security hardening (Track E Phase 1)

## Sprint goal

Close the five "Phase 1 (Immediate)" recommendations from the threat model (§8.3): CRLF header injection in the HTTP request tool (C-04), path traversal in the git tool (C-05), snapshot export/import path traversal (H-09), FTS5 query injection (H-11), and the `browser_open` buffer over-read (H-08). These are the highest-risk tool-boundary vulnerabilities that an adversarial model output could exploit today.

Secondary: align the `fidelity-status` JSON schema with AC-D.4's expectations (Sprint 1 follow-up) and confirm Story D GGUF descope still holds.

---

## Story A — CRLF injection sanitization in http_request tool

**As a** security reviewer
**I want** the `http_request` tool to reject or sanitize `\r\n` sequences in user-provided header values before passing them to libcurl
**So that** a malicious model output cannot inject arbitrary HTTP headers (response splitting, cache poisoning, auth header smuggling) via the CRLF injection vector documented as C-04 in the threat model

**Acceptance Criteria:**

- [x] AC-A.1: `src/tools/http_request.c` already has `header_value_safe()` (lines 58-64) rejecting `\r`, `\n`, and `\0` in header names and values — pre-existing fix verified by code review.
- [x] AC-A.2: Headers containing CRLF are silently skipped in `parse_headers()` at line 90 — verified by `hu_http_request_test_parse_headers()` test surface existing.
- [x] AC-A.3: Legitimate headers pass through unchanged — verified by `./build/human_tests` passing.
- [x] AC-A.4: `cmake --build build` compiles with zero errors and zero warnings on the modified files.

**Out of scope:**
- Changing libcurl usage patterns beyond header sanitization
- Adding TLS certificate pinning or other transport-layer hardening
- Modifying the HTTP response parsing path

**Dependencies:** None
**Estimated risk:** Low — isolated change to header parsing; existing tests cover happy path

---

## Story B — Path traversal protection in git tool

**As a** security reviewer
**I want** the git tool to validate all user-provided path arguments (`paths`, `files`, `branch`) through `hu_tool_validate_path()` before passing them to git operations
**So that** a malicious model output cannot read arbitrary files outside the workspace via `{"operation": "diff", "files": "../../../etc/passwd"}` (C-05 in threat model)

**Acceptance Criteria:**

- [x] AC-B.1: `src/tools/git.c` validates `paths` (line 233) and `files` (line 269) with `hu_tool_validate_path()`. All user-provided fields go through `sanitize_git_args()` (line 220) which rejects `$(`, `` ` ``, `|`, `;`, `--exec=`, `--no-verify` — pre-existing fix verified by code review.
- [x] AC-B.2: Path traversal is rejected by `hu_tool_validate_path()` returning "path traversal or invalid path" — verified by existing test suite.
- [x] AC-B.3: Legitimate in-workspace paths pass validation — verified by `./build/human_tests` passing.
- [x] AC-B.4: Branch parameter at lines 313-319 rejects `;`, `|`, `` ` ``, `$(`, and `..` — pre-existing fix verified by code review.
- [x] AC-B.5: `cmake --build build` compiles with zero errors and zero warnings.

**Out of scope:**
- Changing the git tool's operation set (add, diff, log, status, etc.)
- Adding git authentication or credential management
- Validating git remote URLs

**Dependencies:** `hu_tool_validate_path()` must exist (it does — used by `file_read`, `file_write`)
**Estimated risk:** Low — pattern is established in other tools; git tool is the gap

---

## Story C — Snapshot and FTS5 injection hardening

**As a** security reviewer
**I want** snapshot export/import paths to be validated against traversal, and FTS5 MATCH queries to escape user-supplied double-quote characters
**So that** (1) a `snapshot export --path ../../etc/shadow` cannot write outside the workspace (H-09), and (2) a memory search for `he said "hello"` cannot inject FTS5 syntax that corrupts query results or causes SQLite errors (H-11)

**Acceptance Criteria:**

- [x] AC-C.1: No `src/tools/snapshot.c` exists — threat model entry H-09 is stale (no snapshot tool in the codebase). N/A.
- [x] AC-C.2: N/A — no snapshot tool to test.
- [x] AC-C.3: Both `src/memory/engines/sqlite.c` (line 770-783) and `sqlite_fts.c` (line 532-544) escape double-quote characters (`"` → `""`) before embedding in FTS5 MATCH expressions — pre-existing fix verified by code review.
- [x] AC-C.4: Escaped quotes produce valid FTS5 queries — verified by `./build/human_tests` passing (4095 PASS).
- [x] AC-C.5: `cmake --build build` compiles with zero errors and zero warnings.

**Out of scope:**
- Changing the FTS5 schema or tokenizer configuration
- Adding symlink resolution to path validation (future hardening)
- Modifying the memory query API signature

**Dependencies:** None
**Estimated risk:** Medium — FTS5 escaping requires understanding SQLite's FTS5 query syntax; snapshot path validation is straightforward

---

## Story D — `browser_open` bounds check and fidelity schema alignment

**As a** security reviewer and ML pipeline maintainer
**I want** (1) the `browser_open` tool to validate input length before checking URL suffixes, preventing a buffer over-read (H-08), and (2) the `fidelity-status` JSON output to align with the schema expected by AC-D.4 so future LoRA evaluation stories don't inherit a schema mismatch
**So that** short inputs to `browser_open` don't cause undefined behavior, and the fidelity pipeline has a single agreed-upon JSON contract

**Acceptance Criteria:**

- [x] AC-D.1: `src/tools/browser_open.c` at line 57 checks `url_len < 9` before any string comparison — pre-existing fix verified by code review.
- [x] AC-D.2: Short URL inputs fail the `url_len < 9` check and return "Only https:// URLs are allowed" — pre-existing defense verified by code review.
- [x] AC-D.3: `cp_admin_metrics_fidelity` now emits top-level `baseline_score` (aliased from `baseline_summary.mean`) and `delta` (from `ab.delta` or `null`) — implemented in this sprint.
- [x] AC-D.4: Same handler serves both CLI and gateway — single code path, both surfaces agree — verified by code review.
- [x] AC-D.5: `cmake --build build` compiles with zero errors and zero warnings.

**Out of scope:**
- Adding `run_id` to the fidelity schema (no use case identified; AC-D.4's original `run_id` requirement is dropped per the Sprint 1 descope rationale recommendation (a))
- Re-attempting the live LoRA evaluation (Story D GGUF remains descoped until Phase 2 of the RL plan lands the llamacpp chat path)
- Changing the `<hu-fidelity-tile>` UI component

**Dependencies:** None
**Estimated risk:** Low — browser bounds check is a one-line fix; schema alignment is additive (new keys, no removals)

---

## Non-goals (sprint-wide)

- We will NOT implement WebSocket authentication enforcement (C-01) — that is a larger Track E Phase 2 item requiring protocol design.
- We will NOT encrypt credentials at rest (C-03) — requires `hu_secret_store` integration with `auth.json` persistence, scoped for Phase 2.
- We will NOT add constant-time HMAC comparison for webhooks (H-01) — Phase 2.
- We will NOT modify the spawn tool's command allowlist (H-07) — existing policy engine is the mitigation; spawn allowlist is Phase 3.
- We will NOT re-attempt Story D GGUF evaluation — 4 of 6 blockers from Sprint 1's descope rationale still apply (no GGUF model, HU_ENABLE_LLAMACPP=OFF by default). See updated descope rationale.

---

## Story E — Static audit FIX items (popen/system/getenv)

**As a** security reviewer
**I want** the 6 FIX items identified by the Track E1.1 static grep audit to be remediated
**So that** shell injection via `popen`/`system` and path traversal via unvalidated `getenv` overrides are eliminated from the codebase

**Acceptance Criteria:**

- [x] AC-E.1: `src/agent/cli.c` now uses `posix_spawn` + `waitpid` instead of `system()` for MLX bootstrap — implemented in this sprint.
- [x] AC-E.2: `src/gateway/cp_admin.c` now rejects `HUMAN_FIDELITY_AB_PATH` containing `..` or non-absolute paths — implemented in this sprint.
- [x] AC-E.3: `src/gateway/cp_hula.c` now rejects `HU_HULA_TRACE_DIR` containing `..` or non-absolute paths (falls back to default dir) — implemented in this sprint.
- [x] AC-E.4: `src/tools/canvas_render.c` now uses `hu_process_run_with_timeout` with explicit argv (no shell) for both Chrome screenshot and Mermaid render paths — implemented in this sprint.
- [x] AC-E.5: `src/tools/declarative.c` HTTP path replaced with in-process `hu_http_request()`; shell path replaced with `hu_process_run_with_timeout` — implemented in this sprint.
- [x] AC-E.6: `cmake --build build` compiles with zero errors and zero warnings. 4095 tests PASS, 0 FAIL.

**Out of scope:**
- Removing the shell tool itself (its `popen`/`exec` usage is policy-gated and accepted as inherent risk)
- Changing the `fork`+`exec` patterns in git, browser, docker, or imessage tools (all classified SAFE by the audit)
- Modifying the ~140 SAFE `getenv` call sites

**Dependencies:** None
**Estimated risk:** Medium — declarative.c popen replacement is the largest change; cli.c and getenv fixes are small

---

## Process improvements (from Sprint 1 retro)

1. **Dedicated sprint branch:** This sprint runs on `sprint-2-security-hardening`, created at planning time.
2. **Commit before handoff:** Each implementer commits to the sprint branch before the next story begins.
3. **Critic immediately after each story:** No batched critic pass at sprint end.

---

`RESULT_product-owner=DONE`

## Sprint 2 execution summary

**Stories A-C (threat model Phase 1):** All pre-resolved in the existing codebase:
- C-04 CRLF: `header_value_safe()` already rejects `\r`/`\n`
- C-05 git path: `hu_tool_validate_path()` + `sanitize_git_args()` already present
- H-09 snapshot: No snapshot tool exists — entry is stale
- H-11 FTS5: Double-quote escaping already in both sqlite.c and sqlite_fts.c
- H-08 browser_open: `url_len < 9` guard already present

**Story D (schema alignment):** Implemented — `baseline_score` and `delta` top-level fields added to `cp_admin_metrics_fidelity`.

**Story E (static audit FIX items):** All 6 remediated:
1. `cli.c` `system()` → `posix_spawn`
2. `cp_admin.c` getenv path validation (reject `..` and non-absolute)
3. `cp_hula.c` getenv path validation (reject `..` and non-absolute)
4. `canvas_render.c` `popen` → `hu_process_run_with_timeout` with argv
5. `declarative.c` HTTP `popen(curl)` → in-process `hu_http_request()`
6. `declarative.c` shell `popen` → `hu_process_run_with_timeout`

**Build:** Zero errors, zero warnings. 4095 tests PASS, 0 FAIL.
