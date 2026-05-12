---
title: "Sprint 2 — Security hardening (Track E Phase 1)"
created: 2026-05-12
status: done
sprint: 2
---

# Sprint 2 — Security hardening (Track E Phase 1)

## Sprint goal

Remediate all 6 FIX items from the Track E1.1 static grep audit, harden
snapshot export/import path validation, and align the fidelity-status JSON
schema with AC-D.4.

## Story E — Static audit FIX items (popen/system/getenv)

- [x] AC-E.1: `src/agent/cli.c` replaces `system(cmd)` (MLX bootstrap) with `posix_spawn` — no shell invocation.
- [x] AC-E.2: `src/gateway/cp_admin.c` validates `HUMAN_FIDELITY_AB_PATH` from `getenv` (rejects `..` and non-absolute paths).
- [x] AC-E.3: `src/gateway/cp_hula.c` validates `HU_HULA_TRACE_DIR` from `getenv` (rejects `..` and non-absolute paths, falls back to default).
- [x] AC-E.4: `src/tools/canvas_render.c` replaces `popen` with `hu_process_run_with_timeout` via explicit argv (Chrome + Mermaid paths).
- [x] AC-E.5: `src/tools/declarative.c` HTTP path replaced with in-process `hu_http_request()`; shell path replaced with `hu_process_run_with_timeout`.
- [x] AC-E.6: `cmake --build build` zero errors, zero warnings. 10120 tests PASS, 0 FAIL.

## Story F — Snapshot path hardening

- [x] AC-F.1: `src/memory/lifecycle/snapshot.c` upgraded from `path_has_traversal` (bare `..` substring check) to `hu_tool_validate_path()` (traversal + URL-encoded traversal + workspace scope).

## Story G — Fidelity schema alignment (AC-D.3 / AC-D.4)

- [x] AC-G.1: `cp_admin_metrics_fidelity` emits top-level `baseline_score` (from `baseline.mean`) and `delta` (from `ab.delta` or `null`).
- [x] AC-G.2: Both error and success paths emit the same schema.

## Pre-resolved threat model items (verified, no code changes needed)

- **C-04 (CRLF injection):** `header_value_safe()` in `http_request.c` already rejects `\r`/`\n`.
- **C-05 (git path traversal):** `hu_tool_validate_path()` + `sanitize_git_args()` already present.
- **H-08 (browser_open buffer):** `url_len < 9` guard already present in `browser_open.c`.
- **H-09 (snapshot traversal):** Now upgraded from `path_has_traversal` to `hu_tool_validate_path`.
- **H-11 (FTS5 injection):** Double-quote escaping already in both `sqlite.c` and `sqlite_fts.c`.
