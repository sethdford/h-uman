# Sprint 35 — Persona-derived dynamic detector (G7)

**Branch:** `sprint-35-persona-derived-detector`
**Sprint goal:** add a runtime detector that rejects model output
echoing the loaded persona's *name* in third-person profile
constructs. This catches the leak class that dominated the 2026-05-11
incidents (`"Seth is a technical professional"`, `"Seth Douglas Ford,
51, Chief Architect"`) without hardcoding any individual operator's
PII into the codebase.

## Background

End of Sprint 34 retro:

> "G6 threshold is byte count, not semantic. A determined model
>  could rephrase the directive ('casual short' → 'be brief and
>  informal') and slip past."
>
> "Persona-derived detector: read loaded persona fields and add to
>  the guard context as a synthetic director text. Catches the
>  third-person-about-user echoes that G3 only partly catches."

Sprint 30 explicitly removed hardcoded PII patterns from G3 in favor
of structural signatures — open-source-publishable. Sprint 35 closes
the resulting gap by reading the *runtime-loaded* persona's name and
matching it dynamically. No PII in source; persona-PII in memory only,
where it belongs.

## Design

### Detector phase

Add Phase 4c (G7) after Phase 4b (G6 director echo):

```
hu_guard_has_persona_pii_echo(response, response_len, persona_name, persona_name_len)
```

Returns true if `persona_name` (case-insensitive, ≥ 2 chars, ≤ 64 chars)
appears in the response followed within 30 bytes by a third-person
profile construct:

- `<Name> is `
- `<Name> was `
- `<Name>'s ` (possessive)
- `<Name> has `
- `<Name> lives `
- `<Name> works `
- `<Name> said `
- `<Name> would `

This is the "third-person about user" leak signature. It is *not*
tripped by:

- First-person: `"i'm Seth"`, `"this is Seth"`, `"Seth here"`.
- Direct address: `"hey Seth"`, `"thanks Seth"`, `"yo Seth"`.
- Single-name mentions: `"Seth"` alone, `"Seth!"`, `"Seth?"`.

### API

Extend `hu_guard_context_t`:

```c
typedef struct {
    size_t recent_avg_len;
    const char *director_text;
    size_t director_len;
    /* Sprint 35: persona name for G7 PII-echo detection. NULL/0 means
     * "no persona loaded, do not enforce G7". */
    const char *persona_name;
    size_t persona_name_len;
} hu_guard_context_t;
```

Extend `hu_guard_report_t`:

```c
bool detected_persona_pii_echo;
```

### Wiring

Production call sites (`agent_stream.c` ×2, `agent_turn.c` ×1) read
`agent->persona->name` at guard call time and pass it via the context.
The persona is owned by the agent, lifetime-stable for the duration
of the turn — no daemon plumbing needed.

The new REJECT log emits the new flag:

```
response_guard REJECT: turn final (len=N, recent_avg=M)
  [semantic=0 length=0 director=0 persona=1 repetition_run=K] —
  retrying once with repair prompt
```

## Stories

### S1 — Extend `hu_guard_context_t` and `hu_guard_report_t`

- New field `persona_name` + `persona_name_len` on context (NULL/0 default).
- New field `detected_persona_pii_echo` on report.
- `hu_response_guard_check_ex` Phase 4c calls
  `hu_guard_has_persona_pii_echo(...)`; if true → `HU_GUARD_REJECT`,
  `report.detected_persona_pii_echo = true`.

### S2 — Implement `hu_guard_has_persona_pii_echo`

- Case-insensitive match for `persona_name`.
- Reject `persona_name_len < 2` (prevents single-letter false positives).
- Reject `persona_name_len > 64` (sanity bound).
- For each occurrence of the name, look ahead up to 30 bytes for
  one of the third-person constructs listed above.
- Word-boundary aware: must be preceded by start-of-string or
  non-letter, otherwise "Seth" would match inside "Bethseth" or
  similar substrings.

### S3 — Wire into all 3 call sites

- `agent_stream.c:1404`, `agent_stream.c:2146`, `agent_turn.c:5593`
  populate `guard_ctx.persona_name` and `_len` from
  `agent->persona ? agent->persona->name : NULL` /
  `agent->persona ? agent->persona->name_len : 0`.
- REJECT log line includes `persona=0/1` flag.

### S4 — Tests

Unit tests in `test_response_guard.c` covering:

- `guard_g7_rejects_third_person_is_construct` — "Seth" + " is a " in close proximity.
- `guard_g7_rejects_third_person_possessive` — "Seth's job"
- `guard_g7_rejects_third_person_lives_works` — "Seth lives", "Seth works"
- `guard_g7_passes_first_person_self_reference` — "i'm seth", "this is seth"
- `guard_g7_passes_direct_address` — "hey seth", "thanks seth", "yo seth"
- `guard_g7_passes_short_name_alone` — "seth!" alone
- `guard_g7_skips_when_persona_name_null` — NULL persona_name → no enforcement
- `guard_g7_skips_when_persona_name_too_short` — < 2 chars → no enforcement
- `guard_g7_word_boundary_isolates_name` — "Bethseth" must not match "Seth"
- `guard_g7_case_insensitive` — "SETH is a" same as "seth is a"

Integration test in `test_response_guard_retry.c`:

- `agent_g7_persona_pii_echo_rejects_and_retries` — agent loaded with
  a tiny persona (name="testname"), mock returns "testname is a
  software developer", G7 fires, retry returns clean reply.

## Definition of Done

- All 4 stories shipped.
- Dev build clean with `-Wall -Wextra -Wpedantic -Werror`.
- Full dev test suite passes 10303 + N (N ≥ 10), 0 ASan errors.
- Suite logs visibly emit `[persona=1]` on the new tests.
- Branch tagged `v-sprint-35-close` and cherry-picked to `h-uman` main.

## Out of scope (deferred)

- **Multi-turn director memory** (carry-over from Sprint 34).
- **Quality-gate `MARGINAL → REJECT` policy** (Sprint 28 carry-over).
- **Per-channel length thresholds.**
- **Other persona fields** (age, profession, title, biography) —
  start with name only; expand if name alone proves insufficient.
- **Daemon clear-on-exit test** for scene_direction_text.
