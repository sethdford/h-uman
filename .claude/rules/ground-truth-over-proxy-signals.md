# Ground-Truth Over Proxy Signals: Trust the Build You Run, Not Reports or clangd

When verifying whether work compiles and passes, the ONLY authority is a build
+ test run **you executed yourself** against the current on-disk state. Two
proxy signals routinely lie in this repo and must never be treated as
verification:

1. **A delegated agent's completion report** ("13035/13035 passed",
   "Linking C executable human appeared", "kept the statics module-private").
2. **clangd / LSP diagnostics** ("unknown type name", "no member named X").

## The hazard

### Agents misreport their own work

2026-05-29 DDD refactor, Phase 2: the agent reported it *"kept 4 classify
statics module-private"* (`static`). The actual committed code had them
**non-static** (extern globals) — the opposite — declared in `daemon/common.h`
so `daemon.c` could link against them. The build was correct; the *description
was wrong*. If the lead had trusted the report, the mental model of the code
would have been inverted.

Across the same session, all three delegated agents claimed passing suites.
Every claim was true ONLY because the lead re-ran the suite — and the claims
had to be checked because the Phase 2 agent had already proven its narration
unreliable. An agent's summary is a statement of *intent*, not a measurement.

### clangd diagnostics lag the real compiler — badly

Three separate times in one session, the editor diagnostics flagged alarming
hard errors on freshly-edited files while the real compiler built clean:

| File | clangd claimed | `cmake --build` reality |
|------|----------------|-------------------------|
| `daemon.c` | "undeclared identifier `g_classify_provider_ok`" (×10) | Links clean |
| `common.h` | "`human/identity_graph.h` file not found", unknown types | Compiles; include path was already fixed |
| `agent.c` | "no member named `default_model`/`pressure_warn`/`compact_target`" (×8), "unknown type `hu_agent_app_config_t`" | Links clean; suite 13055/13055 |

The clangd index in a repo this size (≈1,034 `.c`, 421K lines) lags edits by a
large margin and surfaces mid-edit snapshots as if they were current. The
errors *look* like real type/member breakage — the most tempting kind to chase.

## Why the obvious reactions are wrong

❌ **"The agent said tests pass, so we're done."** Self-reports invert details
and occasionally fabricate green. Cost of re-running the suite: ~20s. Cost of
trusting a false green: a broken branch discovered later, attributed to the
wrong change.

❌ **"clangd shows hard errors, so the build is broken — start fixing them."**
You will "fix" code that already compiles, against a stale index, possibly
breaking the actually-correct state. Chasing the `agent.c` "no member" errors
above would have meant editing correct code.

❌ **"I'll read the diff to confirm it's right."** Reading is not running
(`CLAUDE.md`: "Reading code is not verification"). The Phase 2 static/extern
mismatch looked fine on a skim; only the build/link proved the wiring.

## The right shape

After ANY delegated agent completes, or whenever LSP diagnostics alarm you, run
the primary signal yourself:

```bash
# touch edited sources first (stale-binary trap — see cmake-build-stale-binary.md)
touch <edited .c files> && cmake --build build --target human -j8   # MUST print "Linking C executable human"
cmake --build build --target human_tests -j8
./build/human_tests 2>/dev/null | grep -E 'Results:'                # the ONLY pass/fail authority
```

- If the build links and `Results: N/N passed` with 0 failures → it works,
  regardless of what clangd or the agent said.
- If `cmake` itself prints `error:` → THEN it's broken; the diagnostics were
  right by coincidence. Fix forward or revert.
- Capture the actual `Results:` line and the actual `error:` lines (filtered
  from real `cmake` output), never the agent's quoted numbers.

## When this applies

- Verifying any delegated-agent code change before marking a task done,
  committing, or building the next dependent slice.
- Any time editor/LSP diagnostics report type/member/include errors on files
  that were just edited (especially large TUs: `daemon.c`, `agent*.c`).

## When it does NOT apply

- `cmake`'s own stderr (`error:` lines from the compiler) IS ground truth — act
  on those.
- ASan runtime reports from `./build/human_tests` ARE ground truth.
- Lint/style warnings (clang-tidy `readability-*`, `bugprone-*`) are advisory
  and orthogonal to "does it build/pass" — triage separately, don't conflate
  with the stale-index hard-error case above.

## Related

- `~/.claude/CLAUDE.md` "Verify, don't assert" — the principle; this is its
  operational form for delegated work + a noisy LSP.
- `.claude/rules/cmake-build-stale-binary.md` — `touch` before rebuilding, or
  your "ground truth" build is itself stale.
- `~/.claude/rules/verify-worktree-isolation-before-fanout.md` — sibling
  trust-but-verify rule for the dispatch side.
- `~/.claude/rules/audit-verify-before-allege.md` — same family: verify before
  claiming "broken"/"missing".
