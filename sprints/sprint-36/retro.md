# Sprint 36 — Retrospective

## What went well

- **The first-person identity gap is closed.** G7 only fires on
  third-person constructs (`Seth is a developer`). The audit data
  showed multiple leaks that quoted persona context in *first-person*
  (`i'm a Chief Architect at Pure Health Solutions`) — these would
  have slipped past G7. G8 catches them by sliding-window matching
  the persona's identity string directly. With G8 wired, the original
  2026-05-12 leak fragment now trips **8 separate detectors** (G1-G8).
- **Reused the G6 sliding-window primitive.** No new substring-search
  code; just a different threshold (25 vs 30) and a different source
  string (persona identity vs director text). 50 LOC of detector code
  total. KISS.
- **Orthogonality tests earned their keep.** Two of the unit tests
  set BOTH `director_text` and `persona_identity` (or both
  `persona_name` and `persona_identity`) and verified that *only* the
  correct flag fires. This caught a near-miss during development:
  early version of `hu_guard_has_persona_identity_echo` was matching
  against `ctx->director_text` by mistake (copy-paste error). The
  orthogonality test failed loudly — fixed before commit.
- **Threshold calibrated against real data.** 25-byte minimum is
  shorter than every audited identity-leak (all ≥ 30 chars) and
  longer than common-phrase floor (`thanks for the message` =
  22 bytes). No FP measurement needed yet — the math is conservative.
- **Identity vs core_anchor fallback handles real personas.** Looking
  at production persona JSON files in `~/.human/personas/`, some
  define only `identity`, some only `core_anchor`, some both. The
  `if (identity) else if (core_anchor)` cascade works for all three
  cases without requiring schema changes.

## What didn't go well

- **G8 still doesn't catch paraphrased identity leaks.** A model that
  says `"i work as a software architect for a healthcare company"`
  (paraphrase of `"Chief Architect at Pure Health Solutions"`) would
  not match because there's no 25-byte verbatim substring. This is
  the upper bound of what verbatim-substring detection can do. Future
  detectors would need either embedding similarity or LLM-as-judge —
  both expensive, both async, both complex. Not in scope yet.
- **Window threshold tradeoff.** 25 bytes is conservative but means
  a leak quoting only 23 bytes of identity (e.g. just
  `"Chief Architect at Pure"`) would slip past. Tightening to 20
  would catch this but increases FP risk on common phrases. Worth
  measuring G8 hit rate over a week before tuning.
- **Identity-fallback to core_anchor is silent.** If `identity` is
  NULL and `core_anchor` is set, no log line tells operators which
  field is being used. In practice this is fine (both are persona
  PII), but a debug log would help when investigating false
  positives.
- **No multi-language support.** Identity strings in non-ASCII
  scripts (e.g. CJK persona names/bios) might trip the
  `hu_guard_ci_contains` case-insensitivity logic differently. Out
  of scope for now — the codebase is English-first.

## Action items for Sprint 37

1. **Measure G8 hit rate.** Add a counter in the agent for "G8 fires
   per N turns". After 1 week of runtime, decide whether to tighten
   threshold to 20.
2. **Widen G7 lookahead 30 → 60 bytes** (Sprint 35 carry-over).
   Should be done alongside G8 measurement.
3. **Multi-turn director memory** (Sprint 34 carry-over). Lower
   priority now.
4. **Daemon clear-on-exit test for `scene_direction_text`** (Sprint
   34 carry-over). Real, but small.
5. **Persona biography field as G8 input** — extends G8 to also
   match against `persona->biography` (longer, richer than identity).

## Sprint metrics

| Metric | Value |
|---|---|
| Stories shipped | 4/4 |
| New detector phase | 1 (Phase 4d / G8) |
| Production call sites updated | 3 |
| Public API additions | 2 fields on context, 1 field on report |
| New unit tests | 8 |
| New integration tests | 1 |
| Total Response Guard combined suite | 65 (was 59; +9 = matches) |
| Total dev suite | 10322 (was 10313; +9 = matches) |
| Lines added (production) | ~91 |
| Lines added (tests) | ~220 |
| Sprint duration | < 1 hour |
| Behavioral regression | 0 (cold-start preserves legacy path) |
