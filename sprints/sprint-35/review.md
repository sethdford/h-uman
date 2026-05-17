# Sprint 35 — Review

**Branch:** `sprint-35-persona-derived-detector`
**Stories:** S1 (API), S2 (detector), S3 (3 call sites + log), S4 (10 tests).

## Demo

Persona-PII echo (G7) fires through `hu_agent_turn`:

```
[agent_turn] response_guard REJECT: turn final
   (len=57, recent_avg=0)
   [semantic=0 length=0 director=0 persona=1 repetition_run=1] —
   retrying once with repair prompt
[agent_turn] response_guard RECOVERED: retry passed (len=8, stripped=0)
PASS  agent_g7_persona_pii_echo_rejects_and_retries
```

The provider mock returned `"yeah testname is a software developer who
loves dry humor"` — the agent's loaded persona has `name="testname"`,
G7 matched the `"testname is a"` third-person construct, REJECT fired,
slim retry returned `"yeah lol"` (8 bytes).

The new `persona=` field appears in *every* REJECT log line across all
three call sites — including for the existing G5/G6 tests where it is
0, proving the new code path is live and reporting cleanly:

```
[agent_turn] response_guard REJECT: turn final (len=1415, recent_avg=21)
   [semantic=0 length=1 director=0 persona=0 repetition_run=2]   ← G5
[agent_turn] response_guard REJECT: turn final (len=93, recent_avg=0)
   [semantic=0 length=0 director=1 persona=0 repetition_run=1]   ← G6
[agent_turn] response_guard REJECT: turn final (len=57, recent_avg=0)
   [semantic=0 length=0 director=0 persona=1 repetition_run=1]   ← G7
```

## Acceptance check

- [x] **S1**: `hu_guard_context_t` extended with `persona_name` +
      `persona_name_len`. `hu_guard_report_t` extended with
      `detected_persona_pii_echo`.
- [x] **S2**: `hu_guard_has_persona_pii_echo` implemented in
      `response_guard.c`. Word-boundary aware (start-of-string or
      non-letter required). Case-insensitive. Skips when name is
      NULL or `< 2` or `> 64` bytes. Patterns: `" is a"`, `" is an"`,
      `" is the"`, `" is currently"`, `" was a/an/the"`,
      `" has a/an"`, `" has been"`, `" lives"`, `" lives in"`,
      `" lives alone"`, `" works"`, `" works at/as"`, `" said"`,
      `" would"`, `" prefers"`, `" enjoys"`, `" likes"`, possessive
      `"'s "`. Allows up to 30 bytes of slack between name and
      construct (covers `"Seth, 51, is a..."`).
- [x] **S3**: All 3 production call sites populate
      `guard_ctx.persona_name` from `agent->persona->name` when set.
      All REJECT log lines now include `persona=N` flag.
- [x] **S4**: 10 new tests:
       - 9 unit tests covering: rejects (3 patterns × ≥1 case),
         passes (first-person, direct address), skips (NULL,
         too-short), word-boundary isolation, case-insensitivity.
       - 1 integration test driving `hu_agent_turn` with a synthetic
         persona shim and a mock provider that leaks the third-person
         construct.

## Test results

- Response Guard suite: 53/53 (was 44 in Sprint 34; +9 G7 unit tests).
- Response Guard Retry suite: 6/6 (was 5; +1 G7 integration test).
- Full dev suite: 10313/10313 (was 10303; +10 = matches).
- 0 ASan errors. Clean build with `-Wall -Wextra -Wpedantic -Werror`.
- 0 cross-layer topology violations.
- All 4 touched source files compile clean against `h-uman` main with
  strict flags.

## Behavioral guarantee

| Scenario | persona_name | response | G7 fires? | Outcome |
|---|---|---|---|---|
| No persona loaded | NULL | "Seth is a developer" | no | preserved |
| Single-letter persona | "S" | "S is a developer" | no | preserved |
| Persona "Seth", first person | "Seth" | "i'm seth" | no | OK |
| Persona "Seth", direct address | "Seth" | "hey seth thanks" | no | OK |
| Persona "Seth", embedded | "Seth" | "Bethseth is great" | no | OK (boundary) |
| Persona "Seth", third person | "Seth" | "Seth is a developer" | yes | REJECT, retry |
| Persona "Seth", possessive | "Seth" | "Seth's job is wild" | yes | REJECT, retry |
| Persona "Seth", verb | "Seth" | "Seth lives in SLC" | yes | REJECT, retry |
| Case variations | "Seth" | "SETH is a developer" | yes | REJECT, retry |

Cold-start safety: agents without a loaded persona (CLI, tests not
setting `agent.persona`) see byte-identical legacy behavior. The new
detector only activates when the agent has `persona->name` set with
`name_len >= 2`.

## What's in (Sprint 35)

- `include/human/agent/response_guard.h` +14 LOC (2 new fields with
  doc comments).
- `src/agent/response_guard.c` +110 LOC (`hu_guard_persona_pii_construct_at`,
  `hu_guard_has_persona_pii_echo`, Phase 4c integration).
- `src/agent/agent_stream.c` +8 LOC (2 sites populate persona_name).
- `src/agent/agent_turn.c` +5 LOC (1 site).
- All 3 call sites updated REJECT log to include `persona=N` flag.
- `tests/test_response_guard.c` +200 LOC (9 unit tests).
- `tests/test_response_guard_retry.c` +60 LOC (1 integration test).
- `sprints/sprint-35/{stories,review,retro}.md`.

## Out of scope (deferred)

- **Multi-turn director memory** (carry-over from Sprint 34).
- **Quality-gate `MARGINAL → REJECT` policy** (Sprint 28 carry-over).
- **Per-channel length thresholds.**
- **Other persona fields** (age, profession, title, biography) —
  Sprint 36 candidate. Name alone closes the highest-leverage leak
  class. Adding profession/title would expand coverage from third-
  person *name* echoes to *persona snippet* echoes (e.g., model
  saying "i'm a Chief Architect" without using the name).
- **Daemon clear-on-exit test** for scene_direction_text.
- **CI/cron schedule for `scripts/audit-imessage-leaks.sh`**.
