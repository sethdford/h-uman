# Sprint 36 — Persona identity / core-anchor echo (G8)

**Branch:** `sprint-36-persona-identity-echo`
**Sprint goal:** close the leak class that G7 cannot catch — model
output that echoes verbatim chunks of the persona's `identity` or
`core_anchor` string *without using the name*. Examples from the
audit and the original 2026-05-12 leak fragment:

- `"i'm a Chief Architect at Pure Health Solutions"` (no name).
- `"yeah I'm a 51-year-old technical professional, lives alone"`.
- `"speaking professionally, lives alone with a cat"`.
- `"He's talking to Brea (romantic interest, casual, early stage)"`.

G7 (Sprint 35) requires the persona name in a third-person
construct. None of the above contain `"Seth ..."` — they're
first-person echoes of the operator's loaded biographical metadata.
G8 closes this gap.

## Background

Sprint 35 retro:

> "Persona name only covers part of the surface. The leaks also
>  contain profession ('Chief Architect'), age ('51'), and
>  biography fragments. G7 catches `<Name> is a Chief Architect`
>  because of the name, but a leak that says `i'm 51, Chief
>  Architect, lives alone` (no name) would slip past."

G6 (Sprint 31) already has the right primitive: sliding-window
verbatim substring match against a context-supplied string. G6
matches the *director* (per-turn scene-direction text). G8 adds
a parallel match against the *persona identity* — a stable
per-agent string that doesn't change across turns.

## Design

### Detector phase

Add Phase 4d (G8) right after Phase 4c (G7):

```
hu_guard_has_persona_identity_echo(ctx, response, response_len)
```

Returns true if `ctx->persona_identity` is non-NULL with
`persona_identity_len >= HU_GUARD_PERSONA_IDENTITY_MIN_MATCH` and
any contiguous 25-byte substring of `persona_identity` appears
verbatim (case-insensitively) in `response`.

The 25-byte threshold is calibrated against:

- **G6 director-echo threshold = 30 bytes.** Director text is
  per-turn and short (e.g. `"casual short, dry"` = 17 bytes; below
  threshold). Persona identity is per-agent and longer (e.g.
  `"Chief Architect at Pure Health Solutions"` = 40 bytes, well
  above).
- **Common-phrase floor.** Generic English phrases hit 15-20 chars
  ("i think we should", "thanks for the message"). 25 chars is
  long enough that incidental overlap is rare.
- **Audit data.** All identity-echo leaks in `chat.db` quoted ≥30
  contiguous chars of identity. 25 catches them with margin.

### API

Extend `hu_guard_context_t`:

```c
typedef struct {
    /* ... existing fields ... */

    /* Sprint 36: loaded persona's biographical identity string
     * (e.g. "51-year-old technical professional, lives alone with
     *  a cat"). Verbatim 25+ byte substring match → REJECT.
     * NULL or persona_identity_len < HU_GUARD_PERSONA_IDENTITY_MIN_MATCH
     * disables the check. */
    const char *persona_identity;
    size_t persona_identity_len;
} hu_guard_context_t;
```

Extend `hu_guard_report_t`:

```c
bool detected_persona_identity_echo;
```

### Wiring

Production call sites populate `persona_identity` from the loaded
persona, falling back to `core_anchor` if `identity` is empty:

```c
if (agent->persona) {
    if (agent->persona->identity) {
        guard_ctx.persona_identity = agent->persona->identity;
        guard_ctx.persona_identity_len = strlen(agent->persona->identity);
    } else if (agent->persona->core_anchor) {
        guard_ctx.persona_identity = agent->persona->core_anchor;
        guard_ctx.persona_identity_len = strlen(agent->persona->core_anchor);
    }
}
```

The new REJECT log emits the new flag:

```
response_guard REJECT: turn final (len=N, recent_avg=M)
  [semantic=0 length=0 director=0 persona=1 identity=1
   repetition_run=K] — retrying once with repair prompt
```

## Stories

### S1 — Extend `hu_guard_context_t` and `hu_guard_report_t`

- New `persona_identity` + `persona_identity_len` on context.
- New `detected_persona_identity_echo` on report.

### S2 — Implement `hu_guard_has_persona_identity_echo`

- Reuse the same sliding-window primitive as G6 director echo
  (`hu_guard_ci_contains` for case-insensitive substring search).
- 25-byte minimum match (`HU_GUARD_PERSONA_IDENTITY_MIN_MATCH`).
- Slide a 25-byte window across `persona_identity`; for each
  window position, check if the response contains it.
- Skip when `persona_identity` is NULL or
  `persona_identity_len < 25`.

### S3 — Wire into all 3 call sites

- `agent_stream.c` (×2), `agent_turn.c` (×1) populate
  `guard_ctx.persona_identity` from `agent->persona->identity` or
  fall back to `agent->persona->core_anchor`.
- All 3 REJECT log lines include `identity=N` flag.

### S4 — Tests

Unit tests in `test_response_guard.c`:

- `guard_g8_rejects_verbatim_identity_quote` — identity =
  `"51-year-old technical professional, lives alone with a cat"`,
  response contains `"technical professional, lives alone"` (35 chars
  contiguous) → REJECT.
- `guard_g8_rejects_chief_architect_phrase` — identity =
  `"Chief Architect at Pure Health Solutions"`, response =
  `"yeah i'm a Chief Architect at Pure Health Solutions, busy day"`
  → REJECT.
- `guard_g8_passes_short_overlap` — identity =
  `"a senior software engineer at the company"`, response =
  `"i'm a senior dev"` (only 15 chars overlap, "i'm a senior") →
  no fire.
- `guard_g8_passes_when_identity_null` — identity = NULL → no enforcement.
- `guard_g8_passes_when_identity_too_short` — identity = `"hi"`
  (2 chars) → no enforcement.
- `guard_g8_case_insensitive` — identity =
  `"chief architect at pure health"`, response =
  `"CHIEF ARCHITECT AT PURE HEALTH"` → REJECT.
- `guard_g8_orthogonal_to_g6` — both director_text and
  persona_identity set; only G8 fires (no director quote).
- `guard_g8_orthogonal_to_g7` — both persona_name and
  persona_identity set; response contains 30 chars of identity
  but not the name → only G8 fires.

Integration test in `test_response_guard_retry.c`:

- `agent_g8_persona_identity_echo_rejects_and_retries` — agent
  with persona shim (name + identity), mock returns first-person
  identity echo, G8 fires, retry succeeds.

## Definition of Done

- All 4 stories shipped.
- Dev build clean with `-Wall -Wextra -Wpedantic -Werror`.
- Full dev test suite passes 10313 + N (N ≥ 9), 0 ASan errors.
- Suite logs visibly emit `[identity=1]` on the new tests.
- Branch tagged `v-sprint-36-close` and cherry-picked to `h-uman` main.

## Out of scope (deferred)

- **Multi-turn director memory** (Sprint 34 carry-over).
- **Quality-gate `MARGINAL → REJECT` policy** (Sprint 28 carry-over).
- **Per-channel length thresholds.**
- **Daemon clear-on-exit test.**
- **Other persona fields** (biography, principles, traits) — start
  with identity / core_anchor; expand if needed.
- **Widen G7 lookahead 30 → 60 bytes** — measure G7 false-positive
  rate first, defer to Sprint 37.
