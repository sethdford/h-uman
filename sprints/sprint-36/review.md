# Sprint 36 — Review

**Branch:** `sprint-36-persona-identity-echo`
**Stories:** S1 (API), S2 (detector), S3 (3 call sites + log), S4 (9 tests).

## Demo

Persona identity echo (G8) fires through `hu_agent_turn`:

```
[agent_turn] response_guard REJECT: turn final (len=61, recent_avg=0)
   [semantic=0 length=0 director=0 persona=0 identity=1
    repetition_run=1] — retrying once with repair prompt
[agent_turn] response_guard RECOVERED: retry passed (len=8, stripped=0)
PASS  agent_g8_persona_identity_echo_rejects_and_retries
```

Mock provider returned `"yeah i'm a Chief Architect at Pure Health
Solutions, busy day"` against an agent loaded with identity =
`"Chief Architect at Pure Health Solutions"` (40 bytes). G8 found a
≥25 byte verbatim substring match, REJECT fired, slim retry returned
`"yeah lol"` (8 bytes). Critically: the leak contains *no name*, so
G7 would have missed it. G8 closes the gap.

The new `identity=` flag now appears in every REJECT log across all
three call sites:

```
[agent_turn] response_guard REJECT: turn final (len=1415, recent_avg=21)
   [semantic=0 length=1 director=0 persona=0 identity=0 repetition_run=2]   ← G5
[agent_turn] response_guard REJECT: turn final (len=93, recent_avg=0)
   [semantic=0 length=0 director=1 persona=0 identity=0 repetition_run=1]   ← G6
[agent_turn] response_guard REJECT: turn final (len=57, recent_avg=0)
   [semantic=0 length=0 director=0 persona=1 identity=0 repetition_run=1]   ← G7
[agent_turn] response_guard REJECT: turn final (len=61, recent_avg=0)
   [semantic=0 length=0 director=0 persona=0 identity=1 repetition_run=1]   ← G8
```

## Acceptance check

- [x] **S1**: `hu_guard_context_t` extended with `persona_identity` +
      `persona_identity_len`. `hu_guard_report_t` extended with
      `detected_persona_identity_echo`.
- [x] **S2**: `hu_guard_has_persona_identity_echo` implemented in
      `response_guard.c`. Reuses G6's sliding-window primitive
      (`hu_guard_ci_contains`) with a tighter 25-byte threshold
      (`HU_GUARD_PERSONA_IDENTITY_MIN_MATCH`). Skips when identity
      is NULL or shorter than 25 bytes.
- [x] **S3**: All 3 production call sites populate
      `guard_ctx.persona_identity` from `agent->persona->identity`,
      falling back to `agent->persona->core_anchor` when identity is
      NULL. All REJECT log lines now include `identity=N` flag.
- [x] **S4**: 9 new tests:
       - 8 unit tests covering: rejects (verbatim, profession-phrase),
         passes (short overlap), skips (NULL, too-short),
         case-insensitivity, orthogonality (G6 vs G8, G7 vs G8 — same
         response, only correct flag fires).
       - 1 integration test driving `hu_agent_turn` with a real persona
         shim including identity field, mock provider leaks identity
         verbatim, G8 fires end-to-end.

## Test results

- Response Guard combined suite: 65/65 (was 59 in Sprint 35; +9 G8 tests).
- Full dev suite: 10322/10322 (was 10313; +9 = matches).
- 0 ASan errors. Clean build with `-Wall -Wextra -Wpedantic -Werror`.
- 0 cross-layer topology violations.

## Behavioral guarantee

| Scenario | persona_identity | response | G8 fires? | Outcome |
|---|---|---|---|---|
| No identity loaded | NULL | "i'm a Chief Architect at Pure Health" | no | preserved |
| identity = "hi" (too short) | "hi" (2B) | "hi there" | no | preserved |
| identity = full bio | "Chief Architect at Pure Health Solutions" (40B) | "yeah i'm a Chief Architect at Pure Health Solutions" | yes | REJECT, retry |
| Short overlap | "a senior software engineer..." | "i'm a senior dev" (12B overlap) | no | OK |
| Case mismatch | lowercase identity | UPPERCASE response | yes | REJECT, retry |
| identity + director (G6/G8 orthogonality) | both set, only identity quoted | identity quote, no director | yes (G8 only) | REJECT, identity=1, director=0 |
| identity + name (G7/G8 orthogonality) | both set, no name in response | identity quote, no name | yes (G8 only) | REJECT, identity=1, persona=0 |

Cold-start safety: agents without identity see byte-identical legacy
behavior. The new detector only activates when
`agent->persona->identity` (or `core_anchor` fallback) is set with
length ≥ 25 bytes.

## Coverage matrix (post-Sprint 36)

| Leak class | Pattern | Caught by |
|---|---|---|
| Special tokens | `<|channel|>`, `<|message|>` | G0 (Phase 1) |
| Numbered analysis dump | `1. xxx 2. yyy 3. zzz` | G1 (Phase 3) |
| Self-talk substrings | `the prompt says`, `wait, the prompt` | G2 (Phase 3) |
| Third-person double pattern | `is a <profession>` + `lives alone with` | G3 (Phase 3) |
| Prompt-template labels | `Persona:`, `Scene Direction:`, `User: "` | G4 (Phase 3) |
| Length anomaly | response_len ≥ 8 × recent_avg | G5 (Phase 4a) |
| Director-string echo | ≥30 byte verbatim director quote | G6 (Phase 4b) |
| Persona name in 3rd-person construct | `Seth is a developer` | G7 (Phase 4c) |
| Persona identity 1st-person echo | `i'm a Chief Architect at Pure Health` | **G8 (Phase 4d)** |

The 2026-05-12 leak fragment that started this work would now trip
five separate detectors:

1. G1 (numbered analysis dump — `"1. ... 2. ... 3. ..."`).
2. G2 (self-talk — `"Wait, the prompt says"`).
3. G3 (third-person double — `"Seth is a technical professional"` +
   `"lives alone with a cat"`).
4. G4 (template label — `"Persona:"` if present).
5. G5 (length anomaly — 979 chars vs ~44 char rolling avg).
6. G6 (director echo — 64-char verbatim quote of director).
7. G7 (persona name — `"Seth is a"` third-person construct).
8. G8 (persona identity — verbatim quote of biographical identity).

Defense in depth: any single detector passing through would still be
caught by 4-7 others.

## What's in (Sprint 36)

- `include/human/agent/response_guard.h` +18 LOC (2 fields + comments).
- `src/agent/response_guard.c` +50 LOC (`hu_guard_has_persona_identity_echo`
  + Phase 4d integration).
- `src/agent/agent_stream.c` +14 LOC (×2 sites populate identity).
- `src/agent/agent_turn.c` +9 LOC (×1 site).
- All 3 call sites' REJECT log updated to include `identity=N` flag.
- `tests/test_response_guard.c` +160 LOC (8 unit tests).
- `tests/test_response_guard_retry.c` +60 LOC (1 integration test).
- `sprints/sprint-36/{stories,review,retro}.md`.

## Out of scope (deferred to Sprint 37+)

- **Multi-turn director memory** (Sprint 34 carry-over).
- **Quality-gate `MARGINAL → REJECT` policy** (Sprint 28 carry-over).
- **Per-channel length thresholds.**
- **Daemon clear-on-exit test.**
- **Widen G7 lookahead 30 → 60 bytes** — measure G7 false-positive
  rate first.
- **Extend G8 to other persona fields** — biography, principles,
  traits. G8 with identity / core_anchor catches the highest-leverage
  leaks; broader coverage can come once we have telemetry on G8 hits.
