---
title: "Sprint 3 Plan — Hybrid recall: wire, test, embed, backfill"
sprint: 3
created: 2026-05-15
scrum_master: claude-sonnet-4-6
status: ready_to_dispatch
---

# Sprint 3 Plan

## Branch

**sprint-3-hybrid-recall** (tip: `13b89763`, off `origin/main`)

## Working directory

**`/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-1-hybrid-recall`**

This is the ONLY valid working directory for this sprint. No implementer may commit
to any other branch or work in any other directory without explicit Scrum Master
sign-off. Per Sprint 1 audit (`sprints/sprint-1/audit.md`): 4/4 stories failed DoD
because work lived in the working tree only, never committed to any branch.

---

## Sprint Goal

Wire `hu_hybrid_retrieve` into per-contact recall, establish the integration-test
template all future sprints reuse, flag-gate the daemon's embedder initialisation,
and produce a standalone diagnostic backfill tool — so that Mindy returns
semantically-relevant memories not just keyword-matched ones.

---

## Cross-story dependency map

```
US-3.2 (test harness)   ─────────────────────────────────────────────────────> INDEPENDENT
US-3.1 (wire hybrid)    → signature change lands on branch → unblocks US-3.3 step 5
US-3.4 (backfill tool)  ─────────────────────────────────────────────────────> INDEPENDENT

US-3.3 (daemon wiring)  → BLOCKED on US-3.1 commit landing on sprint-3-hybrid-recall
                          specifically: agent_turn.c:1490 call site uses the new
                          hu_memory_recall_for_contact signature (embedder, vector_store
                          params) added by US-3.1; US-3.3 step 5 CANNOT start until
                          `git log sprint-3-hybrid-recall ^origin/main` shows US-3.1's
                          commit.
```

**Serialization point:** US-3.3 is the ONLY story blocked; everything else is parallel.
US-3.2 ships with AC-3.2.5 (semantic-recall) RED by design — US-3.1's implementer
flips it green. Do NOT block US-3.2 merge on AC-3.2.5 passing.

---

## Sequencing

```
Wave 1 (parallel, 3 implementers):   US-3.1  US-3.2  US-3.4
Wave 2 (sequential, 1 implementer):  US-3.3  — starts only after US-3.1's signature
                                               commit appears in sprint-3-hybrid-recall
```

**Wave 1 gate:** All three stories verifier PASS + critic CLEAN + aspect-panel PASS.
Each story's gate is evaluated independently — Wave 2 starts when US-3.1 clears
(not when all of Wave 1 clears). US-3.2 and US-3.4 clearing is still required before
the sprint review can open.

**Wave 2 start condition:**
```bash
git log sprint-3-hybrid-recall ^origin/main --oneline \
  | grep -q "wire hu_hybrid_retrieve\|US-3.1"
```
If the above returns non-zero, Wave 2 has not started. Re-check; do not dispatch
US-3.3 implementer early.

---

## Assignments

| Story | Implementer | Isolation | Priority |
|-------|-------------|-----------|----------|
| US-3.1 | general-purpose | worktree | P0 |
| US-3.2 | general-purpose | worktree | P0 |
| US-3.4 | general-purpose | worktree | P2 |
| US-3.3 | general-purpose | worktree | P1, Wave 2 |

All implementers work inside the sprint worktree at
`/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-1-hybrid-recall` and commit
to branch `sprint-3-hybrid-recall`.

---

## Wave 1 — Implementer prompt requirements

Each Wave 1 implementer prompt MUST include ALL of the following. Do not abbreviate.

### US-3.1 implementer prompt must include:

1. Full text of US-3.1 story (stories.md lines 86-110) and all 7 ACs verbatim.
2. Full text of `sprints/sprint-3/designs/US-3.1.md` (entire file).
3. The sprint branch name: **sprint-3-hybrid-recall**.
4. Explicit instruction: "Your working directory is
   `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-1-hybrid-recall`. You
   MUST commit all changes to branch `sprint-3-hybrid-recall` via
   `git add <specific-paths> && git commit -m "feat(memory): wire hu_hybrid_retrieve..."`.
   Working-tree-only DONE is rejected without exception."
