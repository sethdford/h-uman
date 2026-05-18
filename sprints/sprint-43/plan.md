# Sprint 43 Plan — Distribution MVP

## Header

| Field | Value |
|---|---|
| Sprint | 43 |
| Branch | `sprint-43-distribution-mvp` |
| Working directory | `/Users/sethford/Documents/h-uman/.worktrees/sprint-43-distribution` |
| Base SHA | `5b57ff2b` |
| Estimate | 1L + 1M + 1L + 1M + 1S + 1XS ≈ 4 M-equivalents |
| Stories | US-43.1, US-43.2, US-43.3, US-43.4, US-43.5, US-43.6 |

---

## §1 Sequencing

### Wave 0 — Parallel (no inter-story dependencies)

| Story | Title | Size | Deps |
|---|---|---|---|
| US-43.1 | Homebrew tap + formula | L | none |
| US-43.2 | Onboard nextstep nudge | M | none |
| US-43.3 | iMessage courtesy reply | L | none |
| US-43.4 | `doctor --install` subcommand | M | none |
| US-43.5 | `chat.db` diagnostics | S | none |

All five run concurrently. No shared files between them at edit time.
US-43.3 touches `src/channels/imessage.c` (4444 lines); no other story
touches that file. US-43.1 creates `Formula/human.rb` (new file).

### Wave 1 — Depends on Wave 0 completion of US-43.1

| Story | Title | Size | Deps |
|---|---|---|---|
| US-43.6 | Website install drift | XS | US-43.1 (needs `Formula/human.rb` final URL + SHA) |

US-43.6 must not start until US-43.1 is committed and the formula's
`url` and `sha256` fields are stable. The website snippet is derived
directly from the formula's canonical install command.

---

## §2 Wave Assignments

### US-43.1 — Homebrew tap + formula (L, MEDIUM risk)

- Implementer: `general-purpose` with `isolation: worktree`, impl branch `impl/US-43.1`
- Quality gates: verifier PASS + critic CLEAN
- Evidence required: `brew install --verbose humanlabs/human/human` dry-run output
  OR formula lint (`brew audit --strict Formula/human.rb`) passing

### US-43.2 — Onboard nextstep nudge (M, LOW risk)

- Implementer: `general-purpose` with `isolation: worktree`, impl branch `impl/US-43.2`
- Quality gates: verifier PASS + critic CLEAN
- Evidence required: unit test output showing nextstep surface fired on first-run path

### US-43.3 — iMessage courtesy reply (L, HIGH risk)

- Implementer: `general-purpose` with `isolation: worktree`, impl branch `impl/US-43.3`
- Quality gates: verifier PASS + critic CLEAN + **aspect-panel MANDATORY** (PASS or CLEAN required — ESCALATE blocks story closure)
- Rationale: spoofable outbound iMessage surface. Any regression here risks sending
  unsolicited messages from the user's phone number to third parties.
- Evidence required: test coverage for midnight-crossover spoof guard, rate-limiter
  assertions, and the opt-in gate. Full suite must pass.

### US-43.4 — `doctor --install` subcommand (M, LOW risk)

- Implementer: `general-purpose` with `isolation: worktree`, impl branch `impl/US-43.4`
- Quality gates: verifier PASS + critic CLEAN
- Cross-sprint note: sprint-42 owns `--privacy` doctor slot. US-43.4 must not
  collide with that slot. If sprint-42 has already landed its `--privacy`
  implementation, read its slot registration before writing `--install`.
- Evidence required: `human doctor --install` help text visible + at least one
  fix path exercised in tests

### US-43.5 — `chat.db` diagnostics (S, MEDIUM risk)

- Implementer: `general-purpose` with `isolation: worktree`, impl branch `impl/US-43.5`
- Quality gates: verifier PASS + critic CLEAN
- Evidence required: diagnostic output for a known-corrupt fixture returns a
  structured, actionable error (not a raw sqlite3 message)

### US-43.6 — Website install drift (XS, LOW risk)

- Implementer: `general-purpose` with `isolation: worktree`, impl branch `impl/US-43.6`
- Gate dependency: BLOCKED until US-43.1 commit SHA is available
- Quality gates: verifier PASS + critic CLEAN
- Evidence required: website snippet matches `brew install humanlabs/human/human`
  derived from the merged formula. Link-check passes on the docs page.

---

## §3 Implementer Commit Discipline

Every implementer agent MUST commit work to its `impl/US-43.N` branch
before reporting DONE. Working-tree-only DONE reports will be rejected
and the story re-opened.

Commit command pattern (each agent uses its own worktree path):

```bash
git -C <impl-worktree> add <paths>
git -C <impl-worktree> commit -m "feat(US-43.N): <description>"
```

The Scrum Master will verify each DONE report against:

```bash
git log sprint-43-distribution-mvp ^5b57ff2b --oneline | grep -q "US-43.N"
```

or against the impl branch tip if the cherry-pick / merge has not yet
landed on the sprint branch.

Implementers MUST NOT:
- Switch branches mid-story without Scrum Master sign-off
- Work outside their assigned worktree
- Run `git reset --hard` or any destructive operation on the sprint branch

---

## §4 Quality Gates

### Per-story minimum (all stories)

