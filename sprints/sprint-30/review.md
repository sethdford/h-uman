# Sprint 30 — review

## Goal

Catch the prompt-template label leak class that Sprint 29 missed,
discovered via a chat.db audit run after Sprint 29 closed.

## What was discovered

Audited 367 outbound iMessage attributedBody payloads since
2026-05-10. Found **4 leaks total**, not 1:

| ROWID | When | To | Bytes | S29 caught? | Class |
| --- | --- | --- | --- | --- | --- |
| 56354 | 2026-05-12 17:04:37 | +14848158444 (Brea) | 1908 | ✅ G1+G2+G3 | CoT dump |
| **56355** | **2026-05-12 17:07:38** | **+18012017497** | **1858** | ✅ G1+G2+G3 | **CoT dump (same content, different recipient — sent 3 min after Brea leak)** |
| **56055** | **2026-05-11 00:35:13** | **+13857220896** | **1097** | ❌ G3=1 only | **Prompt-template** |
| **56065** | **2026-05-11 00:45:21** | **+13857220896** | **2208** | ❌ G3=1 only | **Prompt-template** |

The May-11 leaks contained literal prompt-template labels
(`User:`, `Context:`, `Persona:`, `Scene Direction:`, `Rules:`,
`Constraints:`) plus candidate response drafts dumped verbatim
to the recipient.

## Stories shipped

- ✅ **G4** prompt-template label substrings added to
  `hu_guard_has_self_talk_pattern` (12 new patterns: each of 6
  template labels in both `\nLabel:` and ` Label:` forms).
- ✅ Did NOT hardcode persona PII — relied on structural patterns.
  The 4 audit leaks each match 4+ structural patterns, so no
  PII pinning is needed for coverage.

## Tests added

| Test | Asserts |
| --- | --- |
| `guard_rejects_msg_56355_second_brea_leak_to_other_recipient` | The 17:07:38 second-recipient variant of the Brea CoT dump → REJECT (verifies S29 detectors still fire on identical content sent to a different contact). |
| `guard_rejects_msg_56055_persona_block_leak_verbatim` | The 2026-05-11 00:35:13 persona-block leak (verbatim, ~700 bytes of text content) → REJECT. |
| `guard_rejects_msg_56065_persona_block_leak_verbatim` | The 2026-05-11 00:45:21 persona-block leak (verbatim, ~750 bytes) → REJECT. |
| `guard_rejects_template_label_substrings` | Six sub-cases: each template-label substring in isolation → REJECT. |

## Files touched

| File | Change |
| --- | --- |
| `src/agent/response_guard.c` | +12 G4 patterns (template-label substrings), +9 lines of comments |
| `tests/test_response_guard.c` | +4 tests (3 verbatim audit-leak fixtures + 1 isolation matrix) |
| `sprints/sprint-30/{stories,review,retro}.md` | New |

## Verification

| Build | Result |
| --- | --- |
| `cmake --preset dev` | **10290 / 10290 passed**, 0 ASan errors. |
| `cmake --preset minimal` | **8830 / 8830 passed**. |

Response Guard suite: **33 / 33 passed** (was 29 at v-sprint-29-close,
+4 from Sprint 30).

## Out of scope (deferred)

- **Length-anomaly hard-block** (Sprint 31). Threading
  `recent_avg_len` through the guard's public API.
- **Director-string echo detection** (Sprint 31). Same API-change
  scope.
- **Quality gate `MARGINAL → REJECT` policy fix** (Sprint 32).
  Separate file (`agent_stream_v2.c`).
- **Persona-derived dynamic detector** (future). Reading the
  loaded persona's biographical fields and rejecting verbatim
  quotes — generalizes to every user's deployment without
  hardcoding PII into source.
- **Post-mortem doc** (after Sprint 32). Will reference all 4
  audit leaks + the layered fix.
- **Daemon restart** (operational; user's call).

## Operational follow-up

1. Cherry-pick Sprint 30 onto main (`rl-sota-phase-5`).
2. Restart the live `human-daemon service-loop`. Until restarted,
   the running daemon still has Sprint 29's detector but not
   Sprint 30's — May-11-style template-label leaks would still
   slip through.
3. Continue to Sprint 31 (length anomaly + director echo).