5. Mandatory rebase instruction: "Before touching any file, run
   `git fetch origin && git reset --hard 13b89763`. Verify `git status` is clean.
   The modified agent_turn.c / agent_stream.c / main.c in the current working tree
   are from a concurrent agent and MUST NOT enter this commit."
6. The R-HIGH risk: hybrid.c's `search_results_to_entries` populates `entries[i].key`
   from content, not the original key — semantic-only hits will fail the contact-prefix
   filter. Implementation step 3 (hybrid.c key-propagation fix) MUST be done and
   unit-tested BEFORE touching contact_memory.c.
7. All 8 call sites of `hu_memory_recall_for_contact` must be updated in one commit.
   Grep command: `grep -rn "hu_memory_recall_for_contact(" src/ include/ tests/`.
8. Commit message template (verbatim):
   ```
   feat(memory): wire hu_hybrid_retrieve into hu_memory_recall_for_contact

   Adds nullable embedder + vector_store parameters; routes to
   hu_hybrid_retrieve when both are non-NULL, falls back to BM25/FTS
   otherwise. Preserves original entry keys through RRF so the
   contact-prefix filter still matches semantic-only hits.

   Sprint 3 / US-3.1
   ```
9. Evidence path: `sprints/sprint-3/evidence/3.1/`.
10. Definition of Done checklist (stories.md lines 21-31), requiring all 7 items
    before reporting DONE.

### US-3.2 implementer prompt must include:

1. Full text of US-3.2 story (stories.md lines 52-83) and all 9 ACs verbatim.
2. Full text of `sprints/sprint-3/designs/US-3.2.md` (entire file).
3. The sprint branch name: **sprint-3-hybrid-recall**.
4. Explicit instruction: "Your working directory is
   `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-1-hybrid-recall`. Commit
   to branch `sprint-3-hybrid-recall` before reporting DONE."
5. Canonical macro name alert: **`HU_ASSERT_GE` is correct; `HU_ASSERT_INT_GTE`
   does not exist.** The AC text contains a typo. Use `HU_ASSERT_GE` everywhere.
6. Longjmp / tempdir-leak risk: the test framework uses longjmp on assertion failure.
   Tempdir cleanup MUST use the suite-level wrapper pattern with
   `g_sprint3_last_tempdir` static variable — NOT inline teardown at the bottom of
   each test body. Reviewer blocks merge if inline-teardown pattern is present.
7. AC-3.2.5 ships RED by design: add `printf("EXPECTED: this assertion is red until
   US-3.1 ships\n")` immediately before the semantic-recall assertion. Do NOT add an
   ifdef guard — red tests are visible; ifdef guards rot.
8. Mock provider: copy verbatim from `tests/test_e2e.c:40-109`. Do NOT redesign.
9. Commit message:
   `test(integration): add sprint3 hybrid recall harness (US-3.2, FU-3 template)`
10. Evidence path: `sprints/sprint-3/evidence/3.2/`.
11. Definition of Done checklist (stories.md lines 21-31).

### US-3.4 implementer prompt must include:

1. Full text of US-3.4 story (stories.md lines 143-168) and all 8 ACs verbatim.
2. Full text of `sprints/sprint-3/designs/US-3.4.md` (entire file).
3. The sprint branch name: **sprint-3-hybrid-recall**.
4. Explicit instruction: "Your working directory is
   `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-1-hybrid-recall`. Commit
   to branch `sprint-3-hybrid-recall` before reporting DONE."
5. Scope fence: US-3.4 MUST NOT touch any file under `src/memory/`. If the
   implementer is tempted to add a daemon read-path for the `embeddings` table, that
   is out of scope. The critic agent will flag any `src/memory/` change in this PR
   as a scope violation.