- [ ] Implementer commit exists on `impl/US-43.N` branch
- [ ] `/verify` ran and returned `RESULT_verifier=PASS`
- [ ] Critic ran immediately after DONE report (not batched) and returned CLEAN or LOW/INFO only
- [ ] Tests added or updated — no AC without coverage
- [ ] Full suite passes (`./build/human_tests` — 0 failures, 0 ASan errors)
- [ ] Commit message explains WHY, not WHAT

### US-43.3 additional gates (HIGH risk)

- [ ] `/aspect-panel` ran and returned PASS or CLEAN
- [ ] Panel finding of ESCALATE blocks story closure; escalate to user immediately
- [ ] Spoof guard at midnight crossover covered by at least one test
- [ ] Rate-limiter assertions present
- [ ] Opt-in gate tested (off-by-default, must be explicitly enabled)

### Wave 1 gate (US-43.6 specific)

- [ ] US-43.1 commit is merged or cherry-picked to sprint branch BEFORE US-43.6 starts
- [ ] Formula URL and SHA256 fields are final in `Formula/human.rb`

---

## §5 Cross-Sprint Coordination

### Sprint-42 / Sprint-43 doctor slot boundary

- Sprint-42 owns: `--privacy` doctor slot
- Sprint-43 owns: `--install` doctor slot
- Before US-43.4 implementer touches doctor slot registration, check:
  1. `git log main..sprint-42-* --oneline -- src/doctor*` for sprint-42 landing status
  2. If landed: read the slot map and insert `--install` without touching `--privacy`
  3. If not landed: implement `--install` with a comment noting the reserved `--privacy` slot

### Sprint-42 persona-encryption status (used by US-43.3 opt-in check)

The courtesy reply opt-in gate may need to check whether persona encryption
is active (sprint-42 deliverable). Strategy:

1. Prefer: call `hu_persona_encryption_status()` if the symbol resolves
2. Fallback: file-existence check (`~/.human/persona.enc`) + JSON-parse of
   persona config for `encryption_enabled` field
3. Never block US-43.3 implementation on sprint-42 delivery — the fallback
   must compile and pass tests independently

---

## §6 Top 3 Risks

### Risk A — US-43.3 spoof-spam at midnight crossover (ACCEPTED in design)

iMessage courtesy replies sent near midnight could be attributed to the
wrong calendar day by the recipient's device, making them appear as
unsolicited contact. The tech-lead design accepted this with a mitigation:
the rate-limiter window straddles midnight (22:00–02:00 treated as a single
suppression window). Implementer must match this window exactly.

**Mitigation already in design.** Verify via test.

### Risk B — Homebrew tap repo authority (OPERATOR ACTION needed before release CI)

`Formula/human.rb` can be authored in-sprint, but the tap
(`humanlabs/homebrew-human` or in-tree `Formula/`) must exist and have
the correct GitHub permissions before release CI can push a bottle.

**Blocker for release CI, not for sprint delivery.** Sprint delivers the
formula file and the tap setup instructions. Operator must create the tap
repo. Flag to user in open questions.

### Risk C — `src/channels/imessage.c` is 4444 lines (large implementer surface)

US-43.3 touches one of the largest source files in the repo. The
implementer agent faces high context pressure. Mitigate by:

- Scoping the agent prompt to specific function insertion points (named
  in the US-43.3 design doc)
- Splitting "add the guard predicate" from "wire it into the send path"
  if the agent reports context pressure at the midpoint
- N ≤ 8 mechanical sites rule applies: if the wiring requires > 8
  call-site edits, split into two agent dispatches

---

## §7 Open Questions (requires PO / operator input before Wave 0 dispatch)

### OQ-1 — Courtesy reply brand template text (US-43.3)

The PO flagged that the default reply body copy has not been finalized.
The tech-lead design uses a placeholder: `"[human] I saw your message and
will reply when I'm available."` This text will be user-configurable, but
the default must be approved by the PO before the story ships.

**Blocking US-43.3 if not resolved.** Implementer can build the
infrastructure against the placeholder; the copy must be confirmed before
the story closes.

### OQ-2 — Tap repo target (US-43.1)

Two options remain open from the tech-lead design:

| Option | Path | Trade-off |
|---|---|---|
| A | `humanlabs/homebrew-human` (separate tap repo) | Standard Homebrew practice; requires GitHub repo creation |
| B | In-tree `Formula/human.rb` (no separate tap) | Simpler; but violates Homebrew tap conventions for third-party taps |

**Recommendation:** Option A. Operator must create `humanlabs/homebrew-human`
on GitHub before release CI can bottle-build. Sprint delivers the formula
file regardless; the tap target is a config string in the formula URL.

**Blocking tap publication, not sprint delivery.**

---

## Wave Dispatch Checklist (Scrum Master use)

Before dispatching Wave 0:
- [ ] OQ-1 courtesy reply copy resolved or placeholder explicitly accepted by PO
- [ ] OQ-2 tap repo target decided (sprint can proceed with placeholder URL)
- [ ] Each agent prompt includes this plan's §3 commit discipline verbatim
- [ ] Each agent prompt includes the sprint branch name and base SHA
- [ ] US-43.3 agent prompt includes the aspect-panel requirement and HIGH-risk context

Before dispatching Wave 1 (US-43.6):
- [ ] US-43.1 DONE confirmed (commit on sprint branch or impl/US-43.1)
- [ ] `Formula/human.rb` `url` and `sha256` fields are final
