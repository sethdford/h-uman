# LSP for "Is X Wired?" Audits — Named Functions Yes, Vtable Fields No

When auditing whether a symbol is wired ("is X invoked?", "who calls Y?",
"does the agent core reach Z?"), reach for the `LSP` tool's
`findReferences` / `incomingCalls` / `outgoingCalls` on **named functions** —
it answers deterministically in one call. But for **vtable function-pointer
fields** (`hu_*_vtable.send`, `.start`, `.react`, …), clangd cannot see the
designated-initializer assignments or `vtable->field(...)` call-throughs, so
it under-reports. There, grep is still required.

This is the operational tool for `audit-verify-before-allege.md`: the cheapest
way to kill a false-"missing" claim about a named function is a call-hierarchy
query, not a grep-and-infer read.

## The hazard

`audit-verify-before-allege.md` exists because grep-and-infer audits produced
~50% false "missing" claims in one session. Two failure directions:

1. **Grepping when LSP would be exact.** "Is `hu_channel_behavior_class_for_name`
   reached by the agent core?" — grep finds string matches; you still have to
   read each to confirm it's a real call vs a comment vs a decl. LSP answers
   precisely.
2. **Trusting LSP on a vtable field.** `findReferences` on a function-*pointer
   field* returns ~1 hit (the declaration) even when 30 channels assign and
   invoke it — and a naive reading of "1 reference" looks exactly like
   "unwired." That false negative is the dangerous one.

### Evidence (2026-05-29, seth-voice session)

| Query | Tool | Result | Truth |
|---|---|---|---|
| `send` **field** decl, `channel.h:101` | `findReferences` | **1 hit** (decl only) | wired in ~30 channels via `.send =` + invoked via `vtable->send(...)` — clangd sees none of it |
| `hu_channel_behavior_class_for_name`, `behavior_class.h:22` | `findReferences` | **6 hits / 4 files** | correct |
| same function | `incomingCalls` | `at_behavior_channel_class` → `agent_turn.c:389` | **proved** the agent core reaches channel identity only through the delegation wrapper — the `agent-core-boundary.md` contract, in one call |

The `send`-field "1 hit" is the trap: read literally, it says "dead code." It
isn't. clangd indexes references to *named declarations*; a function pointer
stored in a struct field and called through `->` is not a named-decl reference
it tracks.

## Why the obvious approaches are wrong

❌ **"LSP said 1 reference, so the vtable method is unused."** False negative.
clangd cannot resolve `.send = imessage_send` initializers or `vtable->send()`
dispatch as references to the field. Always grep vtable fields:
`grep -rn '\.send\s*=' src/channels/` and `grep -rn 'vtable->send\|->send(' src/`.

❌ **"grep found 12 `send` hits, so it's wired."** grep over-matches: comments,
unrelated locals named `send`, the decl itself. For a *named function* you'd
then read all 12 to confirm — LSP `findReferences` already filtered to real
references. Use the right tool per symbol kind.

❌ **"I'll just read the call sites."** Reading is not verification
(`ground-truth-over-proxy-signals.md`). For named functions, `incomingCalls`
*is* the verification and is faster.

## The right shape

Pick the tool by symbol kind:

| Symbol kind | Tool | Why |
|---|---|---|
| Named function (`hu_module_action`) | `LSP findReferences` / `incomingCalls` | exact; no grep-and-infer |
| Caller chain of a named function | `LSP incomingCalls` (then `outgoingCalls` to descend) | structured call tree |
| Vtable function-pointer field (`.send`, `.react`) | `grep` the `.field =` assignments AND the `->field(` call sites | clangd is blind here |
| "Is this whole vtable implemented by N channels?" | `grep -rn 'static const hu_channel_vtable' src/channels/` | enumerate implementors first |

Preconditions for LSP in this repo: `build/compile_commands.json` exists (any
CMake preset emits it — `CMAKE_EXPORT_COMPILE_COMMANDS=ON`), and `clangd` is on
PATH (`/usr/bin/clangd`). If `findReferences` returns only the declaration for
something you *know* is called, that's the vtable-field signature — switch to
grep, don't conclude "unwired."

## When this rule applies

- Any "is X wired / invoked / reached?" audit, especially the ones
  `audit-verify-before-allege.md` governs.
- Confirming a bounded-context contract (`agent-core-boundary.md`,
  `edge-context-isolation.md`) — `incomingCalls` proves the *only* path in.
- Before alleging a function/feature is MISSING in any Agent dispatch.

## When it does NOT apply

- Pure string/config lookups (factory keys, JSON field names) — those aren't
  C symbols; grep is correct.
- Macros and `#ifdef`-gated symbols — clangd indexes one configuration; a
  symbol live only under a different feature flag may look absent. Cross-check
  with the gate (`test-source-gate-symmetry.md`).
- Behavioral "does it actually run?" questions — that's `/verify` running the
  code, not static cross-references.

## Related

- `~/.claude/rules/audit-verify-before-allege.md` — the discipline; this rule
  is its fastest tool for the named-function case.
- `.claude/rules/agent-core-boundary.md` — `incomingCalls` is how you prove the
  delegation-only contract holds.
- `.claude/rules/ground-truth-over-proxy-signals.md` — LSP cross-refs are a
  strong signal but clangd's index can lag edits; for "does it run", verify.