6. The `embeddings` table is diagnostic-only; the daemon does not read it.
7. Helper binary (`scripts/hu-embed-helper.c`) must call `vtable->deinit` and
   `hu_embedding_free` before exit — ASan-clean is non-negotiable.
8. Shellcheck must pass: `shellcheck scripts/embed-existing-memories.sh` exits 0.
9. Commit message: `feat(scripts): add embed-existing-memories backfill tool (US-3.4)`
10. Evidence path: `sprints/sprint-3/evidence/3.4/`.
11. Definition of Done checklist (stories.md lines 21-31).

---

## Wave 2 — US-3.3 implementer prompt requirements

Wave 2 does NOT dispatch until the Scrum Master verifies:
```bash
git log sprint-3-hybrid-recall ^origin/main --oneline \
  | grep -q "wire hu_hybrid_retrieve\|US-3.1"
# must exit 0
```

### US-3.3 implementer prompt must include:

1. Full text of US-3.3 story (stories.md lines 114-138) and all 7 ACs verbatim.
2. Full text of `sprints/sprint-3/designs/US-3.3.md` (entire file).
3. The sprint branch name: **sprint-3-hybrid-recall**.
4. Explicit instruction: "Your working directory is
   `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-1-hybrid-recall`. The
   US-3.1 signature change is already committed to sprint-3-hybrid-recall. Start from
   that tip; do NOT rebase to origin/main independently. Commit to
   sprint-3-hybrid-recall before reporting DONE."
5. CRITICAL pre-audit instruction: before touching bootstrap.c:871-885, run
   `grep -rn "ctx->embedder\|ctx->vector_store\|app_ctx\.embedder\|app_ctx\.vector_store" src/ tests/`
   and enumerate every reader. If more than 5 readers are found, halt and report to
   Scrum Master before proceeding.
6. Skill-routing coupling: `bootstrap.c:1014` (`hu_agent_set_skill_route_embedder`)
   shares the same embedder instance. Flag-gating the embedder also disables skill
   routing. The approved mitigation is: gate `bootstrap.c:1014` on
   `bi->cfg.memory.hybrid_recall` (or `bi->embedder.ctx != NULL`). Do NOT invent a
   separate `cfg.agent.skill_routing` flag — that is YAGNI scope creep.
7. Step 5 (agent_turn.c wiring) REQUIRES the US-3.1 signature to be present on the
   branch. Verify with:
   `grep -n "hu_embedder_t.*embedder\|hu_vector_store_t.*vector_store" include/human/memory.h`
   before starting step 5.
8. AC-3.3.4 (cold-path safety) is CURRENTLY VIOLATED — bootstrap.c creates embedder
   unconditionally. This is the headline change. The cold-path test must pass before
   reporting DONE.
9. Commit message:
   ```
   feat(bootstrap): flag-gate hybrid recall init behind cfg.memory.hybrid_recall

   Adds hybrid_recall bool to hu_memory_config_t (default false). Gates
   embedder + vector_store + retrieval_engine creation at bootstrap.c:871-885
   on the flag. Adds hu_agent_set_hybrid_retrieval setter; wires embedder
   and vector_store through agent_turn.c call site. Cold-path safety
   now verified: disabled by default, no embedder allocated.

   Sprint 3 / US-3.3
   ```
10. Evidence path: `sprints/sprint-3/evidence/3.3/`.
11. Definition of Done checklist (stories.md lines 21-31).

---

## Pre-flight checks — per wave

### Wave 1 pre-flight (Scrum Master runs before dispatching any implementer):

- [ ] `git -C /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-1-hybrid-recall status --short` shows working tree is at origin/main@13b89763 — no unstaged edits bleed in.
- [ ] `git log sprint-3-hybrid-recall ^origin/main --oneline` shows no unrelated commits from other agents on the sprint branch.
- [ ] Evidence directories exist: `sprints/sprint-3/evidence/3.1/`, `3.2/`, `3.4/`.
- [ ] All three implementer prompts include the AC text verbatim (not summarized).
- [ ] All three implementer prompts include the commit-before-DONE instruction.

