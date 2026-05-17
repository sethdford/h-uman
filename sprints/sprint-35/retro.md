# Sprint 35 — Retrospective

## What went well

- **Highest-leverage detector shipped.** All four 2026-05-11 leaks
  (rowids 56049, 56055, 56063, 56065) and both 2026-05-12 leaks
  (56354, 56355) shared a common signature: third-person quotes of
  the operator's loaded persona name. G7 closes the structural gap
  that Sprint 30 left when it removed hardcoded PII from G3 in favor
  of generic patterns. Operator's name is now matched dynamically at
  runtime, never appearing in source.
- **All three observability layers light up.** REJECT log now emits
  `persona=0/1` consistently across G5/G6/G7 — single grep over the
  daemon's stderr can answer "which detector caught this".
- **Test discipline.** 10 new tests, ~260 LOC. Covered:
  - 3 reject patterns × ≥1 case each (is/possessive/lives/works/etc).
  - 2 pass patterns (first-person, direct address).
  - 2 skip patterns (NULL, too-short).
  - Word-boundary isolation ("Bethseth" ≠ "Seth").
  - Case-insensitivity (4 case variations).
  - 1 integration test driving the wired path through `hu_agent_turn`.
- **Word boundary check earned its keep.** First version of the
  detector matched any substring; would have falsely rejected words
  containing "Seth" inside ("Bethseth", "Seth-something"). The
  `prev_letter`/`next_letter` boundary check is a 6-line addition
  that prevents an entire false-positive class.
- **The slack window for the construct.** The detector accepts up to
  30 bytes between the name and the third-person construct, so
  patterns like `"Seth Douglas Ford, 51, is a Chief Architect"` —
  which the audit found verbatim in rowid 56055 — are caught even
  with parenthetical content between the name and the verb.
- **Persona shim pattern for tests.** Allocating a minimal persona
  with only `name`/`name_len` set + zeros worked cleanly with
  `hu_persona_deinit` (which checks each field for NULL before
  freeing). No ASan complaints.

## What didn't go well

- **Persona name only covers part of the surface.** The leaks also
  contain profession ("Chief Architect"), age ("51"), and biography
  fragments. G7 catches `"<Name> is a Chief Architect"` because of
  the name, but a leak that says `"i'm 51, Chief Architect, lives
  alone"` (no name) would slip past. Sprint 36 should extend G7 to
  also match profession and verbatim biography fragments.
- **Lookahead window may be too tight.** 30 bytes covers
  `"Seth, 51, is a"` (12 bytes from name end to "is") but a leak
  with longer parenthetical content (`"Seth (chief architect at
  Pure Health Solutions, age 51, divorced) is a"` — 50+ bytes) would
  miss. Either widen to 60 bytes or scan the full sentence.
  Trade-off: wider lookahead increases false-positive risk.
- **Integration test deinits the persona shim through `hu_agent_deinit`.**
  Works because `hu_persona_deinit` defensively NULL-checks each
  field — but it's brittle. If someone adds a new persona field
  without a NULL guard in the deinit, the test will SIGSEGV with no
  obvious cause. Mitigation: an explicit `agent.persona = NULL`
  before `hu_agent_deinit` in the test would have been safer; left
  it as-is to keep coverage of the persona-deinit path.
- **Verbose detector implementation.** The construct-list is 23
  patterns hardcoded. A future maintainer who adds a new pattern
  has to remember to add it here. A more principled approach:
  derive constructs from a verb dictionary. Out of scope for now;
  documented in the comment block above the function.

## Action items for Sprint 36

1. **Profession / title in G7.** Add `agent->persona->identity` and
   `->core_anchor` (one-line bio) as additional director-text-class
   inputs to G7. Reject if the response contains either verbatim.
2. **Widen lookahead.** From 30 → 60 bytes, but only when persona
   name is found at a word boundary. Measure false-positive rate
   on real test data first.
3. **Document the construct list in a comment block** so future
   maintainers know why it's hardcoded vs derived. (Or derive it.)
4. **Multi-turn director memory** (Sprint 34 carry-over). Lower
   priority now that G7 catches the higher-leverage class.
5. **Daemon clear-on-exit test** (Sprint 34 carry-over). Still real.

## Sprint metrics

| Metric | Value |
|---|---|
| Stories shipped | 4/4 |
| New detector phase | 1 (Phase 4c / G7) |
| Production call sites updated | 3 |
| Public API additions | 2 fields on context, 1 field on report |
| New unit tests | 9 |
| New integration tests | 1 |
| Total Response Guard suite | 53 (was 44; +9) |
| Total Response Guard Retry suite | 6 (was 5; +1) |
| Total dev suite | 10313 (was 10303; +10 = matches) |
| Lines added (production) | ~135 |
| Lines added (tests) | ~260 |
| Sprint duration | < 1 day |
| Behavioral regression | 0 (cold-start preserves legacy path) |
