# US-3: Remove the dead `channel` parameter in `hu_stop_sequence_registry_lookup`

## Approach
**Recommendation: REMOVE the params.** Rationale:
1. No internal caller passes a non-NULL channel — all 2 production call sites (`src/agent/agent_turn.c:4086`, `src/agent/agent_stream.c:1311`) pass `NULL, 0`. All 7 tests in `tests/test_stop_sequences.c` pass `NULL, 0`.
2. The "future per-channel overrides" comment in the header has been in place since the registry's introduction; no story in this sprint or the deferred backlog requests per-channel stop sequences.
3. Wiring would require a real use case for per-channel divergence (e.g., Slack stripping `\nUser:` but Discord not). No such use case exists.
4. h-uman is pre-1.0, no external SDK consumers documented — internal-only API break is safe.

KISS / YAGNI per project CLAUDE.md: "no speculative abstractions or config flags without a caller."

The replacement is purely mechanical: drop two params from declaration, definition, and 9 call sites (2 production + 7 tests).

## Files to modify
- `include/human/agent/stop_sequence_registry.h` — remove `const char *channel, size_t channel_len` from signature; replace the "reserved for future" comment block with a one-line note: "Per-channel stop sequences are not implemented; provider-only lookup. If needed, add a separate `_lookup_by_channel` overload rather than re-adding parameters."
- `src/agent/stop_sequence_registry.c` — remove the two params and the `(void)channel;` / `(void)channel_len;` suppressions (lines 32–33).
- `src/agent/agent_turn.c:4086` and `src/agent/agent_stream.c:1311` — drop the `NULL, 0` arguments.
- `tests/test_stop_sequences.c` — update all 7 callers (lines 8, 23, 30, 45, 53, 55, 63, 71, 77, 80) to remove `NULL, 0`.

## Risk
- **API break for any out-of-tree caller** (LOW): no SDK header advertises this function; project is pre-1.0.
- **Wrong-direction churn if per-channel sequences are needed later** (LOW): adding a new function later is cleaner than keeping a lying signature now.
- **Build breakage if a caller is missed** (LOW): `-Werror` and the existing test suite catch it on the first build.

## Test strategy
- Update existing 7 test cases in `tests/test_stop_sequences.c` to match new signature; behavior assertions unchanged.
- No new tests needed — AC3 (no `(void)channel` remaining) is a `grep` guard, AC4 is the full suite.
- DoD: `grep "(void)channel" src/agent/stop_sequence_registry.c` returns empty.

## Sequencing
Independent. Can land first, second, or last. Smallest of the three P0 stories — recommend landing first as a warm-up that doesn't touch persona or validator chain.

## Effort estimate
**XS** — ~20 LOC across 4 files, all mechanical. Single sitting, single PR.