### Wave 2 pre-flight (Scrum Master runs before dispatching US-3.3):

- [ ] `git log sprint-3-hybrid-recall ^origin/main --oneline | grep -q "wire hu_hybrid_retrieve\|US-3.1"` exits 0.
- [ ] `grep -n "hu_embedder_t.*embedder\|hu_vector_store_t.*vector_store" /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-1-hybrid-recall/include/human/memory.h` returns the new signature with nullable params.
- [ ] `grep -c "error:" /tmp/us31-build.log` (or equivalent Wave 1 build log) is 0.
- [ ] Evidence directory exists: `sprints/sprint-3/evidence/3.3/`.

---

## Definition of Done enforcement

### Per-story gate (ALL must hold before story is closed)

**Gate 1 — Commit exists on sprint branch (non-negotiable):**
After implementer reports DONE, Scrum Master IMMEDIATELY runs:
```bash
git log sprint-3-hybrid-recall ^origin/main --oneline | grep -q "<expected-pattern>"
```
Expected patterns:
- US-3.1: `"wire hu_hybrid_retrieve"` or `"US-3.1"`
- US-3.2: `"sprint3 hybrid recall harness"` or `"US-3.2"`
- US-3.3: `"flag-gate hybrid recall"` or `"US-3.3"`
- US-3.4: `"embed-existing-memories"` or `"US-3.4"`

If the grep returns non-zero: **the DONE report is REJECTED**. The story re-opens.
Re-dispatch the implementer with explicit instruction to commit before re-reporting.
Working-tree-only DONE has zero tolerance — see Sprint 1 audit for consequences.

**Gate 2 — GREP-VERIFY all claimed fixes:**
For any story where the implementer claims "I fixed X at file:line", the verifier MUST:
- `grep -n "<new_code_snippet>" <file>` — must return ≥ 1 match
- `grep -n "<old_code_snippet>" <file>` — must return 0 matches

This is mandatory per the Sprint 1 audit failure mode: "tr -d claimed but unchanged"
(stash-only work that never landed in any commit).

**Gate 3 — Verifier runs and returns PASS:**
- `/verify` spawns verifier agent
- Verifier runs `cmake --build --preset dev`, `./build/human_tests`, and story-specific
  grep/command checks
- Evidence captured to `sprints/sprint-3/evidence/<story>/`
- Report must contain `RESULT_verifier=PASS`
- FAIL or INCONCLUSIVE: story stays open, implementer fixes before re-verify

**Gate 4 — Per-story critic runs IMMEDIATELY after Gate 3 (NOT batched at sprint end):**
- Critic reviews the specific diff of the implementer's commit
- HIGH or CRITICAL findings: story re-opens; finding converted to follow-up story in
  `sprints/sprint-3/followups.md`; re-dispatch implementer
- LOW or INFO: noted, story may proceed
- CLEAN: proceed to Gate 5

Rationale: Sprint 1 shipped a BSD-grep regex bug into the publish path because critic
ran at sprint close, after the next implementer had already built on broken code.
Per-story critic is the process that prevents this.

**Gate 5 — Aspect-Panel returns PASS or CLEAN:**
- `/aspect-panel` runs 5 specialized verifiers: correctness, edge-case, security,
  regression, style
- 40-60% pass share triggers escalation to Tech Lead
- ESCALATE: halt story closure, surface to Tech Lead for ruling
- PASS or CLEAN: story is done

**Gate 6 — Ordering enforced:**
Verifier (Gate 3) → Critic (Gate 4) → Aspect-Panel (Gate 5). Never out of order.
Critic HIGH+ finding blocks advancement to Panel. This is not negotiable.

