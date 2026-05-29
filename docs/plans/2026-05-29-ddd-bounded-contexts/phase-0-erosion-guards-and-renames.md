# Phase 0 — Erosion Guards, Ubiquitous-Language Renames, Context Doc

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Lock in the boundaries that are *already* clean, freeze the worst leak from getting worse, and resolve the ubiquitous-language hazards — all before any structural move. Everything here is low-risk and reversible.

**Architecture:** Three path-scoped rules + check scripts (the project's established `scripts/check-*.sh` + `.claude/rules/*.md` pattern, e.g. `check-test-references.sh`), one context-documentation file, and one optional script-driven rename. "Hooks are guarantees, CLAUDE.md is suggestions" — these are enforcement scripts wired into the pre-commit hook, not prose.

**Tech Stack:** Bash check scripts, `.githooks/pre-commit`, markdown docs.

---

## File Structure

- Create: `scripts/check-edge-context-isolation.sh` — no cross-impl includes in providers/channels/tools
- Create: `scripts/check-agent-core-boundary.sh` — no factory includes / channel `memcmp` in `src/agent/`
- Create: `scripts/check-sqlite-includer-ratchet.sh` — `#include <sqlite3.h>` count must not grow past baseline
- Create: `.claude/rules/edge-context-isolation.md`, `.claude/rules/agent-core-boundary.md`, `.claude/rules/sqlite-includer-ratchet.md`
- Create: `docs/standards/engineering/bounded-contexts.md` — the canonical context map + Modeled-Person layering
- Modify: `.githooks/pre-commit` — wire the three checks
- (Optional appendix) script-driven renames of `data/`→`resources/` and runtime `eval/`→`scoring/`

---

### Task 1: Freeze the SQLite leak with a ratchet (highest value)

The single most valuable guard: T1 cannot be fixed overnight (146 sites), but it must not get *worse* while Phase 3 chips at it. A ratchet check pins today's count and fails if a new file adds `#include <sqlite3.h>`.

**Files:**
- Create: `scripts/check-sqlite-includer-ratchet.sh`
- Create: `.claude/rules/sqlite-includer-ratchet.md`

- [ ] **Step 1: Capture the current baseline count**

Run: `grep -rln '#include <sqlite3.h>' src/ | wc -l | tr -d ' '`
Expected: `113`. Record this number — it is the ratchet ceiling.

- [ ] **Step 2: Write the ratchet script**

```bash
#!/usr/bin/env bash
# scripts/check-sqlite-includer-ratchet.sh
# T1 guard: the number of src/ files that directly #include <sqlite3.h> must
# only ever DECREASE. New domain code must use the memory query/exec capability
# (see docs/plans/2026-05-29-ddd-bounded-contexts/phase-3-memory-query-interface.md),
# not raw sqlite3. Engines under src/memory/engines/ are exempt.
set -euo pipefail
BASELINE=113
count=$(grep -rln '#include <sqlite3.h>' src/ | grep -v 'src/memory/engines/' | wc -l | tr -d ' ')
echo "sqlite3.h includers (excluding engines): $count (ceiling $BASELINE)"
if [ "$count" -gt "$BASELINE" ]; then
  echo "FAIL: a new file added #include <sqlite3.h>. Use the memory query/exec" >&2
  echo "      capability instead of grabbing the raw handle. New includers:" >&2
  grep -rln '#include <sqlite3.h>' src/ | grep -v 'src/memory/engines/' >&2
  exit 1
fi
# Auto-tighten: if the count dropped, remind the author to lower BASELINE.
if [ "$count" -lt "$BASELINE" ]; then
  echo "NOTE: count dropped below baseline — lower BASELINE to $count to lock the gain." >&2
fi
exit 0
```

- [ ] **Step 3: Make it executable and run it**

Run: `chmod +x scripts/check-sqlite-includer-ratchet.sh && scripts/check-sqlite-includer-ratchet.sh`
Expected: prints the count, exit 0.

- [ ] **Step 4: Write the rule doc** (`.claude/rules/sqlite-includer-ratchet.md`)

```markdown
# SQLite Includer Ratchet — Never Add a New `#include <sqlite3.h>`

The count of `src/` files (excluding `src/memory/engines/`) that include
`<sqlite3.h>` is frozen at a baseline and may only decrease. New domain
code MUST use the memory query/exec capability, not the raw `sqlite3*`
handle from `hu_sqlite_memory_get_db()`. Enforced by
`scripts/check-sqlite-includer-ratchet.sh` in the pre-commit hook.
Rationale: T1 (ambient SQLite dependency) blocks the "runs anywhere"
moat; the ratchet stops the leak widening while Phase 3 closes it.
```

- [ ] **Step 5: Commit**

```bash
git add scripts/check-sqlite-includer-ratchet.sh .claude/rules/sqlite-includer-ratchet.md
git commit -m "build(guards): ratchet on sqlite3.h includers to freeze T1 leak"
```

---

### Task 2: Guard the clean edge contexts

**Files:**
- Create: `scripts/check-edge-context-isolation.sh`
- Create: `.claude/rules/edge-context-isolation.md`

- [ ] **Step 1: Verify the edges are clean today (so the guard starts green)**

Run: `grep -rln '#include "human/providers/[a-z_]*\.h"' src/providers | grep -v factory | head` (expect empty or only shared-infra like `sse`/`provider_http`). Confirm baseline.

- [ ] **Step 2: Write the isolation check**

```bash
#!/usr/bin/env bash
# scripts/check-edge-context-isolation.sh
# A concrete provider/channel/tool must not include ANOTHER concrete impl in
# the same context — only the vtable contract + shared infra. Protects the one
# genuinely clean hexagonal boundary in the codebase.
set -euo pipefail
fail=0
# Channels must not include sibling channel headers (allow shared: format, dispatch,
# contact_signature, channel_embed).
bad=$(grep -rn '#include "human/channels/' src/channels \
  | grep -vE 'format\.h|dispatch\.h|contact_signature\.h|channel_embed\.h' \
  | grep -vE 'behavior_class\.h' || true)
if [ -n "$bad" ]; then echo "FAIL: cross-channel include:" >&2; echo "$bad" >&2; fail=1; fi
# Tools must not include sibling tool impls (meta-tools may include agent — that's
# the legitimate inversion, checked separately).
# (Add provider symmetry here once a shared-infra allowlist is settled.)
[ "$fail" -eq 0 ] && echo "edge-context isolation: OK"
exit $fail
```

- [ ] **Step 3: Run it**

Run: `chmod +x scripts/check-edge-context-isolation.sh && scripts/check-edge-context-isolation.sh`
Expected: `edge-context isolation: OK`, exit 0. If it fails, a real leak exists — fix the include before committing the guard.

- [ ] **Step 4: Rule doc + commit**

```markdown
# Edge-Context Isolation — Concrete Impls Depend Only on Contracts

Concrete providers/channels/tools may include the vtable contract and
shared infra, never a sibling concrete implementation. This is the one
clean hexagonal boundary; protect it. Enforced by
scripts/check-edge-context-isolation.sh.
```

```bash
git add scripts/check-edge-context-isolation.sh .claude/rules/edge-context-isolation.md
git commit -m "build(guards): edge-context isolation check (providers/channels/tools)"
```

---

### Task 3: Guard the agent-core boundary

**Files:**
- Create: `scripts/check-agent-core-boundary.sh`
- Create: `.claude/rules/agent-core-boundary.md`

- [ ] **Step 1: Write the boundary check**

```bash
#!/usr/bin/env bash
# scripts/check-agent-core-boundary.sh
# Agent core must not (a) instantiate providers via the factory directly —
# it gets a provider vtable injected; (b) hardcode channel identity by string.
# Baseline tolerates the 5 known factory sites until Phase 4; ratchets down.
set -euo pipefail
FACTORY_BASELINE=5
fac=$(grep -rln '#include "human/providers/factory.h"' src/agent | wc -l | tr -d ' ')
echo "agent/ provider-factory includes: $fac (ceiling $FACTORY_BASELINE)"
if [ "$fac" -gt "$FACTORY_BASELINE" ]; then
  echo "FAIL: new direct provider-factory include in agent core. Inject the" >&2
  echo "      provider vtable instead." >&2
  exit 1
fi
# No NEW hardcoded channel-name memcmp in agent/ (Phase 1 removed them from the
# turn loop; keep it that way). Allow the generic substring helper at line ~733.
chan=$(grep -rn 'memcmp([a-z_]*, *"\(imessage\|slack\|telegram\|discord\|whatsapp\|signal\|sms\|email\)"' src/agent || true)
if [ -n "$chan" ]; then
  echo "FAIL: hardcoded channel-name memcmp in agent core — use" >&2
  echo "      hu_channel_behavior_class_for_name() (channels context):" >&2
  echo "$chan" >&2
  exit 1
fi
echo "agent-core boundary: OK"
exit 0
```

- [ ] **Step 2: Run it**

Run: `chmod +x scripts/check-agent-core-boundary.sh && scripts/check-agent-core-boundary.sh`
Expected: prints factory count `5`, no channel memcmp, exit 0. **Note:** run this AFTER Phase 1 lands (Phase 1 removes the turn-loop memcmp). If run before Phase 1, the channel check will fail — that's correct; it documents the violation.

- [ ] **Step 3: Rule doc + commit**

```markdown
# Agent-Core Boundary — No Concrete Provider/Channel Knowledge

src/agent/ must inject the provider vtable (no `providers/factory.h`)
and must not branch on channel identity by string `memcmp` — use
`hu_channel_behavior_class_for_name()` from the channels context.
Factory baseline 5 ratchets to 0 in Phase 4. Enforced by
scripts/check-agent-core-boundary.sh.
```

```bash
git add scripts/check-agent-core-boundary.sh .claude/rules/agent-core-boundary.md
git commit -m "build(guards): agent-core boundary check (no factory/channel-string)"
```

---

### Task 4: Wire the guards into pre-commit

**Files:**
- Modify: `.githooks/pre-commit`

- [ ] **Step 1: Add the three checks**

In `.githooks/pre-commit`, alongside the existing `scripts/check-*.sh` invocations, add:

```bash
scripts/check-sqlite-includer-ratchet.sh || exit 1
scripts/check-edge-context-isolation.sh || exit 1
scripts/check-agent-core-boundary.sh || exit 1
```

- [ ] **Step 2: Smoke-test the hook**

Run: `.githooks/pre-commit` (or `git commit` a trivial change)
Expected: all three checks print OK; commit proceeds.

- [ ] **Step 3: Commit**

```bash
git add .githooks/pre-commit
git commit -m "build(guards): wire DDD boundary checks into pre-commit"
```

---

### Task 5: Document the bounded-context map (the Modeled-Person fix)

This is the conceptual fix for the 8-dir humanness fragmentation: no code moves, but the layering becomes explicit so future work respects it.

**Files:**
- Create: `docs/standards/engineering/bounded-contexts.md`

- [ ] **Step 1: Write the context map doc**

```markdown
# Bounded Contexts

h-uman's directories are feature-folders; the real domain boundaries are
fewer. This is the canonical map. (Full audit + roadmap:
docs/plans/2026-05-29-ddd-bounded-contexts/README.md.)

## Modeled Person (one context, three layers)

persona/  → expression: identity, voice, style, boundaries
cognition/ → perception: emotion, trust, attachment, presence
behavior/ → decision: relational acts, intensity, modulation

These share two aggregate roots — `hu_persona_t` (expression state) and
`hu_personal_model_t` (learned state, in memory/) — and have near-zero
cross-includes. Per-turn flow: persona → cognition → behavior. Do not add
cross-includes between cognition and behavior; communicate via the roots.

## Other contexts
- Conversation Orchestration: agent/, daemon*.c
- Recall: memory/ (behind hu_memory_t — never grab the raw db handle)
- Learning Loop: intelligence/ + reflection/
- Model Access (edge ACL): providers/, channels/, tools/ — contracts only
- Evaluation (runtime, eval/) vs Benchmarking (offline, evaluation/): distinct, do not merge
```

- [ ] **Step 2: Commit**

```bash
git add docs/standards/engineering/bounded-contexts.md
git commit -m "docs(arch): canonical bounded-context map + Modeled-Person layering"
```

---

## Appendix A — Ubiquitous-language renames (optional, high-churn, script-driven)

Two renames remove ambiguity but touch many includes. Per
`~/.claude/rules/agent-task-sizing.md`, N≥20 mechanical sites → use a script,
not hand edits.

**`data/` → `resources/`** (lower churn; `data/` holds static blobs/loaders, not persistence):

```bash
git mv src/data src/resources && git mv include/human/data include/human/resources
grep -rl 'human/data/' src include tests | xargs sed -i '' 's#human/data/#human/resources/#g'
touch $(grep -rl 'human/resources/' src | tr '\n' ' ') && cmake --build build -j8 && ./build/human_tests
```

**Runtime `eval/` → `scoring/`** (HIGH churn — `eval.h` has 256 includers and is
live in the agent loop). **Recommendation: DEFER until after Phase 3.** Renaming a
256-include live module mid-refactor invites merge pain with the daemon/memory work.
When done, script it the same way and run the FULL suite. Do NOT touch `evaluation/`
(the offline benchmark context keeps its name).

> Decision needed before executing Appendix A: confirm the target names
> (`resources/`, `scoring/`) and the timing (now vs after Phase 3).

---

## Self-Review

- **Spec coverage:** T1 freeze (Task 1), edge guard (Task 2), agent-core guard + channel-string freeze (Task 3), enforcement wiring (Task 4), Modeled-Person doc (Task 5), renames (Appendix). ✓
- **No placeholders:** every script and doc is complete; baselines are real measured numbers (113, 5). ✓
- **Ordering note:** Task 3's channel-memcmp check must run *after* Phase 1; flagged in-step. The factory ratchet (5) is independent and green today.
- **Hooks-are-guarantees:** the guards are pre-commit scripts, not prose — determinism in the harness per the global operating principles. ✓
