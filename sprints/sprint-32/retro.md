# Sprint 32 — Retrospective

## What went well

- **Audit-first paid out a third time.** Sprint 29 audit found 1 leak,
  Sprint 30 audit found 2 more, Sprint 32 audit found 2 more.
  Same tool, same `chat.db`, three rounds of new findings. The lesson
  is: the audit is not a one-time thing; it's a permanent watchdog.
  Promoting it to `scripts/audit-imessage-leaks.sh` removes the
  "throwaway shell script" friction.
- **Verbatim regression fixtures are dirt cheap and high-value.**
  Adding 2 new leaks took ~30 lines each. They will catch any future
  regression in G1/G2/G3/G4 detection forever, with no fuzzy matching
  required.
- **Post-mortem clarified outstanding work.** Writing the timeline +
  fix matrix exposed 3 carry-over items I hadn't formally tracked
  (cross-recipient context bleed, persona-derived detector,
  selection-step audit) and made it easy to scope Sprint 33.
- **Privacy discipline held.** Real recipient identifiers and persona
  PII were redacted (`<recipient A/B/C>`, no operator name in the doc),
  even though the regression test fixtures necessarily contain the
  verbatim payload. The post-mortem is open-source-publishable; the
  test fixtures are gated by being internal regression strings.

## What didn't go well

- **Rediscovery-by-audit suggests the alert path is missing.** We
  found these 5604x/5606x leaks not because we noticed in real time,
  but because we re-ran an audit script. There is still no telemetry
  that flags an outbound message classified MARGINAL. The fix exists
  in the response guard now (Phase 4 + REJECT instead of REWROTE)
  but we should also be screaming in stderr/log when REJECT triggers,
  not silently retrying.
- **Audit script signature scan is not yet wired to G5/G6.** The
  bash audit checks G1-G4 but not length anomaly or director echo —
  those need per-turn context the script doesn't have. For historical
  audit, this is fine (G1-G4 hit all 6 leaks), but a true production
  audit would want runtime-emitted classification logs.
- **Test/runtime asymmetry persists.** All these guard improvements
  are exercised by tests but the production daemon still calls
  `hu_response_guard_check` (legacy ctx-less path). Sprint 33
  must close that gap.

## Action items for Sprint 33

1. Wire `hu_response_guard_check_ex` into `agent_stream.c`, `agent_turn.c`,
   `daemon.c`. Build `hu_guard_context_t` from the agent's recent
   conversation length history + the active scene-direction buffer.
2. Quality gate policy: `MARGINAL → REJECT` when length anomaly ≥ 5x.
   This is the carry-over from Sprint 28; should be a one-line policy
   change in `agent_stream_v2.c` plus a regression test.
3. Add a daemon-side log line on every guard `REJECT` (severity = warn,
   include the detected_* report fields). Without this, future leaks
   will again be silent until someone runs the audit.

## Sprint metrics

| Metric | Value |
|---|---|
| Stories shipped | 3/3 |
| New regression tests | 2 |
| Total response_guard tests | 41 |
| Lines added (script) | ~200 (bash) |
| Lines added (post-mortem) | ~140 (md) |
| Live audit run time | ~16s for 448 messages |
| New leaks discovered | 2 (rowids 56049, 56063) |
| Sprint duration | < 1 day (incident-driven) |
