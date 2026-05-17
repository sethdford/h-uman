# Sprint 34 — Retrospective

## What went well

- **Both new tests fired live.** The G5 integration test seeded
  history with two short replies, third call returned 1415 bytes,
  guard REJECTed, retry recovered with a 20-byte clean reply. The G6
  test set `scene_direction_text`, mock echoed it verbatim, guard
  REJECTed with `director=1` flag set. The log line evidence is the
  best proof of wiring health — better than any unit-test assertion.
- **Field-assignment over helper API.** I started with `_set_/_clear_`
  helpers in `agent_internal.h` but realized the daemon doesn't have
  that header on its include path. Switched to direct field
  assignment in daemon.c. Three lines instead of seven, no API
  surface bloat. The helpers stayed declared (they exist as a clean
  test API if needed later) but are unused in production. Honest
  trade: pragmatism > consistency.
- **Stack-lifetime safety.** The clear-on-exit pattern prevents
  reading freed stack memory: `director_result.direction[512]` is a
  stack array inside a daemon turn-handling block, and the agent's
  `scene_direction_text` is a non-owning pointer to it. If a
  subsequent turn fired without the daemon setting a new direction,
  the guard would have read stale stack bytes. Clear-on-exit makes
  the lifetime explicit and audit-able. Documented in both daemon.c
  (the call site) and agent.h (the field comment).
- **Test fixture pattern reused.** The `length_provider_ctx_t` mock
  with per-call text array is cleaner than the existing
  `retry_provider` mock (which hardcodes case 1 / case 2). Future
  guard regression tests can reuse this pattern with different
  call sequences.
- **Conservative cold-start.** When `scene_direction_text == NULL`,
  the guard's G6 check sees `director_text=NULL` and skips. So an
  agent that runs without the daemon (CLI, tests not setting the
  field) sees byte-identical legacy behavior. No false positives.

## What didn't go well

- **G6 only fires on the *current* turn's directive.** If turn N's
  director said "casual short" and turn N+1 has a different director,
  a model that echoes "casual short" in turn N+1 won't trip G6. This
  is structurally correct (the field is non-owning per turn) but is
  a real gap — the leaks we found in Sprint 32 included some that
  echoed the previous turn's prompt structure. Future sprint should
  add a small ring buffer of recent director strings.
- **No test for the daemon's clear-on-exit.** The clear is critical
  for stack-lifetime safety but I have no test for it — only the
  field-set test. If someone removes the clear, the agent path will
  still pass (because each test sets `scene_direction_text` per turn)
  but production could read freed memory. ASan would catch it but
  only when the leak actually fired. Documenting it carefully in
  the comments is the next-best mitigation.
- **G6 threshold is byte count, not semantic.** The 30-byte verbatim
  match threshold is a structural signal — but a determined model
  could rephrase the directive ("casual short" → "be brief and
  informal") and slip past. The persona-derived detector and
  semantic-similarity check are the principled fixes; G6 is the
  cheap one.

## Action items for Sprint 35

1. **Multi-turn director memory.** Add `agent->scene_direction_history`
   ring (4-8 entries) so G6 catches verbatim quotes of *recent*
   directors, not just the current one. Daemon pushes onto the
   ring at set time.
2. **Decide on the Sprint 28 quality-gate carry-over.** With G5+G6
   wired, run `human-daemon` for a week, look at `response_guard
   REJECT` log frequency, decide whether the reflection layer also
   needs MARGINAL→REJECT at 5× length. Default: don't add it unless
   measurement shows leaks slipping through.
3. **Persona-derived detector.** Read loaded `hu_persona_t.identity`
   fields (name, age, profession) and add them to the guard context
   as a synthetic director text. Catches the "Seth is a 51yo..."
   third-person echoes that the structural G3 only partly catches.
4. **Test the daemon clear-on-exit.** Add a test that runs two turns
   in a row through `hu_agent_turn`; on the second turn, the daemon
   does NOT set scene_direction; the guard sees `director_text=NULL`,
   not stale memory. Probably easier to write as a daemon-level
   integration test than a unit test.

## Sprint metrics

| Metric | Value |
|---|---|
| Stories shipped | 4/4 |
| Production call sites updated | 3 (guard) + 2 (daemon set/clear) |
| Public API additions | 2 fields on `hu_agent_t` |
| Internal helpers added | 2 (set / clear; unused in prod, kept for tests) |
| New integration tests | 2 (G5, G6 end-to-end) |
| Total dev suite | 10303 (was 10301; +2 = matches) |
| Total guard-retry suite | 5 (was 3) |
| Lines added (production) | ~40 |
| Lines added (tests) | ~185 |
| Sprint duration | < 1 day |
| Behavioral regression | 0 (cold-start preserves legacy path) |
