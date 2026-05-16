# US-4: Cache validator chain on `hu_persona_t`

## Approach
Make the persona own the outbound chain. Today the chain is rebuilt per message at 8+ call sites (5 in `daemon.c`, 2 in `agent_stream.c`, 1 in `agent_turn.c`, 1 in `daemon_cron.c`, plus `gateway/openai_compat.c`, 3 in `channels/format.c`, and `channels/imessage.c`). All call `hu_validators_build_default_outbound_chain(alloc, NULL, 0, &out_chain)` then `_destroy` immediately after. The build cost is small but the allocation traffic is per-token in streaming hot paths.

Add `hu_output_validator_chain_t *outbound_chain;` to `hu_persona_t` (after `structured_output_enabled` at `include/human/persona.h:448`). Populate it once at the end of `hu_persona_load_json` (`src/persona/persona.c:1041`) — the persona name is finalized there, and the chain needs `persona_name` for `assistant_closer` matching. Free it at the head of `hu_persona_deinit` (`src/persona/persona.c`, declared at `persona.h:475`).

Ownership rule (documented in header doxygen on the new field): persona owns the chain for its full lifetime; chain is NULL only when persona has zero validator rules or `_load_json` failed before completion. Persona name is treated as immutable post-load — mutating `persona->name` does not re-derive the chain. State this in the header comment.

Migrate each call site to read `agent->persona->outbound_chain` (or `sc->persona->outbound_chain` in daemon stream ctx). Where no persona is available (e.g. `channels/imessage.c:910`, `openai_compat.c:623`, `daemon_cron.c:290`), fall back to a local build — these are not the AC-4.3 grep target since AC-4.3 says "outside `src/persona/`". Re-read AC-4.3: "5 daemon paths plus `agent_turn.c` and `agent_stream.c`". The grep guard scopes to those. Leave the four non-persona-bearing sites alone for this sprint; note them in a follow-up comment.

## Files to modify
| File | Change | LOC |
|---|---|---|
| `include/human/persona.h:~448` | add `outbound_chain` field + doxygen | +6 |
| `src/persona/persona.c` (`hu_persona_load_json` tail) | build chain from persona name | +12 |
| `src/persona/persona.c` (`hu_persona_deinit`) | destroy chain | +4 |
| `src/daemon.c` lines 1072, 1733, 2096, 9288, 10646, 11686 | swap inline build for `persona->outbound_chain` | -50 / +30 |
| `src/agent/agent_turn.c:5585` + `agent_stream.c:1425,2145` | swap to cached chain | -30 / +15 |
| `tests/test_validator_chain_cache.c` (new) | load → pointer-stable assertion + ASan leak cycle | +90 |

## Implementation steps
1. Add the field. Compile. (Nothing reads it yet — should still pass.)
2. Build at end of `hu_persona_load_json`; destroy in `hu_persona_deinit`. Run `tests/test_persona_*` under ASan.
3. Write the new test (pointer stability + leak-free load/use/unload).
4. Migrate call sites one file at a time; rebuild + run targeted suites between.
5. Grep guard: `grep -rn "hu_validators_build_default_outbound_chain" src/ | grep -v "src/persona/\|imessage.c\|openai_compat\|daemon_cron\|channels/format.c"` returns 0.
6. Full suite + ASan.

## Risks
- **Lifetime (MED/MED)**: persona destroyed while a streaming callback still holds chain pointer. Mitigation: callbacks already hold `sc->persona` via owning agent; persona outlives stream context. Add a `assert(persona)` at use sites.
- **Persona name immutability (LOW/SMALL)**: code paths that rewrite `persona->name` after load would invalidate `assistant_closer` matching. Mitigation: header comment + grep audit for `persona->name =` writes (expected: zero hits outside loader).
- **AC scope ambiguity (LOW)**: AC-4.3 names 5 daemon paths; the audit found 6 in `daemon.c`. Confirm with PO post-design if all 6 should migrate; default = migrate all 6.
- **Concurrency (LOW/SMALL)**: chain is read-only after build; no race.

## Test strategy
- New `tests/test_validator_chain_cache.c`: (a) `outbound_chain != NULL` after load with rules; (b) pointer stable across two `hu_persona_select_examples` calls; (c) full load/use/destroy cycle under ASan reports 0 leaks; (d) zero-rule persona has `outbound_chain == NULL` and call sites tolerate it.
- Pin existing `output_validator`, `validators_builtin`, `validators_persona_safety`, `pattern_c_paths` suites.

## AC mapping
- AC-4.1 → field + load behavior (test case a, d)
- AC-4.2 → pointer-stable test (case b)
- AC-4.3 → grep guard in CI or in test (`test_validator_chain_cache.c` shells out via `system()` — or document grep result in PR per Sprint-3 precedent)
- AC-4.4 → ASan leak cycle (case c)
- AC-4.5 → full suite green

## Effort
**M** — 7 call sites + lifecycle + new test + grep audit. ~200 LOC. Risk concentrated in lifetime correctness.