**Gate 7 — Tests added or updated:**
Every story's ACs must have test coverage. No AC without a test that would catch a
regression in that AC. The critic's checklist includes: "Did the implementer add a
test for every new behavior?" If no: finding is HIGH.

**Gate 8 — Compile flags clean:**
`cmake --build --preset dev 2>&1 | grep -c "error:"` must print `0`.
`-Wall -Wextra -Wpedantic -Werror` — no warnings, no errors. Zero exceptions.

**Gate 9 — ASan clean:**
`./build/human_tests 2>&1 | grep -i "asan\|leak\|ERROR: Address"` must return empty.
Any ASan finding blocks story closure.

### Per-sprint gate (after all four stories close)

- [ ] `tests/integration/test_sprint3_hybrid_recall.c` exists and AC-3.2.5 passes green
      (US-3.1 must be merged for this to flip).
- [ ] `./build/human_tests --suite=sprint3_hybrid_recall` shows 0 failures, 0 ASan errors.
- [ ] `cmake --build --preset dev --target human` exits 0.
- [ ] `scripts/install-human-daemon.sh` exits 0; `human-daemon doctor` reports green.
- [ ] `sprints/sprint-3/verify.sh` runs without error (semantic probe, SQLite count,
      latency p50 < 50ms).
- [ ] Full test suite: 10000+ tests pass, 0 failures, 0 ASan errors.
- [ ] Sprint Auditor returns `RESULT_sprint-auditor=PASS` or `PASS_WITH_NOTES`.
- [ ] PR opened against `main`, Code-Reviewer approved, squash-merged.

---

## Known risks requiring active monitoring

### R-HIGH: hybrid.c key-loss (US-3.1)

`search_results_to_entries` at `hybrid.c:60-66` copies `results[i].content` into
BOTH `entries[i].content` AND `entries[i].key`. This means the original storage key
(e.g., `"contact:+18018285260:hike_memory"`) is silently discarded and replaced with
the raw text content. The contact-prefix filter in `hu_memory_recall_for_contact`
matches on `entry.key`. Without the fix in US-3.1's implementation step 3, every
semantic-only hit fails the prefix filter and is dropped — the semantic recall path
is completely broken even though the code compiles and BM25 tests pass.

**Verifier enforcement:** The verifier for US-3.1 MUST run:
```bash
./build/human_tests --filter=hybrid_round_trip_preserves_original_key
```
and confirm PASS before clearing Gate 3. This test validates the hybrid.c fix in
isolation. If this test is absent from the commit, Gate 3 is FAIL.

### R-HIGH: Skill routing coupling (US-3.3)

`bootstrap.c:1014` (`hu_agent_set_skill_route_embedder`) uses the same embedder
that US-3.3 flag-gates. Naive flag-gating the embedder also silently disables skill
routing for all users who have `hybrid_recall=false` (the default). The implementer
MUST gate line 1014 jointly with lines 871-885, document the coupling in the commit
message, and file `sprints/sprint-3/followups.md` entry
`"FU-3.3-A: decouple skill routing from hybrid_recall flag"` if this is unacceptable
long-term.

**Verifier enforcement:** `grep -n "hu_agent_set_skill_route_embedder" src/bootstrap.c`
must show the call is inside the `if (bi->cfg.memory.hybrid_recall)` block, not at
the unconditional level. If it is unconditional after the flag-gate change, the
verifier returns FAIL.

### R-MEDIUM: AC-3.2.5 cannot be green until US-3.1 merges

