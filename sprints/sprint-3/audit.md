# Sprint 3 Audit — Validator Chain Hardening

Scope: P0 stories US-1, US-2, US-3. Auditor re-derived each AC from `stories.md` independently, then verified against `c890790e..HEAD` (7 commits, +673/-33 LOC). Full suite: 10312/10312 passed.

## Per-AC verdict

### US-1 — Wire structured-output per persona opt-in
- **AC1 DELIVERED.** `bool structured_output_enabled` declared at `include/human/persona.h:448`, populated from JSON at `src/persona/persona.c:1215`. Test fixture sets it true at `tests/test_structured_output.c:186`.
- **AC2 DELIVERED.** `src/agent/agent_turn.c:4091-4096` sets `req.response_format = "json_schema"` and `req.response_schema` when persona opts in, before the chat vtable call at line 4098+. Asserted via capturing mock provider in `persona_opt_in_sets_request_fields_via_agent_turn` (lines 168-206).
- **AC3 DELIVERED.** `persona_opt_out_leaves_request_fields_null_via_agent_turn` (line 210) pre-poisons `captured_response_format` to `0xdeadbeef` and asserts it is NULL after the call — a genuine guard that the field was actively written, not just defaulted. `null_persona_leaves_request_fields_null_via_agent_turn` (line 247) covers NULL persona. No regression in full suite.
- **AC4 DELIVERED.** Three end-to-end tests exercise `hu_agent_turn` with a real mock-provider vtable that captures `hu_chat_request_t`. This is production-path testing, not inlined conditionals. Note: `agent_stream.c:1317` has parallel wiring but no equivalent capture test — minor gap, structurally identical code.
- **DoD PASS.** `structured_output` suite: 9/9 pass. Full build clean. Full suite 10312/10312.

### US-2 — Pattern C escape paths
- **AC1 DELIVERED.** `src/daemon.c:2095-2122` runs `hu_output_validator_chain_execute` in the `HU_AGENT_STREAM_TEXT` branch. Legacy `hu_conversation_strip_channel_tags` calls at lines 2127 and 2137 are **fallback** paths (chain-build failure, missing allocator) — not the primary path. Net result: the primary path is the chain; legacy is degraded-mode only. AC text says "no direct call to ... remains in that branch" — this is **DRIFT against the DoD grep** (`grep ... | wc -l` returns 2), but the *intent* (Pattern C closed in the live path) is satisfied. Implementer comments mark them as MED #5 / fallback. Flag as concern, not failure.
- **AC2 DELIVERED.** `src/channels/format.c` runs the chain for all four Tier-1 channels: discord/telegram (line 595), slack (647), imessage (710). Email is intentionally excluded (HTML formatter, not a messaging channel).
- **AC3 DELIVERED.** `tests/test_pattern_c_paths.c` covers iMessage (lines 78-112), Discord (136), Telegram (154), Slack (172), and a clean-message regression guard (190). All exercise the public `hu_channel_format_outbound` API.
- **AC3 PARTIAL — Site A.** `site_a_chain_strips_assistant_closer` (line 25) **does not exercise production code.** Its own comment (lines 5-7) admits: the daemon stream callback is static and cannot be called directly, so the test rebuilds the same chain inline. Deleting the production call in `daemon.c` would not fail this test. This is the "test inlines production code" anti-pattern the auditor was specifically told to find. Counter-evidence: the chain-execute construction in daemon.c is read-by-grep and the chain itself is heavily covered elsewhere. Mitigation needed: an integration test that drives the daemon callback (deferred — likely US-6).
- **AC4 DELIVERED.** Full suite 10312/10312, ASan dev preset clean.
- **DoD CONCERN.** DoD line 51 (`grep -n "hu_conversation_strip_channel_tags|..." src/daemon.c` zero hits) is **not** zero — 2 hits remain in fallback arms (lines 2127, 2137). Implementer's CRITICAL #1 fix narrative explicitly documented this as "MED #5 fallback so messages aren't published unfiltered". This is sensible defense-in-depth, but the DoD line as written is violated. PO should sign off on the fallback explicitly, or AC1/DoD should be rewritten.

### US-3 — Remove dead `channel` param
- **AC1 DELIVERED.** `include/human/agent/stop_sequence_registry.h:25` signature now reads `(provider, provider_len, out_seqs, out_count)` — no `channel` params. Header comment (lines 15-17) documents deferral.
- **AC2 N/A** (remove path chosen, not wire path).
- **AC3 DELIVERED.** `grep "(void)channel" src/agent/stop_sequence_registry.c` returns empty.
- **AC4 DELIVERED.** Both call sites (`agent_turn.c:4087`, `agent_stream.c:1312`) updated to new signature. `stop_sequences` suite 8/8 pass.
- **DoD PASS.**

## Independent findings (beyond critic)

1. **Site A test is a self-reference, not a production-path test.** Acknowledged in comments but should be tracked as test debt; the daemon static callback needs a seam (function pointer or extraction) so US-6's E2E can cover it.
2. **`agent_stream.c:1317` structured-output wiring has no dedicated capture test.** Identical to `agent_turn.c:4091` but only the turn path is asserted end-to-end.
3. **DoD literalism.** US-2's DoD `grep ... zero hits` is empirically false; AC intent is satisfied, but the DoD line and the code disagree. This is a process gap — either tighten the code or relax the DoD; do not leave them inconsistent.
4. **No scope creep detected.** Each commit traces to a single story (US-1, US-2, US-3) or to a named critic finding. No drive-by edits outside the three P0 stories.
5. **PR #81 backward compat verified.** Full suite 10312 includes pre-sprint validator suites; all green.

## Overall verdict

Three P0 stories are functionally delivered. The only material defect is US-2 Site A's inline-rebuild test and the DoD-grep mismatch on legacy-stripper fallbacks. Both are documented, not hidden, and neither blocks the sprint's safety intent.

RESULT_sprint-auditor=PASS_WITH_NOTES