US-3.2 ships with the semantic-recall assertion red. The Scrum Master must NOT block
US-3.2 merge on AC-3.2.5 passing — that would create a circular deadlock
(US-3.2 needs US-3.1 to be green; US-3.1 needs US-3.2's test to prove AC-3.1.3).
Design resolution: US-3.2 merges with AC-3.2.5 marked as expected-red. US-3.1's
implementer flips it green as part of US-3.1's commit.

### R-MEDIUM: AC-3.3.4 currently violated

`src/bootstrap.c:871-880` creates embedder and vector store unconditionally today.
AC-3.3.4 (cold-path safety: disabled when `hybrid_recall=false`) is the PRIMARY
value of US-3.3. The implementer's first behavioral step (step 2 of the design) is
to add the flag-gate. If the full test suite has tests that assert `ctx.embedder != NULL`
without setting `hybrid_recall=true` in their fixture, those tests must be fixed to
either set the flag or tolerate NULL. The implementer halts and reports to the
Scrum Master if more than 5 such tests require changes (signal that the assumption
is more structural than the design audit suggested).

### R-MEDIUM: Longjmp tempdir leak (US-3.2)

The test framework uses longjmp on assertion failure. Inline teardown at the bottom
of test bodies will leak tempdirs on every failed assertion, eventually filling CI
disks. The suite-level wrapper with `g_sprint3_last_tempdir` is the required pattern.
The critic for US-3.2 MUST check: does the suite-level wrapper exist? If inline
teardown is used instead, finding is HIGH and the story re-opens.

### R-MEDIUM: Scope creep into vector store persistence (US-3.4)

The natural temptation: "since we're writing embeddings to SQLite, why not have the
daemon read them?" Any read path from `src/memory/` in the US-3.4 PR is a scope
violation. Critic for US-3.4 MUST flag any change under `src/memory/` as HIGH.

---

## Budget

| Story | Implementer | Verifier | Critic | Panel | Total |
|-------|-------------|----------|--------|-------|-------|
| US-3.1 | ~$2.00 | ~$0.50 | ~$0.25 | ~$0.25 | ~$3.00 |
| US-3.2 | ~$1.50 | ~$0.50 | ~$0.25 | ~$0.25 | ~$2.50 |
| US-3.3 | ~$1.50 | ~$0.50 | ~$0.25 | ~$0.25 | ~$2.50 |
| US-3.4 | ~$1.00 | ~$0.25 | ~$0.25 | ~$0.25 | ~$1.75 |
| **Sprint total** | | | | | **~$9.75** |

Target: $8-15 per sprint. $9.75 is within budget. Wave 2 re-dispatch (if US-3.3
implementer is rejected for missing commit) adds ~$1.50; still within ceiling.

---

## Sprint ceremonies

This is a 4-story sprint. Match ceremony to size.

- **No daily standup** unless a story is blocked or a DONE report is rejected.
- **Per-story standup note** (inline in this document, in the Blockers section below)
  when any story has been in-flight > 3 hours without a DONE report.
- **Sprint Review** opens only after all four stories clear all 9 gates.
- **Retro** runs after Sprint Auditor clears. `/mine-transcripts` over sprint window.
  Any agent with 2+ verifier failures queued for `/tune-agent`.

---

## Blockers (live — updated as sprint runs)

None at plan time.

---

## Scrum Master enforcement commitments

1. I will run the commit-existence grep (Gate 1) within 5 minutes of every DONE
   report. No exceptions.
2. I will dispatch the per-story critic (Gate 4) within 5 minutes of verifier PASS.
   Not batched. Not deferred to sprint close.
3. I will not advance any story past critic (Gate 4) while a HIGH or CRITICAL finding
   is open. The finding becomes a follow-up story or the implementer fixes it before
   re-close.
4. I will not dispatch US-3.3 until the Wave 2 start condition is verified by grep.
5. I will invoke sprint-auditor after all stories close. The audit is not optional.
6. I will run `/mine-transcripts` after the retro and queue any agent with 2+ verifier
   failures for `/tune-agent` before tagging the sprint close.
7. I will tag the sprint close commit with `scripts/tag-sprint-close.sh sprint-3`
   to create an immutable annotated tag `v-sprint-3-close`. This prevents branch-ref
   hijacking (Sprint 2c incident).

---

`RESULT_scrum-master=PLAN_READY`
